#include "fsutil.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace gage::scripting::fsutil {

    namespace fs = std::filesystem;

    namespace {

        fs::path ToPath(const std::string& utf8)
        {
            return fs::u8path(utf8);
        }

        std::FILE* Open(const fs::path& p, bool write)
        {
#ifdef _WIN32
            return _wfopen(p.c_str(), write ? L"wb" : L"rb");
#else
            return std::fopen(p.c_str(), write ? "wb" : "rb");
#endif
        }

        std::string Lower(const std::string& s)
        {
            std::string o = s;
            for (char& c : o)
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            return o;
        }

    }

    bool ReadFile(const std::string& path, std::string& out, std::string* err, bool* missing, size_t maxBytes)
    {
        if (missing) *missing = false;
        const fs::path p = ToPath(path);
        std::error_code ec;
        const fs::file_status st = fs::status(p, ec);
        // not_found is reported WITH an error code on some platforms (MinGW): test the type first.
        if (st.type() == fs::file_type::not_found)
        {
            if (missing) *missing = true;
            if (err) *err = "no such file";
            return false;
        }
        if (!ec && st.type() == fs::file_type::directory)
        {
            if (err) *err = "is a directory";
            return false;
        }
        std::FILE* f = Open(p, false);
        if (!f)
        {
            if (err) *err = "cannot open (permission denied?)";
            return false;
        }
        out.clear();
        char buf[16384];
        for (;;)
        {
            const size_t n = std::fread(buf, 1, sizeof(buf), f);
            if (n > 0)
            {
                if (out.size() + n > maxBytes)
                {
                    std::fclose(f);
                    if (err) *err = "file is larger than " + std::to_string(maxBytes) + " bytes";
                    return false;
                }
                out.append(buf, n);
            }
            if (n < sizeof(buf))
            {
                const bool bad = std::ferror(f) != 0;
                std::fclose(f);
                if (bad)
                {
                    if (err) *err = "read error";
                    return false;
                }
                return true;
            }
        }
    }

    bool AtomicWrite(const std::string& path, const std::string& content, std::string* err)
    {
        auto fail = [&](const std::string& why) {
            if (err) *err = why;
            return false;
        };
        if (path.empty()) return fail("empty path");
        const std::string tmpName = path + ".tmp";
        const fs::path tmp = ToPath(tmpName);
        std::FILE* f = Open(tmp, true);
        if (!f) return fail("cannot create " + tmpName);
        bool ok = std::fwrite(content.data(), 1, content.size(), f) == content.size() && std::fflush(f) == 0;
#ifdef _WIN32
        ok = ok && _commit(_fileno(f)) == 0;
#else
        ok = ok && fsync(fileno(f)) == 0;
#endif
        ok = (std::fclose(f) == 0) && ok;
        std::error_code ec;
        if (!ok)
        {
            fs::remove(tmp, ec);
            return fail("cannot write " + tmpName + " (disk full or read-only?)");
        }
        // fs::rename replaces an existing target on both platforms (MoveFileEx with
        // REPLACE_EXISTING on Windows, rename(2) elsewhere); std::rename would not on Windows.
        fs::rename(tmp, ToPath(path), ec);
        if (ec)
        {
            std::error_code ignore;
            fs::remove(tmp, ignore);
            return fail("cannot rename " + tmpName + " over " + path + ": " + ec.message());
        }
#ifndef _WIN32
        // Make the rename itself durable: fsync the directory that holds the entry.
        const fs::path dir = ToPath(path).parent_path();
        const std::string dirName = dir.empty() ? std::string(".") : dir.string();
        const int dfd = ::open(dirName.c_str(), O_RDONLY);
        if (dfd >= 0)
        {
            (void)fsync(dfd);
            ::close(dfd);
        }
#endif
        return true;
    }

    bool EnsureDir(const std::string& dir, std::string* err)
    {
        std::error_code ec;
        const fs::path p = ToPath(dir);
        fs::create_directories(p, ec);
        std::error_code ec2;
        if (fs::is_directory(p, ec2)) return true;
        if (err) *err = "cannot create directory " + dir + (ec ? ": " + ec.message() : std::string());
        return false;
    }

    bool ListSubdirs(const std::string& dir, std::vector<std::string>& names, std::string* err)
    {
        names.clear();
        std::error_code ec;
        const fs::path p = ToPath(dir);
        const fs::file_status st = fs::status(p, ec);
        if (st.type() == fs::file_type::not_found) return true;
        if (ec || st.type() != fs::file_type::directory)
        {
            if (err) *err = dir + " is not a directory";
            return false;
        }
        fs::directory_iterator it(p, ec), end;
        if (ec)
        {
            if (err) *err = "cannot list " + dir + ": " + ec.message();
            return false;
        }
        for (; it != end; it.increment(ec))
        {
            if (ec) break;
            std::error_code e2;
            const fs::file_status s = it->symlink_status(e2);
            if (e2 || s.type() != fs::file_type::directory) continue; // links are never resources
#ifdef _WIN32
            // A junction is a reparse point that this runtime may report as a plain directory.
            const std::wstring wpath = it->path().wstring();
            const unsigned long attrs = GetFileAttributesW(wpath.c_str());
            if (attrs != 0xFFFFFFFFul && (attrs & 0x400ul /* FILE_ATTRIBUTE_REPARSE_POINT */) != 0) continue;
#endif
            names.push_back(it->path().filename().u8string());
        }
        if (ec)
        {
            if (err) *err = "cannot list " + dir + ": " + ec.message();
            return false;
        }
        std::sort(names.begin(), names.end());
        return true;
    }

    bool IsRegularFile(const std::string& p)
    {
        std::error_code ec;
        return fs::is_regular_file(ToPath(p), ec) && !ec;
    }

    bool IsSafeRelPath(const std::string& rel)
    {
        if (rel.empty() || rel.size() > 200) return false;
        if (rel.front() == '/' || rel.back() == '/') return false;
        size_t segStart = 0;
        for (size_t i = 0; i <= rel.size(); i++)
        {
            const bool end = i == rel.size();
            const unsigned char c = end ? '/' : static_cast<unsigned char>(rel[i]);
            if (!end && (c < 0x20 || c == 0x7F || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
                         c == '<' || c == '>' || c == '|'))
                return false;
            if (c == '/')
            {
                const std::string seg = rel.substr(segStart, i - segStart);
                if (seg.empty() || seg == "." || seg == "..") return false;
                if (seg.back() == '.' || seg.back() == ' ') return false; // Windows strips these: alias of another name
                segStart = i + 1;
            }
        }
        return Utf8Valid(rel.data(), rel.size());
    }

    bool ResolveInside(const std::string& rootDir, const std::string& rel, std::string* out, std::string* err)
    {
        auto fail = [&](const std::string& why) {
            if (err) *err = why;
            return false;
        };
        if (!IsSafeRelPath(rel))
            return fail("\"" + rel + "\" is not a safe relative path (plain names separated by '/', no '..', no absolute path)");
        const fs::path root = ToPath(rootDir);
        fs::path cur = root;
        size_t start = 0;
        while (start <= rel.size())
        {
            size_t slash = rel.find('/', start);
            if (slash == std::string::npos) slash = rel.size();
            cur /= ToPath(rel.substr(start, slash - start));
            std::error_code ec;
            const fs::file_status st = fs::symlink_status(cur, ec);
            if (st.type() == fs::file_type::not_found) return fail("\"" + rel + "\" does not exist");
            if (ec) return fail("cannot inspect \"" + rel + "\": " + ec.message());
            if (st.type() == fs::file_type::symlink) return fail("\"" + rel + "\" goes through a symbolic link, which resources may not use");
#ifdef _WIN32
            // Junctions are reparse points that some runtimes report as plain directories.
            const std::wstring wcur = cur.wstring();
            const unsigned long attrs = GetFileAttributesW(wcur.c_str());
            if (attrs != 0xFFFFFFFFul && (attrs & 0x400ul /* FILE_ATTRIBUTE_REPARSE_POINT */) != 0)
                return fail("\"" + rel + "\" goes through a link (junction), which resources may not use");
#endif
            if (slash == rel.size()) break;
            start = slash + 1;
        }
        std::error_code ec;
        if (!fs::is_regular_file(cur, ec) || ec) return fail("\"" + rel + "\" is not a regular file");
        // Belt and braces: the resolved location must still sit under the root.
        const fs::path canonRoot = fs::weakly_canonical(root, ec);
        std::error_code ec2;
        const fs::path canonFile = fs::weakly_canonical(cur, ec2);
        if (ec || ec2) return fail("cannot resolve \"" + rel + "\"");
        auto ri = canonRoot.begin();
        auto fi = canonFile.begin();
        for (; ri != canonRoot.end(); ++ri, ++fi)
        {
            if (fi == canonFile.end()) return fail("\"" + rel + "\" resolves outside the resource folder");
#ifdef _WIN32
            if (Lower(fi->u8string()) != Lower(ri->u8string()))
#else
            if (fi->u8string() != ri->u8string())
#endif
                return fail("\"" + rel + "\" resolves outside the resource folder");
        }
        if (out) *out = cur.u8string();
        return true;
    }

    bool ValidResourceName(const std::string& name)
    {
        if (name.empty() || name.size() > 64) return false;
        for (char ch : name)
        {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (!(std::isalnum(c) || c == '_' || c == '-') || c >= 0x80) return false;
        }
        const std::string l = Lower(name);
        static const char* const kDevices[] = {"con", "prn", "aux", "nul"};
        for (const char* d : kDevices)
            if (l == d) return false;
        if (l.size() == 4 && (l.compare(0, 3, "com") == 0 || l.compare(0, 3, "lpt") == 0) && l[3] >= '1' && l[3] <= '9')
            return false;
        return true;
    }

    bool Utf8Valid(const char* p, size_t n)
    {
        const unsigned char* s = reinterpret_cast<const unsigned char*>(p);
        size_t i = 0;
        while (i < n)
        {
            const unsigned char c = s[i];
            if (c < 0x80)
            {
                i++;
                continue;
            }
            size_t len;
            unsigned cp;
            if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1Fu; }
            else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0Fu; }
            else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07u; }
            else return false;
            if (i + len > n) return false;
            for (size_t k = 1; k < len; k++)
            {
                if ((s[i + k] & 0xC0) != 0x80) return false;
                cp = (cp << 6) | (s[i + k] & 0x3Fu);
            }
            if (len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) return false;
            if (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) return false;
            i += len;
        }
        return true;
    }

    std::string StripBom(const std::string& s)
    {
        if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
            static_cast<unsigned char>(s[2]) == 0xBF)
            return s.substr(3);
        return s;
    }

    std::string Utf8Truncate(const std::string& s, size_t maxBytes)
    {
        if (s.size() <= maxBytes) return s;
        size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) cut--;
        return s.substr(0, cut);
    }

    std::string SanitizeForLog(const std::string& s)
    {
        std::string o = s;
        for (char& ch : o)
        {
            const unsigned char c = static_cast<unsigned char>(ch);
            if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7F) ch = '?';
        }
        return o;
    }

}
