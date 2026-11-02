# P2: calling signatures for the extra-content functions (GTA V Legacy 1.0.3889)

Clean-room static analysis of the decrypted image `GTA5.exe-65387520-252545640.img`
(FILE OFFSET == RVA), read-only, nothing launched. Scope: x64 register/argument
facts only. The RPF header parse, encryption word, OPEN-rejection and key code
were not read: where a function calls into that area it is listed as
"calls <addr>" and left alone (`0x1365a80`, `0x1366fec`, `0x13659c8`, `0x1366f4c`).

Tags: **OBSERVED** = read directly from the disassembly. **INFERRED** = deduced
from shape. Scratch scripts: `%TEMP%\x\p2sig\` (`verify.py`, `refs2.py`, `acc.py`, `lits.py`).

All RVAs. Runtime address = `GetModuleHandleW(nullptr) + RVA`. x64 Windows has one
calling convention (rcx, rdx, r8, r9, then stack; 32-byte shadow space owned by the caller).

## 0. Corrections to recon-3889.md (read these first)

1. **The object AddDlcPack / the pack loop operate on is the pointer at `.data 0x1ec1eb0`, NOT `0x1ec1ea8`.** (OBSERVED)
   Chain: boot function `0x187b4` does `mov rcx,[0x1ec1eb0]` then `call 0x940fbc` (site `0x189cd`/`0x189d4`);
   `0x940fbc` never touches rcx and forwards it to `0x91d7c4` -> `0x92955c` (`rdi=rcx`) -> `0x963ed0` (`rdi=rcx`)
   -> `AddDlcPack(rcx=rdi, ...)`, and `0x92955c` -> `0x93ff40(rcx=rdi)`. The pack array lives at `this+0x28`
   (count WORD at `this+0x30`, stride `0xf0`), which is what `0x92f46c` walks. `0x1ec1ea8` is a different,
   smaller object (see section 6); `0x940fbc` only sets a byte at `[0x1ec1ea8]+0xf0` around the scan.
2. `Mount` (`0x94307c`): the mount-point string is in **r8**, rdx is unused; it returns an **int where 0 = success**
   (recon said bool 1 = success; that is inverted).
3. `0x958290` takes **one** argument, the DlcEntry (rcx). It is not `(manager, entry)`.
4. `0x92a3a4` and `0x92adc0` do **not** take change-set name strings. They take 32-bit hashes (section 4).
5. `0x92a9ac`: `edx` = the pack's name hash (`DlcEntry+0x60`), `r8d` = the group hash. Recon had them as group hash / id.
6. `0x1362be8` is an "does this file exist" helper on a global (`.data 0x1e2ee90`, INFERRED fiAssetManager `ASSET.Exists(name, ext)`),
   not a string compare. Consequence: `AddDlcPack` returns false if `<path>dlc.rpf` is not resolvable.

## 1. AddDlcPack, RVA 0x916734

```c
// OBSERVED registers; return OBSERVED (al only)
typedef bool (*AddDlcPack_t)(void* mgr /*rcx = *(void**)(base+0x1ec1eb0)*/,
                             const char* path /*rdx, NARROW, ends with '/'*/);
```
- `mov rdi,rcx` (this), `mov rbx,rdx` (path). r8/r9/stack: not read. (OBSERVED)
- Narrow: the path is passed as `%s` to `0x3030` with format `"%sdlc.rpf"` (single-byte literal, bytes `25 73 64 6c 63 2e 72 70 66 00`
  at `.rdata 0x1a9c5c8`); `0x3030` is a bounded `snprintf(buf, 0x100, ...)` (edx=0x100). The result is measured with a byte loop
  (`cmp BYTE PTR [rax+r8],sil`). So `char*`, and at most about 248 characters (256-byte buffer minus `dlc.rpf` and NUL). Longer paths are
  silently truncated. (OBSERVED)
- Return: `al` only (upper bits of eax undefined). `xor al,al` (0) when `ASSET.Exists("<path>dlc.rpf","")` fails;
  `mov al,1` both when a new entry is appended **and** when an entry with the same path already exists (dedupe via
  `0x93d318(mgr, fullPath)`, which compares against `entry+0xb8`). So the return does not tell you whether a new entry was made:
  compare `count` (WORD at `mgr+0x30`) before and after. (OBSERVED)
- New entry is built on the stack with ctor `0x90f81c`, gets `+0xc8 = 2`, path string at `+0xb8` (len WORD `+0xc0`), name hash at `+0x60`
  (INFERRED: `0x136f418(1, "<path>dlc.rpf")`), then `0x9166e8(mgr, &entry)` appends it at index `count-1` (that helper returns the new index
  in eax, ignored by AddDlcPack) and writes `entry+0xcc = count+1`. (OBSERVED for the stores; the hash function identity INFERRED)
- The array may reallocate on append: re-derive the entry pointer after every call, never cache it.
- Callers pass: `0x963ed0` a 256-byte stack buffer already macro-expanded by `0x92aea8` (see section 6); `0x929e70` a
  `"platform:/dlcPacks/<name>/"` string. Whether an absolute OS path resolves is decided by the `Exists` check inside the device layer: untested.

## 2. Mount, RVA 0x94307c

```c
// OBSERVED registers. Return is an int status, NOT a bool.
typedef int (*DlcMount_t)(void* entry   /*rcx = DlcEntry* (stride 0xf0)*/,
                          uint32_t ignored /*edx: never read; callers forward mgr+0x178*/,
                          const char* mountPoint /*r8, narrow*/);
