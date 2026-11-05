// GAGE-MP in-game overlay: the real UI (loading state, corner mark, chat, quick actions,
// vitals, speedometer). Contract: overlay.h. Seam to the D3D11 plumbing: overlay_internal.h.
//
// Screen layout (one 28 px gutter at 1080p on every edge, bar the docked vitals strip below): the mark top-right, the chat top-left
// (its plate starts 100 px from the top), the vitals plate and the speedometer stacked on the
// bottom-right edge, sharing a right edge and a width. The bottom-left corner is GTA's radar
// (0.4.22 minimap): nothing persistent of ours is drawn there except the vitals, which dock as a slim
// strip directly under the radar while it is on (ClusterLayout / DrawVitalsStrip; the speedometer then
// rests alone on the gutter).
//
// Everything that draws runs between ImGui::NewFrame and ImGui::Render on the render
// thread and never calls a game native. The push/pull functions at the bottom of the
// file are the only thread-safe entry points; they touch a small mutex-guarded block of
// data (chat history, outgoing queue, loader status) and a few atomics.
//
// Name tags (W10): above the remote players' heads, in the world layer under every plate. Plain outlined type, no panel:
// the name in Segoe UI Semibold (kInk2, the role of other players' names), its "[id]" in the small face (kInk2 at 0.62),
// both with a ground-coloured outline and a soft drop so they read on a bright sky and on a dark alley. The engine
// decides which tags exist and where (libs/sync/nametag.h); this file only draws the list it was handed.
//
// Art direction: Solar RP, NOON palette (brand/solar/solar-rp-noon/README.md). Ground #0A0907, panels
// #100E0B / #17140F, warm ink #F7F2E8 (#D9D1C3 for names and captions), one accent, Solar gold #F2B134,
// rationed to rails, lit bars and the active row; a warning is ember #FF8A3D, never gold, so it cannot
// read as an action. Segoe UI Semibold with a bold italic for numerals.
// The plate geometry predates the Solar mark and is unchanged: panels are chamfered at the top-left and
// bottom-right corners and small controls (quick-action rows, the input line, speed and progress segments)
// are parallelograms leaned by kSkew, the cut and lean of the retired italic G. The Solar S is upright with
// its cuts at the top-right and bottom-left.
#include "overlay.h"
#include "overlay_internal.h"
#include "pack_meta.h"
#include "common/logger.h"
#include "protocol/protocol.h"
#include "sync/minimap.h"     // RadarKeepOut / DockVitals: where GTA's radar is, and where the vitals plate goes under it
#include "sync/vehicle_state_row.h"   // 0.4.27 the vehicle state row: where it goes, its cells, the change pulse

#include "imgui.h"
#include "imgui_internal.h"   // ImGuiContext::ConfigNavWindowingKey* (UiInit)

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace gage::game::Overlay {

namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
constexpr float kSkew = 0.14f;   // the HUD's lean (top edge shifted right by height * kSkew)

// The Solar S (brand/solar viewBox 50 x 100): width / height of the mark textures and of the mark as drawn.
constexpr float kMarkAspect = 0.5f;
// The mark textures are exactly 1:2 and the S touches their edges, while the ImGui backends sample with WRAP: a
// bilinear sample on a border row or column would blend in the opposite edge (a faint dark rim when the mark is
// drawn larger than its texture, e.g. 4K). Inset the UVs by half a texel so the filter never leaves the image.
// texH is the texture's height in texels (64, 256 or 1024); its width is texH * kMarkAspect.
constexpr ImVec2 MarkUv0(float texH) { return ImVec2(0.5f / (texH * kMarkAspect), 0.5f / texH); }
constexpr ImVec2 MarkUv1(float texH) { return ImVec2(1.0f - 0.5f / (texH * kMarkAspect), 1.0f - 0.5f / texH); }

struct Rgb { float r, g, b; };
constexpr Rgb Hex(unsigned v)
{
    return Rgb{(float)((v >> 16) & 255u) / 255.0f, (float)((v >> 8) & 255u) / 255.0f, (float)(v & 255u) / 255.0f};
}
// NOON, role by role (brand/solar/solar-rp-noon/README.md).
constexpr Rgb kGround   = Hex(0x0A0907);   // scrims, the loader's ground, the input well, cursor outline, text shadow
constexpr Rgb kPanel    = Hex(0x100E0B);   // the plate fill: chat, vitals, speedometer, quick actions, corner chip
constexpr Rgb kPanelHi  = Hex(0x17140F);   // a raised ground: the top of the loader's ground gradient
constexpr Rgb kInk      = Hex(0xF7F2E8);   // text, digits, lit tips, the cursor, neutral fills (at alpha)
constexpr Rgb kInk2     = Hex(0xD9D1C3);   // other players' names, captions, column heads, glyphs
constexpr Rgb kHair     = Hex(0xFFF4E0);   // hairlines (plate edges, separators), used at 0.10 - 0.16 alpha
constexpr Rgb kGold     = Hex(0xF2B134);   // Solar gold: the one accent (rails, lit bars, selection, active row)
constexpr Rgb kGoldHot  = Hex(0xFFC75A);   // gold, lifted: the pressed row's edge
constexpr Rgb kGoldDown = Hex(0xC98D16);   // gold, pressed: the pressed row's fill
constexpr Rgb kEmber    = Hex(0xFF8A3D);   // warning: low vitals, the last speed bars, a failed connection
constexpr Rgb kOnGold   = Hex(0x1A1206);   // text on gold
constexpr Rgb kNoTint{1.0f, 1.0f, 1.0f};   // texture modulate that changes nothing: the brand PNGs carry their own colour

constexpr std::size_t kChatCap = 100;      // history kept
constexpr std::size_t kOutCap = 16;        // outgoing lines held for the engine
constexpr std::size_t kViewMax = kChatCap; // lines the renderer copies per refresh: the whole history
constexpr std::size_t kBufLen = gage::proto::kMaxChat + 1;   // the server refuses longer lines
constexpr std::size_t kNameMax = 32, kTextMax = 256, kStatusMax = 150;
constexpr float kHoldS = 11.0f;            // a chat line stays this long while the chat is closed...
constexpr float kFadeS = 1.5f;             // ...then fades out over this long
constexpr float kFullSpeedKmh = 240.0f;    // the speed bar is full here
constexpr ULONGLONG kTagStaleMs = 400;     // a name tag list not refreshed for this long is not drawn

// ---------------------------------------------------------------------------
// Pushed data (any thread)
// ---------------------------------------------------------------------------
struct DataCs {
    CRITICAL_SECTION cs;
    DataCs() { InitializeCriticalSection(&cs); }   // never deleted: lives as long as the DLL
};
DataCs g_dcs;
struct DLock {
    DLock() { EnterCriticalSection(&g_dcs.cs); }
    ~DLock() { LeaveCriticalSection(&g_dcs.cs); }
    DLock(const DLock&) = delete;
    DLock& operator=(const DLock&) = delete;
};

struct Line {
    std::string name, text;
    ULONGLONG at = 0;
    bool system = false, self = false;
};
struct Data {
    std::deque<Line> chat;
    std::deque<std::string> out;
    std::string status;
    bool statusFailed = false;      // the loader's status is a dead end, not progress
    uint32_t chatSeq = 0;
    std::vector<NameTag> tags;      // W10: the newest list SetNameTags was given
    ULONGLONG tagsAt = 0;           // ... and when (GetTickCount64)
    std::vector<CarEntry> cars;     // the Tab menu's car browser: the newest list SetCars was given
    bool carsLoading = false;
    uint32_t carsSeq = 0;
};
// Heap-allocated and never freed: a join-worker thread may push a chat line while the DLL's
// statics are being destroyed.
Data& D() { static Data* d = new Data; return *d; }

std::atomic<int>      g_phase{(int)Phase::Loading};   // loading until the engine says otherwise
std::atomic<bool>     g_inVehicle{false};
std::atomic<float>    g_speedKmh{0.0f};
std::atomic<bool>     g_interactive{false};
// The Alt+F4 confirm (OpenQuitPrompt): GetTickCount64 when it opened, 0 = closed. Written by the window
// thread (WndProc) and by the render thread (the timeout), read by both and by the engine's script tick.
std::atomic<ULONGLONG> g_quitPromptAt{0};
constexpr ULONGLONG    kQuitPromptMs = 10000;   // unanswered, the card goes away by itself after this
// Vitals, 0..1. Until the engine pushes, health and satiety read full and the cluster is shown.
std::atomic<float>    g_vitHealth{1.0f};
std::atomic<float>    g_vitSatiety{1.0f};
std::atomic<float>    g_vitFuel{1.0f};
std::atomic<bool>     g_vitShow{true};
std::atomic<bool>     g_vitShowFuel{false};
// 0.4.22 minimap (SetRadarDock): dock the vitals plate under GTA's radar. Off until the engine says so (the pre-0.4.22 layout).
std::atomic<bool>     g_radarDock{false};
std::atomic<float>    g_radarSafeZone{1.0f};
std::atomic<bool>     g_radarSafeZoneOk{false};
std::atomic<float>    g_radarScale{1.0f};   // 0.4.25 minimap_scale: the factor the game's radar was shrunk by (1 = the game's own size)
// 0.4.22 vehicle controls: the key bits pressed since the engine last took them (and when the newest came), and the strip's state packed in
// one word so a frame never draws half of one push: bit 0 left on, 1 right on, 2-3 lights (0..2), 4 belt buckled, 5 belt warning, 6 engine off, 7 shown;
// 0.4.27 state row: 8 lock known, 9 locked, 10 the on-foot lock flash.
std::atomic<unsigned>  g_vehKeys{0};
std::atomic<ULONGLONG> g_vehKeysAt{0};
std::atomic<unsigned>  g_vehHud{0};
constexpr ULONGLONG    kVehKeyStaleMs = 400;
// The HUD clock's capture hook (Overlay::SetHudPhaseForTest): < 0 = off, the clock runs from ImGui's time; 0..1 = every blink and pulse is held at that phase.
std::atomic<float>     g_hudPhaseForce{-1.0f};

// Keep printable text: control characters become spaces, and a cut never leaves half a UTF-8 sequence.
std::string Clean(const char* s, std::size_t maxBytes)
{
    std::string o;
    if (!s)
        return o;
    const char* p = s;
    for (; *p && o.size() < maxBytes; ++p) {
        const unsigned char c = (unsigned char)*p;
        o.push_back((c < 0x20 || c == 0x7f) ? ' ' : (char)c);
    }
    if (*p) {
        while (!o.empty() && ((unsigned char)o.back() & 0xC0) == 0x80)
            o.pop_back();
        if (!o.empty() && ((unsigned char)o.back() & 0x80))
            o.pop_back();
    }
    return o;
}

void PushLine(Line&& l)
{
    DLock lk;
    Data& d = D();
    d.chat.push_back(std::move(l));
    while (d.chat.size() > kChatCap)
        d.chat.pop_front();
    ++d.chatSeq;
}

void PushSystem(const std::string& text)
{
    if (text.empty())
        return;
    Line l;
    l.text = text;
    l.system = true;
    l.at = GetTickCount64();
    PushLine(std::move(l));
}

bool Enqueue(const std::string& s)
{
    DLock lk;
    if (D().out.size() >= kOutCap)
        return false;
    D().out.push_back(s);
    return true;
}

// ---------------------------------------------------------------------------
// Render-thread state
// ---------------------------------------------------------------------------
struct Fnt {
    ImFont* font = nullptr;
    float   px = 0.0f;        // size drawn at this frame (follows the display size)
    float   base = 0.0f;      // size the glyph atlas was baked at
    float   design = 0.0f;    // size at 1080p
    float   minPx = 0.0f;     // never drawn smaller than this
};

struct ViewLine {
    std::string name, text;
    ULONGLONG at = 0;
    bool system = false, self = false;
};

struct Ui {
    float scale = 1.0f;                     // layout scale, H / 1080, follows the display every frame
    float bakeScale = 1.0f;                 // the scale the fonts were baked at (UiInit)
    float px1 = 1.0f;                       // one hairline, in whole pixels
    float chatPlateH = 0.0f;                // the open chat plate's current height (animated)
    Fnt body, semi, small, title, mid, num, numMid;
    detail::UiTextures tex;

    bool     first = true;
    uint32_t frame = 0;
    float    loaderA = 1.0f, hudA = 0.0f, speedA = 0.0f, chatA = 0.0f, invA = 0.0f;
    float    quitA = 0.0f;                  // the Alt+F4 confirm card's visibility
    float    quitLeft = 1.0f;               // its countdown rail as last drawn (held while it fades out)
    ULONGLONG loaderT0 = 0;                 // when the loader first drew: the stills' crossfade cycle starts here

    // The bottom-right cluster's eased values (TickVitals, UiFrame): what is drawn trails what was pushed.
    float    speedSm = 0.0f;                // displayed speed, km/h
    float    vitalsA = 0.0f, fuelA = 0.0f;  // cluster / fuel row visibility
    float    stripA = 0.0f;                 // the speedometer plate's status strip (0.4.22): eased so the plate never jumps in height
    float    rowA = 0.0f;                   // 0.4.27 the vehicle state row's visibility
    float    rowOn[3] = {};                 // its toggles' knobs, eased 0 (off, left) .. 1 (on, right)
    gage::sync::RowTracker rowTrack;        // when each cell last flipped (the change pulse)
    struct Vital {
        float fill = 1.0f;                  // the bar's level as drawn
        float trail = 1.0f;                 // lags above fill after a loss, so damage reads
        float warn = 0.0f;                  // 0..1 mix toward the warning colour
        bool  low = false;                  // below its threshold (with hysteresis)
    };
    Vital vit[3];                           // health, satiety, fuel

    bool      chatOpen = false, invOpen = false;
    uint32_t  chatOpenFrame = 0;
    ULONGLONG chatOpenedAt = 0;
    int       focusFrames = 0;
    bool      inputActive = false;          // the chat text box held keyboard focus last frame
    std::vector<ImWchar> held;              // characters typed before the box took focus
    char      buf[kBufLen] = {};
    int       scroll = 0;
    bool      invJustOpened = false;
    int       invTab = 0;                   // the Tab menu's page: 0 quick actions, 1 cars (kept between openings)

    // The car browser (the Tab menu's "Cars" page): the render thread's copy of SetCars' list, the search and the page.
    struct CarRow {
        std::string model, name, vclass, pack;
        std::string cmd;                    // "/car <model>"
        std::string hay;                    // name, model, class and pack in lower case: what the search looks in
        bool ready = false;
    };
    std::vector<CarRow> cars;
    uint32_t  carsSeq = ~0u;
    bool      carsLoading = false;
    std::vector<int> carHits;               // indexes into cars that match the search, in list order
    std::string carHitsQuery;               // the query carHits was built for
    bool      carHitsStale = true;
    char      carQuery[64] = {};
    int       carPage = 0;
    int       searchFocusFrames = 0;        // focus the search box this many more frames (it takes a frame or two)
    bool      searchActive = false;         // the search box held the keyboard last frame
    bool      searchSelectAll = false;      // the next activation selects the query (opening / switching to the page)
    float     wheelAcc = 0.0f;              // mouse wheel over the list, one page per whole notch
    float     railX = 0.0f, railW = 0.0f;   // the Tab menu's gold rail, eased over the open tab (from the panel's left)

    std::vector<ViewLine> view;
    uint32_t              viewSeq = ~0u;

    UiState last;
};
Ui& u = *new Ui;     // never freed, see D()

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
float S(float v) { return std::floor(v * u.scale + 0.5f); }
float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
float Approach(float v, float target, float step)
{
    return v < target ? std::fmin(target, v + step) : std::fmax(target, v - step);
}
// Frame-rate independent ease: closes 1 - e^(-k dt) of the gap each frame (k = 9 is ~95 % in a third
// of a second), and lands exactly on the target once within eps, so a value never hovers just short.
float Ease(float v, float target, float k, float dt, float eps)
{
    const float d = target - v;
    if (std::fabs(d) <= eps)
        return target;
    return v + d * (1.0f - std::exp(-k * dt));
}
// A pushed fraction: NaN counts as 0, anything else is clamped to 0..1.
float Frac01(float v) { return v == v ? Clamp01(v) : 0.0f; }
Rgb Mix(Rgb a, Rgb b, float t)
{
    t = Clamp01(t);
    return Rgb{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
ImU32 Col(Rgb c, float a)
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, Clamp01(a)));
}
// The one HUD clock (0.4.22): every blink and pulse of the vitals strip, the speedometer's status strip and the plates' rail comes from this phase,
// phi = fract(t * 1.5), so they all beat together. The turn indicators blink on phi < 0.5 (1.5 Hz, on for the first half); every alarm (a low vital,
// the unbuckled belt, the rail) pulses on the 2nd harmonic of the same phase, 3 Hz, brightest exactly when an indicator comes on and when it goes off.
float HudPhase()
{
    const float f = g_hudPhaseForce.load();
    if (f >= 0.0f)
        return f - std::floor(f);
    return (float)std::fmod(ImGui::GetTime() * 1.5, 1.0);
}
float AlarmPulse(float phi) { return 0.5f + 0.5f * std::cos(phi * 12.5663706f); }   // 4 pi: two beats per phase, 1 at phi = 0 and 0.5, 0 at 0.25 and 0.75
// Distance from a text box's top to its baseline at the size drawn (lets two faces share a baseline).
float AscentPx(const Fnt& f) { return f.font->Ascent * (f.px / f.base); }
// The ink box of a numeral at the size drawn, as offsets from the text box's top. '1' is flat on
// both ends (a '0' overshoots), so these are the cap line and the baseline.
void DigitBox(const Fnt& f, float* top, float* bottom)
{
    const ImFontGlyph* g = f.font->FindGlyph((ImWchar)'1');
    const float k = f.base > 0.0f ? f.px / f.base : 1.0f;
    if (g) {
        *top = g->Y0 * k;
        *bottom = g->Y1 * k;
    } else {
        *top = 0.285f * f.px;
        *bottom = 0.81f * f.px;
    }
}
ImVec2 Measure(const Fnt& f, const char* t, float wrap = 0.0f)
{
    return f.font->CalcTextSizeA(f.px, FLT_MAX, wrap, t);
}
// Text with an optional drop shadow (one hairline down-right) so it reads on any scene.
void Text(ImDrawList* dl, const Fnt& f, ImVec2 p, Rgb c, float a, const char* t, float wrap = 0.0f,
          float shadow = 0.0f)
{
    if (a <= 0.003f)
        return;
    if (shadow > 0.0f)
        dl->AddText(f.font, f.px, ImVec2(p.x + u.px1, p.y + u.px1), Col(kGround, a * shadow), t, nullptr, wrap);
    dl->AddText(f.font, f.px, p, Col(c, a), t, nullptr, wrap);
}

// The brand's cut: top-left and bottom-right corners chamfered.
void PlateChamfer(ImDrawList* dl, float x0, float y0, float x1, float y1, float ch, ImU32 fill, ImU32 border)
{
    const ImVec2 p[6] = {{x0 + ch, y0}, {x1, y0}, {x1, y1 - ch}, {x1 - ch, y1}, {x0, y1}, {x0, y0 + ch}};
    dl->AddConvexPolyFilled(p, 6, fill);
    if (border)
        dl->AddPolyline(p, 6, border, ImDrawFlags_Closed, u.px1);
}
// The brand's lean: the top edge sits (y1 - y0) * kSkew to the right of the bottom edge,
// so the plate's bounding box is [x0, x1 + lean]. Returns the lean.
float PlateSlant(ImDrawList* dl, float x0, float y0, float x1, float y1, ImU32 fill, ImU32 border)
{
    const float off = (y1 - y0) * kSkew;
    const ImVec2 p[4] = {{x0 + off, y0}, {x1 + off, y0}, {x1, y1}, {x0, y1}};
    dl->AddConvexPolyFilled(p, 4, fill);
    if (border)
        dl->AddPolyline(p, 4, border, ImDrawFlags_Closed, u.px1);
    return off;
}

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------
// Latin, Latin-1/Extended-A, Cyrillic, Georgian (Mkhedruli + Mtavruli), general punctuation, euro.
const ImWchar kFullRanges[] = {
    0x0020, 0x017F,
    0x0400, 0x04FF,
    0x10A0, 0x10FF,
    0x1C90, 0x1CBF,
    0x2010, 0x2027,
    0x20AC, 0x20AC,
    0,
};
const ImWchar kLatinRanges[] = { 0x0020, 0x00FF, 0 };

bool WindowsFontPath(const char* file, char* out, std::size_t cap)
{
    char dir[MAX_PATH] = {};
    const UINT n = GetWindowsDirectoryA(dir, MAX_PATH);
    if (!n || n > MAX_PATH - 40)
        return false;
    std::snprintf(out, cap, "%s\\Fonts\\%s", dir, file);
    return GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES;
}

// Pixel size of a font at a layout scale: the 1080p design size scaled, never below floorPx
// (15 px type is already small; at 0.75 it must not become 11 px).
float FontPx(float design, float floorPx, float scale)
{
    return std::fmax(floorPx, std::floor(design * scale + 0.5f));
}

// The layout scale follows the display every frame (a resolution or window-size change needs no
// restart). The glyph atlas stays as baked; text is drawn at the matching size.
void RefreshScale(float displayH)
{
    u.scale = std::fmin(3.0f, std::fmax(0.75f, displayH / 1080.0f));
    u.px1 = std::fmax(1.0f, std::floor(u.scale + 0.5f));
    Fnt* const fs[] = {&u.body, &u.semi, &u.small, &u.mid, &u.title, &u.num, &u.numMid};
    for (Fnt* f : fs)
        f->px = FontPx(f->design, f->minPx, u.scale);
    // ImGui draws the one text box itself, with the body font: keep its size in step.
    ImGui::GetIO().FontGlobalScale = u.body.base > 0.0f ? u.body.px / u.body.base : 1.0f;
}

// First file of `files` that exists, baked at `px`; the ImGui default font when none does.
Fnt LoadFont(ImGuiIO& io, const char* const* files, int nFiles, float px, const ImWchar* ranges)
{
    Fnt f;
    f.px = px;
    f.base = px;
    for (int i = 0; i < nFiles && !f.font; ++i) {
        char path[MAX_PATH];
        if (WindowsFontPath(files[i], path, sizeof(path)))
            f.font = io.Fonts->AddFontFromFileTTF(path, px, nullptr, ranges);
    }
    if (!f.font) {
        ImFontConfig cfg;
        cfg.SizePixels = px;
        f.font = io.Fonts->AddFontDefault(&cfg);
    }
    return f;
}

void BuildStyle(float scale)
{
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0.0f;
    st.FrameRounding = 0.0f;
    st.PopupRounding = 0.0f;
    st.ScrollbarRounding = 0.0f;
    st.GrabRounding = 0.0f;
    st.WindowBorderSize = 0.0f;
    st.FrameBorderSize = 0.0f;
    st.ScaleAllSizes(scale);
    // ScaleAllSizes floors the software cursor's scale: below 1080p (0.67 -> 0) the pointer vanished.
    // The cursor bitmap is a fixed 12x19 px, so keep it at a whole number, never below 1.
    st.MouseCursorScale = std::fmax(1.0f, std::floor(scale + 0.5f));
    ImVec4* c = st.Colors;
    const ImVec4 accent(kGold.r, kGold.g, kGold.b, 1.0f);
    c[ImGuiCol_Text]            = ImVec4(kInk.r, kInk.g, kInk.b, 1.0f);
    c[ImGuiCol_TextDisabled]    = ImVec4(kInk.r, kInk.g, kInk.b, 0.52f);
    c[ImGuiCol_WindowBg]        = ImVec4(kGround.r, kGround.g, kGround.b, 0.94f);
    c[ImGuiCol_FrameBg]         = ImVec4(0, 0, 0, 0);          // the panels draw their own plates
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TextSelectedBg]  = ImVec4(accent.x, accent.y, accent.z, 0.38f);
    c[ImGuiCol_Border]          = ImVec4(kHair.r, kHair.g, kHair.b, 0.12f);
    c[ImGuiCol_Separator]       = ImVec4(kHair.r, kHair.g, kHair.b, 0.12f);
    c[ImGuiCol_NavHighlight]    = accent;
}

// ---------------------------------------------------------------------------
// Chat input callbacks
// ---------------------------------------------------------------------------
int ChatCharFilter(ImGuiInputTextCallbackData* d)
{
    const ImWchar c = d->EventChar;
    if (c < 0x20 || c == 0x7f)
        return 1;                       // tab and other control characters never enter the line
    // (The character of the T key that opened the box never reaches ImGui: see OverlayWndProc.)
    return 0;
}

