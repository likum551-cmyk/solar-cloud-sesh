// updater.cpp — see updater.h and docs/UPDATES.md.

#include "updater.h"
#include "common/json.h"
#include "common/logger.h"
#include "net/serverfinder.h"

#include <bcrypt.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "bcrypt")

#ifndef STATUS_INVALID_SIGNATURE
#define STATUS_INVALID_SIGNATURE ((NTSTATUS)0xC000A000L)
#endif

namespace gage::updater {

namespace {

    void SetWhy(std::string* why, const std::string& text) {
        if (why) *why = text;
    }

    std::string ToHex(const unsigned char* d, size_t n) {
        static const char hex[] = "0123456789abcdef";
        std::string s;
        s.reserve(n * 2);
        for (size_t i = 0; i < n; ++i) {
            s += hex[d[i] >> 4];
            s += hex[d[i] & 15];
        }
        return s;
    }

    // Incremental SHA-256 (CNG).
    class Sha256 {
    public:
        Sha256() {
            ok_ = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
                  BCRYPT_SUCCESS(BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0));
        }
        ~Sha256() {
            if (hash_) BCryptDestroyHash(hash_);
            if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
        }
        Sha256(const Sha256&) = delete;
        Sha256& operator=(const Sha256&) = delete;
        void Update(const void* data, size_t len) {
            if (!ok_) return;
            const unsigned char* p = (const unsigned char*)data;
            while (len > 0) {
                ULONG n = (ULONG)std::min<size_t>(len, 1u << 30);
                if (!BCRYPT_SUCCESS(BCryptHashData(hash_, (PUCHAR)p, n, 0))) { ok_ = false; return; }
                p += n;
                len -= n;
            }
        }
        // 32 raw bytes; false if any step failed.
        bool Finish(unsigned char out[32]) {
            return ok_ && BCRYPT_SUCCESS(BCryptFinishHash(hash_, out, 32, 0));
        }
    private:
        BCRYPT_ALG_HANDLE alg_ = nullptr;
        BCRYPT_HASH_HANDLE hash_ = nullptr;
        bool ok_ = false;
    };

    const JVal* Member(const JVal& o, const char* key) {
        if (o.t != JVal::Obj) return nullptr;
        for (const auto& kv : o.o)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }

    bool IsLowerHex64(const std::string& s) {
        if (s.size() != 64) return false;
        for (char c : s)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        return true;
    }

    std::string LowerAscii(std::string s) {
        for (char& c : s)
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return s;
    }

    std::wstring ParentDir(const std::wstring& path) {
        size_t i = path.find_last_of(L"\\/");
        return i == std::wstring::npos ? L"." : path.substr(0, i);
    }

