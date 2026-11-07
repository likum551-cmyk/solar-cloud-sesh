// See character_store.h.
#include "character_store.h"

#include "util.h"

#include <filesystem>
#include <system_error>

namespace gage::server {

    namespace fs = std::filesystem;

    namespace {
        // The account id a "<hex>.chr" file name stands for ("" when the stem is not lower-case hex of an even length).
        std::string AccountOfStem(const std::string& stem)
        {
            auto nib = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                return -1;
            };
            if (stem.empty() || stem.size() % 2) return std::string();
            std::string id;
            for (size_t i = 0; i < stem.size(); i += 2)
            {
                const int hi = nib(stem[i]), lo = nib(stem[i + 1]);
                if (hi < 0 || lo < 0) return std::string();
                id.push_back((char)(hi * 16 + lo));
            }
            return id;
        }
    }

    const char* CharacterStore::ToString(Loaded l)
    {
        switch (l)
        {
        case Loaded::Saved: return "saved";
        case Loaded::Fresh: return "fresh";
        case Loaded::Corrupt: return "corrupt";
        case Loaded::Unreadable: return "unreadable";
        }
        return "?";
    }

    std::string CharacterStore::PathFor(const std::string& accountId) const
    {
        static const char* const kHex = "0123456789abcdef";
        std::string name;
        name.reserve(accountId.size() * 2 + 4);
        for (unsigned char c : accountId)
        {
            name.push_back(kHex[c >> 4]);
            name.push_back(kHex[c & 15]);
        }
        name += ".chr";
        return (fs::path(dir_) / name).string();
    }

    bool CharacterStore::EnsureDir(std::string* why)
    {
        if (dirMade_)
            return true;
        std::error_code ec;
        fs::create_directories(dir_, ec);
        const fs::file_status st = fs::status(dir_, ec);
        if (st.type() != fs::file_type::directory)
        {
            if (why)
                *why = "cannot create the character folder " + dir_ + (ec ? ": " + ec.message() : std::string());
            return false;
        }
        dirMade_ = true;
        return true;
    }

    CharacterStore::Loaded CharacterStore::Load(const std::string& accountId, sync::chr::Roster& out, std::string* why)
    {
        out = sync::chr::Roster();
        if (!Enabled() || accountId.empty())
            return Loaded::Fresh;
        const std::string path = PathFor(accountId);
        std::string bytes, err;
        bool missing = false;
        if (!ReadWholeFile(path, bytes, &err, &missing))
        {
            if (missing)
                return Loaded::Fresh;
            if (why)
                *why = path + ": " + err;
            return Loaded::Unreadable;
        }
        if (sync::chr::DecodeRoster(bytes, out))
            return Loaded::Saved;
        out = sync::chr::Roster();
        std::error_code ec;
        fs::rename(path, path + ".corrupt", ec);
        if (why)
            *why = path + ": " + std::to_string(bytes.size()) + " bytes that do not decode" +
                   (ec ? " (could not rename it aside: " + ec.message() + "; not saving this session)" : std::string(" (kept as .corrupt)"));
        return ec ? Loaded::Unreadable : Loaded::Corrupt;
    }

    bool CharacterStore::Save(const std::string& accountId, const sync::chr::Roster& r, std::string* why)
    {
        if (!Enabled())
        {
            if (why) *why = "saving is off";
            return false;
        }
        if (accountId.empty())
        {
            if (why) *why = "a guest is never saved";
            return false;
        }
        if (!EnsureDir(why))
            return false;
        return AtomicWriteFile(PathFor(accountId), sync::chr::EncodeRoster(r), why);
    }

    CharacterStore::ScanReport CharacterStore::Scan(std::string* why)
    {
        ScanReport rep;
        byKey_.clear();
        keysOf_.clear();
        if (!Enabled())
            return rep;
        std::error_code ec;
        const fs::file_status st = fs::status(dir_, ec);
        if (st.type() == fs::file_type::not_found)
            return rep;
        if (st.type() != fs::file_type::directory)
        {
            if (why) *why = dir_ + " is not a folder";
            return rep;
        }
        fs::directory_iterator it(dir_, ec), end;
        if (ec)
        {
            if (why) *why = "cannot list " + dir_ + ": " + ec.message();
            return rep;
        }
        for (; it != end; it.increment(ec))
        {
            if (ec) break;
            const fs::path p = it->path();
            if (p.extension() != ".chr") continue;
            const std::string account = AccountOfStem(p.stem().string());
            if (account.empty()) continue;
            rep.files++;
            std::string bytes, err;
            sync::chr::Roster r;
            if (!ReadWholeFile(p.string(), bytes, &err) || !sync::chr::DecodeRoster(bytes, r))
            {
                rep.unreadable++;
                continue;
            }
            for (int s = 0; s < sync::chr::kMaxSlots; ++s)
            {
                if (!r.Has(s)) continue;
                const std::string key = sync::chr::NameKey(r.chars[s].first, r.chars[s].last);
                if (byKey_.count(key))
                {
                    rep.duplicates++;
                    continue;
                }
                byKey_[key] = {account, s};
                keysOf_[account].push_back(key);
                rep.characters++;
            }
        }
        if (ec && why) *why = "listing " + dir_ + " stopped: " + ec.message();
        return rep;
    }

    bool CharacterStore::Taken(const std::string& key, const std::string& owner, int slot) const
    {
        const auto it = byKey_.find(key);
        if (it == byKey_.end()) return false;
        return !(it->second.first == owner && it->second.second == slot);
    }

    void CharacterStore::Index(const std::string& owner, const sync::chr::Roster& r)
    {
        Unindex(owner);
        std::vector<std::string>& keys = keysOf_[owner];
        for (int s = 0; s < sync::chr::kMaxSlots; ++s)
        {
            if (!r.Has(s)) continue;
            const std::string key = sync::chr::NameKey(r.chars[s].first, r.chars[s].last);
            // A name another owner already holds stays theirs (the rules never let a duplicate in; a duplicate can only come from files
            // edited by hand, and Scan() counted it).
            if (byKey_.count(key)) continue;
            byKey_[key] = {owner, s};
            keys.push_back(key);
        }
    }

    void CharacterStore::Unindex(const std::string& owner)
    {
        const auto it = keysOf_.find(owner);
        if (it == keysOf_.end()) return;
        for (const std::string& key : it->second)
        {
            const auto k = byKey_.find(key);
            if (k != byKey_.end() && k->second.first == owner) byKey_.erase(k);
        }
        keysOf_.erase(it);
    }

} // namespace gage::server
