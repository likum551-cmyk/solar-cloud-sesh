// STATUS 2026-11-05: the loose route is LIVE-proven (Jesko, Legacy 3889); the NG dlc.rpf route is live-untested. On by default when a pack folder is present (pack_mount below), Legacy GTA5.exe only.
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
// for an OPEN add-on pack. A nested OPEN x64/vehicles.rpf is NOT accepted (live 2026-11-02: the game's INT3
// at +0x136af5b), so in folder mode the models sit LOOSE in <pack>/stream/ (*.yft/*.ytd/*.ydr/*.ydd) and
// are registered, before the change set runs, through the game's own loose-file streaming routine
// (RVA 0x169e4d4 -> image 0 = the RawStreamer collection); content.xml then lists only the data metas.
// vehicles.meta's CVehicleModelInfo::Init finds the fragment/txd slots by name, already bound to the files.
//
// MANY PACKS (2026-11-05): every folder under %LOCALAPPDATA%\GAGE-MP\packs\ with a dlc.rpf is a pack, mounted one per
// tick in name order (tools/packs/stage_packs.mjs lays them out from the operator's DLC PACKS folder):
//   <pack>\setup2.xml present  -> loose folder (OPEN packs: metas loose, models in stream\, dlc.rpf only as the gate)
//   otherwise                  -> the dlc.rpf itself through the game's own loader (NG / AES packs; OPEN is refused)
// Each pack's cars come from its content.xml -> VEHICLE_METADATA_FILE -> <modelName> (pack_meta.h): off disk for a
// loose pack, through the game's device for a dlc.rpf pack. They are polled into the cdimage and listed for the menu.
//
// Registry (all under HKCU\Software\GAGE-MP, strings):
//   pack_mount  = "1" arms it, "0" keeps it off; unset = armed when a pack folder is present (0.4.14). Never on a game
//                 that is not Legacy GTA5.exe (Enhanced is a different binary: none of these signatures apply).
//   pack_name   = <folder>  mount only that pack (default: all of them).
//   pack_probe  = "1"  diagnostic: mount under extra:/ and list the visible files instead of running the change set.
//   pack_ng_tune = "0" an NG pack's gage_tune.json is ignored (default on, 0.4.18): otherwise its HANDLING_FILEs are read
//                  through the mount, tuned (handling_tune.h) and re-defined by one loose override pack written to
//                  %LOCALAPPDATA%\GAGE-MP\tune\gage_tune\ and mounted after every other pack.
//
// Threading: Tick() and PackOfModel() are game-thread only (the script tick), like dlc::Tick / World::ApplyPedOutfit;
// Vehicles() is a locked copy for any thread (the overlay's car browser).
#pragma once

#include "hook.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gage::game::content {

    // One car a mounted pack carries.
    struct PackVehicle {
        std::string model;    // the /car name
        std::string name;     // what the menu shows: "BMW M5" (pack_meta.h BuildCarList; unique in the list)
        std::string make;     // vehicles.meta <vehicleMakeName>
        std::string label;    // vehicles.meta <gameName>
        std::string vclass;   // vehicles.meta <vehicleClass> (VC_SUPER, ...)
        std::string pack;     // the pack folder name, lower case
        bool ready = false;   // IS_MODEL_IN_CDIMAGE said yes
    };

    // Every car of every mounted pack, sorted by name; a model two packs both define is listed once, for the
    // pack that mounted first. Any thread (a locked copy).
    std::vector<PackVehicle> Vehicles();

    // Changes whenever Vehicles() would return something new (a pack mounted, a car reached the cdimage). Any thread.
    uint32_t VehiclesGeneration();

    // The pack folder name (lower case) a model hash came from, or nullptr. Game thread.
    const char* PackOfModel(uint32_t modelHash);

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

    // Raise the game's in-memory MaxExtraVehicleModelInfos (gameconfig) before it creates the fixed DLC vehicle
    // array, so the served packs' cars fit next to Rockstar's DLC cars (0.4.15 overflowed it on the 4th pack).
    // Call at engine attach and retry (any thread) until it returns true = decided (raised, already created, off, or
    // unresolvable). veh_store_extra (HKCU\Software\GAGE-MP) = extra slots, default 256, 0 = off. Packs that do not
    // fit are refused in the mount, never written past.
    bool PrepareVehicleStore();

    // Why Failed (static buffer, valid until the next failure). "" when not Failed.
    const char* FailReason();

}  // namespace gage::game::content
