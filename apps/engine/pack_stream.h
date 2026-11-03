// STATUS 2026-11-02: compile-verified, LIVE-UNVERIFIED. Gated OFF by default (HKCU pack_mount != 1).
//
// pack_stream: mount one already-built add-on DLC pack (e.g. the Jesko car) into GTA V Legacy
// 1.0.3889 at run time, THROUGH THE GAME'S OWN ExtraContentManager, without patching the game.
// This is the native-DLC route, distinct from the parked FiveM-signature route in dlc_mount.{h,cpp}
// (which mounts a single packfile + data files directly): a vehicle pack carries vehicles.meta,
// handling.meta, carcols/carvariations and the model, all of which the game wires up for free once
// its setup2.xml change-set runs. The call chain and every signature below come from the clean-room
// 3889 static analysis in docs/content-streaming/recon-3889.md and docs/content-streaming/
// p2-signatures.md (each pattern verified to match exactly once in the decrypted image); NO FiveM /
// OpenIV / CodeWalker source was used, and this file resolves every address itself by pattern so it
// survives ASLR.
//
// What it does, once, on the game thread after the player is in the world (mirrors the game's own
// boot loop, p2-signatures.md section 10):
//   1. mgr = *(ExtraContentManager**)(base + 0x1ec1eb0); verify non-null + vtable.
//   2. AddDlcPack(mgr, "<device-or-os-path>/")   -- the pack's dlc.rpf is appended to mgr's array.
//   3. DlcSetup2(entry, gen)                      -- temporary extra:/ mount, parse setup2.xml.
//   4. MountGate(entry, gen)                      -- the real mount (dlc.rpf becomes a device).
//   5. RunPackGroup(mgr, packHash, EARLY_ON) then (…, STARTUP)  -- run the pack's change set.
//   6. poll IS_MODEL_IN_CDIMAGE(hash(model)) until the pack's test model streams, or a timeout.
//
// THE TWO THINGS THE FIRST LIVE RUN DECIDES (both flagged, neither assumed):
//   (a) whether the retail 3889 archive loader accepts our pack's encryption. We ship AES-sealed
//       packs (the operator seals once with their own key); the game reads AES natively, but AES
//       acceptance for a *top-level add-on* dlcpack is unverified. An OPEN (unencrypted) pack is
//       REFUSED here with a logged reason -- retail raises a fatal err_gen_invalid on it and the
//       engine carries no patch. NG and AES are handed through.
//   (b) whether the path form resolves through the game's device layer. AddDlcPack returns false if
//       "<path>dlc.rpf" is not resolvable (p2 section 1). The proven form is "platform:/dlcpacks/
//       <name>/" (pack placed under the game dir); an absolute %LOCALAPPDATA% path is untested. Both
//       are driveable from the registry (pack_device / pack_path) without a rebuild, and every step
//       is logged so the first run says exactly where it stopped.
//
// STRATEGY B (pack_folder=1): mount a LOOSE folder, not an RPF. AddDlcPack still builds the entry (its
// Exists gate only needs a dlc.rpf file present in the folder; it is never opened), then the entry is
// overridden to a relative/folder device (Mount 0x94307c folder branch: +0xc8=1, +0xb8=the folder path),
// so DlcSetup2 reads setup2.xml/content.xml off disk and RunPackGroup registers the data-files + nested
// stream RPFs. This never calls fiPackfile::Open and never touches the encryption gate -- the clean route
// for an OPEN add-on pack. Model files still live in a nested x64/vehicles.rpf (the stock pipeline has no
// loose-model path); whether that nested OPEN RPF is accepted is what the first folder-mode run decides.
//
// Registry (all under HKCU\Software\GAGE-MP, strings):
//   pack_mount  = "1"  to arm this at all (default off: Tick is a no-op).
//   pack_folder = "1"  strategy B: loose-folder device instead of a packfile mount (default off = packfile).
//   pack_name   logical pack name, default "gage_jesko" (names the %LOCALAPPDATA% folder).
//   pack_path   absolute path to the pack's dlc.rpf; default
//               %LOCALAPPDATA%\GAGE-MP\packs\<pack_name>\dlc.rpf (what the launcher will drop).
//   pack_device path string handed to AddDlcPack verbatim (must end in '/'); when unset, the folder
//               of pack_path is used as a forward-slash OS path. Set this to
//               "platform:/dlcpacks/<name>/" for the proven device form.
//   pack_model  model name verified present after the mount, default "jesko".
//
// Threading: Tick() is game-thread only (the script tick), like dlc::Tick / World::ApplyPedOutfit.
#pragma once

#include "hook.h"

namespace gage::game::content {

    enum class Status {
        Idle,      // off (pack_mount != 1), not in the world yet, or not reached the mount
        Pending,   // the chain ran; waiting for the pack's model to appear in the cdimage
        Ready,     // verified: the pack's model is in the cdimage
        Failed,    // gave up for good (FailReason()); nothing is retried
    };

    // Game thread (script tick) only. Call every tick while the local player is in the world. Gated by
    // HKCU pack_mount=1 (default off => always Idle). The first eligible call resolves the 3889
    // ExtraContentManager signatures and logs them, then (if a pack is present) runs the mount chain
    // once and begins polling; later calls poll until Ready or a timeout. Cheap once settled.
    Status Tick(World& world);

    // Why Failed (static buffer, valid until the next failure). "" when not Failed.
    const char* FailReason();

}  // namespace gage::game::content