// ---------------------------------------------------------------------------
// Quick actions
// ---------------------------------------------------------------------------
struct Action {
    const char* label;
    const char* cmd;      // what is queued for the engine (QueueCommand in engine.cpp)
    int         column;   // 0 player, 1 movement, 2 weapons
    int         kind = 0; // 1: the noclip toggle (shows On / Off, hidden when the engine says noclip=0)
};
// (The cars that used to be the middle column are the Tab menu's Cars page now; 0.4.22 put the noclip toggle there.)
const Action kActions[] = {
    {"Heal",           "/heal",                 0},
    {"Repair vehicle", "/fix",                  0},
    {"Go to spawn",    "/spawn",                0},
    {"Noclip",         "/noclip",               1, 1},
    {"Pistol",         "/gun pistol",           2},
    {"Micro SMG",      "/gun microsmg",         2},
    {"Carbine rifle",  "/gun carbinerifle",     2},
    {"Pump shotgun",   "/gun pumpshotgun",      2},
    {"Sniper rifle",   "/gun sniperrifle",      2},
};
constexpr int kActionCount = (int)(sizeof(kActions) / sizeof(kActions[0]));
static_assert(kActionCount <= UiState::kMaxActions, "UiState::kMaxActions is too small");
const char* const kColumnTitle[3] = {"Player", "Movement", "Weapons"};
constexpr int kActionNoclip = 1;
// Pushed by the engine (SetNoclip): is the toggle offered at all (the noclip registry switch), and is it on. Until the engine
// says otherwise the row is offered and off.
std::atomic<bool> g_noclipAvail{true};
std::atomic<bool> g_noclipOn{false};
bool ActionShown(const Action& a) { return a.kind != kActionNoclip || g_noclipAvail.load(); }

// ---------------------------------------------------------------------------
// Panel open / close
// ---------------------------------------------------------------------------
void OpenChat(ULONGLONG now)
{
    u.chatOpen = true;
    u.chatOpenFrame = u.frame;
    u.chatOpenedAt = now;
    u.focusFrames = 3;
    u.inputActive = false;
    u.held.clear();
    u.buf[0] = 0;
    u.scroll = 0;
    u.chatA = 0.0f;
}
void CloseChat()
{
    u.chatOpen = false;
    u.focusFrames = 0;
    u.inputActive = false;
    u.held.clear();
    u.buf[0] = 0;
    u.scroll = 0;
}
void OpenInventory()
{
    u.invOpen = true;
    u.invJustOpened = true;
    u.invA = 0.0f;
    u.searchFocusFrames = u.invTab == 1 ? 3 : 0;   // the cars page opens ready to type...
    u.searchSelectAll = u.invTab == 1;             // ...over the last query, which the first key replaces
    u.held.clear();
    u.wheelAcc = 0.0f;
}
// The Tab menu's pages (a header tab click, or Ctrl+Tab). The cars page takes the keyboard for its search at once.
void SwitchInvTab(int t)
{
    if (t == u.invTab)
        return;
    u.invTab = t;
    u.searchFocusFrames = t == 1 ? 3 : 0;
    u.searchSelectAll = t == 1;
    u.searchActive = false;
    u.held.clear();
    u.wheelAcc = 0.0f;
}
void CloseInventory()
{
    u.invOpen = false;
    u.searchFocusFrames = 0;
    u.searchActive = false;
    u.searchSelectAll = false;
    u.held.clear();
}

// What the player submitted: trim, queue for the engine, echo commands as a dim line.
void Submit(const char* raw)
{
    std::string s = raw ? raw : "";
    const char* ws = " \t";
    const std::size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos)
        return;
    s = s.substr(b, s.find_last_not_of(ws) - b + 1);
    if (!Enqueue(s)) {
        PushSystem("Too many messages queued, slow down.");
        return;
    }
    if (s[0] == '/')
        PushSystem(s);
}

// ---------------------------------------------------------------------------
// Panels
// ---------------------------------------------------------------------------
// Corner mark (top-right, one gutter in from the top and the right edge): a dark chamfered chip so the
// mark reads over any scene. The Solar S is 1:2, so the textured mark is drawn at that aspect (never
// stretched into a square), centred in the square chip; the typeset fallback keeps the square box.
// ---------------------------------------------------------------------------
// Name tags (W10)
// ---------------------------------------------------------------------------
// Text with a legibility outline: eight ground-coloured copies one hairline around it (a 1 px outline survives 4K and a
// white sky alike), a soft drop two hairlines down, then the face. No panel behind it.
void OutlinedText(ImDrawList* dl, ImFont* font, float size, ImVec2 p, Rgb c, float a, const char* t)
{
    if (a <= 0.003f || !*t)
        return;
    const float o = u.px1;
    const ImU32 edge = Col(kGround, 0.70f * a), drop = Col(kGround, 0.32f * a);
    dl->AddText(font, size, ImVec2(p.x, p.y + 2.0f * o), drop, t);
    static const float k[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    for (const auto& d : k)
        dl->AddText(font, size, ImVec2(p.x + d[0] * o, p.y + d[1] * o), edge, t);
    dl->AddText(font, size, p, Col(c, a), t);
}

void DrawNameTags(ImDrawList* dl, float W, float H, float hudA, ULONGLONG now, UiState& L)
{
    static std::vector<NameTag> tags;      // render thread only; the pushed list is copied out under the lock
    tags.clear();
    {
        DLock lk;
        const Data& d = D();
        if (d.tagsAt && now - d.tagsAt <= kTagStaleMs)
            tags = d.tags;
    }
    L.nameTagsShown = 0;
    // The list is nearest first: draw it backwards so a near tag lies over a far one.
    for (std::size_t i = tags.size(); i-- > 0;) {
        const NameTag& t = tags[i];
        const float a = Clamp01(t.alpha) * hudA;
        if (a <= 0.01f || !(t.x == t.x) || !(t.y == t.y) || !t.name[0])
            continue;
        const float sc = std::fmin(1.2f, std::fmax(0.5f, t.scale == t.scale ? t.scale : 1.0f));
        const float namePx = std::fmax(11.0f, u.semi.px * sc), idPx = std::fmax(10.0f, u.small.px * sc);
        const float nameK = namePx / u.semi.base, idK = idPx / u.small.base;
        const ImVec2 nameSz = u.semi.font->CalcTextSizeA(namePx, FLT_MAX, 0.0f, t.name);
        const ImVec2 idSz = t.idText[0] ? u.small.font->CalcTextSizeA(idPx, FLT_MAX, 0.0f, t.idText) : ImVec2(0, 0);
        const float gap = idSz.x > 0.0f ? S(5.0f) * sc : 0.0f;
        const float totalW = idSz.x + gap + nameSz.x;
        // Centred on the head, kept inside the screen with a gutter; the bottom edge sits a little above the anchor.
        const float gutter = S(8.0f);
        const float x0 = std::fmax(gutter, std::fmin(W - gutter - totalW, t.x * W - totalW * 0.5f));
        const float bottom = t.y * H - S(3.0f) * sc;
        const float top = bottom - nameSz.y;
        // The two faces share a baseline.
        const float nameAsc = u.semi.font->Ascent * nameK, idAsc = u.small.font->Ascent * idK;
        if (t.idText[0])
            OutlinedText(dl, u.small.font, idPx, ImVec2(x0, top + nameAsc - idAsc), kInk2, a * 0.62f, t.idText);
        OutlinedText(dl, u.semi.font, namePx, ImVec2(x0 + idSz.x + gap, top), kInk2, a, t.name);
        if (L.nameTagsShown++ == 0) {
            L.nameTagX = x0; L.nameTagY = top; L.nameTagW = totalW; L.nameTagH = nameSz.y;
        }
    }
}

void DrawLogo(ImDrawList* dl, float W, float a, UiState& L)
{
    const float chip = S(50.0f), ch = S(8.0f);
    const float markH = S(36.0f), markW = std::floor(markH * kMarkAspect + 0.5f);   // the textured S
    const float box = S(34.0f);                                                      // the fallback monogram's box
    const float x0 = std::floor(W - S(28.0f) - chip), y0 = S(28.0f);
    // One plate rule for every surface: solid ground, a hairline and the accent rail (as on the
    // chat and the cluster), so the chip never tints with the scene behind it.
    PlateChamfer(dl, x0, y0, x0 + chip, y0 + chip, ch, Col(kPanel, 0.92f * a), Col(kHair, 0.14f * a));
    dl->AddRectFilled(ImVec2(x0 + ch, y0), ImVec2(x0 + ch + S(22.0f), y0 + std::fmax(2.0f, S(3.0f))), Col(kGold, a));
    // markH is in screen pixels, so compare against the textures' pixel heights (64 / 256 / 1024), not S()-scaled values.
    void* tex = markH <= 72.0f ? u.tex.mark64 : (markH <= 290.0f ? u.tex.mark256 : u.tex.mark1024);
    if (!tex)
        tex = u.tex.mark256 ? u.tex.mark256 : (u.tex.mark64 ? u.tex.mark64 : u.tex.mark1024);
    float mx, my, mw, mh;
    if (tex) {
        mw = markW; mh = markH;
        mx = x0 + std::floor((chip - mw) * 0.5f); my = y0 + std::floor((chip - mh) * 0.5f);
        const float texH = tex == u.tex.mark64 ? 64.0f : (tex == u.tex.mark256 ? 256.0f : 1024.0f);
        dl->AddImage((ImTextureID)tex, ImVec2(mx, my), ImVec2(mx + mw, my + mh), MarkUv0(texH), MarkUv1(texH),
                     Col(kNoTint, a));
    } else {
        // No texture: a gold chip with the typeset "SR" monogram (Solar RP) in dark-on-gold ink, shrunk to
        // sit inside the chip. "SOLAR RP" is the loader's wordmark; it cannot be read at chip size.
        mw = mh = box;
        mx = x0 + std::floor((chip - mw) * 0.5f); my = y0 + std::floor((chip - mh) * 0.5f);
        PlateChamfer(dl, mx, my, mx + mw, my + mh, S(9.0f), Col(kGold, a), 0);
        static const char* const kMonogram = "SR";
        Fnt mono = u.title;
        ImVec2 ts = Measure(mono, kMonogram);
        const float fit = mw * 0.80f;
        if (ts.x > fit) {
            mono.px *= fit / ts.x;
            ts = Measure(mono, kMonogram);
        }
        Text(dl, mono, ImVec2(mx + (mw - ts.x) * 0.5f, my + (mh - ts.y) * 0.5f), kOnGold, a, kMonogram);
    }
    L.logoTextured = tex != nullptr;
    L.logoX = mx; L.logoY = my; L.logoW = mw; L.logoH = mh;
}

// ---- bottom-right cluster: vitals plate with the speedometer stacked on it --------------------
// One right edge and one width for both plates (the mark's gutter from the edge and the bottom), so
// the two read as a single instrument block. The speedometer sits directly above the vitals plate and
// drops into the corner while the vitals are hidden; the vitals plate is a fixed two rows (HP, FOOD) so
// it never shifts, and fuel is a gauge on the speedometer plate (vehicle data, shown only in a vehicle).
//
// 0.4.22 minimap: with GTA's radar on (SetRadarDock) the vitals leave this cluster and dock at the bottom-left as a
// slim strip, the radar's left edge and width (libs/sync/minimap.h DockVitals), health in its left half and food in
// its right half. The speedometer then rests on the bottom gutter alone.
//
// Placement, one of the two options (2026-11-05): DIRECTLY UNDER the radar's bottom edge, inside the safe-zone margin,
// FLUSH with it (gap 0, as GTA's own bars were, a hairline gap read as a seam), and not over the map where GTA's own
// bars were. At the default safe zone that covers no map pixel at all, it does not depend on the (live-unverified)
// hiding of GTA's bars (minimap_bars: they sit inside the radar, so at worst they show once more above our strip), and
// an error in the radar rectangle (a community formula) only changes the gap. Whether GTA's own radar anchor includes
// a ~18 px band for those bars (radar rect bottom above the drawn map's) is a live question, not decided here: the
// log line names the strip's top y and the radar rect's bottom y, so one live screenshot settles it.
// Where the margin under the radar is too small for the strip (a safe zone past about
// 0.95 at 1080p) it tucks up into the radar's bottom edge, at most kStripMaxCover of the radar's height, which is where
// GTA's bars sit; beyond that the plate returns to the bottom-right, as with the radar off.
constexpr float kElemBandFrac = 0.20f;     // 0.4.28 (GI): the radar element is 80 % map on top, 20 % HP + satiety band at the bottom
constexpr float kStripMinH = 11.0f;        // and never thinner than this: 11 px holds the 7 px glyph class (2 px above and below) and a 5 px bar
struct Cluster {
    float x = 0, w = 0;                      // left edge and width of the speedometer (and of the vitals plate while it stays here)
    float gut = 0, gap = 0;                  // screen gutter, space between the two plates
    float padX = 0, padT = 0, padB = 0;      // vitals plate: side, top (clears the rail) and bottom padding
    float rowH = 0, rowGap = 0;
    float vH = 0, vTop = 0;                  // vitals plate height now (animated rows) and its top edge
    float vx = 0, vw = 0;                    // the vitals' left edge and width (c.x / c.w unless docked); vH / vTop become the strip's
    float speedoBottom = 0;                  // where the speedometer's bottom edge goes
    bool  docked = false;                    // the vitals are the strip under the radar
    int   dockWhy = -1;                      // gage::sync::DockWhy as an int; -1 = no radar asked for
    float radarX = 0, radarY = 0, radarW = 0, radarH = 0;   // the radar rectangle used (docking asked for)
    float coverPx = 0;                       // how far the docked strip reaches up into the radar (0 = wholly under it)
    float dockY = 0;                         // the docked strip's resting top edge (vTop is that while it slides in or out)
};
Cluster ClusterLayout(float W, float H)
{
    Cluster c;
    c.gut = S(28.0f);
    c.gap = S(10.0f);
    c.w = std::floor(std::fmin(S(272.0f), std::fmax(S(160.0f), W - 2.0f * c.gut)));
    c.x = std::floor(W - c.gut - c.w);
    c.padX = S(20.0f);
    c.padT = S(20.0f);
    c.padB = S(16.0f);
    c.rowH = S(22.0f);
    c.rowGap = S(10.0f);
    const float rows = 2.0f;                 // health and satiety, always: fuel lives on the speedometer plate
    c.vH = std::floor(c.padT + rows * c.rowH + (rows - 1.0f) * c.rowGap + c.padB);
    // The vitals plate slides in from below the screen edge (and out again); the speedometer rides on its
    // top edge until the plate is gone, then rests on the gutter (this stacking holds for the bottom-right
    // plate only: with the plate docked under the radar, below, the speedometer always rests on the gutter).
    const float e = u.vitalsA * u.vitalsA * (3.0f - 2.0f * u.vitalsA);
    c.vTop = std::floor(H - c.gut - c.vH + (1.0f - e) * (c.vH + c.gut));
    c.speedoBottom = std::floor(std::fmin(H - c.gut, c.vTop - c.gap));
    c.vx = c.x;
    c.vw = c.w;
    if (g_radarDock.load()) {
        const gage::sync::ScreenRect radar =
            gage::sync::RadarKeepOut(W, H, g_radarSafeZone.load(), g_radarSafeZoneOk.load(), 0.0f, g_radarScale.load());
        c.radarX = radar.x0; c.radarY = radar.y0; c.radarW = radar.x1 - radar.x0; c.radarH = radar.y1 - radar.y0;
        const float edge = std::fmax(2.0f, std::floor(S(2.0f)));
        // 0.4.28 (GI): the radar and the vitals are ONE element, the top 80 % map and the bottom 20 % the HP and satiety band, the radar's
        // own left edge and width. The band covers the bottom kElemBandFrac of the radar rectangle (where GTA's own bars were, plus the map's
        // bottom edge) instead of hanging under it. DockVitals still decides whether the radar is usable at all (on screen, wide enough).
        const float stripH = std::fmax(kStripMinH, std::floor(c.radarH * kElemBandFrac));
        const gage::sync::VitalsDock d = gage::sync::DockVitals(W, H, radar, stripH, S(120.0f), S(400.0f), 0.0f, edge, 1.0f);
        c.dockWhy = (int)d.why;
        if (d.docked) {
            c.docked = true;
            c.vx = std::floor(radar.x0);
            c.vw = std::floor(radar.x1) - c.vx;
            c.vH = stripH;
            const float top = std::floor(radar.y1) - stripH;
            c.coverPx = std::floor(radar.y1) - top;
            c.dockY = top;
            c.vTop = std::floor(top + (1.0f - e) * (H - top));
            c.speedoBottom = std::floor(H - c.gut);        // the speedometer no longer rides on the vitals
        }
    }
    return c;
}

// Below these levels a vital turns the warning ember and pulses: health, satiety, fuel.
constexpr float kVitalLow[3] = {0.30f, 0.25f, 0.15f};

// Icon-first vitals (qb-hud / ps-hud read): one filled glyph per stat is the glance target, not a word.
// Each is drawn from primitives inside a box of side `s` at (x, y), so it is crisp at 14-16 px.
void IconHeart(ImDrawList* dl, float x, float y, float s, ImU32 col)
{
    const float r = s * 0.26f, cy = y + s * 0.34f;
    dl->AddCircleFilled(ImVec2(x + s * 0.31f, cy), r, col, 14);
    dl->AddCircleFilled(ImVec2(x + s * 0.69f, cy), r, col, 14);
    const ImVec2 tri[3] = {{x + s * 0.05f, cy - r * 0.10f}, {x + s * 0.95f, cy - r * 0.10f}, {x + s * 0.50f, y + s * 0.97f}};
    dl->AddTriangleFilled(tri[0], tri[1], tri[2], col);
}
// A burger: a domed top bun, a patty that overhangs the buns, a flat bottom bun. Filled like the heart, so the
// two glyphs carry the same weight; the gaps between the layers (at least 1 px) are what make it read at 16 px.
// It replaced a fork and knife (0.4.22, GI): thin strokes that blurred into a squiggle at HUD size.
void IconFood(ImDrawList* dl, float x, float y, float s, ImU32 col)
{
    // Every edge lands on a whole pixel (visual-critic, 0.4.22): with fractional edges the 16 px glyph's lower gap fell
    // across two rows and smeared the patty into the bun. 16 px: dome 6, gap 1, patty 2, gap 1, bun 3 rows; the patty
    // overhangs the buns by exactly 1 px a side.
    auto R = [](float v) { return std::round(v); };
    const float gap = std::fmax(1.0f, R(s * 0.08f));
    const float domeY0 = R(y + s * 0.06f), topY1 = R(y + s * 0.44f);
    const float patY0 = topY1 + gap, patY1 = patY0 + std::fmax(2.0f, R(s * 0.13f));
    const float botY0 = patY1 + gap, botY1 = botY0 + std::fmax(3.0f, R(s * 0.19f));
    const float domeX0 = R(x + s * 0.06f), domeX1 = R(x + s * 0.94f);
    // Top bun: a half ellipse standing on its flat base.
    const float cx = (domeX0 + domeX1) * 0.5f, rx = (domeX1 - domeX0) * 0.5f, ry = topY1 - domeY0;
    const int kSeg = 16;
    dl->PathClear();
    for (int i = 0; i <= kSeg; ++i) {
        const float t = 3.14159265f * (1.0f + (float)i / (float)kSeg);   // pi .. 2pi: left base, over the top, right base
        dl->PathLineTo(ImVec2(cx + rx * std::cos(t), topY1 + ry * std::sin(t)));
    }
    dl->PathFillConvex(col);
    dl->AddRectFilled(ImVec2(R(x + s * 0.02f), patY0), ImVec2(R(x + s * 0.98f), patY1), col, s * 0.07f);
    dl->AddRectFilled(ImVec2(R(x + s * 0.08f), botY0), ImVec2(R(x + s * 0.92f), botY1), col, s * 0.09f,
                      ImDrawFlags_RoundCornersBottom);
}
// (IconFuel, the petrol pump, is a pixel glyph since 0.4.30: below PixelGlyph)

// ---- Pixel glyphs (0.4.22) ---------------------------------------------------------------------------------------------------------------------
// The vitals strip and the speedometer's status strip draw hand-authored bitmaps, not primitives: IconHeart's anti-aliased circles blur to a blob in a
// 6-13 px box, and a diagonal or a curve built from a triangle or a line lands between pixels at 10-18 px (the old turn arrow was two shapes with a
// double-alpha seam where they overlapped). A glyph is a few size classes of one bitmap ('X' = the glyph's colour, 'x' = the same colour at kPixelTone
// of its alpha, '.' = nothing); a box takes the largest class that fits, drawn as one rectangle per run, every edge on a whole pixel, so one bitmap is
// one alpha with no seam. The row strings are checked at compile time (BmpOk): exactly w characters a row, exactly h rows, only those three characters.
constexpr float kPixelTone = 0.55f;
struct Bmp {
    int w, h;
    const char* const* rows;
};
constexpr bool BmpOk(const Bmp& b)
{
    if (b.w <= 0 || b.h <= 0 || !b.rows)
        return false;
    for (int r = 0; r < b.h; ++r) {
        if (!b.rows[r])
            return false;
        int c = 0;
        for (; b.rows[r][c]; ++c)
            if (b.rows[r][c] != '.' && b.rows[r][c] != 'X' && b.rows[r][c] != 'x')
                return false;
        if (c != b.w)
            return false;
    }
    return true;
}
constexpr bool ClassesOk(const Bmp* cls, int n)
{
    for (int i = 0; i < n; ++i)
        if (!BmpOk(cls[i]) || (i > 0 && cls[i].h <= cls[i - 1].h))
            return false;      // ascending by height, or PixelPick would take the wrong class
    return true;
}
// A glyph's classes, ascending by height. From a box side of scaleFrom px up the class scaleCls is drawn at a whole factor (box / scaleBase) instead.
struct PixelSet {
    const Bmp* cls;
    int n;
    int scaleCls, scaleBase, scaleFrom;
};
// The class a box of side n takes (and the whole factor k it is drawn at); null when even the smallest class does not fit.
const Bmp* PixelPick(const PixelSet& g, int n, int* k)
{
    *k = 1;
    if (n < g.cls[0].h)
        return nullptr;
    if (g.scaleCls >= 0 && n >= g.scaleFrom) {
        *k = n / g.scaleBase;
        return &g.cls[g.scaleCls];
    }
    const Bmp* best = &g.cls[0];
    for (int i = 1; i < g.n; ++i)
        if (g.cls[i].h <= n)
            best = &g.cls[i];
    return best;
}
ImU32 ScaleAlpha(ImU32 col, float k)
{
    const ImU32 a = (col >> IM_COL32_A_SHIFT) & 0xFF;
    return (col & ~IM_COL32_A_MASK) | ((ImU32)((float)a * k + 0.5f) << IM_COL32_A_SHIFT);
}
// Draws a bitmap with its top-left corner at (ox, oy) (whole pixels), every cell k px square; flip mirrors it left to right (one bitmap, both arrows).
void PixelDraw(ImDrawList* dl, float ox, float oy, const Bmp& b, int k, ImU32 col, bool flip)
{
    const ImU32 tone = ScaleAlpha(col, kPixelTone);
    const float kf = (float)k;
    for (int r = 0; r < b.h; ++r) {
        for (int c0 = 0; c0 < b.w;) {
            const char ch = b.rows[r][flip ? b.w - 1 - c0 : c0];
            if (ch == '.') {
                ++c0;
                continue;
            }
            int c1 = c0 + 1;
            while (c1 < b.w && b.rows[r][flip ? b.w - 1 - c1 : c1] == ch)
                ++c1;
            dl->AddRectFilled(ImVec2(ox + kf * (float)c0, oy + kf * (float)r), ImVec2(ox + kf * (float)c1, oy + kf * (float)(r + 1)), ch == 'X' ? col : tone);
            c0 = c1;
        }
    }
}
// A glyph centred in a square box of side s at (x, y): the vitals strip's two glyphs.
void PixelGlyph(ImDrawList* dl, float x, float y, float s, const PixelSet& g, ImU32 col)
{
    const int n = (int)std::floor(s);
    const float x0 = std::floor(x), y0 = std::floor(y);
    int k = 1;
    const Bmp* b = PixelPick(g, n, &k);
    if (!b) {
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + (float)n, y0 + (float)n), col);
        return;
    }
    PixelDraw(dl, x0 + (float)((n - b->w * k) / 2), y0 + (float)((n - b->h * k) / 2), *b, k, col, false);
}

// 0.4.30 the fuel gauge's petrol pump (GI: the old primitive pump read as a battery / a bracket): a pump body with its display window cut out, a
// wide base, and the hose looping out of the body's side up to the nozzle hung on top, in classes of 10, 14 and 20 rows (14 scaled from 28 up).
constexpr const char* const kFuel10[10] = {".XXXX..X..", ".X..X..XX.", ".XXXX...X.", ".XXXX...X.", ".XXXXX..X.",
                                           ".XXXX.X.X.", ".XXXX.XXX.", ".XXXX.....", "XXXXXX....", "XXXXXX...."};
constexpr const char* const kFuel14[14] = {".XXXXXX..XX...", ".X....X..XXX..", ".X....X...XX..", ".X....X....X..", ".XXXXXX....X..", ".XXXXXX....X..",
                                           ".XXXXXX....X..", ".XXXXXXXX..X..", ".XXXXXX.X..X..", ".XXXXXX.XXXX..", ".XXXXXX.......", ".XXXXXX.......",
                                           "XXXXXXXX......", "XXXXXXXX......"};