    // A writable folder: created if needed, proven by creating a self-deleting file in it.
    bool UsableDir(const std::wstring& d) {
        if (!CreateDirectoryW(d.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        const DWORD attr = GetFileAttributesW(d.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return false;
        HANDLE h = CreateFileW((d + L"\\.gage_write_test").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        CloseHandle(h);
        return true;
    }

}  // namespace

// ---------------------------------------------------------------------------
// primitives
// ---------------------------------------------------------------------------

bool Base64Decode(const std::string& text, std::vector<unsigned char>& out) {
    out.clear();
    if (text.empty() || text.size() % 4 != 0) return false;
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    const size_t n = text.size();
    out.reserve(n / 4 * 3);
    for (size_t i = 0; i < n; i += 4) {
        const bool lastGroup = i + 4 == n;
        int v[4] = { 0, 0, 0, 0 };
        int real = 4;  // data characters in this group (the rest is '=' padding)
        for (int k = 0; k < 4; ++k) {
            const char c = text[i + k];
            if (c == '=') {
                if (!lastGroup || k < 2) { out.clear(); return false; }  // padding only at the very end
                if (real == 4) real = k;
            } else {
                if (real != 4) { out.clear(); return false; }          // data after padding
                v[k] = val(c);
                if (v[k] < 0) { out.clear(); return false; }
            }
        }
        // Canonical encoding only: the unused low bits of the last data character are zero.
        if ((real == 2 && (v[1] & 0x0F) != 0) || (real == 3 && (v[2] & 0x03) != 0)) { out.clear(); return false; }
        const unsigned triple = ((unsigned)v[0] << 18) | ((unsigned)v[1] << 12) | ((unsigned)v[2] << 6) | (unsigned)v[3];
        out.push_back((unsigned char)(triple >> 16));
        if (real > 2) out.push_back((unsigned char)((triple >> 8) & 0xFF));
        if (real > 3) out.push_back((unsigned char)(triple & 0xFF));
    }
    return true;
}

std::string Sha256Hex(const void* data, size_t len) {
    Sha256 h;
    h.Update(data, len);
    unsigned char d[32];
    if (!h.Finish(d)) return "";
    return ToHex(d, 32);
}

bool ParseVersion(const std::string& text, unsigned long out[3]) {
    size_t i = 0;
    for (int part = 0; part < 3; ++part) {
        const size_t start = i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
        const size_t len = i - start;
        if (len == 0 || len > 9) return false;
        out[part] = strtoul(text.substr(start, len).c_str(), nullptr, 10);
        if (part < 2) {
            if (i >= text.size() || text[i] != '.') return false;
            ++i;
        }
    }
    return i == text.size();
}

int CompareVersions(const unsigned long a[3], const unsigned long b[3]) {
    for (int i = 0; i < 3; ++i) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

bool IsNewer(const std::string& candidate, const std::string& current, std::string* why) {
    unsigned long c[3], k[3];
    if (!ParseVersion(candidate, c)) { SetWhy(why, "malformed version '" + candidate.substr(0, 40) + "'"); return false; }
    if (!ParseVersion(current, k)) { SetWhy(why, "client version '" + current.substr(0, 40) + "' is malformed"); return false; }
    if (CompareVersions(c, k) <= 0) {
        SetWhy(why, "not newer (signed " + candidate + ", client " + current + ")");
        return false;
    }
    return true;
}

bool ValidHttpUrl(const std::string& url, size_t maxChars) {
    if (url.empty() || url.size() > maxChars) return false;
    for (unsigned char c : url)
        if (c <= 0x20 || c >= 0x7F || c == '\\' || c == '"' || c == '<' || c == '>') return false;
    const std::string low = LowerAscii(url.substr(0, 8));
    size_t schemeLen = 0;
    if (low.compare(0, 7, "http://") == 0) schemeLen = 7;
    else if (low.compare(0, 8, "https://") == 0) schemeLen = 8;
    else return false;
    if (url.find('#') != std::string::npos) return false;
    const size_t hostEnd = url.find_first_of("/?", schemeLen);
    const std::string authority = url.substr(schemeLen, hostEnd == std::string::npos ? std::string::npos : hostEnd - schemeLen);
    if (authority.empty() || authority.find('@') != std::string::npos) return false;
    std::string host = authority;
    std::string port;
    if (authority[0] == '[') {                       // [v6]:port
        const size_t close = authority.find(']');
        if (close == std::string::npos || close < 2) return false;
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') return false;
            port = authority.substr(close + 2);
        }
        for (char c : host)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == ':' || c == '.')) return false;
    } else {
        const size_t colon = authority.find(':');
        if (colon != std::string::npos) {
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
            if (port.empty()) return false;
        }
        if (host.empty()) return false;
        for (char c : host)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_')) return false;
    }
    if (!port.empty()) {
        if (port.size() > 5) return false;
        for (char c : port)
            if (c < '0' || c > '9') return false;
        const int p = atoi(port.c_str());
        if (p < 1 || p > 65535) return false;
    }
    return true;
}

bool VerifySignature(const std::string& pubKeyB64, const std::vector<unsigned char>& message,
                     const std::vector<unsigned char>& sig, std::string* why) {
    std::vector<unsigned char> pub;
    if (!Base64Decode(pubKeyB64, pub) || pub.size() != 64) { SetWhy(why, "bad public key"); return false; }
    if (sig.size() != 64) { SetWhy(why, "bad signature length " + std::to_string(sig.size())); return false; }

    // BCRYPT_ECCKEY_BLOB { magic, cbKey } followed by X and Y.
    std::vector<unsigned char> blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
    BCRYPT_ECCKEY_BLOB head = {};
    head.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    head.cbKey = 32;
    memcpy(blob.data(), &head, sizeof(head));
    memcpy(blob.data() + sizeof(head), pub.data(), 64);

    Sha256 h;
    h.Update(message.data(), message.size());
    unsigned char digest[32];
    if (!h.Finish(digest)) { SetWhy(why, "crypto provider failed (sha256)"); return false; }

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = false;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0))) {
        SetWhy(why, "crypto provider failed (ecdsa)");
    } else if (!BCRYPT_SUCCESS(BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob.data(),
                                                   (ULONG)blob.size(), 0))) {
        SetWhy(why, "bad public key");  // not a point on P-256
    } else {
        const NTSTATUS st = BCryptVerifySignature(key, nullptr, digest, sizeof(digest), (PUCHAR)sig.data(),
                                                  (ULONG)sig.size(), 0);
        if (BCRYPT_SUCCESS(st)) ok = true;
        else if (st == STATUS_INVALID_SIGNATURE) SetWhy(why, "signature invalid");
        else SetWhy(why, "signature check failed (status " + std::to_string((unsigned long)st) + ")");
    }
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// ---------------------------------------------------------------------------
// manifest
// ---------------------------------------------------------------------------

