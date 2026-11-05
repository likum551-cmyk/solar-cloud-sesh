// pack_sync_test.exe: the launcher's car-pack download (apps/launcher/pack_sync.{h,cpp}). Self-contained: an ECDSA
// P-256 key is generated in-process with CNG, manifests are signed with it, the network is a fake Io serving blobs from
// memory, and the packs land in a scratch folder under %TEMP%. Never shipped, never touches the real packs folder.

#include "../apps/launcher/pack_sync.h"
#include "../apps/launcher/updater.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace gage;

namespace {

int g_checks = 0, g_failed = 0;
constexpr int kMinChecks = 70;

void Check(bool ok, const char* what, const std::string& got = "") {
    ++g_checks;
    if (!ok) ++g_failed;
    std::printf("%s  %s%s%s\n", ok ? "ok   " : "FAIL ", what, got.empty() ? "" : "  got: ", got.c_str());
}

std::string B64(const unsigned char* d, size_t n) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    for (size_t i = 0; i < n; i += 3) {
        const unsigned v = (unsigned)d[i] << 16 | (i + 1 < n ? (unsigned)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        s += t[v >> 18 & 63];
        s += t[v >> 12 & 63];
        s += i + 1 < n ? t[v >> 6 & 63] : '=';
        s += i + 2 < n ? t[v & 63] : '=';
    }
    return s;
}
std::string B64(const std::string& s) { return B64((const unsigned char*)s.data(), s.size()); }

struct Key {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    std::string pub;  // base64 X||Y
    bool Make() {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0))) return false;
        if (!BCRYPT_SUCCESS(BCryptGenerateKeyPair(alg, &key, 256, 0)) || !BCRYPT_SUCCESS(BCryptFinalizeKeyPair(key, 0))) return false;
        unsigned char blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
        ULONG got = 0;
        if (!BCRYPT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof(blob), &got, 0)) || got != sizeof(blob))
            return false;
        pub = B64(blob + sizeof(BCRYPT_ECCKEY_BLOB), 64);
        return true;
    }
    // The packs.json envelope for `payload`.
    std::string Sign(const std::string& payload) const {
        unsigned char digest[32];
        const std::string hex = updater::Sha256Hex(payload.data(), payload.size());
        for (int i = 0; i < 32; ++i) digest[i] = (unsigned char)std::stoul(hex.substr(i * 2, 2), nullptr, 16);
        unsigned char sig[64];
        ULONG got = 0;
        if (!BCRYPT_SUCCESS(BCryptSignHash(key, nullptr, digest, 32, sig, 64, &got, 0)) || got != 64) return "";
        return "{\"signed\":\"" + B64(payload) + "\",\"sig\":\"" + B64(sig, 64) + "\"}";
    }
};

Key g_key, g_other;

// ---- a pack set and its manifest ------------------------------------------------------------------------------
struct FFile { std::string path, bytes; };
struct FPack { std::string name; std::vector<FFile> files; };

std::string Sha(const std::string& b) { return updater::Sha256Hex(b.data(), b.size()); }

std::string Payload(const std::vector<FPack>& packs) {
    std::string s = "{\"v\":1,\"kind\":\"packs\",\"issued\":\"2026-11-05T00:00:00Z\",\"packs\":[";
    for (size_t i = 0; i < packs.size(); ++i) {
        s += (i ? "," : "") + std::string("{\"name\":\"") + packs[i].name + "\",\"files\":[";
        for (size_t k = 0; k < packs[i].files.size(); ++k) {
            const FFile& f = packs[i].files[k];
            s += (k ? "," : "") + std::string("{\"path\":\"") + f.path + "\",\"size\":" + std::to_string(f.bytes.size()) +
                 ",\"sha256\":\"" + Sha(f.bytes) + "\"}";
        }
        s += "]}";
    }
    return s + "]}";
}

// ---- the fake network -----------------------------------------------------------------------------------------
const std::string kUpdateUrl = "https://solar-rp.com/update/version.json";
const std::string kPacksUrl = "https://solar-rp.com/update/packs.json";

