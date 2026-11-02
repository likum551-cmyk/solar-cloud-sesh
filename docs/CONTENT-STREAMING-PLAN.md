# Content streaming: server packs downloaded and loaded automatically (plan, 2026-11-02)

Goal (GI): a player installs nothing. The launcher shows "SOL needs 12 MB of game content (Jesko). Download and play?", downloads on Agree, and the game starts with the pack loaded; the GTA folder is never touched (no OpenIV, no ASI loader). First pack: `jesko` (commissioned by Solar RP via Fiverr, 2022; model name `jesko`; RPF7, encryption OPEN; contains setup2.xml, content.xml, vehicles/handling/carvariations/carcols/dlctext meta, jesko.yft, jesko_hi.yft, parts).

Done = on a clean player PC, joining SOL downloads the pack after consent, `/car jesko` spawns it for that player, a second player sees it, and the GTA folder is byte-identical before and after.

## Research summary (sources in the 2026-11-02 research report)
- No permissively licensed game-side implementation exists. GPL (RageOpenV, RpfStreamer), unlicensed (ClosedIV, CodeWalker) and FiveM-derived code (including the signatures in the parked `apps/engine/dlc_mount.cpp`) are excluded: clean room, our own reverse engineering on the 3889 image.
- Archive side: RatPrez/ragekit (MIT) reads/writes RPF7 OPEN/AES/NG and finds the AES key in the game executable by SHA-1; VIRUXE/rpf-archive-rs (Unlicense). Port with attribution.
- Only the RPF entry table is encrypted; re-sealing a pack as AES takes milliseconds. The AES key is found in the player's own running game image and kept in memory only (never written, never shipped). NG needs CodeWalker's magic.dat: not used.

## Phases (each ends with evidence; the next starts only after it)
**P0. Image for recon (GI, one run).** GTA 3889 decrypts in memory, so static work needs a dump: set `HKCU\Software\GAGE-MP` value `enh_dump` = `1` (string), play until the world shows, quit. The engine writes `%LOCALAPPDATA%\GAGE-MP\dump` (file offset = RVA). Set it back to `0` afterwards.

**P1. Recon (read-only, on the dump).** Find ourselves, with byte patterns that resolve uniquely on the dump: the packfile open + mount path (anchors: the RPF7 encryption words 0x0FEFFFFF / 0x0FFFFFF9 / 0x4E45504F, `dlcpacks:`, `setup2.xml`, `content.xml`), the extra-content (DLC) registration path that reads setup2.xml/content.xml, the per-type data-file mounter table (VEHICLE_METADATA_FILE, HANDLING_FILE, CARCOLS_FILE, VEHICLE_VARIATION_FILE, TEXTFILE_METAFILE, RPF_FILE), and where the header parse rejects OPEN. Output: docs/content-streaming/recon-3889.md with every address, pattern, uniqueness count and caller chain. Native hashes (e.g. EXECUTE_CONTENT_CHANGESET_GROUP_FOR_ALL) only via the lookup tool.

**P2. Engine test build (GI live test).** Behind `HKCU\...\pack_mount=1` (default off): read packs from `%LOCALAPPDATA%\GAGE-MP\packs\<name>\dlc.rpf`, after content init mount each one. Route A: seal as AES (key from the image) and mount; route B (only if A is refused): patch the OPEN rejection in memory for our mount only. Register its data files, enable its change set, verify `IS_MODEL_IN_CDIMAGE(jesko)` and `IS_MODEL_A_VEHICLE`, log every step. GI copies `jesko\dlc.rpf` into the packs folder by hand for this one test, then `/car jesko`. Go/no-go for the feature.

**P3. Server manifest + launcher download + consent.** server.json `packs: [{name, url, size, sha256}]` served as a signed `packs.json` (same ECDSA key as the updater) under solar-rp.com/packs/; the UDP query (v2) carries the pack-set hash so the launcher knows when to check. Launcher: consent card (impeccable + visual review), download with progress and resume, SHA-256 verify, atomic move into the packs folder, PLAY blocked until complete, cleanup of packs no longer listed. release.sh / a new publish-packs.sh uploads packs.

**P4. Server model rules.** The server refuses spawning a model from a pack the server does not list; hyper tune (libs/sync/hypertune.h) already covers jesko.

## Risks
- P1 may find the mount path needs a stage of boot we cannot reach after injection: then mounting must happen earlier through the launch shim (more work, still no game-folder change).
- Retail may refuse AES at the top level of a dlc pack (route B then).
- Deriving the key from the player's own executable and patching the loader in memory is what OpenIV/CodeWalker-style tools do for interoperability; nothing is written to the game folder and the key never leaves the process. GI's decision to proceed (2026-11-02).
- The model is branded after a real manufacturer (Koenigsegg): GI's call on naming.
