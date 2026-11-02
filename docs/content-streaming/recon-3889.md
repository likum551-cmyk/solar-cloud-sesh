# P1 recon: extra-content and data-file side (GTA V Legacy 1.0.3889)

Clean-room static analysis of the decrypted in-memory image
`GTA5.exe-65387520-252545640.img` (FILE OFFSET == RVA). Read-only: the dump was
never modified, nothing was launched. Scope per the coordinator's instruction:
**extra-content (DLC) registration and the per-type data-file loader table
only.** The RPF7 header parse / encryption-word switch, the OPEN-rejection
branch and anything about key material are out of scope for this pass and are
not covered below.

Tooling: Python 3.15 (`python -I`), the project's mingw `objdump.exe`, and
scripts under `H:\Users\ADMINI~1\AppData\Local\Temp\x\p1recon\` (`rc.py` =
image/`.pdata` helpers, `sig.py` = wildcard byte-signature builder/verifier,
`d.py`/`who.py`/`up.py` = disassembly and call-graph helpers, `s*.py` = the
individual probes). No FiveM/CitizenFX/RageOpenV/ClosedIV/CodeWalker source
and no signature from `apps/engine/dlc_mount.cpp` was read or used; only
`dlc_mount.h`'s role list told us *what* to look for.

Every byte pattern below was machine-built from the function's own
disassembly (not hand-typed), wildcards only at **RIP-relative displacements**
and **call/jmp rel32 operands**, and then **verified by regex-scanning the
whole 65 MB image**: each one reports `matches=N` from that scan, and `N=1`
for every pattern listed (re-confirmed against the raw bytes at the match
site, not just taken on faith from the match count). `??` = one wildcarded
byte.

## 1. The DLC-pack scan (`dlclist.xml` + folder auto-discovery)

### 1.1 `ParseDlcList` — RVA `0x963ed0`
**OBSERVED.** Builds the path `"common:/data/dlclist.xml"` (string at
`.rdata 0x1a9c590`), opens/parses it via `0x139c980` (the generic
XML-document-open helper used everywhere in this codebase, including by the
item at 1.3), then for each `<Item>` entry (loop bound by
`WORD [rsp+0x48]`, the XML-returned element count) formats a per-entry path
with `0x92aea8` (`sprintf`-style formatter) and calls **`AddDlcPack`
(1.2, RVA `0x916734`)**. After the loop it calls **`EnumerateDlcPacksFolder`
(1.3, RVA `0x929e70`)** once more — i.e. after the explicit `dlclist.xml`
entries it *also* scans `platform:/dlcPacks/` directly and adds every
sub-folder found there that `dlclist.xml` didn't already name.

- Signature (32 bytes, 1 match):
  `48 89 5C 24 08 48 89 74 24 10 57 48 81 EC 50 01 00 00 4C 8B 0D ?? ?? ?? ?? 33 F6 48 8B F9 48 8B`
- Callees: `0x139c980` (open/parse XML doc), `0x92aea8` (format path string),
  `0x916734` (AddDlcPack, per `dlclist.xml` entry), `0x929e70`
  (EnumerateDlcPacksFolder, once, after the loop), `0x10c8bac` (cleanup).
- Callers: none found (direct call or jmp) — see §4(a)/(d) on how early-boot
  functions in this binary are reached; this one is almost certainly reached
  the same way as `AddDlcPack`'s other caller, §1.4.

### 1.2 `AddDlcPack` — RVA `0x916734`
**OBSERVED.** Signature `AddDlcPack(ExtraContentManager* this /*rdi*/, const
wide-or-narrow path "device:/folder/" /*rbx*/)`. Formats
`"%sdlc.rpf"` (string at `.rdata 0x1a9c5c8`) against the incoming path — i.e.
it **appends the literal text `dlc.rpf` to whatever string it is given,
with no separator of its own**; the caller must already end its path in
`/`. It then calls `0x93d318` to check whether an entry with the same name
already exists in `this`'s pack array (see §4(a)); if not, it allocates a new
entry (`0x90f81c`), appends it to the array (`0x9166e8`/`0x911d6c`), and
returns.

- Signature (40 bytes, 1 match):
  `48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 48 8D A8 E8 FE FF FF 48 81 EC 10 02 00 00 48 8B F9 48 8B DA 48 8D 4D 11`
- Callees: `0x1881a50` (string init), `0x3030` (`sprintf`-style format into a
  stack buffer, builds `"%sdlc.rpf"`), `0x1362be8` (string-equal check),
  `0x93d318` (**dedupe**: scans the existing array by name, see §4(a)),
  `0x90f81c` (allocate a new entry when not a dup), `0x9166e8` / `0x911d6c`
  (append to array / housekeeping).
- Callers (direct call sites found): `0x929e70` (1.3, per directory entry)
  and `0x963ed0` (1.1, per `dlclist.xml` entry).

### 1.3 `EnumerateDlcPacksFolder` — RVA `0x929e70`
**OBSERVED.** Builds `"platform:/dlcPacks/"` (`.rdata 0x1a9c5b0`), resolves a
device for it via `0x1364148`, then calls that device's vtable `FindFirst`
(`[obj+0xc8]`) / `FindNext` (`[obj+0xd0]`) / `FindClose` (`[obj+0xd8]`) in a
loop, building `"platform:/dlcPacks/<entry>/"` for each directory entry
(skipping entries starting with `.`) and calling **`AddDlcPack` (1.2)** on
each.

- Signature (48 bytes, 1 match):
  `48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 70 20 55 48 8D 68 A8 48 81 EC 50 01 00 00 48 8B F1 48 8D 0D ?? ?? ?? ?? B2 01 E8 ?? ?? ?? ??`
- Callees: `0x1364148` (get device for a path string), the device's own
  `FindFirst`/`FindNext`/`FindClose` virtual methods (not a fixed RVA —
  resolved through the device object's vtable at call time), `0x137aed8`
  (string append, builds the trailing `/`), `0x916734` (AddDlcPack, per
  entry).
- Caller: `0x963ed0` (1.1), once, after the `dlclist.xml` loop.

### 1.4 The boot trigger — RVA `0x940fbc`
**OBSERVED** (mechanism) / **INFERRED** (exact call timing/label).
`0x940fbc` loads the single global `ExtraContentManager*` singleton from
`.data 0x1ec1ea8`, sets a byte at `manager+0xf0` to `1` ("scan in progress"),
calls `0x91d7c4(dl=1)`, calls an unrelated function `0x94c9b0`, then clears
`manager+0xf0` back to `0`. `0x91d7c4` calls `0x92955c` (which itself calls
`0x963ed0`, the dlclist.xml/folder scan from §1.1, then `0x93ff40`, the
per-pack-entry processing loop, §2.1) — this is the actual "load all extra
content" entry point run at boot.

- Signature (24 bytes, 1 match):
  `48 83 EC 28 48 8B 05 ?? ?? ?? ?? 41 B0 01 41 8A D0 C6 80 F0 00 00 00 01`
- Callers: the only direct call site found is inside the big function at
  `0x187b4`–`0x18ddc` (call site `0x189d4`), which also runs the
  `GROUP_*`/`CCS_*` change-set-group hash caches and the `DLC_ITYP_REQUEST`
  loader-slot registration (§3.3) in the same body, strongly suggesting this
  whole block is one "early boot: set up extra-content and data-file-type
  plumbing" routine rather than a set of unrelated static initializers.
  I could not find who calls `0x187b4` itself (it has no direct callers or
  absolute-pointer references I could locate) — **INFERRED**: it is reached
  through the game's own generic subsystem-init list rather than a C-runtime
  global-constructor table (a plain CRT constructor, e.g. `0x19870dc` seen
  elsewhere, *is* referenced by a plain absolute pointer at `0x19d8ae8`;
  `0x187b4` is not), which is as far as this pass traced it.

## 2. `setup2.xml` and change-set execution for one pack

### 2.1 The per-pack processing loop — RVA `0x93ff40`
**OBSERVED.** Iterates every entry in the manager's pack array
(`this+0x28`, count `this+0x30`), and for each one not already fully set up
(state `this+0xd0` / per-entry flags) calls `0x958290` (2.2, which parses
`setup2.xml`), then — once state `==3` (parsed) — builds the entry's
`cacheLoaderData_DLC%s` path, registers the `DLC_CORRUPT_ERR` failure path,
and (crucially) calls the **changeset-group entry point `0x94301c`**
(mount-status gate, §2.4) and then **`0x92a9ac`** (2.5, "run this pack's
change set"). It also contains the inline registrations of the `GROUP_*`
hash caches it needs (`GROUP_EARLY_ON`=`0x6f8bd408`,
`GROUP_STARTUP`=`0x5bf6578f`) to decide which group to execute first.

- Signature (32 bytes, 1 match):
  `48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 40 48 8B F9 48 8D 4D 40 45 8A`
- Callees: `0x13494d4` (array-count fetch helper), `0x958290` (2.2,
  setup2.xml), `0x93d254` (checks whether two entries are mutually
  compatible/already handled), `0x94301c` (2.4), `0x92a9ac` (2.5),
  `0x92a9ac`'s siblings via `0x1a7218`/`0x5ecd0c` (corrupt-pack error path).
- Callers: `0x92955c` (the "load all extra content" step called from the
  boot trigger, §1.4) and `0x937fa0` (a second, title-update-flavoured
  caller — the same function that also owns the `TitleUpdate`/`mph4_gtxd`
  strings found during the initial string sweep; **INFERRED** this is the
  title/patch-update variant of the same pipeline, not traced further here
  since it is outside "extra-content for one add-on pack").

### 2.2 `setup2.xml` wrapper — RVA `0x958290`
**OBSERVED.** `ProcessOnePack(ExtraContentManager* this /*rbx*/, DlcEntry*
/*rsi*/)`. Builds `"extra:/"` (`.rdata 0x1a9c6f0`), mounts it against the
pack's own device (`0x94307c`, §2.3 — the actual mount call), and if that
succeeds, opens `"setup2.xml"` (`.rdata 0x1a9c6f8`) through the generic XML
helper and, on success, calls **`ParseSetup2Xml`** (2.3b, RVA `0x9424f8`) and
sets the entry's state to `1` (parsed). The twin function at RVA `0x944290`
is the same wrapper shape reused by a second caller.

- Signature (24 bytes, 1 match):
  `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B D9 8B 89 D0 00 00 00`
- Callees: `0x94307c` (2.3, mount), `0x1362be8` (string-equals, confirms the
  file is really named `setup2.xml`), `0x9424f8` (2.3b, parse it),
  `0x95ee6c` (teardown/cleanup on failure), `0x966fcc` (version-range check).
- Callers: `0x93ff40` (2.1, the main per-entry loop) — `0x93ff40` calls
  `0x958290` with `edx = this->0x178` (a per-manager generation counter).

The sibling wrapper **RVA `0x944290`** has the identical body shape (same
`"extra:/"`/`"setup2.xml"` sequence feeding the same `0x9424f8` parser) and
is called from `0x951d78`'s caller chain; both ultimately feed the same
`setup2.xml` parser.

- Signature for `0x944290` (24 bytes, 1 match):
  `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 81 EC 40 01 00 00 33`

### 2.3 Mount — RVA `0x94307c`
**OBSERVED.** `Mount(DlcEntry* this /*rbx*/, mountPointString /*rsi*/)`.
If the entry has no device object yet (`[this+0]==0`) it allocates one
(`0xc0`-byte object, vtable at `.rdata 0x1a3f6f0`, ctor `0x1360af0`), calls
**Open** (`0x1365a80` — opens the backing `.rpf`/archive) then **Mount**
(`0x1366fec` — binds the opened archive to the mount-point string in `rsi`,
e.g. `"dlc_<name>:/"`), then, if that succeeded, calls `0x93bd7c` (the
`dlc%d.rpf` patch-chain mounter — walks `dlc0.rpf`, `dlc1.rpf`, … in the same
folder and mounts each the same way) and finally `0x93a944` (registers the
two always-present data files `"CRC:/"`-prefixed name-hash entries for the
pack). Returns `1` on success, `0` if Open or Mount failed.

- Signature (32 bytes, 1 match):
  `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30 45 33 FF 49 8B F0 48 8B`
- Callees: `0x1360af0` (device ctor), `0x1365a80` (Open), `0x1366fec`
  (Mount), `0x93bd7c` (patch-chain mount), `0x93a944` (register the pack's
  own CRC-keyed data entries).
- Callers: `0x958290`/`0x944290` (2.2, per pack, before `setup2.xml` is
  read) and `0x9431f8` (a second, later re-mount/validate path called from
  within `0x93ff40`'s successors).

### 2.3b `setup2.xml` parser — RVA `0x9424f8`
**OBSERVED.** Reads the tags `device` (`.rdata 0x1a9c768`), `datfile`
(`0x1a9c770`), `contentchangesets` (`0x1a9c778`), `script` (`0x1a9c78c`,
sub-tag `startup`), and `stackSize` (`0x1a9c798`), plus an unrelated
`testmarketplace` flag. For `contentchangesets` it walks a child-element
list and, for each child, calls `0x136f418` (string→hash) and stores the
hash into a newly-allocated array — i.e. **a pack's `setup2.xml` can name
more than one change-set group**, each identified by its hash, and this
array is exactly what §2.5's chain consumes later.

- Signature (24 bytes, 1 match):
  `48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 60 48 8B E9 48`
- Callees: `0x1362d58` (XML attribute helpers), `0x135ab40` (hash),
  `0x13974e0` (find-child-element-by-name, called once per tag name above),
  `0x136f418` (string→hash, used for each change-set-group name and for the
  `script` entry), `0x1880910`/`0x3030` (string copy/format helpers).
- Callers: `0x958290` and `0x944290` (2.2).

### 2.4 Mount-status gate — RVA `0x94301c`
**OBSERVED.** Tiny helper: if the entry's state is already `2` ("mounted"),
returns true; if state is `1` ("parsed"), calls `0x94307c` (2.3) again — the
mount call is itself idempotent/re-entrant from this gate's point of view —
and on success sets state to `2`; otherwise returns whether state `==2`.

- Signature (24 bytes, 1 match):
  `48 89 5C 24 08 57 48 83 EC 20 8B 81 D0 00 00 00 48 8B D9 83 F8 02 75 04`
- Callees: `0x94307c` (2.3).
- Caller: `0x93ff40` (2.1).

### 2.5 Running a pack's change set after it is mounted

Chain (all **OBSERVED** at the byte level; the semantic labels in
parentheses are **INFERRED** from shape/behaviour, since no native-hash
lookup was used):

```
0x92a9ac (ExecuteChangeSetGroup?)
  rcx = manager "this" (saved, passed through unchanged)
  rdx = group-name hash              <- the *key*, passed straight to 0x92f46c
  r8d = an id, saved at [rsp+0x48]
  rcx' = global group-registry @ .data 0x1ec1eb0   (a SECOND singleton,
         distinct from the ExtraContentManager @ 0x1ec1ea8)
  call 0x92f46c(registry, groupHash) -> group-entry ptr or NULL
  if found: call 0x92aa40(this, groupEntry, id)
      -> for each sub-item i in groupEntry's array:
           call 0x92ab00(this, groupEntry, item[i], id, mode=3)
               -> (mode bit tests) call 0x92a4b8(this, ..., groupId, item)
                   -> if groupId is GROUP_MAP (0xbcc89179) or
                      GROUP_MAP_SP (0x578f99e2): call 0x92a404 directly
                   -> else (e.g. GROUP_ON_DEMAND, 0x9ea3e635): extra gating
                      via [manager+0x17c]/[manager+0x181] before the same
                      0x92a404 call
                       -> 0x92a404: for each file-action entry in the
                          change set, call 0x92a1c0(entry)   <-- §3, below