constexpr const char* const kFuel20[20] = {".XXXXXXXXX....XXX...", ".XXXXXXXXX....XXXX..", ".XX.....XX.....XXX..", ".XX.....XX......XX..", ".XX.....XX......XX..",
                                           ".XX.....XX......XX..", ".XX.....XX......XX..", ".XXXXXXXXX......XX..", ".XXXXXXXXX......XX..", ".XXXXXXXXX......XX..",
                                           ".XXXXXXXXXXX....XX..", ".XXXXXXXXXXX....XX..", ".XXXXXXXXX.XX...XX..", ".XXXXXXXXX.XXXXXXX..", ".XXXXXXXXX..XXXXX...",
                                           ".XXXXXXXXX..........", ".XXXXXXXXX..........", ".XXXXXXXXX..........", "XXXXXXXXXXXX........", "XXXXXXXXXXXX........"};
constexpr Bmp kFuelCls[3] = {{10, 10, kFuel10}, {14, 14, kFuel14}, {20, 20, kFuel20}};
static_assert(ClassesOk(kFuelCls, 3), "fuel pump bitmaps: a row is not w wide / a class is not h rows / an unknown character");
constexpr PixelSet kFuelSet = {kFuelCls, 3, 1, 14, 28};
void IconFuel(ImDrawList* dl, float x, float y, float s, ImU32 col) { PixelGlyph(dl, x, y, s, kFuelSet, col); }

// ---- the vitals strip's glyphs: square classes of 5, 7, 9 and 13 px, the 9 px class scaled by a whole factor from an 18 px box up. Both glyphs sit at
// the same offset in a box, so they span the same rows (one baseline).
constexpr const char* const kHeart5[5] = {".X.X.", "XXXXX", "XXXXX", ".XXX.", "..X.."};
constexpr const char* const kHeart7[7] = {".XX.XX.", "XXXXXXX", "XXXXXXX", "XXXXXXX", ".XXXXX.", "..XXX..", "...X..."};
constexpr const char* const kHeart9[9] = {".XX...XX.", "XXXX.XXXX", "XXXXXXXXX", "XXXXXXXXX", "XXXXXXXXX", ".XXXXXXX.", "..XXXXX..", "...XXX...", "....X...."};
constexpr const char* const kHeart13[13] = {".XXXX...XXXX.", "XXXXXX.XXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX",
                                            "XXXXXXXXXXXXX", ".XXXXXXXXXXX.", "..XXXXXXXXX..", "...XXXXXXX...", "....XXXXX....", ".....XXX.....", "......X......"};