```
- `mov rsi,r8; mov rbx,rcx`; rdx, r9 and stack are never read. (OBSERVED)
- mountPoint narrow: OBSERVED at the caller `0x958290`, which passes the literal `"extra:/"` (`.rdata 0x1a9c6f0`, bytes `65 78 74 72 61 3a 2f 00`);
  `0x94301c` passes `entry+0x30` (char* with length WORD at `+0x38`, empty string `.rdata 0x19e3f88` when the length is 0). The `0x94301c`
  string kind is INFERRED narrow (same atString layout as `+0xb8`, which is observed narrow). I did not read `0x1366fec` to confirm
  (it is the archive-side Mount call, next to the header-parse area).
- Return in eax (OBSERVED): `0` = ok **or** `[entry+0]` (device pointer) was already non-null (immediate `xor eax,eax`);
  `1` = `0x1365a80` (Open) returned false; `2` = `0x1366fec` (Mount) returned false or `0x93bd7c` (patch-chain) returned nonzero.
  Callers test `eax==0`: `0x94301c` (`test eax,eax; jne` skips `state=2`), `0x958290` (`test eax,eax; jne`). `0x958290` also compares
  eax to 3, which this body never returns (leftover, or from a path I did not take).
- Branches on `DWORD [entry+0xc8]` ("kind"): `2` = packfile (what AddDlcPack sets): allocates a `0xc0` object (ctor `0x1360af0`), calls
  `0x1365a80(dev, [entry+0xb8] path, r8b=1, r9d=0, stack0=0)`, `0x1366fec(dev, mountPoint)`, `0x93bd7c(entry, mountPoint)`,
  `0x93a944(entry, mountPoint, dev, 0)`; `1` = loose folder: allocates a `0x118` object (vtable `.rdata 0x1a3f6f0`) and uses
  `0x13659c8` / `0x1366f4c` instead. Both then call `0x9431f8(entry, mountPoint)` and return 0. On failure it calls `0x926f34(entry)` (cleanup).

## 3. setup2 wrapper, RVA 0x958290

```c
// OBSERVED
typedef bool (*DlcSetup2_t)(void* entry /*rcx = DlcEntry**/,
                            uint32_t ignored /*edx: not read; 0x93ff40 passes mgr+0x178*/);
