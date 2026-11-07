// radar_mask: 0.5.5 ring_map_round, the round mask for the ring's map (libs/sync/ring_round.h says why and holds the pure parts).
//
// How (one layer below FiveM's ADD_REPLACE_TEXTURE("platform:/textures/graphics", "radarmasksm", ...), on the shared D3D11 bind hook of
// brand_apparel.cpp, swap namespace):
//   1. script thread (engine.cpp TickRingRound): GET_TEXTURE_RESOLUTION gives the size of the game's radarmasksm / radarmasklg, then a CENSUS
//      (0.5.6; 0.5.5's bind-window probe hid the radar and failed live: every view of those sizes was bound in every window, and 64 entries
//      overflowed): for kCensusMinMs and kCensusMinFrames render frames the bind hook keeps every distinct view of those sizes in a format we can
//      rebuild (swap::CensusArm / CensusTake: bounded, and whatever it cannot keep is counted, never dropped silently). The radar is NOT hidden;
//      the engine also draws each mask as an invisible sprite (alpha 1, 1 % of the screen, under the ring's glass) so the game binds it.
//   2. render thread (Poll): every kept view is read back (a staging copy, at most kMaxInFlight in flight, kAnalysesPerFrame analysed per frame,
//      DO_NOT_WAIT: no stall), its FNV-1a 64 over all mips logged, and analysed (sync::ringround::AnalyzeStockMask: grey, bright centre, dark
//      edges, mirror-symmetric, flat inside). sync::ringround::PickMask then takes the view of each size that looks like a mask only if it is the
//      ONLY one in a COMPLETE census (or the only mask-like one carrying a pinned stock hash: kPinned in radar_mask.cpp); anything else (none,
//      several, an incomplete census) replaces nothing and says why. A circle mask of the same size / format / mip chain is built from the stock
//      centre texel (our own artwork: an ellipse inscribed in the texture, a 2 % ramp inside its edge);
//   3. swap::Replace registers our view INACTIVE: the hook counts the radar's binds of the stock view (the gate's evidence, ringround::RoundGate)
//      and hands out ours only while SetActive(true), which the engine sets together with the grown placement (the circle at the 0.5.4 size would
//      sit inside the 0.5.4 hole and leave a gap); swap::Counters also says how often the hook substituted and read it back.
// D3D11 only: on Enhanced (D3D12) nothing installs and the status says so. Only hashes of the stock masks are logged (never their pixels).
#pragma once

#include "sync/ring_round.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gage::game::radarmask {

    enum Kind : int { kSmall = 0, kBig = 1 };                       // radarmasksm (the minimap), radarmasklg (the big radar)
    constexpr const char* kTexName[2] = {"radarmasksm", "radarmasklg"};

    // the census and the readback pace (render thread)
    constexpr uint32_t kCensusMinFrames = 30;        // render frames recorded, and at least
    constexpr uint32_t kCensusMinMs = 1500;          // this long
    constexpr int kMaxInFlight = 3;                  // staging copies waiting for the GPU
    constexpr int kStartsPerFrame = 2;               // staging copies issued per frame
    constexpr int kAnalysesPerFrame = 1;             // views mapped, hashed, decoded and analysed per frame (512 x 512 BC1: well under 1 ms)
    constexpr uint32_t kReadbackTimeoutMs = 4000;    // a copy still busy after this is "not read back" (the census is then incomplete)

    enum class Phase : int { Idle = 0, Requested, Recording, Reading, Done, Failed };
    const char* PhaseName(Phase p);

    // Script thread. Enable(true) asks for the shared hooks (swap::Want); Enable(false) only stops new censuses (a replacement in place stays
    // registered, inactive unless SetActive says otherwise).
    void Enable(bool on);

    // Any thread: hand out our circle for every live replacement (true) or leave the game's own masks bound (false). Returns how many entries
    // changed state as asked.
    int SetActive(bool on);

    // Script thread: a census for the kinds with a size (0 x 0 = that kind is not looked for; a kind already live is kept as it is). The render
    // thread arms on its next frame (Recording), takes it after kCensusMinFrames / kCensusMinMs (Reading), reads every view back and picks (Done;
    // Failed only when the census could not be armed). verbose: log every candidate's analysis (else only the mask-like ones and the failures).
    bool RequestCensus(uint32_t wSmall, uint32_t hSmall, uint32_t wBig, uint32_t hBig, bool verbose);
    // Any thread: drop a census / readbacks in progress (render thread, next frame) and go Idle.
    void Cancel();

    struct KindStatus {
        bool probed = false;      // a size was given for it
        bool picked = false;      // the census for it has been decided (verdict valid)
        bool found = false;       // PickMask chose a view
        bool replaced = false;    // our circle is registered for it (handle valid in `generation`)
        int handle = -1;
        uint32_t w = 0, h = 0, mips = 0;
        sync::ringround::TexFmt fmt = sync::ringround::TexFmt::Unknown;
        uint64_t stockHash = 0;   // FNV-1a 64 over the chosen stock view's mips (logged, never the pixels)
        int views = 0, read = 0, shapes = 0;     // kept by the census / read back so far / looking like a mask
        uint32_t notRecorded = 0, otherFormat = 0;
        sync::ringround::PickVerdict verdict = sync::ringround::PickVerdict::NoViews;
        std::string why;          // what the census / pick / replacement said
    };
    struct Status {
        Phase phase = Phase::Idle;
        uint32_t generation = 0;  // swap::Generation() the handles belong to
        bool hooks = false, notD3D11 = false;
        KindStatus k[2];
        uint32_t censusFrames = 0, lost = 0;
        uint64_t censusBinds = 0;
        std::string why;          // the census as a whole
    };
    Status GetStatus();           // any thread (a copy)
    // True while `kind`'s replacement is alive (registered and the hooks not torn down since).
    bool Live(int kind);

    // Render thread, once per Present after brand::Poll (engine.cpp UiFrame). No-op until Enable(true).
    void Poll(void* swapChain);

    namespace testing {
        void RenderTick();        // what Poll does once the hooks are installed (brand_apparel_test drives it on a WARP device)
        void SetPins(int kind, const std::vector<uint64_t>& hashes);   // replace kind's pinned stock hashes (empty = the built-in ones)
    }

}  // namespace gage::game::radarmask
