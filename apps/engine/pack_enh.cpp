// pack_enh.cpp: live wiring of the Enhanced car-pack port's patterns phase: Round A (read-only diagnostic) and, for pack_enh=2,
// the extra mount patterns. See pack_enh.h. The mount itself is pack_stream.cpp (game script thread).
#include "pack_enh.h"

#include "pack_enh_core.h"
#include "pack_enh_mount_core.h"
#include "pack_profile.h"
#include "pack_win.h"

#include "common/logger.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace gage::game::content {

namespace {

std::unique_ptr<packenh::RoundA> g_round;
std::unique_ptr<packenh::Extras> g_extras;
int g_mode = 0;
bool g_roundASummarised = false;

// The hand-over to the game thread: info is written first, then state is stored with release.
PackEnhMountInfo g_info;
std::atomic<int> g_state{ 0 };   // 0 off / pending, 1 ready, 2 refused

bool EnvRead(void*, uint64_t va, void* out, size_t n) {
    return win::SafeRead(reinterpret_cast<const void*>(static_cast<uintptr_t>(va)), out, n);
}

void EnvLog(void*, bool warn, const char* line) {
    if (warn) GLOGW("%s", line);
    else GLOGI("%s", line);
}

void Publish(bool ready) {
    g_state.store(ready ? 1 : 2, std::memory_order_release);
}

void Refuse(const std::string& why) {
    snprintf(g_info.why, sizeof(g_info.why), "%s", why.c_str());
    GLOGW("[packenh] round B REFUSED, nothing was written or mounted: %s", g_info.why);
    Publish(false);
}

}  // namespace

bool PackEnhArm(int mode, const wchar_t* exeBaseName) {
    if (mode == 0) return false;
    if (mode < 0 || mode > 3) {
        GLOGW("[packenh] pack_enh=%d is not 0..3; treated as 0 (off)", mode);
        return false;
    }
    if (mode >= 3) {
        GLOGW("[packenh] pack_enh=%d: not built yet (3 is reserved for the next mount round); nothing runs", mode);
        return false;
    }
    const PackProfile& prof = SelectPackProfile(exeBaseName);
    if (&prof != &kPackEnhanced) {
        GLOGW("[packenh] pack_enh=%d ignored: the game is not GTA V Enhanced (%ls); Legacy has its own pack_mount switch", mode,
              exeBaseName ? exeBaseName : L"?");
        return false;
    }
    const win::Image im = win::GameImage();
    if (!im.base || !im.size) {
        GLOGW("[packenh] pack_enh=%d cannot start: the game image bounds are unreadable", mode);
        return false;
    }
    packenh::Env env;
    env.scanBase = im.base;
    env.vaBase = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(im.base));
    env.size = im.size;
    env.read = EnvRead;
    env.log = EnvLog;
    g_round = std::make_unique<packenh::RoundA>(prof, env);
    g_mode = mode;
    if (mode == 1) {
        GLOGI("[packenh] pack_enh=1: Round A READ-ONLY diagnostic armed for %s (nothing is mounted, written or called); polled with the "
              "other code patches until it finishes",
              prof.label);
    } else {
        g_extras = std::make_unique<packenh::Extras>(prof, env);
        GLOGI("[packenh] pack_enh=2: Round B armed for %s. Round A (22 read-only checks) runs first; only if ALL pass are the mount patterns "
              "resolved and the packs under %%LOCALAPPDATA%%\\GAGE-MP\\packs-gen9 mounted (every step is logged before it runs; the last "
              "[packenh] line before a crash names the step). pack_enh=0 turns it off.",
              prof.label);
    }
    return true;
}

