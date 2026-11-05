// pack_profile.h: the per-build data of the car-pack machinery (the game's ExtraContentManager / DLC loader), next to
// BuildProfile in hook.cpp. Everything in pack_stream.cpp (Legacy 1.0.3889 mount) and pack_enh.cpp (Enhanced 1158
// Round A diagnostic) that is specific to one GTA V binary lives here: byte patterns, struct offsets, and the RVAs the
// offline reads found. A profile is picked by the main module's file name (SelectPackProfile), exactly like
// BuildProfile.
//
// Rules (docs: .claude/rules/lessons.md 2026-11-04 "0.4.11 Enhanced"):
//  * Every address the engine USES comes from a pattern resolved at run time. `expectRva` is the offline read of where
//    the pattern lands in the decrypted image; it is only logged and tested against, never the sole source.
//  * Legacy values are the ones pack_stream.cpp shipped with (live-proven) and are pinned by tests/pack_profile_test.cpp
//    (FNV-1a of every pattern, taken from git HEAD before the move). Do not edit them without a live Legacy run.
//  * Enhanced values come from the friend's decrypted dump (`enhanced EXE/dump and bin/GTA5_Enhanced.exe-95703552-
//    959889125.img`, file offset = RVA); pack_profile_test asserts each pattern is a UNIQUE hit at its expectRva there.
//    Round A (pack_enh=1) read-only checks passed live on a friend's PC (22 of 22, 0.4.25); the Round B mount patterns
//    (runPackGroup .. regStreamInfo, marked "Round B") are offline reads only until pack_enh=2 has run.
//
// Pure data, C++17, no OS headers.
#pragma once

#include <cstdint>

namespace gage::game::content {

// One byte pattern: where it points and what it is called in the log.
struct PackSig {
    const char* name   = nullptr;
    const char* pat    = nullptr;   // null = this build has no such pattern
    const char* source = nullptr;   // provenance, for the log
    uint64_t    expectRva = 0;      // offline read of the match's RVA (0 = unknown); never the only source
};

// A rip-relative operand inside a match: a rel32 at match+dispAt, the next instruction at match+endAt.
struct RipOp { int dispAt = 0; int endAt = 0; };

struct PackProfile {
    const wchar_t* exeName = nullptr;   // main-module file name, compared lower-cased
    const char*    label   = nullptr;
    bool           mountable = false;   // pack_stream.cpp's LEGACY mount chain (MountGate etc.) is valid for this build; Enhanced mounts through packenh::Mounter

    // ---- the DLC loader's functions -------------------------------------------------------------------------
    // Legacy: AddDlcPack, DlcSetup2, MountGate and RunPackGroup are the four the mount needs; mount .. popFolder are
    // diagnostic-only probes; regStream registers a loose file; vehStore is the DLC vehicle store init.
    // Enhanced: addDlcPack, dlcSetup2, mount, vehStore (Round A) and runPackGroup .. popFolder, regStream (Round B); no mountGate (inlined in the game's loop).
    PackSig addDlcPack, dlcSetup2, mountGate, runPackGroup;
    PackSig mount, pushFolder, exists, popFolder;
    PackSig regStream, vehStore, getDevice;

    // ---- Enhanced-only patterns (null on Legacy, whose equivalents are read out of the functions above) -----------
    PackSig mgrAnchor;   // `mov rcx,[rip+G]; xor edx,edx; call; cmp eax,[..]`: G is the ExtraContentManager* global
    PackSig mgrCtor;     // the manager's constructor tail: `lea rax,[vtable]; mov [rdi],rax; mov [rip+G],rdi`
    RipOp   mgrAnchorG, mgrCtorVtable, mgrCtorG;
    PackSig atStr;       // atString::Append(atString*, const char*, int) function entry
    PackSig atStrCall;   // a call to it, as AddDlcPack makes it (searched inside AddDlcPack only); target must agree
    RipOp   atStrCallTarget;   // the call's rel32
    uint32_t atStrCallScanLen = 0;   // how far into AddDlcPack the call site is searched
    PackSig imageReg;    // the image registry lookup: `cmp edx,[cap]; lea rcx,[registry]; ...; call [rbx+slot]`
    RipOp   imageRegCap, imageRegArray, imageRegArray2;
    int     imageRegSlotAt = 0;   // imm32 at match+this = the collection vtable slot
    uint32_t imageRegSlotExpect = 0;   // 0x1e0 on Enhanced (Legacy 0x188): logged against the operand, not enforced

