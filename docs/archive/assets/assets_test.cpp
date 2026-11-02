// assets_test — drives client/net/assets.cpp for tests/assets_test.mjs, which
// serves the fixture files over local HTTP. Unicode console exe (wmain).
//   assets_test                      ServerAssetId self-checks
//   assets_test id <host> <port>     print ServerAssetId(host, port)
//   assets_test <baseUrl> <dir> <files> <downloaded> <fromCache> <upToDate 0|1> [flags]
//                                    one run through the test seam; files = -1
//                                    expects ok=false (counts not checked); any
//                                    expectation given as "x" is not checked
//   assets_test server <host> <gamePort> <dir> <files> <downloaded> <fromCache> <upToDate>
//                                    the same through DownloadServerAssets
//                                    (files served at gamePort + 1)
// Flags (test seam): --max-files N  --only N  --count-only  --workers N
//   --no-chunks  --chunk-min BYTES  --chunk BYTES  --verify  --quiet-progress
// Prints one "RESULT ..." line; exit 0 = the result matched, 1 = mismatch,
// 2 = bad arguments.

#include "../client/net/assets.h"

#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <mutex>

using namespace gage;
using namespace gage::net;

static int g_run = 0, g_failed = 0;
static const int kAny = INT_MIN; // expectation "x"

static void Check(const char* name, bool ok, const std::wstring& got = L"")
{
    ++g_run;
    if (!ok) ++g_failed;
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", name, got.empty() ? "" : "  got: ", Narrow(got).c_str());
}

static bool IsLowerHex32(const std::wstring& s)
{
    if (s.size() != 32) return false;
    for (wchar_t c : s)
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
    return true;
}

static int SelfTest()
{
    std::wstring a = ServerAssetId(L"Example.COM", 22005);
    std::wstring b = ServerAssetId(L"example.com", 22005);
    std::wstring c = ServerAssetId(L"example.com", 22006);
    std::wstring d = ServerAssetId(L"example.org", 22005);
    Check("id is 32 lowercase hex chars", IsLowerHex32(a), a);
    Check("id is deterministic", ServerAssetId(L"Example.COM", 22005) == a, a);
    Check("id ignores host case", a == b, b);
    Check("id differs by port", a != c, c);
    Check("id differs by host", a != d, d);
    std::printf("%d check(s), %d failed\n", g_run, g_failed);
    return (g_run == 5 && g_failed == 0) ? 0 : 1;
}

static bool ParseInt(const wchar_t* s, int& out)
{
    if (std::wcscmp(s, L"x") == 0)
    {
        out = kAny;
        return true;
    }
    wchar_t* end = nullptr;
    long v = std::wcstol(s, &end, 10);
    if (!s[0] || *end) return false;
    out = (int)v;
    return true;
}

static bool ParseU64(const wchar_t* s, unsigned long long& out)
{
    wchar_t* end = nullptr;
    out = std::wcstoull(s, &end, 10);
    return s[0] && !*end;
}

static bool Expect(int want, int got) { return want == kAny || want == got; }