bool PackEnhStep(uint64_t nowMs, bool waitLog, bool giveUp) {
    if (!g_round) return true;
    std::vector<scan::Run> runs;
    const std::vector<scan::Run>* runsPtr = nullptr;
    if (!g_round->Done()) {
        if (g_round->NeedsScan(nowMs) || (giveUp && !g_round->PatternsDone())) {
            runs = win::ReadableRuns(win::GameImage());
            runsPtr = &runs;
        }
        const bool done = g_round->Step(nowMs, runsPtr, waitLog, giveUp);
        if (!done) return false;
        if (g_mode < 2) return true;
    }
    if (g_mode < 2) return true;

    // ---- Round B: the gate -----------------------------------------------------------------------------------------
    if (!g_roundASummarised) {
        g_roundASummarised = true;
        g_info.roundAOk = g_round->Ok();
        g_info.roundATotal = g_round->Total();
        if (g_round->Ok() != g_round->Total()) {
            std::string bad;
            for (int i = 0; i < packenh::kCheckCount; ++i)
                if (!g_round->CheckOk(i)) bad += (bad.empty() ? "" : ", ") + std::string(packenh::CheckName(i));
            Refuse("round A passed " + std::to_string(g_round->Ok()) + " of " + std::to_string(g_round->Total()) + " checks (failed: " + bad +
                   "); the layout is not the one the offline dump described");
            return true;
        }
        GLOGI("[packenh] round B: round A passed %d of %d checks; resolving the mount patterns", g_round->Ok(), g_round->Total());
    }
    if (!g_extras) { Refuse("internal: the extra-pattern resolver was not armed"); return true; }
    if (!g_extras->Done()) {
        std::vector<scan::Run> runs2;
        const std::vector<scan::Run>* p2 = nullptr;
        if (g_extras->NeedsScan(nowMs) || giveUp) {
            runs2 = win::ReadableRuns(win::GameImage());
            p2 = &runs2;
        }
        if (!g_extras->Step(nowMs, p2, giveUp)) return false;
    }
    if (!g_extras->Ok()) {
        std::string bad;
        for (int i = 0; i < packenh::kExtraCount; ++i)
            if (packenh::ExtraCritical(i) && !g_extras->IdOk(i)) bad += (bad.empty() ? "" : ", ") + std::string(packenh::ExtraName(i));
        Refuse("the mount patterns did not all resolve uniquely (failed: " + bad + ")");
        return true;
    }
    const packenh::RoundA& r = *g_round;
    const packenh::Extras& x = *g_extras;
    g_info.addDlcPack = r.SigRva(packenh::kAddDlcPack);
    g_info.dlcSetup2 = r.SigRva(packenh::kDlcSetup2);
    g_info.mount = r.SigRva(packenh::kMount);
    g_info.atStr = r.SigRva(packenh::kAtStr);
    g_info.mgrGlobal = r.MgrGlobalRva();
    g_info.mgrVtable = r.CtorVtableRva();
    g_info.registry = r.RegistryRva();
    g_info.runPackGroup = x.Rva(packenh::kXRunPackGroup);
    g_info.regStream = x.Rva(packenh::kXRegStream);
    g_info.getDevice = x.Rva(packenh::kXGetDevice);
    g_info.streamInfo = x.Rva(packenh::kXStreamInfo);
    const uint64_t all[] = { g_info.addDlcPack, g_info.dlcSetup2, g_info.mount, g_info.atStr, g_info.mgrGlobal, g_info.mgrVtable, g_info.registry,
                             g_info.runPackGroup, g_info.regStream, g_info.getDevice, g_info.streamInfo };
    for (uint64_t v : all)
        if (v == 0 || v == ~0ull) { Refuse("an address the mount needs did not resolve (internal inconsistency)"); return true; }
    GLOGI("[packenh] round B: ready. AddDlcPack +0x%llx, DlcSetup2 +0x%llx, Mount +0x%llx, atString append +0x%llx, RunPackGroup +0x%llx, RegStreamFile +0x%llx, "
          "GetDevice +0x%llx, streaming-info manager +0x%llx, manager global +0x%llx (vtable +0x%llx), image registry +0x%llx",
          (unsigned long long)g_info.addDlcPack, (unsigned long long)g_info.dlcSetup2, (unsigned long long)g_info.mount, (unsigned long long)g_info.atStr,
          (unsigned long long)g_info.runPackGroup, (unsigned long long)g_info.regStream, (unsigned long long)g_info.getDevice,
          (unsigned long long)g_info.streamInfo, (unsigned long long)g_info.mgrGlobal, (unsigned long long)g_info.mgrVtable, (unsigned long long)g_info.registry);
    Publish(true);
    return true;
}

PackEnhGate PackEnhMountGate(PackEnhMountInfo* out) {
    if (g_mode < 2 || !g_round) return PackEnhGate::Off;
    const int s = g_state.load(std::memory_order_acquire);
    if (s == 0) return PackEnhGate::Pending;
    if (out) *out = g_info;
    return s == 1 ? PackEnhGate::Ready : PackEnhGate::Refused;
}

}  // namespace gage::game::content
