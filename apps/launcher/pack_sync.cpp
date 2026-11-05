// pack_sync.cpp: see pack_sync.h and docs/UPDATES.md "Car packs".

#include "pack_sync.h"
#include "updater.h"
#include "common/json.h"
#include "common/logger.h"
#include "net/serverfinder.h"

#include <bcrypt.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#pragma comment(lib, "bcrypt")

namespace gage::packsync {

namespace {

    void SetWhy(std::string* why, const std::string& text) {
        if (why) *why = text;
    }

    std::string Lower(std::string s) {
        for (char& c : s)
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return s;
    }

    bool IsLowerHex64(const std::string& s) {
        if (s.size() != 64) return false;
        for (char c : s)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        return true;
    }

    const JVal* Member(const JVal& o, const char* key) {
        if (o.t != JVal::Obj) return nullptr;
        for (const auto& kv : o.o)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }

    // CON, PRN, AUX, NUL, COM0-9, LPT0-9, with or without an extension.
    bool IsDeviceName(const std::string& seg) {
        std::string stem = Lower(seg.substr(0, seg.find('.')));
        static const char* fixed[] = { "con", "prn", "aux", "nul", "conin$", "conout$" };
        for (const char* f : fixed)
            if (stem == f) return true;
        return stem.size() == 4 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) &&
               stem[3] >= '0' && stem[3] <= '9';
    }

    bool ValidSegment(const std::string& seg, size_t maxChars) {
        if (seg.empty() || seg.size() > maxChars || seg[0] == '.' || seg.back() == '.') return false;
        for (char c : seg)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
                return false;
        return !IsDeviceName(seg);
    }

    std::wstring ToBackslash(const std::string& rel) {
        std::wstring w = Widen(rel);
        for (wchar_t& c : w)
            if (c == L'/') c = L'\\';
        return w;
    }

