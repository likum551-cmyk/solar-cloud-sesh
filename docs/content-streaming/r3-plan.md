# R3 — runtime content streaming (the FiveM way), 3889 Legacy

**Goal.** Load an add-on vehicle (`jesko`) into the running game **without modifying the user's GTA install** — no files written to the game folder, no `dlclist.xml`/`dlcpacks`, no OpenIV/ClosedIV/RageOpenV ASI. Our already-injected engine (`gage_mp.dll`) registers the model into the game's streaming system **at runtime**, serving the resource bytes from our own cache (`%LOCALAPPDATA%\GAGE-MP\packs\<name>\`). This is FiveM's model (CfxCollection): inject + in-memory streaming, zero disk modification of the game.

## Hard constraints (read before writing a line)
- **CLEAN-ROOM.** Every fact below was derived from **our own RE of our own runtime dump of our own game copy**. Do **NOT** read or copy from FiveM/CitizenFX, OpenIV, ClosedIV/RageOpenV, CodeWalker, or any other third-party source. Implement against this spec only.
- **No DRM-gate patch.** We do NOT patch the game's encryption/anti-mod check. R3's whole point is to bypass `fiPackfile::Open` (and therefore the OPEN-rejection INT3 at `+0x136af5b`) by serving bytes through our own collection, never opening an on-disk packfile.
- **No game-install writes.** Bytes come from our cache via our collection/device object in memory.
- **Engine is Windows + game-hooked.** Cloud sessions can author code and portable unit tests, but **cannot build `gage_mp.dll`** (needs the local WinLibs toolchain + integrity bake) or **run GTA**. Build + live-test happen only on the owner's machine.
- Addresses are **RVAs** on 3889 Legacy. The decrypted dump (`%LOCALAPPDATA%\GAGE-MP\dump\GTA5.exe-65387520-*.img`, file-offset==RVA, SizeOfImage `0x3e5bc00`) stays **local** — it is a decrypted copyrighted binary and must never be committed/pushed. Resolve all addresses at runtime by `imageBase + RVA` (ASLR); prefer byte-pattern sigs for anything shipped.

## Status at a glance
- **DONE, in-repo, offline-proven:** `apps/engine/pack_collection.{h,cpp}` + `tests/pack_collection_test.cpp` (52/52) — the RPF7/RSC7 TOC + byte-layout data layer (pure, no game calls).
- **PINNED (RE complete):** the store/slot/handle/registry/construct machinery (below).
- **OPEN (needs LOCAL dump-RE + live validation, NOT cloud-codeable blind):** the **custom collection→async-streamer device contract** (the one genuinely hard piece), the **archetype manager** (name→CVehicleModelInfo), and the **TXD type descriptor**.

---

## Pinned RE facts (3889)

### Stores (rage fwAssetStore / strStreamingModule)
- **g_FragmentStore** = RVA `0x2f5d690`, vtable `0x1bb4190`, asset type `0xa2` (fragType / `.yft`).
- **g_TxdStore** = RVA `0x2f5da20`, vtable `0x1bb3bd0`, type `0xd` (`.ytd`).
- Store header: `+0x08` dword = **base stream index** (global = baseIndex + localSlot); `+0x38` = slot pool ptr; `+0x40` = per-slot status byte array (**bit `0x80` set = empty/not-loaded**); `+0x4c` dword = **elem size `0x18`**.
- Slot (0x18 bytes): `+0x00` object ptr (fragType*/txd*, set on load); `+0x08` nameHash | load flags; `+0x0c` per-asset hash (NOT the model name); `+0x10` secondary index (`-1` when none); `+0x14` flags.
- **Model names are NOT in the store pool** — the name→slot link lives in the archetype (CVehicleModelInfo), see below.

### Store methods (verified vtable slots / addrs)
- joaat: `0x136f418(rcx=0, rdx=name)` → eax = hash.
- FindSlot: `0x6903dc(rcx=store, rdx=&out, r8=&hash)` → `[out]` = index or `0xffffffff`.
- AddSlot (get-or-add) = **vtable slot 45 (+0x168) = `0x604164`**(rcx=store, rdx=&out, r8=&hash) → `[out]` = index; inits the slot (`+0xc`=hash, `+0x10`=-1, flags &=0xfff8).
- GetSlot = slot 19 (`0x16963cc`) — confirms the layout above.
- Load = slot 11 (`0x16b6ae8`); PlaceResource = slot 42 (+0x150, `0x16b7a0c`); **SetResource (bind) = slot 43 (+0x158, `0x16c2084`)**; frag↔txd dependency link = slot 12 (`0x16b8d40`).

### Image registry + handle encoding
- **handle = (imageIndex << 16) | fileIndex** (confirmed via resolver `0x13545f0`).
- Image registry = `0x2c67400` (array of `fiCollection*`, indexed by imageIndex); cap dword = `0x1b81588` (= 4440 live); index allocator = `0x1b82890`; lock = `0x2c15998`.
- Register-as-image: `fiPackfile::ctor+register` = `0x1360af0` (zero-inits, sets vtable `0x1b82890`, scans `0x2c67400` [1,4440) for a null slot, stores imageIndex at obj `+0x92` WORD, registry[idx]=obj). Register write site `0x1360b97`; unregister `0x1360dab`.
- **Streaming-info manager** = `0x2ea23d0`: array=`[mgr+0]`, count=`[mgr+8]`, **8-byte entries `(u32 handle, u32 flags)`**. Entry for local slot L of a store = `mgrArr[store[+0x08] + L]`. Live flag values: frag `0x28010001`, txd `0x08000000` (bit27 = present/in-image; low byte = load status).

### fiPackfile / fiCollection vtable `0x1b82890` (clone target; full 56 slots dumped in memory [[content-streaming-route]])
- `+0x188` (`0x1364698`) GetEntry(coll, handle): `&entryTable[handle & 0xffff]`, stride `0x10`, table at `coll+0x20`.
- `+0x190` (`0x136471c`) resource read: device=`[coll+0x50]`, byteOffset=`(q0>>40)<<9` (sector), size from entry; tail-calls `0x1363cf8`.
- `+0x198` (`0x13646a4`) GetName (name heap at `coll+0x10`).
- Object members: `+0x10` name-heap base, `+0x20` entry table, `+0x28/+0x30` counts, `+0x50` backing fiDevice, `+0x58` base offset, `+0xb8` sector shift, `+0x92` imageIndex.

### Entry format (CONFIRMED on real jesko rpf; implemented in `pack_collection`)
`q0 = NameOffset[0:16] | FileSize24[16:40] | C[40:64]`; `C = resourceBit(23) + sectorOffset(0:22)`; `byteOffset = (C & 0x7fffff) << 9`. Dir: `C&0x7fffff==0x7fffff`, `q1=(childCount<<32)|firstChild`. Binary: `q1` low32 = uncompressed size, hi = `0x0fefffff`. Resource: `q1` low32 = SystemFlags, hi = GraphicsFlags.

### Construct spine (produces a typed object from a loaded resource; all verified in-dump)
`PlaceResource` slot42 `0x16b7a0c` → wrapper `0x16a533c` → generic ctor `0x1354d0c` (ctx setup `0x1353820`, builder `0x13534e0`, finalize `0x1353c20`) → producer `0x13539c8` → object call **`0x1356850` is HANDLE-driven** (ecx = imgIdx<<16|entry; registry `0x2c67400`; type-descriptor table `0x2c159c0 + type*0x4570`). fragType descriptor `0x1bb5478`, type `0xa2`. **There is no raw-bytes→object entry — construction goes through a registered image + device.** This is why R3 (a registered custom collection) is required, and why the "hand-build a datResourceMap in RAM" idea (M2) was abandoned.
- **RSC7 page-flag decoder** = `0x1370140`(rcx=&{sysFlags,gfxFlags}, rdx=out[2]): baseShift = bits0-3 (base page = `0x200<<shift`); page counts at bit4(`<<8`), bits5-6(`<<7`), bits7-10(`<<6`), bits11-16(`<<5`), bits17-23(`<<4`), bits24-27(`<<3..<<0`); total size = Σ count·(base<<i). Callable, or re-implement offline (testable).
- The game statically links **zlib ~1.2.x** (inflate.c error strings at RVA ~`0x1b7a4e0`) — the game inflates RSC7 payloads itself; **no miniz dependency needed**.

### Archetype (so `REQUEST_MODEL('jesko')` resolves)
- `CVehicleModelInfo::Init` = `0x657661`(rcx=modelinfo, rdx=char* name): auto-resolves frag/txd slots **from the name** (`FindSlot`/AddSlot on `name`, `<name>_hi`), writes **`+0x4c8` = fragment slot idx**, **`+0x4cc` = txd slot idx**, `+0x210/+0x214` hi-txd, binds parent txd (`0x95d364`).
- CBaseModelInfo generic asset access `0x16cb410`: `+0x60` asset type byte, `+0x64` slot index.
- **Clone-adder MVP:** clone an existing `adder` CVehicleModelInfo, repoint `+0x4c8/+0x4cc` to our jesko slots, register under `joaat('jesko')` → jesko spawns & drives with adder physics (Stage 1); swap in the pack's real metas later (Stage 2).

### jesko resources (owner's own source `DLC PACKS/jesko/dlc.rpf`, OPEN, no key)
`jesko.yft` (type `0xa2`, sys `0xa3022403` gfx `0x20000000`), `jesko.ytd` (type `0xd`, sys `0x00060000` gfx `0xdb100008`), `jesko_hi.yft` (`0xa2`). Model/txd/handling name = `jesko`. Extractable loose (no key) with the scratch parsers; the game does the inflate.

---

## THE open gap (do not blind-implement — needs local dump-RE + live)
`0x136471c` forms the resource read as `0x1363cf8(device=[coll+0x50], byteOffset=sector<<9, size)`. In our runtime the paging sentinel `[0x1e2ee00] == -1`, so `0x1363cf8` returns a **2 KB sector/streaming address**, not a data pointer — resource bytes are pulled **asynchronously by the streamer from the collection's device, by sector**. So:
- A RAM blob behind the game's **shared** device is dead (the shared device is sector/cache-addressed, not "read N bytes at offset").
- R3 must provide a **custom fiDevice** (cloned from the device vtable; byte delivery already proven by the relative device) that the streamer actually calls to fetch our bytes for a sector — i.e. satisfy the exact method the async streamer invokes on the collection's `+0x50` device. **This method/contract is the one remaining RE item, and it needs the local dump + a live golden read of a real base-game vehicle (resolve `adder`'s frag slot → global index → its streaming-info entry + the device call the streamer makes).**

## Implementation plan
1. **(offline, cloud-codeable now)** Port the RSC7 page-flag decode (`0x1370140` logic) into `pack_collection` as a pure function + golden tests using the jesko sys/gfx flags above. Extend the collection builder to emit the in-RAM image (entry table + name heap + sector-aligned blob) a custom collection will expose.
2. **(LOCAL RE)** Pin the async-streamer device contract (the open gap) via one read-only live golden-vector run on a stock vehicle.
3. **(engine, local build)** Build the custom `fiCollection`/`fiDevice` object (reuse vtable `0x1b82890` where safe; override only the read/entry methods our bytes need), register it into `0x2c67400` for an imageIndex (via `0x1360af0` or a careful replicate), `AddSlot('jesko')` in both stores, write `mgrArr[baseIdx+slot] = ((imgIdx<<16)|fileIdx, flags)`, clear the status byte.
4. **(engine, local build)** Archetype: clone `adder`'s CVehicleModelInfo, repoint `+0x4c8/+0x4cc`, register under `joaat('jesko')` (needs the archetype-manager RE — still open).
5. **(local live)** `IS_MODEL_IN_CDIMAGE('jesko')` → `/car jesko`. Validation shortcut: **hijack adder's resident frag+txd slot** (SetResource jesko's objects into adder's slots) → `/car adder` spawns jesko, skipping archetype work entirely for a first proof.

## Build / wiring notes
- All R3 engine code goes in `apps/engine/pack_stream.cpp` (has `reg::`), gated behind a registry flag (default off).
- `pack_collection` is wired into `CMakeLists.txt` after `gameplay_test`; keep its tests green.
- **6 unconditional diagnostic one-shots in `hook.cpp` (`streamdiag`, `storewalk`, `vehdiag2`, `fidiag`, `entdiag`, `cdiag`) MUST be removed before ship.** They are the live-RE tooling and only run on the owner's machine.
- The folder-mode path in `pack_stream.cpp` (OPEN nested rpf) is a **dead route** (retail INT3-traps the OPEN nested packfile) — kept for its mount RE only; R3 replaces it.

## Cloud vs local split (for this repo's workflow)
- **Cloud can do now:** step 1 (RSC7 decode + collection image builder + golden tests), review/plan, draft the engine class skeleton against this spec.
- **Local only (owner):** step 2 (dump-RE of the device contract), steps 3–5 build + live-test, anything needing the dump or GTA running. Never commit the dump or `DLC PACKS/jesko`.
