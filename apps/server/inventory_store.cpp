// See inventory_store.h.
#include "inventory_store.h"

#include "util.h"

#include <filesystem>
#include <system_error>

namespace gage::server {

    namespace fs = std::filesystem;

    const char* InventoryStore::ToString(Loaded l)
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

    std::string InventoryStore::PathFor(const std::string& accountId) const
    {
        static const char* const kHex = "0123456789abcdef";
        std::string name;
        name.reserve(accountId.size() * 2 + 4);
        for (unsigned char c : accountId)
        {
            name.push_back(kHex[c >> 4]);
            name.push_back(kHex[c & 15]);
        }
        name += ".inv";
        return (fs::path(dir_) / name).string();
    }

    bool InventoryStore::EnsureDir(std::string* why)
    {
        if (dirMade_)
            return true;
        std::error_code ec;
        fs::create_directories(dir_, ec);
        // create_directories reports false with no error when the folder already exists; test what is there, not the return value.
        const fs::file_status st = fs::status(dir_, ec);
        if (st.type() != fs::file_type::directory)
        {
            if (why)
                *why = "cannot create the inventory folder " + dir_ + (ec ? ": " + ec.message() : std::string());
            return false;
        }
        dirMade_ = true;
        return true;
    }

    InventoryStore::Loaded InventoryStore::Load(const std::string& accountId, sync::inv::Character& out, std::string* why)
    {
        if (!Enabled() || accountId.empty())
        {
            out = sync::inv::StarterKit();
            return Loaded::Fresh;
        }
        const std::string path = PathFor(accountId);
        std::string bytes, err;
        bool missing = false;
        if (!ReadWholeFile(path, bytes, &err, &missing))
        {
            out = sync::inv::StarterKit();
            if (missing)
                return Loaded::Fresh;
            if (why)
                *why = path + ": " + err;
            return Loaded::Unreadable;
        }
        if (sync::inv::Decode(bytes, out))
            return Loaded::Saved;
        // Keep the bad bytes for an admin (one .corrupt per account: a second corrupt save replaces the first), start empty.
        std::error_code ec;
        fs::rename(path, path + ".corrupt", ec);
        out = sync::inv::Character();
        if (why)
            *why = path + ": " + std::to_string(bytes.size()) + " bytes that do not decode" +
                   (ec ? " (could not rename it aside: " + ec.message() + "; not saving this session)" : std::string(" (kept as .corrupt)"));
        // Not renamed aside: the bad save is still the only copy, so this session must not write over it.
        return ec ? Loaded::Unreadable : Loaded::Corrupt;
    }

    bool InventoryStore::Save(const std::string& accountId, const sync::inv::Character& ch, std::string* why)
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
        return AtomicWriteFile(PathFor(accountId), sync::inv::EncodeForSave(ch), why);
    }

} // namespace gage::server