    // Size and last-write time of a regular file (read from the file record, so hard links agree).
    bool StatFile(const std::wstring& path, unsigned long long* size, unsigned long long* mtime) {
        WIN32_FILE_ATTRIBUTE_DATA d;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return false;
        if (d.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        *size = ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
        *mtime = ((unsigned long long)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
        return true;
    }

    bool IsDir(const std::wstring& p) {
        const DWORD a = GetFileAttributesW(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT);
    }

    bool MakeDirs(const std::wstring& dir) {
        if (dir.empty() || IsDir(dir)) return true;
        const size_t i = dir.find_last_of(L'\\');
        if (i != std::wstring::npos && i > 2 && !MakeDirs(dir.substr(0, i))) return false;
        return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
    }

    // Every regular file under `dir`, as lowercase '/' relative path -> real relative path. Names starting with '.'
    // and reparse points are skipped (never followed).
    void ListFiles(const std::wstring& dir, const std::string& prefix, std::map<std::string, std::string>& out, int depth = 0) {
        if (depth > 16) return;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            const std::string name = Narrow(fd.cFileName);
            if (name.empty() || name[0] == '.') continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            const std::string rel = prefix.empty() ? name : prefix + "/" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ListFiles(dir + L"\\" + fd.cFileName, rel, out, depth + 1);
            else out[Lower(rel)] = rel;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // Delete a tree, but only one under `guard` (root\.sync\): never anything else, never through a reparse point.
    void DeleteTree(const std::wstring& path, const std::wstring& guard) {
        if (path.size() <= guard.size() || _wcsnicmp(path.c_str(), guard.c_str(), guard.size()) != 0) {
            GLOGE("packs: refusing to delete %s (outside %s)", Narrow(path).c_str(), Narrow(guard).c_str());
            return;
        }
        const DWORD a = GetFileAttributesW(path.c_str());
        if (a == INVALID_FILE_ATTRIBUTES) return;
        if (!(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
            if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
            if (a & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(path.c_str());  // a junction: remove the link only
            else DeleteFileW(path.c_str());
            return;
        }
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                DeleteTree(path + L"\\" + fd.cFileName, guard);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(path.c_str());
    }

    // ---- the hash cache and the managed list: root\.sync\state.txt -----------------------------------------
    // "managed <name>" and "file <sha> <size> <mtime> <lowercase name/path>" lines.
    struct CacheEntry { unsigned long long size = 0, mtime = 0; std::string sha; };
    struct State {
        std::set<std::string> managed;                 // lowercase pack names installed by Sync
        std::map<std::string, CacheEntry> cache;       // lowercase "name/path"
    };

    State LoadState(const std::wstring& file) {
        State s;
        FILE* f = _wfopen(file.c_str(), L"rb");
        if (!f) return s;
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            std::string l(line);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            if (l.compare(0, 8, "managed ") == 0) {
                const std::string n = l.substr(8);
                if (ValidPackName(n)) s.managed.insert(Lower(n));
            } else if (l.compare(0, 5, "file ") == 0) {
                char sha[80] = {}, key[600] = {};
                unsigned long long size = 0, mtime = 0;
                if (sscanf(l.c_str() + 5, "%79s %llu %llu %599s", sha, &size, &mtime, key) == 4 && IsLowerHex64(sha))
                    s.cache[key] = { size, mtime, sha };
            }
        }
        fclose(f);
        return s;
    }

    bool SaveState(const std::wstring& file, const State& s) {
        const std::wstring tmp = file + L".tmp";
        FILE* f = _wfopen(tmp.c_str(), L"wb");
        if (!f) return false;
        for (const auto& n : s.managed) fprintf(f, "managed %s\n", n.c_str());
        for (const auto& kv : s.cache)
            fprintf(f, "file %s %llu %llu %s\n", kv.second.sha.c_str(), kv.second.size, kv.second.mtime, kv.first.c_str());
        const bool ok = fflush(f) == 0 && fclose(f) == 0;
        if (!ok) { DeleteFileW(tmp.c_str()); return false; }
        return MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    }

    // The SHA-256 of a file on disk, through the cache when size and mtime are unchanged.
    std::string CachedHash(State& st, const std::string& key, const std::wstring& path, unsigned long long expectSize) {
        unsigned long long size = 0, mtime = 0;
        if (!StatFile(path, &size, &mtime) || size != expectSize) return "";
        auto it = st.cache.find(key);
        if (it != st.cache.end() && it->second.size == size && it->second.mtime == mtime) return it->second.sha;
        unsigned long long got = 0;
        const std::string sha = HashFile(path, &got);
        if (sha.empty() || got != size) return "";
        st.cache[key] = { size, mtime, sha };
        return sha;
    }

    void Remember(State& st, const std::string& key, const std::wstring& path, const std::string& sha) {
        unsigned long long size = 0, mtime = 0;
        if (StatFile(path, &size, &mtime)) st.cache[key] = { size, mtime, sha };
    }

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// names and the manifest
// ---------------------------------------------------------------------------------------------------------------

bool ValidPackName(const std::string& name) {
    return ValidSegment(name, 64);
}

bool ValidRelPath(const std::string& path) {
    if (path.empty() || path.size() > kMaxPathChars) return false;
    size_t start = 0;
    int segs = 0;
    for (;;) {
        const size_t slash = path.find('/', start);
        const std::string seg = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!ValidSegment(seg, 100) || ++segs > 8) return false;
        if (slash == std::string::npos) return true;
        start = slash + 1;
    }
}

std::string HashFile(const std::wstring& path, unsigned long long* size) {
    if (size) *size = 0;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0));
    std::vector<unsigned char> buf(1u << 20);
    unsigned long long total = 0;
    while (ok) {
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr)) { ok = false; break; }
        if (got == 0) break;
        ok = BCRYPT_SUCCESS(BCryptHashData(hash, buf.data(), got, 0));
        total += got;
    }
    unsigned char d[32];
    ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, d, 32, 0));
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(h);
    if (!ok) return "";
    static const char hex[] = "0123456789abcdef";
    std::string s;
    for (unsigned char c : d) { s += hex[c >> 4]; s += hex[c & 15]; }
    if (size) *size = total;
    return s;
}