```
- Only `rcx` is read (`mov rbx,rcx; mov ecx,[rcx+0xd0]`). Return in `al` (`sete al` / `mov al,1`). (OBSERVED)
- State machine on `DWORD [entry+0xd0]` (OBSERVED stores; the names are INFERRED): states 1 or 2 on entry: return true at once.
  State 0: `Mount(entry, 0, "extra:/")`; if 0: `0x1367bb8(&ASSET,"extra:/")`, `0x1362be8(&ASSET,"setup2.xml",0)`, then
  `0x9424f8(entry,"setup2.xml")` and `state = ok ? 1 : 4`; file missing: `state = 5`; then `0x13679b4(&ASSET)`; Mount returning 3 sets state 3;
  always followed by `0x95ee6c(entry,"extra:/")` (INFERRED unmount/teardown). With state now 1: `0x944290(entry)`, a version-range check
  `0x966fcc(entry, &0x1ec0b30, [entry+0xa0] or "")` (true sets state 6), and `cmp [entry+0x60],0x876aa600` special case. Returns `state==1`.
  Net effect: temporary mount under `extra:/`, parse `setup2.xml`, unmount; the real mount is `0x94301c` below.

`0x94301c` (needed next; OBSERVED): `bool MountGate(DlcEntry* rcx, uint32 edx /*forwarded unread to Mount*/)`: state 2 -> true;
state 1 -> `Mount(entry, edx, [entry+0x30] or "")`, and if it returned 0 sets `state = 2`; returns `state == 2`. State 0 returns false (run `0x958290` first).

## 4. Change-set entries 0x92a3a4 and 0x92adc0 (and the real per-pack entry 0x92a9ac)

None of them takes a string; there is no narrow/wide question. Group names are hashed by the caller:
`joaat(lowercase)`: `GROUP_EARLY_ON 0x6f8bd408`, `GROUP_STARTUP 0x5bf6578f`, `GROUP_ON_DEMAND 0x9ea3e635`, `GROUP_MAP 0xbcc89179`,
`GROUP_MAP_SP 0x578f99e2` (recomputed offline from the names; they equal the immediates at the call sites).

```c
typedef void (*RunPackGroup_t)(void* mgr /*rcx*/, uint32_t packNameHash /*edx = DlcEntry+0x60*/,
                               uint32_t groupHash /*r8d*/);                              // 0x92a9ac  OBSERVED
typedef void (*RunPackChangeSet_t)(void* mgr /*rcx*/, uint32_t packNameHash /*edx*/,
                               uint32_t groupHash /*r8d*/, uint32_t changeSetHash /*r9d*/); // 0x92a3a4  OBSERVED
typedef void (*TitleUpdateChangeSet_t)(void* mgr /*rcx*/, uint32_t changeSetHash /*edx*/,
                               bool enable /*r8b*/);                                      // 0x92adc0  OBSERVED
typedef void (*RunGroupAllFlagged_t)(void* mgr /*rcx*/, uint32_t groupHash /*edx*/,
                               bool enable /*r8b*/);                                      // 0x92ae24  OBSERVED
