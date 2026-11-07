// Per-account character rosters (0.5.9 characters; libs/sync/character.h; Config::characters / characterDir; docs/api/server.md
// "Characters"): one file per VERIFIED platform account under Config::characterDir, holding sync::chr::EncodeRoster's bytes (the same form
// the CharRoster message carries), written with AtomicWriteFile so a crash leaves the old roster or the new one. A guest's character lives
// for its session only (a guest is known by a name anyone can type).
//
// Load mirrors InventoryStore: a file that does not decode is renamed aside (<file>.corrupt) and the account starts with no character (the
// server then creates one at once); a file that cannot be read is left alone and the session must not write over it.
//
// The name index: two characters may not share a full name (sync::chr::NameKey, case-insensitive) on the whole server. Scan() reads every
// saved roster at start; Index() replaces an owner's names after every change; Unindex() drops an owner whose roster is not kept (a guest,
// or a verified player while saving is off) when it leaves. An owner is any string that names one roster: the account id for a verified
// player, "guest:" + the name key for a guest.
//
// Thread-safety: none needed; the server calls it from its Poll thread only.
#pragma once

#include "sync/character.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace gage::server {

    class CharacterStore
    {
    public:
        explicit CharacterStore(std::string dir = std::string()) : dir_(std::move(dir)) {}
        void SetDir(std::string dir) { dir_ = std::move(dir); }
        bool Enabled() const { return !dir_.empty(); }
        const std::string& Dir() const { return dir_; }

        enum class Loaded
        {
            Saved,       // the account's roster, decoded
            Fresh,       // no roster yet (or saving off, or a guest): empty, the caller creates the instant character and saves it
            Corrupt,     // a roster that does not decode: renamed aside, empty
            Unreadable,  // a roster that could not be read: empty for this session and NOTHING may be saved
        };
        static const char* ToString(Loaded l);

        // The roster of a verified account (empty for a guest: accountId ""). *why gets a one-line reason for Corrupt / Unreadable.
        Loaded Load(const std::string& accountId, sync::chr::Roster& out, std::string* why = nullptr);
        bool Save(const std::string& accountId, const sync::chr::Roster& r, std::string* why = nullptr);
        // "<dir>/<hex of the account id>.chr".
        std::string PathFor(const std::string& accountId) const;

        // ---- the name index ----
        struct ScanReport
        {
            size_t files = 0;        // .chr files read
            size_t characters = 0;   // names indexed
            size_t unreadable = 0;   // files that could not be read or did not decode (left alone; Load deals with them on the join)
            size_t duplicates = 0;   // names a second file also held (the first file read keeps it; logged)
        };
        // Reads every roster in the folder (a missing folder is an empty index).
        ScanReport Scan(std::string* why = nullptr);
        // Whether another character than (owner, slot) holds the name `key`.
        bool Taken(const std::string& key, const std::string& owner, int slot) const;
        // The owner's names become exactly the characters of `r`.
        void Index(const std::string& owner, const sync::chr::Roster& r);
        void Unindex(const std::string& owner);
        size_t IndexedNames() const { return byKey_.size(); }

    private:
        bool EnsureDir(std::string* why);
        std::string dir_;
        bool dirMade_ = false;
        std::map<std::string, std::pair<std::string, int>> byKey_;   // name key -> (owner, slot)
        std::map<std::string, std::vector<std::string>> keysOf_;     // owner -> its name keys
    };

} // namespace gage::server