struct Net {
    std::string manifest;                       // served at kPacksUrl ("" = 404)
    std::map<std::string, std::string> blobs;   // sha -> bytes
    std::set<std::string> corrupt;             // sha -> serve different bytes of the same size
    std::set<std::string> broken;              // sha -> the transfer fails
    int downloads = 0;
    unsigned long long bytes = 0;
    std::vector<std::string> fetched;           // manifest URLs asked for

    void Serve(const std::vector<FPack>& packs, const Key& k) {
        manifest = k.Sign(Payload(packs));
        for (const FPack& p : packs)
            for (const FFile& f : p.files) blobs[Sha(f.bytes)] = f.bytes;
    }
    packsync::Io MakeIo() {
        packsync::Io io;
        io.fetch = [this](const std::string& url, std::string& body, std::string& err) {
            fetched.push_back(url);
            if (url != kPacksUrl || manifest.empty()) { err = "HTTP 404"; return false; }
            body = manifest;
            return true;
        };
        io.download = [this](const std::string& url, unsigned long long size, const std::wstring& part,
                             const std::function<void(unsigned long long)>& progress, std::string* why) {
            const std::string prefix = "https://solar-rp.com/update/packs/";
            if (url.compare(0, prefix.size(), prefix) != 0) { *why = "unexpected url " + url; return false; }
            const std::string sha = url.substr(prefix.size());
            auto it = blobs.find(sha);
            if (it == blobs.end() || broken.count(sha)) { *why = "HTTP 404"; return false; }
            std::string b = it->second;
            if (corrupt.count(sha) && !b.empty()) b[0] = (char)(b[0] ^ 0x5A);
            if (b.size() > size) { *why = "too large"; return false; }
            HANDLE h = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) { *why = "cannot create part"; return false; }
            DWORD wrote = 0;
            WriteFile(h, b.data(), (DWORD)b.size(), &wrote, nullptr);
            CloseHandle(h);
            ++downloads;
            bytes += b.size();
            if (progress) progress(b.size());
            return true;
        };
        return io;
    }
};

// ---- scratch folder helpers ------------------------------------------------------------------------------------
std::wstring g_root;