```

- `0x92a9ac` signature (40 bytes, 1 match):
  `48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 8B 0D ?? ?? ?? ?? 41 8B D8 E8 ?? ?? ?? ?? 48 85 C0 74 14 4C 8D 44 24 48 48 8B`
- `0x92ab00` signature (32 bytes, 1 match):
  `48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 57 48 8B EC 48 83 EC 30 F6 45 40 01 49`
- `0x92a4b8` signature (24 bytes, 1 match):
  `48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 B0 FD FF FF`
- `0x92ac00` signature (GROUP_ON_DEMAND enable helper, 40 bytes, 1 match):
  `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 45 33 FF 48 8B D9 41 8B FF 66 44 3B 79 10 73 57`

Callers of `0x92a9ac`: `0x92a3a4` and `0x92adc0` — both are small wrappers
that resolve a change-set name string to a hash first (via `0x135ab40`) and
then call `0x92a9ac`, so the real public entry point one layer up accepts a
**name string**, not a pre-computed hash.

## 3. Per-item action dispatch and the data-file-type loader table

### 3.1 Per-item dispatcher — RVA `0x92a1c0`
**OBSERVED.** Switches on `item+0x8` (an integer "kind", observed values
0–8). Kind `1` is the one that ties into the shared per-data-file-type
loader table:

```
92a2de: rsi = *item                      ; the file-record this item refers to
92a2e8: call 0x93dd9c(rsi+0x90)          ; -> al (some pre-check)
92a2ed: cmp item+0xc, 0                  ; item+0xc: 0 = "remove", != 0 = "add" (see below)
   je  -> (remove path) ... eventually:
            rcx = loader_table[*(rsi+0x90)]     ; table @ .data 0x25C91E0
            call [rcx+0x10]                      ; vtable slot "Unload"
   (fallthrough, add path) ... eventually:
            rcx = loader_table[*(rsi+0x90)]
            call [rcx+0x8]                       ; vtable slot "Load"
