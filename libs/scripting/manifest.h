// resource.json: parsing and validation of a resource manifest. Pure C++ (no Lua).
//
// Schema (every key optional except name and version; unknown keys are errors, so a typo can
// never be silently ignored):
//   name            string   1..64 of [A-Za-z0-9_-]; must equal the resource's folder name
//   version         string   1..32 printable characters, informational
//   description     string   up to 200 bytes of UTF-8
//   author          string   up to 64 bytes of UTF-8
//   server_scripts  [string] relative .lua paths, executed in this order (default: none)
//   client_files    [string] relative paths reserved for the client engine; validated, not used yet
//   dependencies    [string] resource names that must be running first
//   config          object   defaults for config.get(); any JSON
//
// Thread-safety: re-entrant, no shared state.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace gage::scripting {

    // A validated resource.json.
    struct Manifest
    {
        std::string name;
        std::string version;
        std::string description;
        std::string author;
        std::vector<std::string> serverScripts;
        std::vector<std::string> clientFiles;
        std::vector<std::string> dependencies;
        // config key -> compact JSON text of its value (top-level keys of "config").
        std::map<std::string, std::string> config;
    };

    // Parses and validates `text` (the content of resource.json) for the resource in folder
    // `folderName`. Returns true and fills `out`, or returns false with a one-line reason in
    // *err such as `resource.json: unknown key "server_script" (known: name, version, ...)` or
    // `resource.json: "name" is "foo" but the folder is "bar"`.
    bool ParseManifest(const std::string& text, const std::string& folderName, Manifest& out, std::string* err);

    // Overlays the top-level keys of `text` (a config.json, which must be one JSON object) onto
    // `m.config`, key by key. Returns false with a reason in *err for text that is not a valid
    // JSON object.
    bool ApplyConfigOverride(const std::string& text, Manifest& m, std::string* err);

}