    // ---- the DLC vehicle store init: operand positions inside the vehStore match ------------------------------
    RipOp    vsCfg, vsTbl;        // the gameconfig pointer global, the model-info factory table global
    int      vsLimitAt = 0;       // imm32 = offset of MaxExtraVehicleModelInfos in the limits block
    int      vsSlotAt  = 0;       // imm8  = the vehicle factory's slot in the factory table
    uint32_t vsReadLen = 0;       // bytes of the match that must be readable before the operands are used
    uint32_t vsLimitOffExpect = 0, vsSlotExpect = 0;   // 0x324 / 0x28 on both builds: the operands must agree

    // ---- the Enhanced mount (pack_enh=2, Round B) ---------------------------------------------------------------------
    // RunPackGroup's ABI differs: Legacy (mgr, uint32 packHash, uint32 groupHash); Enhanced (mgr, DlcEntry* entry,
    // uint32_t* groupHash). The streaming-info manager (`this` of the game's register-a-file routine) is read out of
    // RegStreamFile's own call to that routine (a pattern searched inside regStream only); Legacy keeps the offline
    // streamInfoMgrRva below. regStreamInfoCountAt = where the manager keeps its entry count (0 = not read on this build:
    // the register routine's returned index is then only range-checked).
    bool     runGroupByEntry = false;
    PackSig  regStreamInfo;
    RipOp    regStreamInfoOp;
    uint32_t regStreamInfoScanLen = 0;
    uint32_t regStreamInfoCountAt = 0;

    // ---- ExtraContentManager layout ----------------------------------------------------------------------------
    uint32_t mgrVtableRva = 0;    // *mgr == base + this (Legacy: from the ctor; Enhanced: ctor pattern gives it too)
    uint32_t mgrArray = 0;        // mgr -> DlcEntry* array
    uint32_t mgrCount = 0;        // mgr -> WORD entry count
    uint32_t mgrCap   = 0;        // mgr -> WORD array capacity (0 = not read)
    uint32_t mgrGen   = 0;        // mgr -> DWORD generation counter (edx to DlcSetup2)

    // ---- DlcEntry layout -----------------------------------------------------------------------------------------
    uint32_t entryStride = 0;
    uint32_t entryHash = 0;       // DWORD name hash (the RunPackGroup key)
    uint32_t entryPath = 0;       // atString: char* path (Mount loads it as a QWORD ptr)
    uint32_t entryPathLen = 0;    // atString: WORD length (capacity WORD follows)
    uint32_t entryType = 0;       // DWORD device type
    uint32_t entryMountPoint = 0;     // atString: char* mount point ("dlc_x:/"), set by DlcSetup2 from setup2.xml
    uint32_t entryMountPointLen = 0;  // atString: WORD length (Mount is given the pointer only while this is nonzero)
    uint32_t entryState = 0;      // DWORD setup state
    uint32_t typePackfile = 0;    // entryType value: fiPackfile::Open (the crypto path)
    uint32_t typeRelative = 0;    // entryType value: fiDeviceRelative over the path (loose folder)

