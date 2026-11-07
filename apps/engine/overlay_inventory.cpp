// overlay_inventory.cpp: the 0.6 inventory screen (GI's REV3 Claude Design). See overlay_inventory.h for what round 1 covers.
//
// Every number below is the spec's (HUD_DESIGN_COMPONENTS/REV3/BUILD_SPEC.md and layout/txt/*.txt): DESIGN px on a 1920 x 1080 screen with the
// origin at the BOTTOM-LEFT and y UP, boxes as (x, y, w, h) = bottom-left corner + size. The screen maps them with one scale s = min(H / 1080,
// W / 1920), centred (an ultrawide keeps the design's proportions and gains side margins). Fonts are the design's Sofia Sans Medium / SemiBold /
// Bold, baked at the drawn size; the 46 glyphs are the design's 96 px PNGs, area-filtered into the font atlas at every size the design draws,
// with their glows (drop-shadow sigma 2.5) blurred on the CPU. Not drawn: the backdrop blur behind the panels (the spec's sigma 14 pass needs
// a capture of the game's frame; round 1's designed fallback is the plain glass over the world dim).
#include "overlay_inventory.h"

#include "overlay.h"
#include "common/logger.h"
#include "sync/inventory.h"
#include "sync/ped_view.h"      // 0.5.4 inv_ped_view: the CHARACTER panel's window (shared with the engine's camera framing)
#include "overlay_sofia3b.h"   // kOverlaySofia3BoldTtf: Sofia Sans Bold (REV3's conversion, SIL OFL 1.1, brand/fonts/rev3)
#include "overlay_sofia3s.h"    // kOverlaySofia3SemiTtf: SemiBold
#include "overlay_sofia3m.h"    // kOverlaySofia3MediumTtf: Medium
#include "overlay_inv_icons.h"  // kInvIcons[]: the 46 REV3 glyphs (brand/icons/inventory/*.png, 96 x 96 white + alpha)

#include "imgui.h"
#include "imgui_internal.h"     // ImTextCharFromUtf8

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// Kerning pairs (ImGui's text path has none): stb_truetype's own copy, static to this file (imgui_draw.cpp keeps its own static one), used only
// at bake time to read each face's kern / GPOS pairs.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace inv = gage::sync::inv;

// ---------------------------------------------------------------------------------------------------------------------------------------------
// The engine's side: the server's snapshots in, the player's ops out (overlay.h). Any thread; the render thread takes them in DrawScreen.
// ---------------------------------------------------------------------------------------------------------------------------------------------
namespace gage::game::Overlay {
namespace {
    struct Store {
        std::mutex m;
        std::string snapshot;      // inv::Encode bytes, the server's latest
        uint32_t ack = 0;          // the last op seq the server had processed when it sent it
        uint32_t snapSeq = 0;      // +1 per SetInventory
        bool available = false;
        std::string name;          // the character's name (the CHARACTER chip)
        std::deque<std::pair<uint32_t, std::string>> out;   // seq, inv::EncodeOp bytes
        uint32_t nextSeq = 1;
        // 0.5.4 inventory world
        bool world = false;
        std::vector<InvNearbyBox> boxes;
        uint32_t nearSeq = 0;      // +1 per SetNearby / SetInventoryWorld / ClearInventory
    };
    Store& St() { static Store* s = new Store; return *s; }   // never freed (the render thread may outlive static destruction)
}

void SetInventory(const std::string& encoded, uint32_t ackSeq, const char* characterName)
{
    Store& s = St();
    std::lock_guard<std::mutex> lk(s.m);
    s.snapshot = encoded;
    s.ack = ackSeq;
    s.available = true;
    s.name = characterName ? characterName : "";
    ++s.snapSeq;
}

void ClearInventory()
{
    Store& s = St();
    std::lock_guard<std::mutex> lk(s.m);
    s.snapshot.clear();
    s.available = false;
    s.out.clear();
    ++s.snapSeq;
    s.world = false;
    s.boxes.clear();
    ++s.nearSeq;
}

void SetInventoryWorld(bool on)
{
    Store& s = St();
    std::lock_guard<std::mutex> lk(s.m);
    s.world = on;
    if (!on)
        s.boxes.clear();
    ++s.nearSeq;
}

void SetNearby(const std::vector<InvNearbyBox>& boxes)
{
    Store& s = St();
    std::lock_guard<std::mutex> lk(s.m);
    s.boxes = boxes;
    ++s.nearSeq;
}

// 0.5.6 weapon_keys: the number keys 1-4 in the world (overlay_ui.cpp UiFrame reads them, inv3::QuickKey sends the op).
namespace {
    std::atomic<bool> g_quickKeys{true};
    std::atomic<uint32_t> g_quickSent{0}, g_quickRefused{0};
}
void SetQuickKeys(bool on) { g_quickKeys.store(on); }
void QuickKeyCounts(uint32_t* sent, uint32_t* refused)
{
    if (sent) *sent = g_quickSent.exchange(0);
    if (refused) *refused = g_quickRefused.exchange(0);
}
namespace detail::inv3 {
    bool QuickKeysOn() { return g_quickKeys.load(); }
}

bool PopInventoryOp(std::string* encodedOp, uint32_t* seq)
{
    Store& s = St();
    std::lock_guard<std::mutex> lk(s.m);
    if (s.out.empty())
        return false;
    if (seq)
        *seq = s.out.front().first;
    if (encodedOp)
        *encodedOp = std::move(s.out.front().second);
    s.out.pop_front();
    return true;
}

// ---- 0.5.4 inv_ped_view: the CHARACTER panel's window onto the player's own ped (libs/sync/ped_view.h) ------------------------------------------
// The render thread stamps every frame it draws the page (and the display size); the engine's script tick reads the stamp to know the page is
// really on screen, and says back whether its scripted camera is rendering (then the panel shows the window instead of the mannequin).
namespace {
    std::atomic<unsigned long long> g_pedViewDrawnAt{0};   // GetTickCount64 of the last frame that drew the page; 0 = never
    std::atomic<int> g_pedViewW{0}, g_pedViewH{0};
    std::atomic<bool> g_pedViewLive{false};
    std::atomic<unsigned long long> g_pedViewLiveAt{0};    // GetTickCount64 of the engine's last report (it reports every script tick)
    // The engine's word counts only while fresh: a script tick that stopped (focus lost, a hang) must not leave a window over a frame its
    // camera no longer renders.
    bool PedViewFresh()
    {
        const unsigned long long at = g_pedViewLiveAt.load(), now = GetTickCount64();
        return g_pedViewLive.load() && at && now >= at && now - at < 500;
    }
}
bool InventoryPedViewFrame(float* W, float* H, double* agoMs)
{
    const unsigned long long at = g_pedViewDrawnAt.load();
    if (!at)
        return false;
    const unsigned long long now = GetTickCount64();
    if (W) *W = (float)g_pedViewW.load();
    if (H) *H = (float)g_pedViewH.load();
    if (agoMs) *agoMs = now > at ? (double)(now - at) : 0.0;
    return true;
}
void SetInventoryPedView(bool live)
{
    g_pedViewLive.store(live);
    g_pedViewLiveAt.store(GetTickCount64());
}

}  // namespace gage::game::Overlay

namespace gage::game::Overlay::detail::inv3 {
namespace {

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Tokens (section 3)
// ---------------------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kGround = 0x0A0907FF, kAmber = 0xF2B134FF, kBone = 0xD9D1C3FF, kCream = 0xF7F2E8FF, kEmber = 0xE5603AFF, kMuted = 0xA9A295FF;
constexpr uint32_t kGlassBorder = 0xF7F2E824;
// The spec's glass is backdrop-blurred (panels 24 px, overlays 30 px; panels 0xBD over a world dim of 0x73). Round 1 has no blur pass, and the
// unblurred game showed through: a lamp edge ~33 grey levels behind a panel, "SPLIT  KG" read through the menu (critic round 1), then faces,
// signs and lit window grids inside the slots at 8 % transmission (round 2). Until the blur exists the panels are dense rather than see-through:
// panels 0xE6 (3 % of the world: its variation falls under a slot border's step), overlays 0xF7 (the panel-to-overlay step stays), the world
// dim 0xB0 (the margins still show the game). Back to 0xBD / 0xE6 / 0x73 when the blur lands.
constexpr uint32_t kPanelGlass = 0x0A0907E6, kOverlayGlass = 0x0A0907F7, kDim = 0x0A0907B0;
constexpr uint32_t kCardFill = 0xF7F2E80D, kCardBorder = 0xF7F2E81F, kTrack = 0xF7F2E829;
constexpr float kPi = 3.14159265358979f;

ImU32 C(uint32_t rgba, float a = 1.0f)
{
    const float al = (float)(rgba & 0xFF) * (a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a));
    return IM_COL32((rgba >> 24) & 0xFF, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF, (int)(al + 0.5f));
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Fonts (section 6): every (weight, size) the screen draws
// ---------------------------------------------------------------------------------------------------------------------------------------------
enum Face { kBoldFace, kSemiFace, kMediumFace, kFaceCount };
enum F { B10, B11, B12, B13, B14, B16, B18, B20, B24, B28, B34, S12, S13, M13, M14, kFontCount };
const ImWchar kLatin[] = {0x0020, 0x007E, 0x00A0, 0x00FF, 0x2013, 0x2014, 0x2026, 0x2026, 0};
const ImWchar kCaps[] = {0x0020, 0x0020, 0x0041, 0x005A, 0};
const ImWchar kDigits[] = {0x0030, 0x0039, 0};
struct FontSpec { Face face; float px; const ImWchar* ranges; };
const FontSpec kFonts[kFontCount] = {
    {kBoldFace, 10, kLatin}, {kBoldFace, 11, kLatin}, {kBoldFace, 12, kLatin}, {kBoldFace, 13, kLatin}, {kBoldFace, 14, kLatin},
    {kBoldFace, 16, kLatin}, {kBoldFace, 18, kLatin}, {kBoldFace, 20, kLatin}, {kBoldFace, 24, kLatin}, {kBoldFace, 28, kCaps},
    {kBoldFace, 34, kDigits}, {kSemiFace, 12, kLatin}, {kSemiFace, 13, kLatin}, {kMediumFace, 13, kLatin}, {kMediumFace, 14, kLatin},
};

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Icons (section 7): the sizes each glyph is baked at, and the glows
// ---------------------------------------------------------------------------------------------------------------------------------------------
constexpr float kIconSizes[] = {14, 16, 18, 23, 26, 28, 32, 36, 40};
constexpr int kIconSizeCount = (int)(sizeof(kIconSizes) / sizeof(kIconSizes[0]));
constexpr float kGlowSizes[] = {16, 23, 26, 28, 32, 36};
constexpr int kGlowSizeCount = (int)(sizeof(kGlowSizes) / sizeof(kGlowSizes[0]));
constexpr float kGlowSigma = 2.5f;   // drop-shadow 0 0 5 px
constexpr int kMaxIcons = 64;

struct AtlasRect { int rect = -1; int px = 0; int pad = 0; };

struct Baked {
    bool fontsOk = false;
    float bakeScale = 1.0f;
    float em[kFaceCount] = {1, 1, 1};
    // kern pairs for printable ASCII (95 x 95, font units) and 1 / (ascent - descent): a pair's px = units * drawn size * kernUnit
    std::vector<int16_t> kern[kFaceCount];
    float kernUnit[kFaceCount] = {0, 0, 0};
    ImFont* font[kFontCount] = {};
    int iconCount = 0;
    AtlasRect icon[kMaxIcons][kIconSizeCount];
    AtlasRect glow[kMaxIcons][kGlowSizeCount];
    AtlasRect manikin;
    int manW = 0, manH = 0;
};
Baked& B() { static Baked* b = new Baked; return *b; }

float TtfLineToEm(const unsigned char* d, std::size_t n)
{
    auto u16 = [&](std::size_t o) { return o + 2 <= n ? (unsigned)((d[o] << 8) | d[o + 1]) : 0u; };
    auto u32 = [&](std::size_t o) { return o + 4 <= n ? ((std::size_t)d[o] << 24) | ((std::size_t)d[o + 1] << 16) | ((std::size_t)d[o + 2] << 8) | d[o + 3] : (std::size_t)0; };
    const unsigned tables = u16(4);
    std::size_t head = 0, hhea = 0;
    for (unsigned i = 0; i < tables; ++i) {
        const std::size_t rec = 12 + 16 * (std::size_t)i;
        if (rec + 16 > n)
            break;
        if (std::memcmp(d + rec, "head", 4) == 0) head = u32(rec + 8);
        if (std::memcmp(d + rec, "hhea", 4) == 0) hhea = u32(rec + 8);
    }
    if (!head || !hhea)
        return 0.0f;
    const unsigned upem = u16(head + 18);
    const int asc = (int16_t)u16(hhea + 4), desc = (int16_t)u16(hhea + 6);
    if (upem < 16 || asc - desc <= 0)
        return 0.0f;
    return (float)(asc - desc) / (float)upem;
}

// The 'kern' feature's pair adjustments (GPOS lookup type 2, also inside type 9 extensions; formats 1 and 2): the REV3 faces carry their
// kerning only there, and ImGui's stb_truetype reads neither extension lookups nor this font's layout (it found 0 pairs). XAdvance of the
// first glyph only, in font units. Every read is bounds-checked: a malformed table gives 0, never a read past the font.
struct Gpos
{
    const unsigned char* d = nullptr;
    std::size_t n = 0, base = 0;   // base = the GPOS table's offset
    std::vector<std::size_t> subtables;   // absolute offsets of the PairPos subtables of every 'kern' lookup, in lookup order
    std::vector<int> lookupOf;            // which lookup each subtable belongs to (the first matching subtable of a lookup applies)

    unsigned U16(std::size_t o) const { return o + 2 <= n ? (unsigned)((d[o] << 8) | d[o + 1]) : 0u; }
    std::size_t U32(std::size_t o) const
    {
        return o + 4 <= n ? ((std::size_t)d[o] << 24) | ((std::size_t)d[o + 1] << 16) | ((std::size_t)d[o + 2] << 8) | d[o + 3] : 0;
    }
    static int VrSize(unsigned vf) { int c = 0; for (unsigned b = vf & 0xFF; b; b >>= 1) c += (int)(b & 1); return c * 2; }
    // the XAdvance field of a value record at o (0 when the format has none)
    int XAdv(std::size_t o, unsigned vf) const
    {
        if (!(vf & 0x0004)) return 0;
        const int skip = ((vf & 1) ? 2 : 0) + ((vf & 2) ? 2 : 0);
        return (int16_t)U16(o + (std::size_t)skip);
    }
    int Coverage(std::size_t c, unsigned glyph) const
    {
        const unsigned fmt = U16(c);
        if (fmt == 1) {
            const unsigned cnt = U16(c + 2);
            for (unsigned i = 0; i < cnt; ++i)
                if (U16(c + 4 + 2 * (std::size_t)i) == glyph) return (int)i;
        } else if (fmt == 2) {
            const unsigned cnt = U16(c + 2);
            for (unsigned i = 0; i < cnt; ++i) {
                const std::size_t r = c + 4 + 6 * (std::size_t)i;
                if (glyph >= U16(r) && glyph <= U16(r + 2)) return (int)(U16(r + 4) + (glyph - U16(r)));
            }
        }
        return -1;
    }
    unsigned ClassOf(std::size_t c, unsigned glyph) const
    {
        const unsigned fmt = U16(c);
        if (fmt == 1) {
            const unsigned start = U16(c + 2), cnt = U16(c + 4);
            if (glyph >= start && glyph < start + cnt) return U16(c + 6 + 2 * (std::size_t)(glyph - start));
        } else if (fmt == 2) {
            const unsigned cnt = U16(c + 2);
            for (unsigned i = 0; i < cnt; ++i) {
                const std::size_t r = c + 4 + 6 * (std::size_t)i;
                if (glyph >= U16(r) && glyph <= U16(r + 2)) return U16(r + 4);
            }
        }
        return 0;
    }
    bool Init(const unsigned char* data, std::size_t len)
    {
        d = data;
        n = len;
        const unsigned tables = U16(4);
        for (unsigned i = 0; i < tables; ++i) {
            const std::size_t rec = 12 + 16 * (std::size_t)i;
            if (rec + 16 <= n && std::memcmp(d + rec, "GPOS", 4) == 0) base = U32(rec + 8);
        }
        if (!base || base >= n)
            return false;
        const std::size_t features = base + U16(base + 6), lookups = base + U16(base + 8);
        std::vector<unsigned> want;
        const unsigned fc = U16(features);
        for (unsigned i = 0; i < fc; ++i) {
            const std::size_t rec = features + 2 + 6 * (std::size_t)i;
            if (rec + 6 > n || std::memcmp(d + rec, "kern", 4) != 0) continue;
            const std::size_t ft = features + U16(rec + 4);
            const unsigned lc = U16(ft + 2);
            for (unsigned k = 0; k < lc; ++k) want.push_back(U16(ft + 4 + 2 * (std::size_t)k));
        }
        std::sort(want.begin(), want.end());
        want.erase(std::unique(want.begin(), want.end()), want.end());
        const unsigned lookupCount = U16(lookups);
        for (unsigned li : want) {
            if (li >= lookupCount) continue;
            const std::size_t lk = lookups + U16(lookups + 2 + 2 * (std::size_t)li);
            const unsigned type = U16(lk), sc = U16(lk + 4);
            for (unsigned s = 0; s < sc; ++s) {
                std::size_t st = lk + U16(lk + 6 + 2 * (std::size_t)s);
                if (type == 9) {
                    if (U16(st) != 1 || U16(st + 2) != 2) continue;
                    st += U32(st + 4);
                } else if (type != 2) {
                    continue;
                }
                if (st < n) { subtables.push_back(st); lookupOf.push_back((int)li); }
            }
        }
        return !subtables.empty();
    }
    int Pair(unsigned a, unsigned b) const
    {
        int total = 0, doneLookup = -1;
        for (std::size_t i = 0; i < subtables.size(); ++i) {
            if (lookupOf[i] == doneLookup) continue;
            const std::size_t st = subtables[i];
            const unsigned fmt = U16(st), vf1 = U16(st + 4), vf2 = U16(st + 6);
            const int ci = Coverage(st + U16(st + 2), a);
            if (ci < 0) continue;
            if (fmt == 1) {
                if ((unsigned)ci >= U16(st + 8)) continue;
                const std::size_t ps = st + U16(st + 10 + 2 * (std::size_t)ci);
                const unsigned cnt = U16(ps);
                const std::size_t recSize = 2 + (std::size_t)VrSize(vf1) + (std::size_t)VrSize(vf2);
                for (unsigned k = 0; k < cnt; ++k) {
                    const std::size_t r = ps + 2 + recSize * k;
                    if (U16(r) == b) { total += XAdv(r + 2, vf1); doneLookup = lookupOf[i]; break; }
                }
            } else if (fmt == 2) {
                const unsigned c1 = ClassOf(st + U16(st + 8), a), c2 = ClassOf(st + U16(st + 10), b);
                const unsigned n1 = U16(st + 12), n2 = U16(st + 14);
                if (c1 >= n1 || c2 >= n2) continue;
                const std::size_t recSize = (std::size_t)VrSize(vf1) + (std::size_t)VrSize(vf2);
                total += XAdv(st + 16 + ((std::size_t)c1 * n2 + c2) * recSize, vf1);
                doneLookup = lookupOf[i];
            }
        }
        return total;
    }
};

// Which (glyph, size) pairs the screen draws, so the atlas holds only those (at 4K every bake is 4x the pixels).
bool OneOf(const char* n, std::initializer_list<const char*> l)
{
    for (const char* x : l)
        if (std::strcmp(x, n) == 0) return true;
    return false;
}
bool ItemGlyph(const char* n)
{
    for (const inv::ItemDef& d : inv::Catalog())
        if (std::strcmp(d.glyph, n) == 0) return true;
    for (int i = 0; i < inv::kEquipCount; ++i)
        if (std::strcmp(inv::EquipGlyph(inv::EquipAt(i)), n) == 0) return true;
    return false;
}
bool WantIcon(const char* n, float size)
{
    switch ((int)size) {
    case 14: return OneOf(n, {"chev", "key", "lock", "unlock"});
    case 16: return true;                                   // menu rows (every item glyph), tabs, buttons, the search
    case 18: return OneOf(n, {"weight"});
    case 23: return OneOf(n, {"lock", "plus"});
    case 26: return ItemGlyph(n);                           // empty equipment slots
    case 28: return ItemGlyph(n) || OneOf(n, {"car", "moto", "van", "drop", "stash", "lock", "unlock", "person"});
    case 32: case 36: return ItemGlyph(n) || OneOf(n, {"plus"});
    case 40: return OneOf(n, {"car", "moto", "van", "drop", "stash"});
    default: return true;
    }
}
bool WantGlow(const char* n, float size)
{
    switch ((int)size) {
    case 16: return OneOf(n, {"backpack", "wrench"});                         // the active top tab
    case 23: return OneOf(n, {"plus"});                                         // an empty drop target
    case 26: case 32: return ItemGlyph(n);                                      // equipment fits / denied; a selected or denied slot
    case 28: return OneOf(n, {"backpack"});                                     // the PACK slot denied
    case 36: return ItemGlyph(n) || OneOf(n, {"plus"});                         // the detail tile, a pressed / binding quick slot
    default: return false;
    }
}

int IconIndex(const char* name)
{
    if (!name || !*name)
        return -1;
    for (int i = 0; i < kInvIconsCount && i < kMaxIcons; ++i)
        if (std::strcmp(kInvIcons[i].name, name) == 0)
            return i;
    return -1;
}

// PNG -> alpha (0..1), through WIC (as overlay.cpp decodes the brand art). false = not decodable.
bool DecodeAlpha(const unsigned char* bytes, std::size_t n, std::vector<float>& out, int& w, int& h)
{
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInit = SUCCEEDED(hrCo);
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = false;
    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&factory))) break;
        if (FAILED(factory->CreateStream(&stream))) break;
        if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes), (DWORD)n))) break;
        if (FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) break;
        if (FAILED(decoder->GetFrame(0, &frame))) break;
        UINT uw = 0, uh = 0;
        if (FAILED(frame->GetSize(&uw, &uh)) || uw == 0 || uh == 0 || uw > 1024 || uh > 1024) break;
        if (FAILED(factory->CreateFormatConverter(&conv))) break;
        if (FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) break;
        std::vector<unsigned char> px((size_t)uw * uh * 4);
        if (FAILED(conv->CopyPixels(nullptr, uw * 4, (UINT)px.size(), px.data()))) break;
        w = (int)uw;
        h = (int)uh;
        out.assign((size_t)uw * uh, 0.0f);
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = (float)px[i * 4 + 3] / 255.0f;
        ok = true;
    } while (false);
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (coInit) CoUninitialize();
    return ok;
}