// The burger (critic fix: three flat bars read as the "hamburger menu" icon): a domed top bun whose widest rows carry single-pixel sesame knockouts, a gap,
// the patty in the second tone ('x'), a gap, a two-row bun with rounded lower corners. 5 and 7 px have no room for all of that: the 7 px burger keeps a
// three-row dome (3, 5, 7 wide: the curve is what stops it reading as the menu icon), the patty right under it (the tone alone parts them), one gap, the bun.
constexpr const char* const kBurger5[5] = {".XXX.", "XXXXX", "xxxxx", "XXXXX", ".XXX."};
constexpr const char* const kBurger7[7] = {"..XXX..", ".XXXXX.", "XXXXXXX", "xxxxxxx", ".......", "XXXXXXX", ".XXXXX."};
constexpr const char* const kBurger9[9] = {"..XXXXX..", ".XXX.XXX.", "XX.XXX.XX", "XXXXXXXXX", ".........", "xxxxxxxxx", ".........", "XXXXXXXXX", ".XXXXXXX."};
constexpr const char* const kBurger13[13] = {"....XXXXX....", "..XXXX.XXXX..", ".XXX.XXX.XXX.", "XX.XXX.XXX.XX", "XXXXXXXXXXXXX", ".............", "xxxxxxxxxxxxx",
                                             "xxxxxxxxxxxxx", ".............", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", ".XXXXXXXXXXX.", "..XXXXXXXXX.."};
constexpr Bmp kHeartCls[4] = {{5, 5, kHeart5}, {7, 7, kHeart7}, {9, 9, kHeart9}, {13, 13, kHeart13}};
constexpr Bmp kBurgerCls[4] = {{5, 5, kBurger5}, {7, 7, kBurger7}, {9, 9, kBurger9}, {13, 13, kBurger13}};
static_assert(ClassesOk(kHeartCls, 4) && ClassesOk(kBurgerCls, 4), "vitals bitmaps: a row is not w wide / a class is not h rows / an unknown character");
constexpr PixelSet kHeartSet = {kHeartCls, 4, 2, 9, 18};
constexpr PixelSet kBurgerSet = {kBurgerCls, 4, 2, 9, 18};
void IconHeartMini(ImDrawList* dl, float x, float y, float s, ImU32 col) { PixelGlyph(dl, x, y, s, kHeartSet, col); }
void IconFoodMini(ImDrawList* dl, float x, float y, float s, ImU32 col) { PixelGlyph(dl, x, y, s, kBurgerSet, col); }

// ---- the speedometer's status strip (vehicle controls): a left arrow (the right one is its mirror), the headlamp (low / high beam), the seat belt and the
// engine, in three size classes around 10 / 14 / 18 px (the box is floor(S(14)): 11 at 720p, 14 at 1080p, 19 at 1440p; a 14 class scales by a whole
// factor from 28 px, 4K). Cells in the box's centre line are mirrored about the line between two rows (even heights), so nothing is off by half a pixel.
//
// The turn arrow: a 45 degree head and a stem, the tip two cells wide (one bitmap, mirrored for the right arrow).
constexpr const char* const kTurn10[10] = {"....X........", "...XX........", "..XXX........", ".XXXXXXXXXXXX", "XXXXXXXXXXXXX",
                                           "XXXXXXXXXXXXX", ".XXXXXXXXXXXX", "..XXX........", "...XX........", "....X........"};
constexpr const char* const kTurn14[14] = {"......X............", ".....XX............", "....XXX............", "...XXXX............", "..XXXXXXXXXXXXXXXXX",
                                           ".XXXXXXXXXXXXXXXXXX", "XXXXXXXXXXXXXXXXXXX", "XXXXXXXXXXXXXXXXXXX", ".XXXXXXXXXXXXXXXXXX", "..XXXXXXXXXXXXXXXXX",
                                           "...XXXX............", "....XXX............", ".....XX............", "......X............"};
constexpr const char* const kTurn18[18] = {"........X...............", ".......XX...............", "......XXX...............", ".....XXXX...............",
                                           "....XXXXX...............", "...XXXXXX...............", "..XXXXXXXXXXXXXXXXXXXXXX", ".XXXXXXXXXXXXXXXXXXXXXXX",
                                           "XXXXXXXXXXXXXXXXXXXXXXXX", "XXXXXXXXXXXXXXXXXXXXXXXX", ".XXXXXXXXXXXXXXXXXXXXXXX", "..XXXXXXXXXXXXXXXXXXXXXX",
                                           "...XXXXXX...............", "....XXXXX...............", ".....XXXX...............", "......XXX...............",
                                           ".......XX...............", "........X..............."};
// The headlamp: a D (flat side left) with three beams streaming left, 1 cell thick at 9 px and 2 at 14 / 18. High beam: level. Low beam: each beam steps
// down twice on its way out (a 2-step stair, 1 cell a step), so the two read apart by shape as well as by colour.
constexpr const char* const kHigh9[9] = {"......XXX..", "XXXXX.XXXX.", "......XXXXX", "......XXXXX", "XXXXX.XXXXX", "......XXXXX", "......XXXXX", "XXXXX.XXXX.", "......XXX.."};
constexpr const char* const kLow9[9] = {"...XX.XXX..", ".XX...XXXX.", "X.....XXXXX", "...XX.XXXXX", ".XX...XXXXX", "X.....XXXXX", "...XX.XXXXX", ".XX...XXXX.", "X.....XXX.."};
constexpr const char* const kHigh14[14] = {"..........XXXX...", "XXXXXXXXX.XXXXXX.", "XXXXXXXXX.XXXXXXX", "..........XXXXXXX", "..........XXXXXXX", "..........XXXXXXX",
                                           "XXXXXXXXX.XXXXXXX", "XXXXXXXXX.XXXXXXX", "..........XXXXXXX", "..........XXXXXXX", "..........XXXXXXX", "XXXXXXXXX.XXXXXXX",
                                           "XXXXXXXXX.XXXXXX.", "..........XXXX..."};
constexpr const char* const kLow14[14] = {"......XXX.XXXX...", "...XXXXXX.XXXXXX.", "XXXXXX....XXXXXXX", "XXX.......XXXXXXX", "..........XXXXXXX", "......XXX.XXXXXXX",
                                          "...XXXXXX.XXXXXXX", "XXXXXX....XXXXXXX", "XXX.......XXXXXXX", "..........XXXXXXX", "......XXX.XXXXXXX", "...XXXXXX.XXXXXXX",
                                          "XXXXXX....XXXXXX.", "XXX.......XXXX..."};
constexpr const char* const kHigh18[18] = {".............XXXXX....", ".............XXXXXXX..", "XXXXXXXXXXXX.XXXXXXXX.", "XXXXXXXXXXXX.XXXXXXXXX", ".............XXXXXXXXX",
                                           ".............XXXXXXXXX", ".............XXXXXXXXX", ".............XXXXXXXXX", "XXXXXXXXXXXX.XXXXXXXXX", "XXXXXXXXXXXX.XXXXXXXXX",
                                           ".............XXXXXXXXX", ".............XXXXXXXXX", ".............XXXXXXXXX", ".............XXXXXXXXX",
                                           "XXXXXXXXXXXX.XXXXXXXXX", "XXXXXXXXXXXX.XXXXXXXX.", ".............XXXXXXX..", ".............XXXXX...."};
constexpr const char* const kLow18[18] = {".............XXXXX....", "........XXXX.XXXXXXX..", "....XXXXXXXX.XXXXXXXX.", "XXXXXXXX.....XXXXXXXXX", "XXXX.........XXXXXXXXX",
                                          ".............XXXXXXXXX", ".............XXXXXXXXX", "........XXXX.XXXXXXXXX", "....XXXXXXXX.XXXXXXXXX", "XXXXXXXX.....XXXXXXXXX",
                                          "XXXX.........XXXXXXXXX", ".............XXXXXXXXX", ".............XXXXXXXXX", "........XXXX.XXXXXXXXX",
                                          "....XXXXXXXX.XXXXXXXXX", "XXXXXXXX.....XXXXXXXX.", "XXXX.........XXXXXXX..", ".............XXXXX...."};
// The seat-belt telltale (ISO 2575): a person seated facing right, head, torso, thigh forward, shin down, and a 1 cell diagonal strap cut from the
// shoulder to the hip. The strap is a hole in the bitmap, so it is there in every state (idle ghost, buckled, warning); the head is 3x3 at 10 px.
constexpr const char* const kBelt10[10] = {"...XXX...", "...XXX...", "...XXX...", ".........", "..X.XXX..", "..XX.XX..", "..XXX.XXX", "..XXXX.XX", ".......XX", ".......XX"};
constexpr const char* const kBelt14[14] = {".....XX.....", "....XXXX....", "....XXXX....", ".....XX.....", "............", "...X.XXXX...", "...XX.XXX...",
                                           "...XXX.XX...", "...XXXX.X...", "...XXXXX.XXX", "...XXXXXX.XX", "...XXXXXXXXX", ".........XXX", ".........XXX"};
constexpr const char* const kBelt18[18] = {"......XXX......", ".....XXXXX.....", ".....XXXXX.....", ".....XXXXX.....", "......XXX......", "...............",
                                           "....X.XXXXX....", "....XX.XXXX....", "....XXX.XXX....", "....XXXX.XX....", "....XXXXX.X....", "....XXXXXX.XXXX",
                                           "....XXXXXXX.XXX", "....XXXXXXXXXXX", "....XXXXXXXXXXX", "...........XXXX", "...........XXXX", "...........XXXX"};
// The engine-warning pictogram: a block with a capped intake on top, a shaft with a crossbar on the left, a stub on the right and a vent slit cut out of
// the block, so it reads as a machine and not as a blob.
constexpr const char* const kEngine9[9] = {"...XXXXX.....", "....XXX......", "..XXXXXXXXX..", "X.XXXXXXXXX..", "XXXXXXXXXXXXX", "XXXX.....XXXX", "XXXXXXXXXXXXX", "X.XXXXXXXXX..", "..XXXXXXXXX.."};
constexpr const char* const kEngine13[13] = {".....XXXXXXXX.....", ".....XXXXXXXX.....", ".......XXXX.......", ".......XXXX.......", "...XXXXXXXXXXXX...",
                                             "XX.XXXXXXXXXXXX...", "XX.XXXXXXXXXXXX...", "XXXXXXXXXXXXXXXXXX", "XXXXX........XXXXX", "XXXXXXXXXXXXXXXXXX",
                                             "XX.XXXXXXXXXXXX...", "XX.XXXXXXXXXXXX...", "...XXXXXXXXXXXX..."};
constexpr const char* const kEngine17[17] = {"......XXXXXXXXXXX......", "......XXXXXXXXXXX......", "......XXXXXXXXXXX......", ".........XXXXX.........", ".........XXXXX.........",
                                             "....XXXXXXXXXXXXXXX....", "XX..XXXXXXXXXXXXXXX....", "XX..XXXXXXXXXXXXXXX....", "XX..XXXXXXXXXXXXXXX....",
                                             "XXXXXXXXXXXXXXXXXXXXXXX", "XXXXXXX.........XXXXXXX", "XXXXXXX.........XXXXXXX", "XXXXXXXXXXXXXXXXXXXXXXX",
                                             "XX..XXXXXXXXXXXXXXX....", "XX..XXXXXXXXXXXXXXX....", "XX..XXXXXXXXXXXXXXX....", "....XXXXXXXXXXXXXXX...."};
constexpr Bmp kTurnCls[3] = {{13, 10, kTurn10}, {19, 14, kTurn14}, {24, 18, kTurn18}};
constexpr Bmp kHighCls[3] = {{11, 9, kHigh9}, {17, 14, kHigh14}, {22, 18, kHigh18}};
constexpr Bmp kLowCls[3] = {{11, 9, kLow9}, {17, 14, kLow14}, {22, 18, kLow18}};
constexpr Bmp kBeltCls[3] = {{9, 10, kBelt10}, {12, 14, kBelt14}, {15, 18, kBelt18}};
constexpr Bmp kEngineCls[3] = {{13, 9, kEngine9}, {18, 13, kEngine13}, {23, 17, kEngine17}};
static_assert(ClassesOk(kTurnCls, 3) && ClassesOk(kHighCls, 3) && ClassesOk(kLowCls, 3) && ClassesOk(kBeltCls, 3) && ClassesOk(kEngineCls, 3),
              "status bitmaps: a row is not w wide / a class is not h rows / an unknown character");
constexpr PixelSet kTurnSet = {kTurnCls, 3, 1, 14, 28};
constexpr PixelSet kHighSet = {kHighCls, 3, 1, 14, 28};
constexpr PixelSet kLowSet = {kLowCls, 3, 1, 14, 28};
constexpr PixelSet kBeltSet = {kBeltCls, 3, 1, 14, 28};
constexpr PixelSet kEngineSet = {kEngineCls, 3, 1, 14, 28};

// The cluster's accent rail reflects the vitals: gold normally, ember as any vital goes low, and it
// pulses once any vital is critical (at or below half its low mark). Both plates call this, so the whole
// instrument block alarms together instead of only three small fills flickering.
void RailState(Rgb* col, float* alpha, float baseA)
{
    float warn = 0.0f;
    bool crit = false;
    for (int i = 0; i < 3; ++i) {
        if (i == 2 && u.fuelA < 0.5f)
            continue;                        // fuel only counts once its gauge is live
        warn = std::fmax(warn, u.vit[i].warn);
        if (u.vit[i].low && u.vit[i].fill <= kVitalLow[i] * 0.5f)
            crit = true;
    }
    *col = Mix(kGold, kEmber, warn);
    if (crit) {
        *alpha = baseA * (0.55f + 0.45f * AlarmPulse(HudPhase()));      // the HUD clock's alarm beat, the same as the glyphs and the belt
    } else {
        *alpha = baseA;
    }
}

// One brand rule for the pair: a chamfered plate, a hairline and the accent rail along the top
// edge from the end of the chamfer (as on the chat and the palette). The rail carries the vitals alarm.
void ClusterPlate(ImDrawList* dl, float x, float y, float w, float h, float a)
{
    const float ch = S(16.0f);
    PlateChamfer(dl, x, y, x + w, y + h, ch, Col(kPanel, 0.92f * a), Col(kHair, 0.12f * a));
    Rgb rc;
    float ra;
    RailState(&rc, &ra, a);
    dl->AddRectFilled(ImVec2(x + ch, y), ImVec2(x + ch + S(56.0f), y + std::fmax(2.0f, S(3.0f))), Col(rc, ra));
}

// Once per frame, whether or not the cluster is drawn: eases the cluster's visibility and every bar
// toward what the engine pushed. Coming up from hidden snaps to the true level instead of sweeping
// from a stale one (the fuel row snaps each time it appears).
void TickVitals(float dt, bool loading)
{
    const bool show = g_vitShow.load() && !loading;
    const bool fuel = show && g_vitShowFuel.load();
    const float target[3] = {Frac01(g_vitHealth.load()), Frac01(g_vitSatiety.load()), Frac01(g_vitFuel.load())};
    const bool snapAll = u.vitalsA <= 0.003f;
    const bool snapFuel = snapAll || u.fuelA <= 0.003f;
    u.vitalsA = Approach(u.vitalsA, show ? 1.0f : 0.0f, dt / 0.18f);
    u.fuelA = Approach(u.fuelA, fuel ? 1.0f : 0.0f, dt / 0.20f);
    for (int i = 0; i < 3; ++i) {
        Ui::Vital& v = u.vit[i];
        const float t = target[i];
        if (!v.low && t <= kVitalLow[i])
            v.low = true;
        else if (v.low && t > kVitalLow[i] + 0.04f)
            v.low = false;                                   // a little hysteresis: no flicker at the line
        if (i == 2 ? snapFuel : snapAll) {
            v.fill = v.trail = t;
            v.low = t <= kVitalLow[i];
            v.warn = v.low ? 1.0f : 0.0f;
            continue;
        }
        v.fill = Ease(v.fill, t, 9.0f, dt, 0.002f);
        // After a loss the trail hangs above the fill and drains slowly, so a hit reads as a hit.
        v.trail = t < v.trail ? std::fmax(Ease(v.trail, t, 2.0f, dt, 0.002f), v.fill) : v.fill;
        v.warn = Ease(v.warn, v.low ? 1.0f : 0.0f, 10.0f, dt, 0.01f);
    }
}

// The speedometer: the number is the hero, the gauge a rising wedge of leaning bars beside it.
//   [ 128         /////// ]   digits left, leading zeros ghosted so the slot never moves
//   [ KM/H   (pump) ====== ]  the unit under the number; the fuel gauge rides the same baseline (in a vehicle)
// The wedge lights bar by bar (the bar at the tip fades in with the remainder, so the gauge moves
// continuously), the tip in ink; every bar is a neutral ghost at rest, and the last three light ember
// only once speed reaches them (colour means wrong, never decoration). `speed` is the eased value.
// The padlock, closed and open, in square classes of 7, 9 and 13 px (the 9 px class scaled by a whole factor from an 18 px box up, like the heart). The
// open lock's shackle stands one row (two at 13 px) higher with its left leg out of the body: the two read apart by shape, not only by colour.
constexpr const char* const kLockC7[7] = {"..XXX..", ".X...X.", ".X...X.", "XXXXXXX", "XXX.XXX", "XXX.XXX", "XXXXXXX"};
constexpr const char* const kLockO7[7] = {"..XXX..", ".X...X.", ".....X.", ".....X.", "XXXXXXX", "XXX.XXX", "XXXXXXX"};
constexpr const char* const kLockC9[9] = {"..XXXXX..", ".XX...XX.", ".X.....X.", ".X.....X.", "XXXXXXXXX", "XXXX.XXXX", "XXXX.XXXX", "XXXXXXXXX", "XXXXXXXXX"};
constexpr const char* const kLockO9[9] = {"..XXXXX..", ".XX...XX.", ".X.....X.", ".......X.", ".......X.", "XXXXXXXXX", "XXXX.XXXX", "XXXX.XXXX", "XXXXXXXXX"};
constexpr const char* const kLockC13[13] = {"...XXXXXXX...", "..XXXXXXXXX..", "..XX.....XX..", "..XX.....XX..", "..XX.....XX..", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX",
                                            "XXXXXX.XXXXXX", "XXXXX...XXXXX", "XXXXXX.XXXXXX", "XXXXXX.XXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX"};
constexpr const char* const kLockO13[13] = {"...XXXXXXX...", "..XXXXXXXXX..", "..XX.....XX..", ".........XX..", ".........XX..", ".........XX..", "XXXXXXXXXXXXX",
                                            "XXXXXXXXXXXXX", "XXXXXX.XXXXXX", "XXXXX...XXXXX", "XXXXXX.XXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX"};
constexpr Bmp kLockCCls[3] = {{7, 7, kLockC7}, {9, 9, kLockC9}, {13, 13, kLockC13}};
constexpr Bmp kLockOCls[3] = {{7, 7, kLockO7}, {9, 9, kLockO9}, {13, 13, kLockO13}};
static_assert(ClassesOk(kLockCCls, 3) && ClassesOk(kLockOCls, 3), "padlock bitmaps: a row is not w wide / a class is not h rows / an unknown character");
constexpr PixelSet kLockCSet = {kLockCCls, 3, 1, 9, 18};
constexpr PixelSet kLockOSet = {kLockOCls, 3, 1, 9, 18};
// The speedometer strip's padlock, native in the strip's own classes (10, 14, 18 rows; 14 scaled by a whole factor from 28 up, like its neighbours).
// Both states share one body on the bottom row (the strip's baseline) and one keyhole slot; the open shackle is lifted clear (its left leg ends at
// least 2 rows above the body), so unlocked and unknown (= the closed shape, ghosted) read apart by outline, not by a 1-row nudge.
constexpr const char* const kSLockC10[10] = {"........", "..XXXX..", ".X....X.", ".X....X.", ".X....X.", "XXXXXXXX", "XXX..XXX", "XXX..XXX", "XXXXXXXX", "XXXXXXXX"};
constexpr const char* const kSLockO10[10] = {"..XXXX..", ".X....X.", ".X....X.", "......X.", "......X.", "XXXXXXXX", "XXX..XXX", "XXX..XXX", "XXXXXXXX", "XXXXXXXX"};
constexpr const char* const kSLockC14[14] = {"...........", "...........", "...XXXXX...", "..XXXXXXX..", "..XX...XX..", "..XX...XX..", "..XX...XX..",
                                             "XXXXXXXXXXX", "XXXXXXXXXXX", "XXXXX.XXXXX", "XXXXX.XXXXX", "XXXXX.XXXXX", "XXXXXXXXXXX", "XXXXXXXXXXX"};
constexpr const char* const kSLockO14[14] = {"...XXXXX...", "..XXXXXXX..", "..XX...XX..", "..XX...XX..", ".......XX..", ".......XX..", ".......XX..",
                                             "XXXXXXXXXXX", "XXXXXXXXXXX", "XXXXX.XXXXX", "XXXXX.XXXXX", "XXXXX.XXXXX", "XXXXXXXXXXX", "XXXXXXXXXXX"};
constexpr const char* const kSLockC18[18] = {"..............", "..............", "..............", "....XXXXXX....", "...XXXXXXXX...", "...XX....XX...",
                                             "...XX....XX...", "...XX....XX...", "...XX....XX...", "XXXXXXXXXXXXXX", "XXXXXXXXXXXXXX", "XXXXXX..XXXXXX",
                                             "XXXXXX..XXXXXX", "XXXXXX..XXXXXX", "XXXXXX..XXXXXX", "XXXXXXXXXXXXXX", "XXXXXXXXXXXXXX", "XXXXXXXXXXXXXX"};
constexpr const char* const kSLockO18[18] = {"....XXXXXX....", "...XXXXXXXX...", "...XX....XX...", "...XX....XX...", "...XX....XX...", "...XX....XX...",
                                             ".........XX...", ".........XX...", ".........XX...", "XXXXXXXXXXXXXX", "XXXXXXXXXXXXXX", "XXXXXX..XXXXXX",
                                             "XXXXXX..XXXXXX", "XXXXXX..XXXXXX", "XXXXXX..XXXXXX", "XXXXXXXXXXXXXX", "XXXXXXXXXXXXXX", "XXXXXXXXXXXXXX"};
constexpr Bmp kSLockCCls[3] = {{8, 10, kSLockC10}, {11, 14, kSLockC14}, {14, 18, kSLockC18}};
constexpr Bmp kSLockOCls[3] = {{8, 10, kSLockO10}, {11, 14, kSLockO14}, {14, 18, kSLockO18}};
static_assert(ClassesOk(kSLockCCls, 3) && ClassesOk(kSLockOCls, 3), "strip padlock bitmaps: a row is not w wide / a class is not h rows / an unknown character");
constexpr PixelSet kSLockCSet = {kSLockCCls, 3, 1, 14, 28};
constexpr PixelSet kSLockOSet = {kSLockOCls, 3, 1, 14, 28};
// The first and last inked column of a bitmap (the strip spaces its glyphs by ink, not by box: a box with blank columns crowds its neighbour).
int InkCol(const Bmp& b, bool last)
{
    int lo = b.w, hi = -1;
    for (int r = 0; r < b.h; ++r)
        for (int c = 0; c < b.w; ++c)
            if (b.rows[r][c] != '.') {
                lo = c < lo ? c : lo;
                hi = c > hi ? c : hi;
            }
    return hi < 0 ? 0 : (last ? hi : lo);
}

void DrawSpeedo(ImDrawList* dl, float W, float H, float a, float speed, UiState& L)
{
    const Cluster c = ClusterLayout(W, H);
    const float padX = S(20.0f), padT = S(20.0f), padB = S(14.0f), capGap = S(8.0f);
    const float stripGap = S(10.0f), stripH = S(14.0f);      // 0.4.22 status strip under the caption row (vehicle controls)
    float dTop = 0.0f, dBot = 0.0f;
    DigitBox(u.num, &dTop, &dBot);
    const float dH = dBot - dTop;                            // the numerals' height, the wedge's tallest bar
    const float pw = c.w, px = c.x;
    const float ph = std::floor(padT + dH + capGap + u.small.px + padB + (stripGap + stripH) * u.stripA);
    const float py = std::floor(c.speedoBottom - ph);
    ClusterPlate(dl, px, py, pw, ph, a);

    // The number. Digits are laid left to right from the slot's left edge; the zeros that pad it to three
    // places are ghosted. The top of the digits sits padT under the plate's top, the baseline is yb.
    int shown = (int)(speed + 0.5f);
    shown = shown < 0 ? 0 : (shown > 999 ? 999 : shown);
    char digits[8];
    std::snprintf(digits, sizeof(digits), "%d", shown);
    const int nd = (int)std::strlen(digits);
    const float numX = px + padX, numY = py + padT - dTop;
    const float yb = py + padT + dH;
    float gx = 0.0f;
    if (nd < 3) {
        const char* ghost = nd == 1 ? "00" : "0";
        Text(dl, u.num, ImVec2(numX, numY), kInk, a * 0.16f, ghost);
        gx = Measure(u.num, ghost).x;
    }
    Text(dl, u.num, ImVec2(numX + gx, numY), kInk, a, digits, 0.0f, 0.5f);
    const float slotW = Measure(u.num, "000").x;

    // The wedge: kBars bars leaning with the mark, right-aligned to the plate's padding, as tall as the digits.
    constexpr int kBars = 16, kRedBars = 3;
    const float leanMax = dH * kSkew;
    const float availW = (px + pw - padX) - (numX + slotW + S(20.0f)) - leanMax;
    const float pitch = std::fmax(3.0f, std::floor(availW / (float)kBars));
    const float gapB = std::fmax(2.0f, std::floor(S(2.0f)));
    const float barW = pitch - gapB;
    const float wx0 = std::floor(px + pw - padX - leanMax - pitch * (float)kBars + gapB);
    const float hMin = std::floor(dH * 0.28f);
    const float t = Clamp01(speed / kFullSpeedKmh) * (float)kBars;
    const int tip = t > 0.001f ? (int)std::ceil(t) - 1 : -1;
    for (int i = 0; i < kBars; ++i) {
        const float h = std::floor(hMin + (dH - hMin) * (float)i / (float)(kBars - 1));
        const float lean = h * kSkew;
        const float x = wx0 + pitch * (float)i;
        const bool red = i >= kBars - kRedBars;
        const ImVec2 p[4] = {{x + lean, yb - h}, {x + lean + barW, yb - h}, {x + barW, yb}, {x, yb}};
        dl->AddConvexPolyFilled(p, 4, Col(kInk, 0.12f * a));     // every bar is a neutral ghost until lit
        const float cover = Clamp01(t - (float)i);
        if (cover <= 0.0f)
            continue;
        if (i == tip) {
            // The leading bar: its colour fades in with the remainder, then the ink tip over it.
            dl->AddConvexPolyFilled(p, 4, Col(red ? kEmber : kGold, 0.95f * cover * a));
            dl->AddConvexPolyFilled(p, 4, Col(kInk, 0.90f * cover * a));
        } else {
            dl->AddConvexPolyFilled(p, 4, Col(red ? kEmber : kGold, 0.95f * a));
        }
    }

    // The unit caption under the number; the rest of this baseline carries the fuel gauge (vehicle data)
    // when the engine feeds it, otherwise it stays clear — the wedge already shows the scale.
    const float capY = yb + capGap;
    Text(dl, u.small, ImVec2(numX, capY), kInk2, a * 0.82f, "KM/H");
    int fuelPct = 0;
    bool fuelLow = false;
    // Where the fuel bar's first pixel is: the pump's box and its gaps are layout constants, so it is known whether or not the gauge is live, and the
    // status strip below lines the headlamp's left edge up with it (a bar start and a glyph edge 2 px apart read as a mistake).
    const float pumpS = std::floor(u.small.px * 1.25f);          // a pump a little taller than the caption
    const float fuelBarX0 = std::floor(numX + Measure(u.small, "KM/H").x + S(14.0f) + pumpS + S(10.0f));
    if (u.fuelA > 0.003f) {
        // The fuel gauge on the caption baseline: a pump glyph, a bar (ink-capped, as the vitals bars),
        // and the value in the brand's numerals. Ember as a whole once fuel is low. It is the vital a
        // driver most needs, so it is sized to read, not tucked in.
        const Ui::Vital& fv = u.vit[2];
        const float fa = a * u.fuelA;
        const Rgb fcol = Mix(kGold, kEmber, fv.warn), gcol = Mix(kInk2, kEmber, fv.warn), icol = Mix(kInk, kEmber, fv.warn);
        fuelPct = (int)(fv.fill * 100.0f + 0.5f);
        if (fuelPct == 0 && fv.fill > 0.0f)
            fuelPct = 1;
        fuelPct = fuelPct > 100 ? 100 : fuelPct;
        fuelLow = fv.low;
        char fval[8];
        std::snprintf(fval, sizeof(fval), "%d", fuelPct);
        Fnt fsm = u.numMid;
        fsm.px = std::floor(u.small.px + S(4.0f));                 // the value, bold-italic, matched to the vitals values
        const float gs = pumpS;
        const float gx0 = numX + Measure(u.small, "KM/H").x + S(14.0f);
        IconFuel(dl, gx0, std::floor(capY + (u.small.px - gs) * 0.5f), gs, Col(gcol, fa * 0.95f));
        const float vx = px + pw - padX - Measure(fsm, fval).x;
        Text(dl, fsm, ImVec2(vx, std::floor(capY + (u.small.px - fsm.px) * 0.5f)), icol, fa, fval, 0.0f, 0.5f);
        const float bh = std::fmax(5.0f, S(7.0f)), lean = bh * kSkew, tipw = std::fmax(2.0f, S(2.0f));
        const float by = std::floor(capY + (u.small.px - bh) * 0.5f);
        const float bx0 = fuelBarX0, bx1 = vx - S(10.0f);
        if (bx1 - bx0 >= S(16.0f)) {
            PlateSlant(dl, bx0, by, bx1 - lean, by + bh, Col(kInk, 0.10f * fa), 0);
            const float fx = std::floor(bx0 + (bx1 - lean - bx0) * Clamp01(fv.fill));
            if (fx - bx0 >= 1.0f) {
                PlateSlant(dl, bx0, by, fx, by + bh, Col(fcol, 0.95f * fa), 0);
                if (fx - bx0 >= tipw + 1.0f)
                    PlateSlant(dl, fx - tipw, by, fx, by + bh, Col(kInk, 0.90f * fa), 0);
            }
        }
    }

    // The status strip (0.4.22): [<]  headlamp  belt  engine  [>], pixel bitmaps (PixelSet; the size class comes from the box floor(S(14)): 10 px at 720p, 14 at
    // 1080p, 18 at 1440p). At rest every glyph is a ghost, Col(kInk, 12 %) (the same as the unlit speed bars and every other ghost), so the slots never move;
    // colour means something only when it is on or wrong: a turn arrow blinks gold (the HUD clock's phi < 0.5, both together for the hazards), the headlamp is
    // lit ink on low beam and gold on high beam, a buckled belt is lit ink, an unbuckled belt at speed pulses ember (the alarm beat, 2nd harmonic of the blink),
    // and a stopped engine is lit ink: it is a state, not an alarm (ember stays for alarms). The belt's strap is a hole in its bitmap, so it shows in every
    // state. The outer edges stay on the KM/H caption's left edge and the fuel value's right edge; the centre group (lamp, belt, engine, lock) stays
    // centred, spaced by ink, its first ink on the pump's left edge above it (when no gap in range does, the default gap, centred).
    if (u.stripA > 0.003f) {
        const unsigned hud = g_vehHud.load();
        const bool indL = (hud & 1u) != 0, indR = (hud & 2u) != 0;
        const int lights = (int)((hud >> 2) & 3u);
        const bool beltOn = (hud & 16u) != 0, beltWarn = (hud & 32u) != 0, engOff = (hud & 64u) != 0;
        const float sa = a * u.stripA;
        const int gn = (int)std::floor(stripH);
        const float ay = std::floor(py + padT + dH + capGap + u.small.px + stripGap);
        const float phi = HudPhase();
        const bool blinkOn = phi < 0.5f;
        const float pulse = AlarmPulse(phi);
        const float ghost = 0.12f, litA = 0.92f;
        const ImU32 gh = Col(kInk, sa * ghost);                          // every idle glyph
        auto pick = [&](const PixelSet& s, int* k) -> const Bmp& {
            const Bmp* b = PixelPick(s, gn, k);
            return b ? *b : s.cls[0];
        };
        auto top = [&](const Bmp& b, int k) { return ay + (float)(gn - b.h * k); };   // 0.4.29: every glyph stands on the strip's bottom row
        // 0.4.29 the padlock joins the centre group (GI: lock, belt and engine belong on the car cluster on the right): locked = lit ink closed, unlocked
        // = ghost open, unknown = ghost closed. Bits 256 known, 512 locked. A lock flip seen while the strip is up flashes the lock gold (0.9 s).
        const int lock = (hud & 256u) ? ((hud & 512u) ? 1 : 0) : -1;
        const double nowS = ImGui::GetTime();
        static int s_prevLock = -2;
        static double s_lockFlipAt = -1e9, s_stripSeenAt = -1e9;
        if (lock >= 0 && s_prevLock >= 0 && lock != s_prevLock && nowS - s_stripSeenAt < 0.25)
            s_lockFlipAt = nowS;
        s_prevLock = lock;
        s_stripSeenAt = nowS;
        const float lockFlash = std::fmax(0.0f, 1.0f - (float)(nowS - s_lockFlipAt) / 0.9f);
        int kT = 1, kL = 1, kB = 1, kE = 1, kK = 1;
        const Bmp& bT = pick(kTurnSet, &kT);
        const Bmp& bL = pick(lights == 2 ? kHighSet : kLowSet, &kL);
        const Bmp& bB = pick(kBeltSet, &kB);
        const Bmp& bE = pick(kEngineSet, &kE);
        const Bmp& bK = pick(lock == 0 ? kSLockOSet : kSLockCSet, &kK);
        const float wT = (float)(bT.w * kT);

        const ImU32 lit = Col(kGold, sa * 0.95f);
        PixelDraw(dl, px + padX, top(bT, kT), bT, kT, (indL && blinkOn) ? lit : gh, false);
        PixelDraw(dl, px + pw - padX - wT, top(bT, kT), bT, kT, (indR && blinkOn) ? lit : gh, true);

        // The centre group is spaced by INK (equal gaps between inked edges) and spans the pump's left edge to its mirror on the right, so the arrows and
        // the four telltales read as one evenly spaced bar; when that gap is out of range (a narrow plate), the default gap, centred.
        const Bmp* gb[4] = {&bL, &bB, &bE, &bK};
        const int gk[4] = {kL, kB, kE, kK};
        float inkW[4], inkL[4], sum = 0.0f;
        for (int i = 0; i < 4; ++i) {
            inkL[i] = (float)(InkCol(*gb[i], false) * gk[i]);
            inkW[i] = (float)((InkCol(*gb[i], true) + 1) * gk[i]) - inkL[i];
            sum += inkW[i];
        }
        const float pumpX0 = std::floor(numX + Measure(u.small, "KM/H").x + S(14.0f));
        const float want = pw - 2.0f * (pumpX0 - px);
        float gap = std::floor((want - sum) / 3.0f);
        float gx = pumpX0;
        if (!(gap >= S(8.0f) && gap <= S(34.0f))) {
            gap = std::floor(S(16.0f));
            gx = std::floor(px + (pw - (sum + 3.0f * gap)) * 0.5f);
        }
        float gxs[4];
        for (int i = 0; i < 4; ++i) {
            gxs[i] = gx - inkL[i];                                        // the box's left edge that puts this ink at gx
            gx += inkW[i] + gap;
        }
        const float gxL = gxs[0] + inkL[0];
        PixelDraw(dl, gxs[0], top(bL, kL), bL, kL, lights > 0 ? Col(lights == 2 ? kGold : kInk2, sa * litA) : gh, false);
        PixelDraw(dl, gxs[1], top(bB, kB), bB, kB, beltWarn ? Col(kEmber, sa * (0.55f + 0.45f * pulse)) : (beltOn ? Col(kInk2, sa * litA) : gh), false);
        PixelDraw(dl, gxs[2], top(bE, kE), bE, kE, engOff ? Col(kInk2, sa * litA) : gh, false);
        PixelDraw(dl, gxs[3], top(bK, kK), bK, kK, lock == 1 ? Col(kInk2, sa * litA) : gh, false);
        if (lockFlash > 0.0f)
            PixelDraw(dl, gxs[3], top(bK, kK), bK, kK, Col(kGold, sa * 0.95f * lockFlash), false);

        L.stripVisible = true;
        L.stripX = px; L.stripY = ay; L.stripW = pw; L.stripH = (float)gn;
        L.stripLampX = gxL; L.fuelBarX = fuelBarX0;
        L.stripIndLeftLit = indL && blinkOn; L.stripIndRightLit = indR && blinkOn;
        L.stripLights = lights; L.stripBeltOn = beltOn; L.stripBeltWarn = beltWarn; L.stripEngineOff = engOff; L.stripLock = lock;
    }

    L.speedoVisible = true;
    L.speedoX = px; L.speedoY = py; L.speedoW = pw; L.speedoH = ph;
    L.speedShown = shown; L.speedLit = (int)(t + 0.5f); L.speedSegments = kBars;
    L.fuelShown = fuelPct; L.fuelLow = fuelLow;
}

// 0.4.22 minimap: the vitals docked under GTA's radar, a slim strip instead of the plate (DrawVitals calls this when
// ClusterLayout docked them). The strip's rect: the radar's left edge and width, kStripFrac of its height; health in the left half,
// food in the right half, each a glyph (the bar's height + 2 px) and a thin bar, no numbers (they do not fit on a bar's baseline at
// this height). One opaque underlay, one glyph row and one bar row for both halves, every edge on a whole pixel, the same colour
// logic as the plate (ember + pulse when low): a low vital turns its glyph and fill ember and tints its EMPTY track ember (0.30), all pulsing together
// on the HUD clock's alarm beat. The speedometer's rail still carries the block alarm; the strip has no rail of its own.
void DrawVitalsStrip(ImDrawList* dl, const Cluster& c, float a, UiState& L)
{
    const float sx = c.vx, sy = c.vTop, sw = c.vw, sh = c.vH;       // whole pixels: ClusterLayout floors them
    // Opaque, so no map shows through where the strip tucks into the radar's edge, and the bars read on one flat colour. The bottom-right
    // corner is cut like the speedometer plate's (the family's one diagonal), deep enough to read as a cut (0.55 of the strip's height, 7 px at
    // 1080p); the right half's track is held back from it (padR) so its corner clears the diagonal by 2 px.
    // 0.4.28: the band is the bottom 20 % of the radar element (ClusterLayout), ~40 px at 1440p: the glyph fills the middle 56 % of it and
    // the bar is half the glyph's height, centred on it, so a tall band reads as a frame with two bars in it, not as two slabs.
    const float cut = 0.0f;   // 0.4.28: no corner cut: inside the radar element a cut shows a triangle of map (capture hud-r5)
    const ImVec2 hull[5] = {{sx, sy}, {sx + sw, sy}, {sx + sw, sy + sh - cut}, {sx + sw - cut, sy + sh}, {sx, sy + sh}};
    dl->AddConvexPolyFilled(hull, 5, Col(kPanel, a));
    dl->AddLine(ImVec2(sx, sy + 0.5f), ImVec2(sx + sw, sy + 0.5f), Col(kGold, 0.55f * a), 1.0f);   // the seam between map and band
    const float pad = std::fmax(sh >= 11.0f ? 2.0f : 1.0f, std::floor(sh * 0.22f));   // above and below the glyph
    const float gs = std::fmax(4.0f, sh - 2.0f * pad);
    const float barH = std::fmax(3.0f, std::floor(gs * 0.5f));
    const float gy = sy + pad, by = gy + std::floor((gs - barH) * 0.5f);   // the one glyph row and the one bar row of both halves
    const float padX = std::fmax(2.0f, std::floor(S(5.0f)));        // inside the strip's left edge
    const float diagIn = std::fmax(0.0f, (by + barH) - (sy + sh - cut));   // how far the diagonal has moved in at the track's bottom edge
    const float padR = std::fmax(padX, diagIn + 2.0f);              // inside the strip's right edge: clear of the diagonal
    const float midGap = std::fmax(4.0f, std::floor(S(10.0f)));     // between the two halves
    const float gGap = std::fmax(2.0f, std::floor(S(4.0f)));        // glyph to bar
    const float half = std::floor((sw - padX - padR - midGap) * 0.5f);
    const float bw = half - gs - gGap;
    const float tip = std::fmax(1.0f, std::floor(S(2.0f)));
    const float phi = HudPhase();
    const float pulse = AlarmPulse(phi);                            // one beat for both halves, the speedometer's belt and the rail
    void (*const kGlyph[2])(ImDrawList*, float, float, float, ImU32) = {IconHeartMini, IconFoodMini};

    int pct[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        const Ui::Vital& v = u.vit[i];
        const Rgb glyph = Mix(kInk2, kEmber, v.warn);
        const float fillA = 1.0f - v.warn * 0.38f * (1.0f - pulse);
        pct[i] = (int)(v.fill * 100.0f + 0.5f);
        if (pct[i] == 0 && v.fill > 0.0f)
            pct[i] = 1;                                      // never "0" while there is anything left
        pct[i] = pct[i] > 100 ? 100 : pct[i];

        const float gx = sx + padX + (float)i * (half + midGap);
        kGlyph[i](dl, gx, gy, gs, Col(glyph, a * 0.95f * fillA));   // a low vital pulses its glyph with its fill
        if (bw < 4.0f)
            continue;                                        // a display too narrow for a bar keeps its glyphs
        const float bx0 = gx + gs + gGap;
        dl->AddRectFilled(ImVec2(bx0, by), ImVec2(bx0 + bw, by + barH), Col(kInk, 0.18f * a));   // the track: lifted from the plate's 0.10 so an empty bar reads
        const float fx = std::floor(bx0 + bw * Clamp01(v.fill));
        const float tx = std::floor(bx0 + bw * Clamp01(v.trail));
        if (v.warn > 0.003f)                                 // a low vital tints its EMPTY track ember and pulses it with the glyph and the fill
            dl->AddRectFilled(ImVec2(fx, by), ImVec2(bx0 + bw, by + barH), Col(kEmber, 0.30f * a * v.warn * (0.35f + 0.65f * pulse)));
        if (tx > fx + 1.0f)
            dl->AddRectFilled(ImVec2(fx, by), ImVec2(tx, by + barH), Col(kInk, 0.30f * a));   // the drain trail after a loss
        if (fx - bx0 >= 1.0f) {
            dl->AddRectFilled(ImVec2(bx0, by), ImVec2(fx, by + barH), Col(Mix(kGold, kEmber, v.warn), 0.95f * a * fillA));
            if (fx - bx0 >= tip + 1.0f)
                dl->AddRectFilled(ImVec2(fx - tip, by), ImVec2(fx, by + barH), Col(kInk, 0.90f * a * fillA));
        }
    }

    L.vitalsVisible = true;
    L.vitalsX = sx; L.vitalsY = sy; L.vitalsW = sw; L.vitalsH = sh;
    L.vitalsDocked = true; L.vitalsDockWhy = c.dockWhy; L.vitalsCoverPx = c.coverPx;
    L.radarX = c.radarX; L.radarY = c.radarY; L.radarW = c.radarW; L.radarH = c.radarH;
    L.vitalsRows = 2;
    L.healthShown = pct[0]; L.satietyShown = pct[1];
    L.healthLow = u.vit[0].low; L.satietyLow = u.vit[1].low;
}

// The vitals plate: two fixed rows, HP and FOOD, each a glyph, a bar and the value. The glyph is the
// glance target (qb-hud / ps-hud read), not a word. One accent for every bar: colour means something
// only when it is wrong, so a bar under its threshold turns the glyph, bar and number ember together and
// the fill pulses, while the plate's rail (ClusterPlate) alarms for the block. The fill's end is capped in
// ink; fills ease (TickVitals) and after a loss a pale trail drains behind the fill. Fuel is not here —
// it is a gauge on the speedometer plate.
void DrawVitals(ImDrawList* dl, float W, float H, float a, UiState& L)
{
    const Cluster c = ClusterLayout(W, H);
    // Once per change, where the vitals went and why (the engine's 10 s minimap line repeats it). Render thread only.
    {
        static int s_state = -1000;
        const int state = c.dockWhy < 0 ? -1000 : (c.docked ? (c.coverPx > 0.5f ? 1 : 0) : 100 + c.dockWhy);
        if (state != s_state) {
            s_state = state;
            if (c.dockWhy >= 0) {
                if (c.docked)
                    GLOGI("overlay: vitals strip %s: x %.0f-%.0f y %.0f-%.0f (radar x %.0f-%.0f y %.0f-%.0f, %.0f px tucked into the map = %.1f %% of its height); "
                          "strip top y %.0f, radar rect bottom y %.0f (gap %.0f px; if GTA's radar anchor includes its ~18 px bars band the drawn map ends that much above the rect's bottom); "
                          "GTA's own bars sit inside the radar, not under it (minimap_bars hides them)",
                          c.coverPx > 0.5f ? "TUCKED into the radar's bottom edge (the margin under it is too small)"
                                           : "FLUSH UNDER the radar's bottom edge (inside the safe-zone margin, no map covered)",
                          (double)c.vx, (double)(c.vx + c.vw), (double)c.dockY, (double)(c.dockY + c.vH), (double)c.radarX, (double)(c.radarX + c.radarW),
                          (double)c.radarY, (double)(c.radarY + c.radarH), (double)c.coverPx,
                          c.radarH > 0.0f ? (double)(100.0f * c.coverPx / c.radarH) : 0.0,
                          (double)c.dockY, (double)(c.radarY + c.radarH), (double)(c.dockY - (c.radarY + c.radarH)));
                else
                    GLOGI("overlay: vitals plate stays BOTTOM-RIGHT (radar dock refused: %s; radar x %.0f-%.0f y %.0f-%.0f)",
                          gage::sync::DockWhyName((gage::sync::DockWhy)c.dockWhy), (double)c.radarX, (double)(c.radarX + c.radarW), (double)c.radarY,
                          (double)(c.radarY + c.radarH));
            }
        }
    }
    if (c.docked) {
        DrawVitalsStrip(dl, c, a, L);
        return;
    }
    const float px = c.vx, pw = c.vw, py = c.vTop, ph = c.vH;
    ClusterPlate(dl, px, py, pw, ph, a);
    dl->PushClipRect(ImVec2(px, py), ImVec2(px + pw, py + ph), true);

    const float gs = S(16.0f);                               // glyph box
    const float valW = Measure(u.numMid, "000").x;           // the value slot (no "%")
    const float colGap = S(10.0f);
    const float barH = S(12.0f), lean = barH * kSkew;        // the leaning bar reserves `lean` at its right
    const float bx0 = px + c.padX + gs + colGap;
    const float bw = std::floor((px + pw - c.padX) - (valW + colGap) - bx0 - lean);
    const float tip = std::fmax(2.0f, S(2.0f));
    const float pulse = AlarmPulse(HudPhase());                      // the HUD clock's alarm beat (also the strip's, the belt's and the rail's)
    void (*const kGlyph[2])(ImDrawList*, float, float, float, ImU32) = {IconHeart, IconFood};

    int pct[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        const Ui::Vital& v = u.vit[i];
        const float ry = std::floor(py + c.padT + (float)i * (c.rowH + c.rowGap));
        const Rgb ink = Mix(kInk, kEmber, v.warn), glyph = Mix(kInk2, kEmber, v.warn);
        const float fillA = 1.0f - v.warn * 0.38f * (1.0f - pulse);

        pct[i] = (int)(v.fill * 100.0f + 0.5f);
        if (pct[i] == 0 && v.fill > 0.0f)
            pct[i] = 1;                                      // never "0" while there is anything left
        pct[i] = pct[i] > 100 ? 100 : pct[i];
        char val[8];
        std::snprintf(val, sizeof(val), "%d", pct[i]);

        // The glyph and the value share the row's vertical centre; the value is the brand's numerals.
        kGlyph[i](dl, px + c.padX, ry + std::floor((c.rowH - gs) * 0.5f), gs, Col(glyph, a * 0.95f));
        const float vy = ry + std::floor((c.rowH - u.numMid.px) * 0.5f);
        Text(dl, u.numMid, ImVec2(px + pw - c.padX - Measure(u.numMid, val).x, vy), ink, a, val, 0.0f, 0.5f);

        if (bw < 4.0f)
            continue;                                        // a display too narrow for a bar keeps its numbers
        const float by = ry + std::floor((c.rowH - barH) * 0.5f);
        PlateSlant(dl, bx0, by, bx0 + bw, by + barH, Col(kInk, 0.10f * a), 0);
        const float fx = std::floor(bx0 + bw * Clamp01(v.fill));
        const float tx = std::floor(bx0 + bw * Clamp01(v.trail));
        if (tx > fx + 1.0f)
            PlateSlant(dl, fx, by, tx, by + barH, Col(kInk, 0.30f * a), 0);   // the drain trail after a loss
        if (fx - bx0 >= 1.0f) {
            PlateSlant(dl, bx0, by, fx, by + barH, Col(Mix(kGold, kEmber, v.warn), 0.95f * a * fillA), 0);
            if (fx - bx0 >= tip + 1.0f)
                PlateSlant(dl, fx - tip, by, fx, by + barH, Col(kInk, 0.90f * a * fillA), 0);
        }
    }
    dl->PopClipRect();

    L.vitalsVisible = true;
    L.vitalsX = px; L.vitalsY = py; L.vitalsW = pw; L.vitalsH = ph;
    L.vitalsDocked = c.docked; L.vitalsDockWhy = c.dockWhy; L.vitalsCoverPx = c.coverPx;
    L.radarX = c.radarX; L.radarY = c.radarY; L.radarW = c.radarW; L.radarH = c.radarH;
    L.vitalsRows = 2;
    L.healthShown = pct[0]; L.satietyShown = pct[1];
    L.healthLow = u.vit[0].low; L.satietyLow = u.vit[1].low;
}

// ---- 0.4.27 the vehicle state row (libs/sync/vehicle_state_row.h): lock, belt, engine ---------------------------------------------------------
// (the padlock bitmaps sit above DrawSpeedo: 0.4.29 the speedometer strip draws the lock too)
// The belt and the engine of the speedometer strip, each with a 7 px class in front: the row is the vitals strip's height (11 px at its smallest:
// a 7 px glyph box), which the strip's 9 / 10 px classes do not fit.
constexpr const char* const kBelt7[7] = {"..XX...", "..XX...", ".......", ".X.XX..", ".XX.X..", ".XXX.XX", ".....XX"};
constexpr const char* const kEngine7[7] = {"...XXX....", "....X.....", ".XXXXXXX..", "XXXXXXXXXX", "XXX....XXX", "XXXXXXXXXX", ".XXXXXXX.."};
constexpr Bmp kBeltRowCls[4] = {{7, 7, kBelt7}, {9, 10, kBelt10}, {12, 14, kBelt14}, {15, 18, kBelt18}};
constexpr Bmp kEngineRowCls[4] = {{10, 7, kEngine7}, {13, 9, kEngine9}, {18, 13, kEngine13}, {23, 17, kEngine17}};
static_assert(ClassesOk(kBeltRowCls, 4) && ClassesOk(kEngineRowCls, 4),
              "state row bitmaps: a row is not w wide / a class is not h rows / an unknown character");
constexpr PixelSet kBeltRowSet = {kBeltRowCls, 4, 2, 14, 28};
constexpr PixelSet kEngineRowSet = {kEngineRowCls, 4, 2, 14, 28};

// The row's rect and cells come from libs/sync/vehicle_state_row.h (PlaceStateRow / LayoutStateRow) against the vitals strip's rect as ClusterLayout
// computes it (never its own sizing): the strip's left edge, width and height; under it when the margin allows, else above the radar. Opaque plate with
// the strip's cut corner. Per cell: the glyph (ghost when the state is not known or not live), then a toggle: gold track with the dark knob at its right =
// on, ghost track with an ink knob at its left = off; the knob slides (eased) when the state flips, and the cell flashes gold for kRowPulseMs. An
// unbuckled belt at speed is the strip's alarm: ember glyph and an ember-tinted track on the alarm beat.
void DrawStateRow(ImDrawList* dl, float W, float H, float a, unsigned hud, double nowMs, UiState& L)
{
    const Cluster c = ClusterLayout(W, H);
    gage::sync::RowAnchor an;
    an.screenH = H;
    an.docked = c.docked;
    an.edge = std::fmax(2.0f, std::floor(S(2.0f)));
    // 0.4.29 (GI): the row belongs to the car, so it always sits on the bottom-right cluster, on top of the speedometer plate, never on the
    // radar block (the radar + HP / satiety element stays one piece on the left)
    an.docked = false;
    an.x = c.x;
    an.w = c.w;
    an.rowH = std::fmax(kStripMinH, std::floor(S(14.0f)));
    an.plateTop = L.speedoVisible ? L.speedoY : (L.vitalsVisible && !c.docked ? L.vitalsY : H - c.gut);
    an.gap = c.gap;
    const gage::sync::RowRect r = gage::sync::PlaceStateRow(an);
    if (r.where == gage::sync::RowWhere::None)
        return;
    const float padX = std::fmax(2.0f, std::floor(S(5.0f)));
    const float sep = std::fmax(4.0f, std::floor(S(10.0f)));
    const float gGap = std::fmax(2.0f, std::floor(S(4.0f)));
    const gage::sync::RowLayout lay = gage::sync::LayoutStateRow(r.x, r.y, r.w, r.h, padX, sep, gGap);
    if (!lay.ok)
        return;

    const bool lockKnown = (hud & 256u) != 0, locked = (hud & 512u) != 0, flash = (hud & 1024u) != 0;
    const bool beltOn = (hud & 16u) != 0, beltWarn = (hud & 32u) != 0, engOn = (hud & 64u) == 0;
    const ImVec2 hull[5] = {{r.x, r.y}, {r.x + r.w, r.y}, {r.x + r.w, r.y + r.h - lay.cut}, {r.x + r.w - lay.cut, r.y + r.h}, {r.x, r.y + r.h}};
    dl->AddConvexPolyFilled(hull, 5, Col(kPanel, a));

    const int gn = (int)std::floor(lay.gs);
    int k[3] = {1, 1, 1};
    const Bmp* bmp[3] = {PixelPick((lockKnown && !locked) ? kLockOSet : kLockCSet, gn, &k[0]), PixelPick(kBeltRowSet, gn, &k[1]),
                         PixelPick(kEngineRowSet, gn, &k[2])};
    // The toggles only when every cell has room for its glyph's real width, the gap and the track (a wide engine glyph in a 7 px box is 10 px).
    float glyphW = 0.0f;
    for (int i = 0; i < 3; ++i)
        glyphW = std::fmax(glyphW, bmp[i] ? (float)(bmp[i]->w * k[i]) : lay.gs);
    const float cellW = lay.cell[0].x1 - lay.cell[0].x0;
    const bool toggles = lay.toggles && glyphW + gGap + (lay.cell[0].tx1 - lay.cell[0].tx0) <= cellW;
    const float alarm = AlarmPulse(HudPhase());

    const bool live[3] = {lockKnown, !flash, !flash};
    const bool on[3] = {locked, beltOn, engOn};
    for (int i = 0; i < 3; ++i) {
        const gage::sync::RowCellBox& cb = lay.cell[i];
        const float p = u.rowTrack.Pulse(i, nowMs);
        if (p > 0.003f)                                             // the change flash: the cell lights gold and fades
            dl->AddRectFilled(ImVec2(cb.x0 - 2.0f, r.y), ImVec2(cb.x1 + 2.0f, r.y + r.h), Col(kGold, 0.30f * p * a));
        const bool warn = i == 1 && live[i] && beltWarn;
        ImU32 gcol;
        if (!live[i])
            gcol = Col(kInk, 0.12f * a);                             // the idle ghost of every HUD glyph
        else if (warn)
            gcol = Col(kEmber, a * (0.55f + 0.45f * alarm));
        else
            gcol = Col(Mix(kInk2, kGold, p), a * (on[i] ? 0.95f : 0.45f + 0.50f * p));
        if (bmp[i])
            PixelDraw(dl, cb.gx, lay.gy + (float)((gn - bmp[i]->h * k[i]) / 2), *bmp[i], k[i], gcol, false);
        else
            dl->AddRectFilled(ImVec2(cb.gx, lay.gy), ImVec2(cb.gx + lay.gs, lay.gy + lay.gs), gcol);
        if (!toggles)
            continue;
        const float tx0 = cb.tx0, ty0 = cb.ty0, tx1 = cb.tx1, ty1 = cb.ty1;
        if (!live[i]) {
            dl->AddRectFilled(ImVec2(tx0, ty0), ImVec2(tx1, ty1), Col(kInk, 0.10f * a));   // not known: an empty track, no knob
            continue;
        }
        const float e = u.rowOn[i] * u.rowOn[i] * (3.0f - 2.0f * u.rowOn[i]);
        dl->AddRectFilled(ImVec2(tx0, ty0), ImVec2(tx1, ty1), Col(kInk, 0.18f * a));       // the track (the vitals track's lift)
        if (e > 0.003f)
            dl->AddRectFilled(ImVec2(tx0, ty0), ImVec2(tx1, ty1), Col(kGold, 0.95f * a * e));
        if (warn)
            dl->AddRectFilled(ImVec2(tx0, ty0), ImVec2(tx1, ty1), Col(kEmber, 0.30f * a * (0.35f + 0.65f * alarm)));
        const float kx = std::floor(cb.kOff + (cb.kOn - cb.kOff) * e);
        dl->AddRectFilled(ImVec2(kx, ty0 + 1.0f), ImVec2(kx + lay.knob, ty0 + 1.0f + lay.knob), Col(Mix(kInk2, kPanel, e), a * 0.95f));
    }

    L.rowVisible = true;
    L.rowWhere = (int)r.where;
    L.rowX = r.x; L.rowY = r.y; L.rowW = r.w; L.rowH = r.h;
    L.rowLock = lockKnown ? (locked ? 1 : 0) : -1;
    L.rowBelt = beltOn; L.rowEngine = engOn; L.rowToggles = toggles;
    for (int i = 0; i < 3; ++i)
        L.rowPulse[i] = u.rowTrack.Pulse(i, nowMs);

    // Once per change of place, where the row went (the strip's own line names the strip and the radar).
    static int s_where = -1;
    if ((int)r.where != s_where) {
        s_where = (int)r.where;
        GLOGI("overlay: vehicle state row %s: x %.0f-%.0f y %.0f-%.0f (%s); lock / belt / engine, toggles %s",
              gage::sync::RowWhereName(r.where), (double)r.x, (double)(r.x + r.w), (double)r.y, (double)(r.y + r.h),
              "on the speedometer: the bottom-right cluster's edge and width", toggles ? "drawn" : "left out (cells too narrow)");
    }
}

// Chat, top-left: the plate's top edge is fixed 100 px (1080p design) from the top of the screen and the
// left edge is the common 28 px gutter; the log reads top to bottom, oldest first, and the open plate
// grows downward with the input line at its foot. Lines never jump: a new line adds to the bottom, and in
// the closed view a line that is fading out rolls up as it fades, so the lines below glide up.
void DrawChat(ImDrawList* bg, float /*W*/, float /*H*/, ULONGLONG now, float hudA, UiState& L)
{
    ImGuiIO& io = ImGui::GetIO();
    const bool open = u.chatOpen;
    const float x0 = S(28.0f), cw = S(588.0f), padX = S(16.0f), padY = S(14.0f);
    const float inH = S(44.0f), inGap = S(12.0f);
    const float plateTop = S(100.0f);
    const float logTop = plateTop + padY;          // the first line starts here
    const float logH = S(310.0f);
    const float logX = x0 + padX, logW = cw - 2.0f * padX;
    const float openA = u.chatA * hudA;
    const float dt = std::fmin(std::fmax(io.DeltaTime, 0.0f), 0.1f);

    // One name column for the whole log, so the message bodies line up (a longer name is clipped).
    float gutter = 0.0f;
    {
        const int from = (int)u.view.size() - 1 - (open ? u.scroll : 0);
        for (int i = from, seen = 0; i >= 0 && seen < 40; --i, ++seen) {
            const ViewLine& m = u.view[(std::size_t)i];
            if (!m.system)
                gutter = std::fmax(gutter, Measure(u.semi, m.name.c_str()).x);
        }
        gutter = std::fmin(gutter, S(150.0f));
    }
    const float indent = gutter > 0.0f ? gutter + S(10.0f) : 0.0f;

    // Layout, newest first, bottom up, in coordinates relative to the newest line's bottom edge (0):
    // y is a slot's top, h its text height, vis the height it currently takes (less than h only for a
    // line that is rolling up while it fades out).
    struct Slot { int i; float y, h, vis, a; };
    Slot slots[kViewMax];
    int ns = 0;
    {
        float y = 0.0f;
        const int start = (int)u.view.size() - 1 - (open ? u.scroll : 0);
        const float budget = open ? logH : S(260.0f);
        for (int i = start; i >= 0 && ns < (int)kViewMax; --i) {
            const ViewLine& m = u.view[(std::size_t)i];
            float a = 1.0f;
            if (!open) {
                const float age = (float)(now - m.at) / 1000.0f;
                if (age > kHoldS)
                    a = 1.0f - (age - kHoldS) / kFadeS;
                if (a <= 0.0f)
                    break;                  // older lines are older still
            }
            const float wrapW = std::fmax(S(80.0f), logW - (m.system ? 0.0f : indent));
            const ImVec2 ts = Measure(u.body, m.text.c_str(), wrapW);
            const float h = std::fmax(ts.y, m.system ? 0.0f : u.semi.px);
            const float vis = h * a;        // a is 1 while open and for every line not yet fading
            y -= vis;
            if (!open && -y > budget)
                break;                      // no half lines in the closed view
            slots[ns++] = {i, y, h, vis, a};
            y -= S(6.0f) * a;
            if (open && -y > budget + S(40.0f))
                break;
        }
    }
    // Height of the block of lines, first line's top to the newest line's bottom.
    const float contentH = ns > 0 ? -slots[ns - 1].y : 0.0f;

    // The open plate is as tall as its lines (at least four lines' worth, at most logH), with the input
    // line under them, and grows downward smoothly as lines arrive; it snaps to size when it opens.
    // Lines are placed against the plate's top (the newest line's bottom, blockBottom, sits at the
    // block's height below logTop; a block taller than logH shows its newest lines, the rest is clipped).
    const float logAreaH = std::fmin(logH, std::fmax(S(100.0f), contentH + S(4.0f)));
    const float wantH = padY + logAreaH + inGap + inH + padY;
    if (!open)
        u.chatPlateH = 0.0f;
    else if (u.chatPlateH <= 0.0f)
        u.chatPlateH = wantH;
    else
        u.chatPlateH = Approach(u.chatPlateH, wantH, S(1200.0f) * dt);
    const float plateBottom = plateTop + u.chatPlateH;
    const float inY1 = plateBottom - padY, inY0 = inY1 - inH;
    const float logBottom = inY0 - inGap;          // the log's clip edge (follows the animated plate)
    const float blockBottom = logTop + (open ? std::fmin(contentH, logAreaH) : contentH);

    // Wheel scrolls the history while the box is open and the pointer is over it.
    if (open && io.MouseWheel != 0.0f && io.MousePos.x >= x0 && io.MousePos.x <= x0 + cw &&
        io.MousePos.y >= plateTop && io.MousePos.y <= plateBottom) {
        const int maxScroll = std::max(0, (int)u.view.size() - 1);
        u.scroll = std::min(maxScroll, std::max(0, u.scroll + (io.MouseWheel > 0.0f ? 2 : -2)));
    }

    if (open) {
        PlateChamfer(bg, x0, plateTop, x0 + cw, plateBottom, S(24.0f), Col(kPanel, 0.90f * openA), Col(kHair, 0.12f * openA));
        // History position: a slim leaning thumb on the right edge while there is more than is shown.
        const int total = (int)u.view.size();
        if (total > ns && ns > 0) {
            const int newest = total - 1 - u.scroll;              // index of the newest line shown
            const float tTop = plateTop + S(26.0f), tBot = logBottom - S(2.0f), tH = tBot - tTop;
            const float f0 = (float)(newest + 1 - ns) / (float)total, f1 = (float)(newest + 1) / (float)total;
            const float ty0 = tTop + tH * f0, ty1 = std::fmax(ty0 + S(14.0f), tTop + tH * f1);
            // The track is a plain vertical hairline: a leaning one would shift its top by 14 % of its
            // whole length and poke out of the plate. The thumb leans by a fixed 3 px, whatever its length.
            const float tw = S(4.0f), tx = x0 + cw - S(16.0f), tl = S(3.0f);
            const float trackW = std::fmax(u.px1, S(2.0f)), trackX = tx + tl * 0.5f + std::floor((tw - trackW) * 0.5f);
            bg->AddRectFilled(ImVec2(trackX, tTop), ImVec2(trackX + trackW, tBot), Col(kInk, 0.16f * openA));
            const float tb = std::fmin(ty1, tBot);
            const ImVec2 thumb[4] = {{tx + tl, ty0}, {tx + tl + tw, ty0}, {tx + tw, tb}, {tx, tb}};
            bg->AddConvexPolyFilled(thumb, 4, Col(kInk, 0.75f * openA));
        }
        // Solar gold at rest, as on the palette: a short rail along the top edge.
        bg->AddRectFilled(ImVec2(x0 + S(24.0f), plateTop), ImVec2(x0 + S(24.0f) + S(72.0f), plateTop + std::fmax(2.0f, S(3.0f))),
                          Col(kGold, openA));
        bg->PushClipRect(ImVec2(logX, plateTop + S(6.0f)), ImVec2(logX + logW, logBottom + S(2.0f)), true);
    } else if (ns > 0) {
        // Closed: a chamfered plate like every other surface, but fitted to the lines it shows so it is
        // never a half-empty slab, and opaque enough (0.82) to carry text on a bright sky. Under the
        // quick-actions palette it steps back, so the two plates do not read as accidentally stacked.
        const float a0 = slots[0].a * hudA * (1.0f - 0.8f * u.invA);
        float maxW = 0.0f;
        for (int k = 0; k < ns; ++k) {
            const ViewLine& m = u.view[(std::size_t)slots[k].i];
            const float ind = m.system ? 0.0f : indent;
            maxW = std::fmax(maxW, ind + Measure(u.body, m.text.c_str(), std::fmax(S(80.0f), logW - ind)).x);
        }
        const float top = plateTop, bot = logTop + contentH + padY;
        const float fitW = std::fmin(cw, std::fmax(S(300.0f), 2.0f * padX + maxW));
        PlateChamfer(bg, x0, top, x0 + fitW, bot, S(24.0f), Col(kPanel, 0.90f * a0), Col(kHair, 0.10f * a0));
        bg->AddRectFilled(ImVec2(x0 + S(24.0f), top), ImVec2(x0 + S(24.0f) + S(72.0f), top + std::fmax(2.0f, S(3.0f))),
                          Col(kGold, 0.9f * a0));
    }

    for (int k = ns - 1; k >= 0; --k) {
        const Slot& s = slots[k];
        const ViewLine& m = u.view[(std::size_t)s.i];
        const float a = s.a * hudA * (open ? 1.0f : 1.0f - 0.8f * u.invA);
        const float sy = blockBottom + s.y;
        // A line rolling up as it fades is cut to the height it still takes.
        const bool rolling = s.vis < s.h - 0.5f;
        const float sh = 0.7f;                     // a light 1px shadow; the plate carries the contrast
        if (rolling)
            bg->PushClipRect(ImVec2(logX - S(2.0f), sy), ImVec2(logX + logW + S(2.0f), sy + s.vis), true);
        if (m.system) {
            Text(bg, u.body, ImVec2(logX, sy), kInk, a * 0.82f, m.text.c_str(), logW, sh);
        } else {
            bg->PushClipRect(ImVec2(logX, sy - S(2.0f)), ImVec2(logX + gutter, sy + s.h + S(2.0f)), true);
            Text(bg, u.semi, ImVec2(logX, sy), m.self ? kInk : kInk2, a, m.name.c_str(), 0.0f, sh);
            bg->PopClipRect();
            Text(bg, u.body, ImVec2(logX + indent, sy), kInk, a * 0.95f, m.text.c_str(),
                 std::fmax(S(80.0f), logW - indent), sh);
        }
        if (rolling)
            bg->PopClipRect();
    }
    if (open)
        bg->PopClipRect();

    L.chatOpen = open;
    L.chatLinesShown = ns;
    L.chatX = x0; L.chatY = plateTop; L.chatW = cw;
    L.chatH = open ? plateBottom - plateTop : (ns > 0 ? contentH + 2.0f * padY : 0.0f);
    if (!open)
        return;

    // The input line: a leaning plate with the ImGui text box laid over it.
    const float inX = logX, inW = logW - inH * kSkew;
    const ImU32 plate = Col(kGround, 0.80f * openA), edge = Col(kGold, 0.85f * openA);
    PlateSlant(bg, inX, inY0, inX + inW, inY1, Col(kInk, 0.07f * openA), 0);
    PlateSlant(bg, inX, inY0, inX + inW, inY1, plate, edge);

    ImGui::SetNextWindowPos(ImVec2(inX, inY0));
    ImGui::SetNextWindowSize(ImVec2(inW, inH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, std::floor((inH - u.body.px) * 0.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, Clamp01(openA));
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    bool submitted = false, active = false;
    if (ImGui::Begin("##gage_chat_input", nullptr, wf)) {
        ImGui::PushFont(u.body.font);
        ImGui::SetCursorPos(ImVec2(S(20.0f) + inH * kSkew * 0.5f, 0.0f));
        ImGui::SetNextItemWidth(inW - S(40.0f) - inH * kSkew * 0.5f);
        if (u.focusFrames > 0) {
            ImGui::SetKeyboardFocusHere(0);
            --u.focusFrames;
        }
        const ImGuiInputTextFlags tf = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter |
                                       ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_NoUndoRedo;
        submitted = ImGui::InputTextWithHint("##msg", "Message, or a /command   (Enter sends, Esc cancels)", u.buf,
                                             sizeof(u.buf), tf, ChatCharFilter);
        active = ImGui::IsItemActive();
        u.inputActive = active;
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar(4);

    L.inputX = inX; L.inputY = inY0; L.inputW = inW + inH * kSkew; L.inputH = inH;

    const bool settled = u.frame > u.chatOpenFrame + 8 && now - u.chatOpenedAt > 160;
    if (submitted) {
        if (u.frame <= u.chatOpenFrame + 1) {
            // The Enter that opened the box must not submit it.
            u.buf[0] = 0;
            u.focusFrames = 3;
        } else {
            Submit(u.buf);
            CloseChat();
        }
    } else if (settled && !active) {
        CloseChat();                         // Esc, or a click away from the box
    }
}

// t cut from the end and closed with "..." so it is at most maxW wide; "" when not even the dots fit.
// (ASCII dots: the baked ranges are not guaranteed to hold an ellipsis glyph. Inputs are ASCII.)
std::string FitText(const Fnt& f, const char* t, float maxW)
{
    if (maxW <= 0.0f)
        return std::string();
    if (Measure(f, t).x <= maxW)
        return std::string(t);
    std::string s(t);
    while (!s.empty() && Measure(f, (s + "...").c_str()).x > maxW)
        s.pop_back();
    return s.empty() ? std::string() : s + "...";
}

// A key hint: a small leaning key cap with its label. Returns the cap's right edge. minW (default 0) widens the cap to a
// shared width (the label is centred in it), so a cluster of caps reads as one even group.
float KeyCap(ImDrawList* dl, float x, float y, const char* label, float a, float minW = 0.0f)
{
    const float h = u.small.px + S(8.0f);
    const float tw = Measure(u.small, label).x;
    const float w = (tw + S(16.0f) > minW) ? tw + S(16.0f) : minW;
    const float lean = PlateSlant(dl, x, y, x + w, y + h, Col(kInk, 0.10f * a), Col(kInk, 0.36f * a));
    Text(dl, u.small, ImVec2(x + (w - tw) * 0.5f + lean * 0.5f, y + S(4.0f)), kInk, a * 0.94f, label);
    return x + w + lean;
}

// ---- the Tab menu's quick actions page --------------------------------------------------------
// The three columns of kActions under the header, the line of typed commands, and the key hints. The y values are
// offsets from the panel's top (DrawInventory's layout).
// The noclip row's key hints, under the row: the keys that fly (key caps like the footer's, labels in the footer's dim ink).
// Three lines; they light up while noclip is on. Returns the height they take below `y`.
constexpr int kNoclipHintLines = 3;
// The block ends level with the bottom of the third row of the Player column (the noclip row + the hints = three rows and two
// gaps), so the two columns finish on one line: the gap between the lines follows from that. It never drops below S(3).
struct NoclipHintsGeo { float top, lineGap, height; };
NoclipHintsGeo NoclipHintsGeometry(float rowH, float rowGap)
{
    const float capH = u.small.px + S(8.0f);
    NoclipHintsGeo g;
    g.top = S(12.0f);
    const float want = 2.0f * (rowH + rowGap);
    const float lines = capH * (float)kNoclipHintLines;
    g.lineGap = (want - g.top - lines) * 0.5f;
    if (g.lineGap < S(3.0f))
        g.lineGap = S(3.0f);
    g.height = g.top + lines + g.lineGap * (float)(kNoclipHintLines - 1);
    return g;
}
float NoclipHintsHeight(float rowH, float rowGap) { return NoclipHintsGeometry(rowH, rowGap).height; }
void DrawNoclipHints(ImDrawList* dl, float x0, float y, float rowH, float rowGap, float a)
{
    const NoclipHintsGeo geo = NoclipHintsGeometry(rowH, rowGap);
    const float capH = u.small.px + S(8.0f);
    const float g = S(8.0f), gg = S(18.0f), gk = S(4.0f);
    auto label = [&](float x, float cy, const char* t) {
        Text(dl, u.small, ImVec2(x, cy + S(4.0f)), kInk, a * 0.80f, t);
        return x + Measure(u.small, t).x;
    };
    // Shared cap widths: the four letters are one width, Space / Shift one, Ctrl / Alt one; the second pair of lines starts at one x.
    const float wLetter = Measure(u.small, "W").x + S(16.0f);
    const float wLong = (Measure(u.small, "Space").x > Measure(u.small, "Shift").x ? Measure(u.small, "Space").x : Measure(u.small, "Shift").x) + S(16.0f);
    const float wMod = (Measure(u.small, "Ctrl").x > Measure(u.small, "Alt").x ? Measure(u.small, "Ctrl").x : Measure(u.small, "Alt").x) + S(16.0f);
    const float wUp = Measure(u.small, "up").x, wFast = Measure(u.small, "fast").x;
    float cy = y + geo.top;
    float x = x0;
    for (const char* k : {"W", "A", "S", "D"})
        x = KeyCap(dl, x, cy, k, a, wLetter) + gk;
    label(x - gk + g, cy, "fly");
    cy += capH + geo.lineGap;
    const float xs = KeyCap(dl, x0, cy, "Space", a, wLong) + g;
    const float x2 = xs + (wUp > wFast ? wUp : wFast) + gg;
    label(xs, cy, "up");
    x = KeyCap(dl, x2, cy, "Ctrl", a, wMod) + g;
    label(x, cy, "down");
    cy += capH + geo.lineGap;
    KeyCap(dl, x0, cy, "Shift", a, wLong);
    label(xs, cy, "fast");
    x = KeyCap(dl, x2, cy, "Alt", a, wMod) + g;
    label(x, cy, "slow");
}

void DrawQuickActionsPage(ImDrawList* dl, float px, float py, float pw, float pad, float rowH, float rowGap, float headY0, float rowsY0, float helpY0, float footSepY0, float footH,
                          const char* const* help, int helpN, float a, UiState& L)
{
    // Typed commands, one line under the columns.
    {
        const float hy = py + helpY0;
        const char* lead = "Or type in chat";
        Text(dl, u.small, ImVec2(px + pad, hy), kInk2, a * 0.82f, lead);
        float hx = px + pad + Measure(u.small, lead).x + S(24.0f);
        for (int h = 0; h < helpN; ++h) {
            Text(dl, u.small, ImVec2(hx, hy), kInk, a * 0.90f, help[h]);
            hx += Measure(u.small, help[h]).x + S(26.0f);
        }
    }

    // Footer: how to drive it without the mouse, as key caps.
    const float footY = py + footSepY0;
    dl->AddLine(ImVec2(px + pad, footY), ImVec2(px + pw - pad, footY), Col(kHair, 0.14f * a), u.px1);
    {
        const float capH = u.small.px + S(8.0f);
        const float capY = footY + std::floor((footH - capH) * 0.5f);
        const float g = S(8.0f), gg = S(22.0f);
        float x = px + pad;
        x = KeyCap(dl, x, capY, "Arrows", a) + g;
        Text(dl, u.small, ImVec2(x, capY + S(4.0f)), kInk, a * 0.80f, "move");
        x += Measure(u.small, "move").x + gg;
        x = KeyCap(dl, x, capY, "Enter", a) + g;
        Text(dl, u.small, ImVec2(x, capY + S(4.0f)), kInk, a * 0.80f, "run");
        // Right group, laid out from its measured width.
        const float lean = capH * kSkew;
        const float wTab = Measure(u.small, "Tab").x + S(16.0f) + lean;
        const float wEsc = Measure(u.small, "Esc").x + S(16.0f) + lean;
        const float wOr = Measure(u.small, "or").x, wClose = Measure(u.small, "close").x;
        float rx = px + pw - pad - (wTab + g + wOr + g + wEsc + g + wClose);
        rx = KeyCap(dl, rx, capY, "Tab", a) + g;
        Text(dl, u.small, ImVec2(rx, capY + S(4.0f)), kInk, a * 0.80f, "or");
        rx = KeyCap(dl, rx + wOr + g, capY, "Esc", a) + g;
        Text(dl, u.small, ImVec2(rx, capY + S(4.0f)), kInk, a * 0.80f, "close");
    }

    // Columns: a column with no row to show (the noclip switch off empties the middle one) leaves the others wider, so the
    // panel never carries an empty gap.
    const float gapC = S(26.0f);
    bool colUsed[3] = {false, false, false};
    for (int i = 0; i < kActionCount; ++i)
        if (ActionShown(kActions[i]))
            colUsed[kActions[i].column] = true;
    int slot[3] = {-1, -1, -1}, nCols = 0;
    for (int c = 0; c < 3; ++c)
        if (colUsed[c])
            slot[c] = nCols++;
    const float colW = std::floor((pw - 2.0f * pad - (float)(nCols > 1 ? nCols - 1 : 0) * gapC) / (float)(nCols > 0 ? nCols : 1));
    const float headY = py + headY0;
    const float rowsY = py + rowsY0;
    int rowIndex[3] = {0, 0, 0};
    for (int c = 0; c < 3; ++c)
        if (slot[c] >= 0)
            Text(dl, u.small, ImVec2(px + pad + (colW + gapC) * (float)slot[c], headY), kInk2, a * 0.82f, kColumnTitle[c]);

    L.actionCount = 0;
    L.noclipRow = -1;
    L.noclipOn = g_noclipOn.load();
    int clicked = -1;
    for (int i = 0; i < kActionCount; ++i) {
        const Action& act = kActions[i];
        if (!ActionShown(act))
            continue;
        const bool isNoclip = act.kind == kActionNoclip;
        const bool noclipOn = isNoclip && g_noclipOn.load();
        const float cx = px + pad + (colW + gapC) * (float)slot[act.column];
        const float cy = rowsY + (rowH + rowGap) * (float)rowIndex[act.column]++;
        const float lean = rowH * kSkew;
        ImGui::SetCursorScreenPos(ImVec2(cx, cy));
        ImGui::PushID(i);
        const bool pressed = ImGui::InvisibleButton("##act", ImVec2(colW, rowH));
        if (u.invJustOpened && i == 0)
            ImGui::SetItemDefaultFocus();
        // The keyboard focus ring shows only while the keyboard is the one steering (NavVisible);
        // otherwise the default-focused first row would stay lit next to the hovered one.
        const bool hot = ImGui::IsItemHovered() || (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible);
        const bool down = ImGui::IsItemActive();
        ImGui::PopID();
        if (pressed)
            clicked = i;

        const float x1 = cx + colW - lean;
        ImU32 fill = Col(kInk, 0.09f * a), edge = 0;
        if (down)      { fill = Col(kGoldDown, 0.62f * a); edge = Col(kGoldHot, 0.95f * a); }
        else if (hot)  { fill = Col(kGold, 0.30f * a); edge = Col(kGold, 0.95f * a); }
        else if (noclipOn) { fill = Col(kGold, 0.16f * a); edge = Col(kGold, 0.45f * a); }   // a toggle that is on: gold, quieter than a hot row
        PlateSlant(dl, cx, cy, x1, cy + rowH, fill, edge);
        if (hot || noclipOn)
            PlateSlant(dl, cx, cy, cx + S(4.0f), cy + rowH, Col(kGold, a), 0);   // the lit row's tick
        Text(dl, u.semi, ImVec2(cx + S(18.0f) + lean * 0.5f, cy + std::floor((rowH - u.semi.px) * 0.5f)), kInk,
             a, act.label);
        // The command, right-aligned; when the row is too narrow for label and command it is cut
        // with an ellipsis rather than dropped (a missing label looks like a bug). The noclip toggle shows its state
        // instead ("On" in gold, "Off" in the command's dim ink); its command is on the "Or type in chat" line.
        const ImVec2 ls = Measure(u.semi, act.label);
        const float cmdR = x1 - S(14.0f) + lean * 0.5f;
        const float cmdAvail = cmdR - (cx + S(18.0f) + lean * 0.5f + ls.x + S(14.0f));
        const char* rightText = isNoclip ? (noclipOn ? "On" : "Off") : act.cmd;
        const std::string cmdText = FitText(u.small, rightText, cmdAvail);
        if (!cmdText.empty())
            Text(dl, u.small, ImVec2(cmdR - Measure(u.small, cmdText.c_str()).x, cy + std::floor((rowH - u.small.px) * 0.5f)),
                 noclipOn ? kGold : kInk, noclipOn ? a : a * (hot ? 0.90f : 0.66f), cmdText.c_str());
        if (isNoclip) {
            L.noclipRow = L.actionCount;
            DrawNoclipHints(dl, cx, cy + rowH, rowH, rowGap, a * (noclipOn ? 1.0f : 0.75f));
        }

        if (L.actionCount < UiState::kMaxActions) {
            const int k = L.actionCount++;
            L.actionRect[k][0] = cx; L.actionRect[k][1] = cy; L.actionRect[k][2] = colW; L.actionRect[k][3] = rowH;
            std::snprintf(L.actionCmd[k], sizeof(L.actionCmd[k]), "%s", act.cmd);
        }
    }

    if (clicked >= 0) {
        Submit(kActions[clicked].cmd);
        CloseInventory();
    }
}

// ---- the Tab menu's cars page -----------------------------------------------------------------
constexpr int kCarCols = 2, kCarRows = 8, kCarsPerPage = kCarCols * kCarRows;
static_assert(kCarsPerPage <= UiState::kMaxActions, "UiState::kMaxActions is too small for a page of cars");

// 1234567 -> "1,234,567"
std::string Thousands(int n)
{
    std::string s = std::to_string(n < 0 ? 0 : n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3)
        s.insert((std::size_t)i, ",");
    return s;
}

// A small leaning button (the pager), key-cap sized but dressed like the rows (ink 0.09 at rest, no edge; gold on
// hover / press), so what is clickable never looks like a key legend. A disabled one is dimmed and never pressed.
// Returns pressed; *right gets its right edge, rect its box.
bool CapButton(ImDrawList* dl, const char* id, float x, float y, const char* label, bool enabled, float a, float* right,
               float rect[4])
{
    const float h = u.small.px + S(8.0f);
    const float w = Measure(u.small, label).x + S(16.0f);
    const float lean = h * kSkew;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w + lean, h)) && enabled;
    const bool hot = enabled && (ImGui::IsItemHovered() || (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible));
    const bool down = enabled && ImGui::IsItemActive();
    const float k = enabled ? 1.0f : 0.38f;
    ImU32 fill = Col(kInk, (enabled ? 0.09f : 0.04f) * a), edge = 0;
    if (down)     { fill = Col(kGoldDown, 0.62f * a); edge = Col(kGoldHot, 0.95f * a); }
    else if (hot) { fill = Col(kGold, 0.30f * a); edge = Col(kGold, 0.95f * a); }
    PlateSlant(dl, x, y, x + w, y + h, fill, edge);
    Text(dl, u.small, ImVec2(x + S(8.0f) + lean * 0.5f, y + S(4.0f)), kInk, a * 0.94f * k, label);
    rect[0] = x; rect[1] = y; rect[2] = w + lean; rect[3] = h;
    *right = x + w + lean;
    return pressed;
}

// The search glyph: a lens and its handle, from primitives (like the vitals icons), in a box of side s.
void IconSearch(ImDrawList* dl, float x, float y, float s, ImU32 col)
{
    const float r = s * 0.34f, t = std::fmax(1.5f, s * 0.13f);
    const ImVec2 c(x + s * 0.42f, y + s * 0.42f);
    dl->AddCircle(c, r, col, 24, t);
    const float d = r * 0.7071f;
    dl->AddLine(ImVec2(c.x + d + t * 0.3f, c.y + d + t * 0.3f), ImVec2(x + s * 0.94f, y + s * 0.94f), col, t * 1.25f);
}

// Copy SetCars' list when it changed, and rebuild the search hits when the list or the query changed.
void RefreshCars()
{
    std::vector<CarEntry> fresh;
    bool loading = false, got = false;
    {
        DLock lk;
        const Data& d = D();
        if (d.carsSeq != u.carsSeq) {
            fresh = d.cars;
            loading = d.carsLoading;
            u.carsSeq = d.carsSeq;
            got = true;
        }
    }
    if (got) {
        u.carsLoading = loading;
        u.cars.clear();
        u.cars.reserve(fresh.size());
        for (const CarEntry& e : fresh) {
            Ui::CarRow r;
            r.model = e.model; r.name = e.name[0] ? e.name : e.model; r.vclass = e.vclass; r.pack = e.pack;
            r.ready = e.ready;
            r.cmd = "/car " + r.model;
            r.hay = r.name + " " + r.model + " " + r.vclass + " " + r.pack;
            for (char& c : r.hay)
                c = (char)std::tolower((unsigned char)c);
            u.cars.push_back(std::move(r));
        }
        u.carHitsStale = true;
    }
    if (u.carHitsStale || u.carHitsQuery != u.carQuery) {
        if (u.carHitsQuery != u.carQuery)
            u.carPage = 0;                     // a new search starts on its first page
        u.carHitsQuery = u.carQuery;
        u.carHitsStale = false;
        u.carHits.clear();
        for (int i = 0; i < (int)u.cars.size(); ++i)
            if (content::MatchesAllTerms(u.carQuery, u.cars[(std::size_t)i].hay))
                u.carHits.push_back(i);
    }
}

// The cars page under the header: the search line, a page of cars in two columns, the pager, the key hints.
// Its y values are offsets from the panel's top (CarsPageLayout), so DrawInventory can size the panel first.
struct CarsLayout {
    float searchY0, searchH, rowsY0, rowsH, pagerY0, pagerH, footSepY0, footH, ph;
};
CarsLayout CarsPageLayout(float sepY0, float rowH, float rowGap, float footH)
{
    CarsLayout c;
    c.searchY0 = sepY0 + S(22.0f);
    c.searchH = S(46.0f);
    c.rowsY0 = c.searchY0 + c.searchH + S(22.0f);
    c.rowsH = (rowH + rowGap) * (float)kCarRows - rowGap;
    c.pagerY0 = c.rowsY0 + c.rowsH + S(20.0f);
    c.pagerH = u.small.px + S(8.0f);
    c.footSepY0 = c.pagerY0 + c.pagerH + S(20.0f);
    c.footH = footH;
    c.ph = c.footSepY0 + footH;
    return c;
}

void DrawCarsPage(ImDrawList* dl, float px, float py, float pw, float pad, float rowH, float rowGap, const CarsLayout& cl,
                  float a, UiState& L)
{
    RefreshCars();
    const int total = (int)u.cars.size(), matched = (int)u.carHits.size();
    const int pages = std::max(1, (matched + kCarsPerPage - 1) / kCarsPerPage);
    // PgUp / PgDn and the wheel over the list turn the page (read before the clamp so a press at the end is a no-op).
    // The page owns the two keys: keyboard navigation would otherwise also take them (NavUpdatePageUpPageDown reads
    // only unowned keys) and move the focus off the search onto a pager button, so a following Enter pressed that.
    const ImGuiID pagerOwner = ImGui::GetID("##carspager");
    ImGui::SetKeyOwner(ImGuiKey_PageDown, pagerOwner);
    ImGui::SetKeyOwner(ImGuiKey_PageUp, pagerOwner);
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown))
        ++u.carPage;
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp))
        --u.carPage;
    const float listX0 = px + pad, listX1 = px + pw - pad;
    const float rowsY = py + cl.rowsY0;
    // A precision touchpad sends a small wheel delta every frame: sum them and turn one page per whole notch.
    if (ImGui::IsMouseHoveringRect(ImVec2(listX0, rowsY), ImVec2(listX1, rowsY + cl.rowsH), false)) {
        u.wheelAcc += ImGui::GetIO().MouseWheel;
        while (u.wheelAcc <= -1.0f) { ++u.carPage; u.wheelAcc += 1.0f; }
        while (u.wheelAcc >= 1.0f) { --u.carPage; u.wheelAcc -= 1.0f; }
    } else {
        u.wheelAcc = 0.0f;
    }
    u.carPage = std::max(0, std::min(pages - 1, u.carPage));

    // ---- the search line: a leaning well like the chat's input line, the ImGui text box laid over it ----
    const float sy = py + cl.searchY0, sh = cl.searchH;
    const float sx = listX0, sw = (listX1 - listX0) - sh * kSkew;
    // The edge is gold while the box has the keyboard (as of last frame: the plate is drawn before the box), and at
    // rest while there is nothing to search.
    PlateSlant(dl, sx, sy, sx + sw, sy + sh, Col(kInk, 0.07f * a), 0);
    PlateSlant(dl, sx, sy, sx + sw, sy + sh, Col(kGround, 0.80f * a),
               u.searchActive && (total > 0 || u.carsLoading) ? Col(kGold, 0.85f * a) : Col(kHair, 0.20f * a));
    const float lean = sh * kSkew;
    const float icon = std::floor(u.body.px * 0.95f);
    IconSearch(dl, sx + S(18.0f) + lean * 0.5f, sy + std::floor((sh - icon) * 0.5f), icon, Col(kInk2, 0.80f * a));
    // The count, right-aligned in the well: "1,024 cars", or "12 of 1,024" while searching; nothing while there are
    // no cars at all (the empty state below says why).
    std::string count;
    if (total > 0)
        count = (u.carQuery[0] ? Thousands(matched) + " of " + Thousands(total) : Thousands(total) + (total == 1 ? " car" : " cars")) +
                (u.carsLoading ? " so far" : "");
    const float countW = count.empty() ? 0.0f : Measure(u.small, count.c_str()).x;
    // Its right edge is the rows' right-hand text edge (a row's x1 - S(14) + lean / 2), so count, ids and pager align.
    const float countX = listX1 - S(14.0f) - rowH * kSkew * 0.5f - countW;
    if (!count.empty())
        Text(dl, u.small, ImVec2(countX, sy + std::floor((sh - u.small.px) * 0.5f)), kInk2, a * 0.82f, count.c_str());

    const float textX = sx + S(18.0f) + lean * 0.5f + icon + S(12.0f);
    ImGui::SetCursorScreenPos(ImVec2(textX, sy));
    ImGui::SetNextItemWidth(std::fmax(S(80.0f), countX - S(16.0f) - textX));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, std::floor((sh - u.body.px) * 0.5f)));
    ImGui::PushFont(u.body.font);
    // With no packs at all (and none loading) there is nothing to search: the box is not handed the keyboard.
    const bool searchable = total > 0 || u.carsLoading;
    if (!searchable && u.searchActive)
        ImGui::ClearActiveID();                  // the packs went away while it was typing: give the keyboard back
    if (u.searchFocusFrames > 0) {
        if (!u.searchActive && searchable)
            ImGui::SetKeyboardFocusHere(0);
        --u.searchFocusFrames;
    }
    // Select-all only for the activation that opening the page asked for (a focus request otherwise keeps the caret,
    // and a click puts it where the click was).
    const ImGuiInputTextFlags tf = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter |
                                   ImGuiInputTextFlags_NoUndoRedo |
                                   (u.searchSelectAll ? ImGuiInputTextFlags_AutoSelectAll : ImGuiInputTextFlags_None);
    const bool enterInSearch = ImGui::InputTextWithHint("##carsearch", "Search by name, model, class or pack", u.carQuery,
                                                        sizeof(u.carQuery), tf, ChatCharFilter);
    u.searchActive = ImGui::IsItemActive();
    if (u.searchActive)
        u.searchSelectAll = false;
    ImGui::PopFont();
    ImGui::PopStyleVar();
    L.searchX = sx; L.searchY = sy; L.searchW = sw + lean; L.searchH = sh;
    RefreshCars();                               // the query may have changed this frame

    // ---- the page of cars, column by column (an alphabetical list reads down, then across) ----
    const float gapC = S(26.0f);
    const float colW = std::floor((listX1 - listX0 - gapC * (float)(kCarCols - 1)) / (float)kCarCols);
    const int first = u.carPage * kCarsPerPage;
    const int onPage = std::max(0, std::min(kCarsPerPage, (int)u.carHits.size() - first));
    // Always exactly one lit car (the row the keys act on and the status line describes): the row under the pointer
    // or the arrow keys, else the first car on THIS page the game has. Enter in the search spawns it. The buttons are
    // laid out first so this frame's hover decides the lit row before any row is drawn.
    int clicked = -1;
    bool hotK[kCarsPerPage] = {}, downK[kCarsPerPage] = {};
    int hotRow = -1;
    for (int k = 0; k < onPage; ++k) {
        const Ui::CarRow& car = u.cars[(std::size_t)u.carHits[(std::size_t)(first + k)]];
        const float cx = listX0 + (colW + gapC) * (float)(k / kCarRows);
        const float cy = rowsY + (rowH + rowGap) * (float)(k % kCarRows);
        ImGui::SetCursorScreenPos(ImVec2(cx, cy));
        ImGui::PushID(first + k);
        const bool pressed = ImGui::InvisibleButton("##car", ImVec2(colW, rowH));
        hotK[k] = ImGui::IsItemHovered() || (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible);
        downK[k] = car.ready && ImGui::IsItemActive();
        ImGui::PopID();
        if (hotK[k] && hotRow < 0)
            hotRow = k;
        if (pressed && car.ready)
            clicked = u.carHits[(std::size_t)(first + k)];
    }
    int litK = hotRow;
    for (int k = 0; k < onPage && litK < 0; ++k)
        if (u.cars[(std::size_t)u.carHits[(std::size_t)(first + k)]].ready)
            litK = k;
    if (litK < 0 && onPage > 0)
        litK = 0;                                // nothing on the page is loaded: still describe the first row
    L.actionCount = 0;
    for (int k = 0; k < onPage; ++k) {
        const Ui::CarRow& car = u.cars[(std::size_t)u.carHits[(std::size_t)(first + k)]];
        const int col = k / kCarRows, row = k % kCarRows;
        const float cx = listX0 + (colW + gapC) * (float)col;
        const float cy = rowsY + (rowH + rowGap) * (float)row;
        const float rl = rowH * kSkew;
        const bool hot = hotK[k], down = downK[k];

        const float x1 = cx + colW - rl;
        const bool lit = hot && car.ready;       // a car the game does not have never lights up as an action
        const bool resting = k == litK && !hot && car.ready;   // the default lit car: tick and hairline, no gold fill
        ImU32 fill = Col(kInk, 0.09f * a), edge = 0;
        if (down)         { fill = Col(kGoldDown, 0.62f * a); edge = Col(kGoldHot, 0.95f * a); }
        else if (lit)     { fill = Col(kGold, 0.30f * a); edge = Col(kGold, 0.95f * a); }
        else if (hot || resting) { edge = Col(kHair, 0.22f * a); }
        PlateSlant(dl, cx, cy, x1, cy + rowH, fill, edge);
        if (lit || down || resting)
            PlateSlant(dl, cx, cy, cx + S(4.0f), cy + rowH, Col(kGold, a), 0);
        // Right: the model (what /car takes), or why the car cannot spawn. The name gets the room first; the
        // right-hand text is cut before the name is.
        const char* right = car.ready ? car.model.c_str() : (u.carsLoading ? "loading" : "not loaded");
        const float nameX = cx + S(18.0f) + rl * 0.5f;
        const float rightR = x1 - S(14.0f) + rl * 0.5f;
        const float inner = rightR - nameX;
        const float rightWant = std::fmin(Measure(u.small, right).x, inner * 0.34f);
        const std::string name = FitText(u.semi, car.name.c_str(), inner - rightWant - S(14.0f));
        Text(dl, u.semi, ImVec2(nameX, cy + std::floor((rowH - u.semi.px) * 0.5f)), kInk, a * (car.ready ? 1.0f : 0.56f),
             name.c_str());
        const float rightAvail = inner - Measure(u.semi, name.c_str()).x - S(14.0f);
        const std::string rt = FitText(u.small, right, rightAvail);
        if (!rt.empty()) {
            const Rgb rc = car.ready || u.carsLoading ? kInk : kEmber;
            const float ra = car.ready ? (hot ? 0.90f : 0.66f) : (u.carsLoading ? 0.50f : 0.85f);
            Text(dl, u.small, ImVec2(rightR - Measure(u.small, rt.c_str()).x, cy + std::floor((rowH - u.small.px) * 0.5f)),
                 rc, a * ra, rt.c_str());
        }
        if (L.actionCount < UiState::kMaxActions) {
            const int n = L.actionCount++;
            L.actionRect[n][0] = cx; L.actionRect[n][1] = cy; L.actionRect[n][2] = colW; L.actionRect[n][3] = rowH;
            std::snprintf(L.actionCmd[n], sizeof(L.actionCmd[n]), "%s", car.cmd.c_str());
        }
    }

    // ---- nothing to list: loading, no packs, or a search with no hits. Built from the rows' own plate, anchored where
    // the list starts (heading at the row-text x), so the page never shows a bare void. ----
    if (onPage == 0) {
        std::string head, sub;
        const bool loadingNow = u.carsLoading;
        if (total == 0 && loadingNow) {
            head = "Loading car packs";
            sub = "Cars appear here as each pack mounts.";
        } else if (total == 0) {
            head = "No car packs mounted";
            sub = "Add-on cars appear here once their packs load.";
        } else {
            std::string q = FitText(u.semi, u.carQuery, (listX1 - listX0) * 0.5f);
            head = (loadingNow ? "No match yet for \xE2\x80\x9C" : "No cars match \xE2\x80\x9C") + q + "\xE2\x80\x9D";
            sub = loadingNow ? "More car packs are still loading." : "Search by name, model, class or pack, for example";
        }
        const float bh = rowH * 2.0f + rowGap;
        const float x1 = listX1 - bh * kSkew;    // a taller plate leans further: its top-right corner ends at listX1
        PlateSlant(dl, listX0, rowsY, x1, rowsY + bh, Col(kInk, 0.09f * a), 0);
        const float tx = listX0 + S(18.0f) + rowH * kSkew * 0.5f;   // the rows' text x, not this taller plate's
        const float hy = rowsY + S(22.0f);
        Text(dl, u.semi, ImVec2(tx, hy), kInk, a, FitText(u.semi, head.c_str(), x1 - tx - S(14.0f)).c_str());
        const float sy2 = hy + u.semi.px + S(10.0f);
        const std::string subFit = FitText(u.small, sub.c_str(), x1 - tx - S(14.0f));
        Text(dl, u.small, ImVec2(tx, sy2), kInk2, a * 0.82f, subFit.c_str());
        if (total > 0 && !loadingNow && subFit == sub) {
            // The example searches in command ink, like the quick actions page's "Or type in chat" line.
            float ex = tx + Measure(u.small, sub.c_str()).x + S(8.0f);
            for (const char* w : {"super", "lambo"}) {
                if (ex + Measure(u.small, w).x > x1 - S(14.0f))
                    break;
                Text(dl, u.small, ImVec2(ex, sy2), kInk, a * 0.90f, w);
                ex += Measure(u.small, w).x + S(14.0f);
            }
        }
        if (loadingNow) {
            // Indeterminate progress: a short lit segment sweeping along the plate's foot, the loader's lit-bar idiom.
            const float y1 = rowsY + bh, y0 = y1 - std::fmax(2.0f, S(3.0f));
            const float span = x1 - listX0, seg = std::floor(span * 0.18f);
            const float t = (float)std::fmod(ImGui::GetTime() / 1.6, 1.0);
            const float sx0 = listX0 + (span + seg) * t - seg;
            const float a0 = std::fmax(listX0, sx0), a1 = std::fmin(x1, sx0 + seg);
            if (a1 > a0)
                dl->AddRectFilled(ImVec2(a0, y0), ImVec2(a1, y1), Col(kGold, a));
        }
    }

    // ---- the status line: the lit car (the command first, then its class and pack) on the left, the pager right ----
    const float pgY = py + cl.pagerY0, capH = cl.pagerH;
    if (matched > 0) {                           // no pager over an empty state
        float rx = 0.0f;
        const float lean2 = capH * kSkew, g = S(10.0f);
        const float wPrev = Measure(u.small, "Previous").x + S(16.0f) + lean2;
        const float wNext = Measure(u.small, "Next").x + S(16.0f) + lean2;
        char pageText[48];
        std::snprintf(pageText, sizeof(pageText), "Page %s of %s", Thousands(u.carPage + 1).c_str(), Thousands(pages).c_str());
        const float wPage = Measure(u.small, pageText).x;
        float bx = listX1 - (wPrev + g + wPage + g + wNext);
        const float pagerX0 = bx;
        if (CapButton(dl, "##prev", bx, pgY, "Previous", u.carPage > 0, a, &rx, L.pagePrevRect))
            --u.carPage;
        Text(dl, u.small, ImVec2(rx + g, pgY + S(4.0f)), kInk, a * 0.80f, pageText);
        if (CapButton(dl, "##next", rx + g + wPage + g, pgY, "Next", u.carPage + 1 < pages, a, &rx, L.pageNextRect))
            ++u.carPage;
        u.carPage = std::max(0, std::min(pages - 1, u.carPage));

        const float infoEnd = pagerX0 - S(20.0f);
        float ix = listX0;
        const float ty = pgY + S(4.0f);
        if (litK >= 0) {
            const Ui::CarRow& car = u.cars[(std::size_t)u.carHits[(std::size_t)(first + litK)]];
            // "/car rs6   Super · rs6". A car the game does not have says so instead of advertising a command that
            // would not run.
            const std::string cmd = FitText(u.small, car.ready ? car.cmd.c_str() : (u.carsLoading ? "loading" : "not loaded"),
                                            infoEnd - ix);
            Text(dl, u.small, ImVec2(ix, ty), car.ready || u.carsLoading ? kInk : kEmber,
                 a * (car.ready ? 0.92f : (u.carsLoading ? 0.50f : 0.85f)), cmd.c_str());
            ix += Measure(u.small, cmd.c_str()).x + S(22.0f);
            std::string where = car.vclass;
            if (!car.pack.empty())
                where += (where.empty() ? "" : "  \xC2\xB7  ") + car.pack;
            if (!where.empty() && ix < infoEnd)
                Text(dl, u.small, ImVec2(ix, ty), kInk2, a * 0.72f, FitText(u.small, where.c_str(), infoEnd - ix).c_str());
        }
    }

    // ---- footer: the keys ----
    const float footY = py + cl.footSepY0;
    dl->AddLine(ImVec2(listX0, footY), ImVec2(listX1, footY), Col(kHair, 0.14f * a), u.px1);
    {
        const float fcapH = u.small.px + S(8.0f);
        const float capY = footY + std::floor((cl.footH - fcapH) * 0.5f);
        const float g = S(8.0f), gg = S(22.0f);
        // A key with nothing to do right now (an empty page, a single page) is dimmed like a disabled pager button.
        const float aRows = onPage > 0 ? a : a * 0.38f, aPage = pages > 1 ? a : a * 0.38f;
        float x = listX0;                        // the quick actions page's order: Arrows, then Enter
        x = KeyCap(dl, x, capY, "Arrows", aRows) + g;
        Text(dl, u.small, ImVec2(x, capY + S(4.0f)), kInk, aRows * 0.80f, "move");
        x += Measure(u.small, "move").x + gg;
        x = KeyCap(dl, x, capY, "Enter", aRows) + g;
        Text(dl, u.small, ImVec2(x, capY + S(4.0f)), kInk, aRows * 0.80f, "spawn");
        x += Measure(u.small, "spawn").x + gg;
        x = KeyCap(dl, x, capY, "PgUp", aPage) + g;
        x = KeyCap(dl, x, capY, "PgDn", aPage) + g;
        Text(dl, u.small, ImVec2(x, capY + S(4.0f)), kInk, aPage * 0.80f, "page");
        const float lean3 = fcapH * kSkew;
        const float wTab = Measure(u.small, "Tab").x + S(16.0f) + lean3;
        const float wEsc = Measure(u.small, "Esc").x + S(16.0f) + lean3;
        const float wOr = Measure(u.small, "or").x, wClose = Measure(u.small, "close").x;
        float rx = listX1 - (wTab + g + wOr + g + wEsc + g + wClose);
        rx = KeyCap(dl, rx, capY, "Tab", a) + g;
        Text(dl, u.small, ImVec2(rx, capY + S(4.0f)), kInk, a * 0.80f, "or");
        rx = KeyCap(dl, rx + wOr + g, capY, "Esc", a) + g;
        Text(dl, u.small, ImVec2(rx, capY + S(4.0f)), kInk, a * 0.80f, "close");
    }

    // Enter in the search spawns the lit car (when the game has it); a click spawns its row.
    if (enterInSearch && clicked < 0 && litK >= 0 && u.cars[(std::size_t)u.carHits[(std::size_t)(first + litK)]].ready)
        clicked = u.carHits[(std::size_t)(first + litK)];
    L.litCar = litK;
    L.carsTotal = total; L.carsMatched = matched; L.carPage = u.carPage; L.carPages = pages; L.carsLoading = u.carsLoading;
    if (clicked >= 0) {
        Submit(u.cars[(std::size_t)clicked].cmd.c_str());
        CloseInventory();
    }
}

