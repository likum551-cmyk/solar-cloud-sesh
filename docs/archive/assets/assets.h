#pragma once
#include "../common/common.h"
#include <functional>

namespace gage::net {

    // Live counters of a running download, handed to the progress callback.
    struct AssetProgress
    {
        int                files = 0;     // files finished so far (downloaded + from cache)
        int                total = -1;    // files the server has; -1 while still being counted
        int                fromCache = 0; // of those, kept from the cache
        unsigned long long bytes = 0;     // body bytes received so far
        double             mbps = 0;      // average rate since the start, megabits per second
        // Files downloading right now (their sizes come from each reply's
        // Content-Length): how many, and the bytes still to come of those
        // whose size is known.
        int                active = 0;
        unsigned long long activeLeft = 0;
        // The in-flight file with the most bytes still to come (-1: none of
        // known size): what a long wait near the end is waiting on.
        int                bigIndex = -1;
        unsigned long long bigSize = 0, bigDone = 0;
    };

    struct AssetResult
    {
        bool               ok = false;
        bool               upToDate = false; // fast path: /file/0 unchanged and the cache complete
        int                files = 0;        // files the server has (0 = server has no assets)
        int                downloaded = 0;   // bodies fetched and written this run
        int                fromCache = 0;    // kept from the cache (304, same bytes or same size)
        unsigned long long bytes = 0;        // body bytes received for the files counted
        std::wstring       error;            // short, player-facing; set when !ok
    };

    // 32 lowercase hex chars: the first 16 bytes of SHA-256 over the UTF-8 of
    // lowercase "host:port" (invariant-locale lowercasing). Stable and
    // filesystem-safe: one cache folder per server. Empty only if the system
    // hash provider fails (logged).
    std::wstring ServerAssetId(const std::wstring& host, int port);

    // Download the client assets of the RAGE:MP server host:port into `dir`
    // (created if missing; trailing backslash optional). The server publishes
    // them at http://host:(port+1)/file/N, N = 0, 1, 2, ... with no count up
    // front; redirects (http -> https, other hosts) are followed.
    //
    // Cache layout in `dir`: file\<N> blobs, files.idx (one UTF-8 line per
    // file: N<TAB>size<TAB>etag<TAB>last-modified<TAB>md5; 4-column lines from
    // older versions still load) and a `complete` marker holding the file
    // count, written only after a full successful run.
    //
    // Validation: when /file/0's ETag is the MD5 of its body (gta5rp,
    // stake-rp CDNs), every file must hash to its ETag; nginx "size-mtime"
    // ETags must name the body length. A file that fails is downloaded once
    // more, then the run fails. The fast path also checks every cached file's
    // size against files.idx.
    // Every file lands under its final name by an atomic rename, never partly.
    //
    // /file/0 is fetched first; when its bytes equal the cached copy and the
    // cache is complete the call returns at once (upToDate). Otherwise a
    // counter finds the file total (gallop + 17-way search with 1-byte probes,
    // boundary re-verified) while 16 workers fetch files 1.., revalidating
    // cached ones (ETag / Last-Modified, else a same-size check). Files of
    // 16 MiB and more are split into 8 MiB ranges across idle connections when
    // the server answers ranged requests with 206. Only the counter's verified
    // total ends the list: a file below it that keeps failing fails the run
    // (never a short success). After success, cached files at or past the end
    // are removed. A server with no assets (/file/0 is 404) gives ok with
    // files = 0 and clears the `complete` marker but keeps the blobs. Each blob
    // is flushed before its rename and journalled (files.idx.journal) so a
    // crash keeps what was already downloaded.
    //
    // Blocking; run it off the UI thread. `onProgress` (may be empty) is
    // called at most every 250 ms, from the calling thread or a worker; calls
    // are serialised, never concurrent.
    AssetResult DownloadServerAssets(const std::wstring& host, int port, const std::wstring& dir,
                                     const std::function<void(const AssetProgress&)>& onProgress);

    struct AssetOptions
    {
        // "Verify files": before anything else, re-hash every cached file
        // against the MD5 recorded when it was written and delete the ones
        // that differ, so the run downloads them again. Reads the whole cache
        // (GBs on big servers); off for a normal launch.
        bool verifyLocal = false;
    };

    // As above, with options.
    AssetResult DownloadServerAssets(const std::wstring& host, int port, const std::wstring& dir,
                                     const AssetOptions& options,
                                     const std::function<void(const AssetProgress&)>& onProgress);

    // Test seam: as DownloadServerAssets, but the files are fetched from
    // `baseUrl` + "/file/N" (e.g. L"http://127.0.0.1:12345").
    AssetResult DownloadAssetsFrom(const std::wstring& baseUrl, const std::wstring& dir,
                                   const std::function<void(const AssetProgress&)>& onProgress);

    // ---- TEST ONLY (tests/assets_test.cpp); the launcher never calls these ----

    struct AssetTestStats
    {
        int    total = -1;     // the counter's verified total (-1 = not counted)
        int    probes = 0;     // counter requests sent
        double countSeconds = 0;
        int    hashMode = 0;   // ETag scheme found on /file/0: 0 none, 1 MD5, 2 nginx size-mtime
        int    verifyBad = 0;  // verifyLocal: cached files whose MD5 differed (deleted, re-fetched)
    };

    struct AssetTestOptions
    {
        int  maxFiles = -1;     // > 0: fetch only indices below this (live prefix runs);
                                // no `complete` marker, no stale-file removal
        int  onlyIndex = -1;    // >= 0: fetch just this index (speed runs); no counter
        bool countOnly = false; // run only the counter; result.files = the total
        int  workers = 0;       // 0 = 16
        bool noChunks = false;  // never split big files
        unsigned long long chunkMinBytes = 0; // 0 = 16 MiB
        unsigned long long chunkBytes = 0;    // 0 = 8 MiB
        bool verifyLocal = false;             // AssetOptions::verifyLocal
        AssetTestStats* stats = nullptr;      // filled when given
    };

    AssetResult DownloadAssetsTest(const std::wstring& baseUrl, const std::wstring& dir,
                                   const AssetTestOptions& options,
                                   const std::function<void(const AssetProgress&)>& onProgress);

}