bool VerifyManifest(const std::string& bodyIn, const std::string& pubKeyB64, Manifest& out, std::string* why) {
    out = Manifest();
    if (bodyIn.empty()) { SetWhy(why, "manifest is empty"); return false; }
    if (bodyIn.size() > kMaxManifestBytes) { SetWhy(why, "manifest too large"); return false; }
    std::string body = bodyIn;
    if (body.size() >= 3 && (unsigned char)body[0] == 0xEF && (unsigned char)body[1] == 0xBB && (unsigned char)body[2] == 0xBF)
        body.erase(0, 3);

    // 1. The envelope: only "signed" and "sig" are read from it.
    JParser env;
    env.maxNodes = 256;
    if (!env.parse(body.data(), body.size()) || env.v.t != JVal::Obj) { SetWhy(why, "manifest is not a JSON object"); return false; }
    const JVal* signedV = Member(env.v, "signed");
    const JVal* sigV = Member(env.v, "sig");
    if (!signedV || signedV->t != JVal::Str || !sigV || sigV->t != JVal::Str) {
        SetWhy(why, "manifest has no signed payload (unsigned or old format)");
        return false;
    }
    std::vector<unsigned char> payloadBytes, sig;
    if (!Base64Decode(signedV->s, payloadBytes) || payloadBytes.empty()) { SetWhy(why, "signed payload is not valid base64"); return false; }
    if (!Base64Decode(sigV->s, sig)) { SetWhy(why, "signature is not valid base64"); return false; }

    // 2. The signature, over the decoded bytes exactly as published.
    if (!VerifySignature(pubKeyB64, payloadBytes, sig, why)) return false;

    // 3. Only now the payload is read.
    JParser pj;
    pj.maxNodes = 256;
    if (!pj.parse((const char*)payloadBytes.data(), payloadBytes.size()) || pj.v.t != JVal::Obj) {
        SetWhy(why, "signed payload is not a JSON object");
        return false;
    }
    const JVal& p = pj.v;
    const JVal* v = Member(p, "v");
    if (!v || v->t != JVal::Num || v->n != 1.0) { SetWhy(why, "unsupported payload format (v != 1)"); return false; }

    Manifest m;
    const JVal* ver = Member(p, "version");
    unsigned long parts[3];
    if (!ver || ver->t != JVal::Str || !ParseVersion(ver->s, parts)) { SetWhy(why, "malformed version in signed payload"); return false; }
    m.version = ver->s;

    const JVal* url = Member(p, "url");
    if (!url || url->t != JVal::Str || !ValidHttpUrl(url->s, kMaxUrlChars)) { SetWhy(why, "bad installer url in signed payload"); return false; }
    m.url = url->s;

    const JVal* sha = Member(p, "sha256");
    if (!sha || sha->t != JVal::Str || !IsLowerHex64(LowerAscii(sha->s))) { SetWhy(why, "bad sha256 in signed payload"); return false; }
    m.sha256 = LowerAscii(sha->s);

    const JVal* size = Member(p, "size");
    if (!size || size->t != JVal::Num || !(size->n >= 1.0) || size->n != std::floor(size->n) ||
        size->n > (double)kMaxInstallerBytes) {
        SetWhy(why, "bad size in signed payload (1.." + std::to_string(kMaxInstallerBytes) + " bytes)");
        return false;
    }
    m.size = (unsigned long long)size->n;

    if (const JVal* notes = Member(p, "notes")) {
        if (notes->t != JVal::Str || notes->s.size() > kMaxNotesChars) { SetWhy(why, "bad notes in signed payload"); return false; }
        m.notes = notes->s;
    }
    if (const JVal* mand = Member(p, "mandatory")) {
        if (mand->t != JVal::Bool) { SetWhy(why, "bad mandatory flag in signed payload"); return false; }
        m.mandatory = mand->b;
    }
    if (const JVal* uu = Member(p, "updateUrl")) {
        if (uu->t != JVal::Str) { SetWhy(why, "bad updateUrl in signed payload"); return false; }
        if (!uu->s.empty()) {
            if (ValidHttpUrl(uu->s, kMaxUpdateUrlChars)) m.updateUrl = uu->s;
            else GLOGW("update: ignoring an invalid updateUrl in the signed payload (not http(s), over %zu characters, or malformed)", kMaxUpdateUrlChars);
        }
    }
    if (const JVal* iss = Member(p, "issued"); iss && iss->t == JVal::Str && iss->s.size() <= 64) m.issued = iss->s;

    out = std::move(m);
    return true;
}