void DrawInventory(float W, float H, float hudA, UiState& L)
{
    RefreshCars();                                            // the cars tab shows its count on either page
    const float a = u.invA * hudA;
    const float pad = S(30.0f);
    const float rowH = S(42.0f), rowGap = S(8.0f);
    const float pw = std::fmin(S(880.0f), W - S(48.0f));     // never wider than the screen

    // The panel is as tall as its page's content. Quick actions: the tallest column, then one line of typed
    // commands, then the footer with the key hints. Cars: CarsPageLayout.
    int rowCount[3] = {0, 0, 0};
    float hintsH[3] = {0.0f, 0.0f, 0.0f};              // the noclip row's key hints sit under its row
    for (int i = 0; i < kActionCount; ++i) {
        if (!ActionShown(kActions[i]))
            continue;
        ++rowCount[kActions[i].column];
        if (kActions[i].kind == kActionNoclip)
            hintsH[kActions[i].column] = NoclipHintsHeight(rowH, rowGap);
    }
    static const char* const kHelp[] = {"/car <model>", "/gun <weapon>", "/tp <x> <y> <z>", "/noclip"};
    const int helpN = g_noclipAvail.load() ? 4 : 3;
    float contentH = 0.0f;
    for (int c = 0; c < 3; ++c)
        if (rowCount[c] > 0)
            contentH = std::fmax(contentH, (rowH + rowGap) * (float)rowCount[c] - rowGap + hintsH[c]);
    const float sepY0 = S(66.0f), headY0 = sepY0 + S(18.0f), rowsY0 = headY0 + S(32.0f);
    const float helpY0 = rowsY0 + contentH + S(22.0f);
    const float footSepY0 = helpY0 + u.small.px + S(18.0f);
    const float footH = S(52.0f);
    const float phQuick = footSepY0 + footH;
    const CarsLayout cl = CarsPageLayout(sepY0, rowH, rowGap, footH);
    const bool carsPage = u.invTab == 1;
    const float ph = carsPage ? cl.ph : phQuick;

    // Placed for the taller page, so the header (and the tabs under the cursor) stays put when the page changes;
    // only the bottom edge moves.
    const float px = std::floor((W - pw) * 0.5f);
    const float py = std::floor((H - std::fmax(phQuick, cl.ph)) * 0.5f + (1.0f - u.invA) * S(14.0f));

    ImGui::SetNextWindowPos(ImVec2(px, py));
    ImGui::SetNextWindowSize(ImVec2(pw, ph));
    if (u.invJustOpened)
        ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    const bool began = ImGui::Begin("##gage_inventory", nullptr, wf);
    if (began) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        PlateChamfer(dl, px, py, px + pw, py + ph, S(30.0f), Col(kPanel, 0.95f * a), Col(kHair, 0.16f * a));

        // Header: the two pages as tabs, in the title face. The open one is ink and the plate's Solar gold rail (along
        // the top edge, the one straight gold bar every plate carries) sits over it, sliding when the page changes; the
        // other is a dimmer caption that lifts on hover. A click, Ctrl+Tab, or Arrows + Enter
        // switches. The cars tab carries its count while it is closed (open, the search well says it).
        const float sepY = py + sepY0;
        dl->AddLine(ImVec2(px + pad, sepY), ImVec2(px + pw - pad, sepY), Col(kHair, 0.14f * a), u.px1);
        {
            static const char* const kTabs[2] = {"Quick actions", "Cars"};
            const float ty = py + S(22.0f);
            float tx = px + pad;
            float openX = tx, openW = 0.0f;
            int switchTo = -1;
            for (int t = 0; t < 2; ++t) {
                const bool open = u.invTab == t;
                const ImVec2 ts = Measure(u.title, kTabs[t]);
                std::string badge;
                if (t == 1 && !open && !u.cars.empty())
                    badge = Thousands((int)u.cars.size());
                const float badgeW = badge.empty() ? 0.0f : S(10.0f) + Measure(u.small, badge.c_str()).x;
                const float hitY0 = py + S(12.0f), hitY1 = sepY - S(2.0f);
                ImGui::SetCursorScreenPos(ImVec2(tx - S(6.0f), hitY0));
                ImGui::PushID(100 + t);
                const bool pressed = ImGui::InvisibleButton("##tab", ImVec2(ts.x + badgeW + S(12.0f), hitY1 - hitY0));
                const bool hot = ImGui::IsItemHovered() || (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible);
                ImGui::PopID();
                if (pressed && !open)
                    switchTo = t;
                Text(dl, u.title, ImVec2(tx, ty), open ? kInk : kInk2, a * (open ? 1.0f : (hot ? 0.86f : 0.52f)), kTabs[t]);
                if (!badge.empty())
                    Text(dl, u.small, ImVec2(tx + ts.x + S(10.0f), ty + AscentPx(u.title) - AscentPx(u.small)), kInk2,
                         a * 0.52f, badge.c_str());
                if (open) {                      // a closed tab's hover is its label lifting (0.52 -> 0.86), nothing more
                    openX = tx;
                    openW = ts.x;
                }
                L.tabRect[t][0] = tx; L.tabRect[t][1] = hitY0; L.tabRect[t][2] = ts.x + badgeW; L.tabRect[t][3] = hitY1 - hitY0;
                tx += ts.x + badgeW + S(40.0f);
            }
            // The rail: eased toward the open tab (offsets from the panel, so the open-slide does not drag it). It
            // never starts inside the top-left chamfer.
            const float wantX = std::fmax(S(30.0f), openX - px), wantW = openW;
            const float dt = ImGui::GetIO().DeltaTime;
            if (u.invJustOpened || u.railW <= 0.0f) {
                u.railX = wantX;
                u.railW = wantW;
            } else {
                u.railX = Ease(u.railX, wantX, 16.0f, dt, 0.5f);
                u.railW = Ease(u.railW, wantW, 16.0f, dt, 0.5f);
            }
            dl->AddRectFilled(ImVec2(px + std::floor(u.railX), py),
                              ImVec2(px + std::floor(u.railX + u.railW), py + std::fmax(2.0f, S(3.0f))), Col(kGold, a));
            // Ctrl+Tab, at the header's right end in the footer's muted key style.
            {
                const float capH = u.small.px + S(8.0f);
                const float capY = ty + std::floor((u.title.px - capH) * 0.5f) + S(2.0f);
                const float lean = capH * kSkew, g = S(8.0f);
                const float wCap = Measure(u.small, "Ctrl+Tab").x + S(16.0f) + lean;
                const float wLbl = Measure(u.small, "switch").x;
                const float cx = px + pw - pad - (wCap + g + wLbl);
                const float r = KeyCap(dl, cx, capY, "Ctrl+Tab", a * 0.82f) + g;
                Text(dl, u.small, ImVec2(r, capY + S(4.0f)), kInk, a * 0.66f, "switch");
            }
            if (switchTo >= 0)
                SwitchInvTab(switchTo);
        }

        if (carsPage)
            DrawCarsPage(dl, px, py, pw, pad, rowH, rowGap, cl, a, L);
        else
            DrawQuickActionsPage(dl, px, py, pw, pad, rowH, rowGap, headY0, rowsY0, helpY0, footSepY0, footH, kHelp, helpN, a, L);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    u.invJustOpened = false;

    L.invOpen = true;
    L.invTab = u.invTab;
    L.invX = px; L.invY = py; L.invW = pw; L.invH = ph;
}