int wmain(int argc, wchar_t** argv)
{
    if (argc == 1)
        return SelfTest();

    if (argc == 4 && std::wcscmp(argv[1], L"id") == 0)
    {
        int port = 0;
        if (!ParseInt(argv[3], port)) return 2;
        std::printf("ID %s\n", Narrow(ServerAssetId(argv[2], port)).c_str());
        return 0;
    }

    // "server <host> <gamePort>" stands in for "<baseUrl>"; the rest is shared.
    const bool viaServer = argc >= 2 && std::wcscmp(argv[1], L"server") == 0;
    wchar_t** a = viaServer ? argv + 1 : argv; // a[1] = baseUrl or host, a[2] = dir or game port
    const int fixed = viaServer ? 9 : 7;       // argv count before the flags
    int gamePort = 0, expFiles = 0, expDown = 0, expCache = 0, expUp = 0;
    wchar_t** e = viaServer ? a + 1 : a;       // e[2] = dir, e[3..6] = expectations
    if (argc < fixed || (viaServer && !ParseInt(a[2], gamePort))
        || !ParseInt(e[3], expFiles) || !ParseInt(e[4], expDown)
        || !ParseInt(e[5], expCache) || !ParseInt(e[6], expUp))
    {
        std::printf("usage: see the header of tests/assets_test.cpp\n");
        return 2;
    }
    const wchar_t* dir = e[2];

    AssetTestOptions opt;
    AssetTestStats stats;
    opt.stats = &stats;
    bool quiet = false;
    for (int i = fixed; i < argc; ++i)
    {
        std::wstring f = argv[i];
        bool hasValue = i + 1 < argc;
        int iv = 0;
        unsigned long long uv = 0;
        if (f == L"--count-only") opt.countOnly = true;
        else if (f == L"--no-chunks") opt.noChunks = true;
        else if (f == L"--verify") opt.verifyLocal = true;
        else if (f == L"--quiet-progress") quiet = true;
        else if (f == L"--max-files" && hasValue && ParseInt(argv[i + 1], iv)) { opt.maxFiles = iv; ++i; }
        else if (f == L"--only" && hasValue && ParseInt(argv[i + 1], iv)) { opt.onlyIndex = iv; ++i; }
        else if (f == L"--workers" && hasValue && ParseInt(argv[i + 1], iv)) { opt.workers = iv; ++i; }
        else if (f == L"--chunk-min" && hasValue && ParseU64(argv[i + 1], uv)) { opt.chunkMinBytes = uv; ++i; }
        else if (f == L"--chunk" && hasValue && ParseU64(argv[i + 1], uv)) { opt.chunkBytes = uv; ++i; }
        else
        {
            std::printf("unknown flag %s\n", Narrow(f).c_str());
            return 2;
        }
    }
    if (viaServer && argc > fixed)
    {
        std::printf("flags need the baseUrl form\n");
        return 2;
    }

    // Progress contract: calls never overlap and come at most every 250 ms
    // (200 ms allowed for tick-granularity jitter in this measurement).
    std::atomic<bool> inCall{false};
    std::atomic<int> calls{0}, overlaps{0}, lastTotal{-1};
    std::mutex gapLock;
    ULONGLONG lastCall = 0, minGap = ~0ull;
    const ULONGLONG started = GetTickCount64();
    auto onProgress = [&](const AssetProgress& p)
    {
        if (inCall.exchange(true)) overlaps++;
        ULONGLONG now = GetTickCount64();
        {
            std::lock_guard<std::mutex> g(gapLock);
            if (lastCall && now - lastCall < minGap) minGap = now - lastCall;
            lastCall = now;
        }
        calls++;
        lastTotal = p.total;
        if (!quiet)
        {
            // t = ms since the run began, stamped here: piped stdout is
            // buffered, so a reader's arrival times are not the ticks' times
            std::printf("PROGRESS t=%llu files=%d total=%d fromCache=%d bytes=%llu mbps=%.2f active=%d"
                " activeLeft=%llu bigIndex=%d bigSize=%llu bigDone=%llu\n",
                now - started, p.files, p.total, p.fromCache, p.bytes, p.mbps, p.active, p.activeLeft,
                p.bigIndex, p.bigSize, p.bigDone);
            std::fflush(stdout);
        }
        Sleep(5); // widen the window an overlapping call would show up in
        inCall.store(false);
    };

    ULONGLONG t0 = GetTickCount64();
    AssetResult r = viaServer ? DownloadServerAssets(a[1], gamePort, dir, onProgress)
                              : DownloadAssetsTest(a[1], dir, opt, onProgress);
    ULONGLONG ms = GetTickCount64() - t0;
    double mbs = ms ? (double)r.bytes / 1048576.0 / ((double)ms / 1000.0) : 0;

    std::printf("RESULT ok=%d upToDate=%d files=%d downloaded=%d fromCache=%d bytes=%llu progress=%d"
                " total=%d counted=%d probes=%d countMs=%d hash=%d verifyBad=%d ms=%llu MiBps=%.2f error=%s\n",
        r.ok ? 1 : 0, r.upToDate ? 1 : 0, r.files, r.downloaded, r.fromCache, r.bytes, calls.load(),
        lastTotal.load(), stats.total, stats.probes, (int)(stats.countSeconds * 1000), stats.hashMode,
        stats.verifyBad, ms, mbs, Narrow(r.error).c_str());

    bool match;
    if (expFiles == -1)
        match = !r.ok && !r.error.empty();
    else
        match = r.ok && r.error.empty() && Expect(expFiles, r.files) && Expect(expDown, r.downloaded)
            && Expect(expCache, r.fromCache) && Expect(expUp, r.upToDate ? 1 : 0)
            && r.downloaded + r.fromCache == r.files;
    bool progressOk = overlaps.load() == 0 && (calls.load() < 2 || minGap >= 200);
    if (!progressOk)
        std::printf("FAIL  progress contract: overlaps=%d minGap=%llu ms\n", overlaps.load(), minGap);
    std::printf("%s\n", match && progressOk ? "MATCH" : "MISMATCH");
    return match && progressOk ? 0 : 1;
}
