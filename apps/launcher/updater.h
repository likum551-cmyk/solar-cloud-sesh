#pragma once
// The launcher's signed self-update (docs/UPDATES.md has the format and the trust model).
//
// Everything here is window-free and callable from tests/update_check_test.cpp, except
// StartInstaller, which tests never call. Strings are UTF-8 (std::string) at this layer; the
// launcher converts at its edges.
//
// Trust: a manifest is believed only if its ECDSA P-256 / SHA-256 signature over the exact
// bytes of the "signed" member verifies against the public key baked into SolarLauncher.exe
// (GAGE_UPDATE_PUBKEY). The unsigned top-level display fields are never read. The installer
// it names is believed only if its size and SHA-256 equal the signed values, checked on a
// handle that nobody else can write to while the installer starts.

#include "common/common.h"

#include <functional>
#include <string>
#include <vector>

namespace gage::updater {

    // ---- limits -----------------------------------------------------------
    inline constexpr size_t kMaxManifestBytes = 64 * 1024;                          // the whole version.json
    inline constexpr unsigned long long kMaxInstallerBytes = 512ull * 1024 * 1024;  // a signed size above this is refused
    inline constexpr size_t kMaxUrlChars = 2048;                                    // installer URL
    inline constexpr size_t kMaxUpdateUrlChars = 256;                               // migration manifest URL
    inline constexpr size_t kMaxNotesChars = 2000;

    // ---- primitives -------------------------------------------------------

    // Strict RFC 4648 base64 (standard alphabet, '=' padding required, no whitespace, nothing
    // after the padding). False on anything else; `out` is cleared first.
    bool Base64Decode(const std::string& text, std::vector<unsigned char>& out);

    // SHA-256 of a buffer as 64 lowercase hex characters; "" if the crypto provider fails.
    std::string Sha256Hex(const void* data, size_t len);

    // "x.y.z" and nothing else: three decimal numbers of 1..9 digits. Fills out[0..2].
    bool ParseVersion(const std::string& text, unsigned long out[3]);

    // -1 / 0 / +1 for a < b, a == b, a > b.
    int CompareVersions(const unsigned long a[3], const unsigned long b[3]);

    // True when `candidate` is a well-formed version strictly newer than `current` (also well
    // formed). False sets *why ("malformed version", "not newer").
    bool IsNewer(const std::string& candidate, const std::string& current, std::string* why);

    // http:// or https:// followed by a host, at most maxChars long, no whitespace, control or
    // non-ASCII characters, no userinfo ('@') and no fragment.
    bool ValidHttpUrl(const std::string& url, size_t maxChars);

    // ECDSA P-256 over SHA-256 with Windows CNG. `pubKeyB64` is the base64 of the raw 64-byte
    // X||Y public point; `sig` is the 64-byte IEEE-P1363 r||s signature of `message`. False
    // sets *why: "bad public key", "bad signature length", "signature invalid", ...
    bool VerifySignature(const std::string& pubKeyB64, const std::vector<unsigned char>& message,
                         const std::vector<unsigned char>& sig, std::string* why);

    // ---- manifest ---------------------------------------------------------

    // The verified payload (tools/update-sign/sign.mjs writes it).
    struct Manifest {
        std::string version;          // x.y.z
        std::string url;              // installer URL, http(s)
        std::string sha256;           // 64 lowercase hex
        unsigned long long size = 0;  // installer bytes, 1..kMaxInstallerBytes
        std::string notes;
        bool mandatory = false;
        std::string updateUrl;        // validated; "" if absent or invalid
        std::string issued;
    };

    // Parse `body` (a version.json), verify its signature with `pubKeyB64`, and only then read
    // the payload: v == 1, well-formed version, a valid installer URL, 64-hex sha256, a size in
    // range, notes within the limit, `mandatory` a bool when present. Says nothing about
    // whether the version is newer (see IsNewer). False sets *why.
    bool VerifyManifest(const std::string& body, const std::string& pubKeyB64, Manifest& out, std::string* why);

    // Fetches one manifest URL into `body`; false sets *err.
    using FetchFn = std::function<bool(const std::string& url, std::string& body, std::string& err)>;
    // The real fetch: net::HttpGet (WinHTTP, http and https), at most kMaxManifestBytes in 15 s.
    FetchFn DefaultFetch();

    struct CheckResult {
        bool ok = false;          // a manifest was fetched and verified
        bool available = false;   // ok, and its version is strictly newer than the client's
        Manifest manifest;        // valid when ok
        std::string usedUrl;      // the manifest URL that answered
        std::string migrateUrl;   // non-empty: the verified payload's updateUrl, to be stored
        std::string error;        // when !ok: the last URL's failure
    };