bool VerifyManifest(const std::string& bodyIn, const std::string& pubKeyB64, Manifest& out, std::string* why) {
    out = Manifest();
    if (bodyIn.empty()) { SetWhy(why, "packs.json is empty"); return false; }
    if (bodyIn.size() > kMaxManifestBytes) { SetWhy(why, "packs.json too large"); return false; }
    std::string body = bodyIn;
    if (body.size() >= 3 && (unsigned char)body[0] == 0xEF && (unsigned char)body[1] == 0xBB && (unsigned char)body[2] == 0xBF)
        body.erase(0, 3);

    JParser env;
    env.maxNodes = 64;
    if (!env.parse(body.data(), body.size()) || env.v.t != JVal::Obj) { SetWhy(why, "packs.json is not a JSON object"); return false; }
    const JVal* signedV = Member(env.v, "signed");
    const JVal* sigV = Member(env.v, "sig");
    if (!signedV || signedV->t != JVal::Str || !sigV || sigV->t != JVal::Str) { SetWhy(why, "packs.json is not signed"); return false; }
    std::vector<unsigned char> payload, sig;
    if (!updater::Base64Decode(signedV->s, payload) || payload.empty()) { SetWhy(why, "signed payload is not valid base64"); return false; }
    if (!updater::Base64Decode(sigV->s, sig)) { SetWhy(why, "signature is not valid base64"); return false; }
    if (!updater::VerifySignature(pubKeyB64, payload, sig, why)) return false;

    JParser pj;
    pj.maxNodes = kMaxFiles * 5 + kMaxPacks * 4 + 16;
    if (!pj.parse((const char*)payload.data(), payload.size()) || pj.v.t != JVal::Obj) { SetWhy(why, "signed payload is not a JSON object"); return false; }
    const JVal& p = pj.v;
    const JVal* v = Member(p, "v");
    const JVal* kind = Member(p, "kind");
    if (!v || v->t != JVal::Num || v->n != 1.0) { SetWhy(why, "unsupported packs format (v != 1)"); return false; }
    if (!kind || kind->t != JVal::Str || kind->s != "packs") { SetWhy(why, "signed payload is not a packs manifest"); return false; }
    const JVal* packs = Member(p, "packs");
    if (!packs || packs->t != JVal::Arr) { SetWhy(why, "signed payload has no packs list"); return false; }
    if (packs->a.size() > kMaxPacks) { SetWhy(why, "too many packs"); return false; }

    Manifest m;
    if (const JVal* iss = Member(p, "issued"); iss && iss->t == JVal::Str && iss->s.size() <= 64) m.issued = iss->s;
    std::set<std::string> names;
    for (const JVal& pv : packs->a) {
        const JVal* name = Member(pv, "name");
        const JVal* files = Member(pv, "files");
        if (!name || name->t != JVal::Str || !ValidPackName(name->s)) { SetWhy(why, "bad pack name"); return false; }
        if (!names.insert(Lower(name->s)).second) { SetWhy(why, "pack '" + name->s + "' is listed twice"); return false; }
        if (!files || files->t != JVal::Arr || files->a.empty()) { SetWhy(why, "pack '" + name->s + "' has no files"); return false; }
        Pack pk;
        pk.name = name->s;
        std::set<std::string> paths;
        bool marker = false;
        for (const JVal& fv : files->a) {
            const JVal* path = Member(fv, "path");
            const JVal* size = Member(fv, "size");
            const JVal* sha = Member(fv, "sha256");
            if (!path || path->t != JVal::Str || !ValidRelPath(path->s)) { SetWhy(why, "bad file path in pack '" + pk.name + "'"); return false; }
            if (!paths.insert(Lower(path->s)).second) { SetWhy(why, "file '" + path->s + "' is listed twice in pack '" + pk.name + "'"); return false; }
            if (!size || size->t != JVal::Num || size->n < 0 || size->n != std::floor(size->n) || size->n > (double)kMaxFileBytes) {
                SetWhy(why, "bad size for '" + path->s + "' in pack '" + pk.name + "'");
                return false;
            }
            if (!sha || sha->t != JVal::Str || !IsLowerHex64(sha->s)) { SetWhy(why, "bad sha256 for '" + path->s + "' in pack '" + pk.name + "'"); return false; }
            if (Lower(path->s) == "dlc.rpf") marker = true;
            File f{ path->s, (unsigned long long)size->n, sha->s };
            pk.bytes += f.size;
            pk.files.push_back(std::move(f));
            if (++m.files > kMaxFiles) { SetWhy(why, "too many files"); return false; }
        }
        if (!marker) { SetWhy(why, "pack '" + pk.name + "' has no dlc.rpf"); return false; }
        m.bytes += pk.bytes;
        if (m.bytes > kMaxTotalBytes) { SetWhy(why, "packs are larger than the limit"); return false; }
        m.packs.push_back(std::move(pk));
    }
    out = std::move(m);
    return true;
}

