#include "storage.h"

#include "fsutil.h"
#include "json.h"

namespace gage::scripting {

    namespace {

        constexpr size_t kEntryOverhead = 8; // quotes, colon, comma, newline

        bool ValidKey(const std::string& key)
        {
            return !key.empty() && key.size() <= Storage::kMaxKeyBytes && fsutil::Utf8Valid(key.data(), key.size());
        }

    }

    bool Storage::Open(const std::string& dir, AtomicWriteFn writer, size_t maxBytes, std::string* err)
    {
        auto fail = [&](const std::string& why) {
            if (err) *err = why;
            return false;
        };
        dir_ = dir;
        path_ = dir + "/storage.json";
        writer_ = std::move(writer);
        maxBytes_ = maxBytes;
        bytes_ = 0;
        dirty_ = false;
        entries_.clear();

        std::string text, why;
        bool missing = false;
        if (!fsutil::ReadFile(path_, text, &why, &missing, maxBytes + 4096))
        {
            if (missing) return true;
            return fail("cannot read " + path_ + ": " + why);
        }
        json::JsonValue root;
        if (!json::ParseDom(fsutil::StripBom(text), root, &why))
            return fail(path_ + " is not valid JSON: " + why + " (fix or delete the file)");
        if (root.t != json::JsonValue::T::Obj) return fail(path_ + " must contain a JSON object (fix or delete the file)");
        for (const auto& kv : root.obj)
        {
            if (!ValidKey(kv.first)) return fail(path_ + " has an invalid key (empty, over 128 bytes or not UTF-8)");
            std::string value = json::Serialize(kv.second);
            bytes_ += kv.first.size() + value.size() + kEntryOverhead;
            if (bytes_ > maxBytes) return fail(path_ + " holds more than the storage limit of " + std::to_string(maxBytes) + " bytes");
            entries_[kv.first] = std::move(value);
        }
        return true;
    }

    const std::string* Storage::Get(const std::string& key) const
    {
        auto it = entries_.find(key);
        return it == entries_.end() ? nullptr : &it->second;
    }

    bool Storage::Set(const std::string& key, const char* json, size_t n, std::string* err)
    {
        if (!ValidKey(key))
        {
            if (err) *err = "storage key must be 1.." + std::to_string(kMaxKeyBytes) + " bytes of UTF-8";
            return false;
        }
        auto it = entries_.find(key);
        const size_t oldCost = it == entries_.end() ? 0 : key.size() + it->second.size() + kEntryOverhead;
        const size_t newCost = key.size() + n + kEntryOverhead;
        if (bytes_ - oldCost + newCost > maxBytes_)
        {
            if (err) *err = "storage limit of " + std::to_string(maxBytes_) + " bytes exceeded";
            return false;
        }
        if (it == entries_.end())
            entries_.emplace(key, std::string(json, n));
        else
            it->second.assign(json, n);
        bytes_ = bytes_ - oldCost + newCost;
        dirty_ = true;
        return true;
    }

    bool Storage::Delete(const std::string& key)
    {
        auto it = entries_.find(key);
        if (it == entries_.end()) return false;
        bytes_ -= key.size() + it->second.size() + kEntryOverhead;
        entries_.erase(it);
        dirty_ = true;
        return true;
    }

    std::vector<std::string> Storage::Keys() const
    {
        std::vector<std::string> keys;
        keys.reserve(entries_.size());
        for (const auto& kv : entries_) keys.push_back(kv.first);
        return keys;
    }

    bool Storage::Flush(std::string* err)
    {
        if (!dirty_) return true;
        std::string out = "{";
        bool first = true;
        for (const auto& kv : entries_)
        {
            out += first ? "\n  " : ",\n  ";
            first = false;
            out += json::Quote(kv.first);
            out += ": ";
            out += kv.second;
        }
        out += first ? "}\n" : "\n}\n";
        std::string why;
        if (!fsutil::EnsureDir(dir_, &why))
        {
            if (err) *err = why;
            return false;
        }
        if (!writer_(path_, out, &why))
        {
            if (err) *err = why;
            return false;
        }
        dirty_ = false;
        return true;
    }

}
