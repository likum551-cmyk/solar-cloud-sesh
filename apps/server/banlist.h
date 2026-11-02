// The server's ban list and its bans.json file. Portable C++17.
//
// An entry names a player, an IPv4 address, or both (what `ban <online player>`
// writes). A connection is banned when its address or its (case-insensitive) name
// matches any entry; removing an entry lifts all of it.
//
// bans.json:
//   { "bans": [ { "name": "Alice", "ip": "203.0.113.5", "reason": "cheating",
//                 "at": "2026-10-29T08:34:56Z" } ] }
// "name" and "ip" are each optional but at least one must be there; "reason" and
// "at" are optional text. Every mutation is saved at once, atomically (temp file,
// flush, rename over the old one), so a crash cannot leave a half-written list.
// A file that exists but cannot be parsed is an error, never an empty list: a
// damaged ban list must not silently unban everybody.
#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace gage::server {

    // One line of bans.json. At least one of `name` and `ip` is set for an entry in a list.
    struct BanEntry
    {
        std::string name;   // as written by the admin (matching is case-insensitive); may be empty
        std::string ip;     // dotted IPv4; may be empty
        std::string reason; // free text shown to the banned player; may be empty
        std::string at;     // ISO-8601 UTC, informational
    };

    // The in-memory ban list backed by one file. Lookups are O(1) hash lookups.
    //
    // Thread-safety: none. Use one instance from one thread (the server keeps it on
    // its Poll() thread). Pointers returned by Match/FindName/FindIp point into the list
    // and are invalidated by the next Add, Remove or Load: copy what you need first.
    // Errors: nothing throws; failures are a false return with a one-line reason.
    class BanList
    {
    public:
        // Loads `path`, replacing the list. A missing file is an empty list (the file
        // appears on the first ban); an unreadable or invalid one (a directory, no
        // permission, bad JSON, unknown key, bad name/address, trailing data, over 10000
        // entries) returns false with *err ("<path>: <why>") and leaves the list
        // unchanged. An empty `path` keeps the list in memory only (no file is ever read
        // or written). Remembers `path` for later saves.
        bool Load(const std::string& path, std::string* err);

        // Adds `entry` and saves. `name` and/or `ip` must be non-empty, `ip` a strict
        // dotted IPv4, `name` a valid player name, `reason` valid UTF-8 without control
        // characters (at most 200 bytes); `at` is filled with the current UTC time when
        // empty. The entry is kept whole even when one half is already banned in another
        // entry. Returns true when added. Returns false with *err when invalid, when the
        // list is full, or when everything the entry would ban is already banned (then
        // *already, when non-null, is set true and *err is "already banned"). A failed
        // save does NOT fail the call: the ban stays in effect in memory and the reason
        // is put in *saveErr (when non-null; left untouched on success), so the caller
        // can warn the admin.
        bool Add(BanEntry entry, std::string* err, std::string* saveErr, bool* already = nullptr);

        // Removes every entry whose name matches `target` (case-insensitively) or whose
        // ip equals it, whole (a name+address entry goes away entirely), and saves when
        // it removed any. Returns how many entries were removed (0 = nothing matched,
        // nothing saved). A failed save is reported in *saveErr like Add's.
        size_t Remove(const std::string& target, std::string* saveErr);

        // The entry that bans a connection with this `name` and/or `ip`, or null. Either
        // argument may be empty to skip that half (address-only check before Hello,
        // name-only check). The name matches case-insensitively, the address exactly;
        // when both match different entries the address's entry wins.
        const BanEntry* Match(const std::string& name, const std::string& ip) const;
        // The first entry banning this name (case-insensitive), or null.
        const BanEntry* FindName(const std::string& name) const;
        // The first entry banning exactly this dotted-IPv4 address, or null.
        const BanEntry* FindIp(const std::string& ip) const;

        // All entries, in the order they were added (the order of the file).
        const std::vector<BanEntry>& entries() const { return entries_; }
        // The file Load() was given ("" = memory only).
        const std::string& path() const { return path_; }

        // The exact text a save writes (the bans.json format above); public for tests.
        std::string ToJson() const;

    private:
        bool Save(std::string* err) const;
        void Reindex();

        std::string path_;
        std::vector<BanEntry> entries_;
        std::unordered_map<std::string, size_t> byName_; // NameKey -> index of the first entry with it
        std::unordered_map<std::string, size_t> byIp_;
    };

}
