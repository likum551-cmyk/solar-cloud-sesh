// pack_enh.h: the live wiring of the car-pack port to GTA V Enhanced: Round A (the read-only diagnostic, pack_enh_core.h) and
// Round B (the mount, pack_enh_mount_core.h + pack_stream.cpp). pack_profile.h has the patterns and offsets.
//
// Registry (HKCU\Software\GAGE-MP, read in gage_mp.cpp Run() into World::packEnh and again by pack_stream.cpp):
//   pack_enh = 0  (default) Enhanced behaves exactly as before; this module does nothing.
//              1  Round A: READ-ONLY. Resolves the pack patterns, waits for the game to build its DLC manager / image
//                 registry / vehicle store, reads them with fault-proof reads and logs `[packenh]` lines ending in
//                 `[packenh] round A: N of M checks ok`. Mounts nothing, writes no game memory, calls no game function.
//              2  Round B: Round A first (all 22 checks must pass, else NOTHING is written), then the extra mount patterns,
//                 then EVERY pack under %LOCALAPPDATA%\GAGE-MP\packs is mounted through the game's own DLC loader
//                 (pack_stream.cpp, one pack per script tick), the cars are polled into the cdimage, and each car model is
//                 streamed once as a probe (pack_enh_stream=0 skips that). Log lines: `[packenh] pack X: ...`,
//                 `[packenh] READY P of P pack(s) mounted, C of T car(s) in the cdimage`, `[packenh] stream: ...`.
//              3  reserved: logs "not built yet", does nothing.
// A value of 1 or 2 on a game that is not Enhanced is logged and ignored.
//
// The patterns step runs on hook.cpp's native-table poller thread (pollCodePatches), i.e. it reuses the engine's decrypt-wait
// machinery: the same 75 ms / 1 s cadence, codeWaitStep's 120 s Arxan budget for the patterns, and codePatchesPending().
// The mount itself runs on the game script thread (pack_stream.cpp Tick); PackEnhMountGate hands the poller's result over.
#pragma once

#include <cstdint>

namespace gage::game::content {

// Parses a pack_enh registry value: "1".."3" -> that number, anything else (empty, "0", junk) -> 0.
inline int ParsePackEnh(const wchar_t* v) {
    if (!v || !v[0] || v[1]) return 0;
    return (v[0] >= L'1' && v[0] <= L'3') ? (int)(v[0] - L'0') : 0;
}

// Called once from hook.cpp Init with the registry value and the main module's file name. Logs the value. Returns true
// when the Round A diagnostic (mode 1) or Round A + the mount patterns (mode 2) will run on an Enhanced exe and
// PackEnhStep must be polled.
bool PackEnhArm(int mode, const wchar_t* exeBaseName);

// One poll step (poller thread only). waitLog = the 10 s progress tick, giveUp = the 120 s pattern budget ran out.
// Returns true when the diagnostic has finished (or was never armed).
bool PackEnhStep(uint64_t nowMs, bool waitLog, bool giveUp);

// What Round B needs from Round A and the extra patterns. Every address is an RVA in the game image (the engine adds the
// module base). Filled by the poller thread, read by the game thread through PackEnhMountGate.
struct PackEnhMountInfo {
    uint64_t addDlcPack = 0, dlcSetup2 = 0, mount = 0, atStr = 0;       // Round A's patterns
    uint64_t runPackGroup = 0, regStream = 0, getDevice = 0;            // the extras
    uint64_t streamInfo = 0;                                            // strStreamingInfoManager (from RegStreamFile's own call)
    uint64_t mgrGlobal = 0, mgrVtable = 0;                              // ExtraContentManager* global, its vtable
    uint64_t registry = 0;                                              // image registry array (registry[0] = the RawStreamer)
    int      roundAOk = 0, roundATotal = 0;
    char     why[300] = "";                                             // when Refused: the failed checks / patterns
};

enum class PackEnhGate {
    Off,       // pack_enh < 2, or not Enhanced: Round B does not run
    Pending,   // Round A or the extra patterns are still running
    Ready,     // every check and critical pattern passed; PackEnhMountInfo is valid
    Refused,   // something failed: nothing may be written; `why` says what
};

// Any thread. Fills *out when Ready or Refused.
PackEnhGate PackEnhMountGate(PackEnhMountInfo* out);

}  // namespace gage::game::content
