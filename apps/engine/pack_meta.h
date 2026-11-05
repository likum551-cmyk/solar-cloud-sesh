// pack_meta: the text lookups the content-pack mount needs, pure (no game, no OS calls) and offline-tested in
// tests/pack_collection_test.cpp:
//   MetaFilesOfType(content.xml, "VEHICLE_METADATA_FILE")  the dataFiles entries of one type, as written
//                                                          ("dlc_rs6:/common/data/vehicles.meta")
//   ParseVehiclesMeta(vehicles.meta)                       every vehicle the meta defines: model / make / label / class
// and the overlay car browser's text rules (CarDisplayName, CarClassName, MatchesAllTerms).
// Both are tolerant scanners, not XML parsers: add-on packs come from many tools (comments, odd whitespace, attribute
// order), and a field the scan cannot find is left empty rather than failing the whole pack.
#pragma once

#include <cctype>
#include <string>
#include <vector>

namespace gage::game::content {

    struct MetaVehicle {
        std::string model;   // <modelName>: the /car name
        std::string make;    // <vehicleMakeName> (a text label in the game; usually the brand written out)
        std::string label;   // <gameName> (a text label; packs usually put the car's name here)
        std::string vclass;  // <vehicleClass>, e.g. VC_SUPER
    };

    namespace metadetail {
        inline std::string StripComments(const std::string& s)
        {
            std::string out;
            out.reserve(s.size());
            size_t i = 0;
            while (i < s.size()) {
                const size_t c = s.find("<!--", i);
                if (c == std::string::npos) { out.append(s, i, std::string::npos); break; }
                out.append(s, i, c - i);
                const size_t e = s.find("-->", c + 4);
                if (e == std::string::npos) break;          // an unterminated comment hides the rest
                i = e + 3;
            }
            return out;
        }
        inline std::string Trim(const std::string& s)
        {
            size_t b = 0, e = s.size();
            while (b < e && std::isspace((unsigned char)s[b])) ++b;
            while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
            return s.substr(b, e - b);
        }
        // The text of the first <tag>...</tag> in [from, to), trimmed; "" when absent.
        inline std::string Field(const std::string& s, size_t from, size_t to, const char* tag)
        {
            const std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
            const size_t a = s.find(open, from);
            if (a == std::string::npos || a >= to) return "";
            const size_t b = s.find(close, a + open.size());
            if (b == std::string::npos || b > to) return "";
            return Trim(s.substr(a + open.size(), b - a - open.size()));
        }
        inline bool IEquals(const std::string& a, const char* b)
        {
            size_t i = 0;
            for (; i < a.size() && b[i]; ++i)
                if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
            return i == a.size() && !b[i];
        }
        inline bool ValidModelName(const std::string& m)
        {
            if (m.empty() || m.size() > 63) return false;
            for (char c : m)
                if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-')) return false;
            return true;
        }
    }  // namespace metadetail

    // The <filename> of every <dataFiles> item whose <fileType> equals `fileType` (case-insensitive), in file order.
    inline std::vector<std::string> MetaFilesOfType(const std::string& contentXml, const char* fileType)
    {
        using namespace metadetail;
        std::vector<std::string> out;
        const std::string s = StripComments(contentXml);
        const size_t d0 = s.find("<dataFiles>");
        if (d0 == std::string::npos) return out;
        size_t d1 = s.find("</dataFiles>", d0);
        if (d1 == std::string::npos) d1 = s.size();
        size_t i = d0;
        while (true) {
            const size_t a = s.find("<Item", i);
            if (a == std::string::npos || a >= d1) break;
            size_t b = s.find("</Item>", a);
            if (b == std::string::npos || b > d1) b = d1;
            const std::string type = Field(s, a, b, "fileType");
            const std::string name = Field(s, a, b, "filename");
            if (!name.empty() && IEquals(type, fileType)) out.push_back(name);
            i = b + 1;
        }
        return out;
    }

    // Every vehicle in a vehicles.meta. modelName is the first field of a CVehicleModelInfo__InitData item, so each
    // vehicle's fields are read from its <modelName> up to the next one. Invalid or duplicate model names are skipped.
    inline std::vector<MetaVehicle> ParseVehiclesMeta(const std::string& xml)
    {
        using namespace metadetail;
        std::vector<MetaVehicle> out;
        const std::string s = StripComments(xml);
        const std::string tag = "<modelName>";
        size_t a = s.find(tag);
        while (a != std::string::npos) {
            size_t next = s.find(tag, a + tag.size());
            const size_t end = next == std::string::npos ? s.size() : next;
            MetaVehicle v;
            v.model = Field(s, a, end, "modelName");
            v.make = Field(s, a, end, "vehicleMakeName");
            v.label = Field(s, a, end, "gameName");
            v.vclass = Field(s, a, end, "vehicleClass");
            bool dup = false;
            for (const MetaVehicle& o : out)
                if (IEquals(o.model, v.model.c_str())) dup = true;
            if (ValidModelName(v.model) && !dup) out.push_back(v);
            a = next;
        }
        return out;
    }