// The Alt+F4 confirm: a card a little above the middle that takes neither the mouse nor the game's keys (the
// world, the sender and the player keep running). The WndProc answers Enter / Alt+F4 (quit) and Esc
// (stay); the gold rail along the top runs down the time left before the card goes away by itself.
void DrawQuitPrompt(ImDrawList* dl, float W, float H, float a, float left)
{
    const float pad = S(28.0f), ch = S(22.0f);
    const float pw = std::fmin(S(560.0f), W - S(48.0f));
    const char* title = "Quit Solar RP?";
    const char* body = "The game keeps running until you choose.";
    const float titleY0 = S(24.0f), bodyY0 = titleY0 + u.title.px + S(10.0f);
    const float sepY0 = bodyY0 + u.small.px + S(22.0f), footH = S(52.0f);
    const float ph = sepY0 + footH;
    const float px = std::floor((W - pw) * 0.5f);
    const float py = std::floor(H * 0.36f - (1.0f - a) * S(10.0f)); // GI 2026-11-02: lower than the first 0.26

    PlateChamfer(dl, px, py, px + pw, py + ph, ch, Col(kPanel, 0.95f * a), Col(kHair, 0.16f * a));
    // The countdown: the rail spans the top edge after the chamfer and shrinks toward its start.
    const float railW = (pw - ch) * std::fmin(std::fmax(left, 0.0f), 1.0f);
    if (railW > 0.5f)
        dl->AddRectFilled(ImVec2(px + ch, py), ImVec2(px + ch + railW, py + std::fmax(2.0f, S(3.0f))), Col(kGold, a));

    Text(dl, u.title, ImVec2(px + pad, py + titleY0), kInk, a, title);
    Text(dl, u.small, ImVec2(px + pad, py + bodyY0), kInk2, a * 0.86f, body);

    const float sepY = py + sepY0;
    dl->AddLine(ImVec2(px + pad, sepY), ImVec2(px + pw - pad, sepY), Col(kHair, 0.14f * a), u.px1);
    const float capH = u.small.px + S(8.0f);
    const float capY = sepY + std::floor((footH - capH) * 0.5f);
    const float g = S(8.0f);
    float x = px + pad;
    x = KeyCap(dl, x, capY, "Enter", a) + g;
    Text(dl, u.small, ImVec2(x, capY + S(4.0f)), kInk, a * 0.80f, "or");
    x += Measure(u.small, "or").x + g;
    x = KeyCap(dl, x, capY, "Alt+F4", a) + g;
    Text(dl, u.small, ImVec2(x, capY + S(4.0f)), kInk, a * 0.80f, "quit");
    const float lean = capH * kSkew;
    const float wEsc = Measure(u.small, "Esc").x + S(16.0f) + lean, wStay = Measure(u.small, "stay").x;
    float rx = px + pw - pad - (wEsc + g + wStay);
    rx = KeyCap(dl, rx, capY, "Esc", a) + g;
    Text(dl, u.small, ImVec2(rx, capY + S(4.0f)), kInk, a * 0.80f, "stay");
}

