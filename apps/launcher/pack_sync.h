#pragma once
// pack_sync: the launcher downloads the server's car packs before the game starts (docs/UPDATES.md "Car packs").
//
// The VPS publishes, next to the launcher's update manifest, a signed packs.json (tools/packs/sign_packs.mjs,
// tools/deploy/publish-packs.sh) and every pack file under its own SHA-256:
//   <update dir>/packs.json            {"signed": base64(payload), "sig": base64(ECDSA P-256 r||s)}
//   <update dir>/packs/<sha256>        the bytes of one file, content-addressed (never replaced by other bytes)
// payload: {"v":1, "kind":"packs", "issued":"...", "packs":[{"name":"jesko", "files":[{"path":"stream/jesko.yft",
//           "size":123, "sha256":"<64 hex>"}, ...]}, ...]}
// It is signed with the SAME key as version.json and is believed only after the signature verifies against the key
// baked into SolarLauncher.exe; `kind` keeps the two manifests apart (an update payload has no "kind": "packs" and a
// packs payload has no installer url / sha256 / size, so neither verifier accepts the other's file).
//
// On the player's PC the packs land in %LOCALAPPDATA%\GAGE-MP\packs\<name>\, where the engine mounts every folder
// holding a dlc.rpf (apps/engine/pack_stream.h). A pack is replaced WHOLE: its missing files are downloaded into
// packs\.sync\blobs\ and hash-checked, the complete new folder is assembled in packs\.sync\new\ (hard links to the
// unchanged files, the new blobs), and only then swapped in by two renames. An interrupted or failed download leaves
// the previous pack (or no pack) in place, never a half pack the game would mount; finished blobs are kept for the next
// run. Packs this code never installed (an operator's own staging) are left alone; a pack it installed that is no
// longer published is removed.
//
// Window-free and callable from tests/pack_sync_test.cpp (the network and the clock come in through Io).