FetchFn DefaultFetch() {
    return [](const std::string& url, std::string& body, std::string& err) {
        body.clear();
        net::HttpGetOptions opts;
        opts.maxBytes = kMaxManifestBytes;
        opts.overallMs = 15000;
        std::wstring e;
        const bool ok = net::HttpGet(Widen(url), opts,
            [&body](const char* d, size_t n) { body.append(d, n); return true; }, nullptr, &e);
        if (!ok) err = Narrow(e);
        return ok;
    };
}

CheckResult Check(const std::vector<std::string>& urls, const std::string& pubKeyB64,
                  const std::string& clientVersion, const FetchFn& fetch) {
    CheckResult r;
    unsigned long client[3];
    if (!ParseVersion(clientVersion, client)) {
        r.error = "client version '" + clientVersion.substr(0, 40) + "' is malformed";
        GLOGE("update: %s", r.error.c_str());
        return r;
    }
    r.error = "no update URL configured";
    std::vector<std::string> tried;
    for (const std::string& url : urls) {
        if (url.empty() || std::find(tried.begin(), tried.end(), url) != tried.end()) continue;
        tried.push_back(url);
        std::string body, err;
        if (!fetch(url, body, err)) {
            r.error = "could not reach the update server: " + err;
            GLOGW("update: %s (%s)", r.error.c_str(), url.c_str());
            continue;
        }
        Manifest m;
        std::string why;
        if (!VerifyManifest(body, pubKeyB64, m, &why)) {
            r.error = why;
            GLOGW("update: %s (%s)", why.c_str(), url.c_str());
            continue;
        }
        r.ok = true;
        r.error.clear();
        r.manifest = m;
        r.usedUrl = url;
        std::string newerWhy;
        r.available = IsNewer(m.version, clientVersion, &newerWhy);
        if (r.available) GLOGI("update: signed manifest ok: %s is newer than %s (%s)", m.version.c_str(), clientVersion.c_str(), url.c_str());
        else GLOGI("update: signed manifest ok, nothing to install: %s (%s)", newerWhy.c_str(), url.c_str());
        // Move clients to a new manifest URL, but never on the word of an old manifest: only a
        // payload at least as new as the client may change where it looks.
        unsigned long signedV[3];
        if (!m.updateUrl.empty() && ParseVersion(m.version, signedV) && CompareVersions(signedV, client) >= 0)
            r.migrateUrl = m.updateUrl;
        return r;
    }
    return r;
}

