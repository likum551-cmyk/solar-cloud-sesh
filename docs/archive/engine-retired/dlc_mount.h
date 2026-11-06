// STATUS 2026-11-01: NOT built, NOT wired (no CMake entry, nothing calls Tick/Recon) and NOT the route the logo ships on.
// The branded tee is brand_apparel.{h,cpp} (D3D11 texture swap, no game signatures). Why this route is parked: retail GTA V
// rejects an OPEN-encrypted RPF7 (FiveM HookInitialMount.cpp "Detected modified game files ... encryption type OPEN"), the pack
// lacks the ymt/ydd/ytd assets (content/dlc/gage_apparel/tools/pack_dlc_rpf.mjs REQUIRED_ASSETS), and 14 signatures below are
// unverified on 3889. Kept as the fallback if the texture swap ever has to give way to a real streamed add-on.
//
// dlc_mount: put the GAGE-MP logo tee into GTA V Legacy 1.0.3889 at run time, without touching the
// game install. The pack is an add-on clothing DLC (content/dlc/gage_apparel) that the engine mounts
// into the running game the way FiveM mounts packfiles and registers data files:
//
//   1. fiPackfile ctor + OpenPackfile + Mount("dlc_gage_apparel:/")   (the pack's dlc.rpf becomes a device)
//   2. the pack's inner ped archive through the RPF_FILE data-file path (addPackfile, with the manifest
//      chunk calls FiveM wraps around it)
//   3. the pack's shop meta through CDataFileMount::sm_Interfaces[SHOP_PED_APPAREL_META_FILE]->LoadDataFile
//   4. count the freemode tops (GET_NUMBER_OF_PED_DRAWABLE_VARIATIONS) before and after: the new drawable
//      index is the old count, and only a count that grew by exactly the pack's size is accepted.
//
// Every game address comes from a signature taken from the FiveM source (citations next to each one in
// dlc_mount.cpp). None of them has been confirmed on the decrypted 3889 image yet: Recon() / StartReconThread()
// log, per signature, whether it resolves UNIQUELY, and nothing is mounted unless every required one does
// AND HKCU\Software\GAGE-MP\dlc_mount = "1" (default off). See the report that came with this file for what is
// still unknown (above all: the retail game rejects an unencrypted OPEN archive).
//
// Threading: Recon/StartReconThread are read-only and may run on any thread once the image is decrypted.
// Tick() is game-thread only (the script tick), like World::ApplyPedOutfit.
#pragma once

#include "hook.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace gage::game::dlc {

    // ePedVarComp 11: the top, the slot the logo tee goes into.
    constexpr int kTopComponent = 11;

    // What the pack adds to the freemode male tops (content/dlc/gage_apparel): one drawable with one
    // texture. A drawable count that grows by anything else is rejected, never trusted.
    constexpr int kPackTopDrawables = 1;
    constexpr int kPackTopTextures  = 1;

    // ---- recon (read-only) ---------------------------------------------------------------------
    struct ReconSummary {
        int  unique = 0, ambiguous = 0, missing = 0, bad = 0;   // over all signatures
        bool mountPathReady = false;   // every REQUIRED signature resolved uniquely and passed its check
    };

    // One pass over the live image: for each signature, log UNIQUE (with the image offset and the bytes
    // at the site) / ambiguous / missing, then log the packfile-encryption leads (see dlc_mount.cpp).
    // Installs nothing and calls nothing in the game. `pass` is only used in the log lines.
    ReconSummary Recon(int pass);

    // Background thread: Recon() at ~15 s, 45 s, 75 s and 120 s (the image decrypts in stages), then ends.
    // Call once after the engine is up. Idempotent.
    void StartReconThread();

    // ---- mount + resolve -----------------------------------------------------------------------
    struct Options {
        // The pack archive. Empty = <folder of gage_mp.dll>\dlc\gage_apparel\dlc.rpf.
        std::wstring packPath;
        // The retail 3889 executable raises err_gen_invalid on an archive whose header says OPEN
        // (FiveM rage-device-five HookInitialMount.cpp, PackfileEncryptionError). Leave this false until
        // the engine carries a verified patch that makes the game accept OPEN; an OPEN pack is then
        // refused with a logged reason instead of reaching the game. An NG or AES pack needs no patch.
        bool openArchivesPatched = false;
    };
    void Configure(const Options& options);

    enum class Status {
        Idle,      // not started: switch off (dlc_mount != 1), no freemode ped yet, or Tick not reached the mount
        Pending,   // mounted and registered, waiting for the game to publish the new drawable
        Ready,     // verified: *outTop is the logo top
        Failed,    // gave up for good (FailReason()); the base tee stays
    };

    // Game thread (script tick) only. Call every tick while the freemode ped is the local player, with its
    // handle. The first call with a freemode ped records the baseline drawable count, mounts and registers
    // the pack, then each call polls until the count has grown by exactly kPackTopDrawables (or ~15 s pass).
    // On Ready, `*outTop` is {kTopComponent, drawable, 0, ...} ready to replace slot 11 of the outfit.
    // Cheap once settled (Ready/Failed return at once).
    Status Tick(World& world, int32_t ped, PedComponent* outTop);

    // True exactly once after the status first becomes Ready: the caller re-dresses the ped then.
    bool ConsumeReadyEdge();

    // Why Failed (static string or a small buffer; valid until the next failure). "" when not Failed.
    const char* FailReason();

}  // namespace gage::game::dlc
