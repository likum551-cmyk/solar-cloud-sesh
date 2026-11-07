// radar_mask: 0.5.5 ring_map_round, the round mask for the ring's map (libs/sync/ring_round.h says why and holds the pure parts).
//
// How (one layer below FiveM's ADD_REPLACE_TEXTURE("platform:/textures/graphics", "radarmasksm", ...), on the shared D3D11 bind hook of
// brand_apparel.cpp, swap namespace):
//   1. script thread (engine.cpp TickRingRound): GET_TEXTURE_RESOLUTION gives the size of the game's radarmasksm / radarmasklg, then a PROBE:
//      the radar is held hidden for about two seconds while DRAW_SPRITE draws each mask (alpha 1, under the ring's glass) in its own windows of
//      ten ticks and nothing in the windows between; the bind hook records every view of those sizes the game binds, per window;
//   2. render thread (Poll): the view bound in exactly its texture's windows IS the game's own mask (no fingerprint of Rockstar pixels needed,
//      the size and a mask-shape check on the read-back guard the choice); it is read back (staging copy, no stall), checked to look like a
//      mask (grey, bright centre, dark edges; sync::ringround::AnalyzeStockMask), and a circle mask of the same size / format / mip chain is
//      built from the stock centre texel (our own artwork: an ellipse inscribed in the texture, a 2 % ramp inside its edge);
//   3. swap::Replace registers our view INACTIVE: the hook counts the radar's binds of the stock view (the gate's evidence that the game really
//      draws the radar with it, ringround::RoundGate) and hands out ours only while SetActive(true), which the engine sets together with the
//      grown placement (the circle at the 0.5.4 size would sit inside the 0.5.4 hole and leave a gap); swap::Counters also says how often the
//      hook substituted and read it back.
// D3D11 only: on Enhanced (D3D12) nothing installs and the status says so. The stock mask's FNV-1a 64 hash over all its mips is logged (never
// its pixels), so a later round can pin it as a fingerprint.
#pragma once

#include "sync/ring_round.h"

#include <cstdint>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace gage::game::radarmask {

    enum Kind : int { kSmall = 0, kBig = 1 };                       // radarmasksm (the minimap), radarmasklg (the big radar)
    constexpr const char* kTexName[2] = {"radarmasksm", "radarmasklg"};

    enum class Phase : int { Idle = 0, ArmRequested, Armed, FinishRequested, Done, Failed };
    const char* PhaseName(Phase p);

    // Script thread. Enable(true) asks for the shared hooks (swap::Want); Enable(false) only stops new probes (a replacement in place stays
    // registered, inactive unless SetActive says otherwise).
    void Enable(bool on);

    // Any thread: hand out our circle for every live replacement (true) or leave the game's own masks bound (false). Returns how many entries
    // changed state as asked.
    int SetActive(bool on);

    // The probe handshake. RequestArm: sizes of the two masks (0 x 0 = not probed); the render thread arms on its next frame (phase Armed).
    // RequestFinish: windows 1..n were spent on kindOfWindow[i] (kSmall, kBig, or -1 = nothing drawn); the render thread picks the views,
    // reads them back and replaces them (phase Done or Failed, Status says what happened). Reset: back to Idle (a new probe may follow).
    bool RequestArm(uint32_t wSmall, uint32_t hSmall, uint32_t wBig, uint32_t hBig);
    bool RequestFinish(const int* kindOfWindow, int n);
    void Reset();

    struct KindStatus {
        bool probed = false;      // a size was given for it
        bool found = false;       // one view matched its windows exactly
        bool replaced = false;    // our circle is registered for it (handle valid in `generation`)
        int handle = -1;
        uint32_t w = 0, h = 0, mips = 0;
        sync::ringround::TexFmt fmt = sync::ringround::TexFmt::Unknown;
        uint64_t stockHash = 0;   // FNV-1a 64 over the stock mips (logged, never the pixels)
        std::string why;          // what the probe / analysis said
    };
    struct Status {
        Phase phase = Phase::Idle;
        uint32_t generation = 0;  // swap::Generation() the handles belong to
        bool hooks = false, notD3D11 = false;
        KindStatus k[2];
        std::string why;          // the probe as a whole
    };
    Status GetStatus();           // any thread (a copy)
    // True while `kind`'s replacement is alive (registered and the hooks not torn down since).
    bool Live(int kind);

    // Render thread, once per Present after brand::Poll (engine.cpp UiFrame). No-op until Enable(true).
    void Poll(void* swapChain);

    namespace testing {
        void RenderTick();        // what Poll does once the hooks are installed (brand_apparel_test drives it on a WARP device)
    }

}  // namespace gage::game::radarmask