```
Both paths index the **same** table at `.data 0x25c91e0` by the 32-bit field
at `record+0x90` — this field is exactly the type index produced by the
name→index hash table in §3.2. The `item+0xc` add/remove interpretation is
**INFERRED** from the CARCOLS_FILE loader's own vtable (§3.3: slot `+0x8`
is a real loader, slot `+0x10` is a no-op `ret 0` stub — consistent with
"load" vs. an unsupported "unload").

- Signature (32 bytes, 1 match):
  `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B D9 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 4B`
- Callee (for kind 1): `0x93dd9c`, then the resolved loader object's own
  vtable method (`+0x8` or `+0x10`, not a fixed RVA).
- Caller: `0x92a404`, in a loop over the change-set's item array (§2.5).

### 3.2 The name→index hash table — `.data 0x1dce090`
**OBSERVED.** 208 entries of `{uint32 joaat_hash, uint32 index}` (plain
`joaat` over the exact-case type name, e.g. `"RPF_FILE"`,
`"VEHICLE_METADATA_FILE"` — not lower-cased). Table is contiguous and
sequential (`index == entry position`), ends at `0x1dce710`. Confirmed
entries relevant to this plan (hash, index):

| Type name | hash | index |
|---|---|---|
| `RPF_FILE` | `0x04df4461` | `0x00` |
| `HANDLING_FILE` | `0x02ca1d59` | `0x06` |
| `PEDSTREAM_FILE` | `0x50f54a62` | `0x09` |
| `CARCOLS_FILE` | `0x33d82852` | `0x0a` |
| `VEHICLE_METADATA_FILE` | `0xf5e0a315` | `0x49` |
| `WEAPONINFO_FILE` | `0x12b7d65a` | `0x4e` |
| `VEHICLE_VARIATION_FILE` | `0xfbe8de2f` | `0x87` |
| `CONTENT_UNLOCKING_META_FILE` | `0x08470577` | `0x88` |
| `SHOP_PED_APPAREL_META_FILE` | `0xf9af6eb9` | `0x89` |
| `AUDIO_GAMEDATA` | `0x99134f34` | `0x8c` |
| `TEXTFILE_METAFILE` | `0x44180923` | `0xa0` |
| `DLC_WEAPON_PICKUPS` | `0xbcdf954b` | `0xa6` |
| `DLC_ITYP_REQUEST` | `0xf53935be` | `0xae` |

`VEHICLE_METADATA_FILE`, `HANDLING_FILE`, `CARCOLS_FILE`,
`VEHICLE_VARIATION_FILE`, `TEXTFILE_METAFILE`, `RPF_FILE` and
`DLC_ITYP_REQUEST` (everything the plan asked us to find) are all present.
I could not find the code that *does the hash lookup* against this specific
table (i.e., a function taking a name string and returning the index) in
this pass — only the *consumers* of the resulting index (§3.1, §3.3). This
table itself is read-only data; it is not where a pack's type name gets
turned into the index baked into `record+0x90` — that conversion happens
somewhere in the `content.xml`/data-file registration path, which was not
traced further this pass.

### 3.3 The loader-object pointer table — `.data 0x25c91e0`
**OBSERVED.** 208 `void*` slots (same indexing as §3.2, `0xd0` = 208 decimal
checked against the table's own bounds check `cmp ecx, 0xd0` seen in the
fillers below), zero in this dump (filled only at runtime). Each non-null
slot holds a pointer to a 3-entry vtable: `{destructor, Load, Unload}` (slot
offsets `+0x0`, `+0x8`, `+0x10`). Confirmed fillers, each **OBSERVED** by a
direct store instruction into the exact slot address:

| index (type) | slot address | filled by |
|---|---|---|
| `0x06` (`HANDLING_FILE`) | `0x25c9210` | `0xf715cc` |
| `0x09` (`PEDSTREAM_FILE`) | `0x25c9228` | `0x698e68` |
| `0x0a` (`CARCOLS_FILE`) | `0x25c9230` | `0x6356a4` |
| `0x49` (`VEHICLE_METADATA_FILE`) | `0x25c9428` | `0x6356a4` |
| `0x4e` (`WEAPONINFO_FILE`) | `0x25c9450` | `0x1063218` |
| `0x87` (`VEHICLE_VARIATION_FILE`) | `0x25c9618` | `0x6356a4` |
| `0x88` (`CONTENT_UNLOCKING_META_FILE`) | `0x25c9620` | `0x937fa0` |
| `0x89` (`SHOP_PED_APPAREL_META_FILE`) | `0x25c9628` | `0x9225dc` |
| `0x8c` (`AUDIO_GAMEDATA`) | `0x25c9640` | `0x396df4` |
| `0xa0` (`TEXTFILE_METAFILE`) | `0x25c96e0` | `0x937fa0` |
| `0xa6` (`DLC_WEAPON_PICKUPS`) | `0x25c9710` | `0x6cbd10` |
| `0xae` (`DLC_ITYP_REQUEST`) | `0x25c9750` | `0x93a5e0` |

One function can fill several related slots — `0x6356a4` fills
`CARCOLS_FILE`, `VEHICLE_METADATA_FILE` and `VEHICLE_VARIATION_FILE` together
(guarded by a one-shot sentinel per slot at `.data 0x25c6854` /
`0x25c694c` / `0x25c6758`, each compared against `-1` before writing, so a
second call does not re-register); `0x937fa0` (the same function that owns
the `"update:/"` / `"TitleUpdate"` / `"mph4_gtxd"` strings found in the
initial anchor sweep) fills both `CONTENT_UNLOCKING_META_FILE` and
`TEXTFILE_METAFILE`.

- `0x6356a4` signature (24 bytes, 1 match):
  `83 F9 04 0F 85 CA 00 00 00 57 48 83 EC 20 8A 05 ?? ?? ?? ?? 49 83 C8 FF`
- `0x93a5e0` signature (24 bytes, 1 match):
  `40 57 48 83 EC 20 8A 05 ?? ?? ?? ?? 49 83 C8 FF 4C 8D 0D ?? ?? ?? ?? 41`

**When do these run?** Only `0x93a5e0` (`DLC_ITYP_REQUEST`) has a located
caller: it is called directly (`call 0x93a5e0`, call site `0x188d9`) from
inside the big function at `0x187b4`, the **same function that also runs the
`GROUP_*`/`CCS_*` change-set-group hash caches** and (via §1.4's
`0x940fbc`) ends up triggering the whole `dlclist.xml`/folder scan — i.e.
**OBSERVED**: at least the `DLC_ITYP_REQUEST` slot is filled in the same
early-boot block that precedes/parallels the first extra-content load.
The other eleven filler functions above (`0x6356a4`, `0xf715cc`, `0x698e68`,
`0x1063218`, `0x937fa0`, `0x9225dc`, `0x396df4`, `0x6cbd10`) have **no
direct call or jmp site and no absolute-pointer reference** findable with
this pass's methods — **INFERRED**: each is almost certainly invoked
indirectly through its own subsystem's generic init dispatch (the standard
pattern in this codebase, e.g. vehicle/ped/weapon/audio/shop subsystem
`Init()` calls), which this pass did not trace to its call site. By
contrast, a genuine C-runtime global constructor in this image (e.g.
`0x19870dc`, one of the `GROUP_*` hash-cache setters) *is* referenced by a
plain absolute 8-byte pointer (found at `.rdata 0x19d8ae8`) — none of the
eleven un-located fillers are referenced that way either, which is further
(weak) evidence they are reached through a different, subsystem-owned
mechanism rather than the CRT init-array.

## 4. Answers to the specific questions

### (a) Can `AddDlcPack` (`0x916734`) and `Mount` (`0x94307c`) be called for a single pack after boot?

**OBSERVED mechanism, INFERRED conclusion.** Neither function reads any
"boot phase" flag, latch, or stage enum:

- `AddDlcPack(this, path)` only touches (1) the single global
  `ExtraContentManager*` at `.data 0x1ec1ea8` (passed in as `this` by the
  caller, not re-fetched internally) and (2) that manager's own growable
  pack array. Before allocating a new entry it calls `0x93d318`, which
  **linearly scans the existing array comparing names, case-insensitively**,
  and refuses (returns without adding) if the name is already present. This
  is a plain append-with-dedupe helper, not a one-shot/boot-only routine —
  calling it again later for a *new* name is architecturally the same as
  calling it the first time.
- `Mount(entry, mountPoint)` only touches the per-entry struct it is handed
  (device pointer, open/mount vtable calls, the `dlc%d.rpf` patch chain,
  CRC-keyed data-file registration). It does not read the manager object or
  any global stage flag at all. `0x94301c` (§2.4), which gates repeat mounts
  by entry state, treats a second call as a normal no-op/retry rather than
  an error.

So **structurally both are safe to call again after boot for one new pack**,
*provided* the `ExtraContentManager` singleton at `0x1ec1ea8` already exists
(non-null) — I could not find the line that constructs it (no plain
`mov [rip+disp], reg` store to that address anywhere in `.text`; it is
almost certainly built through a `lea`-into-register-then-store sequence I
did not isolate this pass), nor any code that nulls or tears it down later.
Absence of a teardown is not proof there is none — flag this as the one
load-bearing assumption for P2 and verify it live (read the pointer after
boot, confirm it stays non-null).

### (b) How does a change set get run for one pack after boot?

**OBSERVED**, chain only (no native-hash name was looked up, per the plan's
"only via the lookup tool" rule, which was not used this pass): the entry
point one layer above the raw hash-keyed function is `0x92a3a4` or
`0x92adc0` — both take a **change-set name string**, hash it with
`0x135ab40`, and call `0x92a9ac(this, nameHash, ..., id)`. From there the
chain in §2.5 runs unconditionally for whichever `groupEntry` the hash
resolves to in the global registry at `.data 0x1ec1eb0` — there is no boot
gate anywhere in this chain either. The group names observed registered via
the `GROUP_*` hash caches include `GROUP_MAP` / `GROUP_MAP_SP` (handled as a
direct, unconditional path in `0x92a4b8`) and `GROUP_ON_DEMAND` (handled
through an extra enable/flag check in `0x92ac00` before the same code path
runs) — `GROUP_ON_DEMAND` is the one that looks designed for exactly this
"enable a specific pack's content after boot" use case.

### (c) What path/device-name forms does `AddDlcPack` accept?

**OBSERVED** for the shape, **INFERRED/untested** for what actually
resolves. `AddDlcPack` performs no validation or rewriting of its input path
beyond literally concatenating `"dlc.rpf"` onto the end of it (`"%sdlc.rpf"`)
— it must already end in `/`. Its only caller that builds such a string from
scratch, `EnumerateDlcPacksFolder` (§1.3), always builds
`"platform:/dlcPacks/<name>/"` — a **device-prefixed virtual path**, resolved
earlier through `0x1364148` (get-device-for-path). Whether a path using a
different registered device prefix (`dlcpacks:/...`, `extra:/...`) or a raw
absolute OS path (`C:/...` or `\\?\...`) also resolves was **not traced**
this pass — that resolution happens inside the generic device-mapping layer
(the same one that owns the `"update:/"`, `"update2:/"`, `"dlcpacks:/"`
string table found during the anchor sweep), which `AddDlcPack` itself never
touches directly. Recommend testing this specific point live in P2 rather
than assuming either way.

### (d) Which object fills the loader table at `0x25c91e0`, and when?

Answered in full in §3.3: **not one single object** — twelve functions,
owned by different subsystems (vehicle, ped, weapon, audio, shop/apparel,
plus a title-update-flavoured one), each fill one or more of their own
slots, guarded by a per-slot one-shot sentinel so a repeat call is a no-op.
Only one of the twelve (`DLC_ITYP_REQUEST`, `0x93a5e0`) has a located call
site, and it fires from the same early-boot block that also triggers the
first `dlclist.xml`/folder scan (§1.4). The other eleven are **INFERRED**
to run from their own subsystems' init sequences, at or before that same
boot stage, based on the sentinel pattern and the absence of any later
caller — but their exact call sites were not located this pass.

## What P2 should do, concretely

Given the above, the recommended order to mount one already-unpacked add-on
`dlc.rpf` from an absolute location after boot, and make its contents live
(this section is only about the extra-content/data-file plumbing; it says
nothing about archive format or encryption, which is out of scope for this
pass):

1. Confirm `ExtraContentManager*` at `.data 0x1ec1ea8` is non-null (§4(a)).
2. Build (or reuse) a device-prefixed path string ending in `/` that the
   generic device layer can resolve to the pack's folder — **verify this
   live**; `"platform:/dlcPacks/<name>/"` is proven, an arbitrary absolute
   path is not (§4(c)).
3. Call `AddDlcPack` (`0x916734`) with that path. It dedupes by name and
   appends a new entry to the manager's array; returns the usual bool.
4. Call `0x94301c` (or `0x94307c` directly) on the new entry to mount the
   pack's `dlc.rpf` (and its `dlc%d.rpf` patch chain) and register its
   CRC-keyed data files.
5. Have `setup2.xml` parsed for that entry (`0x9424f8`, normally reached via
   `0x958290`/`0x944290` — call the wrapper, not the raw parser, so the
   `"extra:/"` device mount happens first) to learn its `device`, `datfile`,
   and `contentchangesets` list.
6. For each content-changeset name in that list, call `0x92a3a4` or
   `0x92adc0` (name-string entry point, §4(b)) to run it; prefer wiring the
   pack's own changeset through `GROUP_ON_DEMAND` if that is how its
   `setup2.xml`/`content.xml` is authored, since that group already carries
   an explicit enable/flag gate meant for exactly this "activate after
   boot" case.
7. The individual `VEHICLE_METADATA_FILE`/`HANDLING_FILE`/`CARCOLS_FILE`/
   `VEHICLE_VARIATION_FILE` data-file entries inside that changeset reach the
   game through the per-type loader table at `0x25c91e0` (§3.1/§3.3)
   automatically, once steps 1–6 are done — no separate call is needed for
   those, provided the relevant loader slots are already filled, which
   should be true by the time any pack is mounted post-boot (§3.3's sentinel
   fillers run earlier in the same boot sequence that performs step 1-6 for
   the built-in packs).

## What was not found / not attempted this pass

- The function that converts a type **name string** (e.g. `"HANDLING_FILE"`)
  from a pack's `content.xml` into the integer index stored at
  `record+0x90` was not located — only its two tables (name→hash→index,
  §3.2; index→loader-object, §3.3) and its consumer (§3.1) were.
- The call sites for eleven of the twelve loader-table fillers (§3.3) were
  not located (no direct call/jmp, no absolute-pointer reference found with
  the methods used this pass).
- The construction site of the `ExtraContentManager` singleton at
  `.data 0x1ec1ea8` was not located (no plain `mov [rip+disp], reg` store
  found; it is built through a register-indirect store this pass's `grep`
  for direct rip-relative stores does not catch).
- Native-hash names (e.g. `EXECUTE_CONTENT_CHANGESET_GROUP_FOR_ALL`) were
  deliberately not looked up — the plan restricts that to the lookup tool,
  which was not invoked this pass; all semantic labels above derived from
  shape/behaviour are marked INFERRED for that reason.
- Per the coordinator's scope for this pass: the RPF7 header parse, the
  encryption-word switch, the OPEN-archive rejection branch, and anything
  about key material were not investigated and are not reported here.
