// GAGE-MP in-game overlay (P5 foundation): Dear ImGui drawn over the game's D3D11
// swap chain from inside the Present detour.
//
// This module is self-contained: it needs only the IDXGISwapChain* that
// game::GetSwapChain() returns (hook.h). It builds the ImGui context, the Win32
// and DX11 backends and a WndProc subclass on first use, draws one ImGui frame
// per Present with proper alpha, and puts every piece of device-context state it
// touches back the way the game left it.
//
// Threading: Init / Render / Shutdown / LoadPngTexture run on the render thread
// (the Present detour). WantsInput() may be called from any thread. The WndProc
// subclass runs on the window's thread; a critical section covers the one place
// the two threads touch the same ImGui input state.
//
// The drawn content (overlay_ui.cpp) is the real in-game UI: a full-screen loading
// state until the player is in the world, then the logo mark (top-right), chat (top-left,
// display + input), the Tab quick-actions palette and, bottom-right, the vitals cluster and
// a speedometer that shows only in a vehicle.
//
// Data flow (the render thread never calls a game native): the engine pushes values
// from whatever thread has them (SetPhase, SetStats, PushChatLine, ...) and the overlay
// only renders what was pushed. What the player types or clicks leaves through an
// outgoing queue the engine drains on the script tick (PopOutgoing) and feeds to
// engine::SendChat, which routes /commands locally and plain text to the server.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gage::game::Overlay {

    // ---- what the engine pushes (thread-safe, any thread) ------------------

    // Loading: the full-screen GAGE loading state covers the game (also the default
    // until the engine says otherwise). InWorld: it fades out and the HUD fades in.
    enum class Phase { Loading, InWorld };
    void SetPhase(Phase p);

    // One line of status text under the loader's logo ("Connecting to host:port").
    // UTF-8, truncated to 150 bytes. Pushing the same text every frame is fine.
    // failed marks a dead end ("The server refused the connection"): the progress
    // row stops travelling and the text is shown at full strength, no trailing dots.
    void SetLoaderStatus(const char* utf8, bool failed = false);

    // The local player's vehicle state; the speedometer is drawn only while inVehicle. The
    // displayed number is eased toward speedKmh, so pushing a raw per-tick value is fine.
    void SetStats(bool inVehicle, float speedKmh);

    // The vitals cluster (bottom-right, under the speedometer): health and satiety, each a 0..1
    // fraction (NaN counts as 0, anything else is clamped). show false hides the cluster (the
    // speedometer is not part of it: it follows SetStats). fuelFrac, while showFuel, is drawn as a
    // gauge on the speedometer plate (vehicle data), so it appears only in a vehicle. Fills ease
    // toward the pushed values; a value below its low threshold (health 30 %, satiety 25 %, fuel
    // 15 %) turns amber and pulses, and the cluster's accent rail alarms with it. Until the engine
    // pushes anything the overlay shows health and satiety full, no fuel.
    void SetVitals(float healthFrac, float satietyFrac, float fuelFrac, bool show, bool showFuel);

    // 0.4.22 minimap: dock = true puts the vitals under GTA's radar (bottom-left) as a slim strip instead of the bottom-right plate:
    // the radar's left edge and width, about 7 % of its height, health in the left half and food in the right half (a glyph and a
    // thin bar each, no numbers), directly under the radar's bottom edge inside the safe-zone margin. The speedometer then rests on
    // the bottom gutter. The radar's rectangle is computed here (libs/sync/minimap.h RadarKeepOut) from the display size and safeZone
    // (GET_SAFE_ZONE_SIZE, as the engine reads it; safeZoneOk false = unreadable, the widest margin is assumed). Where the margin under
    // the radar is too small the strip tucks up into the radar's bottom edge, at most 8 % of its height; past that the plate stays in
    // the bottom-right (UiState::vitalsDockWhy says why). dock = false is the pre-0.4.22 layout. Any thread.
    // 0.4.25 scale (0.5..1, default 1): the factor the game's radar was shrunk by (minimap_scale; the engine passes the one its table holds, read back,
    // never the one asked for). The radar rectangle loses width and height about its bottom-left corner, so the strip stays flush under the smaller map.
    void SetRadarDock(bool dock, float safeZone, bool safeZoneOk, float scale = 1.0f);

    // 0.4.31 hud_ring: true draws the vitals as GI's REV1 ring (libs/sync/hud_ring.h: a glass disc with the HP arc on the left and the SAT arc on
    // the right, the HP / SAT strip under it), 56 px from the bottom-left corner at 1080p, scaled by H / 1080; the speedometer rests on the bottom
    // gutter. The engine hides GTA's radar while it is on (the design's "map off" state). false = the 0.4.30 plate / radar strip. Any thread.
    void SetHudRing(bool on);

    // 0.5.2 ring_map: true = GTA's radar sits in the ring now (the engine checked the table, the radar and the big radar): the disc becomes an opaque
    // annulus over the radar square's corners (hudring::MapRadiusPx out to the disc) and the map shows through the hole, with the design's vignette
    // and amber map ring at the hole's edge. false = the full glass disc (map off). Any thread.
    void SetRingMap(bool open);

    // v8 accounts: true = this player may use the admin commands (/heal, /fix, /spawn, /tp, /noclip, /gun, /car, the Cars page spawn), so the
    // overlay may offer them. The engine sets it from the server's AccountInfo (kAcctCommands: an admin role, or a guest on a server that does
    // not require accounts), false when a session ends, true with no server configured (single player). The server still checks every such
    // command (AdminCmd), so this only decides what is shown. Any thread.
    void SetIsAdmin(bool admin);
    bool IsAdmin();

    // 0.6 inventory (overlay_inventory.cpp, GI's REV3 design on the Tab menu). SetInventory: the server's InvState (`encoded` =
    // sync::inv::Encode bytes, `ackSeq` = the last op it processed, `characterName` for the CHARACTER chip); the screen drops its
    // predictions up to ackSeq and applies the rest again on top. ClearInventory: the session ended (the screen shows "unavailable").
    // PopInventoryOp: the next op the player made (sync::inv::EncodeOp bytes and its seq) for the engine to send as InvOp; false when
    // none is waiting. Any thread.
    void SetInventory(const std::string& encoded, uint32_t ackSeq, const char* characterName);
    void ClearInventory();
    bool PopInventoryOp(std::string* encodedOp, uint32_t* seq);
    // 0.6: true while the Tab menu shows the inventory page (the engine hides GTA's radar under it). Any thread.
    bool InventoryOpen();
    // 0.6 inv_rev3 (HKCU\Software\GAGE-MP, default on): the Tab menu opens on the REV3 inventory, admins also get the old menu as ADMIN;
    // off = the old Tab menu only (0.5.2). Any thread.
    void SetInventoryScreen(bool on);
    // 0.5.4 inventory world (overlay_inventory.cpp; rules in libs/sync/inventory_world.h). SetInventoryWorld: the server speaks it and the
    // engine answered (inv_world on): the screen shows the HAND pill, weapons, and the NEARBY panel's trunk / ground boxes, and sends ops
    // on them. SetNearby: the server's InvNearby (at most one trunk and one ground pile; `encoded` = sync::inv::EncodeBox, empty while locked).
    // ClearInventory also clears both. Any thread.
    struct InvNearbyBox {
        uint32_t id = 0;
        uint8_t kind = 0;          // sync::inv::Kind (Trunk 3, Ground 5)
        bool locked = false;
        uint16_t veh = 0xFFFF;     // a trunk's server vehicle id
        uint32_t model = 0;
        int cls = -1;              // GET_VEHICLE_CLASS of the trunk's vehicle
        int piles = 0;             // ground: piles in reach (the nearest is this box)
        float distM = 0;
        std::string encoded;
    };
    void SetInventoryWorld(bool on);
    void SetNearby(const std::vector<InvNearbyBox>& boxes);

    // 0.5.2 round-map probe: the next frame's bottom-left (the radar's quarter, before the overlay draws on it) is saved to
    // %LOCALAPPDATA%\GAGE-MP\radar_probe_<n>.bmp and logged, so the radar's landing place can be measured. D3D11 (Legacy) only; on D3D12 one line
    // says it was skipped. Any thread.
    void RequestRadarProbe(int n);

    // 0.4.22 vehicle controls (libs/sync/vehicle_controls.h). The keys are read in UiFrame from ImGui's key events, the same source as Tab / T /
    // Enter: Left / Right Arrow (indicators), Down Arrow (hazards), Y (engine), B (seatbelt); H is GTA's own headlight control and is not read here.
    // Nothing is taken while the chat, the Tab menu, a text box or the quit card has the keyboard. TakeVehicleKeys returns the bits pressed since
    // the last call (edges, no auto-repeat) and clears them; a press older than 400 ms is dropped (the engine's tick was not running). Any thread.
    enum VehicleKey : unsigned { kVkLeft = 1u << 0, kVkRight = 1u << 1, kVkHazard = 1u << 2, kVkEngine = 1u << 3, kVkBelt = 1u << 4 };
    unsigned TakeVehicleKeys();

    // The small status strip along the foot of the speedometer plate (vehicle only): the two turn arrows (blinking at about 1.5 Hz while on;
    // the blink is the overlay's), the headlights (off / low / high), the seatbelt (beltOn buckled; beltWarn = unbuckled and moving, the warning
    // colour) and the engine (engineOff, the warning colour). show false removes the strip and the plate returns to its old height.
    // 0.4.27 the vehicle state row (libs/sync/vehicle_state_row.h): lock, belt and engine, each a pixel glyph and a toggle, next to the vitals (under the
    // docked strip when the margin allows, else above the radar; above the bottom-right plates with the radar off), shown while `show`, and on foot
    // for a few seconds after the server confirmed a lock (lockFlash: only the lock cell is live). A cell pulses gold when its state flips.
    struct VehicleHud {
        bool show = false;
        bool indLeft = false, indRight = false;
        int  lights = 0;                          // 0 off, 1 low beam, 2 high beam
        bool beltOn = false, beltWarn = false, engineOff = false;
        int  lock = -1;                           // -1 not known (not a networked car), 0 unlocked, 1 locked
        bool lockFlash = false;                   // on foot: show the row for the lock the server just confirmed
    };
    void SetVehicleHud(const VehicleHud& h);

    // Capture proof only (tools / selftests; the engine never calls it): hold the HUD clock at one phase, 0..1. Every blink and pulse of the HUD (the turn
    // indicators blink on phase < 0.5; the low-vital, belt-warning and rail alarms pulse on the 2nd harmonic of the same phase) comes from the one clock
    // phase = fract(t * 1.5), so a still frame can show any beat. A value < 0 (the default) gives the clock back to the real time. Any thread.
    void SetHudPhaseForTest(float phase);

    // W10: the player name tags, one per remote ped in range, drawn in the world layer under the HUD plates. The
    // engine decides which tags exist and where (libs/sync/nametag.h: range, projection, line of sight, dead, the
    // fade); the overlay only draws what it was handed. x / y are 0..1 of the screen (y grows down) and mark the
    // tag's bottom centre, alpha 0..1 is the whole tag's visibility, scale 0.5..1.2 multiplies the HUD fonts. idText
    // ("[12]") and name are UTF-8; control characters become spaces, the strings are cut at their buffers. The list
    // REPLACES the previous one (count 0 clears it) and is copied under the overlay's data lock, so any thread may
    // push; entries past kMaxNameTags are ignored. A list not refreshed for 400 ms is no longer drawn (a stalled
    // script tick never leaves tags hanging), and the tags are hidden with the rest of the HUD while the loader
    // covers the game.
    static constexpr int kMaxNameTags = 16;
    struct NameTag {
        float x = 0.0f, y = 0.0f;
        float alpha = 0.0f, scale = 1.0f;
        char  idText[12] = {};
        char  name[104] = {};
    };
    void SetNameTags(const NameTag* tags, int count);

    // The Tab menu's "Cars" page: every car the mounted content packs carry (pack_stream.h Vehicles()). The list
    // REPLACES the previous one, is copied under the data lock (any thread), and is shown in the order given;
    // entries past kMaxCars are ignored. `loading` says packs are still being mounted (an empty list then reads
    // "loading", not "no packs"). Strings are UTF-8, cut at their buffers. Clicking a car submits "/car <model>".
    static constexpr int kMaxCars = 4096;
    struct CarEntry {
        char model[64] = {};    // the /car name
        char name[80] = {};     // what the row shows (pack_meta.h CarDisplayName)
        char vclass[32] = {};   // "Super" (CarClassName), may be empty
        char pack[64] = {};     // the pack folder it came from
        bool ready = false;     // the game has the model (IS_MODEL_IN_CDIMAGE); a not-ready row is dimmed
    };
    void SetCars(const CarEntry* cars, int count, bool loading);

    // The Tab menu's "Noclip" toggle (quick actions, middle column): available says the engine offers it at all (the noclip
    // registry switch; false hides the row, its key hints and the "/noclip" typed-command hint), active is the state the row
    // shows (On in gold / Off). Clicking the row submits "/noclip". Any thread; until the engine pushes, the row is offered
    // and off.
    void SetNoclip(bool available, bool active);

    // A chat line for the log. name is empty/null for a server or system line;
    // self marks the local player's own line. UTF-8, control characters become
    // spaces, name is cut at 32 bytes and text at 256.
    void PushChatLine(const char* name, const char* text, bool self = false);

    // A dim system line (feedback such as "Not connected").
    void PushSystemLine(const char* text);

    // ---- what the engine pulls --------------------------------------------

    // Pops the oldest line the player submitted from the chat box or a quick action
    // ("hello", "/car adder"). Returns false when the queue is empty; out is always
    // NUL-terminated. The engine calls this on the script tick.
    bool PopOutgoing(char* out, std::size_t cap);

    // True while the chat input or the quick-actions palette is open (and the HUD is
    // live): the engine shows the game cursor state and disables the game controls.
    bool Interactive();

    // Build everything from the game's swap chain: device and immediate context
    // (IDXGISwapChain::GetDevice), the HWND (GetDesc.OutputWindow), the ImGui
    // context, both backends, and subclass the window procedure. Idempotent for
    // the same swap chain and device; a different swap chain or device tears the
    // old overlay down first. After a failed attempt it backs off (2 s, five
    // tries per swap chain) and logs the reason, so calling it every frame is safe.
    bool Init(void* swapChain);

    // One overlay frame onto swapChain's back buffer: NewFrame, the draw function,
    // Render, a render-target view made from GetBuffer(0) this frame, the ImGui
    // draw data, then the view released. Initialises on demand. Call once per
    // Present, before the original Present runs.
    void Render(void* swapChain);

    // Release every overlay resource and restore the window procedure (when
    // nothing was subclassed on top of ours). Waits briefly for a Render() that is
    // in flight on another thread. Safe to call twice and when never initialised.
    void Shutdown();

    // True while the overlay is initialised for a swap chain.
    bool Ready();

    // ImGui's WantCaptureMouse / WantCaptureKeyboard as of the last frame, or an open
    // chat / quick-actions panel (Interactive()). While true the WndProc subclass keeps
    // mouse-down / key-down / char messages from the game (button-ups and key-ups
    // always pass, so nothing sticks); the engine disables the game's controls.
    bool WantsInput();

    // Alt+F4 and window close (WM_SYSKEYDOWN VK_F4 with Alt, WM_CLOSE, WM_SYSCOMMAND SC_CLOSE;
    // quit_gate.h): with a handler set, the WndProc subclass keeps the message from the game (whose
    // own "quit game?" screen pauses everything) before ImGui sees it; the first one opens the
    // confirm card (OpenQuitPrompt, below) and the answer calls the handler on the window's
    // thread. While the overlay is not ready there is no card and the first one calls it. Without one (nullptr, the default)
    // the game handles them as before. The handler is expected not to return (it ends the
    // process); if it does, the message stays swallowed. Any thread.
    void SetQuitHandler(void (*handler)());

    // The Alt+F4 confirm card (drawn by the overlay, never pausing the game or taking its keys).
    // With a quit handler and a Ready() overlay, the first Alt+F4 / close opens it instead of
    // quitting; while it is open Enter or another Alt+F4 / close calls the handler, Esc closes it,
    // and after 10 s unanswered it closes by itself. OpenQuitPrompt returns false when it was
    // already open. QuitPromptOpen is what the engine's pause-menu gate reads so the Esc that
    // answers the card does not also open the pause menu. Any thread.
    bool OpenQuitPrompt();
    bool QuitPromptOpen();
    void CloseQuitPrompt();

    // Decode an in-memory PNG with WIC to straight-alpha RGBA (what ImGui's DX11
    // blend state expects; the colour under transparent texels is filled from their
    // neighbours so bilinear filtering draws no dark fringe), and upload it as an immutable
    // ID3D11Texture2D plus shader resource view. *outTexture receives the SRV as
    // the ImTextureID to pass to ImGui::Image. Needs a Ready() overlay; textures
    // belong to the current device, so request them again after a re-init (they are
    // released by Shutdown / FreeTexture). Returns false and logs the reason on
    // any failure (*outTexture is null then).
    bool LoadPngTexture(const unsigned char* bytes, std::size_t n, void** outTexture,
                        int* outWidth = nullptr, int* outHeight = nullptr);

    // Release a texture from LoadPngTexture. Null is ignored. Only for textures the caller
    // loaded itself: the overlay owns its brand textures and frees them in Shutdown.
    void FreeTexture(void* texture);

    // What the last frame laid out, in screen pixels, for the capture proof (and
    // layout checks). valid is false until a frame has been drawn.
    struct UiState {
        static constexpr int kMaxActions = 16;
        bool  valid = false;
        float displayW = 0, displayH = 0, scale = 1;
        float loaderAlpha = 0, hudAlpha = 0;      // 0..1
        bool  interactive = false;
        bool  loaderFailed = false;               // the status is a dead end: the progress row is still
        bool  loaderSceneTextured = false;        // the GTA V pictures are behind the loader
        float loaderSceneMix = 0;                 // 0: the current picture, 1: the next; between the two is a crossfade
        bool  loaderLockupTextured = false;       // false: the typeset fallback wordmark was drawn
        float loaderLockupX = 0, loaderLockupY = 0, loaderLockupW = 0, loaderLockupH = 0;
        float loaderBarX = 0, loaderBarY = 0, loaderBarW = 0, loaderBarH = 0;        // progress segments
        float loaderStatusX = 0, loaderStatusY = 0, loaderStatusW = 0, loaderStatusH = 0;
        // the corner mark (top-right)
        bool  logoTextured = false;               // false: the designed fallback mark was drawn
        float logoX = 0, logoY = 0, logoW = 0, logoH = 0;
        // chat (top-left, 100 px of the 1080p design from the top): the open plate, or the visible
        // log block when closed
        bool  chatOpen = false;
        float chatX = 0, chatY = 0, chatW = 0, chatH = 0;
        float inputX = 0, inputY = 0, inputW = 0, inputH = 0;   // valid while chatOpen
        int   chatLinesShown = 0;
        // quick actions
        bool  invOpen = false;
        float invX = 0, invY = 0, invW = 0, invH = 0;
        int   actionCount = 0;
        float actionRect[kMaxActions][4] = {};    // x, y, w, h of each button
        char  actionCmd[kMaxActions][48] = {};
        int   noclipRow = -1;                     // index of the noclip toggle in actionRect / actionCmd on the quick actions page, -1 when it is not shown
        bool  noclipOn = false;                   // the state the toggle showed
        // the Tab menu's pages: 0 quick actions, 1 cars. On the cars page actionRect / actionCmd hold its rows.
        int   invTab = 0;
        float tabRect[2][4] = {};                 // the two page tabs in the header
        float searchX = 0, searchY = 0, searchW = 0, searchH = 0;
        float pageNextRect[4] = {}, pagePrevRect[4] = {};
        int   carsTotal = 0, carsMatched = 0, carPage = 0, carPages = 0;
        int   litCar = -1;                        // the lit row on the cars page (Enter's target), -1 when none
        bool  carsLoading = false;
        // speedometer (bottom-right; speedShown is the eased number on screen, speedLit of
        // speedSegments gauge bars are lit)
        bool  speedoVisible = false;
        float speedoX = 0, speedoY = 0, speedoW = 0, speedoH = 0;
        int   speedShown = 0, speedLit = 0, speedSegments = 0;
        // the status strip on the speedometer plate (0.4.22): its box, the arrows lit this frame (the blink), and what each glyph showed
        bool  stripVisible = false;
        float stripX = 0, stripY = 0, stripW = 0, stripH = 0;
        bool  stripIndLeftLit = false, stripIndRightLit = false;
        float stripLampX = 0, fuelBarX = 0;       // the headlamp's first ink column (0.4.29: on the pump's left edge when the gap allows) and the fuel bar's first pixel
        int   stripLights = 0;
        bool  stripBeltOn = false, stripBeltWarn = false, stripEngineOff = false;
        int   stripLock = -1;   // the strip's padlock: -1 unknown, 0 unlocked, 1 locked
        // 0.4.27 the vehicle state row: its box, where it went (sync::RowWhere as an int), and what each cell showed (lock -1 / 0 / 1)
        bool  rowVisible = false;
        int   rowWhere = 0;
        float rowX = 0, rowY = 0, rowW = 0, rowH = 0;
        int   rowLock = -1;
        bool  rowBelt = false, rowEngine = false, rowToggles = false;
        float rowPulse[3] = {};
        // name tags (W10): how many were drawn last frame (alpha > 0.01), and the first one's box, for the capture proof
        int   nameTagsShown = 0;
        float nameTagX = 0, nameTagY = 0, nameTagW = 0, nameTagH = 0;
        // vitals cluster (bottom-right, same right edge and width as the speedometer, under it)
        bool  vitalsVisible = false;
        float vitalsX = 0, vitalsY = 0, vitalsW = 0, vitalsH = 0;
        // 0.4.22: the vitals strip under GTA's radar (SetRadarDock); then vitalsX..H is the strip. vitalsDocked: it is there (else
        // the bottom-right plate); vitalsDockWhy: the sync::DockWhy as an int (0 = docked); radarX..H: the radar rectangle the
        // layout used (0 when not docking); vitalsCoverPx: how far the strip reaches up into that rectangle (0 = wholly under it).
        bool  vitalsDocked = false;
        int   vitalsDockWhy = -1;                 // -1: not asked (dock off)
        float radarX = 0, radarY = 0, radarW = 0, radarH = 0;
        float vitalsCoverPx = 0;
        int   vitalsRows = 0;                     // always 2 (health, satiety); fuel is on the speedometer plate
        bool  vitalsRing = false;                 // 0.4.31: the vitals are the REV1 ring (vitalsX..H = its 288 x 352 component box, scaled)
        float ringPulse[2] = {1.0f, 1.0f};        // the ring's fill opacity from the critical pulse (HP, SAT): 1 outside critical
        int   healthShown = 0, satietyShown = 0, fuelShown = 0;   // percent as drawn (eased); fuelShown from the speedo
        bool  healthLow = false, satietyLow = false, fuelLow = false;   // below the warning threshold
    };
    UiState LastUiState();

    // How many ImGui assertions fired since the DLL loaded (each is logged, none
    // aborts; see overlay_imconfig.h). Zero in a healthy run.
    int AssertFailures();

}