    // "dlc_rs6:/common/data/vehicles.meta" -> "common/data/vehicles.meta" (the part after the device's ":/");
    // "%PLATFORM%" -> "x64". A name without a device prefix is returned as is.
    inline std::string PathInPack(const std::string& name)
    {
        std::string p = name;
        const size_t c = p.find(":/");
        if (c != std::string::npos) p = p.substr(c + 2);
        const std::string plat = "%PLATFORM%";
        for (size_t k = p.find(plat); k != std::string::npos; k = p.find(plat))
            p.replace(k, plat.size(), "x64");
        return p;
    }

    // ---- the car browser (overlay Tab menu, "Cars") --------------------------------------------------------------
    namespace metadetail {
        // Lower-case letters and digits only: "BMW M5" and "BMWM5" squash to the same "bmwm5".
        inline std::string Squash(const std::string& s)
        {
            std::string o;
            for (char c : s)
                if (std::isalnum((unsigned char)c)) o.push_back((char)std::tolower((unsigned char)c));
            return o;
        }
        // A text label the game itself would show as missing.
        inline bool NoText(const std::string& s)
        {
            return s.empty() || IEquals(s, "NULL") || IEquals(s, "CARNOTFOUND");
        }
        // Words written all in lower case get a capital ("jesko" -> "Jesko"); anything with a capital is kept.
        inline std::string CapitalizeLowerWords(std::string s)
        {
            for (size_t i = 0; i < s.size();) {
                size_t e = i;
                while (e < s.size() && s[e] != ' ') ++e;
                bool anyUpper = false;
                for (size_t k = i; k < e; ++k)
                    if (std::isupper((unsigned char)s[k])) anyUpper = true;
                if (!anyUpper && e > i && std::islower((unsigned char)s[i])) s[i] = (char)std::toupper((unsigned char)s[i]);
                i = e + 1;
            }
            return s;
        }
        inline int Spaces(const std::string& s)
        {
            int n = 0;
            for (char c : s) n += c == ' ';
            return n;
        }
    }  // namespace metadetail

    // The name the car browser shows. Packs fill <vehicleMakeName> / <gameName> loosely ("jesko" / "jesko",
    // "BMW M5" / "BMWM5", "AUDI" / "RS6", "Lamborghini" / "Diablo SV"), so repeats are folded:
    //   both missing           -> the model name
    //   one missing            -> the other (a lone make is followed by the model: "AUDI rs6")
    //   the same once squashed -> the one written with more spaces ("BMW M5")
    //   label starts with make -> the label ("BMW M3")
    //   otherwise              -> "<make> <label>" ("AUDI RS6", "Lamborghini Diablo SV")
    // then an all-lower-case word gets a capital ("Jesko").
    inline std::string CarDisplayName(const std::string& make, const std::string& label, const std::string& model)
    {
        using namespace metadetail;
        const std::string mk = NoText(make) ? "" : Trim(make), lb = NoText(label) ? "" : Trim(label);
        std::string out;
        if (mk.empty() && lb.empty()) out = model;
        else if (lb.empty()) out = mk + " " + model;
        else if (mk.empty()) out = lb;
        else {
            const std::string sm = Squash(mk), sl = Squash(lb);
            if (sm == sl) out = Spaces(mk) > Spaces(lb) ? mk : lb;
            else if (!sm.empty() && sl.compare(0, sm.size(), sm) == 0) out = lb;
            else out = mk + " " + lb;
        }
        return CapitalizeLowerWords(out);
    }

    // "VC_SUPER" -> "Super", "VC_SPORT_CLASSIC" -> "Sport classic"; "" stays "".
    inline std::string CarClassName(const std::string& vclass)
    {
        std::string s = vclass;
        if (s.size() > 3 && (s[0] == 'V' || s[0] == 'v') && (s[1] == 'C' || s[1] == 'c') && s[2] == '_') s = s.substr(3);
        for (char& c : s) c = c == '_' ? ' ' : (char)std::tolower((unsigned char)c);
        if (!s.empty()) s[0] = (char)std::toupper((unsigned char)s[0]);
        return s;
    }

    // The search: every whitespace-separated term of `query` (any case) is a substring of `haystackLower` (already
    // lower case). An empty or blank query matches everything.
    inline bool MatchesAllTerms(const char* query, const std::string& haystackLower)
    {
        std::string term;
        for (const char* p = query ? query : "";; ++p) {
            if (*p && !std::isspace((unsigned char)*p)) {
                term.push_back((char)std::tolower((unsigned char)*p));
                continue;
            }
            if (!term.empty() && haystackLower.find(term) == std::string::npos) return false;
            term.clear();
            if (!*p) return true;
        }
    }

}  // namespace gage::game::content