```
- `0x92a9ac` (40-byte sig in recon, 1 match): `rdi=rcx`; `rax = 0x92f46c([0x1ec1eb0], edx)` (finds the DlcEntry whose `+0x60` equals edx);
  null -> silent no-op; else `0x92aa40(mgr, entry, &groupHash)` which gets the pack's change sets for that group via `0x92f12c` and calls
  `0x92ab00(mgr, entry, &changeSetHash, &groupHash, mode=3)` for each. No return value (void). **This is the function `0x93ff40` calls for every
  pack right after `0x94301c`** (r8d = `GROUP_EARLY_ON` when its early flag is set, else `GROUP_STARTUP`), so it is the right public entry to
  "turn on one pack's content after boot". Use `groupHash = 0x5bf6578f` (STARTUP) or `0x6f8bd408` (EARLY_ON) to mirror boot, or `0x9ea3e635`
  (ON_DEMAND) for on-demand sets (extra gating by flags at `mgr+0x17c/+0x181` per recon; not re-verified here).
- `0x92a3a4`: same lookup (global `[0x1ec1eb0]`, edx), then `0x92ab00(mgr, entry, &groupHash(r8d), &changeSetHash(r9d), mode=7)`: runs ONE change
  set by hash. `void`.
- `0x92adc0`: hashes the literal `"TitleUpdate"` (`.rdata 0x1a9c560`, via `0x135ab40`), finds that pack in `rcx`'s array, then
  `enable ? 0x92a3a4(mgr, pack.hash, 0x9ea3e635, edx) : 0x952848(mgr, pack.hash, 0x9ea3e635, edx)` (0x952848 has the same argument shape and calls `0x952e08` with mode 3; "disable twin" is INFERRED). **Hard-wired to the
  TitleUpdate pack: not a generic entry.** Boot calls it with edx = `0x1c3bc144` and `0xeeaf512a`; script wrapper `0x802168` with `0x19e536c7`.
- `0x92ae24` (extra, no recon signature): for every pack with `BYTE [entry+0xe8] & 4`, calls `0x92a9ac(mgr, entry+0x60, groupHash)` if enable else `0x952ca4(...)`.
  `0x92a9ec(mgr, groupHash)` does the same for every pack (no flag test).
- New signatures (built from the disassembly, wildcarding rip/call displacements, 1 match each):
  - `0x92a3a4`: `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 8B F1 48 8B 0D ?? ?? ?? ?? 41 8B D9 41 8B F8 E8 ?? ?? ?? ?? 48 85 C0 74 25 4C 8D 4C 24 30 4C 8D`
  - `0x92adc0`: `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B D9 8B FA 48 8D 0D ?? ?? ?? ?? 33 D2 41 8A F0`

## 5. The singleton: how to get a valid pointer

- **No accessor exists.** A scan for `mov rax,[G]; ret` and for short pdata functions reading either global found none. Every consumer reads the
  global directly (`mov rcx,[rip+disp]`, 228 sites for `0x1ec1ea8`, 88 for `0x1ec1eb0`, no `lea` of either address). So read the QWORD at
  `base + 0x1ec1eb0`. (OBSERVED)
- Construction (OBSERVED; recon missed these because they are plain stores):
  - `0x1ec1eb0`: written at `0x93865d` inside `0x938618` (an `Init(mode)`): `alloc(0x9b8)`, ctor `0x90eccc`, `[obj] = base+0x1a9c508` (vtable),
    store, then `0x937fa0(obj, mode)`. Called from the boot function at `0x18973` (`ecx=r14d`). Cleared at `0x95a648` inside `0x95a600`
    (`Shutdown(mode)`): `vtbl[+0x30]` then `[G]=0`, only when `mode == 1` (full shutdown).
  - `0x1ec1ea8`: written at `0x1887d` in `0x187b4`: `alloc(0x108)`, ctor `0x90e7d4`, store (or 0 if alloc fails, at `0x18886`).
- So after boot both are stable until process shutdown. Safe read: `p = *(void**)(base+0x1ec1eb0); if (p && *(uint64_t*)p == base+0x1a9c508)` (vtable check
  from the ctor above; the second condition is INFERRED to hold for the whole lifetime).
- Resolve the address from a signature instead of hard-coding the RVA: in `0x92a9ac` the instruction `48 8B 0D disp32` sits at fn+13 (7 bytes), so
  `G_B = (fn + 20) + (int32)disp`; in `0x940fbc` the `48 8B 05 disp32` at fn+4 gives `G_A = (fn + 11) + disp`. Both verified to land on
  `0x1ec1eb0` / `0x1ec1ea8`.
- Game-thread note (INFERRED): none of this code takes a lock; call from the game thread after the boot function has run.

## 6. Layouts seen (partial, OBSERVED unless marked)

Manager (`[0x1ec1eb0]`, 0x9b8 bytes, vtable at +0): `+0x28` DlcEntry* array, `+0x30` WORD count, `+0x178` DWORD generation counter
(forwarded as edx by the boot loop), `+0x182` BYTE flags (bit0 is tested/cleared by `0x91d7c4`/`0x92955c`, bit2 is set by `0x93ff40`; meanings INFERRED), `+0x17c/+0x181` on-demand gating (recon, not re-read).
DlcEntry (stride `0xf0`): `+0x00` device object* (0 = none); `+0x30` char* mount point, `+0x38` WORD length; `+0x60` DWORD name hash (key of
`0x92f46c`); `+0x90/+0x94` DWORDs used by the pack loop (meaning not determined); `+0xa0` char*, `+0xa8` WORD (string used by the version check);
`+0xb0` BYTE; `+0xb8` char* full path `"<path>dlc.rpf"`, `+0xc0` WORD length; `+0xc8` DWORD kind (2 packfile, 1 loose folder); `+0xcc` DWORD ordinal;
`+0xd0` DWORD setup state; `+0xe8` BYTE flags (bit 4 "participates in for-all").
`[0x1ec1ea8]` object (0x108 bytes, ctor `0x90e7d4`): provides `0x92aea8(this, const char* in, char* out, size_t outSize)`, a `%NAME%` macro expander
that `0x963ed0` runs on each `dlclist.xml` item before `AddDlcPack` (narrow, `outSize = 0x100`), and `0x93f428(this, path, bool)` (content XML loader
at boot); byte at `+0xf0` is the "scan in progress" flag. You do not need it to call `AddDlcPack` directly.

## 7. Per-item dispatcher 0x92a1c0 and the loader table 0x25c91e0 (quick pass)

```c
typedef void (*DispatchItem_t)(void* item /*rcx*/);   // 0x92a1c0, void, OBSERVED
// item+0x00 QWORD target pointer (for kind 1: the data-file record)
// item+0x08 DWORD kind (0..8; switch via successive dec)
// item+0x0c BYTE  0 = remove/unload, nonzero = add/load
// item+0x10 DWORD id for kind 8 (-1 = ignore)
```
Kind 1 (data file): `rsi = record = *(item+0)`; `al = 0x93dd9c(&record->+0x90)`; for add: if `al==0` then `record->+0x9b = 0`; then
`loader = *(void**)(base + 0x25c91e0 + (int64)(int32)record->+0x90 * 8)`; if non-null: `(*(void(**)(void*,void*))(*(void**)loader + 0x8))(loader, record)`,
i.e. **`loader->vtbl[1](rcx = loader, rdx = record)`** (Load). Remove path uses `vtbl[2]` (Unload), gated by `byte [0x1dd216d]` or a per-type
sentinel (`dword [0x25c6730 + idx*4] == dword [0x1dd2f2c]`). The dispatcher does not test the return value and does **not** bounds-check
the index (table has 0xd0 slots): a record with `+0x90 >= 0xd0` reads out of bounds. (OBSERVED)
Concrete example: the `DLC_ITYP_REQUEST` loader is a static object at `.data 0x1dd76e8` (stored into slot `0x25c9750` by `0x93a5e0`), vtable
`.rdata 0x1a9c828` = `{0xd5e7bc dtor, 0x940484 Load, 0x95ec30 Unload}`. Its Load is `bool Load(void* /*rcx unused*/, Record* rdx)`: `rcx=rdx; call 0x91691c; mov al,1`.
So a Load takes the record in **rdx** (rcx is the loader object) and returns `al=1`. What name/other fields each Load reads from the record was not
traced beyond that (the record's own name field offset is not established here).

## 8. Pattern re-verification (whole 65 MB image, wildcards at rip/call displacements only)

| function | RVA | matches | pdata bounds |
|---|---|---|---|
| AddDlcPack | 0x916734 | 1 | 0x916734-0x91684d |
| Mount | 0x94307c | 1 | 0x94307c-0x9431f6 |
| setup2 wrapper | 0x958290 | 1 | 0x958290-0x9583d2 |
| RunPackGroup | 0x92a9ac | 1 | 0x92a9ac-0x92a9ec |
| 0x92a3a4 (new sig above) | 0x92a3a4 | 1 | 0x92a3a4-0x92a402 |
| 0x92adc0 (new sig above) | 0x92adc0 | 1 | 0x92adc0-0x92ae24 |
| per-item dispatcher | 0x92a1c0 | 1 | 0x92a1c0-0x92a3a2 |
| boot trigger | 0x940fbc | 1 | 0x940fbc-0x940ff1 |
| MountGate | 0x94301c | 1 | 0x94301c-0x94307a |
| sibling wrapper | 0x944290 | 1 | 0x944290-0x944384 |

## 9. Could not determine

- What `0x1365a80` / `0x1366fec` accept (deliberately not read). The mount-point string kind at the `0x94301c` call site is inferred, not read through to the callee.
- Where `Mount` could return 3 (never seen), and the meaning of `DlcEntry+0x90/+0x94/+0xb0`.
- Whether `entry+0x60` for a dlclist pack is a hash of the full `"<path>dlc.rpf"` string or of a logical name (`0x136f418` not read). Read it from the entry after
  `AddDlcPack` instead of computing it; `0x92adc0`'s `"TitleUpdate"` lookup shows some entries use `joaat(lower(name))` of a logical name.
- Whether an absolute OS path passes the `Exists` check in `AddDlcPack` (device layer, untested).
- Whether re-running `0x93ff40` is idempotent for already-active packs (its second loop re-runs the group for every pack): use the per-pack calls instead.
- `GROUP_ON_DEMAND` gating at `mgr+0x17c/+0x181` and the `mode` argument bits of `0x92ab00` (3 vs 7) were not re-derived.

## 10. Suggested post-boot sequence for one pack (from the observed boot loop; not yet run)

1. `mgr = *(void**)(base+0x1ec1eb0)`; check non-null and vtable. `n0 = count`.
2. `AddDlcPack(mgr, "<device-path>/")`; if `count == n0` the pack already existed (or Exists failed, if it returned 0).
3. `e = *(char**)(mgr+0x28) + (count-1)*0xf0`; `DlcSetup2(e, gen)` must return true (state 1).
4. Optional: `e->+0xe8 |= 4` so `0x92ae24` includes it.
5. `MountGate(e, gen)` must return true (state 2); `gen = *(uint32_t*)(mgr+0x178)`.
6. `RunPackGroup(mgr, *(uint32_t*)(e+0x60), 0x6f8bd408)` then `RunPackGroup(mgr, same, 0x5bf6578f)` (what the boot does), re-deriving `e` if anything reallocated.
