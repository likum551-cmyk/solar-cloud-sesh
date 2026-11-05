// pack_enh.cpp: live wiring of the Round A read-only car-pack diagnostic on GTA V Enhanced. See pack_enh.h.
#include "pack_enh.h"

#include "pack_enh_core.h"
#include "pack_profile.h"
#include "pack_win.h"

#include "common/logger.h"

#include <memory>
#include <vector>

namespace gage::game::content {

namespace {

std::unique_ptr<packenh::RoundA> g_round;

bool EnvRead(void*, uint64_t va, void* out, size_t n) {
    return win::SafeRead(reinterpret_cast<const void*>(static_cast<uintptr_t>(va)), out, n);
}

void EnvLog(void*, bool warn, const char* line) {
    if (warn) GLOGW("%s", line);
    else GLOGI("%s", line);
}

}  // namespace

bool PackEnhArm(int mode, const wchar_t* exeBaseName) {
    if (mode == 0) return false;
    if (mode < 0 || mode > 3) {
        GLOGW("[packenh] pack_enh=%d is not 0..3; treated as 0 (off)", mode);
        return false;
    }
    if (mode >= 2) {
        GLOGW("[packenh] pack_enh=%d: not built yet (2 and 3 are reserved for the mount rounds); nothing runs", mode);
        return false;
    }
    const PackProfile& prof = SelectPackProfile(exeBaseName);
    if (&prof != &kPackEnhanced) {
        GLOGW("[packenh] pack_enh=1 ignored: the game is not GTA V Enhanced (%ls); Legacy has its own pack_mount switch",
              exeBaseName ? exeBaseName : L"?");
        return false;
    }
    const win::Image im = win::GameImage();
    if (!im.base || !im.size) {
        GLOGW("[packenh] pack_enh=1 cannot start: the game image bounds are unreadable");
        return false;
    }
    packenh::Env env;
    env.scanBase = im.base;
    env.vaBase = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(im.base));
    env.size = im.size;
    env.read = EnvRead;
    env.log = EnvLog;
    g_round = std::make_unique<packenh::RoundA>(prof, env);
    GLOGI("[packenh] pack_enh=1: Round A READ-ONLY diagnostic armed for %s (nothing is mounted, written or called); polled with the "
          "other code patches until it finishes",
          prof.label);
    return true;
}

bool PackEnhStep(uint64_t nowMs, bool waitLog, bool giveUp) {
    if (!g_round) return true;
    std::vector<scan::Run> runs;
    const std::vector<scan::Run>* runsPtr = nullptr;
    if (g_round->NeedsScan(nowMs) || (giveUp && !g_round->PatternsDone())) {
        runs = win::ReadableRuns(win::GameImage());
        runsPtr = &runs;
    }
    return g_round->Step(nowMs, runsPtr, waitLog, giveUp);
}

}  // namespace gage::game::content