// The pointer: an ink arrow with a ground-coloured outline (readable on the dark panels and on a bright
// street), sized with the display. Drawn on the foreground list so nothing covers it.
void DrawCursor(ImVec2 m)
{
    if (!(m.x > -1.0e6f && m.y > -1.0e6f))
        return;                                    // ImGui has not seen the mouse yet
    const float k = std::fmax(1.0f, u.scale * 1.25f);
    const ImVec2 q[7] = {{0.0f, 0.0f}, {0.0f, 17.0f}, {4.2f, 13.4f}, {7.2f, 20.0f},
                         {10.0f, 18.8f}, {7.0f, 12.4f}, {12.4f, 12.4f}};
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // The arrow is concave: fill it as convex pieces (left sliver, head triangles, tail). The outline
    // is the same shape in the ground colour, stamped one pixel to every side first (a stroked polyline puts a
    // miter spike, drawn as a stray dot, on the 45 degree tip).
    auto fill = [&](float ox, float oy, ImU32 c) {
        ImVec2 p[7];
        for (int i = 0; i < 7; ++i)
            p[i] = ImVec2(std::floor(m.x + ox + q[i].x * k + 0.5f), std::floor(m.y + oy + q[i].y * k + 0.5f));
        dl->AddTriangleFilled(p[0], p[1], p[2], c);
        dl->AddTriangleFilled(p[0], p[2], p[5], c);
        dl->AddTriangleFilled(p[0], p[5], p[6], c);
        dl->AddQuadFilled(p[2], p[3], p[4], p[5], c);
    };
    const float o = u.px1;
    const ImU32 outline = Col(kGround, 1.0f);
    static const float kDir[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    for (const auto& d : kDir)
        fill(d[0] * o, d[1] * o, outline);
    fill(0.0f, 0.0f, Col(kInk, 1.0f));
}

void DrawLoader(float W, float H, ULONGLONG now, const std::string& status, bool failed, UiState& L)
{
    const float a = u.loaderA;
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    // Ground: opaque while the loader is fully up, so GTA's own loading screen is hidden.
    const ImU32 top = Col(kPanelHi, a), bot = Col(kGround, a);
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H), top, top, bot, bot);

    // GTA V artwork behind the block, cover-cropped to the display and cycled: each picture holds 8 s, then
    // crossfades over 2 s into the next; the first one is picked at random per launch so a short load does not
    // always show the same picture. The ground stays under them (fully opaque while the loader is up). A dark
    // gradient from the left keeps the type readable and leaves the right of the picture visible.
    static int s_firstScene = -1;
    if (!u.loaderT0) {
        u.loaderT0 = now;
        if (s_firstScene < 0 && u.tex.sceneCount > 0)
            s_firstScene = (int)((GetTickCount64() / 7ULL) % (ULONGLONG)u.tex.sceneCount);
    }
    const int nScenes = u.tex.sceneCount;
    L.loaderSceneTextured = nScenes > 0;
    if (nScenes > 0) {
        const float aspect = 16.0f / 9.0f, dispAspect = W / H;
        float u0 = 0.0f, u1 = 1.0f, v0 = 0.0f, v1 = 1.0f;
        if (dispAspect > aspect) {            // wider than the picture: crop top and bottom
            const float k = aspect / dispAspect;
            v0 = (1.0f - k) * 0.5f; v1 = v0 + k;
        } else {                              // narrower: crop the sides, keeping the right-hand scene
            const float k = dispAspect / aspect;
            u0 = (1.0f - k) * 0.6f; u1 = u0 + k;
        }
        const ULONGLONG el = now - u.loaderT0;
        const int slot = (int)(el / 10000ULL);                                  // 10 s per picture
        const float in = (float)(el % 10000ULL) / 1000.0f;                     // 0..10 s into it
        const int cur = ((s_firstScene < 0 ? 0 : s_firstScene) + slot) % nScenes;
        float mix = in < 8.0f || nScenes < 2 ? 0.0f : (in - 8.0f) / 2.0f;     // 0: this picture, 1: the next
        mix = mix * mix * (3.0f - 2.0f * mix);
        L.loaderSceneMix = mix;
        const float dim = 0.80f;               // the picture's own brightness under the gradient
        void* first = u.tex.scenes[cur];
        void* second = nScenes > 1 ? u.tex.scenes[(cur + 1) % nScenes] : nullptr;
        dl->AddImage((ImTextureID)first, ImVec2(0, 0), ImVec2(W, H), ImVec2(u0, v0), ImVec2(u1, v1),
                     Col({dim, dim, dim}, a));
        if (second && mix > 0.003f)
            dl->AddImage((ImTextureID)second, ImVec2(0, 0), ImVec2(W, H), ImVec2(u0, v0), ImVec2(u1, v1),
                         Col({dim, dim, dim}, a * mix));
        const float mid = std::floor(W * 0.50f);
        const ImU32 sl = Col(kGround, 0.94f * a), sm = Col(kGround, 0.52f * a), sr = Col(kGround, 0.16f * a);
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(mid, H), sl, sm, sm, sl);
        dl->AddRectFilledMultiColor(ImVec2(mid, 0), ImVec2(W, H), sm, sr, sr, sm);
        // Foot of the picture: the lower fifth sinks into the ground.
        const ImU32 f0 = Col(kGround, 0.0f), f1 = Col(kGround, 0.55f * a);
        dl->AddRectFilledMultiColor(ImVec2(0, std::floor(H * 0.78f)), ImVec2(W, H), f0, f0, f1, f1);
    } else if (u.tex.mark1024) {
        // No stills: the mark, huge and nearly invisible, bleeding off the right edge. The S is 1:2, so it is
        // as tall as the old square mark was and half as wide.
        const float gh = std::floor(H * 1.28f), gw = std::floor(gh * kMarkAspect);
        const float gx = std::floor(W - gw * 0.72f), gy = std::floor((H - gh) * 0.5f);
        dl->AddImage((ImTextureID)u.tex.mark1024, ImVec2(gx, gy), ImVec2(gx + gw, gy + gh), MarkUv0(1024.0f), MarkUv1(1024.0f),
                     Col(kNoTint, 0.085f * a));
    }

    // Block: lockup, tagline, progress segments, status.
    const float lh = S(112.0f);
    const float lw = (u.tex.lockup && u.tex.lockupH > 0) ? std::floor(lh * (float)u.tex.lockupW / (float)u.tex.lockupH)
                                                         : S(480.0f);
    const float tagH = S(24.0f), segH = S(18.0f), stH = S(26.0f);
    const float gap1 = S(18.0f), gap2 = S(52.0f), gap3 = S(22.0f);
    const float blockH = lh + gap1 + tagH + gap2 + segH + gap3 + stH;
    const float bx = std::floor(W * 0.085f), by = std::floor((H - blockH) * 0.5f);

    if (u.tex.lockup) {
        dl->AddImage((ImTextureID)u.tex.lockup, ImVec2(bx, by), ImVec2(bx + lw, by + lh), ImVec2(0, 0), ImVec2(1, 1),
                     Col(kNoTint, a));
    } else {
        // No texture: the wordmark typeset in the numeral face.
        Text(dl, u.num, ImVec2(bx, by + (lh - u.num.px) * 0.5f), kInk, a, "SOLAR RP");
    }
    L.loaderLockupTextured = u.tex.lockup != nullptr;
    L.loaderLockupX = bx; L.loaderLockupY = by; L.loaderLockupW = lw; L.loaderLockupH = lh;

    Text(dl, u.mid, ImVec2(bx + S(4.0f), by + lh + gap1), kInk, a * 0.70f, "GTA V multiplayer network");

    // Progress: the speedometer's leaning segments, a lit run travelling along them. The row is as
    // wide as the lockup above it. A failed connection is not progress: the row stands still, dim.
    const float sy = by + lh + gap1 + tagH + gap2;
    const int n = 26;
    const float lean = segH * kSkew;
    // The lockup's artwork starts 4 px (design) inside its image, so the row starts there too and both
    // end on the lockup's right edge.
    const float barX = bx + S(4.0f);
    const float pitch = (lw - S(4.0f) - lean) / (float)n, segW = pitch - S(5.0f);
    const float head = std::fmod((float)(now % 2600ULL) / 2600.0f * (float)(n + 8), (float)(n + 8));
    for (int i = 0; i < n; ++i) {
        const float x = barX + pitch * (float)i;
        const ImVec2 p[4] = {{x + lean, sy}, {x + lean + segW, sy}, {x + segW, sy + segH}, {x, sy + segH}};
        const float d = head - (float)i;     // behind the head
        ImU32 c = Col(kInk, (failed ? 0.18f : 0.10f) * a);     // a stopped row must still read over the picture
        if (!failed && d >= 0.0f && d < 8.0f) {
            const float t = 1.0f - d / 8.0f;
            c = d < 1.0f ? Col(kInk, a) : Col(kGold, (0.14f + 0.86f * t) * a);
        }
        dl->AddConvexPolyFilled(p, 4, c);
    }
    L.loaderBarX = barX; L.loaderBarY = sy; L.loaderBarW = lw - S(4.0f); L.loaderBarH = segH;
    L.loaderFailed = failed;

    // Status: wrapped to the screen (a long host name must not run off it), with travelling dots
    // while it is still working, and at full ink once it has given up.
    const char* st = status.empty() ? "Loading" : status.c_str();
    const float stY = sy + segH + gap3;
    const float stWrap = std::fmax(S(200.0f), lw);      // never wider than the block above it
    const ImVec2 sm = Measure(u.mid, st, stWrap);
    const float stA = failed ? 0.97f : 0.88f;
    const Rgb stC = failed ? kEmber : kInk;
    Text(dl, u.mid, ImVec2(bx + S(4.0f), stY), stC, a * stA, st, stWrap);
    L.loaderStatusX = bx; L.loaderStatusY = stY; L.loaderStatusW = sm.x + S(40.0f);
    L.loaderStatusH = sm.y;
    const bool oneLine = sm.y < u.mid.px * 1.5f;
    if (!failed && oneLine) {
        const float dotsX = bx + S(4.0f) + sm.x + S(3.0f);
        const int dots = (int)((now / 420ULL) % 4ULL);
        char ds[4] = {0, 0, 0, 0};
        for (int i = 0; i < dots; ++i) ds[i] = '.';
        if (dots)
            Text(dl, u.mid, ImVec2(dotsX, stY), kInk, a * stA, ds);
    }
}

