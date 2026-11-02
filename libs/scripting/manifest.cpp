#include "manifest.h"

#include "fsutil.h"
#include "json.h"

#include <set>

namespace gage::scripting {

    namespace {

        using json::JsonValue;

        bool EndsWith(const std::string& s, const char* suffix)
        {
            const std::string x(suffix);
            return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
        }

        bool Printable(const std::string& s)
        {
            for (unsigned char c : s)
                if (c < 0x20 || c >= 0x7F) return false;
            return true;
        }

        // A list of strings under `key`; false (with *err) on a type error.
        bool StringList(const JsonValue& root, const char* key, size_t maxItems, std::vector<std::string>& out, std::string* err)
        {
            const JsonValue* v = root.Find(key);
            if (!v) return true;
            if (v->t != JsonValue::T::Arr)
            {
                *err = std::string("resource.json: \"") + key + "\" must be an array of strings";
                return false;
            }
            if (v->arr.size() > maxItems)
            {
                *err = std::string("resource.json: \"") + key + "\" has more than " + std::to_string(maxItems) + " entries";
                return false;
            }
            std::set<std::string> seen;
            for (const JsonValue& e : v->arr)
            {
                if (e.t != JsonValue::T::Str)
                {
                    *err = std::string("resource.json: \"") + key + "\" must be an array of strings";
                    return false;
                }
                if (!seen.insert(e.s).second)
                {
                    *err = std::string("resource.json: \"") + key + "\" lists \"" + e.s + "\" twice";
                    return false;
                }
                out.push_back(e.s);
            }
            return true;
        }

        bool OptString(const JsonValue& root, const char* key, size_t maxBytes, std::string& out, std::string* err)
        {
            const JsonValue* v = root.Find(key);
            if (!v) return true;
            if (v->t != JsonValue::T::Str || v->s.size() > maxBytes)
            {
                *err = std::string("resource.json: \"") + key + "\" must be a string of at most " + std::to_string(maxBytes) + " bytes";
                return false;
            }
            for (unsigned char c : v->s)
                if (c < 0x20 || c == 0x7F)
                {
                    *err = std::string("resource.json: \"") + key + "\" must not contain control characters";
                    return false;
                }
            out = v->s;
            return true;
        }

    }

    bool ParseManifest(const std::string& rawText, const std::string& folderName, Manifest& out, std::string* errOut)
    {
        std::string local;
        std::string* err = errOut ? errOut : &local;
        out = Manifest();
        JsonValue root;
        std::string why;
        if (!json::ParseDom(fsutil::StripBom(rawText), root, &why))
        {
            *err = "resource.json is not valid JSON: " + why;
            return false;
        }
        if (root.t != JsonValue::T::Obj)
        {
            *err = "resource.json must contain a JSON object";
            return false;
        }
        static const char* const kKnown[] = {"name", "version", "description", "author", "server_scripts",
                                             "client_files", "dependencies", "config"};
        for (const auto& kv : root.obj)
        {
            bool known = false;
            for (const char* k : kKnown)
                if (kv.first == k) known = true;
            if (!known)
            {
                std::string list;
                for (const char* k : kKnown) list += (list.empty() ? "" : ", ") + std::string(k);
                *err = "resource.json: unknown key \"" + kv.first + "\" (known: " + list + ")";
                return false;
            }
        }

        const JsonValue* name = root.Find("name");
        if (!name || name->t != JsonValue::T::Str)
        {
            *err = "resource.json: \"name\" is required and must be a string";
            return false;
        }
        if (!fsutil::ValidResourceName(name->s))
        {
            *err = "resource.json: \"name\" must be 1..64 of A-Z a-z 0-9 _ - (and not a device name like con or nul)";
            return false;
        }
        if (name->s != folderName)
        {
            *err = "resource.json: \"name\" is \"" + name->s + "\" but the folder is \"" + folderName + "\"";
            return false;
        }
        out.name = name->s;

        const JsonValue* version = root.Find("version");
        if (!version || version->t != JsonValue::T::Str || version->s.empty() || version->s.size() > 32 || !Printable(version->s))
        {
            *err = "resource.json: \"version\" is required: 1..32 printable ASCII characters, e.g. \"1.0.0\"";
            return false;
        }
        out.version = version->s;

        if (!OptString(root, "description", 200, out.description, err)) return false;
        if (!OptString(root, "author", 64, out.author, err)) return false;
        if (!StringList(root, "server_scripts", 64, out.serverScripts, err)) return false;
        if (!StringList(root, "client_files", 256, out.clientFiles, err)) return false;
        if (!StringList(root, "dependencies", 32, out.dependencies, err)) return false;

        for (const std::string& s : out.serverScripts)
        {
            if (!fsutil::IsSafeRelPath(s) || !EndsWith(s, ".lua"))
            {
                *err = "resource.json: server_scripts entry \"" + s + "\" must be a relative path ending in .lua with no '..' or absolute parts";
                return false;
            }
        }
        for (const std::string& s : out.clientFiles)
        {
            if (!fsutil::IsSafeRelPath(s))
            {
                *err = "resource.json: client_files entry \"" + s + "\" must be a relative path with no '..' or absolute parts";
                return false;
            }
        }
        for (const std::string& d : out.dependencies)
        {
            if (!fsutil::ValidResourceName(d))
            {
                *err = "resource.json: dependency \"" + d + "\" is not a valid resource name";
                return false;
            }
            if (d == out.name)
            {
                *err = "resource.json: a resource cannot depend on itself";
                return false;
            }
        }

        if (const JsonValue* cfg = root.Find("config"))
        {
            if (cfg->t != JsonValue::T::Obj)
            {
                *err = "resource.json: \"config\" must be an object";
                return false;
            }
            for (const auto& kv : cfg->obj) out.config[kv.first] = json::Serialize(kv.second);
        }
        return true;
    }

    bool ApplyConfigOverride(const std::string& text, Manifest& m, std::string* errOut)
    {
        std::string local;
        std::string* err = errOut ? errOut : &local;
        json::JsonValue root;
        std::string why;
        if (!json::ParseDom(fsutil::StripBom(text), root, &why))
        {
            *err = "config.json is not valid JSON: " + why;
            return false;
        }
        if (root.t != json::JsonValue::T::Obj)
        {
            *err = "config.json must contain a JSON object";
            return false;
        }
        for (const auto& kv : root.obj) m.config[kv.first] = json::Serialize(kv.second);
        return true;
    }

}
