// pack_enh.h: the live wiring of the car-pack port's Round A diagnostic on GTA V Enhanced (pack_enh_core.h has the
// logic, pack_profile.h the patterns and offsets).
//
// Registry (HKCU\Software\GAGE-MP, read in gage_mp.cpp Run() into World::packEnh):
//   pack_enh = 0  (default) Enhanced behaves exactly as before; this module does nothing.
//              1  Round A: READ-ONLY. Resolves the pack patterns, waits for the game to build its DLC manager / image
//                 registry / vehicle store, reads them with fault-proof reads and logs `[packenh]` lines ending in
//                 `[packenh] round A: N of M checks ok`. Mounts nothing, writes no game memory, calls no game function.
//              2, 3  reserved for the mount rounds: log "not built yet", do nothing.
// A value of 1 on a game that is not Enhanced is logged and ignored.
//
// The step runs on hook.cpp's native-table poller thread (pollCodePatches), i.e. it reuses the engine's decrypt-wait
// machinery: the same 75 ms / 1 s cadence, codeWaitStep's 120 s Arxan budget for the patterns, and codePatchesPending().
#pragma once

#include <cstdint>

namespace gage::game::content {

// Called once from hook.cpp Init with the registry value and the main module's file name. Logs the value. Returns true
// when the Round A diagnostic will run (mode 1 on an Enhanced exe) and PackEnhStep must be polled.
bool PackEnhArm(int mode, const wchar_t* exeBaseName);

// One poll step (poller thread only). waitLog = the 10 s progress tick, giveUp = the 120 s pattern budget ran out.
// Returns true when the diagnostic has finished (or was never armed).
bool PackEnhStep(uint64_t nowMs, bool waitLog, bool giveUp);

}  // namespace gage::game::content