void Nuke(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((p + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) Nuke(p + L"\\" + fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(p.c_str());
    } else {
        DeleteFileW(p.c_str());
    }
}

std::wstring W(const std::string& s) {
    std::wstring w(s.begin(), s.end());
    for (wchar_t& c : w)
        if (c == L'/') c = L'\\';
    return w;
}

bool Exists(const std::string& rel) { return GetFileAttributesW((g_root + L"\\" + W(rel)).c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string ReadAll(const std::string& rel) {
    FILE* f = _wfopen((g_root + L"\\" + W(rel)).c_str(), L"rb");
    if (!f) return "<missing>";
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

void WriteAll(const std::string& rel, const std::string& bytes) {
    const std::wstring p = g_root + L"\\" + W(rel);
    for (size_t i = g_root.size() + 1; i < p.size(); ++i)
        if (p[i] == L'\\') CreateDirectoryW(p.substr(0, i).c_str(), nullptr);
    FILE* f = _wfopen(p.c_str(), L"wb");
    if (f) { fwrite(bytes.data(), 1, bytes.size(), f); fclose(f); }
}

// Files under root\<pack> (not dot-names), as '/' paths.
std::set<std::string> FilesOf(const std::string& pack) {
    std::set<std::string> out;
    std::function<void(const std::wstring&, const std::string&)> walk = [&](const std::wstring& d, const std::string& pre) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            std::string n;
            for (const wchar_t* c = fd.cFileName; *c; ++c) n += (char)*c;
            if (n[0] == '.') continue;
            const std::string rel = pre.empty() ? n : pre + "/" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) walk(d + L"\\" + fd.cFileName, rel);
            else out.insert(rel);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    };
    walk(g_root + L"\\" + W(pack), "");
    return out;
}

bool PackEquals(const FPack& p) {
    std::set<std::string> want;
    for (const FFile& f : p.files) {
        want.insert(f.path);
        if (ReadAll(p.name + "/" + f.path) != f.bytes) return false;
    }
    return FilesOf(p.name) == want;
}

std::string Summary(const packsync::Result& r) {
    char b[256];
    std::snprintf(b, sizeof(b), "ok %d published %d current %d installed %d failed %d removed %d bytes %llu err '%s'", r.ok,
                  r.published, r.packsCurrent, r.packsInstalled, r.packsFailed, r.packsRemoved, r.downloadedBytes,
                  r.error.c_str());
    return b;
}

packsync::Result Run(Net& net, int* progressCalls = nullptr) {
    int calls = 0;
    packsync::Result r = packsync::Sync(g_root, { "https://moved.example/update/version.json", kUpdateUrl }, g_key.pub,
                                        net.MakeIo(), [&](const packsync::Progress&) { ++calls; });
    if (progressCalls) *progressCalls = calls;
    return r;
}

std::string Blob(char c, size_t n) { return std::string(n, c); }

// ---------------------------------------------------------------------------------------------------------------

void TestNames() {
    std::printf("-- names and paths\n");
    for (const char* good : { "jesko", "veh_diabloSV", "vehicle_nissan350z", "a.b-c_d" })
        Check(packsync::ValidPackName(good), (std::string("pack name accepted: ") + good).c_str());
    static const std::string tooLong(65, 'a');
    for (const char* bad : { "", ".sync", "..", "a/b", "a\\b", "con", "COM1", "lpt9.txt", "a b", "x.", "c:", tooLong.c_str() })
        Check(!packsync::ValidPackName(bad), (std::string("pack name refused: '") + bad + "'").c_str());
    for (const char* good : { "dlc.rpf", "stream/jesko.yft", "data/handling.meta", "a/b/c/d/e/f/g/h" })
        Check(packsync::ValidRelPath(good), (std::string("path accepted: ") + good).c_str());
    for (const char* bad : { "", "../x", "a/../b", "a/./b", "/a", "a/", "a//b", "a\\b", "C:/x", "c:", "stream/.hidden", "nul.txt",
                             "stream/aux", "a/b/c/d/e/f/g/h/i", "x y.yft", "a%2fb" })
        Check(!packsync::ValidRelPath(bad), (std::string("path refused: '") + bad + "'").c_str());
}

void TestUrls() {
    std::printf("-- urls\n");
    Check(packsync::ManifestUrlFor(kUpdateUrl) == kPacksUrl, "packs.json sits next to version.json", packsync::ManifestUrlFor(kUpdateUrl));
    Check(packsync::ManifestUrlFor("http://5.175.253.187/update/version.json?x=1") == "http://5.175.253.187/update/packs.json",
          "the bare-IP feed, query dropped");
    Check(packsync::ManifestUrlFor("https://host") == "https://host/packs.json", "a host with no path");
    Check(packsync::ManifestUrlFor("not a url").empty(), "garbage gives no URL");
    Check(packsync::BlobUrl(kPacksUrl, std::string(64, 'a')) == "https://solar-rp.com/update/packs/" + std::string(64, 'a'),
          "blobs live under packs/<sha256>");
}

void TestVerify() {
    std::printf("-- manifest verification\n");
    const std::vector<FPack> packs = { { "jesko", { { "dlc.rpf", "R" }, { "stream/jesko.yft", Blob('j', 300) } } } };
    packsync::Manifest m;
    std::string why;
    Check(packsync::VerifyManifest(g_key.Sign(Payload(packs)), g_key.pub, m, &why) && m.packs.size() == 1 &&
              m.packs[0].files.size() == 2 && m.bytes == 301,
          "a signed manifest verifies and is read", why);
    Check(!packsync::VerifyManifest(g_other.Sign(Payload(packs)), g_key.pub, m, &why), "another key's signature is refused", why);
    {
        std::string env = g_key.Sign(Payload(packs));
        std::string tampered = g_key.Sign(Payload({ { "jesko", { { "dlc.rpf", "X" } } } }));
        // the payload of one envelope with the signature of another
        const std::string sigOf = tampered.substr(tampered.find("\"sig\""));
        env = env.substr(0, env.find("\"sig\"")) + sigOf;
        Check(!packsync::VerifyManifest(env, g_key.pub, m, &why), "a payload with someone else's signature is refused", why);
    }
    Check(!packsync::VerifyManifest("{\"packs\":[]}", g_key.pub, m, &why), "an unsigned body is refused", why);
    const std::string noKind = "{\"v\":1,\"packs\":[]}";
    Check(!packsync::VerifyManifest(g_key.Sign(noKind), g_key.pub, m, &why), "a payload without kind=packs is refused", why);
    // Domain separation: a signed launcher-update payload is not a packs list, and a packs list is not an update.
    const std::string update = "{\"v\":1,\"version\":\"9.9.9\",\"url\":\"https://solar-rp.com/update/x.exe\",\"sha256\":\"" +
                               std::string(64, 'a') + "\",\"size\":10}";
    Check(!packsync::VerifyManifest(g_key.Sign(update), g_key.pub, m, &why), "a signed update manifest is not accepted as packs", why);
    updater::Manifest um;
    Check(!updater::VerifyManifest(g_key.Sign(Payload(packs)), g_key.pub, um, &why), "a signed packs manifest is not accepted as an update", why);
    Check(!packsync::VerifyManifest(g_key.Sign(Payload({ { "Jesko", { { "dlc.rpf", "a" } } }, { "jesko", { { "dlc.rpf", "b" } } } })),
                                    g_key.pub, m, &why),
          "two packs whose names differ only in case are refused", why);
    Check(!packsync::VerifyManifest(g_key.Sign(Payload({ { "p", { { "dlc.rpf", "a" }, { "DLC.RPF", "b" } } } })), g_key.pub, m, &why),
          "two paths that differ only in case are refused", why);
    Check(!packsync::VerifyManifest(g_key.Sign(Payload({ { "p", { { "stream/x.yft", "a" } } } })), g_key.pub, m, &why),
          "a pack without a top-level dlc.rpf is refused", why);
    Check(!packsync::VerifyManifest(g_key.Sign(Payload({ { "p", { { "dlc.rpf", "a" }, { "../evil.dll", "b" } } } })), g_key.pub, m, &why),
          "a path climbing out of the pack is refused", why);
    Check(!packsync::VerifyManifest(g_key.Sign(Payload({ { "..", { { "dlc.rpf", "a" } } } })), g_key.pub, m, &why),
          "a pack named '..' is refused", why);
    const std::string badSize = "{\"v\":1,\"kind\":\"packs\",\"packs\":[{\"name\":\"p\",\"files\":[{\"path\":\"dlc.rpf\",\"size\":1.5,\"sha256\":\"" +
                                std::string(64, 'a') + "\"}]}]}";
    Check(!packsync::VerifyManifest(g_key.Sign(badSize), g_key.pub, m, &why), "a fractional size is refused", why);
    const std::string badSha = "{\"v\":1,\"kind\":\"packs\",\"packs\":[{\"name\":\"p\",\"files\":[{\"path\":\"dlc.rpf\",\"size\":1,\"sha256\":\"" +
                               std::string(64, 'A') + "\"}]}]}";
    Check(!packsync::VerifyManifest(g_key.Sign(badSha), g_key.pub, m, &why), "an upper-case / non-hex sha256 is refused", why);
}

void TestSync() {
    std::printf("-- sync\n");
    Net net;
    FPack a{ "veh_A", { { "dlc.rpf", Blob('r', 10) }, { "content.xml", "<c/>" }, { "stream/a.yft", Blob('a', 5000) },
                        { "stream/a.ytd", Blob('t', 3000) } } };
    FPack b{ "packB", { { "dlc.rpf", Blob('R', 12) }, { "stream/b.yft", Blob('b', 4000) } } };

    // Nothing published yet: nothing happens, nothing is created in the packs folder.
    packsync::Result r = Run(net);
    Check(!r.ok && !r.published && r.packsInstalled == 0, "no packs.json anywhere: not published, nothing installed", Summary(r));
    Check(net.fetched.size() == 2 && net.fetched[0] == "https://moved.example/update/packs.json" && net.fetched[1] == kPacksUrl,
          "both update URLs are asked, in order, for their packs.json");

    // Fresh install.
    net.Serve({ a, b }, g_key);
    int calls = 0;
    r = Run(net, &calls);
    const unsigned long long total = 10 + 4 + 5000 + 3000 + 12 + 4000;
    Check(r.ok && r.packsInstalled == 2 && r.downloadedBytes == total, "a fresh PC gets both packs", Summary(r));
    Check(PackEquals(a) && PackEquals(b), "each pack folder holds exactly its files, byte for byte");
    Check(calls >= 6, "progress is reported while downloading", std::to_string(calls));
    Check(!Exists(".sync/new") && !Exists(".sync/old"), "no assembly leftovers");

    // Second run: everything current, nothing fetched.
    net.downloads = 0;
    r = Run(net);
    Check(r.ok && r.packsCurrent == 2 && r.packsInstalled == 0 && net.downloads == 0, "a second run downloads nothing", Summary(r));

    // A pack changes: one file new, one gone, one added; a stray local file must not survive the swap.
    WriteAll("veh_A/stream/stray.yft", "left over from an old version");
    FPack a2{ "veh_A", { { "dlc.rpf", Blob('r', 10) }, { "stream/a.yft", Blob('A', 5000) }, { "stream/a.ytd", Blob('t', 3000) },
                         { "stream/extra.ydr", Blob('e', 700) } } };
    net.blobs.clear();
    net.Serve({ a2, b }, g_key);
    net.downloads = 0;
    net.bytes = 0;
    r = Run(net);
    Check(r.ok && r.packsInstalled == 1 && r.packsCurrent == 1, "only the changed pack is replaced", Summary(r));
    Check(net.downloads == 2 && net.bytes == 5700, "only the changed and the new file are downloaded",
          std::to_string(net.downloads) + " files, " + std::to_string(net.bytes) + " bytes");
    Check(PackEquals(a2), "the replaced pack is exactly the new list (old file and stray file gone)");
    Check(PackEquals(b), "the untouched pack is unchanged");

    // A stray file alone makes a pack not current (the engine would stream it).
    WriteAll("packB/stream/stray.ytd", "x");
    net.downloads = 0;
    r = Run(net);
    Check(r.ok && r.packsInstalled == 1 && net.downloads == 0 && PackEquals(b),
          "a stray file in a pack is removed by re-assembling it from the files already there", Summary(r));

    // Same size, different bytes, written after the hash was cached: found by the mtime and repaired.
    Sleep(20);
    WriteAll("packB/stream/b.yft", Blob('z', 4000));
    net.downloads = 0;
    r = Run(net);
    Check(r.ok && r.packsInstalled == 1 && net.downloads == 1 && PackEquals(b), "a file edited in place is detected and re-downloaded",
          Summary(r));

    // An operator's own pack (never installed by Sync) stays when it is not published; ours is removed.
    WriteAll("mine/dlc.rpf", "mine");
    net.blobs.clear();
    net.Serve({ a2 }, g_key);
    r = Run(net);
    Check(r.ok && r.packsRemoved == 1 && !Exists("packB"), "a pack we installed that is no longer published is removed", Summary(r));
    Check(ReadAll("mine/dlc.rpf") == "mine", "a folder Sync never installed is left alone");

    // A download with the wrong bytes: the new pack is not installed at all, and nothing else breaks.
    FPack c{ "packC", { { "dlc.rpf", Blob('c', 20) }, { "stream/c.yft", Blob('C', 900) } } };
    net.Serve({ a2, c }, g_key);
    net.corrupt.insert(Sha(c.files[1].bytes));
    r = Run(net);
    Check(!r.ok && r.packsFailed == 1 && r.packsCurrent == 1 && !Exists("packC"),
          "a file whose bytes do not match the signed hash keeps the whole pack out", Summary(r));
    Check(r.error.find("do not match") != std::string::npos, "the failure names the mismatch", r.error);
    net.corrupt.clear();

    // A corrupted UPDATE of an installed pack leaves the old version in place.
    FPack a3 = a2;
    a3.files[1].bytes = Blob('3', 5000);
    net.Serve({ a3, c }, g_key);
    net.corrupt.insert(Sha(a3.files[1].bytes));
    r = Run(net);
    Check(!r.ok && PackEquals(a2) && PackEquals(c), "a failed update keeps the previous pack intact (and the other pack installs)", Summary(r));
    net.corrupt.clear();

    // Interrupted download: the finished blob is kept and the next run only fetches the rest.
    FPack d{ "packD", { { "dlc.rpf", Blob('d', 30) }, { "stream/d1.yft", Blob('1', 2000) }, { "stream/d2.yft", Blob('2', 2500) } } };
    net.Serve({ a3, c, d }, g_key);
    net.broken.insert(Sha(d.files[2].bytes));
    r = Run(net);
    Check(!r.ok && !Exists("packD") && r.packsFailed == 1 && PackEquals(a3), "a transfer that breaks leaves no half pack (the other update lands)",
          Summary(r));
    net.broken.clear();
    net.downloads = 0;
    net.bytes = 0;
    r = Run(net);
    Check(r.ok && PackEquals(d) && PackEquals(a3), "the next run finishes the job", Summary(r));
    Check(net.bytes == 2500, "blobs finished by the broken run are reused (only the failed file is fetched)",
          std::to_string(net.bytes) + " bytes");
    Check(!Exists(".sync/blobs/" + Sha(d.files[1].bytes)), "used blobs are cleaned up after success");

    // A manifest signed with a key the launcher does not trust changes nothing.
    net.Serve({ b }, g_other);
    r = Run(net);
    Check(!r.ok && r.published && PackEquals(d) && !Exists("packB"), "a manifest with a foreign signature changes nothing", Summary(r));
}

}  // namespace

// --e2e <update manifest URL> <public key base64> <root folder>: one real Sync (WinHTTP) against a served packs.json,
// for checking tools/packs/sign_packs.mjs output with the launcher's own verifier. Exit 0 only if every pack is current
// or installed. Run by hand with a THROWAWAY key, never the real one.
int E2e(int argc, char** argv) {
    if (argc != 5) { std::printf("usage: pack_sync_test --e2e <update url> <pub b64> <root>\n"); return 2; }
    const std::string root(argv[4]);
    unsigned long long lastPct = 101;
    const packsync::Result r = packsync::Sync(std::wstring(root.begin(), root.end()), { argv[2] }, argv[3], packsync::DefaultIo(),
        [&](const packsync::Progress& p) {
            const unsigned long long pct = p.totalBytes ? p.doneBytes * 100 / p.totalBytes : 100;
            if (pct / 10 != lastPct / 10) { lastPct = pct; std::printf("  progress %llu%% (%llu of %llu bytes)\n", pct, p.doneBytes, p.totalBytes); }
        });
    std::printf("e2e: %s\n", Summary(r).c_str());
    return r.ok ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--e2e") == 0) return E2e(argc, argv);
    if (!g_key.Make() || !g_other.Make()) {
        std::printf("FAIL: cannot generate a test key with CNG\n");
        return 1;
    }
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_root = std::wstring(tmp) + L"pack_sync_test_" + std::to_wstring(GetCurrentProcessId());
    Nuke(g_root);
    CreateDirectoryW(g_root.c_str(), nullptr);

    TestNames();
    TestUrls();
    TestVerify();
    TestSync();

    Nuke(g_root);
    if (g_checks < kMinChecks) {
        std::printf("FAIL: only %d checks ran (expected at least %d)\n", g_checks, kMinChecks);
        ++g_failed;
    }
    std::printf("%d/%d passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
