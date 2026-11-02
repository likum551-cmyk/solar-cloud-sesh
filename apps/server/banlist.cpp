#include "banlist.h"

#include "protocol/protocol.h"
#include "util.h"

// The launcher's header-only JSON reader (libs/common/json.h), as config.cpp uses it.
#include "common/json.h"

namespace gage::server {

    namespace {

        constexpr size_t kMaxEntries = 10000;
        constexpr size_t kMaxReason = proto::kMaxChat; // 200 bytes: what a Refuse text can carry, with room

        bool ValidReason(const std::string& r) { return r.empty() || proto::ValidChat(r); }

    }

    void BanList::Reindex()
    {
        byName_.clear();
        byIp_.clear();
        for (size_t i = 0; i < entries_.size(); i++)
        {
            if (!entries_[i].name.empty()) byName_.emplace(proto::NameKey(entries_[i].name), i);
            if (!entries_[i].ip.empty()) byIp_.emplace(entries_[i].ip, i);
        }
    }

    bool BanList::Load(const std::string& path, std::string* err)
    {
        auto fail = [&](const std::string& why) {
            if (err) *err = path + ": " + why;
            return false;
        };
        if (path.empty())
        {
            path_.clear();
            entries_.clear();
            Reindex();
            return true;
        }

        std::string text, readErr;
        bool missing = false;
        std::vector<BanEntry> loaded;
        if (!ReadWholeFile(path, text, &readErr, &missing))
        {
            if (!missing) return fail("cannot read the ban list (" + readErr + ")");
            // No file yet: an empty list. It is created by the first ban.
        }
        else
        {
            JParser p;
            p.maxNodes = 8 * kMaxEntries + 16;
            if (!p.parse(text.data(), text.size())) return fail("not valid JSON");
            const char* rest = p.p;
            while (rest < p.end && (*rest == ' ' || *rest == '\t' || *rest == '\n' || *rest == '\r')) rest++;
            if (rest != p.end) return fail("trailing data after the JSON object");
            if (p.v.t != JVal::Obj) return fail("top level must be an object");
            for (const auto& top : p.v.o)
            {
                if (top.first != "bans") return fail("unknown key \"" + top.first + "\" (known: bans)");
                if (top.second.t != JVal::Arr) return fail("\"bans\" must be an array");
                if (top.second.a.size() > kMaxEntries) return fail("more than " + std::to_string(kMaxEntries) + " bans");
                for (size_t i = 0; i < top.second.a.size(); i++)
                {
                    const JVal& e = top.second.a[i];
                    const std::string at = "bans[" + std::to_string(i) + "]";
                    if (e.t != JVal::Obj) return fail(at + " must be an object");
                    BanEntry b;
                    for (const auto& kv : e.o)
                    {
                        if (kv.second.t != JVal::Str) return fail(at + "." + kv.first + " must be a string");
                        if (kv.first == "name") b.name = kv.second.s;
                        else if (kv.first == "ip") b.ip = kv.second.s;
                        else if (kv.first == "reason") b.reason = kv.second.s;
                        else if (kv.first == "at") b.at = kv.second.s;
                        else return fail(at + ": unknown key \"" + kv.first + "\" (known: name, ip, reason, at)");
                    }
                    if (b.name.empty() && b.ip.empty()) return fail(at + " needs a \"name\" or an \"ip\"");
                    if (!b.name.empty() && !proto::ValidName(b.name)) return fail(at + ".name is not a valid player name");
                    if (!b.ip.empty() && !ParseIpv4(b.ip)) return fail(at + ".ip is not a dotted IPv4 address");
                    if (b.reason.size() > kMaxReason || !ValidReason(b.reason))
                        return fail(at + ".reason must be valid UTF-8 text without control characters, at most " +
                                    std::to_string(kMaxReason) + " bytes");
                    if (b.at.size() > 40) return fail(at + ".at is too long");
                    loaded.push_back(std::move(b));
                }
            }
        }
        path_ = path;
        entries_ = std::move(loaded);
        Reindex();
        return true;
    }