std::string ManifestUrlFor(const std::string& updateManifestUrl, const std::string& name) {
    std::string u = updateManifestUrl.substr(0, updateManifestUrl.find_first_of("?#"));
    const size_t scheme = u.find("://");
    const size_t slash = u.find_last_of('/');
    if (scheme == std::string::npos || slash == std::string::npos || u.size() <= scheme + 3) return "";
    // "https://host" with no path at all: the manifest sits at the root.
    if (slash == scheme + 2) return u + "/" + name;
    if (slash < scheme + 3) return "";
    return u.substr(0, slash + 1) + name;
}

std::string BlobUrl(const std::string& packsManifestUrl, const std::string& sha256) {
    const size_t slash = packsManifestUrl.find_last_of('/');
    return packsManifestUrl.substr(0, slash + 1) + "packs/" + sha256;
}

std::wstring DefaultRoot(bool gen9) {
    wchar_t la[MAX_PATH] = L"";
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    return std::wstring(la) + (gen9 ? L"\\GAGE-MP\\packs-gen9" : L"\\GAGE-MP\\packs");
}

Io DefaultIo() {
    Io io;
    io.fetch = [](const std::string& url, std::string& body, std::string& err) {
        body.clear();
        net::HttpGetOptions opts;
        opts.maxBytes = kMaxManifestBytes;
        opts.overallMs = 60000;
        std::wstring e;
        const bool ok = net::HttpGet(Widen(url), opts, [&body](const char* d, size_t n) { body.append(d, n); return true; }, nullptr, &e);
        if (!ok) err = Narrow(e);
        return ok;
    };
    io.download = [](const std::string& url, unsigned long long size, const std::wstring& part,
                     const std::function<void(unsigned long long)>& progress, std::string* why) {
        // A zero-byte file is still fetched (the server must have it), with a cap of one byte so it can be told apart.
        return updater::DownloadToFile(url, size ? size : 1, part,
                                       [&](double frac) { if (progress) progress((unsigned long long)(frac * (double)size)); }, why);
    };
    return io;
}

// ---------------------------------------------------------------------------------------------------------------
// Sync
// ---------------------------------------------------------------------------------------------------------------

