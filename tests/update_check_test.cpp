// update_check_test — the launcher's signed self-update (apps/launcher/updater.cpp):
// base64, SHA-256, version rules, URL rules, ECDSA signature verification, payload parsing, the
// manifest fallback / migration logic, the size cap, the SHA-256 match on a write-protected
// handle, and an end-to-end download from a local HTTP server.
//
//   update_check_test --fixtures <dir> --work <dir> --base <url> --serve-base <url> --client <x.y.z>
//                     [--real-manifest <version.json>]
//
// tests/update_check_test.mjs drives it: it generates throwaway keys with tools/update-sign/keygen.mjs,
// signs the fixtures with sign.mjs (and, for payloads sign.mjs would never write, with the same
// algorithm), starts the HTTP servers and passes their addresses. This program NEVER starts an
// installer: it does not call updater::StartInstaller and the .mjs asserts that by grepping this file.
//
// Exit 0 only if every check passed and at least kMinChecks ran; the last line is "CHECKS n FAILED m".

#include "../apps/launcher/updater.h"
#include "net/serverfinder.h"
#include "build_config.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace gage;
using namespace gage::updater;

namespace {

int g_checks = 0, g_failed = 0;
const int kMinChecks = 150;

void Ok(bool ok, const std::string& name, const std::string& detail = "") {
    ++g_checks;
    if (!ok) ++g_failed;
    std::printf("  %s %s%s%s\n", ok ? "ok  " : "FAIL", name.c_str(), detail.empty() ? "" : "  (", detail.empty() ? "" : (detail + ")").c_str());
}

bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

std::string g_fixtures, g_work, g_base, g_serveBase, g_realManifest;
std::string g_pub1, g_pub2;               // throwaway public keys: 1 signs the fixtures, 2 is the wrong key
const std::string kPure = "0.3.4";        // the client version the pure-function cases are written against
std::string g_client;                     // the real kClientVersion (--client), for the fixtures built relative to it

bool ReadAll(const std::wstring& path, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    out.clear();
    char buf[64 * 1024];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
    CloseHandle(f);
    return true;
}

// A fixture file; a missing one is a failed check, never an empty pass.
std::string Fx(const std::string& name) {
    std::string s;
    const bool ok = ReadAll(Widen(g_fixtures + "\\" + name), s);
    Ok(ok, "fixture " + name + " exists");
    return s;
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::vector<unsigned char> Bytes(const std::string& s) { return std::vector<unsigned char>(s.begin(), s.end()); }

void MakeDir(const std::wstring& p) { CreateDirectoryW(p.c_str(), nullptr); }

// ---------------------------------------------------------------------------

void TestBase64() {
    std::printf("[base64]\n");
    struct V { const char* enc; const char* dec; };
    const V good[] = { {"Zg==", "f"}, {"Zm8=", "fo"}, {"Zm9v", "foo"}, {"Zm9vYg==", "foob"}, {"Zm9vYmE=", "fooba"}, {"Zm9vYmFy", "foobar"} };
    for (const V& v : good) {
        std::vector<unsigned char> out;
        const bool ok = Base64Decode(v.enc, out);
        Ok(ok && std::string(out.begin(), out.end()) == v.dec, std::string("decodes ") + v.enc);
    }
    std::vector<unsigned char> out;
    Ok(Base64Decode("+/+/", out) && out.size() == 3 && out[0] == 0xFB && out[1] == 0xFF && out[2] == 0xBF, "standard alphabet + and /");
    const char* bad[] = { "", "Zg", "Zg=", "Zm9", "=Zg=", "Z===", "Zg=a", "Zm9v====", "Zm 9v", "Zm9v\n", "Zm9v!A==", "Zh==", "Zm9=", "Zm9vYg=", "-_-_" };
    for (const char* b : bad) {
        std::vector<unsigned char> o2 = { 1, 2, 3 };
        Ok(!Base64Decode(b, o2) && o2.empty(), std::string("rejects '") + b + "'");
    }
}

void TestSha256() {
    std::printf("[sha256]\n");
    Ok(Sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty input");
    Ok(Sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc");
    const std::string longer(1000, 'a');
    Ok(Sha256Hex(longer.data(), longer.size()) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3", "1000 x 'a'");
}

void TestVersions() {
    std::printf("[versions]\n");
    unsigned long v[3];
    Ok(ParseVersion("0.3.4", v) && v[0] == 0 && v[1] == 3 && v[2] == 4, "parses 0.3.4");
    Ok(ParseVersion("12.345.6789", v) && v[0] == 12 && v[1] == 345 && v[2] == 6789, "parses 12.345.6789");
    Ok(ParseVersion("999999999.0.0", v), "nine digits are fine");
    const char* malformed[] = { "", "1", "1.2", "1.2.3.4", "v1.2.3", "1.2.x", "1..3", ".1.2", "1.2.", "1.2.3 ", " 1.2.3", "1.2.3-beta", "1,2,3", "-1.2.3", "1.2.+3", "1.2.3\n", "1234567890.0.0", "\xD9\xA1.2.3" };
    for (const char* m : malformed) Ok(!ParseVersion(m, v), std::string("malformed '") + m + "'");

    std::string why;
    Ok(IsNewer("0.3.5", kPure, &why), "newer: patch");
    Ok(IsNewer("0.4.0", kPure, &why), "newer: minor");
    Ok(IsNewer("1.0.0", kPure, &why), "newer: major");
    Ok(IsNewer("0.10.0", "0.9.0", &why), "newer: numeric, not textual (0.10.0 > 0.9.0)");
    Ok(!IsNewer("0.3.4", kPure, &why) && Has(why, "not newer"), "equal is not newer", why);
    Ok(!IsNewer("0.3.3", kPure, &why) && Has(why, "not newer"), "older patch is not newer", why);
    Ok(!IsNewer("0.2.9", kPure, &why) && Has(why, "not newer"), "older minor is not newer", why);
    Ok(!IsNewer("0.03.4", kPure, &why) && Has(why, "not newer"), "leading zeros compare numerically (0.03.4 == 0.3.4)", why);
    Ok(!IsNewer("0.3", kPure, &why) && Has(why, "malformed"), "malformed candidate", why);
    Ok(!IsNewer("v9.9.9", kPure, &why) && Has(why, "malformed"), "malformed candidate with a v", why);
    Ok(!IsNewer("9.9.9", "banana", &why) && Has(why, "client version"), "malformed client version", why);
}

void TestUrls() {
    std::printf("[urls]\n");
    Ok(ValidHttpUrl("http://5.175.253.187/update/version.json", 256), "http ip");
    Ok(ValidHttpUrl("https://updates.example.com/v.json", 256), "https host");
    Ok(ValidHttpUrl("HTTP://Example.com:8080/v.json?x=1", 256), "scheme case, port, query");
    Ok(ValidHttpUrl("http://[::1]:18600/version.json", 256), "ipv6 literal with port");
    Ok(!ValidHttpUrl("", 256), "empty");
    Ok(!ValidHttpUrl("ftp://example.com/v.json", 256), "ftp scheme");
    Ok(!ValidHttpUrl("file:///c:/x", 256), "file scheme");
    Ok(!ValidHttpUrl("//example.com/v.json", 256), "scheme-relative");
    Ok(!ValidHttpUrl("http://", 256), "no host");
    Ok(!ValidHttpUrl("http:///v.json", 256), "empty host");
    Ok(!ValidHttpUrl("http://user@example.com/v.json", 256), "userinfo");
    Ok(!ValidHttpUrl("http://example.com/a b", 256), "space");
    Ok(!ValidHttpUrl("http://example.com/\x01", 256), "control character");
    Ok(!ValidHttpUrl("http://exa\xC3\xA9mple.com/", 256), "non-ASCII host");
    Ok(!ValidHttpUrl("http://example.com/v.json#frag", 256), "fragment");
    Ok(!ValidHttpUrl("http://example.com\\v.json", 256), "backslash");
    Ok(!ValidHttpUrl("http://example.com:99999/v.json", 256), "port out of range");
    Ok(!ValidHttpUrl("http://example.com:/v.json", 256), "empty port");
    Ok(!ValidHttpUrl("http://example.com:80a/v.json", 256), "non-numeric port");
    const std::string pad(300, 'a');
    Ok(ValidHttpUrl("http://example.com/" + pad, 2048), "300 characters are fine under a 2048 limit");
    Ok(!ValidHttpUrl("http://example.com/" + pad, 256), "300 characters are refused under the 256 limit");
    Ok(ValidHttpUrl("http://example.com/" + std::string(256 - 19, 'a'), 256), "exactly 256 characters");
    Ok(!ValidHttpUrl("http://example.com/" + std::string(256 - 18, 'a'), 256), "257 characters");
}

// ---------------------------------------------------------------------------

void TestSignatures() {
    std::printf("[signature and payload]\n");
    std::string installer;
    Ok(ReadAll(Widen(g_fixtures + "\\installer.bin"), installer) && installer.size() > 1000, "installer.bin fixture exists");
    const std::string installerSha = Sha256Hex(installer.data(), installer.size());

    // The positive control every rejection below is read against.
    Manifest m;
    std::string why;
    const std::string good = Fx("good.json");
    Ok(VerifyManifest(good, g_pub1, m, &why), "good manifest verifies", why);
    Ok(m.version == "9.9.9", "version from the signed payload", m.version);
    Ok(m.size == installer.size(), "size equals the installer's", std::to_string(m.size));
    Ok(m.sha256 == installerSha, "sha256 (computed by sign.mjs) equals ours", m.sha256);
    Ok(!m.mandatory && m.notes.empty() && m.updateUrl.empty(), "defaults: not mandatory, no notes, no updateUrl");
    Ok(Has(m.url, "/good/SolarRP-Setup-9.9.9.exe"), "installer url", m.url);

    // The same manifest under the other key, or damaged, must be refused.
    Ok(!VerifyManifest(good, g_pub2, m, &why) && Has(why, "signature invalid"), "wrong key is refused", why);
    Ok(!VerifyManifest(Fx("wrongkey.json"), g_pub1, m, &why) && Has(why, "signature invalid"), "signed by another key is refused", why);
    Ok(VerifyManifest(Fx("wrongkey.json"), g_pub2, m, &why), "... and verifies under that other key (control)", why);
    Ok(!VerifyManifest(Fx("flipped.json"), g_pub1, m, &why) && Has(why, "signature invalid"), "one flipped payload byte is refused", why);
    Ok(!VerifyManifest(Fx("badsig.json"), g_pub1, m, &why) && Has(why, "signature invalid"), "one flipped signature bit is refused", why);
    Ok(!VerifyManifest(Fx("trunc-signed.json"), g_pub1, m, &why) && Has(why, "not valid base64"), "payload base64 cut mid-way is refused", why);
    Ok(!VerifyManifest(Fx("trunc-4.json"), g_pub1, m, &why) && Has(why, "signature invalid"), "payload base64 cut at a 4-character boundary is refused", why);
    Ok(!VerifyManifest(Fx("trunc-sig.json"), g_pub1, m, &why) && Has(why, "not valid base64"), "signature base64 cut mid-way is refused", why);
    Ok(!VerifyManifest(Fx("sig63.json"), g_pub1, m, &why) && Has(why, "bad signature length"), "a 63-byte signature is refused", why);
    Ok(!VerifyManifest(Fx("sig65.json"), g_pub1, m, &why) && Has(why, "bad signature length"), "a 65-byte signature is refused", why);
    Ok(!VerifyManifest(Fx("legacy.json"), g_pub1, m, &why) && Has(why, "no signed payload"), "old unsigned format is refused", why);

    // Unsigned top-level fields are display text for old launchers: tampering with them changes nothing.
    Ok(VerifyManifest(Fx("unsigned-tampered.json"), g_pub1, m, &why), "tampered unsigned fields do not break a good signature", why);
    Ok(m.version == "9.9.9" && Has(m.url, "/good/SolarRP-Setup-9.9.9.exe") && !Has(m.url, "evil"), "... and are never read (signed version/url win)", m.version + " " + m.url);

    // Envelope junk.
    Ok(!VerifyManifest("", g_pub1, m, &why) && Has(why, "empty"), "empty body", why);
    Ok(!VerifyManifest(Fx("garbage.json"), g_pub1, m, &why) && Has(why, "not a JSON object"), "not JSON", why);
    Ok(!VerifyManifest(Fx("array.json"), g_pub1, m, &why) && Has(why, "not a JSON object"), "JSON array", why);
    Ok(!VerifyManifest(Fx("big.json"), g_pub1, m, &why) && Has(why, "too large"), "over 64 KiB", why);
    Ok(VerifyManifest(Fx("bom.json"), g_pub1, m, &why), "a UTF-8 BOM in front is tolerated", why);
    Ok(!VerifyManifest(good, "", m, &why) && Has(why, "bad public key"), "empty public key", why);
    Ok(!VerifyManifest(good, "AAAA", m, &why) && Has(why, "bad public key"), "short public key", why);
    // 64 zero bytes are not a point on P-256.
    const std::string zeroKey = std::string(86, 'A') + "==";
    std::vector<unsigned char> zeroRaw;
    Ok(Base64Decode(zeroKey, zeroRaw) && zeroRaw.size() == 64, "(control) the zero key decodes to 64 bytes");
    Ok(!VerifyManifest(good, zeroKey, m, &why) && !why.empty(), "a public key that is not on the curve", why);

    // Payloads with a VALID signature but unacceptable content.
    struct Neg { const char* file; const char* why; };
    const Neg neg[] = {
        {"neg-v2.json", "unsupported payload format"},
        {"neg-v-missing.json", "unsupported payload format"},
        {"neg-version-short.json", "malformed version"},
        {"neg-version-v.json", "malformed version"},
        {"neg-version-four.json", "malformed version"},
        {"neg-version-number.json", "malformed version"},
        {"neg-sha-short.json", "bad sha256"},
        {"neg-sha-nothex.json", "bad sha256"},
        {"neg-sha-missing.json", "bad sha256"},
        {"neg-size-zero.json", "bad size"},
        {"neg-size-negative.json", "bad size"},
        {"neg-size-huge.json", "bad size"},
        {"neg-size-fraction.json", "bad size"},
        {"neg-size-string.json", "bad size"},
        {"neg-url-ftp.json", "bad installer url"},
        {"neg-url-space.json", "bad installer url"},
        {"neg-url-missing.json", "bad installer url"},
        {"neg-url-long.json", "bad installer url"},
        {"neg-mandatory-string.json", "bad mandatory"},
        {"neg-notes-long.json", "bad notes"},
        {"neg-notes-number.json", "bad notes"},
        {"neg-updateurl-number.json", "bad updateUrl"},
        {"neg-not-object.json", "not a JSON object"},
    };
    for (const Neg& n : neg) {
        Manifest tmp;
        std::string w;
        const bool ok = VerifyManifest(Fx(n.file), g_pub1, tmp, &w);
        Ok(!ok && Has(w, n.why), std::string("signed but unacceptable: ") + n.file, w);
    }
    // Controls: the neighbours of those cases that ARE acceptable.
    Ok(VerifyManifest(Fx("pos-size-cap.json"), g_pub1, m, &why) && m.size == kMaxInstallerBytes, "size exactly at the cap is accepted", why);
    Ok(!VerifyManifest(Fx("neg-size-cap-plus-one.json"), g_pub1, m, &why) && Has(why, "bad size"), "size one over the cap is refused", why);
    Ok(VerifyManifest(Fx("pos-notes-2000.json"), g_pub1, m, &why) && m.notes.size() == 2000, "2000 characters of notes are accepted", why);
    Ok(VerifyManifest(Fx("pos-sha-upper.json"), g_pub1, m, &why) && m.sha256 == installerSha, "an upper-case sha256 is normalised", why);

    // mandatory / notes / updateUrl
    Ok(VerifyManifest(Fx("mandatory.json"), g_pub1, m, &why), "mandatory manifest verifies", why);
    Ok(m.mandatory && m.notes == "Fixes the join crash." && Has(m.updateUrl, "/moved/version.json"), "mandatory, notes and updateUrl come from the signed payload", m.updateUrl);
    Ok(VerifyManifest(Fx("updateurl-ftp.json"), g_pub1, m, &why) && m.updateUrl.empty(), "an ftp updateUrl is ignored, the update itself is kept", why);
    Ok(VerifyManifest(Fx("updateurl-long.json"), g_pub1, m, &why) && m.updateUrl.empty(), "an updateUrl over 256 characters is ignored", why);
    Ok(VerifyManifest(Fx("updateurl-256.json"), g_pub1, m, &why) && m.updateUrl.size() == 256, "an updateUrl of exactly 256 characters is kept", why);
}

// ---------------------------------------------------------------------------

void TestCheck() {
    std::printf("[check: fallback, newer, migration]\n");
    std::map<std::string, std::string> web;  // url -> body; a missing url is an unreachable server
    web["http://a/good"] = Fx("good.json");
    web["http://a/equal"] = Fx("equal.json");
    web["http://a/older"] = Fx("older.json");
    web["http://a/badsig"] = Fx("badsig.json");
    web["http://a/wrongkey"] = Fx("wrongkey.json");
    web["http://a/legacy"] = Fx("legacy.json");
    web["http://a/mandatory"] = Fx("mandatory.json");
    web["http://a/equal-moved"] = Fx("equal-moved.json");
    web["http://a/older-moved"] = Fx("older-moved.json");
    web["http://a/ftp-moved"] = Fx("updateurl-ftp.json");
    web["http://a/malformed"] = Fx("neg-version-short.json");
    int fetches = 0;
    const FetchFn fetch = [&](const std::string& url, std::string& body, std::string& err) {
        ++fetches;
        auto it = web.find(url);
        if (it == web.end()) { err = "cannot connect"; return false; }
        body = it->second;
        return true;
    };
    auto run = [&](std::vector<std::string> urls) { return updater::Check(urls, g_pub1, g_client, fetch); };

    CheckResult r = run({ "http://a/good" });
    Ok(r.ok && r.available && r.manifest.version == "9.9.9" && r.usedUrl == "http://a/good" && r.error.empty(), "newer signed manifest: available");
    Ok(r.migrateUrl.empty(), "no updateUrl in it: nothing to migrate");

    r = run({ "http://a/equal" });
    Ok(r.ok && !r.available && r.manifest.version == g_client && r.error.empty(), "equal version: verified, not available");
    r = run({ "http://a/older" });
    Ok(r.ok && !r.available && r.error.empty(), "older version: verified, not available (anti-downgrade)");
    r = run({ "http://a/malformed" });
    Ok(!r.ok && !r.available && Has(r.error, "malformed version"), "malformed signed version: refused", r.error);
    r = run({ "http://a/badsig" });
    Ok(!r.ok && !r.available && Has(r.error, "signature invalid"), "bad signature: refused", r.error);
    r = run({ "http://a/wrongkey" });
    Ok(!r.ok && Has(r.error, "signature invalid"), "wrong key: refused", r.error);
    r = run({ "http://a/legacy" });
    Ok(!r.ok && Has(r.error, "no signed payload"), "legacy unsigned manifest: refused", r.error);
    r = run({ "http://a/nowhere" });
    Ok(!r.ok && Has(r.error, "could not reach") && Has(r.error, "cannot connect"), "unreachable: error carries the reason", r.error);
    r = run({});
    Ok(!r.ok && Has(r.error, "no update URL"), "no URLs at all", r.error);
    fetches = 0;
    r = run({ "", "" });
    Ok(!r.ok && fetches == 0 && Has(r.error, "no update URL"), "empty URLs are skipped, never fetched", std::to_string(fetches));

    // fallback order
    fetches = 0;
    r = run({ "http://a/nowhere", "http://a/good" });
    Ok(r.ok && r.available && r.usedUrl == "http://a/good", "stored URL unreachable: falls back to the baked one", r.usedUrl);
    r = run({ "http://a/badsig", "http://a/good" });
    Ok(r.ok && r.available && r.usedUrl == "http://a/good", "stored URL serves a bad signature: falls back", r.usedUrl);
    r = run({ "http://a/legacy", "http://a/good" });
    Ok(r.ok && r.usedUrl == "http://a/good", "stored URL serves an unsigned manifest: falls back", r.usedUrl);
    fetches = 0;
    r = run({ "http://a/good", "http://a/nowhere" });
    Ok(r.ok && r.usedUrl == "http://a/good" && fetches == 1, "the first good manifest wins; later URLs are not asked", std::to_string(fetches));
    fetches = 0;
    r = run({ "http://a/equal", "http://a/good" });
    Ok(r.ok && !r.available && r.usedUrl == "http://a/equal" && fetches == 1, "a verified 'nothing newer' answer is final (no fall-through)", r.usedUrl);
    fetches = 0;
    r = run({ "http://a/good", "http://a/good" });
    Ok(fetches == 1, "a duplicate URL is asked once");
    r = run({ "http://a/nowhere", "http://a/badsig" });
    Ok(!r.ok && Has(r.error, "signature invalid"), "all URLs failing: the last reason is reported", r.error);
    r = updater::Check({ "http://a/good" }, g_pub1, "banana", fetch);
    Ok(!r.ok && Has(r.error, "client version"), "malformed client version: refused up front", r.error);

    // migration
    r = run({ "http://a/mandatory" });
    Ok(r.ok && r.available && r.manifest.mandatory && Has(r.migrateUrl, "/moved/version.json"), "newer manifest with updateUrl: migrate", r.migrateUrl);
    r = run({ "http://a/equal-moved" });
    Ok(r.ok && !r.available && Has(r.migrateUrl, "/moved/version.json"), "same-version manifest with updateUrl: migrate (no release needed to move clients)", r.migrateUrl);
    r = run({ "http://a/older-moved" });
    Ok(r.ok && !r.available && r.migrateUrl.empty(), "OLD manifest with updateUrl: never migrates (replay)", r.migrateUrl);
    r = run({ "http://a/ftp-moved" });
    Ok(r.ok && r.available && r.migrateUrl.empty(), "invalid updateUrl: update kept, no migration");
}

// ---------------------------------------------------------------------------

void TestInstallerFile() {
    std::printf("[installer file: size, sha256, write protection]\n");
    std::string data(300 * 1024 + 7, '\0');
    for (size_t i = 0; i < data.size(); ++i) data[i] = (char)((i * 31 + 7) & 0xFF);
    const std::string sha = Sha256Hex(data.data(), data.size());
    const std::wstring dir = Widen(g_work + "\\file");
    MakeDir(dir);
    const std::wstring path = dir + L"\\SolarRP-Setup-9.9.9.exe";
    auto put = [&](const std::string& bytes) {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD w = 0;
        const bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, bytes.data(), (DWORD)bytes.size(), &w, nullptr) && w == bytes.size();
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        return ok;
    };

    Ok(put(data), "wrote the test file");
    Installer inst;
    std::string why;
    Ok(VerifyInstallerFile(path, data.size(), sha, inst, &why) && inst.Open(), "matching size and sha256 verify", why);

    // While `inst` is open nobody may change what was checked.
    const DWORD shareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE w = CreateFileW(path.c_str(), GENERIC_WRITE, shareAll, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD wErr = GetLastError();
    Ok(w == INVALID_HANDLE_VALUE && wErr == ERROR_SHARING_VIOLATION, "a writer is refused while the verified handle is open", std::to_string(wErr));
    if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
    HANDLE a = CreateFileW(path.c_str(), FILE_APPEND_DATA, shareAll, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Ok(a == INVALID_HANDLE_VALUE, "an appender is refused");
    if (a != INVALID_HANDLE_VALUE) CloseHandle(a);
    HANDLE t = CreateFileW(path.c_str(), GENERIC_WRITE, shareAll, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Ok(t == INVALID_HANDLE_VALUE, "truncating is refused");
    if (t != INVALID_HANDLE_VALUE) CloseHandle(t);
    Ok(!DeleteFileW(path.c_str()), "deleting is refused");
    Ok(!MoveFileExW(path.c_str(), (path + L".moved").c_str(), 0), "renaming is refused");
    // ... while reading and executing it (what CreateProcess asks for) still works.
    HANDLE r = CreateFileW(path.c_str(), FILE_GENERIC_READ | FILE_EXECUTE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Ok(r != INVALID_HANDLE_VALUE, "a read/execute open (what the loader does) still succeeds", std::to_string(GetLastError()));
    if (r != INVALID_HANDLE_VALUE) CloseHandle(r);
    std::string again;
    Ok(ReadAll(path, again) && again == data, "contents unchanged");
    inst.Close();
    Ok(DeleteFileW(path.c_str()), "after Close the file can be deleted");

    // mismatches
    std::string flipped = data;
    flipped[1000] = (char)(flipped[1000] ^ 1);
    Ok(put(flipped), "wrote a file with one flipped bit");
    Ok(!VerifyInstallerFile(path, data.size(), sha, inst, &why) && Has(why, "sha256 mismatch") && !inst.Open(), "one flipped bit: sha256 mismatch", why);
    Ok(put(data.substr(0, data.size() - 1)), "wrote a file one byte short");
    Ok(!VerifyInstallerFile(path, data.size(), sha, inst, &why) && Has(why, "size mismatch") && !inst.Open(), "one byte short: size mismatch", why);
    Ok(put(data + "x"), "wrote a file one byte long");
    Ok(!VerifyInstallerFile(path, data.size(), sha, inst, &why) && Has(why, "size mismatch") && !inst.Open(), "one byte long: size mismatch", why);
    Ok(put(data), "wrote the matching file again");
    std::string upper = sha;
    for (char& c : upper) if (c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
    Ok(VerifyInstallerFile(path, data.size(), upper, inst, &why), "the expected hash may be upper case", why);
    inst.Close();
    std::string wrongHash = sha;
    wrongHash[0] = wrongHash[0] == '0' ? '1' : '0';
    Ok(!VerifyInstallerFile(path, data.size(), wrongHash, inst, &why) && Has(why, "sha256 mismatch"), "wrong expected hash (first nibble)", why);
    Ok(!VerifyInstallerFile(dir + L"\\nothing.exe", 1, sha, inst, &why) && !inst.Open(), "missing file", why);

    // An existing writer blocks the exclusive open, so a file being modified is never "verified".
    HANDLE writer = CreateFileW(path.c_str(), GENERIC_WRITE, shareAll, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Ok(writer != INVALID_HANDLE_VALUE, "opened a concurrent writer");
    Ok(!VerifyInstallerFile(path, data.size(), sha, inst, &why) && !inst.Open(), "a file somebody else holds open for writing is not verified", why);
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);

    // Installer move semantics: the handle follows the object.
    Ok(VerifyInstallerFile(path, data.size(), sha, inst, &why), "verify again", why);
    Installer moved = std::move(inst);
    Ok(moved.Open() && !inst.Open(), "move transfers the open handle");
    Ok(!DeleteFileW(path.c_str()), "...which still protects the file");
    moved.Close();

    // command line
    const std::wstring cl = InstallerCommandLine(L"C:\\Users\\A B\\GAGE-MP\\updates\\SolarRP-Setup-9.9.9.exe", L"C:\\Users\\A B\\GAGE-MP\\updates\\setup.log");
    Ok(cl == L"\"C:\\Users\\A B\\GAGE-MP\\updates\\SolarRP-Setup-9.9.9.exe\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /NOCLOSEAPPLICATIONS /GAGEUPDATE /LOG=\"C:\\Users\\A B\\GAGE-MP\\updates\\setup.log\"",
          "installer command line", Narrow(cl));

    // updates folder housekeeping
    const std::wstring cdir = Widen(g_work + "\\clean");
    MakeDir(cdir);
    auto touch = [&](const wchar_t* n) { HANDLE f = CreateFileW((cdir + L"\\" + n).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); if (f != INVALID_HANDLE_VALUE) CloseHandle(f); };
    touch(L"SolarRP-Setup-0.3.9.exe");
    touch(L"SolarRP-Setup-0.4.0.exe.part");
    touch(L"setup.log");
    MakeDir(cdir + L"\\sub");
    const int removed = CleanUpdatesDir(cdir);
    Ok(removed == 2 && !Exists(cdir + L"\\SolarRP-Setup-0.3.9.exe") && !Exists(cdir + L"\\SolarRP-Setup-0.4.0.exe.part"), "cleanup removes old installers and .part files", std::to_string(removed));
    Ok(Exists(cdir + L"\\setup.log") && Exists(cdir + L"\\sub"), "cleanup keeps the installer log and subfolders");
    std::string dwhy;
    const std::wstring resolved = ResolveUpdatesDir(Widen(g_work + "\\"), &dwhy);
    Ok(resolved == Widen(g_work) + L"updates" || resolved == Widen(g_work + "\\updates"), "updates folder is <exe dir>\\updates when writable", Narrow(resolved));

    // The fallback folder: <exe dir>\updates is unusable (a FILE has that name), so the update goes
    // to %LOCALAPPDATA%\GAGE-MP\updates, and the start-up cleanup empties both places.
    wchar_t oldLad[MAX_PATH] = L"";
    const DWORD oldLadLen = GetEnvironmentVariableW(L"LOCALAPPDATA", oldLad, MAX_PATH);
    const std::wstring ladRoot = Widen(g_work + "\\lad");
    const std::wstring exeDir2 = Widen(g_work + "\\exe2\\");
    MakeDir(ladRoot);
    MakeDir(Widen(g_work + "\\exe2"));
    SetEnvironmentVariableW(L"LOCALAPPDATA", ladRoot.c_str());
    {
        HANDLE blocker = CreateFileW((exeDir2 + L"updates").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        Ok(blocker != INVALID_HANDLE_VALUE, "created a file where <exe dir>\\updates would be");
        if (blocker != INVALID_HANDLE_VALUE) CloseHandle(blocker);
        std::string fwhy;
        const std::wstring fb = ResolveUpdatesDir(exeDir2, &fwhy);
        Ok(fb == ladRoot + L"\\GAGE-MP\\updates", "an unusable <exe dir>\\updates falls back to %LOCALAPPDATA%\\GAGE-MP\\updates", Narrow(fb) + " " + fwhy);
        auto touchIn = [](const std::wstring& d, const wchar_t* n) {
            HANDLE f = CreateFileW((d + L"\\" + n).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        };
        const std::wstring primary2 = Widen(g_work + "\\exe3\\");
        MakeDir(Widen(g_work + "\\exe3"));
        MakeDir(primary2 + L"updates");
        touchIn(primary2 + L"updates", L"SolarRP-Setup-0.3.8.exe");
        touchIn(primary2 + L"updates", L"setup.log");
        if (!fb.empty()) {
            touchIn(fb, L"SolarRP-Setup-0.3.9.exe.part");
            touchIn(fb, L"setup.log");
        }
        const int n2 = CleanUpdatesDirs(primary2);
        Ok(n2 == 2, "CleanUpdatesDirs removes one old file in each of the two folders", std::to_string(n2));
        Ok(!Exists(primary2 + L"updates\\SolarRP-Setup-0.3.8.exe") && !Exists(fb + L"\\SolarRP-Setup-0.3.9.exe.part") &&
               Exists(primary2 + L"updates\\setup.log") && Exists(fb + L"\\setup.log"),
           "...and keeps both installer logs");
    }
    if (oldLadLen > 0 && oldLadLen < MAX_PATH) SetEnvironmentVariableW(L"LOCALAPPDATA", oldLad);
    else SetEnvironmentVariableW(L"LOCALAPPDATA", nullptr);
}

// ---------------------------------------------------------------------------

struct Downloaded {
    bool ok = false;
    std::string why;
    std::vector<double> fractions;
    int verifyingCalls = 0;
    Installer inst;
};

Downloaded RunDownload(const std::string& manifestFile, const std::string& tag) {
    Downloaded d;
    Manifest m;
    std::string why;
    const std::string body = Fx(manifestFile);
    const bool verified = VerifyManifest(body, g_pub1, m, &why);
    Ok(verified, "manifest " + manifestFile + " verifies (control for the download below)", why);
    if (!verified) return d;
    const std::wstring dir = Widen(g_work + "\\dl-" + tag);
    MakeDir(dir);
    d.ok = Download(m, dir, [&](double f) { d.fractions.push_back(f); }, [&] { ++d.verifyingCalls; }, d.inst, &d.why);
    return d;
}

bool DirHasNoInstallerFiles(const std::string& tag) {
    const std::wstring dir = Widen(g_work + "\\dl-" + tag);
    return !Exists(dir + L"\\SolarRP-Setup-9.9.9.exe") && !Exists(dir + L"\\SolarRP-Setup-9.9.9.exe.part");
}

void TestEndToEnd(const std::string& manifestUrl, const std::string& label, const std::string& file) {
    std::printf("[end to end: %s]\n", label.c_str());
    std::string installer;
    ReadAll(Widen(g_fixtures + "\\installer.bin"), installer);

    // fetch + check over real HTTP
    CheckResult c = updater::Check({ manifestUrl }, g_pub1, g_client, DefaultFetch());
    Ok(c.ok && c.available && c.manifest.version == "9.9.9", "fetch over HTTP + verify + newer", c.error);
    Ok(c.usedUrl == manifestUrl, "used the URL given");
    // the unsigned display fields stay readable by launchers older than this one
    std::wstring ferr;
    const std::wstring legacyVersion = net::FetchVersion(Widen(manifestUrl), &ferr);
    Ok(legacyVersion == L"9.9.9", "an older launcher's FetchVersion still reads the signed manifest", Narrow(ferr));
    if (!c.ok) return;

    // download + verify
    Downloaded d;
    Manifest m = c.manifest;
    const std::wstring dir = Widen(g_work + "\\dl-" + file);
    MakeDir(dir);
    // a stale leftover that the download must clear away (named like a pre-rename launcher's download)
    HANDLE stale = CreateFileW((dir + L"\\GAGE-MP-Setup-0.0.1.exe").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (stale != INVALID_HANDLE_VALUE) CloseHandle(stale);
    d.ok = Download(m, dir, [&](double f) { d.fractions.push_back(f); }, [&] { ++d.verifyingCalls; }, d.inst, &d.why);
    Ok(d.ok && d.inst.Open(), "download + size + sha256", d.why);
    Ok(!d.fractions.empty() && d.fractions.back() == 1.0, "progress ends at 1.0", d.fractions.empty() ? "none" : std::to_string(d.fractions.back()));
    bool mono = true;
    for (size_t i = 1; i < d.fractions.size(); ++i) if (d.fractions[i] < d.fractions[i - 1]) mono = false;
    Ok(mono && d.verifyingCalls == 1, "progress never goes backwards; 'verifying' fires once", std::to_string(d.verifyingCalls));
    Ok(d.inst.path == dir + L"\\SolarRP-Setup-9.9.9.exe", "final name is SolarRP-Setup-<version>.exe", Narrow(d.inst.path));
    Ok(!Exists(dir + L"\\SolarRP-Setup-9.9.9.exe.part"), ".part is gone after the rename");
    Ok(!Exists(dir + L"\\GAGE-MP-Setup-0.0.1.exe"), "old files in the updates folder were cleaned (a pre-rename GAGE-MP-Setup file too)");
    std::string onDisk;
    Ok(ReadAll(d.inst.path, onDisk) && onDisk == installer, "the file on disk is the installer, byte for byte");
    HANDLE w = CreateFileW(d.inst.path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Ok(w == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION, "the downloaded file is write-protected while the handle is held");
    if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
    d.inst.Close();
}

void TestDownloadFailures() {
    std::printf("[download failures]\n");
    {
        Downloaded d = RunDownload("tamper.json", "tamper");
        Ok(!d.ok && Has(d.why, "sha256 mismatch") && !d.inst.Open(), "same size, different bytes: sha256 mismatch", d.why);
        Ok(DirHasNoInstallerFiles("tamper"), "... and the file is deleted");
    }
    {
        Downloaded d = RunDownload("short.json", "short");
        Ok(!d.ok && !d.inst.Open() && (Has(d.why, "truncated") || Has(d.why, "size mismatch")), "server sends fewer bytes than signed", d.why);
        Ok(DirHasNoInstallerFiles("short"), "... and nothing is kept");
    }
    {
        Downloaded d = RunDownload("long.json", "long");
        Ok(!d.ok && Has(d.why, "response too large") && !d.inst.Open(), "Content-Length above the signed size: refused up front", d.why);
        Ok(DirHasNoInstallerFiles("long"), "... and nothing is kept");
    }
    {
        Downloaded d = RunDownload("chunklong.json", "chunklong");
        Ok(!d.ok && Has(d.why, "response too large") && !d.inst.Open(), "chunked stream that outgrows the signed size: cut off at the cap", d.why);
        Ok(DirHasNoInstallerFiles("chunklong"), "... and nothing is kept");
    }
    {
        Downloaded d = RunDownload("missing.json", "missing");
        Ok(!d.ok && Has(d.why, "HTTP 404") && !d.inst.Open(), "404", d.why);
        Ok(DirHasNoInstallerFiles("missing"), "... and nothing is kept");
    }
    {
        Downloaded d = RunDownload("dead.json", "dead");
        Ok(!d.ok && Has(d.why, "download failed") && !d.inst.Open(), "server not listening", d.why);
    }
    // The cap itself, without the manifest: a body of N+1 bytes against a cap of N.
    {
        const std::wstring part = Widen(g_work + "\\cap.part");
        std::string why;
        const bool ok = DownloadToFile(g_base + "/good/SolarRP-Setup-9.9.9.exe", 1000, part, nullptr, &why);
        Ok(!ok && Has(why, "response too large (limit 1000 B)") && !Exists(part), "hard cap of 1000 bytes on a 300 KiB file", why);
        std::string installer;
        ReadAll(Widen(g_fixtures + "\\installer.bin"), installer);
        const bool exact = DownloadToFile(g_base + "/good/SolarRP-Setup-9.9.9.exe", installer.size(), part, nullptr, &why);
        std::string got;
        Ok(exact && ReadAll(part, got) && got == installer, "a cap equal to the size lets the whole file through", why);
        const bool onePast = DownloadToFile(g_base + "/good/SolarRP-Setup-9.9.9.exe", installer.size() - 1, part, nullptr, &why);
        Ok(!onePast && Has(why, "response too large"), "a cap one byte below the size refuses it", why);
        DeleteFileW(part.c_str());
    }
    // Fallback through real HTTP: dead stored URL, live baked URL.
    {
        const CheckResult c = updater::Check({ g_base + "/nothing-here/version.json", g_base + "/good/version.json" }, g_pub1, g_client, DefaultFetch());
        Ok(c.ok && c.available && c.usedUrl == g_base + "/good/version.json", "HTTP 404 on the first URL: falls back to the second", c.error);
        const CheckResult bad = updater::Check({ g_base + "/tamper/version.json.sig-broken" }, g_pub1, g_client, DefaultFetch());
        Ok(!bad.ok && Has(bad.error, "HTTP 404"), "HTTP error is reported", bad.error);
    }
}

void TestRealManifest() {
    std::printf("[real signing key]\n");
    if (g_realManifest.empty()) {
        std::printf("  SKIPPED: no --real-manifest given (the driver passes %%TEMP%%\\signt\\version.json when it exists)\n");
        return;
    }
    const std::string baked = GAGE_UPDATE_PUBKEY;
    std::string body;
    Ok(ReadAll(Widen(g_realManifest), body), "real-key manifest is readable");
    Manifest m;
    std::string why;
    Ok(VerifyManifest(body, baked, m, &why), "signed with the real key: verifies under the key baked into this build", why);
    Ok(m.version == "0.3.4", "version from the signed payload", m.version);
    Ok(!IsNewer(m.version, kPure, &why), "0.3.4 against client 0.3.4: nothing to install", why);
    Ok(!VerifyManifest(body, g_pub1, m, &why) && Has(why, "signature invalid"), "... and refused under a throwaway key", why);
}

}  // namespace

int main() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 2;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&](std::string& dst) { if (i + 1 < argc) dst = Narrow(argv[++i]); };
        if (a == L"--fixtures") next(g_fixtures);
        else if (a == L"--work") next(g_work);
        else if (a == L"--base") next(g_base);
        else if (a == L"--serve-base") next(g_serveBase);
        else if (a == L"--real-manifest") next(g_realManifest);
        else if (a == L"--client") next(g_client);
        else { std::fprintf(stderr, "unknown argument %s\n", Narrow(a).c_str()); LocalFree(argv); return 2; }
    }
    LocalFree(argv);
    if (g_fixtures.empty() || g_work.empty() || g_base.empty() || g_serveBase.empty() || g_client.empty()) {
        std::fprintf(stderr, "usage: update_check_test --fixtures <dir> --work <dir> --base <url> --serve-base <url> --client <x.y.z> [--real-manifest <file>]\n");
        return 2;
    }
    g_pub1 = Trim(Fx("pub1.txt"));
    g_pub2 = Trim(Fx("pub2.txt"));
    if (g_pub1.size() != 88 || g_pub2.size() != 88) { std::fprintf(stderr, "pub1.txt/pub2.txt are missing or malformed\n"); return 2; }

    TestBase64();
    TestSha256();
    TestVersions();
    TestUrls();
    TestSignatures();
    TestCheck();
    TestInstallerFile();
    TestEndToEnd(g_base + "/good/version.json", "test server", "good");
    TestEndToEnd(g_serveBase + "/version.json", "tools/update-server/serve.mjs", "serve");
    TestDownloadFailures();
    TestRealManifest();

    if (g_checks < kMinChecks) {
        std::printf("FAIL: only %d checks ran (expected at least %d)\n", g_checks, kMinChecks);
        ++g_failed;
    }
    std::printf("CHECKS %d FAILED %d\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