// Area (box) filter of a w x h coverage map down to n x n.
std::vector<float> Downsample(const std::vector<float>& src, int w, int h, int n)
{
    std::vector<float> out((size_t)n * n, 0.0f);
    const float fx = (float)w / (float)n, fy = (float)h / (float)n;
    for (int oy = 0; oy < n; ++oy) {
        const float y0 = oy * fy, y1 = (oy + 1) * fy;
        for (int ox = 0; ox < n; ++ox) {
            const float x0 = ox * fx, x1 = (ox + 1) * fx;
            float acc = 0.0f;
            for (int sy = (int)std::floor(y0); sy < (int)std::ceil(y1) && sy < h; ++sy) {
                const float wy = std::min(y1, (float)sy + 1.0f) - std::max(y0, (float)sy);
                if (wy <= 0.0f) continue;
                for (int sx = (int)std::floor(x0); sx < (int)std::ceil(x1) && sx < w; ++sx) {
                    const float wx = std::min(x1, (float)sx + 1.0f) - std::max(x0, (float)sx);
                    if (wx > 0.0f) acc += wx * wy * src[(size_t)sy * w + sx];
                }
            }
            out[(size_t)oy * n + ox] = acc / (fx * fy);
        }
    }
    return out;
}

void GaussBlur(std::vector<float>& a, int W, int H, float sigma)
{
    sigma = std::max(0.5f, sigma);
    const int kr = (int)std::ceil(3.0f * sigma);
    std::vector<float> ker((size_t)(2 * kr + 1));
    float ks = 0.0f;
    for (int j = -kr; j <= kr; ++j) ks += ker[(size_t)(j + kr)] = std::exp(-0.5f * (float)(j * j) / (sigma * sigma));
    for (float& kv : ker) kv /= ks;
    std::vector<float> tmp((size_t)W * H, 0.0f);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float acc = 0.0f;
            for (int j = -kr; j <= kr; ++j) { const int xx = x + j; if (xx >= 0 && xx < W) acc += ker[(size_t)(j + kr)] * a[(size_t)y * W + xx]; }
            tmp[(size_t)y * W + x] = acc;
        }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float acc = 0.0f;
            for (int j = -kr; j <= kr; ++j) { const int yy = y + j; if (yy >= 0 && yy < H) acc += ker[(size_t)(j + kr)] * tmp[(size_t)yy * W + x]; }
            a[(size_t)y * W + x] = acc;
        }
}

// Coverage of an ImDrawList's triangles (vertex alpha interpolated, union by max) into a W x H map. The mannequin's shapes are unioned so
// its group opacity is one alpha (overlapping limbs never darken).
void Rasterise(const ImDrawList& dl, int W, int H, std::vector<float>& a)
{
    a.assign((size_t)W * H, 0.0f);
    for (const ImDrawCmd& cmd : dl.CmdBuffer)
        for (unsigned e = 0; e + 2 < cmd.ElemCount; e += 3) {
            const ImDrawVert* v[3];
            for (int t = 0; t < 3; ++t) v[t] = &dl.VtxBuffer[(int)(cmd.VtxOffset + dl.IdxBuffer[(int)(cmd.IdxOffset + e + t)])];
            const float area = (v[1]->pos.x - v[0]->pos.x) * (v[2]->pos.y - v[0]->pos.y) - (v[1]->pos.y - v[0]->pos.y) * (v[2]->pos.x - v[0]->pos.x);
            if (std::fabs(area) < 1e-6f) continue;
            const int x0 = std::max(0, (int)std::floor(std::min({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x})));
            const int x1 = std::min(W - 1, (int)std::ceil(std::max({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x})));
            const int y0 = std::max(0, (int)std::floor(std::min({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y})));
            const int y1 = std::min(H - 1, (int)std::ceil(std::max({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y})));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const float px = (float)x + 0.5f, py = (float)y + 0.5f;
                    const float w0 = ((v[1]->pos.x - px) * (v[2]->pos.y - py) - (v[1]->pos.y - py) * (v[2]->pos.x - px)) / area;
                    const float w1 = ((v[2]->pos.x - px) * (v[0]->pos.y - py) - (v[2]->pos.y - py) * (v[0]->pos.x - px)) / area;
                    const float w2 = 1.0f - w0 - w1;
                    if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                    const float va = (w0 * (float)(v[0]->col >> IM_COL32_A_SHIFT) + w1 * (float)(v[1]->col >> IM_COL32_A_SHIFT) +
                                      w2 * (float)(v[2]->col >> IM_COL32_A_SHIFT)) / 255.0f;
                    float& d = a[(size_t)y * W + x];
                    d = std::max(d, va);
                }
        }
}

