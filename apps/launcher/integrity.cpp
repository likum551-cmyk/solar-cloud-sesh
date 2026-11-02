// integrity.cpp — see integrity.h.

#include "integrity.h"
#include "common/logger.h"
#include "chain_hashes.h"  // generated: kGageChain / kGageChainCount

#include <bcrypt.h>
#include <vector>

#pragma comment(lib, "bcrypt")

namespace gage::integrity {

    std::string Sha256File(const std::wstring& path) {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, 0);
        if (f == INVALID_HANDLE_VALUE) return "";
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        unsigned char digest[32];
        bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
                  BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0));
        std::vector<unsigned char> buf(256 * 1024);
        while (ok) {
            DWORD got = 0;
            if (!ReadFile(f, buf.data(), (DWORD)buf.size(), &got, 0)) {
                ok = false;
                break;
            }
            if (got == 0) break;
            ok = BCRYPT_SUCCESS(BCryptHashData(hash, buf.data(), got, 0));
        }
        ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
        if (hash) BCryptDestroyHash(hash);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        CloseHandle(f);
        if (!ok) return "";
        static const char hex[] = "0123456789abcdef";
        std::string out;
        for (unsigned char b : digest) {
            out += hex[b >> 4];
            out += hex[b & 15];
        }
        return out;
    }

    bool VerifyFiles(const std::wstring& dir, const ChainFile* files, int count, std::wstring* bad) {
        if (count <= 0) {  // an empty list proves nothing
            if (bad) *bad = L"(no files listed)";
            GLOGE("integrity: the chain list is empty");
            return false;
        }
        for (int i = 0; i < count; ++i) {
            std::string got = Sha256File(dir + files[i].name);
            if (got.empty() || _stricmp(got.c_str(), files[i].sha256) != 0) {
                GLOGE("integrity: %s %s (expected %s, got %s)", Narrow(files[i].name).c_str(),
                      got.empty() ? "unreadable" : "changed", files[i].sha256, got.empty() ? "-" : got.c_str());
                if (bad) *bad = files[i].name;
                return false;
            }
        }
        GLOGI("integrity: %d file(s) match the build", count);
        return true;
    }

    bool VerifyChain(const std::wstring& dir, std::wstring* bad) {
        static_assert(sizeof(kGageChain[0]) == sizeof(ChainFile), "same layout");
        std::vector<ChainFile> files;
        for (int i = 0; i < kGageChainCount; ++i) files.push_back({ kGageChain[i].name, kGageChain[i].sha256 });
        return VerifyFiles(dir, files.data(), (int)files.size(), bad);
    }

}