// ---------------------------------------------------------------------------
// download
// ---------------------------------------------------------------------------

// %LOCALAPPDATA%\GAGE-MP (no trailing backslash), "" when the variable is missing or too long.
static std::wstring LocalAppDataRoot() {
    wchar_t la[MAX_PATH] = L"";
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    return std::wstring(la) + L"\\GAGE-MP";
}

std::wstring ResolveUpdatesDir(const std::wstring& exeDir, std::string* why) {
    const std::wstring primary = exeDir + L"updates";
    if (UsableDir(primary)) return primary;
    const std::wstring root = LocalAppDataRoot();
    if (!root.empty()) {
        CreateDirectoryW(root.c_str(), nullptr);
        const std::wstring fallback = root + L"\\updates";
        if (UsableDir(fallback)) {
            GLOGW("update: %s is not writable; downloading to %s", Narrow(primary).c_str(), Narrow(fallback).c_str());
            return fallback;
        }
    }
    SetWhy(why, "no writable folder for the update (" + Narrow(primary) + ")");
    return L"";
}

int CleanUpdatesDir(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int removed = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (_wcsicmp(fd.cFileName, InstallerLogName()) == 0) continue;
        if (DeleteFileW((dir + L"\\" + fd.cFileName).c_str())) ++removed;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return removed;
}

int CleanUpdatesDirs(const std::wstring& exeDir) {
    int removed = CleanUpdatesDir(exeDir + L"updates");
    const std::wstring root = LocalAppDataRoot();
    if (!root.empty()) removed += CleanUpdatesDir(root + L"\\updates");
    return removed;
}

bool DownloadToFile(const std::string& url, unsigned long long maxBytes, const std::wstring& partPath,
                    const std::function<void(double)>& progress, std::string* why) {
    HANDLE f = CreateFileW(partPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        SetWhy(why, "download failed: cannot create " + Narrow(partPath) + " (" + Narrow(FormatError(GetLastError())) + ")");
        return false;
    }
    DWORD writeErr = 0;
    net::HttpGetOptions opts;
    opts.maxBytes = maxBytes;
    opts.overallMs = 30ull * 60ull * 1000ull;
    std::wstring err;
    const bool ok = net::HttpGet(Widen(url), opts,
        [&](const char* d, size_t n) {
            DWORD wrote = 0;
            if (!WriteFile(f, d, (DWORD)n, &wrote, nullptr) || wrote != n) {
                writeErr = GetLastError();
                if (writeErr == 0) writeErr = ERROR_WRITE_FAULT;
                return false;
            }
            return true;
        },
        [&](unsigned long long got, unsigned long long) {
            if (progress && maxBytes > 0) progress(std::min(1.0, (double)got / (double)maxBytes));
        },
        &err);
    const bool closed = CloseHandle(f) != 0;
    if (!ok || !closed) {
        DeleteFileW(partPath.c_str());
        if (writeErr) SetWhy(why, "download failed: cannot write the file (" + Narrow(FormatError(writeErr)) + ")");
        else if (!ok) SetWhy(why, "download failed: " + Narrow(err));
        else SetWhy(why, "download failed: cannot close the file");
        return false;
    }
    return true;
}

