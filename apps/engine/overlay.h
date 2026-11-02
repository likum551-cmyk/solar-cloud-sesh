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
        bool  loaderSceneTextured = false;        // the real GTA V stills are behind the loader
        float loaderSceneMix = 0;                 // 0: first still, 1: second; between the two is a crossfade
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
        // speedometer (bottom-right; speedShown is the eased number on screen, speedLit of
        // speedSegments gauge bars are lit)
        bool  speedoVisible = false;
        float speedoX = 0, speedoY = 0, speedoW = 0, speedoH = 0;
        int   speedShown = 0, speedLit = 0, speedSegments = 0;
        // vitals cluster (bottom-right, same right edge and width as the speedometer, under it)
        bool  vitalsVisible = false;
        float vitalsX = 0, vitalsY = 0, vitalsW = 0, vitalsH = 0;
        int   vitalsRows = 0;                     // always 2 (health, satiety); fuel is on the speedometer plate
        int   healthShown = 0, satietyShown = 0, fuelShown = 0;   // percent as drawn (eased); fuelShown from the speedo
        bool  healthLow = false, satietyLow = false, fuelLow = false;   // below the warning threshold
    };
    UiState LastUiState();

    // How many ImGui assertions fired since the DLL loaded (each is logged, none
    // aborts; see overlay_imconfig.h). Zero in a healthy run.
    int AssertFailures();

}