    std::string BanList::ToJson() const
    {
        std::string out = "{\n  \"bans\": [";
        for (size_t i = 0; i < entries_.size(); i++)
        {
            const BanEntry& e = entries_[i];
            out += i ? ",\n    { " : "\n    { ";
            bool first = true;
            auto field = [&](const char* key, const std::string& v) {
                if (v.empty()) return;
                if (!first) out += ", ";
                first = false;
                out += std::string("\"") + key + "\": \"" + JsonEscape(v) + "\"";
            };
            field("name", e.name);
            field("ip", e.ip);
            field("reason", e.reason);
            field("at", e.at);
            out += " }";
        }
        out += entries_.empty() ? "]\n}\n" : "\n  ]\n}\n";
        return out;
    }

    bool BanList::Save(std::string* err) const
    {
        if (path_.empty()) return true; // in-memory list
        return AtomicWriteFile(path_, ToJson(), err);
    }

    bool BanList::Add(BanEntry entry, std::string* err, std::string* saveErr, bool* already)
    {
        auto fail = [&](const std::string& why) {
            if (err) *err = why;
            return false;
        };
        if (already) *already = false;
        entry.reason = Trim(entry.reason);
        if (entry.name.empty() && entry.ip.empty()) return fail("nothing to ban");
        if (!entry.name.empty() && !proto::ValidName(entry.name)) return fail("not a valid player name: " + entry.name);
        if (!entry.ip.empty() && !ParseIpv4(entry.ip)) return fail("not a dotted IPv4 address: " + entry.ip);
        if (entry.reason.size() > kMaxReason || !ValidReason(entry.reason))
            return fail("reason must be valid UTF-8 text without control characters, at most " +
                        std::to_string(kMaxReason) + " bytes");
        if (entries_.size() >= kMaxEntries) return fail("the ban list is full (" + std::to_string(kMaxEntries) + ")");

        const bool nameKnown = entry.name.empty() || FindName(entry.name);
        const bool ipKnown = entry.ip.empty() || FindIp(entry.ip);
        if (nameKnown && ipKnown)
        {
            if (already) *already = true;
            return fail("already banned");
        }
        // Kept whole even when one half is already banned elsewhere: `unban <name>` then
        // removes everything `ban <name>` added, and `unban <ip>` only this entry.
        if (entry.at.empty()) entry.at = IsoTimestamp();
        entries_.push_back(std::move(entry));
        Reindex();
        std::string se;
        if (!Save(&se) && saveErr) *saveErr = se;
        return true;
    }

    size_t BanList::Remove(const std::string& target, std::string* saveErr)
    {
        const std::string key = proto::NameKey(target);
        size_t removed = 0;
        std::vector<BanEntry> keep;
        for (BanEntry& e : entries_)
        {
            const bool hit = (!e.name.empty() && proto::NameKey(e.name) == key) || (!e.ip.empty() && e.ip == target);
            if (hit)
                removed++;
            else
                keep.push_back(std::move(e));
        }
        if (removed == 0) return 0;
        entries_ = std::move(keep);
        Reindex();
        std::string se;
        if (!Save(&se) && saveErr) *saveErr = se;
        return removed;
    }

    const BanEntry* BanList::FindName(const std::string& name) const
    {
        auto it = byName_.find(proto::NameKey(name));
        return it == byName_.end() ? nullptr : &entries_[it->second];
    }

    const BanEntry* BanList::FindIp(const std::string& ip) const
    {
        auto it = byIp_.find(ip);
        return it == byIp_.end() ? nullptr : &entries_[it->second];
    }

    const BanEntry* BanList::Match(const std::string& name, const std::string& ip) const
    {
        if (!ip.empty())
            if (const BanEntry* e = FindIp(ip)) return e;
        if (!name.empty())
            if (const BanEntry* e = FindName(name)) return e;
        return nullptr;
    }

}