Result Sync(const std::wstring& root, const std::vector<std::string>& updateUrls, const std::string& pubKeyB64,
            const Io& io, const std::function<void(const Progress&)>& progress, const std::string& manifestName) {
    Result r;
    if (root.empty()) { r.error = "no folder for the packs (LOCALAPPDATA is not set)"; return r; }

    // 1. The first packs.json that verifies.
    Manifest m;
    bool have = false;
    std::vector<std::string> tried;
    for (const std::string& up : updateUrls) {
        const std::string url = ManifestUrlFor(up, manifestName);
        if (url.empty() || std::find(tried.begin(), tried.end(), url) != tried.end()) continue;
        tried.push_back(url);
        std::string body, err;
        if (!io.fetch(url, body, err)) {
            if (r.error.empty()) r.error = "could not fetch " + url + ": " + err;
            GLOGW("packs: could not fetch %s: %s", url.c_str(), err.c_str());
            continue;
        }
        r.published = true;
        std::string why;
        if (!VerifyManifest(body, pubKeyB64, m, &why)) {
            r.error = url + ": " + why;
            GLOGW("packs: %s rejected: %s", url.c_str(), why.c_str());
            continue;
        }
        r.usedUrl = url;
        r.error.clear();
        have = true;
        break;
    }
    if (!have) return r;
    GLOGI("packs: %s verified: %zu pack(s), %zu file(s), %llu bytes (issued %s)", r.usedUrl.c_str(), m.packs.size(), m.files,
          m.bytes, m.issued.c_str());

    const std::wstring sync = root + L"\\.sync";
    const std::wstring guard = sync + L"\\";
    const std::wstring blobs = sync + L"\\blobs";
    const std::wstring stateFile = sync + L"\\state.txt";
    if (!MakeDirs(blobs)) { r.error = "cannot create " + Narrow(blobs) + " (" + Narrow(FormatError(GetLastError())) + ")"; return r; }
    DeleteTree(sync + L"\\new", guard);  // leftovers of an interrupted run
    DeleteTree(sync + L"\\old", guard);
    State st = LoadState(stateFile);

    // 2. What each pack lacks.
    struct Work { const Pack* pack; std::vector<const File*> need; };  // need = files whose bytes are not on disk
    std::vector<Work> work;
    std::set<std::string> published;
    for (const Pack& pk : m.packs) {
        published.insert(Lower(pk.name));
        const std::wstring dir = root + L"\\" + Widen(pk.name);
        std::map<std::string, std::string> local;
        if (IsDir(dir)) ListFiles(dir, "", local);
        Work w{ &pk, {} };
        bool current = IsDir(dir);
        for (const File& f : pk.files) {
            const std::string key = Lower(pk.name + "/" + f.path);
            const std::wstring path = dir + L"\\" + ToBackslash(f.path);
            const bool here = local.count(Lower(f.path)) && CachedHash(st, key, path, f.size) == f.sha256;
            if (!here) { w.need.push_back(&f); current = false; }
        }
        // A file the pack no longer has would still be streamed by the engine: the folder must hold exactly the list.
        if (current && local.size() != pk.files.size()) current = false;
        if (current) { ++r.packsCurrent; continue; }
        work.push_back(std::move(w));
    }

    // 3. Packs we installed that are no longer published go; an operator's own folders stay.
    for (auto it = st.managed.begin(); it != st.managed.end();) {
        if (published.count(*it)) { ++it; continue; }
        const std::wstring dir = root + L"\\" + Widen(*it);
        if (IsDir(dir)) {
            MakeDirs(sync + L"\\old");
            const std::wstring away = sync + L"\\old\\" + Widen(*it);
            if (MoveFileExW(dir.c_str(), away.c_str(), 0)) {
                DeleteTree(away, guard);
                ++r.packsRemoved;
                GLOGI("packs: removed %s (no longer published)", it->c_str());
            } else {
                GLOGW("packs: could not remove %s (%s)", it->c_str(), Narrow(FormatError(GetLastError())).c_str());
                ++it;
                continue;
            }
        }
        for (auto c = st.cache.begin(); c != st.cache.end();)
            c = c->first.compare(0, it->size() + 1, *it + "/") == 0 ? st.cache.erase(c) : std::next(c);
        it = st.managed.erase(it);
    }

    // 4. The bytes to fetch: each distinct sha once, minus blobs a previous run already finished.
    Progress pr;
    pr.packsToInstall = (int)work.size();
    std::set<std::string> counted;
    for (const Work& w : work)
        for (const File* f : w.need) {
            if (!counted.insert(f->sha256).second) continue;
            unsigned long long sz = 0, mt = 0;
            const std::wstring blob = blobs + L"\\" + Widen(f->sha256);
            if (StatFile(blob, &sz, &mt) && sz == f->size) continue;
            pr.totalBytes += f->size;
        }
    if (pr.totalBytes > 0) {
        ULARGE_INTEGER freeBytes = {};
        if (GetDiskFreeSpaceExW(root.c_str(), &freeBytes, nullptr, nullptr) &&
            freeBytes.QuadPart < pr.totalBytes + 256ull * 1024 * 1024) {
            r.error = "not enough disk space for the car packs (" + std::to_string(pr.totalBytes >> 20) + " MB needed, " +
                      std::to_string(freeBytes.QuadPart >> 20) + " MB free)";
            r.packsFailed = (int)work.size();
            SaveState(stateFile, st);
            return r;
        }
    }
    if (progress) progress(pr);

    // 5. Per pack: fetch its blobs, assemble the whole new folder, swap it in.
    std::set<std::string> keepBlobs;  // blobs of packs that failed: the next run resumes from them
    for (const Work& w : work) {
        const Pack& pk = *w.pack;
        std::string fail;
        for (const File* f : w.need) {
            const std::wstring blob = blobs + L"\\" + Widen(f->sha256);
            unsigned long long sz = 0, mt = 0;
            if (StatFile(blob, &sz, &mt) && sz == f->size) {
                if (HashFile(blob, nullptr) == f->sha256) continue;
                DeleteFileW(blob.c_str());
            }
            const std::wstring part = blob + L".part";
            const unsigned long long base = pr.doneBytes;
            std::string why;
            const bool ok = io.download(BlobUrl(r.usedUrl, f->sha256), f->size, part,
                [&](unsigned long long got) { pr.doneBytes = base + std::min(got, f->size); if (progress) progress(pr); }, &why);
            if (!ok) { DeleteFileW(part.c_str()); fail = f->path + ": " + why; break; }
            unsigned long long got = 0;
            const std::string sha = HashFile(part, &got);
            if (got != f->size || sha != f->sha256) {
                DeleteFileW(part.c_str());
                fail = f->path + ": downloaded bytes do not match the signed list (size " + std::to_string(got) + " of " +
                       std::to_string(f->size) + ")";
                break;
            }
            if (!MoveFileExW(part.c_str(), blob.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                DeleteFileW(part.c_str());
                fail = f->path + ": cannot keep the download (" + Narrow(FormatError(GetLastError())) + ")";
                break;
            }
            pr.doneBytes = base + f->size;
            r.downloadedBytes += f->size;
            if (progress) progress(pr);
        }

        // Assemble <sync>\new\<name>: unchanged files are hard links to the live ones (a copy if the volume refuses),
        // new files hard links to their blob.
        const std::wstring live = root + L"\\" + Widen(pk.name);
        const std::wstring fresh = sync + L"\\new\\" + Widen(pk.name);
        if (fail.empty()) {
            DeleteTree(fresh, guard);
            std::set<const File*> needSet(w.need.begin(), w.need.end());
            for (const File& f : pk.files) {
                const std::wstring dst = fresh + L"\\" + ToBackslash(f.path);
                const std::wstring src = needSet.count(&f) ? blobs + L"\\" + Widen(f.sha256) : live + L"\\" + ToBackslash(f.path);
                if (!MakeDirs(dst.substr(0, dst.find_last_of(L'\\')))) { fail = "cannot create a folder for " + f.path; break; }
                if (!CreateHardLinkW(dst.c_str(), src.c_str(), nullptr) && !CopyFileW(src.c_str(), dst.c_str(), TRUE)) {
                    fail = f.path + ": cannot place the file (" + Narrow(FormatError(GetLastError())) + ")";
                    break;
                }
            }
        }
        // Swap: the old folder out, the new one in. A failure on the second rename puts the old one back.
        if (fail.empty()) {
            const std::wstring away = sync + L"\\old\\" + Widen(pk.name);
            MakeDirs(sync + L"\\old");
            DeleteTree(away, guard);
            const bool hadOld = IsDir(live);
            if (hadOld && !MoveFileExW(live.c_str(), away.c_str(), 0)) {
                fail = "cannot replace the old folder (" + Narrow(FormatError(GetLastError())) + "); is the game running?";
            } else if (!MoveFileExW(fresh.c_str(), live.c_str(), 0)) {
                fail = "cannot move the new folder in (" + Narrow(FormatError(GetLastError())) + ")";
                if (hadOld) MoveFileExW(away.c_str(), live.c_str(), 0);
            } else {
                if (hadOld) DeleteTree(away, guard);
            }
        }
        if (!fail.empty()) {
            DeleteTree(fresh, guard);
            ++r.packsFailed;
            if (r.error.empty()) r.error = pk.name + ": " + fail;
            GLOGW("packs: %s not installed: %s", pk.name.c_str(), fail.c_str());
            for (const File* f : w.need) keepBlobs.insert(f->sha256);
            continue;
        }
        st.managed.insert(Lower(pk.name));
        for (const File& f : pk.files)
            Remember(st, Lower(pk.name + "/" + f.path), live + L"\\" + ToBackslash(f.path), f.sha256);
        ++r.packsInstalled;
        GLOGI("packs: %s installed (%zu file(s), %zu downloaded)", pk.name.c_str(), pk.files.size(), w.need.size());
    }

    // 6. Blobs only a failed pack still needs stay; the rest (and every .part) go.
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((blobs + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (!keepBlobs.count(Narrow(fd.cFileName))) DeleteFileW((blobs + L"\\" + fd.cFileName).c_str());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    DeleteTree(sync + L"\\new", guard);
    DeleteTree(sync + L"\\old", guard);
    // Cache entries of files that are not in any published pack are dropped (the file list is the manifest's).
    {
        std::set<std::string> keys;
        for (const Pack& pk : m.packs)
            for (const File& f : pk.files) keys.insert(Lower(pk.name + "/" + f.path));
        for (auto c = st.cache.begin(); c != st.cache.end();) c = keys.count(c->first) ? std::next(c) : st.cache.erase(c);
    }
    if (!SaveState(stateFile, st)) GLOGW("packs: could not save %s", Narrow(stateFile).c_str());
    r.ok = r.packsFailed == 0;
    GLOGI("packs: %d current, %d installed, %d failed, %d removed, %llu bytes downloaded", r.packsCurrent, r.packsInstalled,
          r.packsFailed, r.packsRemoved, r.downloadedBytes);
    return r;
}

}  // namespace gage::packsync