// The mannequin (section 4.1) in its own top-left coordinates (224 x 532), times k.
void DrawManikinShapes(ImDrawList* dl, float k, float o)
{
    const ImU32 w = IM_COL32(255, 255, 255, 255);
    const auto P = [&](float x, float y) { return ImVec2(o + x * k, o + y * k); };
    const auto capsule = [&](float x0, float y0, float x1, float y1, float width) {
        dl->AddLine(P(x0, y0), P(x1, y1), w, width * k);
        dl->AddCircleFilled(P(x0, y0), width * 0.5f * k, w, 48);
        dl->AddCircleFilled(P(x1, y1), width * 0.5f * k, w, 48);
    };
    dl->AddCircleFilled(P(112, 58), 34 * k, w, 64);
    capsule(112, 88, 112, 116, 26);
    dl->AddRectFilled(P(62, 108), P(162, 284), w, 32 * k);
    capsule(58, 134, 38, 296, 30);
    capsule(166, 134, 186, 296, 30);
    dl->AddCircleFilled(P(36, 316), 15 * k, w, 40);
    dl->AddCircleFilled(P(188, 316), 15 * k, w, 40);
    capsule(88, 276, 84, 476, 44);
    capsule(136, 276, 140, 476, 44);
    dl->AddEllipseFilled(P(80, 504), 30 * k, 14 * k, w, 0.0f, 48);
    dl->AddEllipseFilled(P(144, 504), 30 * k, 14 * k, w, 0.0f, 48);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Frame geometry: design (bottom-left, y up) -> screen
// ---------------------------------------------------------------------------------------------------------------------------------------------
struct Geo { float s = 1, ox = 0, oy = 0, a = 1; ImDrawList* dl = nullptr; };
Geo g;

float Sx(float x) { return g.ox + x * g.s; }
float Sy(float y) { return g.oy + (1080.0f - y) * g.s; }
float Rn(float v) { return std::floor(v + 0.5f); }
// A design box as a snapped screen rect.
struct Rc { ImVec2 a, b; };
Rc Box(float x, float y, float w, float h) { return {ImVec2(Rn(Sx(x)), Rn(Sy(y + h))), ImVec2(Rn(Sx(x + w)), Rn(Sy(y)))}; }
ImVec2 DesignOf(ImVec2 p) { return ImVec2((p.x - g.ox) / g.s, 1080.0f - (p.y - g.oy) / g.s); }
bool In(ImVec2 d, float x, float y, float w, float h) { return d.x >= x && d.x < x + w && d.y >= y && d.y < y + h; }

// Rounded-rect outline (screen), clockwise from the top-left corner, 4 x (kArc + 1) points.
constexpr int kArc = 8, kRRPts = 4 * (kArc + 1);
void RRPath(ImVec2 a, ImVec2 b, float r, ImVec2* out)
{
    r = std::max(0.0f, std::min(r, std::min(b.x - a.x, b.y - a.y) * 0.5f));
    const ImVec2 c[4] = {ImVec2(a.x + r, a.y + r), ImVec2(b.x - r, a.y + r), ImVec2(b.x - r, b.y - r), ImVec2(a.x + r, b.y - r)};
    const float a0[4] = {kPi, 1.5f * kPi, 0.0f, 0.5f * kPi};
    int n = 0;
    for (int k = 0; k < 4; ++k)
        for (int j = 0; j <= kArc; ++j) {
            const float t = a0[k] + 0.5f * kPi * (float)j / (float)kArc;
            out[n++] = ImVec2(c[k].x + r * std::cos(t), c[k].y + r * std::sin(t));
        }
}

void Fill(const Rc& r, float rad, uint32_t rgba, float a = 1.0f) { g.dl->AddRectFilled(r.a, r.b, C(rgba, g.a * a), rad * g.s); }
// A border INSIDE the box (CSS). ImGui's AddRect already insets its path by half a pixel.
void Border(const Rc& r, float rad, uint32_t rgba, float bw, float a = 1.0f)
{
    const float w = bw * g.s, k = w * 0.5f - 0.5f;
    g.dl->AddRect(ImVec2(r.a.x + k, r.a.y + k), ImVec2(r.b.x - k, r.b.y - k), C(rgba, g.a * a), std::max(0.0f, rad * g.s - w * 0.5f), 0, w);
}
// Dashed border (section 0: dash 3, gap 2 on the stroke's centre line, wrapping the corners).
void Dashed(const Rc& r, float rad, uint32_t rgba, float bw, float a = 1.0f)
{
    const float w = bw * g.s, h = w * 0.5f;
    ImVec2 pts[kRRPts + 1];
    RRPath(ImVec2(r.a.x + h, r.a.y + h), ImVec2(r.b.x - h, r.b.y - h), rad * g.s - h, pts);
    pts[kRRPts] = pts[0];
    std::vector<float> cum(kRRPts + 1, 0.0f);
    for (int i = 1; i <= kRRPts; ++i)
        cum[(size_t)i] = cum[(size_t)i - 1] + std::sqrt(ImLengthSqr(ImVec2(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y)));
    const float L = cum[kRRPts], dash = 3.0f * g.s, gap = 2.0f * g.s;
    if (L <= 0.0f)
        return;
    const auto at = [&](float t) {
        int i = 1;
        while (i < kRRPts && cum[(size_t)i] < t) ++i;
        const float seg = cum[(size_t)i] - cum[(size_t)i - 1];
        const float u = seg > 0.0f ? (t - cum[(size_t)i - 1]) / seg : 0.0f;
        return std::make_pair(i, ImVec2(pts[i - 1].x + (pts[i].x - pts[i - 1].x) * u, pts[i - 1].y + (pts[i].y - pts[i - 1].y) * u));
    };
    const ImU32 col = C(rgba, g.a * a);
    std::vector<ImVec2> run;
    for (float t = 0.0f; t < L; t += dash + gap) {
        const float t1 = std::min(L, t + dash);
        const auto p0 = at(t), p1 = at(t1);
        run.clear();
        run.push_back(p0.second);
        for (int i = p0.first; i < p1.first; ++i) run.push_back(pts[i]);
        run.push_back(p1.second);
        g.dl->AddPolyline(run.data(), (int)run.size(), col, 0, w);
    }
}
// box-shadow 0 0 blur (sigma) outside the box: rings from the edge (0.5 x alpha, the edge of a blurred box) out to 3 sigma, each at the
// Gaussian's tail there; the inside is the element's own (CSS never paints a box-shadow under its box). dy: offset down (screen px).
void Shadow(const Rc& r, float rad, uint32_t rgba, float sigmaDesign, float dyDesign = 0.0f, float a = 1.0f)
{
    const float sigma = sigmaDesign * g.s;
    if (sigma <= 0.0f)
        return;
    constexpr int kRings = 10;
    const float ext = 3.0f * sigma, dy = dyDesign * g.s;
    const ImVec2 uv = ImGui::GetDrawListSharedData()->TexUvWhitePixel;
    ImDrawList* dl = g.dl;
    dl->PrimReserve(kRRPts * (kRings - 1) * 6, kRRPts * kRings);
    const unsigned base = dl->_VtxCurrentIdx;
    ImVec2 pts[kRRPts];
    for (int j = 0; j < kRings; ++j) {
        const float d = ext * (float)j / (float)(kRings - 1);
        const float al = 0.5f * std::erfc(d / (sigma * 1.41421356f));
        RRPath(ImVec2(r.a.x - d, r.a.y - d + dy), ImVec2(r.b.x + d, r.b.y + d + dy), rad * g.s + d, pts);
        const ImU32 col = C(rgba, g.a * a * al);
        for (int i = 0; i < kRRPts; ++i)
            dl->PrimWriteVtx(pts[i], uv, col);
    }
    for (int j = 0; j + 1 < kRings; ++j)
        for (int i = 0; i < kRRPts; ++i) {
            const unsigned i0 = base + (unsigned)(j * kRRPts + i), i1 = base + (unsigned)(j * kRRPts + (i + 1) % kRRPts);
            const unsigned o0 = i0 + kRRPts, o1 = i1 + kRRPts;
            dl->PrimWriteIdx((ImDrawIdx)i0); dl->PrimWriteIdx((ImDrawIdx)i1); dl->PrimWriteIdx((ImDrawIdx)o1);
            dl->PrimWriteIdx((ImDrawIdx)i0); dl->PrimWriteIdx((ImDrawIdx)o1); dl->PrimWriteIdx((ImDrawIdx)o0);
        }
}
// radial-gradient(closest-side, rgba, transparent) in a design square (the CHARACTER halo).
void RadialGlow(float x, float y, float size, uint32_t rgba)
{
    const ImVec2 c((Sx(x) + Sx(x + size)) * 0.5f, (Sy(y) + Sy(y + size)) * 0.5f);
    const float R = size * 0.5f * g.s;
    constexpr int kSeg = 72;
    const ImVec2 uv = ImGui::GetDrawListSharedData()->TexUvWhitePixel;
    ImDrawList* dl = g.dl;
    dl->PrimReserve(kSeg * 3, kSeg + 1);
    const unsigned base = dl->_VtxCurrentIdx;
    dl->PrimWriteVtx(c, uv, C(rgba, g.a));
    for (int i = 0; i < kSeg; ++i) {
        const float t = 2.0f * kPi * (float)i / (float)kSeg;
        dl->PrimWriteVtx(ImVec2(c.x + R * std::cos(t), c.y + R * std::sin(t)), uv, C(rgba, 0.0f));
    }
    for (int i = 0; i < kSeg; ++i) {
        dl->PrimWriteIdx((ImDrawIdx)base);
        dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
        dl->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % kSeg));
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Text: x / baseline in design px; anchor 0 = x is the left edge, 1 = the centre, 2 = the right edge (trailing tracking included, as the spec)
// ---------------------------------------------------------------------------------------------------------------------------------------------
enum Anchor { kLeft, kCenter, kRight };
// Text never shrinks below 9 px (the design's 10-12 px captions came out at 6.7-8 px at 720p, scale 0.667); the layout itself still scales.
constexpr float kMinTextPx = 9.0f;
float CssPx(float css, float scale) { return std::max(css * scale, std::min(css, kMinTextPx)); }
float DrawPx(F f) { return CssPx(kFonts[f].px, g.s) * B().em[kFonts[f].face]; }

float DigitCell(const ImFont* font, float k)
{
    float m = 0.0f;
    for (ImWchar c = '0'; c <= '9'; ++c)
        if (const ImFontGlyph* gl = font->FindGlyphNoFallback(c)) m = std::max(m, gl->AdvanceX);
    return m * k;
}

// The kerning between two characters in screen px (0 outside printable ASCII, and around a tabular digit: its cell is fixed).
float Kern(F f, unsigned a, unsigned b, bool tab)
{
    const Baked& bk = B();
    const int face = kFonts[f].face;
    if (a < 0x20 || a > 0x7E || b < 0x20 || b > 0x7E || bk.kern[face].empty())
        return 0.0f;
    if (tab && ((a >= '0' && a <= '9') || (b >= '0' && b <= '9')))
        return 0.0f;
    return (float)bk.kern[face][(size_t)(a - 0x20) * 95 + (b - 0x20)] * DrawPx(f) * bk.kernUnit[face];
}

// Screen width of a text (tracking after every glyph, the last included).
float TextW(F f, const char* t, float track = 0.0f, bool tab = false)
{
    const ImFont* font = B().font[f];
    if (!font || !t)
        return 0.0f;
    const float k = DrawPx(f) / font->FontSize, tr = track * g.s;
    const float cell = tab ? DigitCell(font, k) : 0.0f;
    float w = 0.0f;
    unsigned prev = 0;
    const char* end = t + std::strlen(t);
    for (const char* p = t; p < end;) {
        unsigned c = 0;
        const int n = ImTextCharFromUtf8(&c, p, end);
        p += n > 0 ? n : 1;
        const ImFontGlyph* gl = font->FindGlyph((ImWchar)c);
        w += Kern(f, prev, c, tab) + ((tab && c >= '0' && c <= '9') ? cell : (gl ? gl->AdvanceX * k : 0.0f)) + tr;
        prev = c;
    }
    return w;
}
float TextWDesign(F f, const char* t, float track = 0.0f, bool tab = false) { return TextW(f, t, track, tab) / g.s; }

void Text(F f, float x, float base, uint32_t rgba, const char* t, float track = 0.0f, Anchor an = kLeft, bool tab = false, float a = 1.0f,
          bool shadow = false)
{
    const ImFont* font = B().font[f];
    if (!font || !t || !*t)
        return;
    const float px = DrawPx(f), k = px / font->FontSize, tr = track * g.s;
    const float cell = tab ? DigitCell(font, k) : 0.0f;
    float pen = Sx(x);
    if (an != kLeft) {
        const float w = TextW(f, t, track, tab);
        pen -= an == kCenter ? w * 0.5f : w;
    }
    const float top = Rn(Sy(base)) - font->Ascent * k;
    const ImU32 col = C(rgba, g.a * a);
    // text-shadow #00000099 offset (0, -1) blur 2 (the quantity tags): one copy a pixel down at the shadow's mean strength
    const ImU32 sh = IM_COL32(0, 0, 0, (int)(0.6f * 0.55f * 255.0f * std::clamp(g.a * a, 0.0f, 1.0f)));
    const char* end = t + std::strlen(t);
    unsigned prev = 0;
    for (const char* p = t; p < end;) {
        unsigned c = 0;
        const int n = ImTextCharFromUtf8(&c, p, end);
        p += n > 0 ? n : 1;
        pen += Kern(f, prev, c, tab);
        prev = c;
        const ImFontGlyph* gl = font->FindGlyph((ImWchar)c);
        const float adv = gl ? gl->AdvanceX * k : 0.0f;
        const bool digit = tab && c >= '0' && c <= '9';
        const float gx = digit ? pen + (cell - adv) * 0.5f : pen;
        // x stays fractional (the atlas is oversampled 3x horizontally for it): snapping each pen to a whole pixel spaced words unevenly
        // ("Re stores", "PAC K" at 1:1); y is snapped so every baseline stays crisp.
        if (c != ' ') {
            if (shadow)
                font->RenderChar(g.dl, px, ImVec2(gx, Rn(top + g.s)), sh, (ImWchar)c);
            font->RenderChar(g.dl, px, ImVec2(gx, Rn(top)), col, (ImWchar)c);
        }
        pen += (digit ? cell : adv) + tr;
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Icons and the mannequin from the atlas
// ---------------------------------------------------------------------------------------------------------------------------------------------
void AtlasImage(const AtlasRect& r, float x0, float y0, float drawPx, ImU32 col)
{
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    if (r.rect < 0 || !atlas->TexID || r.px <= 0)
        return;
    const ImFontAtlasCustomRect* cr = atlas->GetCustomRectByIndex(r.rect);
    if (!cr || !cr->IsPacked())
        return;
    ImVec2 uv0, uv1;
    atlas->CalcCustomRectUV(cr, &uv0, &uv1);
    const float f = drawPx / (float)r.px;
    const ImVec2 p0(x0 - (float)r.pad * f, y0 - (float)r.pad * f);
    g.dl->AddImage(atlas->TexID, p0, ImVec2(p0.x + (float)cr->Width * f, p0.y + (float)cr->Height * f), uv0, uv1, col);
}

// glyph `ic` in the design box (x, y, size): colour rgba at opacity a; glow = the drop-shadow colour (0 = none).
void Icon(int ic, float x, float y, float size, uint32_t rgba, float a = 1.0f, uint32_t glow = 0)
{
    if (ic < 0 || ic >= B().iconCount)
        return;
    // the first bake at least this large; failing that the largest one (WantIcon bakes only the sizes the screen draws each glyph at)
    int si = -1;
    for (int i = 0; i < kIconSizeCount && si < 0; ++i)
        if (kIconSizes[i] >= size - 0.01f && B().icon[ic][i].rect >= 0) si = i;
    for (int i = kIconSizeCount - 1; i >= 0 && si < 0; --i)
        if (B().icon[ic][i].rect >= 0) si = i;
    if (si < 0)
        return;
    const float x0 = Rn(Sx(x)), y0 = Rn(Sy(y + size)), drawPx = size * g.s;
    if (glow) {
        int gi = -1;
        for (int i = 0; i < kGlowSizeCount && gi < 0; ++i)
            if (kGlowSizes[i] >= size - 0.01f && B().glow[ic][i].rect >= 0) gi = i;
        if (gi >= 0)
            AtlasImage(B().glow[ic][gi], x0, y0, drawPx, C(glow, g.a * a));   // AtlasImage scales the bake (and its pad) to the drawn size
    }
    // the first bake at least as large as the drawn size (34.56 for the drag ghost draws the 36 scaled down)
    AtlasImage(B().icon[ic][si], x0, y0, drawPx, C(rgba, g.a * a));
}
// 0.5.4: the weapon items' glyphs ("gun", "blade", "grenade": inventory.h WeaponCatalog) are drawn shapes (REV3 has no weapon PNGs), in the
// glyph's design box (x, y, size), unit coordinates with v up. false = not a weapon glyph.
bool WeaponGlyph(const char* name, float x, float y, float size, uint32_t rgba, float a)
{
    const bool gun = std::strcmp(name, "gun") == 0, blade = std::strcmp(name, "blade") == 0, grenade = std::strcmp(name, "grenade") == 0;
    if (!gun && !blade && !grenade)
        return false;
    const ImU32 col = C(rgba, g.a * a);
    const auto P = [&](float u, float v) { return ImVec2(Sx(x + u * size), Sy(y + v * size)); };
    const auto poly = [&](std::initializer_list<ImVec2> pts) {
        ImVec2 b[8];
        int n = 0;
        for (const ImVec2& p : pts)
            if (n < 8) b[n++] = p;
        g.dl->AddConvexPolyFilled(b, n, col);
    };
    if (gun) {
        poly({P(0.08f, 0.56f), P(0.92f, 0.56f), P(0.92f, 0.76f), P(0.08f, 0.76f)});   // slide and barrel
        poly({P(0.14f, 0.56f), P(0.40f, 0.56f), P(0.33f, 0.14f), P(0.08f, 0.14f)});   // grip
        const float t = std::max(1.0f, 0.06f * size * g.s);
        const ImVec2 guard[4] = {P(0.40f, 0.56f), P(0.42f, 0.40f), P(0.58f, 0.40f), P(0.60f, 0.56f)};
        g.dl->AddPolyline(guard, 4, col, 0, t);                                         // trigger guard
    } else if (blade) {
        poly({P(0.08f, 0.22f), P(0.22f, 0.08f), P(0.38f, 0.24f), P(0.24f, 0.38f)});    // handle
        poly({P(0.20f, 0.44f), P(0.44f, 0.20f), P(0.50f, 0.26f), P(0.26f, 0.50f)});    // guard
        poly({P(0.32f, 0.46f), P(0.46f, 0.32f), P(0.80f, 0.62f), P(0.92f, 0.92f), P(0.62f, 0.80f)});   // blade
    } else {
        g.dl->AddCircleFilled(P(0.46f, 0.40f), 0.30f * size * g.s, col, 24);           // body
        poly({P(0.35f, 0.64f), P(0.57f, 0.64f), P(0.57f, 0.80f), P(0.35f, 0.80f)});    // cap
        poly({P(0.57f, 0.80f), P(0.86f, 0.70f), P(0.86f, 0.62f), P(0.57f, 0.70f)});    // lever
        g.dl->AddCircle(P(0.26f, 0.84f), 0.08f * size * g.s, col, 12, std::max(1.0f, 0.05f * size * g.s));   // pin ring
    }
    return true;
}
void Icon(const char* name, float x, float y, float size, uint32_t rgba, float a = 1.0f, uint32_t glow = 0)
{
    const int ic = IconIndex(name);
    if (ic < 0 && name && WeaponGlyph(name, x, y, size, rgba, a))
        return;
    Icon(ic, x, y, size, rgba, a, glow);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Components (section 5)
// ---------------------------------------------------------------------------------------------------------------------------------------------
enum class Look { Empty, Filled, Hover, Selected, Source, Target, Deny, Locked, Ghost };
struct LookDef { uint32_t fill, border; float bw; bool dashed; uint32_t shadow; uint32_t glyph, glyphGlow; };
LookDef LookOf(Look l)
{
    switch (l) {
    case Look::Empty: return {0xF7F2E80A, 0xF7F2E81A, 1.5f, false, 0, kBone, 0};
    case Look::Filled: return {0xF7F2E812, 0xF7F2E829, 1.5f, false, 0, kBone, 0};
    case Look::Hover: return {0xF7F2E81F, 0xF7F2E857, 1.5f, false, 0, kCream, 0};
    case Look::Selected: return {0xF2B13424, 0xF2B13499, 1.5f, false, 0xF2B13459, kAmber, 0xF2B134CC};
    case Look::Source: return {0xF7F2E808, 0xF7F2E847, 1.5f, true, 0, kBone, 0};
    case Look::Target: return {0xF2B1341A, 0xF2B134B3, 1.5f, true, 0, kAmber, 0xF2B134CC};
    case Look::Deny: return {0xE5603A29, 0xE5603A99, 1.5f, false, 0xE5603A59, kEmber, 0xE5603ABF};
    case Look::Locked: return {0xF7F2E805, 0xF7F2E81F, 1.5f, true, 0, kMuted, 0};
    case Look::Ghost: return {0xF7F2E824, 0xF7F2E866, 1.62f, false, 0, kCream, 0};
    }
    return {0, 0, 1, false, 0, kBone, 0};
}
void SlotFrame(float x, float y, float size, float rad, Look l, float a = 1.0f)
{
    const LookDef d = LookOf(l);
    const Rc r = Box(x, y, size, size);
    if (d.shadow)
        Shadow(r, rad, d.shadow, 8.0f, 0.0f, a);
    Fill(r, rad, d.fill, a);
    if (d.dashed)
        Dashed(r, rad, d.border, d.bw, a);
    else
        Border(r, rad, d.border, d.bw, a);
}
// An item slot's contents: glyph (half the slot, centred), quantity tag, condition bar.
void SlotItem(float x, float y, float size, const inv::Slot& s, Look l, float a = 1.0f, uint32_t qtyOverride = 0)
{
    const inv::ItemDef* d = inv::Find(s.item);
    if (!d || s.Empty())
        return;
    const LookDef ld = LookOf(l);
    const float gs = size * 0.5f;
    Icon(d->glyph, x + size * 0.25f, y + size * 0.25f, gs, ld.glyph, a, ld.glyphGlow);
    const uint32_t q = qtyOverride ? qtyOverride : s.qty;
    std::string tag = inv::QtyTag(q);
    if (d->weapon) {
        // 0.5.4: a weapon is one item; its tag is the rounds it holds (a thrown weapon's count: "x5"), nothing for none
        char b[16] = {};
        if (inv::IsThrown(*d))
            tag = inv::QtyTag(s.ammo);
        else if (s.ammo > 0)
            std::snprintf(b, sizeof b, "%u", (unsigned)s.ammo), tag = b;
    }
    if (!tag.empty())
        Text(B13, x + size - 8.0f, y + 9.0f, kCream, tag.c_str(), 0.0f, kRight, true, a, true);
    if (d->condition && s.cond <= 99) {
        const float tw = size - 22.0f;
        Fill(Box(x + 11.0f, y + 7.0f, tw, 3.0f), 2.0f, kTrack, a);
        if (s.cond > 0)
            Fill(Box(x + 11.0f, y + 7.0f, std::max(1.0f, tw * (float)s.cond / 100.0f), 3.0f), 2.0f, s.cond <= 25 ? kEmber : kBone, a);
    }
}

enum class Chip { Amber, Bone, Ghost };
void ChipBox(float x, float y, float w, float h, Chip c, float a = 1.0f)
{
    const Rc r = Box(x, y, w, h);
    const float rad = h * 0.5f;
    switch (c) {
    case Chip::Amber: Fill(r, rad, 0xF2B13424, a); Border(r, rad, 0xF2B13499, 1.5f, a); break;
    case Chip::Bone: Fill(r, rad, 0xD9D1C31A, a); Border(r, rad, 0xD9D1C34D, 1.5f, a); break;
    case Chip::Ghost: Fill(r, rad, 0xF7F2E812, a); Border(r, rad, 0xF7F2E81F, 1.0f, a); break;
    }
}
uint32_t ChipText(Chip c) { return c == Chip::Amber ? kAmber : (c == Chip::Bone ? kBone : kMuted); }

void ButtonGhost(float x, float y, float w, float h, const char* label, float a = 1.0f, bool hot = false)
{
    const Rc r = Box(x, y, w, h);
    Fill(r, h * 0.5f, hot ? 0xF7F2E81F : 0xF7F2E812, a);
    Border(r, h * 0.5f, hot ? 0xF7F2E847 : 0xF7F2E824, 1.0f, a);
    if (label)
        Text(B13, x + w * 0.5f, y + 16.5f, kCream, label, 1.56f, kCenter, false, a);
}
void ButtonSolid(float x, float y, float w, float h, const char* label, bool enabled = true, bool hot = false)
{
    const Rc r = Box(x, y, w, h);
    if (!enabled) {
        // disabled: no glow, an amber ghost (a 35 % solid pill read as a muddy brown with dark text on it)
        Fill(r, h * 0.5f, 0xF7F2E80A);
        Border(r, h * 0.5f, 0xF2B13433, 1.0f);
        if (label)
            Text(B13, x + w * 0.5f, y + 16.5f, kAmber, label, 1.56f, kCenter, false, 0.35f);
        return;
    }
    const float a = 1.0f;
    Shadow(r, h * 0.5f, hot ? 0xF2B13480 : 0xF2B13459, 8.0f, 0.0f, a);
    Fill(r, h * 0.5f, hot ? 0xFFC75AFF : kAmber, a);
    if (label)
        Text(B13, x + w * 0.5f, y + 16.5f, kGround, label, 1.56f, kCenter, false, a);
}
void Panel(float x, float y, float w, float h, float rad, uint32_t fill)
{
    const Rc r = Box(x, y, w, h);
    Fill(r, rad, fill);
    Border(r, rad, kGlassBorder, 1.0f);
}
void Keycap(float x, float y, const char* label)   // h 22, padding 0 9 (+1 border), r7
{
    const float w = TextWDesign(B11, label, 0.88f) + 18.0f;
    const Rc r = Box(x, y, w, 22.0f);
    Fill(r, 7.0f, 0xF7F2E81F);
    Border(r, 7.0f, 0xF7F2E82E, 1.0f);
    Text(B11, x + w * 0.5f, y + 7.5f, kCream, label, 0.88f, kCenter);
}
float KeycapW(const char* label) { return TextWDesign(B11, label, 0.88f) + 18.0f; }

// ---------------------------------------------------------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------------------------------------------------------
enum class BoxK : uint8_t { None, Pockets, Pack, Equip, Quick, Hand /* 0.5.4 */, Near /* 0.5.4: the NEARBY box shown */ };
struct Ref {
    BoxK box = BoxK::None;
    int i = -1;
    bool Valid() const { return box != BoxK::None && i >= 0; }
    bool operator==(const Ref& o) const { return box == o.box && i == o.i; }
    bool operator!=(const Ref& o) const { return !(*this == o); }
};
inv::Where WhereOf(BoxK b)
{
    switch (b) {
    case BoxK::Pack: return inv::Where::Pack;
    case BoxK::Equip: return inv::Where::Equip;
    case BoxK::Quick: return inv::Where::Quick;
    case BoxK::Hand: return inv::Where::Hand;
    case BoxK::Near: return inv::Where::Nearby;
    default: return inv::Where::Pockets;
    }
}

struct Ui {
    // data
    bool have = false;                      // the server sent an inventory (false: this server keeps none, or none yet)
    inv::Character server, ch;              // the server's snapshot, and it + the ops it has not processed (what is drawn)
    struct Pending { uint32_t seq; inv::Op op; double at; };
    std::vector<Pending> pending;
    uint32_t seenSnap = ~0u;
    std::string name;
    std::string fallbackName;               // FrameIn::playerName: the name chip while the server has sent no inventory
    // interaction
    Ref hover, sel, press;
    ImVec2 pressPos;
    ImVec2 grab;                            // the press point inside the pressed slot (screen px from its top-left): where the ghost is held
    bool pressShift = false, pressAlt = false, pressCtrl = false;
    bool dragging = false;
    int dragMode = 0;                       // 0 whole, 1 half (Alt), 2 split (Ctrl)
    uint32_t dragQty = 0;
    Ref tipRef;
    double tipSince = 0.0;
    bool menuOpen = false;
    Ref menuRef;
    ImVec2 menuPos;                         // screen top-left
    int menuSub = -1;                       // the open submenu: 1 = BIND TO SLOT
    bool splitOpen = false;
    Ref splitSrc, splitDst;
    uint32_t splitN = 1;
    int splitChip = 0;                      // the fraction chip last chosen (-1 = the stepper set the amount): one chip lit even when fractions coincide
    int category = 0;                       // 0 ALL, 1 TOOLS, 2 FOOD, 3 MEDICAL, 4 DOCS
    char search[32] = {};
    bool searchFocus = false;
    uint32_t actQty = 1;
    double quickFlash[4] = {-10, -10, -10, -10};
    Ref denyRef;
    double denyAt = -10.0;
    double now = 0.0;
    bool clickEaten = false;                // a click this frame already did something (an overlay took it)
    int frame = 0, overlayFrame = -1;       // the frame a menu / dialog opened: the click that opened it is not also a click inside it
};
Ui& U() { static Ui* u = new Ui; return *u; }

// 0.5.4 inventory world: the server's NEARBY boxes and their prediction (what is drawn: the server's + the pending ops on top, like Ui::ch)
struct NearC {
    InvNearbyBox meta;
    inv::Container box;
    bool ok = false;                        // `meta.encoded` decoded (a locked box has none)
};
struct NearUi {
    bool world = false;                     // SetInventoryWorld: the hand, weapons and NEARBY are live
    std::vector<NearC> server, pred;
    uint32_t seen = ~0u;
    int tab = -1;                           // the tab the player picked: 0 VEHICLE, 1 GROUND (-1 = none yet: the vehicle when there is one)
};
NearUi& NU() { static NearUi* n = new NearUi; return *n; }
constexpr uint8_t kKindTrunk = (uint8_t)inv::Kind::Trunk, kKindGround = (uint8_t)inv::Kind::Ground;
// The box of `kind` in the predicted list (null = none in reach).
NearC* NearOf(uint8_t kind)
{
    for (NearC& n : NU().pred)
        if (n.meta.kind == kind)
            return &n;
    return nullptr;
}
// The box the NEARBY panel shows: the picked tab's, else the other one.
NearC* NearShown()
{
    NearUi& n = NU();
    NearC* v = NearOf(kKindTrunk);
    NearC* gr = NearOf(kKindGround);
    if (n.tab == 1) return gr ? gr : v;
    if (n.tab == 0) return v ? v : gr;
    return v ? v : gr;
}
// The open (unlocked, decoded) container with `id` in a list, for ApplyEx.
inv::Container* NearBox(std::vector<NearC>& list, uint32_t id)
{
    for (NearC& n : list)
        if (n.meta.id == id && n.ok && !n.meta.locked)
            return &n.box;
    return nullptr;
}
inv::ApplyOut ApplyNear(inv::Character& ch, std::vector<NearC>& list, const inv::Op& op)
{
    inv::Container* s = op.src == inv::Where::Nearby ? NearBox(list, op.srcBox) : nullptr;
    inv::Container* d = op.dst == inv::Where::Nearby ? NearBox(list, op.dstBox) : nullptr;
    return inv::ApplyEx(ch, op, s, d);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Data: the store -> the prediction; ops out
// ---------------------------------------------------------------------------------------------------------------------------------------------
void Sync()
{
    Ui& u = U();
    Store& s = St();
    bool fresh = false, nearFresh = false;
    uint32_t ack = 0;
    {
        std::lock_guard<std::mutex> lk(s.m);
        if (s.snapSeq != u.seenSnap) {
            u.seenSnap = s.snapSeq;
            fresh = true;
            u.name = s.name;
            if (!s.available) {
                u.have = false;
                u.pending.clear();   // the session that would have answered them is gone
            } else {
                inv::Character c;
                if (inv::Decode(s.snapshot, c)) {
                    u.server = c;
                    u.have = true;
                } else {
                    GLOGW("inventory: the server's snapshot did not decode (%zu bytes); keeping the last one", s.snapshot.size());
                }
            }
            ack = s.ack;
        }
        // 0.5.4: the NEARBY boxes
        if (s.nearSeq != NU().seen) {
            NearUi& n = NU();
            n.seen = s.nearSeq;
            n.world = s.world;
            n.server.clear();
            for (const InvNearbyBox& b : s.boxes) {
                NearC c;
                c.meta = b;
                c.meta.encoded.clear();
                c.ok = !b.locked && inv::DecodeBox(b.encoded, c.box);
                n.server.push_back(std::move(c));
            }
            nearFresh = true;
        }
    }
    // a prediction the server never answered (3 s) is dropped: the server's state is the truth
    bool stale = false;
    for (const Ui::Pending& p : u.pending)
        stale |= u.now - p.at > 3.0;
    if (!fresh && !stale && !nearFresh)
        return;
    if (fresh)
        u.pending.erase(std::remove_if(u.pending.begin(), u.pending.end(), [&](const Ui::Pending& p) { return p.seq <= ack; }), u.pending.end());
    if (stale)
        u.pending.clear();
    u.ch = u.server;
    NU().pred = NU().server;
    for (const Ui::Pending& p : u.pending)
        ApplyNear(u.ch, NU().pred, p.op);
}

void Deny(Ref r)
{
    U().denyRef = r;
    U().denyAt = U().now;
}

// Apply to the prediction and queue for the server. A refused op is not sent (the slot flashes denied).
bool Send(const inv::Op& op, Ref flash = Ref())
{
    Ui& u = U();
    if (!u.have)
        return false;
    inv::Character t = u.ch;
    std::vector<NearC> tn = NU().pred;
    const inv::ApplyOut r = ApplyNear(t, tn, op);
    if (!inv::Ok(r.r)) {
        Deny(flash);
        return false;
    }
    u.ch = t;
    NU().pred = std::move(tn);
    Store& s = St();
    uint32_t seq;
    {
        std::lock_guard<std::mutex> lk(s.m);
        seq = s.nextSeq++;
        s.out.emplace_back(seq, inv::EncodeOp(op));
    }
    u.pending.push_back({seq, op, u.now});
    return true;
}
inv::Op MakeOp(inv::OpKind k, Ref src, Ref dst = Ref(), uint32_t qty = 0)
{
    inv::Op o;
    o.kind = k;
    o.src = WhereOf(src.box);
    o.si = (int16_t)src.i;
    o.dst = WhereOf(dst.box);
    o.di = (int16_t)dst.i;
    o.qty = qty;
    // 0.5.4: a Nearby side names the box the panel shows; the hand is index 0
    if (src.box == BoxK::Near || dst.box == BoxK::Near) {
        const NearC* n = NearShown();
        if (src.box == BoxK::Near) o.srcBox = n ? n->meta.id : 0;
        if (dst.box == BoxK::Near) o.dstBox = n ? n->meta.id : 0;
    }
    if (src.box == BoxK::Hand) o.si = 0;
    if (dst.box == BoxK::Hand) o.di = 0;
    return o;
}
bool Allowed(const inv::Op& op)
{
    inv::Character t = U().ch;
    std::vector<NearC> tn = NU().pred;
    return inv::Ok(ApplyNear(t, tn, op).r);
}

const inv::Slot* SlotOf(Ref r)
{
    Ui& u = U();
    if (r.box == BoxK::Quick || !r.Valid())
        return nullptr;
    if (r.box == BoxK::Near) {   // 0.5.4
        const NearC* n = NearShown();
        return n && n->ok && !n->meta.locked && r.i < (int)n->box.slots.size() ? &n->box.slots[(size_t)r.i] : nullptr;
    }
    return inv::SlotAt(u.ch, WhereOf(r.box), r.i);
}
bool Filled(Ref r) { const inv::Slot* s = SlotOf(r); return s && !s->Empty(); }

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Layout (section 4): where every slot is
// ---------------------------------------------------------------------------------------------------------------------------------------------
constexpr float kGridX = 571.0f, kPocketY0 = 725.0f, kPackY0 = 523.0f, kPitch = 74.0f, kSlot = 64.0f;
// 0.5.4: the HAND pill in the INVENTORY header (between the title and the weight pill) and the NEARBY grids (panel_right_*.txt)
constexpr float kHandX = 761.0f, kHandY = 911.0f, kHandW = 144.0f, kHandH = 40.0f;
constexpr float kNearGridX = 1243.0f, kNearTrunkY0 = 687.0f, kNearGroundY0 = 699.0f;
void SlotPos(Ref r, float& x, float& y, float& size)
{
    size = kSlot;
    switch (r.box) {
    case BoxK::Pockets: x = kGridX + kPitch * (float)(r.i % 8); y = kPocketY0 - kPitch * (float)(r.i / 8); break;
    case BoxK::Pack: x = kGridX + kPitch * (float)(r.i % 8); y = kPackY0 - kPitch * (float)(r.i / 8); break;
    case BoxK::Equip: x = r.i < 7 ? 97.0f : 417.0f; y = 823.0f - 78.0f * (float)(r.i % 7); break;
    case BoxK::Quick: x = 110.0f + 96.0f * (float)r.i; y = 214.0f; size = 72.0f; break;
    case BoxK::Hand: x = kHandX; y = kHandY; size = kHandH; break;   // 0.5.4: the HAND pill's left square (the hit test takes the whole pill)
    case BoxK::Near: {                                                // 0.5.4: the NEARBY grid (a trunk under its card, a pile under its title)
        const NearC* n = NearShown();
        const float y0 = n && n->meta.kind == kKindGround ? kNearGroundY0 : kNearTrunkY0;
        x = kNearGridX + kPitch * (float)(r.i % 8);
        y = y0 - kPitch * (float)(r.i / 8);
        break;
    }
    default: x = y = 0.0f; break;
    }
}
Ref HitSlot(ImVec2 d)
{
    Ui& u = U();
    const auto test = [&](BoxK b, int n) -> Ref {
        for (int i = 0; i < n; ++i) {
            float x, y, sz;
            SlotPos(Ref{b, i}, x, y, sz);
            if (In(d, x, y, sz, sz))
                return Ref{b, i};
        }
        return Ref();
    };
    Ref r = test(BoxK::Pockets, (int)u.ch.pockets.slots.size());
    if (!r.Valid() && u.ch.HasPack()) r = test(BoxK::Pack, (int)u.ch.pack.slots.size());
    if (!r.Valid()) r = test(BoxK::Equip, inv::kEquipCount);
    if (!r.Valid()) r = test(BoxK::Quick, 4);
    // 0.5.4: the HAND pill and the NEARBY box shown (a locked trunk's slots too: a drop there is refused, visibly)
    if (!r.Valid() && NU().world && In(d, kHandX, kHandY, kHandW, kHandH))
        r = Ref{BoxK::Hand, 0};
    if (!r.Valid() && NU().world)
        if (const NearC* n = NearShown()) {
            const int count = n->ok ? (int)n->box.slots.size() : (n->meta.kind == kKindTrunk ? inv::StorageForClass(n->meta.cls).trunkSlots : 0);
            r = test(BoxK::Near, std::min(count, 48));
        }
    return r;
}

bool MatchesFilter(const inv::Slot& s)
{
    Ui& u = U();
    const inv::ItemDef* d = inv::Find(s.item);
    if (!d)
        return true;
    static const inv::Category kCat[5] = {inv::Category::Tools, inv::Category::Tools, inv::Category::Food, inv::Category::Medical, inv::Category::Docs};
    if (u.category > 0 && d->cat != kCat[u.category])
        return false;
    if (u.search[0]) {
        std::string hay = d->name, q = u.search;
        for (char& c : hay) c = (char)std::tolower((unsigned char)c);
        for (char& c : q) c = (char)std::tolower((unsigned char)c);
        if (hay.find(q) == std::string::npos)
            return false;
    }
    return true;
}

// What the dragged item would do on a slot: the slot's look while dragging.
inv::Op DropOp(Ref src, Ref dst)
{
    Ui& u = U();
    if (src.box == BoxK::Quick)
        return MakeOp(inv::OpKind::Move, src, dst);
    if (dst.box == BoxK::Quick)
        return MakeOp(inv::OpKind::Bind, src, dst);
    const uint32_t q = u.dragMode == 1 ? u.dragQty : 0;
    return MakeOp(inv::OpKind::Move, src, dst, q);
}
bool DropAllowed(Ref src, Ref dst)
{
    if (!dst.Valid() || src == dst)
        return false;
    if ((src.box == BoxK::Quick) != (dst.box == BoxK::Quick) && src.box == BoxK::Quick)
        return false;
    if (U().dragMode == 2) {   // Ctrl: a split into an EMPTY container slot
        const inv::Slot* d = SlotOf(dst);
        const inv::Slot* s = SlotOf(src);
        return d && d->Empty() && s && s->qty >= 2 && src.box != BoxK::Equip && dst.box != BoxK::Equip && dst.box != BoxK::Quick;
    }
    return Allowed(DropOp(src, dst));
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------------------------------------------------------------------------
// The first empty slot for a split of src: its own container first, then the other one.
Ref SplitTarget(Ref src)
{
    Ui& u = U();
    const BoxK order[2] = {src.box, src.box == BoxK::Pockets ? BoxK::Pack : BoxK::Pockets};
    for (BoxK b : order) {
        const inv::Container* c = inv::BoxOf(u.ch, WhereOf(b));
        if (!c) continue;
        for (size_t i = 0; i < c->slots.size(); ++i)
            if (c->slots[i].Empty()) return Ref{b, (int)i};
    }
    return Ref();
}
bool CanSplit(Ref src)
{
    const inv::Slot* s = SlotOf(src);
    return s && s->qty >= 2 && (src.box == BoxK::Pockets || src.box == BoxK::Pack) && SplitTarget(src).Valid();
}
void OpenSplit(Ref src, Ref dst)
{
    Ui& u = U();
    const inv::Slot* s = SlotOf(src);
    if (!s || s->qty < 2 || !dst.Valid())
        return;
    u.splitOpen = true;
    u.splitSrc = src;
    u.splitDst = dst;
    u.splitN = inv::SplitAmount(s->qty, 1, 2);
    u.splitChip = 0;
    u.menuOpen = false;
    u.overlayFrame = u.frame;
}
const char* UseLabel(Ref r)
{
    if (r.box == BoxK::Equip)
        return "TAKE OFF";
    if (r.box == BoxK::Hand)
        return "HOLSTER";   // 0.5.4
    if (r.box == BoxK::Near)
        return "TAKE";      // 0.5.4: off the ground / out of the trunk
    const inv::Slot* s = SlotOf(r);
    if (s && inv::Wearable(s->item))
        return "EQUIP";
    if (s && inv::IsWeapon(s->item))
        return "HOLD";      // 0.5.4: into the hand
    return "USE";
}
bool CanUse(Ref r)
{
    const inv::Slot* s = SlotOf(r);
    if (!s || s->Empty())
        return false;
    if (r.box == BoxK::Near)
        return Allowed(MakeOp(inv::OpKind::QuickMove, r));
    return Allowed(MakeOp(inv::OpKind::Use, r));
}
void DoUse(Ref r)
{
    if (r.box == BoxK::Near) {   // 0.5.4: TAKE
        Send(MakeOp(inv::OpKind::QuickMove, r), r);
        return;
    }
    if (r.box == BoxK::Quick) {
        U().quickFlash[r.i] = U().now;
        Send(MakeOp(inv::OpKind::UseQuick, r), r);
        return;
    }
    Send(MakeOp(inv::OpKind::Use, r), r);
}
void DoDrop(Ref r, uint32_t qty) { Send(MakeOp(inv::OpKind::Drop, r, Ref(), qty), r); }
void DoBind(Ref r, int k)
{
    Ui& u = U();
    const inv::Slot* s = SlotOf(r);
    if (!s || s->Empty() || k < 0 || k > 3)
        return;
    if (u.ch.quick[k].item == s->item)
        Send(MakeOp(inv::OpKind::Unbind, Ref(), Ref{BoxK::Quick, k}));
    else
        Send(MakeOp(inv::OpKind::Bind, r, Ref{BoxK::Quick, k}), r);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Drawing: the panels
// ---------------------------------------------------------------------------------------------------------------------------------------------
Look SlotLookNow(Ref r, bool filled)
{
    Ui& u = U();
    if (u.denyRef == r && u.now - u.denyAt < 0.35)
        return Look::Deny;
    if (!u.have)
        return Look::Locked;
    if (u.dragging) {
        if (u.press == r)
            return Look::Source;
        if (u.hover == r)
            return DropAllowed(u.press, r) ? Look::Target : Look::Deny;
        // section 8: "slots that accept the item light up as targets": every empty slot it may go to, and every stack of the same item it
        // would merge into (a swap with another item is allowed on hover but is not an "accepting" slot, or the whole grid would light)
        const inv::Slot* ds = SlotOf(u.press);
        const inv::Slot* rs = SlotOf(r);
        if (ds && rs && (rs->Empty() || rs->item == ds->item) && DropAllowed(u.press, r))
            return Look::Target;
        return filled ? Look::Filled : Look::Empty;
    }
    if (u.sel == r && filled)
        return Look::Selected;
    if (u.hover == r && filled && !u.menuOpen && !u.splitOpen)
        return Look::Hover;
    return filled ? Look::Filled : Look::Empty;
}

void DrawGridSlot(Ref r, const inv::Slot& s)
{
    float x, y, sz;
    SlotPos(r, x, y, sz);
    const bool filled = !s.Empty();
    Look l = SlotLookNow(r, filled);
    // the search / category filter: the frame stays an empty slot's (the grid keeps reading as one lattice), only the glyph and tag dim
    const bool dimmed = filled && !MatchesFilter(s) && (l == Look::Filled || l == Look::Hover);
    if (dimmed)
        l = Look::Empty;
    // the drag ghost sits over the hovered slot and carries its state (DrawGhost): nothing of the slot is drawn under it (its frame made a
    // second ring next to the ghost's, its glyph showed through)
    if (U().dragging && U().hover == r && U().press != r)
        return;
    SlotFrame(x, y, sz, 16.0f, l, l == Look::Source ? 0.55f : 1.0f);
    if (l == Look::Target && !filled)
        Icon("plus", x + 20.5f, y + 20.5f, 23.0f, kAmber, 0.85f, 0xF2B134CC);
    else if (filled)
        SlotItem(x, y, sz, s, l, dimmed ? 0.25f : (l == Look::Source ? 0.55f : 1.0f));
}

void DrawSectionHead(float base, const char* label, uint64_t grams, uint64_t cap, bool none = false)
{
    Text(B12, kGridX, base, kMuted, label, 2.4f);
    const Rc track = Box(kGridX, base - 15.0f, 582.0f, 4.0f);
    if (none) {
        Text(B12, 1153.0f, base, kMuted, "NONE EQUIPPED", 1.68f, kRight);
        return;
    }
    if (!U().have) {
        // the server sent no inventory: the weight is unknown, never "0.0" (the spec's unknown style)
        const std::string unk = "\xE2\x80\x94 / " + inv::KgCap(cap) + " KG";
        Text(B12, 1153.0f, base, kMuted, unk.c_str(), 1.68f, kRight);
        Fill(track, 2.0f, kTrack);
        return;
    }
    const inv::Load ld = inv::LoadOf(grams, cap);
    const bool heavy = ld != inv::Load::Normal;
    char t[48];
    std::snprintf(t, sizeof t, "%s / %s KG", inv::Kg(grams).c_str(), inv::KgCap(cap).c_str());
    Text(B12, 1153.0f, base, heavy ? kEmber : kBone, t, 1.2f, kRight);
    Fill(track, 2.0f, kTrack);
    const float frac = cap ? std::min(1.0f, (float)grams / (float)cap) : 0.0f;
    if (frac > 0.0f) {
        const float pulse = ld == inv::Load::Full ? 0.7f + 0.3f * std::cos((float)U().now * kPi / 0.8f) : 1.0f;   // 1.0 <-> 0.4 over 0.8 s
        Fill(Box(kGridX, base - 15.0f, std::max(2.0f, 582.0f * frac), 4.0f), 2.0f, heavy ? kEmber : kBone, pulse);
    }
}

void DrawWeightPill()
{
    Ui& u = U();
    const uint64_t carried = u.ch.Carried(), cap = u.ch.CarryCap();
    const inv::Load ld = inv::LoadOf(carried, cap);
    const bool heavy = ld != inv::Load::Normal;
    const float x = 915.0f, y = 911.0f;
    const Rc r = Box(x, y, 238.0f, 40.0f);
    if (heavy) {
        Fill(r, 20.0f, 0xE5603A1A);
        Border(r, 20.0f, 0xE5603A80, 1.5f);
    } else {
        Fill(r, 20.0f, 0xF7F2E80F);
        Border(r, 20.0f, 0xF7F2E824, 1.0f);
    }
    Icon("weight", x + 15.0f, y + 10.0f, 18.0f, heavy ? kEmber : kBone);
    if (!u.have) {
        Text(B20, x + 43.0f, y + 13.0f, kMuted, "\xE2\x80\x94");
        const std::string capT = "/ " + inv::KgCap(cap) + " KG";
        Text(B12, x + 43.0f + TextWDesign(B20, "\xE2\x80\x94") + 5.0f, y + 13.0f, kMuted, capT.c_str(), 1.2f);
        Fill(Box(x + 159.0f, y + 16.0f, 66.0f, 6.0f), 3.0f, kTrack);
        return;
    }
    const std::string num = inv::Kg(carried);
    Text(B20, x + 43.0f, y + 13.0f, heavy ? kEmber : kCream, num.c_str(), 0.0f, kLeft, true);
    const std::string capT = "/ " + inv::KgCap(cap) + " KG";
    Text(B12, x + 43.0f + TextWDesign(B20, num.c_str(), 0.0f, true) + 5.0f, y + 13.0f, kMuted, capT.c_str(), 1.2f);
    Fill(Box(x + 159.0f, y + 16.0f, 66.0f, 6.0f), 3.0f, kTrack);
    const float frac = cap ? std::min(1.0f, (float)carried / (float)cap) : 0.0f;
    if (frac > 0.0f) {
        const float pulse = ld == inv::Load::Full ? 0.7f + 0.3f * std::cos((float)u.now * kPi / 0.8f) : 1.0f;
        Fill(Box(x + 159.0f, y + 16.0f, std::max(3.0f, 66.0f * frac), 6.0f), 3.0f, heavy ? kEmber : kBone, pulse);
    }
}

// ---- 0.5.4 inv_ped_view: the CHARACTER panel with a window onto the player's own ped --------------------------------------------------------------
// While the engine's scripted camera renders (Overlay::SetInventoryPedView), the panel and the world dim leave out the window
// (libs/sync/ped_view.h kHole*): the game draws the ped there. Otherwise (switch off, camera not rendering yet, in a vehicle, dead) the panel is
// the 0.6 one with the mannequin.
namespace pv = gage::sync::pedview;
bool g_pedViewThisFrame = false;   // read once per frame (DrawScreen): the dim and the panel agree even if the engine flips it mid-frame
bool PedViewLive() { return g_pedViewThisFrame; }
Rc HoleRc() { return Box(pv::kHoleX, pv::kHoleY, pv::kHoleW, pv::kHoleH); }
// The corner square at `corner` minus the quarter disc of radius r around `centre` (screen px), from angle a0 over 90 degrees (RRPath's
// order): fills the window's rounded corner with the panel (or the dim) around it.
void Fillet(ImVec2 corner, ImVec2 centre, float r, float a0, ImU32 col)
{
    constexpr int kSeg = 12;
    const ImVec2 uv = ImGui::GetDrawListSharedData()->TexUvWhitePixel;
    ImDrawList* dl = g.dl;
    dl->PrimReserve(kSeg * 3, kSeg + 2);
    const unsigned base = dl->_VtxCurrentIdx;
    dl->PrimWriteVtx(corner, uv, col);
    for (int j = 0; j <= kSeg; ++j) {
        const float t = a0 + 0.5f * kPi * (float)j / (float)kSeg;
        dl->PrimWriteVtx(ImVec2(centre.x + r * std::cos(t), centre.y + r * std::sin(t)), uv, col);
    }
    for (int j = 0; j < kSeg; ++j) {
        dl->PrimWriteIdx((ImDrawIdx)base);
        dl->PrimWriteIdx((ImDrawIdx)(base + 1 + j));
        dl->PrimWriteIdx((ImDrawIdx)(base + 2 + j));
    }
}
void HoleFillets(const Rc& h, ImU32 col)
{
    const float r = std::min(pv::kHoleR * g.s, std::min(h.b.x - h.a.x, h.b.y - h.a.y) * 0.5f);
    Fillet(h.a, ImVec2(h.a.x + r, h.a.y + r), r, kPi, col);
    Fillet(ImVec2(h.b.x, h.a.y), ImVec2(h.b.x - r, h.a.y + r), r, 1.5f * kPi, col);
    Fillet(h.b, ImVec2(h.b.x - r, h.b.y - r), r, 0.0f, col);
    Fillet(ImVec2(h.a.x, h.b.y), ImVec2(h.a.x + r, h.b.y - r), r, 0.5f * kPi, col);
}
// The world dim over the whole display, the window left out.
void DrawDim(const FrameIn& in)
{
    const ImU32 col = C(kDim, in.a);
    if (!PedViewLive()) {
        g.dl->AddRectFilled(ImVec2(0, 0), ImVec2(in.W, in.H), col);
        return;
    }
    const Rc h = HoleRc();
    g.dl->AddRectFilled(ImVec2(0, 0), ImVec2(in.W, h.a.y), col);
    g.dl->AddRectFilled(ImVec2(0, h.b.y), ImVec2(in.W, in.H), col);
    g.dl->AddRectFilled(ImVec2(0, h.a.y), ImVec2(h.a.x, h.b.y), col);
    g.dl->AddRectFilled(ImVec2(h.b.x, h.a.y), ImVec2(in.W, h.b.y), col);
    HoleFillets(h, col);
}
// The panel glass around the window (four bands + the window's corners), its outer border, and the window's light tint and border.
void PanelWithWindow()
{
    const Rc p = Box(pv::kPanelX, pv::kPanelY, pv::kPanelW, pv::kPanelH);
    const Rc h = HoleRc();
    const ImU32 col = C(kPanelGlass, g.a);
    const float rad = 32.0f * g.s;
    g.dl->AddRectFilled(p.a, ImVec2(p.b.x, h.a.y), col, rad, ImDrawFlags_RoundCornersTop);
    g.dl->AddRectFilled(ImVec2(p.a.x, h.b.y), p.b, col, rad, ImDrawFlags_RoundCornersBottom);
    g.dl->AddRectFilled(ImVec2(p.a.x, h.a.y), ImVec2(h.a.x, h.b.y), col);
    g.dl->AddRectFilled(ImVec2(h.b.x, h.a.y), ImVec2(p.b.x, h.b.y), col);
    HoleFillets(h, col);
    Border(p, 32.0f, kGlassBorder, 1.0f);
    Fill(h, pv::kHoleR, 0x0A09071F);   // a light tint: the ped reads as part of the panel, the world behind it stays visible
    Border(h, pv::kHoleR, kGlassBorder, 1.0f);
}

void DrawCharacter()
{
    Ui& u = U();
    const bool pedView = PedViewLive();
    if (pedView)
        PanelWithWindow();
    else
        Panel(64, 172, 448, 800, 32, kPanelGlass);
    Text(B28, 97, 923, kCream, "CHARACTER", 0.28f);
    {
        std::string nm = !u.name.empty() ? u.name : (!u.fallbackName.empty() ? u.fallbackName : std::string("PLAYER"));
        for (char& c : nm) c = (char)std::toupper((unsigned char)c);
        const float tw = TextWDesign(B12, nm.c_str(), 1.44f);
        const float w = std::max(150.0f, std::min(280.0f, tw + 30.0f));
        ChipBox(481.0f - w, 914, w, 34, Chip::Ghost);
        Text(B12, 481.0f - w * 0.5f, 928, kMuted, nm.c_str(), 1.44f, kCenter);
    }
    if (!pedView) {   // the 0.6 mannequin and its halo; with the ped view the game draws the real ped in the window
        RadialGlow(139, 477, 300, 0xF2B13421);
        const Rc m = Box(177, 355, 224, 532);
        if (B().manikin.rect >= 0)
            AtlasImage(B().manikin, m.a.x, m.a.y, (m.b.x - m.a.x), C(kCream, g.a * 0.085f));
    }
    // equipment
    for (int i = 0; i < inv::kEquipCount; ++i) {
        const Ref r{BoxK::Equip, i};
        float x, y, sz;
        SlotPos(r, x, y, sz);
        const inv::Slot& s = u.ch.equip[i];
        const inv::Equip e = inv::EquipAt(i);
        const bool worn = !s.Empty();
        const bool isPack = e == inv::Equip::Pack;
        Look l = SlotLookNow(r, worn);
        // while dragging, every slot the item is worn in shows "fits" (equip.item-fits); hovering another shows denied
        if (u.dragging && u.press != r && u.press.box != BoxK::Quick) {
            const inv::Slot* ds = SlotOf(u.press);
            const inv::ItemDef* dd = ds ? inv::Find(ds->item) : nullptr;
            if (dd && dd->equip == e && u.press.box != BoxK::Equip)
                l = u.hover == r && !DropAllowed(u.press, r) ? Look::Deny : Look::Target;
            else if (u.hover == r)
                l = Look::Deny;
        }
        // a worn pack with something in it cannot come off: denied while hovered (section 8)
        if (!u.dragging && isPack && worn && u.hover == r && inv::UsedSlots(u.ch.pack) != 0)
            l = Look::Deny;
        const char* glyph = inv::EquipGlyph(e);
        const char* cap = inv::EquipCaption(e);
        if (u.dragging && u.hover == r && u.press != r)
            continue;   // under the drag ghost, which carries the state
        if (isPack && !worn && (l == Look::Empty || l == Look::Hover)) {
            const Rc rc = Box(x, y, 64, 64);
            Fill(rc, 16, 0xF2B1340D);
            Dashed(rc, 16, 0xF2B13473, 1.5f);
            Icon(glyph, x + 19, y + 26, 28, kAmber, 0.6f);
            Text(B10, x + 33, y + 10, 0xF2B134BF, cap, 1.0f, kCenter);
            continue;
        }
        SlotFrame(x, y, 64, 16, l, l == Look::Source ? 0.55f : 1.0f);
        const LookDef ld = LookOf(l);
        if (worn) {
            const inv::ItemDef* d = inv::Find(s.item);
            if (isPack) {
                Icon(d ? d->glyph : glyph, x + 19, y + 28, 28, ld.glyph, 1.0f, ld.glyphGlow);
                char tag[16];
                std::snprintf(tag, sizeof tag, "%d SLOTS", u.ch.PackTierNow());
                // under the 9 px text floor (720p) the tag loses its tracking and most of its padding and never outgrows the slot (it covered
                // both border strokes)
                const bool floored = CssPx(10.0f, g.s) > 10.0f * g.s + 0.01f;
                const float track = floored ? 0.0f : 0.6f;
                const float tw = std::min(floored ? 61.0f : 58.0f, TextWDesign(B10, tag, track) + (floored ? 8.0f : 14.0f));
                Fill(Box(x + 32.0f - tw * 0.5f, y + 6.0f, tw, 16.0f), 8.0f, kAmber);
                Text(B10, x + 32.0f, y + 11.0f, kGround, tag, track, kCenter);
            } else {
                Icon(d ? d->glyph : glyph, x + 17, y + 15, 32, ld.glyph, l == Look::Source ? 0.55f : 1.0f, ld.glyphGlow);
                if (d && d->condition && s.cond <= 99) {
                    Fill(Box(x + 11, y + 7, 42, 3), 2, kTrack);
                    if (s.cond > 0) Fill(Box(x + 11, y + 7, std::max(1.0f, 42.0f * s.cond / 100.0f), 3), 2, s.cond <= 25 ? kEmber : kBone);
                }
            }
        } else {
            const bool st = l == Look::Target || l == Look::Deny;
            Icon(glyph, x + 20, y + 28, 26, st ? ld.glyph : kMuted, st ? 1.0f : 0.45f, st ? ld.glyphGlow : 0);
            Text(B10, x + 33, y + 10, st ? ld.glyph : 0xA9A295CC, cap, 1.0f, kCenter);
        }
    }
    // quick slots
    Text(B12, 97, 314, kMuted, "QUICK SLOTS", 2.4f);
    Text(B12, 481, 314, kMuted, "PRESS 1 \xE2\x80\x93 4", 2.4f, kRight);
    {
        const Rc pill = Box(97, 203, 384, 96);
        Fill(pill, 48, kCardFill);
        Border(pill, 48, kCardBorder, 1.0f);
    }
    const bool binding = u.dragging && (u.press.box == BoxK::Pockets || u.press.box == BoxK::Pack) && u.dragMode != 2;
    for (int k = 0; k < 4; ++k) {
        const Ref r{BoxK::Quick, k};
        float x, y, sz;
        SlotPos(r, x, y, sz);
        inv::QuickRef q = u.ch.quick[k];
        const bool bound = q.item != 0;
        const bool live = bound && inv::Resolve(u.ch, q);
        const inv::Slot* s = live ? inv::SlotAt(u.ch, q.kind == inv::Kind::Pack ? inv::Where::Pack : inv::Where::Pockets, q.index) : nullptr;
        const bool pressed = u.now - u.quickFlash[k] < 0.15;
        Look l;
        if (pressed)
            l = Look::Selected;
        else if (binding)
            l = Look::Target;
        else if (u.dragging && u.press.box == BoxK::Quick)
            l = u.press == r ? Look::Source : (u.hover == r ? Look::Target : (bound ? Look::Filled : Look::Empty));
        else if (u.denyRef == r && u.now - u.denyAt < 0.35)
            l = Look::Deny;
        else if (live)
            l = u.hover == r && !u.menuOpen && !u.splitOpen ? Look::Hover : Look::Filled;
        else
            l = Look::Empty;
        if (binding && u.hover == r)
            l = Look::Target;
        if (!u.have)
            l = Look::Locked;
        if (u.dragging && u.hover == r && u.press != r)
            continue;   // under the drag ghost, which carries the state
        SlotFrame(x, y, 72, 18, l, l == Look::Source ? 0.55f : 1.0f);
        const LookDef ld = LookOf(l);
        char key[2] = {(char)('1' + k), 0};
        if (l == Look::Locked) {
            Text(B12, x + 9.0f, y + 56.0f, 0xF7F2E859, key);
            continue;
        }
        if (bound) {
            const inv::ItemDef* d = inv::Find(q.item);
            if (d) {
                const uint32_t glyphCol = (l == Look::Target || l == Look::Empty) ? kBone : ld.glyph;
                Icon(d->glyph, x + 18, y + 18, 36, glyphCol, live ? 1.0f : 0.35f, l == Look::Selected ? ld.glyphGlow : 0);
            }
            if (s) {
                const std::string tag = inv::QtyTag(s->qty);
                if (!tag.empty())
                    Text(B13, x + 64.0f, y + 9.0f, kCream, tag.c_str(), 0.0f, kRight, true, 1.0f, true);
            }
        } else if (l != Look::Target) {
            Icon("plus", x + 18, y + 18, 36, kMuted, 0.55f);
        } else {
            Icon("plus", x + 18, y + 18, 36, kAmber, 0.85f, 0xF2B134CC);
        }
        Text(B12, x + 9.0f, y + 56.0f, pressed ? 0xF2B134F2 : 0xF7F2E88C, key);
    }
}

void DrawDetailCard()
{
    Ui& u = U();
    const float cx = 571, cy = 281;
    const Rc card = Box(cx, cy, 582, 150);
    Fill(card, 24, kCardFill);
    Border(card, 24, kCardBorder, 1.0f);
    const inv::Slot* s = u.sel.Valid() ? SlotOf(u.sel) : nullptr;
    const inv::ItemDef* d = s && !s->Empty() ? inv::Find(s->item) : nullptr;
    if (!d) {
        // a dashed, muted tile (a plain empty tile read as a missing image)
        SlotFrame(592, 338, 72, 18, Look::Locked);
        Text(B24, 684, 389, kMuted, "Nothing selected");
        if (u.have)
            Text(M14, 684, 360, kMuted, "Click an item to see it here.");
        return;
    }
    SlotFrame(592, 338, 72, 18, Look::Selected);
    Icon(d->glyph, 610, 356, 36, kAmber, 1.0f, 0xF2B134CC);
    Text(B24, 684, 389, kCream, d->name);
    {
        const char* cat = inv::CategoryName(d->cat);
        const float w = std::round(TextWDesign(B10, cat, 1.4f) + 26.66f);
        ChipBox(684, 352, w, 22, Chip::Ghost);
        Text(B10, 684 + w * 0.5f, 360, kMuted, cat, 1.4f, kCenter);
    }
    Text(M14, 684, 329, kBone, d->desc);
    // stats: QUANTITY, UNIT, STACK (or CONDITION for an item that wears)
    char q[32], unit[32], third[32];
    if (d->stackMax >= 10000)
        std::snprintf(q, sizeof q, "%u", (unsigned)s->qty);
    else
        std::snprintf(q, sizeof q, "%u / %u", (unsigned)s->qty, (unsigned)d->stackMax);
    const auto kg = [](uint64_t grams, char* out, size_t n) {
        if (grams > 0 && grams < 100) std::snprintf(out, n, "%.2f kg", grams / 1000.0);
        else std::snprintf(out, n, "%.1f kg", grams / 1000.0);
    };
    kg(d->unitGrams, unit, sizeof unit);
    const bool cond = d->condition && s->cond <= 100;
    if (cond)
        std::snprintf(third, sizeof third, "%u %%", (unsigned)s->cond);
    else
        kg((uint64_t)d->unitGrams * s->qty, third, sizeof third);
    if (d->weapon)   // 0.5.4: a weapon's count is its rounds (a thrown weapon's: how many)
        std::snprintf(q, sizeof q, "%u", (unsigned)s->ammo);
    Text(B10, 684, 308, kMuted, d->weapon ? (inv::IsThrown(*d) ? "COUNT" : "AMMO") : "QUANTITY", 1.6f);
    Text(B16, 684, 290, kCream, q, 0.0f, kLeft, true);
    Text(B10, 784, 308, kMuted, "UNIT", 1.6f);
    Text(B16, 784, 290, kCream, unit, 0.0f, kLeft, true);
    Text(B10, 874, 308, kMuted, cond ? "CONDITION" : "STACK", 1.6f);
    Text(B16, 874, 290, cond && s->cond <= 25 ? kEmber : kCream, third, 0.0f, kLeft, true);
    // BIND TO QUICK SLOT 1..4 (worn items are not bound)
    if (u.sel.box == BoxK::Pockets || u.sel.box == BoxK::Pack) {
        Text(B10, 1134, 400, kMuted, "BIND TO QUICK SLOT", 1.6f, kRight);
        for (int k = 0; k < 4; ++k) {
            const float x = 1004.0f + 34.0f * k;
            const inv::ItemId bound = u.ch.quick[k].item;
            const Chip c = bound == 0 ? Chip::Ghost : (bound == s->item ? Chip::Amber : Chip::Bone);
            ChipBox(x, 360, 28, 28, c);
            char key[2] = {(char)('1' + k), 0};
            Text(B12, x + 14.0f, 371, ChipText(c), key, 0.0f, kCenter);
        }
    }
}

void DrawActionBar()
{
    Ui& u = U();
    const Rc pill = Box(571, 209, 582, 56);
    Fill(pill, 28, kCardFill);
    Border(pill, 28, kCardBorder, 1.0f);
    const inv::Slot* s = u.sel.Valid() ? SlotOf(u.sel) : nullptr;
    const bool has = s && !s->Empty();
    const uint32_t maxQ = has ? s->qty : 1;
    u.actQty = std::max<uint32_t>(1, std::min(u.actQty, maxQ));
    const ImVec2 d = DesignOf(ImGui::GetIO().MousePos);
    const bool free = !u.menuOpen && !u.splitOpen && !u.dragging;
    const auto hot = [&](float x, float w) { return free && In(d, x, 216, w, 40); };
    const float aMinus = has && u.actQty > 1 ? 1.0f : 0.35f, aPlus = has && u.actQty < maxQ ? 1.0f : 0.35f;
    ButtonGhost(580, 216, 40, 40, nullptr, aMinus, aMinus > 0.5f && hot(580, 40));
    Icon("minus", 592, 228, 16, kCream, aMinus);
    char qt[16];
    std::snprintf(qt, sizeof qt, "%u", (unsigned)u.actQty);
    Text(B20, 642, 230, kCream, qt, 0.0f, kCenter, true, has ? 1.0f : 0.35f);
    ButtonGhost(664, 216, 40, 40, nullptr, aPlus, aPlus > 0.5f && hot(664, 40));
    Icon("plus", 676, 228, 16, kCream, aPlus);
    const bool canUse = has && CanUse(u.sel);
    ButtonSolid(718, 216, 118, 40, has ? UseLabel(u.sel) : "USE", canUse, canUse && hot(718, 118));
    ButtonGhost(846, 216, 92, 40, "GIVE", 0.35f);   // round 1: no one to give to yet (players nearby arrive with the NEARBY panel)
    const bool canDrop = has && u.sel.box != BoxK::Near;   // 0.5.4: what lies nearby is taken, not dropped
    ButtonGhost(948, 216, 92, 40, "DROP", canDrop ? 1.0f : 0.35f, canDrop && hot(948, 92));
    const bool canSplit = has && CanSplit(u.sel);
    ButtonGhost(1050, 216, 96, 40, "SPLIT", canSplit ? 1.0f : 0.35f, canSplit && hot(1050, 96));
}

void DrawHandPill();   // 0.5.4 (with the NEARBY panel below)
void DrawInventoryPanel()
{
    Ui& u = U();
    Panel(538, 172, 646, 800, 32, kPanelGlass);
    Text(B28, 571, 923, kCream, "INVENTORY", 0.28f);
    // no inventory from this server: the pill, search and chips at 0.55 (and inert: HandleInput), the grids locked, one message card
    const float keepA = g.a;
    if (!u.have)
        g.a *= 0.55f;
    DrawWeightPill();
    DrawHandPill();   // 0.5.4
    // search
    {
        const Rc r = Box(571, 857, 160, 36);
        Fill(r, 18, u.searchFocus ? 0xF7F2E814 : kCardFill);
        Border(r, 18, u.searchFocus ? 0xF2B13499 : kCardBorder, u.searchFocus ? 1.5f : 1.0f);
        Icon("search", 586, 866, 16, u.searchFocus ? kAmber : kMuted);
        g.dl->PushClipRect(Box(608, 857, 116, 36).a, Box(608, 857, 116, 36).b, true);
        if (u.search[0])
            Text(M14, 612, 870, kCream, u.search);
        else
            Text(M14, 612, 870, kMuted, "Search");
        if (u.searchFocus && std::fmod(u.now, 1.0) < 0.55) {
            const float cx = 612.0f + (u.search[0] ? TextWDesign(M14, u.search) : 0.0f) + 1.0f;
            Fill(Box(cx, 863, 1.5f, 15), 0, kAmber);
        }
        g.dl->PopClipRect();
    }
    // category chips
    {
        static const char* const kCats[5] = {"ALL", "TOOLS", "FOOD", "MEDICAL", "DOCS"};
        static const float kX[5] = {745, 809, 887, 961, 1059}, kW[5] = {56, 70, 66, 90, 70};
        for (int i = 0; i < 5; ++i) {
            const Chip c = u.category == i ? Chip::Amber : Chip::Ghost;
            ChipBox(kX[i], 857, kW[i], 36, c);
            Text(B12, kX[i] + kW[i] * 0.5f, 872, ChipText(c), kCats[i], 1.44f, kCenter);
        }
    }
    // pockets
    {
        char lab[48];
        std::snprintf(lab, sizeof lab, "POCKETS \xC2\xB7 %d SLOTS", (int)u.ch.pockets.slots.size());
        DrawSectionHead(816, lab, inv::Grams(u.ch.pockets), u.ch.pockets.maxGrams);
        g.a = keepA;
        if (u.have)
            for (int i = 0; i < (int)u.ch.pockets.slots.size(); ++i)
                DrawGridSlot(Ref{BoxK::Pockets, i}, u.ch.pockets.slots[(size_t)i]);
    }
    if (!u.have) {
        Text(B12, kGridX, 614, kMuted, "BACKPACK", 2.4f, kLeft, false, 0.55f);
        const Rc pc = Box(571, 449, 582, 138);
        Fill(pc, 24, 0xF7F2E805);
        Dashed(pc, 24, 0xF7F2E81F, 1.5f);
        Icon("backpack", 844, 500, 36, kMuted, 0.3f);
        // the message card in place of the whole pocket grid (a smaller card over drawn slots left their edges sticking out)
        const Rc card = Box(571, 651, 582, 138);
        Fill(card, 24, kOverlayGlass);
        Border(card, 24, kGlassBorder, 1.0f);
        Icon("lock", 850.5f, 739, 23, kMuted);
        Text(B18, 862, 715, kCream, "INVENTORY UNAVAILABLE", 2.88f, kCenter);
        Text(M13, 862, 690, kMuted, "This server does not keep inventories yet.", 0.0f, kCenter);
    } else if (u.ch.HasPack()) {
        const inv::PackTier t = inv::PackFor(u.ch.PackTierNow());
        char lab[64];
        std::snprintf(lab, sizeof lab, "BACKPACK \xC2\xB7 %s \xC2\xB7 %d SLOTS", t.name, t.slots);
        DrawSectionHead(614, lab, inv::Grams(u.ch.pack), u.ch.pack.maxGrams);
        for (int i = 0; i < (int)u.ch.pack.slots.size(); ++i)
            DrawGridSlot(Ref{BoxK::Pack, i}, u.ch.pack.slots[(size_t)i]);
    } else {
        DrawSectionHead(614, "BACKPACK", 0, 0, true);
        const Rc card = Box(571, 449, 582, 138);
        Fill(card, 24, 0xF7F2E805);
        Dashed(card, 24, 0xF7F2E81F, 1.5f);
        Icon("backpack", 844, 523.5f, 36, kAmber, 0.6f);
        Text(B14, 862, 502.5f, kMuted, "NO BACKPACK", 1.96f, kCenter);
        Text(M13, 862, 479.5f, kMuted, "Equip one in the PACK slot for 4, 8 or 16 extra slots.", 0.0f, kCenter);
    }
    DrawDetailCard();
    DrawActionBar();
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// 0.5.4 inventory world: the HAND pill and the NEARBY panel's trunk / ground boxes (panel_right_vehicle-suv / -locked / ground.txt). No
// glovebox, stash or GIVE TO NEARBY yet: those parts of the design are left out rather than drawn inert.
// ---------------------------------------------------------------------------------------------------------------------------------------------
void DrawHandPill()
{
    Ui& u = U();
    if (!NU().world || !u.have)
        return;
    const Ref r{BoxK::Hand, 0};
    const inv::Slot& s = u.ch.hand;
    const inv::ItemDef* d = s.Empty() ? nullptr : inv::Find(s.item);
    if (u.dragging && u.hover == r && u.press != r)
        return;   // the ghost carries the drop's state here (as on the grid slots)
    const Look l = SlotLookNow(r, d != nullptr);
    const LookDef ld = LookOf(l);
    const Rc pill = Box(kHandX, kHandY, kHandW, kHandH);
    if (ld.shadow)
        Shadow(pill, 20.0f, ld.shadow, 8.0f);
    Fill(pill, 20.0f, ld.fill, l == Look::Source ? 0.55f : 1.0f);
    if (ld.dashed)
        Dashed(pill, 20.0f, ld.border, ld.bw);
    else
        Border(pill, 20.0f, ld.border, ld.bw);
    if (!d) {
        Text(B10, kHandX + 16.0f, kHandY + 23.0f, kMuted, "HAND", 1.6f);
        Text(B12, kHandX + 16.0f, kHandY + 9.0f, kMuted, l == Look::Target ? "HOLD IT" : "UNARMED", 1.2f);
        return;
    }
    Icon(d->glyph, kHandX + 11.0f, kHandY + 9.0f, 22.0f, ld.glyph, l == Look::Source ? 0.55f : 1.0f);
    std::string name = d->name;
    while (name.size() > 3 && TextWDesign(B12, name.c_str(), 0.6f) > kHandW - 52.0f)
        name.pop_back();
    if (name.size() < std::strlen(d->name))
        name += "\xE2\x80\xA6";
    Text(B12, kHandX + 41.0f, kHandY + 22.0f, kCream, name.c_str(), 0.6f);
    char am[24];
    if (inv::IsThrown(*d))
        std::snprintf(am, sizeof am, "\xC3\x97%u", (unsigned)s.ammo);
    else
        std::snprintf(am, sizeof am, "%u RDS", (unsigned)s.ammo);
    Text(B10, kHandX + 41.0f, kHandY + 8.0f, kMuted, am, 1.2f, kLeft, true);
}

const char* VehicleClassName(int cls)
{
    static const char* const k[23] = {"Compact", "Sedan", "SUV", "Coupe", "Muscle", "Sports classic", "Sports", "Super", "Motorcycle",
                                      "Off-road", "Industrial", "Utility", "Van", "Cycle", "Boat", "Helicopter", "Plane", "Service",
                                      "Emergency", "Military", "Commercial", "Train", "Open wheel"};
    return cls >= 0 && cls < 23 ? k[cls] : "Vehicle";
}
const char* VehicleGlyph(int cls) { return cls == 8 ? "moto" : (cls == 10 || cls == 12 || cls == 20 ? "van" : "car"); }

void DrawNearSlots(const NearC& n, int count)
{
    for (int i = 0; i < count && i < 48; ++i) {
        const Ref r{BoxK::Near, i};
        float x, y, sz;
        SlotPos(r, x, y, sz);
        if (!n.ok || n.meta.locked) {
            const bool deny = U().denyRef == r && U().now - U().denyAt < 0.35;
            SlotFrame(x, y, sz, 16.0f, deny ? Look::Deny : Look::Locked);
            Icon("lock", x + 20.5f, y + 20.5f, 23.0f, kMuted, 0.35f);
            continue;
        }
        DrawGridSlot(r, n.box.slots[(size_t)i]);
    }
}

void DrawNearTabs(const NearC* shown)
{
    const bool hasV = NearOf(kKindTrunk) != nullptr, hasG = NearOf(kKindGround) != nullptr;
    const Rc pill = Box(1589, 911, 236, 40);
    Fill(pill, 20, kCardFill);
    Border(pill, 20, kCardBorder, 1.0f);
    const ImVec2 m = DesignOf(ImGui::GetIO().MousePos);
    struct Tab { const char* glyph; const char* label; uint8_t kind; bool has; float x; };
    const Tab tabs[2] = {{"car", "VEHICLE", kKindTrunk, hasV, 1594}, {"drop", "GROUND", kKindGround, hasG, 1708}};
    for (const Tab& t : tabs) {
        const bool on = shown && shown->meta.kind == t.kind;
        if (on) {
            const Rc r = Box(t.x, 914, 114, 32);
            Fill(r, 16, 0xF2B13424);
            Border(r, 16, 0xF2B13499, 1.5f);
        }
        const bool hv = t.has && !on && In(m, t.x, 914, 114, 32);
        const uint32_t col = on ? kAmber : (hv ? kBone : kMuted);
        const float lw = TextWDesign(B12, t.label, 1.44f), cx = t.x + (114.0f - (16.0f + 7.0f + lw)) * 0.5f;
        const float al = t.has ? 1.0f : 0.4f;
        Icon(t.glyph, cx, 922, 16, col, al);
        Text(B12, cx + 23.0f, 927, col, t.label, 1.44f, kLeft, false, al);
    }
}

void DrawNearby()
{
    Panel(1210, 172, 646, 800, 32, kPanelGlass);
    Text(B28, 1243, 923, kCream, "NEARBY", 0.28f);
    NearC* n = NU().world && U().have ? NearShown() : nullptr;
    if (!n) {
        // nothing in reach (or a 0.5.3 server / inv_world off: no world containers at all): the designed "nothing in reach" state, no tabs
        const Rc circle = Box(1486, 625, 96, 96);
        Fill(circle, 48, 0xF7F2E808);
        Dashed(circle, 48, 0xF7F2E829, 1.5f);
        Icon("car", 1515, 652, 40, kMuted, 0.5f);
        Text(B18, 1534, 581, kMuted, "NOTHING IN REACH", 2.88f, kCenter);
        Text(M14, 1534, 550, kMuted, NU().world ? "Stand by a vehicle or a drop to open it here." : "No vehicle, drop or stash within 2 m.", 0.0f,
             kCenter);
        return;
    }
    DrawNearTabs(n);
    const ImVec2 m = DesignOf(ImGui::GetIO().MousePos);
    const bool free = !U().menuOpen && !U().splitOpen && !U().dragging;
    if (n->meta.kind == kKindTrunk) {
        const inv::VehicleStorage st = inv::StorageForClass(n->meta.cls);
        // the vehicle card
        Fill(Box(1243, 811, 582, 76), 24, kCardFill);
        Border(Box(1243, 811, 582, 76), 24, kCardBorder, 1.0f);
        SlotFrame(1256, 822, 52, 16, Look::Filled);
        Icon(VehicleGlyph(n->meta.cls), 1268, 834, 28, kBone);
        Text(B20, 1324, 856, kCream, VehicleClassName(n->meta.cls));
        char idt[24];
        std::snprintf(idt, sizeof idt, "#%u", (unsigned)n->meta.veh);
        const float iw = std::round(TextWDesign(B11, idt, 1.32f) + 32.0f);
        ChipBox(1324, 820, iw, 22, Chip::Ghost);
        Text(B11, 1324 + iw * 0.5f, 827.5f, kMuted, idt, 1.32f, kCenter);
        char dist[24];
        std::snprintf(dist, sizeof dist, n->meta.distM < 0.05f ? "IN IT" : "%.1f M", n->meta.distM);
        const float dw = std::round(TextWDesign(B11, dist, 1.32f) + 32.0f);
        ChipBox(1324 + iw + 8.0f, 820, dw, 22, Chip::Ghost);
        Text(B11, 1324 + iw + 8.0f + dw * 0.5f, 827.5f, kMuted, dist, 1.32f, kCenter);
        const bool locked = n->meta.locked;
        ChipBox(1678, 832, 136, 32, locked ? Chip::Amber : Chip::Bone);
        const char* lt = locked ? "LOCKED" : "UNLOCKED";
        const float lw = TextWDesign(B12, lt, 1.44f), lx = 1678.0f + (136.0f - (14.0f + 6.0f + lw)) * 0.5f;
        Icon(locked ? "lock" : "unlock", lx, 841, 14, locked ? kAmber : kBone);
        Text(B12, lx + 20.0f, 845, locked ? kAmber : kBone, lt, 1.44f);
        // the trunk
        char lab[48];
        std::snprintf(lab, sizeof lab, "%s \xC2\xB7 %d SLOTS", st.trunkLabel[0] ? st.trunkLabel : "TRUNK", st.trunkSlots);
        Text(B12, 1243, 778, kMuted, lab, 2.4f);
        const Rc track = Box(1243, 763, 582, 4);
        Fill(track, 2, kTrack);
        if (locked || !n->ok) {
            const std::string unk = "\xE2\x80\x94 / " + inv::KgCap(st.trunkGrams) + " KG";
            Text(B12, 1825, 778, kMuted, unk.c_str(), 1.68f, kRight);
        } else {
            const uint64_t grams = inv::Grams(n->box);
            char w[48];
            std::snprintf(w, sizeof w, "%s / %s KG", inv::Kg(grams).c_str(), inv::KgCap(n->box.maxGrams).c_str());
            Text(B12, 1825, 778, kBone, w, 1.2f, kRight);
            const float frac = n->box.maxGrams ? std::min(1.0f, (float)grams / (float)n->box.maxGrams) : 0.0f;
            if (frac > 0.0f)
                Fill(Box(1243, 763, std::max(2.0f, 582.0f * frac), 4), 2, kBone);
        }
        const int count = n->ok ? (int)n->box.slots.size() : st.trunkSlots;
        DrawNearSlots(*n, count);
        if (locked) {
            const Rc card = Box(1322, 499, 424, 120);
            Fill(card, 24, 0x0A0907E6);
            Border(card, 24, kGlassBorder, 1.0f);
            Icon("lock", 1520, 566.5f, 28, kAmber, 1.0f, 0xF2B134CC);
            Text(B14, 1534, 547.5f, kCream, "VEHICLE LOCKED", 2.24f, kCenter);
            Text(M13, 1534, 526.5f, kMuted, "Unlock it with your key to open storage.", 0.0f, kCenter);
        }
        // the action pill
        Fill(Box(1243, 209, 582, 56), 28, kCardFill);
        Border(Box(1243, 209, 582, 56), 28, kCardBorder, 1.0f);
        if (locked) {
            Text(B11, 1814, 233, kMuted, "KEY REQUIRED", 1.54f, kRight);
        } else {
            const bool any = n->ok && inv::UsedSlots(n->box) > 0;
            ButtonGhost(1252, 216, 124, 40, "TAKE ALL", any ? 1.0f : 0.35f, any && free && In(m, 1252, 216, 124, 40));
            Text(B11, 1814, 233, kMuted, "SHIFT + CLICK TO MOVE", 1.54f, kRight);
        }
        return;
    }
    // the ground
    Fill(Box(1243, 811, 582, 76), 24, kCardFill);
    Border(Box(1243, 811, 582, 76), 24, kCardBorder, 1.0f);
    SlotFrame(1256, 822, 52, 16, Look::Filled);
    Icon("drop", 1268, 834, 28, kBone);
    Text(B20, 1324, 854, kCream, "On the ground");
    uint32_t items = 0;
    if (n->ok)
        for (const inv::Slot& s : n->box.slots)
            if (!s.Empty()) items += s.qty;
    char sub[64];
    if (n->meta.piles > 1)
        std::snprintf(sub, sizeof sub, "NEAREST OF %d DROPS \xC2\xB7 %u ITEMS \xC2\xB7 %.1f M", n->meta.piles, (unsigned)items, n->meta.distM);
    else
        std::snprintf(sub, sizeof sub, "%u ITEMS \xC2\xB7 %.1f M AWAY", (unsigned)items, n->meta.distM);
    Text(B11, 1324, 831, kMuted, sub, 1.54f);
    const bool any = items > 0;
    ButtonSolid(1694, 828, 120, 40, "TAKE ALL", any, any && free && In(m, 1694, 828, 120, 40));
    Text(B12, 1243, 778, kMuted, "DROPPED ITEMS", 2.4f);
    char it[24];
    std::snprintf(it, sizeof it, "%u ITEMS", (unsigned)items);
    Text(B12, 1825, 778, kBone, it, 1.2f, kRight);
    DrawNearSlots(*n, n->ok ? (int)n->box.slots.size() : 0);
    Fill(Box(1243, 209, 582, 56), 28, kCardFill);
    Border(Box(1243, 209, 582, 56), 28, kCardBorder, 1.0f);
    Text(B11, 1814, 233, kMuted, "SHIFT + CLICK TO TAKE", 1.54f, kRight);
}

// The NEARBY panel's clicks: a tab, TAKE ALL (every stack, one QuickMove each, pockets then the pack; a stack that does not fit stays).
void NearClicks(ImVec2 d)
{
    NearUi& nu = NU();
    if (In(d, 1594, 914, 114, 32) && NearOf(kKindTrunk)) nu.tab = 0;
    if (In(d, 1708, 914, 114, 32) && NearOf(kKindGround)) nu.tab = 1;
    NearC* n = NearShown();
    if (!n || !n->ok || n->meta.locked)
        return;
    const bool takeAll = n->meta.kind == kKindTrunk ? In(d, 1252, 216, 124, 40) : In(d, 1694, 828, 120, 40);
    if (!takeAll)
        return;
    const int count = (int)n->box.slots.size();
    for (int i = 0; i < count; ++i) {
        NearC* cur = NearShown();   // Send replaces the prediction (NU().near): look the box up again each time
        if (!cur || i >= (int)cur->box.slots.size() || cur->box.slots[(size_t)i].Empty())
            continue;
        Send(MakeOp(inv::OpKind::QuickMove, Ref{BoxK::Near, i}));
    }
}

void DrawHintBar()
{
    struct Hint { const char* key; const char* label; };
    static const Hint kHints[] = {{"SHIFT", "click \xC2\xB7 quick move"}, {"ALT", "drag \xC2\xB7 half stack"}, {"CTRL", "drag \xC2\xB7 split"},
                                  {"RMB", "menu"}, {"MMB", "drop"}, {"1\xE2\x80\x93" "4", "use quick slot"}, {"TAB", "close"}};
    float content = 0.0f;
    for (size_t i = 0; i < sizeof(kHints) / sizeof(kHints[0]); ++i)
        content += (i ? 24.0f : 0.0f) + KeycapW(kHints[i].key) + 8.0f + TextWDesign(S13, kHints[i].label);
    const float w = content + 54.0f, x0 = 960.0f - w * 0.5f;
    Panel(x0, 106, w, 40, 20, kPanelGlass);
    float x = x0 + 27.0f;
    for (size_t i = 0; i < sizeof(kHints) / sizeof(kHints[0]); ++i) {
        if (i) x += 24.0f;
        Keycap(x, 115, kHints[i].key);
        x += KeycapW(kHints[i].key) + 8.0f;
        Text(S13, x, 122.5f, kBone, kHints[i].label);
        x += TextWDesign(S13, kHints[i].label);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Overlays (section 5.5): tooltip, context menu (+ BIND TO SLOT submenu), split dialog, the drag ghost. Each draws in its own local design
// box placed at a screen point; LocalGeo moves the design origin there.
// ---------------------------------------------------------------------------------------------------------------------------------------------
// Place a w x h component so its TOP-LEFT is at screen point p.
void LocalAt(ImVec2 p, float h) { g.ox = p.x; g.oy = p.y - (1080.0f - h) * g.s; }

void DrawTooltip(const inv::Slot& s, Ref at, float W, float H)
{
    const inv::ItemDef* d = inv::Find(s.item);
    if (!d)
        return;
    // anchored to the hovered slot (its right edge + 12, top-aligned; the left edge - 12 when it would leave the screen), never on the cursor:
    // at cursor + 16 it covered the slot's own corner and its quantity tag
    float sx, sy, ssz;
    SlotPos(at, sx, sy, ssz);
    const Rc sr = Box(sx, sy, ssz, ssz);
    const Geo save = g;
    const float w = 300.0f * g.s, h = 196.0f * g.s;
    ImVec2 p(sr.b.x + 12.0f * g.s, sr.a.y);
    if (p.x + w > W - 8.0f) p.x = sr.a.x - 12.0f * g.s - w;
    if (p.y + h > H - 8.0f) p.y = H - 8.0f - h;
    LocalAt(ImVec2(Rn(p.x), Rn(p.y)), 196.0f);
    Panel(0, 0, 300, 196, 24, kOverlayGlass);
    Text(B18, 19, 161, kCream, d->name);
    {
        const char* cat = inv::CategoryName(d->cat);
        const float cw = std::round(TextWDesign(B10, cat, 1.4f) + 26.66f);
        ChipBox(283.0f - cw, 155, cw, 22, Chip::Ghost);
        Text(B10, 283.0f - cw * 0.5f, 163, kMuted, cat, 1.4f, kCenter);
    }
    Text(M13, 19, 133, kBone, d->desc);
    Fill(Box(19, 112, 264, 1), 0, 0xF7F2E81F);
    char q[32], unit[32], stack[32];
    if (d->stackMax >= 10000) std::snprintf(q, sizeof q, "%u", (unsigned)s.qty);
    else std::snprintf(q, sizeof q, "%u / %u", (unsigned)s.qty, (unsigned)d->stackMax);
    std::snprintf(unit, sizeof unit, d->unitGrams > 0 && d->unitGrams < 100 ? "%.2f kg" : "%.1f kg", d->unitGrams / 1000.0);
    std::snprintf(stack, sizeof stack, "%.1f kg", (double)d->unitGrams * s.qty / 1000.0);
    Text(B10, 19, 89, kMuted, "QUANTITY", 1.6f);
    Text(B16, 19, 71, kCream, q, 0.0f, kLeft, true);
    Text(B10, 113, 89, kMuted, "UNIT", 1.6f);
    Text(B16, 113, 71, kCream, unit, 0.0f, kLeft, true);
    Text(B10, 197, 89, kMuted, "STACK", 1.6f);
    Text(B16, 197, 71, kCream, stack, 0.0f, kLeft, true);
    Fill(Box(19, 54, 264, 1), 0, 0xF7F2E81F);
    Keycap(19, 19, "RMB");
    Text(S13, 19 + KeycapW("RMB") + 8.0f, 26.5f, kBone, "menu");
    const float x2 = 19 + KeycapW("RMB") + 8.0f + TextWDesign(S13, "menu") + 16.0f;
    Keycap(x2, 19, "SHIFT");
    Text(S13, x2 + KeycapW("SHIFT") + 8.0f, 26.5f, kBone, "move");
    g = save;
}

struct MenuRow { const char* icon; const char* label; bool enabled; bool chev; int id; };
enum MenuId { kMUse, kMGive, kMDrop, kMSplit, kMBind, kMInspect };

// Returns the screen rect of the menu (for click-outside) and handles its clicks.
void DrawMenu(float W, float H)
{
    Ui& u = U();
    const inv::Slot* s = SlotOf(u.menuRef);
    if (!s || s->Empty()) {
        u.menuOpen = false;
        return;
    }
    const inv::ItemDef* d = inv::Find(s->item);
    const bool worn = u.menuRef.box == BoxK::Equip;
    const MenuRow rows[5] = {
        {d ? d->glyph : "box", UseLabel(u.menuRef), CanUse(u.menuRef), false, kMUse},
        {"person", "GIVE", false, true, kMGive},
        {"drop", "DROP", u.menuRef.box != BoxK::Near, false, kMDrop},
        {"box", "SPLIT", CanSplit(u.menuRef), false, kMSplit},
        {"ring", "BIND TO SLOT", !worn && u.menuRef.box != BoxK::Near && u.menuRef.box != BoxK::Hand, true, kMBind},
    };
    const Geo save = g;
    ImVec2 p = u.menuPos;
    const float w = 210.0f * g.s, h = 254.0f * g.s;
    if (p.x + w > W - 8.0f) p.x = W - 8.0f - w;
    if (p.y + h > H - 8.0f) p.y = H - 8.0f - h;
    LocalAt(ImVec2(Rn(p.x), Rn(p.y)), 254.0f);
    Panel(0, 0, 210, 254, 24, kOverlayGlass);
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = DesignOf(io.MousePos);
    const bool fresh = u.overlayFrame == u.frame;
    const bool click = ImGui::IsMouseClicked(0) && !fresh;
    int clicked = -1;
    int hoverRow = -1;
    for (int i = 0; i < 5; ++i) {
        const float ry = 209.0f - 38.0f * i;
        const bool hv = In(m, 9, ry, 194, 36);
        if (hv) hoverRow = i;
        const bool lit = hv && rows[i].enabled;
        if (lit || (rows[i].id == kMBind && u.menuSub == 1))
            Fill(Box(9, ry, 194, 36), 12, 0xF2B13424);
        const float al = rows[i].enabled ? 1.0f : 0.4f;
        Icon(rows[i].icon, 21, ry + 10, 16, lit ? kAmber : kBone, al);
        Text(B13, 47, ry + 14.5f, lit ? kAmber : kCream, rows[i].label, 1.3f, kLeft, false, al);
        if (rows[i].chev)
            Icon("chev", 177, ry + 11, 14, kMuted, al);
        if (hv && click && rows[i].enabled)
            clicked = rows[i].id;
    }
    Fill(Box(17, 50, 178, 1), 0, 0xF7F2E81F);
    {
        const bool hv = In(m, 9, 7, 194, 36);
        if (hv) { Fill(Box(9, 7, 194, 36), 12, 0xF2B13424); hoverRow = 5; }
        Icon("search", 21, 17, 16, hv ? kAmber : kMuted);
        Text(B13, 47, 21.5f, hv ? kAmber : kMuted, "INSPECT", 1.3f);
        if (hv && click) clicked = kMInspect;
    }
    if (hoverRow == 4 && rows[4].enabled) u.menuSub = 1;
    else if (hoverRow >= 0 && hoverRow != 4) u.menuSub = -1;
    const Rc menuRc = Box(0, 0, 210, 254);
    // BIND TO SLOT > : four rows to the right of the menu
    bool inSub = false;
    if (u.menuSub == 1) {
        const ImVec2 sp(menuRc.b.x + 6.0f * g.s, menuRc.a.y + (254.0f - 133.0f - 36.0f - 9.0f) * g.s);
        const Geo inMenu = g;
        LocalAt(ImVec2(Rn(sp.x), Rn(sp.y)), 170.0f);
        Panel(0, 0, 170, 170, 24, kOverlayGlass);
        const ImVec2 m2 = DesignOf(io.MousePos);
        inSub = In(m2, 0, 0, 170, 170);
        for (int k = 0; k < 4; ++k) {
            const float ry = 125.0f - 38.0f * k;
            const bool hv = In(m2, 9, ry, 152, 36);
            const inv::ItemId b = u.ch.quick[k].item;
            const bool mine = b == s->item;
            if (hv) Fill(Box(9, ry, 152, 36), 12, 0xF2B13424);
            char lab[24];
            std::snprintf(lab, sizeof lab, "SLOT %d", k + 1);
            Text(B13, 21, ry + 14.5f, hv || mine ? kAmber : kCream, lab, 1.3f);
            if (b != 0) {
                const inv::ItemDef* bd = inv::Find(b);
                if (bd) Icon(bd->glyph, 131, ry + 10, 16, mine ? kAmber : kMuted);
            }
            if (hv && click) {
                DoBind(u.menuRef, k);
                u.menuOpen = false;
                u.clickEaten = true;
            }
        }
        g = inMenu;
    }
    const bool inMenu = In(m, 0, 0, 210, 254);
    g = save;
    if (clicked >= 0) {
        u.clickEaten = true;
        switch (clicked) {
        case kMUse: DoUse(u.menuRef); u.menuOpen = false; break;
        case kMDrop: DoDrop(u.menuRef, 0); u.menuOpen = false; break;
        case kMSplit: OpenSplit(u.menuRef, SplitTarget(u.menuRef)); break;
        case kMBind: u.menuSub = 1; break;
        case kMInspect: u.sel = u.menuRef; u.actQty = 1; u.menuOpen = false; break;
        default: break;
        }
    } else if ((click || (ImGui::IsMouseClicked(1) && !fresh)) && !inMenu && !inSub && u.menuOpen) {
        u.menuOpen = false;   // a click outside closes it and does nothing else
        u.clickEaten = true;
    }
}

void DrawSplit(float W, float H)
{
    Ui& u = U();
    const inv::Slot* s = SlotOf(u.splitSrc);
    if (!s || s->qty < 2 || !u.splitDst.Valid()) {
        u.splitOpen = false;
        return;
    }
    const inv::ItemDef* d = inv::Find(s->item);
    const Geo save = g;
    // a modal: the dim under it, the card in the middle of the screen
    g.dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), C(kGround, g.a * 0.35f));
    const float w = 320.0f * g.s, h = 294.0f * g.s;
    LocalAt(ImVec2(Rn((W - w) * 0.5f), Rn((H - h) * 0.5f)), 294.0f);
    Panel(0, 0, 320, 294, 24, kOverlayGlass);
    Text(B12, 25, 262, kMuted, "SPLIT STACK", 2.4f);
    SlotFrame(25, 187, 56, 14, Look::Filled);
    if (d) Icon(d->glyph, 39, 201, 28, kBone);
    {
        const std::string tag = inv::QtyTag(s->qty);
        Text(B13, 25 + 56 - 8, 196, kCream, tag.c_str(), 0.0f, kRight, true, 1.0f, true);
    }
    if (d) {
        Text(B18, 97, 223, kCream, d->name);
        char sub[64];
        std::snprintf(sub, sizeof sub, "%u in stack \xC2\xB7 %.1f kg each", (unsigned)s->qty, d->unitGrams / 1000.0);
        Text(S12, 97, 202, kMuted, sub);
    }
    const ImVec2 m = DesignOf(ImGui::GetIO().MousePos);
    const bool click = ImGui::IsMouseClicked(0) && u.overlayFrame != u.frame;
    const uint32_t maxN = s->qty - 1;
    u.splitN = std::max<uint32_t>(1, std::min(u.splitN, maxN));
    const auto stepper = [&](float x, const char* glyph, bool en) {
        const bool hv = en && In(m, x, 125, 44, 44);
        ButtonGhost(x, 125, 44, 44, nullptr, en ? 1.0f : 0.35f, hv);
        Icon(glyph, x + 14, 139, 16, kCream, en ? 1.0f : 0.35f);
        return hv && click;
    };
    if (stepper(25, "minus", u.splitN > 1)) { u.splitN--; u.splitChip = -1; }
    if (stepper(253, "plus", u.splitN < maxN)) { u.splitN++; u.splitChip = -1; }
    char n[16];
    std::snprintf(n, sizeof n, "%u", (unsigned)u.splitN);
    Text(B34, 161, 136, kCream, n, 0.0f, kCenter, true);
    static const int kNum[5] = {1, 1, 2, 1, 3}, kDen[5] = {2, 3, 3, 4, 4};
    static const char* const kLab[5] = {"1/2", "1/3", "2/3", "1/4", "3/4"};
    for (int i = 0; i < 5; ++i) {
        const float x = 25.0f + 56.0f * i;
        const uint32_t v = inv::SplitAmount(s->qty, kNum[i], kDen[i]);
        const Chip c = u.splitChip == i && v == u.splitN ? Chip::Amber : Chip::Ghost;
        ChipBox(x, 81, 48, 28, c);
        Text(B12, x + 24.0f, 92, ChipText(c), kLab[i], 0.48f, kCenter);
        if (click && In(m, x, 81, 48, 28)) { u.splitN = v; u.splitChip = i; }
    }
    const bool hvCancel = In(m, 25, 23, 128, 40), hvSplit = In(m, 169, 23, 128, 40);
    ButtonGhost(25, 23, 128, 40, "CANCEL", 1.0f, hvCancel);
    ButtonSolid(169, 23, 128, 40, "SPLIT", true, hvSplit);
    const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    if ((click && hvSplit) || enter) {
        Send(MakeOp(inv::OpKind::Split, u.splitSrc, u.splitDst, u.splitN), u.splitSrc);
        u.splitOpen = false;
    } else if (click && hvCancel) {
        u.splitOpen = false;
    }
    if (click)
        u.clickEaten = true;   // modal: a click anywhere belongs to the dialog
    g = save;
}

void DrawGhost(ImVec2 mouse)
{
    Ui& u = U();
    const inv::Slot* s = nullptr;
    inv::Slot qs;
    if (u.press.box == BoxK::Quick) {
        inv::QuickRef q = u.ch.quick[u.press.i];
        if (q.item == 0) return;
        qs.item = q.item;
        qs.qty = 1;
        s = &qs;
    } else {
        s = SlotOf(u.press);
    }
    if (!s || s->Empty())
        return;
    const Geo save = g;
    // the ghost is the filled slot scaled 1.08, held where it was grabbed (centred on the cursor it covered the very slot it was over, and
    // that slot's state with it); its glow says what a drop does here: amber = accepted, ember = refused, none = over nothing
    float sx, sy, ssz;
    SlotPos(u.press, sx, sy, ssz);
    const float size = 64.0f * 1.08f, k = size / ssz;
    LocalAt(ImVec2(Rn(mouse.x - u.grab.x * k), Rn(mouse.y - u.grab.y * k)), size);
    const Rc r = Box(0, 0, size, size);
    // the slot under the ghost is not drawn (DrawGridSlot etc.): the ghost IS the drop's state. Over nothing: the plain cream ghost; over an
    // accepting slot: the target's dashed amber border and glow; over a refusing one: the design's DENIED recipe (ember fill, border, glyph, glow)
    enum { kOverNothing, kAccepted, kRefused } over = kOverNothing;
    if (u.hover.Valid() && u.hover != u.press)
        over = DropAllowed(u.press, u.hover) ? kAccepted : kRefused;
    constexpr float kRad = 17.28f, kBw = 1.62f;
    Shadow(r, kRad, 0x00000080, 12.0f, 12.0f);
    uint32_t glyphCol = kCream, glyphGlow = 0;
    if (over == kAccepted) {
        Shadow(r, kRad, 0xF2B13459, 8.0f);
        Fill(r, kRad, 0xF7F2E824);
        Dashed(r, kRad, 0xF2B134B3, kBw);
    } else if (over == kRefused) {
        Shadow(r, kRad, 0xE5603A59, 8.0f);
        Fill(r, kRad, 0xE5603A29);   // the ember fill alone (over the cream ghost fill it read salmon)
        Border(r, kRad, 0xE5603A99, kBw);
        glyphCol = kEmber;
        glyphGlow = 0xE5603ABF;
    } else {
        SlotFrame(0, 0, size, kRad, Look::Ghost);
    }
    const inv::ItemDef* d = inv::Find(s->item);
    if (d) Icon(d->glyph, 17.28f, 17.28f, 34.56f, glyphCol, 1.0f, glyphGlow);
    const uint32_t q = u.dragMode == 1 ? u.dragQty : s->qty;
    const std::string tag = u.press.box == BoxK::Quick ? std::string() : inv::QtyTag(q);
    if (!tag.empty())
        Text(B13, size - 8.0f * 1.08f, 7.16f, kCream, tag.c_str(), 0.0f, kRight, true, 1.0f, true);
    g = save;
}

void TopTabsInto(ImDrawList* dl, const FrameIn& in, Page& page)
{
    if (!in.isAdmin)
        return;
    struct Tab { const char* glyph; const char* label; Page p; };
    static const Tab kTabs[2] = {{"backpack", "INVENTORY", Page::Inventory}, {"wrench", "ADMIN", Page::Admin}};
    float tw[2];
    // two equal tabs and equal end padding, so the pill's divide sits on the screen's centre line (critic round 1: 6 px off)
    float tmax = 114.0f;
    for (int i = 0; i < 2; ++i)
        tmax = std::max(tmax, std::ceil(16.0f + 7.0f + TextWDesign(B12, kTabs[i].label, 1.44f) + 36.0f));
    tw[0] = tw[1] = tmax;
    // centred between the screen top (y 1080) and the panels' tops (972): 34 px each side of the 40 px pill
    const float w = 4.0f + tw[0] + tw[1] + 4.0f, x0 = 960.0f - w * 0.5f, y0 = 1006.0f;
    ImDrawList* keep = g.dl;
    g.dl = dl;
    const Rc pill = Box(x0, y0, w, 40);
    Fill(pill, 20, kPanelGlass);
    Fill(pill, 20, kCardFill);
    Border(pill, 20, kCardBorder, 1.0f);
    const ImVec2 m = DesignOf(ImGui::GetIO().MousePos);
    float x = x0 + 4.0f;
    for (int i = 0; i < 2; ++i) {
        const bool on = page == kTabs[i].p;
        const bool hv = In(m, x, y0 + 3, tw[i], 32);
        if (on) {
            const Rc r = Box(x, y0 + 3, tw[i], 32);
            Fill(r, 16, 0xF2B13424);
            Border(r, 16, 0xF2B13499, 1.5f);
        }
        const float lw = TextWDesign(B12, kTabs[i].label, 1.44f);
        const float cx = x + (tw[i] - (16.0f + 7.0f + lw)) * 0.5f;
        const uint32_t col = on ? kAmber : (hv ? kBone : kMuted);
        Icon(kTabs[i].glyph, cx, y0 + 11, 16, col, 1.0f, on ? 0xF2B134CC : 0);
        Text(B12, cx + 23.0f, y0 + 16, col, kTabs[i].label, 1.44f);
        if (hv && ImGui::IsMouseClicked(0))
            page = kTabs[i].p;
        x += tw[i];
    }
    g.dl = keep;
}

void SetGeo(const FrameIn& in)
{
    g.s = std::max(0.25f, std::min(in.H / 1080.0f, in.W / 1920.0f));
    g.ox = std::floor((in.W - 1920.0f * g.s) * 0.5f);
    g.oy = std::floor((in.H - 1080.0f * g.s) * 0.5f);
    g.a = in.a;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Input (section 8 "Interactions")
// ---------------------------------------------------------------------------------------------------------------------------------------------
void HandleInput(const FrameIn& in)
{
    Ui& u = U();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 d = DesignOf(io.MousePos);
    u.hover = HitSlot(d);
    if (u.hover.box == BoxK::Pack && !u.ch.HasPack()) u.hover = Ref();
    if (!u.have) {
        // nothing to act on: the screen is a read-only "unavailable" state
        u.hover = u.press = u.sel = u.tipRef = Ref();
        u.searchFocus = u.dragging = false;
        return;
    }

    // the search box: a click on it focuses, anywhere else unfocuses; typing goes to it
    const bool overSearch = In(d, 571, 857, 160, 36);
    if (ImGui::IsMouseClicked(0) && !u.menuOpen && !u.splitOpen)
        u.searchFocus = overSearch;
    if (u.searchFocus) {
        for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
            const ImWchar c = io.InputQueueCharacters[i];
            const size_t n = std::strlen(u.search);
            if (c >= 0x20 && c < 0x7F && n + 1 < sizeof u.search) {
                u.search[n] = (char)c;
                u.search[n + 1] = 0;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) {
            const size_t n = std::strlen(u.search);
            if (n) u.search[n - 1] = 0;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false)) u.searchFocus = false;
    }
    if (in.justOpened)
        u.searchFocus = false;

    if (u.splitOpen || u.menuOpen)
        return;   // the overlays take the mouse this frame (DrawMenu / DrawSplit)

    // keys 1..4: the quick slots (not while typing)
    if (!u.searchFocus) {
        static const ImGuiKey kKeys[4] = {ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4};
        for (int k = 0; k < 4; ++k)
            if (ImGui::IsKeyPressed(kKeys[k], false))
                DoUse(Ref{BoxK::Quick, k});
    }

    // category chips
    if (ImGui::IsMouseClicked(0)) {
        static const float kX[5] = {745, 809, 887, 961, 1059}, kW[5] = {56, 70, 66, 90, 70};
        for (int i = 0; i < 5; ++i)
            if (In(d, kX[i], 857, kW[i], 36)) u.category = i;
    }

    // the detail card's BIND chips and the action bar
    if (ImGui::IsMouseClicked(0) && !u.dragging) {
        const inv::Slot* s = u.sel.Valid() ? SlotOf(u.sel) : nullptr;
        const bool has = s && !s->Empty();
        if (has && (u.sel.box == BoxK::Pockets || u.sel.box == BoxK::Pack))
            for (int k = 0; k < 4; ++k)
                if (In(d, 1004.0f + 34.0f * k, 360, 28, 28)) DoBind(u.sel, k);
        if (has) {
            if (In(d, 580, 216, 40, 40) && u.actQty > 1) u.actQty--;
            if (In(d, 664, 216, 40, 40) && u.actQty < s->qty) u.actQty++;
            if (In(d, 718, 216, 118, 40) && CanUse(u.sel)) DoUse(u.sel);
            if (In(d, 948, 216, 92, 40) && u.sel.box != BoxK::Near) DoDrop(u.sel, u.actQty >= s->qty ? 0 : u.actQty);
            if (In(d, 1050, 216, 96, 40) && CanSplit(u.sel)) OpenSplit(u.sel, SplitTarget(u.sel));
        }
        if (NU().world)
            NearClicks(d);   // 0.5.4: the NEARBY tabs and TAKE ALL
    }

    // slots: press, drag, release
    if (ImGui::IsMouseClicked(0) && u.hover.Valid()) {
        const bool draggable = u.hover.box == BoxK::Quick ? U().ch.quick[u.hover.i].item != 0 : Filled(u.hover);
        u.press = draggable ? u.hover : Ref();
        u.pressPos = io.MousePos;
        {
            float sx, sy, ssz;
            SlotPos(u.hover, sx, sy, ssz);
            const Rc sr = Box(sx, sy, ssz, ssz);
            u.grab = ImVec2(io.MousePos.x - sr.a.x, io.MousePos.y - sr.a.y);
        }
        u.pressShift = io.KeyShift;
        u.pressAlt = io.KeyAlt;
        u.pressCtrl = io.KeyCtrl;
        if (!draggable && u.hover.box != BoxK::Quick)
            u.sel = Ref();   // a click on an empty slot clears the selection
        if (ImGui::IsMouseDoubleClicked(0) && draggable) {
            DoUse(u.hover);
            u.press = Ref();
        }
    }
    if (u.press.Valid() && ImGui::IsMouseDown(0) && !u.dragging) {
        const float dx = io.MousePos.x - u.pressPos.x, dy = io.MousePos.y - u.pressPos.y;
        if (dx * dx + dy * dy > (4.0f * g.s) * (4.0f * g.s)) {
            u.dragging = true;
            u.dragMode = u.press.box == BoxK::Quick || u.press.box == BoxK::Equip ? 0 : (u.pressCtrl ? 2 : (u.pressAlt ? 1 : 0));
            const inv::Slot* s = SlotOf(u.press);
            u.dragQty = s ? (u.dragMode == 1 ? (s->qty + 1) / 2 : s->qty) : 1;
            u.tipRef = Ref();
        }
    }
    if (ImGui::IsMouseReleased(0) && u.press.Valid()) {
        const Ref src = u.press;
        if (u.dragging) {
            const Ref dst = u.hover;
            if (src.box == BoxK::Quick && !dst.Valid()) {
                Send(MakeOp(inv::OpKind::Unbind, Ref(), src));   // dragged off the strip: unbound
            } else if (dst.Valid() && dst != src) {
                if (u.dragMode == 2 && DropAllowed(src, dst)) {
                    OpenSplit(src, dst);
                } else if (DropAllowed(src, dst)) {
                    Send(DropOp(src, dst), dst);
                    if (u.sel == src && dst.box != BoxK::Quick) u.sel = dst;
                } else {
                    Deny(dst);
                }
            }
        } else if (src.box == BoxK::Quick) {
            // a click on a quick slot selects what it is bound to
            inv::QuickRef q = u.ch.quick[src.i];
            if (inv::Resolve(u.ch, q)) {
                u.sel = Ref{q.kind == inv::Kind::Pack ? BoxK::Pack : BoxK::Pockets, q.index};
                u.actQty = 1;
            }
        } else if (u.pressShift) {
            // 0.5.4: with a trunk / pile open in NEARBY, Shift+click on what the player carries puts it there (the panel's SHIFT + CLICK)
            const NearC* open = NU().world ? NearShown() : nullptr;
            const bool intoNear = open && open->ok && !open->meta.locked && (src.box == BoxK::Pockets || src.box == BoxK::Pack);
            Send(MakeOp(inv::OpKind::QuickMove, src, intoNear ? Ref{BoxK::Near, -1} : Ref()), src);
        } else {
            if (u.sel != src) u.actQty = 1;
            u.sel = src;
        }
        u.press = Ref();
        u.dragging = false;
    }
    if (ImGui::IsMouseClicked(1)) {
        if (u.dragging) {
            u.dragging = false;   // right click while dragging cancels
            u.press = Ref();
        } else if (Filled(u.hover)) {
            u.menuOpen = true;
            u.menuRef = u.hover;
            u.menuPos = io.MousePos;
            u.menuSub = -1;
            u.tipRef = Ref();
            u.overlayFrame = u.frame;
        }
    }
    if (ImGui::IsMouseClicked(2) && !u.dragging && Filled(u.hover))
        DoDrop(u.hover, 0);   // middle click: drop the stack

    // the tooltip's 400 ms clock
    const Ref tipNow = !u.dragging && Filled(u.hover) ? u.hover : Ref();
    if (tipNow != u.tipRef) {
        u.tipRef = tipNow;
        u.tipSince = u.now;
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------------
// The bake (UiInit)
// ---------------------------------------------------------------------------------------------------------------------------------------------
void Reserve(ImFontAtlas* atlas, float scale)
{
    Baked& b = B();
    b = Baked();
    b.bakeScale = scale;
    const unsigned char* data[kFaceCount] = {kOverlaySofia3BoldTtf, kOverlaySofia3SemiTtf, kOverlaySofia3MediumTtf};
    const std::size_t len[kFaceCount] = {(std::size_t)kOverlaySofia3BoldTtfLen, (std::size_t)kOverlaySofia3SemiTtfLen, (std::size_t)kOverlaySofia3MediumTtfLen};
    int kernPairs = 0;
    for (int f = 0; f < kFaceCount; ++f) {
        const float em = TtfLineToEm(data[f], len[f]);
        b.em[f] = em > 0.5f && em < 2.0f ? em : 1.0f;
        stbtt_fontinfo fi;
        if (stbtt_InitFont(&fi, data[f], stbtt_GetFontOffsetForIndex(data[f], 0))) {
            int asc = 0, desc = 0, gap = 0;
            stbtt_GetFontVMetrics(&fi, &asc, &desc, &gap);
            if (asc - desc > 0) {
                b.kernUnit[f] = 1.0f / (float)(asc - desc);
                b.kern[f].assign(95 * 95, 0);
                Gpos gp;
                const bool hasGpos = gp.Init(data[f], len[f]);   // a font with GPOS kerning is kerned by it alone (as browsers do)
                int glyph[95];
                for (int a = 0; a < 95; ++a)
                    glyph[a] = stbtt_FindGlyphIndex(&fi, a + 0x20);
                for (int a = 0; a < 95; ++a)
                    for (int c = 0; c < 95; ++c) {
                        const int v = hasGpos ? gp.Pair((unsigned)glyph[a], (unsigned)glyph[c]) : stbtt_GetCodepointKernAdvance(&fi, a + 0x20, c + 0x20);
                        b.kern[f][(size_t)a * 95 + c] = (int16_t)std::clamp(v, -32768, 32767);
                        kernPairs += v != 0;
                    }
            }
        }
    }
    GLOGI("inventory: %d kerning pairs read from the 3 faces", kernPairs);
    int okFonts = 0;
    for (int i = 0; i < kFontCount; ++i) {
        const FontSpec& fs = kFonts[i];
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        // Coverage per weight against the goldens' stems (critic round 1: the HUD's flat 1.22 made 14 px Medium read as SemiBold, 2 px solid
        // stems where the golden has 1 px plus anti-aliasing). OversampleH 3: the pens are fractional (Text).
        cfg.OversampleH = 3;
        cfg.OversampleV = 1;
        cfg.RasterizerMultiply = fs.px > 14.0f ? 1.0f : (fs.face == kMediumFace ? 1.0f : (fs.face == kSemiFace ? 1.06f : 1.1f));
        const float px = std::max(6.0f, std::floor(CssPx(fs.px, scale) * b.em[fs.face] + 0.5f));
        b.font[i] = atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(data[fs.face]), (int)len[fs.face], px, &cfg, fs.ranges);
        okFonts += b.font[i] ? 1 : 0;
    }
    b.fontsOk = okFonts == kFontCount;
    if (!b.fontsOk) {
        GLOGW("inventory: %d of %d REV3 faces loaded; the missing ones use ImGui's default font", okFonts, kFontCount);
        ImFontConfig dc;
        ImFont* def = atlas->AddFontDefault(&dc);
        for (ImFont*& f : b.font)
            if (!f) f = def;
    }
    b.iconCount = std::min(kInvIconsCount, kMaxIcons);
    for (int i = 0; i < b.iconCount; ++i) {
        for (int s = 0; s < kIconSizeCount; ++s) {
            AtlasRect& r = b.icon[i][s];
            if (!WantIcon(kInvIcons[i].name, kIconSizes[s])) continue;
            r.px = (int)std::lround(kIconSizes[s] * scale);
            r.rect = r.px > 0 && r.px < 512 ? atlas->AddCustomRectRegular(r.px, r.px) : -1;
        }
        for (int s = 0; s < kGlowSizeCount; ++s) {
            AtlasRect& r = b.glow[i][s];
            if (!WantGlow(kInvIcons[i].name, kGlowSizes[s])) continue;
            r.px = (int)std::lround(kGlowSizes[s] * scale);
            r.pad = (int)std::ceil(3.0f * kGlowSigma * scale) + 1;
            const int side = r.px + 2 * r.pad;
            r.rect = side > 0 && side < 1024 ? atlas->AddCustomRectRegular(side, side) : -1;
        }
    }
    b.manW = (int)std::ceil(224.0f * scale);
    b.manH = (int)std::ceil(532.0f * scale);
    b.manikin.px = b.manW;
    b.manikin.rect = b.manW > 0 && b.manH < 4096 ? atlas->AddCustomRectRegular(b.manW, b.manH) : -1;
}

void Fill(ImFontAtlas* atlas, float scale)
{
    Baked& b = B();
    unsigned char* tex = nullptr;
    int tw = 0, th = 0;
    atlas->GetTexDataAsAlpha8(&tex, &tw, &th);
    if (!tex) {
        GLOGW("inventory: no alpha atlas; icons and the mannequin are not drawn");
        b.iconCount = 0;
        b.manikin.rect = -1;
        return;
    }
    const auto write = [&](int rectIdx, const std::vector<float>& a, int W, int H) {
        const ImFontAtlasCustomRect* r = atlas->GetCustomRectByIndex(rectIdx);
        if (!r || !r->IsPacked() || r->Width != W || r->Height != H)
            return false;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                tex[(size_t)(r->Y + y) * tw + (r->X + x)] = (unsigned char)std::lround(std::clamp(a[(size_t)y * W + x], 0.0f, 1.0f) * 255.0f);
        return true;
    };
    int icons = 0, glows = 0;
    for (int i = 0; i < b.iconCount; ++i) {
        std::vector<float> src;
        int w = 0, h = 0;
        if (!DecodeAlpha(kInvIcons[i].data, (std::size_t)kInvIcons[i].len, src, w, h)) {
            GLOGW("inventory: icon %s did not decode", kInvIcons[i].name);
            for (AtlasRect& r : b.icon[i]) r.rect = -1;
            for (AtlasRect& r : b.glow[i]) r.rect = -1;
            continue;
        }
        for (int s = 0; s < kIconSizeCount; ++s) {
            AtlasRect& r = b.icon[i][s];
            if (r.rect < 0) continue;
            if (write(r.rect, Downsample(src, w, h, r.px), r.px, r.px)) ++icons;
            else r.rect = -1;
        }
        for (int s = 0; s < kGlowSizeCount; ++s) {
            AtlasRect& r = b.glow[i][s];
            if (r.rect < 0) continue;
            const int side = r.px + 2 * r.pad;
            const std::vector<float> small = Downsample(src, w, h, r.px);
            std::vector<float> a((size_t)side * side, 0.0f);
            for (int y = 0; y < r.px; ++y)
                for (int x = 0; x < r.px; ++x)
                    a[(size_t)(y + r.pad) * side + (x + r.pad)] = small[(size_t)y * r.px + x];
            GaussBlur(a, side, side, kGlowSigma * scale);
            if (write(r.rect, a, side, side)) ++glows;
            else r.rect = -1;
        }
    }
    // the mannequin: its shapes rasterised and unioned, one alpha map
    if (b.manikin.rect >= 0) {
        ImDrawListSharedData sd;
        sd.SetCircleTessellationMaxError(0.30f);
        sd.ClipRectFullscreen = ImVec4(-8192.0f, -8192.0f, 8192.0f, 8192.0f);
        sd.InitialFlags = ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;
        ImDrawList dl(&sd);
        dl._ResetForNewFrame();
        dl.PushClipRectFullScreen();
        DrawManikinShapes(&dl, scale, 0.0f);
        std::vector<float> a;
        Rasterise(dl, b.manW, b.manH, a);
        if (!write(b.manikin.rect, a, b.manW, b.manH))
            b.manikin.rect = -1;
    }
    GLOGI("inventory: REV3 bake at scale %.2f: %d faces, %d icon sizes, %d glows, mannequin %s (atlas %dx%d)", scale, b.fontsOk ? kFontCount : 0, icons, glows,
          b.manikin.rect >= 0 ? "ok" : "missing", tw, th);
}

void Reset()
{
    Ui& u = U();
    u.press = u.hover = u.menuRef = u.splitSrc = u.splitDst = u.tipRef = Ref();
    u.dragging = u.menuOpen = u.splitOpen = u.searchFocus = false;
    u.seenSnap = ~0u;   // re-read the store on the next frame
    B() = Baked();
}

// 0.5.6 weapon_keys: a number key 1-4 pressed while no panel is open (overlay_ui.cpp UiFrame). The same op the open page sends (UseQuick:
// a bound weapon goes into the hand, the weapon in hand goes away, food is eaten), through the same prediction and queue.
bool QuickKey(int k)
{
    Ui& u = U();
    if (k < 0 || k > 3)
        return false;
    u.now = ImGui::GetTime();
    Sync();
    const Ref r{BoxK::Quick, k};
    u.quickFlash[k] = u.now;
    const bool sent = Send(MakeOp(inv::OpKind::UseQuick, r), r);
    (sent ? g_quickSent : g_quickRefused).fetch_add(1);
    static int lines = 0;
    if (lines < 40) {
        ++lines;
        const inv::ItemDef* d = inv::Find(u.ch.quick[k].item);
        const inv::ItemDef* h = inv::Find(u.ch.hand.item);
        GLOGI("inventory: quick key %d (page closed): %s; slot bound to %s, hand now %s (predicted)", k + 1,
              !u.have ? "no server inventory, nothing sent" : sent ? "UseQuick sent" : "refused by the rules (empty slot, item gone)",
              d ? d->key : "nothing", h ? h->key : "empty");
    }
    return sent;
}

bool HandleEsc()
{
    Ui& u = U();
    if (u.dragging) { u.dragging = false; u.press = Ref(); return true; }
    if (u.splitOpen) { u.splitOpen = false; return true; }
    if (u.menuOpen) { u.menuOpen = false; return true; }
    if (u.searchFocus) { u.searchFocus = false; return true; }
    return false;
}

Page DrawTopTabs(const FrameIn& in)
{
    SetGeo(in);
    Page p = in.page;
    TopTabsInto(ImGui::GetForegroundDrawList(), in, p);
    return p;
}

FrameOut DrawScreen(const FrameIn& in)
{
    FrameOut out;
    out.page = in.page;
    Ui& u = U();
    u.now = ImGui::GetTime();
    u.clickEaten = false;
    ++u.frame;
    if (in.justOpened) {
        u.dragging = u.menuOpen = u.splitOpen = false;
        u.press = Ref();
    }
    Sync();
    u.fallbackName = in.playerName;
    SetGeo(in);
    const ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(in.W, in.H));
    if (in.justOpened)
        ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##gage_inventory_rev3", nullptr, wf)) {
        g.dl = ImGui::GetWindowDrawList();
        // 0.5.4 inv_ped_view: the page is on screen (the engine's camera follows this stamp); the dim leaves the ped's window out
        g_pedViewDrawnAt.store(GetTickCount64());
        g_pedViewW.store((int)in.W);
        g_pedViewH.store((int)in.H);
        g_pedViewThisFrame = PedViewFresh();
        // the overlays first take this frame's clicks (they are drawn last, on top)
        DrawDim(in);
        HandleInput(in);
        DrawHintBar();
        DrawCharacter();
        DrawInventoryPanel();
        DrawNearby();
        Page p = in.page;
        TopTabsInto(g.dl, in, p);
        out.page = p;
        if (u.dragging)
            DrawGhost(io.MousePos);
        else if (u.menuOpen)
            DrawMenu(in.W, in.H);
        else if (u.splitOpen)
            DrawSplit(in.W, in.H);
        else if (u.tipRef.Valid() && u.now - u.tipSince >= 0.4) {
            const inv::Slot* s = SlotOf(u.tipRef);
            if (s && !s->Empty())
                DrawTooltip(*s, u.tipRef, in.W, in.H);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    out.typing = u.searchFocus;
    return out;
}

}  // namespace gage::game::Overlay::detail::inv3