bool VerifyInstallerFile(const std::wstring& path, unsigned long long size, const std::string& sha256Hex,
                         Installer& out, std::string* why) {
    out.Close();
    out.path.clear();
    // Deny write and delete to everyone else from this moment until `out` is closed.
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        SetWhy(why, "cannot open the downloaded installer exclusively (" + Narrow(FormatError(GetLastError())) + ")");
        return false;
    }
    auto fail = [&](const std::string& text) {
        CloseHandle(h);
        SetWhy(why, text);
        return false;
    };
    if (GetFileType(h) != FILE_TYPE_DISK) return fail("downloaded installer is not a regular file");
    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(h, &sz)) return fail("cannot read the downloaded installer's size");
    if ((unsigned long long)sz.QuadPart != size)
        return fail("size mismatch (got " + std::to_string((unsigned long long)sz.QuadPart) + " bytes, signed " + std::to_string(size) + ")");

    Sha256 hash;
    std::vector<unsigned char> buf(256 * 1024);
    unsigned long long total = 0;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr)) return fail("cannot read the downloaded installer");
        if (got == 0) break;
        hash.Update(buf.data(), got);
        total += got;
    }
    unsigned char digest[32];
    if (total != size) return fail("size mismatch (read " + std::to_string(total) + " bytes, signed " + std::to_string(size) + ")");
    if (!hash.Finish(digest)) return fail("crypto provider failed (sha256)");
    const std::string got = ToHex(digest, 32);
    if (got != LowerAscii(sha256Hex)) return fail("sha256 mismatch (got " + got + ", signed " + LowerAscii(sha256Hex) + ")");

    out.file = h;
    out.path = path;
    return true;
}

bool Download(const Manifest& m, const std::wstring& dir, const std::function<void(double)>& progress,
              const std::function<void()>& verifying, Installer& out, std::string* why) {
    out.Close();
    const std::wstring base = dir + L"\\SolarRP-Setup-" + Widen(m.version) + L".exe";
    const std::wstring part = base + L".part";
    const int cleaned = CleanUpdatesDir(dir);
    if (cleaned > 0) GLOGI("update: removed %d old file(s) from %s", cleaned, Narrow(dir).c_str());

    if (!DownloadToFile(m.url, m.size, part, progress, why)) return false;
    if (verifying) verifying();
    if (!MoveFileExW(part.c_str(), base.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        SetWhy(why, "download failed: cannot rename the finished download (" + Narrow(FormatError(GetLastError())) + ")");
        DeleteFileW(part.c_str());
        return false;
    }
    if (!VerifyInstallerFile(base, m.size, m.sha256, out, why)) {
        DeleteFileW(base.c_str());  // a mismatch is never kept; a file still in use simply stays
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// install
// ---------------------------------------------------------------------------

std::wstring InstallerCommandLine(const std::wstring& setupPath, const std::wstring& logPath) {
    return L"\"" + setupPath + L"\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /NOCLOSEAPPLICATIONS /GAGEUPDATE /LOG=\"" +
           logPath + L"\"";
}

bool StartInstaller(const Installer& inst, const std::wstring& logPath, unsigned long* pid, std::string* why) {
    if (!inst.Open()) { SetWhy(why, "no verified installer to start"); return false; }
    const std::wstring cmd = InstallerCommandLine(inst.path, logPath);
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(L'\0');
    const std::wstring cwd = ParentDir(inst.path);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(inst.path.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd.c_str(), &si, &pi)) {
        SetWhy(why, "could not start the installer (" + Narrow(FormatError(GetLastError())) + ")");
        return false;
    }
    if (pid) *pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace gage::updater