    // ---- single RVAs of the Legacy mount path (read-only checks / sources proven live on Legacy) ------------------
    uint32_t assetGlobalRva = 0;      // &ASSET (fiAssetManager)
    uint32_t streamInfoMgrRva = 0;    // strStreamingInfoManager
    uint32_t imageRegistryRva = 0;    // fiCollection* per image index (Enhanced: the pattern gives it; this is the offline read)
    uint32_t rawStreamerVtRva = 0;    // image 0's collection vtable
};

// ---------------------------------------------------------------------------------------------------------------
// Legacy 1.0.3889 (GTA5.exe). Moved verbatim from pack_stream.cpp: patterns, offsets and sources unchanged.
// ---------------------------------------------------------------------------------------------------------------
constexpr PackProfile MakeLegacyPackProfile() {
    PackProfile p{};
    p.exeName = L"gta5.exe";
    p.label = "Legacy 1.0.3889";
    p.mountable = true;

    p.addDlcPack = { "AddDlcPack",   // RVA 0x916734: bool(mgr /*rcx*/, const char* path /*rdx, narrow, ends '/'*/)
      "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 48 8D A8 E8 FE FF FF 48 81 EC 10 02 00 00 48 8B F9 48 8B DA 48 8D 4D 11",
      "recon-3889 1.2 / p2 section 1", 0x916734 };
    p.dlcSetup2 = { "DlcSetup2",    // RVA 0x958290: bool(entry /*rcx*/, uint32 gen /*edx, forwarded*/)
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B D9 8B 89 D0 00 00 00",
      "recon-3889 2.2 / p2 section 3", 0x958290 };
    p.mountGate = { "MountGate",    // RVA 0x94301c: bool(entry /*rcx*/, uint32 gen /*edx, forwarded to Mount*/)
      "48 89 5C 24 08 57 48 83 EC 20 8B 81 D0 00 00 00 48 8B D9 83 F8 02 75 04",
      "recon-3889 2.4 / p2 section 3", 0x94301c };
    p.runPackGroup = { "RunPackGroup", // RVA 0x92a9ac: void(mgr /*rcx*/, uint32 packHash /*edx*/, uint32 groupHash /*r8d*/)
      "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 8B 0D ?? ?? ?? ?? 41 8B D8 E8 ?? ?? ?? ?? 48 85 C0 74 14 4C 8D 44 24 48 48 8B",
      "recon-3889 2.5 / p2 section 4", 0x92a9ac };

    // Diagnostic-only signatures, resolved lazily and independently of the four above (a probe-sig miss must never
    // disable the normal mount). Mount(entry, gen, mountPoint) opens+mounts the archive and returns 0 on success;
    // PushFolder/Exists/PopFolder are the fiAssetManager calls DlcSetup2 itself uses to test setup2.xml.
    p.mount = { "Mount",       // RVA 0x94307c: int(entry /*rcx*/, uint32 gen /*edx*/, const char* mountPoint /*r8*/); 0=ok
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30 45 33 FF 49 8B F0 48 8B D9 4C 39 39",
      "recon-3889 2.3 / p2 section 3", 0x94307c };
    p.pushFolder = { "PushFolder",  // RVA 0x1367bb8: void(asset /*rcx*/, const char* folder /*rdx*/)
      "48 89 5C 24 18 48 89 54 24 10 57 48 81 EC 20 01 00 00 48 8B D9 48 8D 0D ?? ?? ?? ?? 48 8B FA E8 ?? ?? ?? ?? 80 3F 24 74",
      "fiAssetManager::PushFolder (3889)", 0x1367bb8 };
    p.exists = { "Exists",      // RVA 0x1362be8: uint8(asset /*rcx*/, const char* name /*rdx*/, uint64 flags /*r8=0*/)
      "48 81 EC 38 01 00 00 4C 8B CA 4C 89 44 24 20 48 8D 54 24 30 41 B8 00 01 00 00 E8 ?? ?? ?? ?? 48 81 C4 38 01 00 00 C3",
      "fiAssetManager::Exists (3889)", 0x1362be8 };
    p.popFolder = { "PopFolder",   // RVA 0x13679b4: void(asset /*rcx*/)
      "FF 89 00 0C 00 00 48 8D 0D ?? ?? ?? ?? E9 ?? ?? ?? ??",
      "fiAssetManager::PopFolder (3889)", 0x13679b4 };

    // Loose model files (folder mode): GTA's own "register a loose file with the streaming system" routine (3889
    // static RE 2026-11-05): int* RegisterStreamFile(int* out, const char* path, bool one, const char* name,
    // bool fatalIfMissing, bool allowOverride). See pack_stream.cpp for the call chain.
    p.regStream = { "RegStreamFile",  // RVA 0x169e4d4 (unique in both 3889 dumps)
      "48 89 5C 24 08 48 89 6C 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 50 48 8B EA 4C 8B FA 48 8B D9 "
      "4D 85 C9 B2 01 48 8B CD 45 8A E0 4D 0F 45 F9 E8",
      "3889 RE 2026-11-05: device -> RawStreamer +0x1a8 -> streaming-info register 0x169e5e0", 0x169e4d4 };

    // fiDevice::GetDevice(path, readOnly): the pack's mount for "dlc_x:/..." (3889 RE 2026-11-05), used to read an NG
    // pack's metas through the game's device layer.
    p.getDevice = { "GetDevice",      // RVA 0x1364150 (the jmp target of 0x1364148, which every device-for-path caller uses)
      "48 89 5C 24 08 88 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 20 48 8D 15 ?? ?? ?? ?? 41 B8 07 00 00 00 48 8B F1 E8",
      "3889 RE 2026-11-05: fiDevice::GetDevice(path, readOnly)", 0x1364150 };

    // The DLC vehicle store init (0.4.16): the three key-0xf000 stores in the model-info init (case 8).
    p.vehStore = { "VehStoreInit",   // RVA 0x632610
      "48 8B 05 ?? ?? ?? ?? BF 00 F0 00 00 48 8B 58 18 48 8B 05 ?? ?? ?? ?? 8B D7 48 8B 48 20 44 8B 83 ?? ?? ?? ?? "
      "48 8B 01 FF 50 20 48 8B 05 ?? ?? ?? ?? 44 8B 83 ?? ?? ?? ?? 48 8B 48 28",
      "3889 RE 2026-11-05: cfg=[[+3]]+0x18, factories=[+19], vehicle limit disp @+52, vehicle factory slot @+59", 0x632610 };
    p.vsCfg = { 3, 7 };
    p.vsTbl = { 19, 23 };
    p.vsLimitAt = 52;
    p.vsSlotAt = 59;
    p.vsReadLen = 60;
    p.vsLimitOffExpect = 0x324;
    p.vsSlotExpect = 0x28;

    p.mgrVtableRva = 0x1a9c508u;   // *mgr == base + this, from the manager ctor (p2 section 5)
    p.mgrArray = 0x28;
    p.mgrCount = 0x30;
    p.mgrGen   = 0x178;

    p.entryStride = 0xf0;
    p.entryHash = 0x60;
    p.entryPath = 0xb8;
    p.entryPathLen = 0xc0;         // capacity WORD at +0xc2; Mount ignores +0xb8 while the length is 0
    p.entryType = 0xc8;
    p.entryMountPoint = 0x30;      // atString {char* +0x30, WORD length +0x38}: MountGate 0x94301c hands it to Mount when the length is nonzero
    p.entryMountPointLen = 0x38;
    p.entryState = 0xd0;           // 0 fresh, 1 parsed, 2 mounted
    p.typePackfile = 2;            // Mount 0x94307c: +0xc8==2 -> fiPackfile::Open
    p.typeRelative = 1;            // Mount 0x94307c: +0xc8==1 -> fiDeviceRelative over +0xb8

    p.assetGlobalRva = 0x1e2ee90u;
    p.streamInfoMgrRva = 0x2ea23d0u;
    p.regStreamInfoCountAt = 8;    // strStreamingInfoManager [+8] = entry count (pack_stream.cpp RegisterLooseStream's read-back)
    p.imageRegistryRva = 0x2c67400u;
    p.rawStreamerVtRva = 0x1b7d5e0u;
    return p;
}

// ---------------------------------------------------------------------------------------------------------------
// Enhanced build 1158 (GTA5_Enhanced.exe). Derived offline from the friend's decrypted dump; every pattern is a UNIQUE
// hit at its expectRva there (tests/pack_profile_test.cpp). Round A (read-only) passed live; Round B (mount) is offline-derived.
// ---------------------------------------------------------------------------------------------------------------
constexpr PackProfile MakeEnhancedPackProfile() {
    PackProfile p{};
    p.exeName = L"gta5_enhanced.exe";
    p.label = "Enhanced 1158.13";
    p.mountable = false;

    // AddDlcPack(mgr rcx, const char* path rdx) -> bool. Builds a DlcEntry on its stack at rsp+0x120 (hash at +0x80 =
    // [rsp+0x1a0], path atString at +0xd8 = [rsp+0x1f8], type at +0xf8 = [rsp+0x218] = 2) and appends it to mgr+0x28
    // (stride 9*32 = 0x120, count WORD at +0x30, capacity WORD at +0x32).
    p.addDlcPack = { "AddDlcPack",
      "41 57 41 56 56 57 53 48 81 EC 40 02 00 00 48 89 D6 49 89 CE 0F 57 C0 0F 29 84 24 10 01 00 00 "
      "0F 29 84 24 00 01 00 00 0F 29 84 24 F0 00 00 00",
      "Enhanced dump 2026-11-05: prologue (push r15,r14,rsi,rdi,rbx; sub rsp,0x240; zero the entry)", 0xc29fc0 };
    // DlcSetup2(entry rcx, gen edx): state = [rcx+0x100]; 1..3 = already set up; 0 -> Mount(entry, .., "extra:/").
    p.dlcSetup2 = { "DlcSetup2",
      "56 57 48 83 EC 38 48 89 CE 8B 89 00 01 00 00 B0 01 8D 51 FF 83 FA 02 0F 82 ?? ?? ?? ?? 85 C9 0F 85 ?? ?? ?? ?? "
      "4C 8D 05 ?? ?? ?? ?? 48 89 F1 E8 ?? ?? ?? ??",
      "Enhanced dump 2026-11-05: state at +0x100, calls Mount", 0xc2a890 };
    // Mount(entry rcx, gen edx, mountPoint r8) -> 0 = ok: device pointer at [entry], type [entry+0xf8] (2 packfile,
    // 1 relative), path atString at +0xd8.
    p.mount = { "Mount",
      "41 56 56 57 53 48 83 EC 28 31 DB 48 83 39 00 74 0C 89 D8 48 83 C4 28 5B 5F 5E 41 5E C3 4D 89 C6 48 89 CE "
      "8B 81 F8 00 00 00 83 F8 02 0F 85 ?? ?? ?? ??",
      "Enhanced dump 2026-11-05: type at +0xf8, packfile branch", 0xc2c290 };

    // The vehicle store init (the Legacy 0x632610 shape, rebuilt): the three key-0xf000 factory stores, in the order
    // (slot 0x20, limit +0x328), (slot 0x28, limit +0x324 = MaxExtraVehicleModelInfos, as on Legacy), (slot 0x30, limit
    // +0x320, tail call). The second one is the vehicle store.
    p.vehStore = { "VehStoreInit",
      "48 8B 05 ?? ?? ?? ?? 48 8B 70 18 48 8B 05 ?? ?? ?? ?? 48 8B 48 20 44 8B 86 28 03 00 00 48 8B 01 BA 00 F0 00 00 "
      "FF 50 20 48 8B 05 ?? ?? ?? ?? 48 8B 48 28 44 8B 86 24 03 00 00 48 8B 01 BA 00 F0 00 00 FF 50 20",
      "Enhanced dump 2026-11-05: cfg=[[+3]]+0x18, factories=[+14], vehicle limit imm @+54, vehicle factory slot @+50", 0x1334b16 };
    p.vsCfg = { 3, 7 };
    p.vsTbl = { 14, 18 };
    p.vsLimitAt = 54;
    p.vsSlotAt = 50;
    p.vsReadLen = 69;
    p.vsLimitOffExpect = 0x324;
    p.vsSlotExpect = 0x28;

    // The manager singleton, two independent ways (they must agree).
    p.mgrAnchor = { "ManagerAnchor",
      "48 8B 0D ?? ?? ?? ?? 31 D2 E8 ?? ?? ?? ?? 3B 84",
      "YimMenuV2 enhanced DLCManager `31 D2 E8 ?? ?? ?? ?? 3B 84`, widened by the `mov rcx,[rip+G]` before it", 0x14ab192 };
    p.mgrAnchorG = { 3, 7 };
    p.mgrCtor = { "ManagerCtor",
      "48 8D 05 ?? ?? ?? ?? 48 89 07 48 89 3D ?? ?? ?? ?? EB 05 48 85 FF 74 10 48 89 F9 89 F2",
      "Enhanced dump 2026-11-05: the 0x9b8-byte manager's constructor tail", 0xc255d8 };
    p.mgrCtorVtable = { 3, 7 };
    p.mgrCtorG = { 13, 17 };

    // atString::Append, found as a function and as the call AddDlcPack makes (the two must agree).
    p.atStr = { "AtStringAppend",
      "41 57 41 56 41 54 56 57 55 53 48 83 EC 20 45 85 C0 0F 84 ?? ?? ?? ?? 44 89 C6 49 89 D6 48 89 CF 0F B7 59 08 "
      "8D 04 33 0F B7 49 0A 39 C8 7D 05 4C 8B 3F EB 3E",
      "Enhanced dump 2026-11-05: ptr +0, length WORD +8, capacity WORD +0xa", 0xaa02e0 };
    p.atStrCall = { "AtStringCall",
      "4C 89 F9 48 89 F2 41 89 C0 E8 ?? ?? ?? ??",
      "AddDlcPack's `mov rcx,r15; mov rdx,rsi; mov r8d,eax; call Append`", 0 };
    p.atStrCallTarget = { 10, 14 };
    p.atStrCallScanLen = 0x240;

    // The image registry: GTA's "collection for a streaming handle" lookup. handle>>16 indexes a pointer array bounded
    // by a global; the collection's vtable slot +0x1e0 (Legacy +0x188) is then called with the handle.
    p.imageReg = { "ImageRegistry",
      "C1 EA 10 3B 15 ?? ?? ?? ?? 7D 10 48 8D 0D ?? ?? ?? ?? 48 8B 0C D1 48 85 C9 75 0C CC 48 8D 0D ?? ?? ?? ?? "
      "48 8B 0C D1 48 8B 19 89 C2 FF 93 E0 01 00 00",
      "Enhanced dump 2026-11-05: handle>>16 -> registry[idx] -> call [vtable+0x1e0]", 0x1275f6a };
    p.imageRegCap = { 5, 9 };
    p.imageRegArray = { 14, 18 };
    p.imageRegArray2 = { 31, 35 };
    p.imageRegSlotAt = 46;
    p.imageRegSlotExpect = 0x1e0;

    p.mgrVtableRva = 0x262c470u;   // offline read of the ctor's `lea rax,[vtable]`; the ctor pattern gives the live one
    p.mgrArray = 0x28;
    p.mgrCount = 0x30;
    p.mgrCap   = 0x32;
    p.mgrGen   = 0x178;

    p.entryStride = 0x120;
    p.entryHash = 0x80;
    p.entryPath = 0xd8;
    p.entryPathLen = 0xe0;
    p.entryType = 0xf8;
    p.entryMountPoint = 0x40;      // atString {char* +0x40, WORD length +0x48}: the mount loop at 0xc28c18 passes it to Mount when the length is nonzero
    p.entryMountPointLen = 0x48;
    p.entryState = 0x100;
    p.typePackfile = 2;
    p.typeRelative = 1;

    p.imageRegistryRva = 0x2c075b0u;   // offline read; the live address comes from imageReg
    p.rawStreamerVtRva = 0x2665bb0u;   // registry[0]'s vtable in the dump's process (the 0x7b8-byte object made at 0x1275b1c)

    // ---- Round B (pack_enh=2): the rest of what the Legacy mount path calls, found in the same dump ------------------
    // RunPackGroup(mgr rcx, DlcEntry* rdx, uint32_t* groupHash r8): the manager's loop at 0xc28b18 calls it with
    // r8 = &[rsp+0x38] holding 0x6f8bd408 (early_on, set at 0xc28d1d) or 0x5bf6578f (startup, set at 0xc28ae5), and
    // rdx = the entry whose hash [+0x80] matches, found in the manager's own array.
    p.runGroupByEntry = true;
    p.runPackGroup = { "RunPackGroup",
      "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 48 48 85 D2 0F 84 ?? ?? ?? ?? 4D 89 C5 48 89 D7 49 89 CC "
      "48 C7 44 24 38 00 00 00 00 C7 44 24 40 00 00 00 00 41 8B 10 4C 8D 44 24 38 48 89 F9 E8 ?? ?? ?? ??",
      "Enhanced dump 2026-11-05: (mgr, entry*, uint32_t* group); six callers, 0xc28af5 (startup) and 0xc28d1d (early_on) are the mount loop's", 0xc26f00 };
    // fiDevice::GetDevice(path, readOnly): the thunk 0x101810 every device-for-path caller uses is a jmp to this.
    // Vtable of the device it returns (the packfile device 0x2488e90): +0x08 Open 0x110580, +0x30 Read 0x110d50,
    // +0x60 Close 0x110ce0, +0xb0 GetFileSize 0x111250: the same slots Legacy's ReadGameFile calls.
    p.getDevice = { "GetDevice",
      "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 88 05 00 00 89 D6 49 89 CE BA 3A 00 00 00 E8 ?? ?? ?? ??",
      "Enhanced dump 2026-11-05: fiDevice::GetDevice(path, readOnly); first call is strchr(path, ':')", 0x10caf0 };
    // fiAssetManager (ASSET = 0x2bc3fe0), the calls DlcSetup2 makes on it to test setup2.xml: PushFolder / Exists / PopFolder.
    p.pushFolder = { "PushFolder",
      "41 57 41 56 56 57 53 48 81 EC 20 01 00 00 48 89 D7 49 89 CE 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? "
      "0F B6 07 83 F8 24 74 24 83 F8 2F 74 1B 83 F8 5C 74 16",
      "Enhanced dump 2026-11-05: fiAssetManager::PushFolder(asset, folder)", 0x102720 };
    p.exists = { "Exists",
      "48 81 EC 38 01 00 00 49 89 D1 4C 89 44 24 20 48 8D 54 24 30 41 B8 00 01 00 00 E8 ?? ?? ?? ?? 90 "
      "48 81 C4 38 01 00 00 C3",
      "Enhanced dump 2026-11-05: fiAssetManager::Exists(asset, name, flags)", 0x101e00 };
    // The same machine code as Legacy's PopFolder (pack_profile_test pins that this text is also unique here).
    p.popFolder = { "PopFolder",
      "FF 89 00 0C 00 00 48 8D 0D ?? ?? ?? ?? E9 ?? ?? ?? ??",
      "Enhanced dump 2026-11-05: fiAssetManager::PopFolder(asset), identical to Legacy's", 0x102ab0 };
    p.assetGlobalRva = 0x2bc3fe0u;
    // RegisterStreamFile(int* out rcx, const char* path rdx, bool one r8 (unread), const char* name r9 (null = path),
    // bool fatalIfMissing [rsp+0x28], bool allowOverride [rsp+0x30]): the same routine Legacy's 0x169e4d4 is. Device ->
    // vt+0x170 image index, vt+0x150 isCollection; a plain OS path lands on the RawStreamer (accessor 0x1279990 ->
    // singleton 0x4681bc0, vtable 0x2665bb0), whose +0x208 (0x127a110, lower-cases the path) appends it to its file list.
    p.regStream = { "RegStreamFile",
      "41 57 41 56 56 57 55 53 48 83 EC 58 4C 89 CF 48 89 D6 49 89 CE 44 0F B6 BC 24 B0 00 00 00 4D 85 C9 48 0F 44 FA "
      "48 89 D1 B2 01 E8 ?? ?? ?? ?? 48 89 C3 48 85 C0",
      "Enhanced dump 2026-11-05: loose-file registration, device -> RawStreamer +0x208 -> streaming-info register 0xce3320", 0xce46d0 };
    // Inside it: `lea rcx,[strStreamingInfoManager]; mov rdx,r14; mov r8,rdi; mov r9d,eax; call 0xce3320` (the register
    // routine's `this`). 0xce3320 reads the handle array through [this+0] (8-byte entries); the count's offset is not read.
    p.regStreamInfo = { "StreamInfoMgr",
      "48 8D 0D ?? ?? ?? ?? 4C 89 F2 49 89 F8 41 89 C1 E8 ?? ?? ?? ??",
      "Enhanced dump 2026-11-05: the register call inside RegStreamFile (searched there only)", 0xce4793 };
    p.regStreamInfoOp = { 3, 7 };
    p.regStreamInfoScanLen = 0x100;
    p.regStreamInfoCountAt = 0;
    p.streamInfoMgrRva = 0x42f9e48u;   // offline read; the live address comes from regStreamInfo
    return p;
}

inline constexpr PackProfile kPackLegacy = MakeLegacyPackProfile();
inline constexpr PackProfile kPackEnhanced = MakeEnhancedPackProfile();

// The profile whose exeName equals the main module's file name (case-insensitive). Unmatched -> Legacy (the same
// default BuildProfile uses, so a test harness exe keeps the Legacy behaviour it always had).
inline const PackProfile& SelectPackProfile(const wchar_t* moduleBaseName) {
    auto ieq = [](const wchar_t* a, const wchar_t* b) {
        for (; *a && *b; ++a, ++b) {
            const wchar_t ca = (*a >= L'A' && *a <= L'Z') ? (wchar_t)(*a + 32) : *a;
            const wchar_t cb = (*b >= L'A' && *b <= L'Z') ? (wchar_t)(*b + 32) : *b;
            if (ca != cb) return false;
        }
        return *a == *b;
    };
    if (moduleBaseName && ieq(moduleBaseName, kPackEnhanced.exeName)) return kPackEnhanced;
    return kPackLegacy;
}

}  // namespace gage::game::content
