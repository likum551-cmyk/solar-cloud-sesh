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

    std::string InventoryStore::PathFor(const std::string& accountId, int slot) const
    {
        static const char* const kHex = "0123456789abcdef";
        std::string name;
        name.reserve(accountId.size() * 2 + 8);
        for (unsigned char c : accountId)
        {
            name.push_back(kHex[c >> 4]);
            name.push_back(kHex[c & 15]);
        }
        if (slot > 0)
            name += ".c" + std::to_string(slot);
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

    InventoryStore::Loaded InventoryStore::Load(const std::string& accountId, sync::inv::Character& out, std::string* why, int slot)
    {
        if (!Enabled() || accountId.empty())
        {
            out = sync::inv::StarterKit();
            return Loaded::Fresh;
        }
        const std::string path = PathFor(accountId, slot);
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

    bool InventoryStore::Save(const std::string& accountId, const sync::inv::Character& ch, std::string* why, int slot)
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
        return AtomicWriteFile(PathFor(accountId, slot), sync::inv::EncodeForSave(ch), why);
    }

    InventoryStore::Retired InventoryStore::Retire(const std::string& accountId, int slot, std::string* why)
    {
        if (!Enabled() || accountId.empty())
            return Retired::None;
        const std::string path = PathFor(accountId, slot);
        std::error_code ec;
        const fs::file_status st = fs::status(path, ec);
        // MinGW reports not_found AND sets ec for a missing file: test the type first (lessons 2026-10-29).
        if (st.type() == fs::file_type::not_found)
            return Retired::None;
        if (st.type() != fs::file_type::regular)
        {
            if (why) *why = path + ": not a readable file" + (ec ? " (" + ec.message() + ")" : std::string());
            return Retired::Failed;
        }
        fs::rename(path, path + ".deleted", ec);   // replaces an older .deleted (a slot deleted twice)
        if (ec)
        {
            if (why) *why = path + ": cannot set it aside: " + ec.message();
            return Retired::Failed;
        }
        return Retired::SetAside;
    }

    bool InventoryStore::Restore(const std::string& accountId, int slot, std::string* why)
    {
        const std::string path = PathFor(accountId, slot);
        std::error_code ec;
        fs::rename(path + ".deleted", path, ec);
        if (ec && why) *why = path + ".deleted: cannot put it back: " + ec.message();
        return !ec;
    }

    bool InventoryStore::Purge(const std::string& accountId, int slot, std::string* why)
    {
        const std::string path = PathFor(accountId, slot) + ".deleted";
        std::error_code ec;
        fs::remove(path, ec);
        if (ec && why) *why = path + ": cannot remove it: " + ec.message();
        return !ec;
    }

} // namespace gage::server
