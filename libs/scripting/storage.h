// Storage: the per-resource key-value store behind the Lua `storage` table. Pure C++ (no Lua)
// so it is testable on its own.
//
// Values are kept as compact JSON text, so a value read back is always a fresh copy and a
// stored value can never be mutated through a reference. The file is
// `<dir>/storage.json`, one JSON object `{"key": value, ...}`, written atomically
// (temp file + rename) so a crash leaves the old file or the new one, never half of either.
//
// Thread-safety: none, single server thread.
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace gage::scripting {

    // The function that writes a file atomically; the runtime passes fsutil::AtomicWrite and
    // tests pass one that fails or simulates a crash.
    using AtomicWriteFn = std::function<bool(const std::string& path, const std::string& content, std::string* err)>;

    class Storage
    {
    public:
        static constexpr size_t kMaxKeyBytes = 128;

        // Binds the store to `<dir>/storage.json` and loads it when the file exists. Nothing
        // is created on disk until the first Flush(). A missing file is an empty store. Returns
        // false with *err ("cannot read ...", "not valid JSON ...", "must be a JSON object",
        // "stored limit exceeded") for a file that exists but cannot be used: the caller must not
        // start the resource, so damaged data is never silently replaced by an empty store.
        bool Open(const std::string& dir, AtomicWriteFn writer, size_t maxBytes, std::string* err);

        // The JSON text stored under `key`, or nullptr. The pointer stays valid until the next
        // Set/Delete.
        const std::string* Get(const std::string& key) const;

        // Stores `json` (already valid compact JSON text, [json, json+n)) under `key`. Returns
        // false with *err for an invalid key (empty, over 128 bytes, not UTF-8) or when the
        // store would exceed its byte limit (the old value stays).
        bool Set(const std::string& key, const char* json, size_t n, std::string* err);

        // Removes `key`; true when it existed.
        bool Delete(const std::string& key);

        // All keys, sorted bytewise.
        std::vector<std::string> Keys() const;

        // True when there are changes not yet written.
        bool dirty() const { return dirty_; }

        // Writes the store when dirty (creating the directory if needed). Returns true when
        // nothing needed writing or the write succeeded; false with *err when it failed, in
        // which case the store stays dirty and the previous file is untouched.
        bool Flush(std::string* err);

        // Number of keys, and the bytes counted against the limit (keys + values + overhead).
        size_t count() const { return entries_.size(); }
        size_t bytes() const { return bytes_; }
        // The file this store reads and writes.
        const std::string& path() const { return path_; }

    private:
        std::string dir_;
        std::string path_;
        AtomicWriteFn writer_;
        size_t maxBytes_ = 0;
        size_t bytes_ = 0;
        bool dirty_ = false;
        std::map<std::string, std::string> entries_;
    };

}