#include "common/common.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gage::packsync {

    inline constexpr size_t kMaxManifestBytes = 16u * 1024u * 1024u;               // packs.json
    inline constexpr size_t kMaxPacks = 4096;
    inline constexpr size_t kMaxFiles = 100000;                                    // all packs together
    inline constexpr unsigned long long kMaxFileBytes = 2ull * 1024 * 1024 * 1024;  // one file
    inline constexpr unsigned long long kMaxTotalBytes = 128ull * 1024 * 1024 * 1024;
    inline constexpr size_t kMaxPathChars = 200;                                   // one file's path inside its pack

    struct File {
        std::string path;             // inside the pack, '/' separated (ValidRelPath)
        unsigned long long size = 0;
        std::string sha256;           // 64 lowercase hex
    };
    struct Pack {
        std::string name;             // the folder name (ValidPackName)
        std::vector<File> files;      // at least one; exactly one is "dlc.rpf" (the engine's pack marker)
        unsigned long long bytes = 0;
    };
    struct Manifest {
        std::vector<Pack> packs;
        std::string issued;
        unsigned long long bytes = 0;
        size_t files = 0;
    };

    // 1..64 of [A-Za-z0-9_.-], not starting with '.', not a Windows device name.
    bool ValidPackName(const std::string& name);
    // '/' separated segments, each 1..100 of [A-Za-z0-9_.-], none "." / ".." / starting with '.' / a device name /
    // ending in '.', at most 8 segments and kMaxPathChars in all.
    bool ValidRelPath(const std::string& path);

    // Parse `body`, verify its signature with `pubKeyB64`, and only then read the payload. Rejects: kind != "packs",
    // bad names or paths, names or paths that collide case-insensitively, a pack without a top-level dlc.rpf, sizes
    // out of range, more than the limits above. False sets *why.
    bool VerifyManifest(const std::string& body, const std::string& pubKeyB64, Manifest& out, std::string* why);

    // ".../update/version.json" -> ".../update/packs.json" (the last path segment replaced; query dropped).
    // name: "packs.json" (Legacy) or kGen9Manifest (Enhanced: the same cars, models converted to the gen9 resource format).
    std::string ManifestUrlFor(const std::string& updateManifestUrl, const std::string& name = "packs.json");
    inline constexpr const char* kGen9Manifest = "packs-gen9.json";
    // ".../update/packs.json" + sha -> ".../update/packs/<sha>".
    std::string BlobUrl(const std::string& packsManifestUrl, const std::string& sha256);

    struct Io {
        // GET a manifest (at most kMaxManifestBytes) into `body`; false sets `err`.
        std::function<bool(const std::string& url, std::string& body, std::string& err)> fetch;
        // GET `url` (exactly `size` bytes expected, never more) into `partPath`; `progress` gets bytes so far.
        std::function<bool(const std::string& url, unsigned long long size, const std::wstring& partPath,
                           const std::function<void(unsigned long long)>& progress, std::string* why)> download;
    };
    // The real network: net::HttpGet through updater's fetch and DownloadToFile.
    Io DefaultIo();

    struct Progress {
        unsigned long long doneBytes = 0;    // downloaded this run (finished files + the current one)
        unsigned long long totalBytes = 0;   // to download this run
        int packsToInstall = 0;
    };

    struct Result {
        bool ok = false;                // a manifest verified and every pack it names is installed and current
        bool published = false;         // a packs.json was reachable at all (false + no error = none published)
        int packsCurrent = 0;           // already up to date
        int packsInstalled = 0;         // installed or replaced this run
        int packsFailed = 0;
        int packsRemoved = 0;           // installed by us earlier, no longer published
        unsigned long long downloadedBytes = 0;
        std::string usedUrl;
        std::string error;              // the first failure, for the log and the status line
    };

    // Bring `root` (no trailing backslash; %LOCALAPPDATA%\GAGE-MP\packs for real) in line with the first packs.json
    // among ManifestUrlFor(each of updateUrls) that verifies. `progress` is called often (throttle at the caller).
    Result Sync(const std::wstring& root, const std::vector<std::string>& updateUrls, const std::string& pubKeyB64,
                const Io& io, const std::function<void(const Progress&)>& progress,
                const std::string& manifestName = "packs.json");

    // %LOCALAPPDATA%\GAGE-MP\packs (gen9: ...\packs-gen9, where the Enhanced engine mounts from), "" if LOCALAPPDATA is missing.
    std::wstring DefaultRoot(bool gen9 = false);

    // Which games the launcher syncs the car packs for (gage_gui.cpp DoLaunch step 1c). Pure, so the gate is tested.
    enum class SyncMode {
        Skip,       // a game that does not mount the packs (or Enhanced with pack_enh below 2)
        Enforced,   // Legacy GTA5.exe: a failed sync refuses the launch (code 16) unless pack_required=0
        BestEffort, // GTA5_Enhanced.exe with pack_enh >= 2 (the Enhanced mount, apps/engine/pack_enh.h): a failed sync only
                    // warns and the game starts with the packs already on the PC
    };
    // The HKCU\Software\GAGE-MP pack_enh value, parsed like the engine does (ParsePackEnh): exactly one character '1'..'3'
    // is that number, anything else (empty, "0", "02", junk) is 0.
    inline int PackEnhLevel(const std::wstring& v) {
        return (v.size() == 1 && v[0] >= L'1' && v[0] <= L'3') ? (int)(v[0] - L'0') : 0;
    }
    inline bool BaseNameIs(const std::wstring& name, const wchar_t* want) {
        size_t i = 0;
        for (; i < name.size() && want[i]; ++i) {
            wchar_t a = name[i], b = want[i];
            if (a >= L'A' && a <= L'Z') a = (wchar_t)(a - L'A' + L'a');
            if (b >= L'A' && b <= L'Z') b = (wchar_t)(b - L'A' + L'a');
            if (a != b) return false;
        }
        return i == name.size() && want[i] == 0;
    }
    inline SyncMode PlanSync(const std::wstring& exeBaseName, const std::wstring& packEnhValue) {
        if (BaseNameIs(exeBaseName, L"GTA5.exe")) return SyncMode::Enforced;
        if (BaseNameIs(exeBaseName, L"GTA5_Enhanced.exe") && PackEnhLevel(packEnhValue) >= 2) return SyncMode::BestEffort;
        return SyncMode::Skip;
    }

    // SHA-256 of a file as lowercase hex, "" if it cannot be read. *size gets the bytes read.
    std::string HashFile(const std::wstring& path, unsigned long long* size);

}  // namespace gage::packsync