// ---------------------------------------------------------------------------
// Chat view refresh
// ---------------------------------------------------------------------------
void RefreshView()
{
    DLock lk;
    Data& d = D();
    if (u.viewSeq == d.chatSeq)
        return;
    const bool grew = u.viewSeq != ~0u;
    u.viewSeq = d.chatSeq;
    const std::size_t n = std::min(d.chat.size(), kViewMax);
    u.view.clear();
    u.view.reserve(n);
    for (std::size_t i = d.chat.size() - n; i < d.chat.size(); ++i) {
        const Line& l = d.chat[i];
        ViewLine v;
        v.name = l.name; v.text = l.text; v.at = l.at; v.system = l.system; v.self = l.self;
        u.view.push_back(std::move(v));
    }
    if (grew)
        u.scroll = 0;       // a new line while scrolled back jumps to the bottom
}

} // namespace

// ---------------------------------------------------------------------------
// detail:: seam (overlay.cpp)
// ---------------------------------------------------------------------------
namespace detail {

void UiInit(float scale)
{
    u = Ui();
    u.scale = scale;
    u.px1 = std::fmax(1.0f, std::floor(scale + 0.5f));
    ImGuiIO& io = ImGui::GetIO();

    static const char* const kRegular[] = {"segoeui.ttf"};
    static const char* const kSemibold[] = {"seguisb.ttf", "segoeuib.ttf", "segoeui.ttf"};
    static const char* const kBoldItalic[] = {"segoeuiz.ttf", "seguisbi.ttf", "segoeuii.ttf", "segoeuib.ttf", "segoeui.ttf"};
    u.bakeScale = scale;
    // Pixel size at this scale, never below `floorPx`: 15 px type is already small, at 0.75 it must not
    // become 11 px (small text) or 14 px (body).
    const auto load = [&](const char* const* files, int nFiles, float design, float floorPx, const ImWchar* ranges) {
        Fnt f = LoadFont(io, files, nFiles, FontPx(design, floorPx, scale), ranges);
        f.design = design;
        f.minPx = floorPx;
        return f;
    };

    u.body  = load(kRegular, 1, 19.0f, 15.0f, kFullRanges);      // first font: ImGui's default
    u.semi  = load(kSemibold, 3, 19.0f, 15.0f, kFullRanges);
    u.small = load(kSemibold, 3, 16.0f, 14.0f, kLatinRanges);
    u.mid   = load(kSemibold, 3, 23.0f, 17.0f, kLatinRanges);
    u.title = load(kSemibold, 3, 30.0f, 22.0f, kLatinRanges);
    u.num   = load(kBoldItalic, 5, 76.0f, 56.0f, kLatinRanges);
    u.numMid = load(kBoldItalic, 5, 22.0f, 17.0f, kLatinRanges);   // the vitals values, in the brand's numerals
    BuildStyle(scale);
    // Ctrl+Tab turns the Tab menu's page (UiFrame), so ImGui must not also take it for its window switcher.
    if (ImGuiContext* ctx = ImGui::GetCurrentContext()) {
        ctx->ConfigNavWindowingKeyNext = ImGuiKey_None;
        ctx->ConfigNavWindowingKeyPrev = ImGuiKey_None;
    }
}

void UiSetTextures(const UiTextures& t)
{
    u.tex = t;
    if (!kUseBrandArt) {    // belt and braces: no backend can bring the retired art back
        u.tex.mark64 = u.tex.mark256 = u.tex.mark1024 = u.tex.lockup = nullptr;
        u.tex.lockupW = u.tex.lockupH = 0;
    }
}

void UiReset()
{
    g_interactive.store(false);
    g_vehKeys.store(0);
    u = Ui();
}

bool UiInteractive() { return g_interactive.load(); }

void UiFrame()
{
    if (!u.body.font)
        return;                              // UiInit never ran: draw nothing rather than crash
    ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    if (W < 16.0f || H < 16.0f)
        return;
    RefreshScale(H);
    const float dt = std::fmin(std::fmax(io.DeltaTime, 0.0f), 0.1f);
    const ULONGLONG now = GetTickCount64();
    const bool loading = g_phase.load() == (int)Phase::Loading;
    ++u.frame;

    // Fades. The loader comes up fast and leaves slowly; the HUD arrives as the loader thins.
    if (u.first) {
        u.first = false;
        u.loaderA = loading ? 1.0f : 0.0f;
    }
    u.loaderA = Approach(u.loaderA, loading ? 1.0f : 0.0f, dt / (loading ? 0.20f : 0.70f));
    const float hudTarget = (!loading && u.loaderA < 0.85f) ? 1.0f : 0.0f;
    u.hudA = Approach(u.hudA, hudTarget, dt / (hudTarget > 0.0f ? 0.45f : 0.12f));
    const bool live = !loading && u.hudA > 0.6f;

    const bool inVeh = g_inVehicle.load();
    const float speed = g_speedKmh.load();
    // The speed on screen eases toward the pushed value (k = 12: ~95 % in a quarter second) so a jittery
    // per-tick push never flickers the digits or the gauge; it snaps while the speedometer is hidden, so
    // getting into a moving car starts at the true speed.
    if (u.speedA <= 0.003f)
        u.speedSm = speed;
    else
        u.speedSm = Ease(u.speedSm, speed, 12.0f, dt, 0.15f);
    u.speedA = Approach(u.speedA, (inVeh && !loading) ? 1.0f : 0.0f, dt / 0.18f);
    u.stripA = Approach(u.stripA, (inVeh && !loading && (g_vehHud.load() & 128u) != 0) ? 1.0f : 0.0f, dt / 0.18f);
    // 0.4.27 the vehicle state row: in a vehicle (the strip's own condition), or on foot while the lock flash lasts. Its knobs ease toward the state
    // (snapped while the row is hidden, so getting in never slides them), and the tracker stamps each flip for the change pulse.
    const unsigned rowHud = g_vehHud.load();
    const double rowNowMs = ImGui::GetTime() * 1000.0;
    {
        const bool flash = (rowHud & 1024u) != 0;
        const bool wanted = !loading && flash;   // 0.4.29: in a vehicle the speedometer strip carries lock / belt / engine; the row is only the on-foot lock flash
        const bool wasHidden = u.rowA <= 0.003f;
        u.rowA = Approach(u.rowA, wanted ? 1.0f : 0.0f, dt / 0.18f);
        gage::sync::RowState rs;
        rs.visible = wanted && live;
        rs.lockOnly = flash;
        rs.lock = (rowHud & 256u) ? ((rowHud & 512u) ? 1 : 0) : -1;
        rs.belt = (rowHud & 16u) != 0;
        rs.engine = (rowHud & 64u) == 0;
        u.rowTrack.Update(rs, rowNowMs);
        const float want[3] = {rs.lock > 0 ? 1.0f : 0.0f, rs.belt ? 1.0f : 0.0f, rs.engine ? 1.0f : 0.0f};
        for (int i = 0; i < 3; ++i)
            u.rowOn[i] = wasHidden ? want[i] : Approach(u.rowOn[i], want[i], dt / 0.12f);
    }
    TickVitals(dt, loading);
    u.chatA = Approach(u.chatA, u.chatOpen ? 1.0f : 0.0f, dt / 0.12f);
    u.invA = Approach(u.invA, u.invOpen ? 1.0f : 0.0f, dt / 0.14f);
    {
        // The Alt+F4 confirm times out here; a WndProc answer may close it at the same moment, so
        // only the opening this frame saw is cleared.
        ULONGLONG at = g_quitPromptAt.load();
        if (at && now - at >= kQuitPromptMs && g_quitPromptAt.compare_exchange_strong(at, 0))
            at = 0;
        if (at)
            u.quitLeft = 1.0f - (float)(now - at) / (float)kQuitPromptMs;
        u.quitA = Approach(u.quitA, at ? 1.0f : 0.0f, dt / 0.12f);
    }

    if (loading && (u.chatOpen || u.invOpen)) {
        CloseChat();
        CloseInventory();
    }

    // Hotkeys: ImGui key events (the WndProc subclass feeds them), no natives.
    if (live) {
        const bool tab = ImGui::IsKeyPressed(ImGuiKey_Tab, false);
        const bool esc = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
        const bool tKey = ImGui::IsKeyPressed(ImGuiKey_T, false);
        if (u.chatOpen) {
            if (esc)
                CloseChat();
        } else if (u.invOpen) {
            if (tab && io.KeyCtrl)
                SwitchInvTab(1 - u.invTab);   // Ctrl+Tab turns the page (ImGui's own Ctrl+Tab window switch is off: UiInit)
            else if (tab || esc)
                CloseInventory();
        } else if (tab) {
            OpenInventory();
        } else if (enter || tKey) {
            OpenChat(now);
        }
    }

    // 0.4.22 vehicle keys: Left / Right / Down Arrow, Y and B as edges for the engine's script tick (TakeVehicleKeys). Never while the chat,
    // the Tab menu, a text box or the quit card has the keyboard. The engine decides what a press means (in a vehicle, driver or not).
    if (live && !u.chatOpen && !u.invOpen && !io.WantTextInput && g_quitPromptAt.load() == 0) {
        unsigned k = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false))  k |= kVkLeft;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) k |= kVkRight;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false))  k |= kVkHazard;
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false))          k |= kVkEngine;
        if (ImGui::IsKeyPressed(ImGuiKey_B, false))          k |= kVkBelt;
        if (k) {
            g_vehKeysAt.store(now);
            g_vehKeys.fetch_or(k);
        }
    }

    // The text box takes focus a frame or two after the chat opens, and ImGui drops the characters
    // queued before then. Hold what the player types meanwhile and put it back in front, in order,
    // on the first frame the box is active.
    // The Tab menu's car search works the same way, and also takes whatever is typed while it does not have the keyboard
    // (after a click on the pager or a tab, or while the arrows steer the rows): the characters are held and the box
    // is focused again, caret kept (no select-all, which would eat the query on the next key).
    const bool carsPage = u.invOpen && !u.chatOpen && u.invTab == 1;
    if (carsPage && !u.searchActive) {
        bool printable = false;
        for (int i = 0; i < io.InputQueueCharacters.Size; ++i)
            printable |= io.InputQueueCharacters[i] >= 0x20 && io.InputQueueCharacters[i] != 0x7f;
        if (printable && u.searchFocusFrames == 0) {
            u.searchFocusFrames = 3;
            u.searchSelectAll = false;
        }
    }
    const bool boxWaiting = (u.chatOpen && !u.inputActive) || (carsPage && !u.searchActive && u.searchFocusFrames > 0);
    const bool boxReady = (u.chatOpen && u.inputActive) || (carsPage && u.searchActive);
    if (u.chatOpen || carsPage) {
        if (boxWaiting) {
            for (int i = 0; i < io.InputQueueCharacters.Size && u.held.size() < (std::size_t)kBufLen; ++i)
                if (io.InputQueueCharacters[i] >= 0x20 && io.InputQueueCharacters[i] != 0x7f)   // not the Tab that opened it
                    u.held.push_back(io.InputQueueCharacters[i]);
            io.InputQueueCharacters.resize(0);
        } else if (!boxReady) {
            u.held.clear();                      // the box never took focus: drop what was held for it
        } else if (!u.held.empty()) {
            ImVector<ImWchar> merged;
            for (std::size_t i = 0; i < u.held.size(); ++i)
                merged.push_back(u.held[i]);
            for (int i = 0; i < io.InputQueueCharacters.Size; ++i)
                merged.push_back(io.InputQueueCharacters[i]);
            io.InputQueueCharacters.swap(merged);
            u.held.clear();
        }
    }

    // Keyboard navigation of the quick actions (arrows + Enter) only while that panel is open.
    if (u.invOpen)
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    else
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;

    RefreshView();
    std::string status;
    bool statusFailed = false;
    if (u.loaderA > 0.003f) {
        DLock lk;
        status = D().status;
        statusFailed = D().statusFailed;
    }

    UiState L;
    L.valid = true;
    L.displayW = W; L.displayH = H; L.scale = u.scale;
    L.loaderAlpha = u.loaderA; L.hudAlpha = u.hudA;

    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    if (u.hudA > 0.003f) {
        // Bottom-right cluster (speedometer, vitals) and chat first, then the scrim over them (the
        // palette is a modal layer), then the mark on top so the brand stays at full strength.
        // The name tags belong to the world, so they go first: every plate and the scrim lie over them.
        DrawNameTags(bg, W, H, u.hudA, now, L);
        if (u.speedA > 0.003f)
            DrawSpeedo(bg, W, H, u.speedA * u.hudA, u.speedSm, L);
        if (u.vitalsA > 0.003f)
            DrawVitals(bg, W, H, u.vitalsA * u.hudA, L);
        if (u.rowA > 0.003f)
            DrawStateRow(bg, W, H, u.rowA * u.hudA, rowHud, rowNowMs, L);   // after both: it is placed against their rects
        DrawChat(bg, W, H, now, u.hudA, L);
        if (u.invA > 0.003f)
            bg->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), Col(kGround, 0.52f * u.invA * u.hudA));
        DrawLogo(bg, W, u.hudA, L);
        if (u.invOpen)
            DrawInventory(W, H, u.hudA, L);
    }
    if (u.loaderA > 0.003f)
        DrawLoader(W, H, now, status, statusFailed, L);
    if (u.quitA > 0.003f)                 // over everything, the loader included: Alt+F4 works while loading
        DrawQuitPrompt(ImGui::GetForegroundDrawList(), W, H, u.quitA, u.quitLeft);

    const bool interactive = live && (u.chatOpen || u.invOpen);
    L.interactive = interactive;
    g_interactive.store(interactive);
    io.MouseDrawCursor = false;           // ImGui's own 12 x 19 bitmap is unreadable on a 4K display: draw ours
    if (interactive)
        DrawCursor(io.MousePos);
    u.last = L;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Public push / pull API (overlay.h)
// ---------------------------------------------------------------------------
void SetPhase(Phase p) { g_phase.store((int)p); }

void SetLoaderStatus(const char* utf8, bool failed)
{
    std::string s = Clean(utf8, kStatusMax);
    DLock lk;
    if (D().status != s)
        D().status = std::move(s);
    D().statusFailed = failed;
}

void SetStats(bool inVehicle, float speedKmh)
{
    g_inVehicle.store(inVehicle);
    g_speedKmh.store(speedKmh == speedKmh && speedKmh > 0.0f ? speedKmh : 0.0f);   // NaN and negatives show 0
}

void SetRadarDock(bool dock, float safeZone, bool safeZoneOk, float scale)
{
    g_radarSafeZone.store(safeZone == safeZone ? safeZone : 1.0f);   // NaN reads as unknown (the layout also checks safeZoneOk)
    g_radarSafeZoneOk.store(safeZoneOk);
    g_radarScale.store(gage::sync::ClampRadarScale(scale));          // NaN / out of range: 1 = the game's own size, or the nearest end
    g_radarDock.store(dock);
}

void SetVitals(float healthFrac, float satietyFrac, float fuelFrac, bool show, bool showFuel)
{
    g_vitHealth.store(Frac01(healthFrac));        // NaN counts as 0, the rest is clamped to 0..1
    g_vitSatiety.store(Frac01(satietyFrac));
    g_vitFuel.store(Frac01(fuelFrac));
    g_vitShow.store(show);
    g_vitShowFuel.store(showFuel);
}

void PushChatLine(const char* name, const char* text, bool self)
{
    Line l;
    l.name = Clean(name, kNameMax);
    l.text = Clean(text, kTextMax);
    if (l.text.empty())
        return;
    l.system = l.name.empty();
    l.self = self;
    l.at = GetTickCount64();
    PushLine(std::move(l));
}

void PushSystemLine(const char* text) { PushSystem(Clean(text, kTextMax)); }

unsigned TakeVehicleKeys()
{
    const unsigned k = g_vehKeys.exchange(0);
    if (!k)
        return 0;
    const ULONGLONG at = g_vehKeysAt.load();
    return GetTickCount64() - at <= kVehKeyStaleMs ? k : 0;      // the engine's tick was not running: a stale press is not a press
}

void SetVehicleHud(const VehicleHud& h)
{
    const int lights = h.lights < 0 ? 0 : (h.lights > 2 ? 2 : h.lights);
    const unsigned w = (h.indLeft ? 1u : 0u) | (h.indRight ? 2u : 0u) | ((unsigned)lights << 2) | (h.beltOn ? 16u : 0u) |
                       (h.beltWarn ? 32u : 0u) | (h.engineOff ? 64u : 0u) | (h.show ? 128u : 0u) |
                       (h.lock >= 0 ? 256u : 0u) | (h.lock > 0 ? 512u : 0u) | (h.lockFlash ? 1024u : 0u);
    g_vehHud.store(w);
}

void SetHudPhaseForTest(float phase) { g_hudPhaseForce.store(phase == phase ? phase : -1.0f); }

void SetNameTags(const NameTag* tags, int count)
{
    std::vector<NameTag> v;
    if (tags && count > 0) {
        v.assign(tags, tags + std::min(count, kMaxNameTags));
        for (NameTag& t : v) {
            // Never trust the caller's terminators; control characters become spaces like every pushed string.
            t.idText[sizeof(t.idText) - 1] = 0;
            t.name[sizeof(t.name) - 1] = 0;
            const std::string id = Clean(t.idText, sizeof(t.idText) - 1), nm = Clean(t.name, sizeof(t.name) - 1);
            std::memset(t.idText, 0, sizeof(t.idText));
            std::memset(t.name, 0, sizeof(t.name));
            std::memcpy(t.idText, id.data(), id.size());
            std::memcpy(t.name, nm.data(), nm.size());
        }
    }
    DLock lk;
    Data& d = D();
    d.tags.swap(v);
    d.tagsAt = GetTickCount64();
}

void SetCars(const CarEntry* cars, int count, bool loading)
{
    std::vector<CarEntry> v;
    if (cars && count > 0) {
        v.assign(cars, cars + std::min(count, kMaxCars));
        for (CarEntry& e : v) {
            // Same rule as every pushed string: our own terminators, control characters become spaces.
            auto clean = [](char* s, std::size_t cap) {
                s[cap - 1] = 0;
                const std::string c = Clean(s, cap - 1);
                std::memset(s, 0, cap);
                std::memcpy(s, c.data(), c.size());
            };
            clean(e.model, sizeof(e.model));
            clean(e.name, sizeof(e.name));
            clean(e.vclass, sizeof(e.vclass));
            clean(e.pack, sizeof(e.pack));
        }
    }
    DLock lk;
    Data& d = D();
    d.cars.swap(v);
    d.carsLoading = loading;
    ++d.carsSeq;
}

void SetNoclip(bool available, bool active)
{
    g_noclipAvail.store(available);
    g_noclipOn.store(available && active);
}

bool PopOutgoing(char* out, std::size_t cap)
{
    if (!out || cap == 0)
        return false;
    out[0] = 0;
    DLock lk;
    std::deque<std::string>& q = D().out;
    if (q.empty())
        return false;
    const std::string s = std::move(q.front());
    q.pop_front();
    const std::size_t n = std::min(s.size(), cap - 1);
    std::memcpy(out, s.data(), n);
    out[n] = 0;
    return true;
}

bool Interactive() { return g_interactive.load(); }

bool OpenQuitPrompt()
{
    ULONGLONG closed = 0;
    return g_quitPromptAt.compare_exchange_strong(closed, std::max<ULONGLONG>(GetTickCount64(), 1));
}

bool QuitPromptOpen() { return g_quitPromptAt.load() != 0; }

void CloseQuitPrompt() { g_quitPromptAt.store(0); }

UiState LastUiState() { return u.last; }

} // namespace gage::game::Overlay