    // Try `urls` in order (the stored migration URL first, the baked one last) and take the first
    // that yields a verified manifest; every failure is logged and falls through to the next.
    // `migrateUrl` is set only from a verified payload that is not older than the client, so an
    // old manifest replayed to a client cannot move it.
    CheckResult Check(const std::vector<std::string>& urls, const std::string& pubKeyB64,
                      const std::string& clientVersion, const FetchFn& fetch);

    // ---- download ---------------------------------------------------------

    // A downloaded installer whose size and SHA-256 matched the signed manifest. `file` is open
    // with FILE_SHARE_READ only: until it is closed nobody can write to, truncate, rename or
    // delete the file, so what was checked is what runs.
    struct Installer {
        HANDLE file = INVALID_HANDLE_VALUE;
        std::wstring path;
        Installer() = default;
        Installer(const Installer&) = delete;
        Installer& operator=(const Installer&) = delete;
        Installer(Installer&& o) noexcept : file(o.file), path(std::move(o.path)) { o.file = INVALID_HANDLE_VALUE; }
        Installer& operator=(Installer&& o) noexcept {
            if (this != &o) { Close(); file = o.file; path = std::move(o.path); o.file = INVALID_HANDLE_VALUE; }
            return *this;
        }
        ~Installer() { Close(); }
        bool Open() const { return file != INVALID_HANDLE_VALUE; }
        void Close() { if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); file = INVALID_HANDLE_VALUE; } }
    };

    // The updates folder (no trailing backslash): <exeDir>updates when it can be created and written
    // to, else %LOCALAPPDATA%\GAGE-MP\updates. "" (and *why) if neither works. `exeDir` ends in a backslash.
    std::wstring ResolveUpdatesDir(const std::wstring& exeDir, std::string* why);

    // Delete the files directly inside `dir` (old installers, .part leftovers) except the
    // installer log. A file in use stays; nothing here fails the update. Returns files deleted.
    int CleanUpdatesDir(const std::wstring& dir);
    // CleanUpdatesDir on both places ResolveUpdatesDir can choose: <exeDir>updates and
    // %LOCALAPPDATA%\GAGE-MP\updates. Run once at start, before the first download.
    int CleanUpdatesDirs(const std::wstring& exeDir);

    // Name of the installer log inside the updates folder (kept across cleanups).
    inline const wchar_t* InstallerLogName() { return L"setup.log"; }

    // Download m.url into <dir>\SolarRP-Setup-<version>.exe.part with m.size as a hard cap,
    // then <dir>\SolarRP-Setup-<version>.exe, and verify size and SHA-256 against `m` on a
    // FILE_SHARE_READ handle that stays open in `out`. `progress` gets 0..1 while downloading;
    // `verifying` fires once before the hash. On any failure the files are deleted and *why says
    // which check failed ("download failed: ...", "sha256 mismatch", "size mismatch").
    bool Download(const Manifest& m, const std::wstring& dir,
                  const std::function<void(double)>& progress, const std::function<void()>& verifying,
                  Installer& out, std::string* why);

    // The two halves of Download, exposed for tests.
    bool DownloadToFile(const std::string& url, unsigned long long maxBytes, const std::wstring& partPath,
                        const std::function<void(double)>& progress, std::string* why);
    // Open `path` FILE_SHARE_READ, require exactly `size` bytes with SHA-256 `sha256Hex`, and keep
    // the handle in `out`. Nothing is deleted here; on failure `out` is closed.
    bool VerifyInstallerFile(const std::wstring& path, unsigned long long size, const std::string& sha256Hex,
                             Installer& out, std::string* why);

    // ---- install ----------------------------------------------------------

    // `"<setup>" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /NOCLOSEAPPLICATIONS /GAGEUPDATE /LOG="<log>"`
    // (/GAGEUPDATE, kept under that name so older launchers still work, makes gage-mp.iss wait for this launcher to exit and then start the new one).
    std::wstring InstallerCommandLine(const std::wstring& setupPath, const std::wstring& logPath);

    // Start the verified installer (CreateProcess; it inherits the launcher's token, so no UAC
    // prompt) with `inst` still open. Returns the process id in *pid. False sets *why. Tests never
    // call this.
    bool StartInstaller(const Installer& inst, const std::wstring& logPath, unsigned long* pid, std::string* why);

}
