// Per-account inventory saves (0.6, GI's "server items, staged"): one file per platform account under Config::inventoryDir, holding
// sync::inv::Encode's bytes (the same form the InvState message carries), written with AtomicWriteFile so a crash leaves the old save or the
// new one. Only VERIFIED accounts are saved: a guest is known by a name anyone can type, so a guest gets the starter kit for the session and
// nothing is written (a name-keyed save would hand one player's items to whoever joins under that name).
//
// Load never invents items for a save it could not read: a file that is there but does not decode is renamed aside (<file>.corrupt, for an
// admin to look at) and the player starts EMPTY, not with a new starter kit (a corrupt save must not become a way to farm kits); a file that
// cannot be read at all (permissions, I/O) is left alone and the session is marked not to save, so it can never overwrite the real one.
//
// Thread-safety: none needed; the server calls it from its Poll thread only.
#pragma once

#include "sync/inventory.h"

#include <string>

namespace gage::server {

    class InventoryStore
    {
    public:
        // dir: where the saves go (created on first use; relative = the working directory, like bans.json). Empty = saving off (every player
        // gets the starter kit, nothing is written).
        explicit InventoryStore(std::string dir = std::string()) : dir_(std::move(dir)) {}
        void SetDir(std::string dir) { dir_ = std::move(dir); }
        bool Enabled() const { return !dir_.empty(); }
        const std::string& Dir() const { return dir_; }

        enum class Loaded
        {
            Saved,       // the account's save, decoded
            Fresh,       // no save yet (or saving off, or a guest): the starter kit; save it
            Corrupt,     // a save that does not decode: renamed aside, the player starts empty; save over the (now absent) file
            Unreadable,  // a save that could not be read: the player gets the starter kit for this session and NOTHING may be saved
        };
        static const char* ToString(Loaded l);

        // The save for a verified account (accountId non-empty) or the starter kit for a guest (accountId empty). *why (when non-null) gets a
        // one-line reason for Corrupt / Unreadable.
        // 0.5.9: `slot` is the character's roster slot (0..2): every character has its own items. Slot 0 is the account's pre-0.5.9 file,
        // so an existing save becomes the first character's without a migration.
        Loaded Load(const std::string& accountId, sync::inv::Character& out, std::string* why = nullptr, int slot = 0);
        // Writes the save. False (with a reason) when saving is off, the account is a guest, or the write failed.
        bool Save(const std::string& accountId, const sync::inv::Character& ch, std::string* why = nullptr, int slot = 0);

        // "<dir>/<hex of the account id>.inv" (slot 0) or "<dir>/<hex>.c<slot>.inv" (slots 1, 2): hex so any id the platform hands out
        // is a safe file name on both systems.
        std::string PathFor(const std::string& accountId, int slot = 0) const;

    private:
        bool EnsureDir(std::string* why);
        std::string dir_;
        bool dirMade_ = false;
    };

} // namespace gage::server
