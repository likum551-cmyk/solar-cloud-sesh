// handling_tune: the runtime half of tools/packs/handling_tune.json, pure (no game, no OS calls) and offline-tested in
// tests/pack_collection_test.cpp.
//
// Loose (OPEN) packs are tuned when they are staged (tools/packs/stage_packs.mjs rewrites their handling.meta). An NG
// pack's dlc.rpf cannot be read outside the game, so stage_packs ships the same rules next to it as gage_tune.json and
// the engine applies them here, to the handling.meta text it reads through the game's own device once the pack is
// mounted (pack_stream.cpp). The tuned <Item type="CHandlingData"> blocks go into one loose override pack, mounted after
// every other pack, whose HANDLING_FILE redefines those handlingNames.
//
//   ParseHandlingTune(json)              the rules: "*" (every item) and per-handlingName (case-insensitive) entries,
//                                        each {"scale": {field: factor}, "set": {field: value}}; keys starting with
//                                        '_' ("_comment", "_why") are ignored
//   TuneHandlingMeta(handling.meta, t)   every CHandlingData item a rule changes, rewritten; an item where a listed field
//                                        is missing or appears more than once is SKIPPED whole (never half-applied)
//   BuildTune{Meta,Setup2,Content}       the override pack's three text files
//
// Same semantics as stage_packs.mjs tuneHandling: "*" first, then the named rule, each applied to the text the previous
// one left (a named "set" overrides a "*" "scale"); a value is written rounded to 6 decimals, shortest form.
// Difference by design: stage_packs throws on a missing field (staging stops, a human fixes the rule); at runtime the
// item is skipped and reported, so one bad rule never takes the other cars' tune with it.
#pragma once

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace gage::game::content {

    struct HandlingRule {
        std::vector<std::pair<std::string, double>> scale;   // field -> factor
        std::vector<std::pair<std::string, double>> set;     // field -> value
    };
    struct HandlingTune {
        bool ok = false;
        std::string error;                                   // why !ok
        bool haveStar = false;
        HandlingRule star;                                   // "*"
        std::vector<std::pair<std::string, HandlingRule>> named;
    };
    struct TunedHandlingItem {
        std::string name;      // <handlingName>
        std::string text;      // the whole <Item type="CHandlingData">...</Item>, tuned
        std::string changes;   // "fTractionCurveMax 2.95->3.3925, ..."
    };
    struct HandlingTuneResult {
        std::vector<TunedHandlingItem> items;   // changed items only, in file order
        std::vector<std::string> skipped;       // "<name>: <reason>"
        int seen = 0;                           // CHandlingData items in the text
    };

    namespace tunedetail {
        // ---- a small JSON reader: just enough for the rules file (objects, arrays, strings, numbers, literals) ----
        struct J {
            const std::string& s;
            size_t i = 0;
            std::string err;
            explicit J(const std::string& src) : s(src) {}
            void Ws() { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; }
            bool Fail(const char* what) { if (err.empty()) err = std::string(what) + " at offset " + std::to_string(i); return false; }
            bool Str(std::string& out) {
                Ws();
                if (i >= s.size() || s[i] != '"') return Fail("expected a string");
                ++i;
                out.clear();
                while (i < s.size() && s[i] != '"') {
                    char c = s[i++];
                    if (c == '\\') {
                        if (i >= s.size()) return Fail("bad escape");
                        const char e = s[i++];
                        if (e == 'u') { if (i + 4 > s.size()) return Fail("bad \\u escape"); i += 4; c = '?'; }
                        else c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e;
                    }
                    out.push_back(c);
                }
                if (i >= s.size()) return Fail("unterminated string");
                ++i;
                return true;
            }
            bool Num(double& out) {
                Ws();
                const char* b = s.c_str() + i;
                char* e = nullptr;
                out = std::strtod(b, &e);
                if (e == b) return Fail("expected a number");
                i += (size_t)(e - b);
                return std::isfinite(out) ? true : Fail("number out of range");
            }
            bool Skip() {   // any value
                Ws();
                if (i >= s.size()) return Fail("unexpected end");
                const char c = s[i];
                if (c == '"') { std::string t; return Str(t); }
                if (c == '{' || c == '[') {
                    const char close = c == '{' ? '}' : ']';
                    ++i;
                    Ws();
                    if (i < s.size() && s[i] == close) { ++i; return true; }
                    while (true) {
                        if (c == '{') { std::string k; if (!Str(k)) return false; Ws(); if (i >= s.size() || s[i++] != ':') return Fail("expected ':'"); }
                        if (!Skip()) return false;
                        Ws();
                        if (i < s.size() && s[i] == ',') { ++i; continue; }
                        if (i < s.size() && s[i] == close) { ++i; return true; }
                        return Fail("expected ',' or a closing bracket");
                    }
                }
                for (const char* lit : { "true", "false", "null" })
                    if (s.compare(i, std::char_traits<char>::length(lit), lit) == 0) { i += std::char_traits<char>::length(lit); return true; }
                double d;
                return Num(d);
            }
            // { "key": <fn(key)>, ... }; fn consumes the value.
            template <class F> bool Obj(F&& fn) {
                Ws();
                if (i >= s.size() || s[i] != '{') return Fail("expected an object");
                ++i;
                Ws();
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                while (true) {
                    std::string k;
                    if (!Str(k)) return false;
                    Ws();
                    if (i >= s.size() || s[i++] != ':') return Fail("expected ':'");
                    if (!fn(k)) return false;
                    Ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == '}') { ++i; return true; }
                    return Fail("expected ',' or '}'");
                }
            }
        };

        inline bool IEq(const std::string& a, const std::string& b) {
            if (a.size() != b.size()) return false;
            for (size_t k = 0; k < a.size(); ++k)
                if (std::tolower((unsigned char)a[k]) != std::tolower((unsigned char)b[k])) return false;
            return true;
        }
        inline std::string TrimWs(const std::string& s) {
            size_t b = 0, e = s.size();
            while (b < e && std::isspace((unsigned char)s[b])) ++b;
            while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
            return s.substr(b, e - b);
        }

        // Rounded to 6 decimals, shortest form ("4.6", "300", "0", "-0.005"), like stage_packs' fmt().
        inline std::string FormatValue(double v) {
            double r = std::round(v * 1e6) / 1e6;
            if (r == 0) r = 0;   // no "-0"
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.6f", r);
            std::string o = buf;
            if (o.find('.') != std::string::npos) {
                while (!o.empty() && o.back() == '0') o.pop_back();
                if (!o.empty() && o.back() == '.') o.pop_back();
            }
            return o;
        }

        // The end (one past "</Item>") of the <Item ...> element opening at `a`, counting nested non-self-closing
        // <Item> elements (SubHandlingData holds <Item type="CCarHandlingData">...</Item> and <Item type="NULL" />).
        // npos when unterminated.
        inline size_t ItemEnd(const std::string& s, size_t a) {
            int depth = 0;
            size_t i = a;
            while (i < s.size()) {
                const size_t lt = s.find('<', i);
                if (lt == std::string::npos) return std::string::npos;
                if (s.compare(lt, 4, "<!--") == 0) {
                    const size_t e = s.find("-->", lt + 4);
                    if (e == std::string::npos) return std::string::npos;
                    i = e + 3;
                    continue;
                }
                const size_t gt = s.find('>', lt);
                if (gt == std::string::npos) return std::string::npos;
                const bool close = s.compare(lt, 7, "</Item>") == 0;
                const bool open = !close && s.compare(lt, 5, "<Item") == 0 && (lt + 5 < s.size()) &&
                                  (s[lt + 5] == '>' || std::isspace((unsigned char)s[lt + 5]) || s[lt + 5] == '/');
                if (open && s[gt - 1] != '/') ++depth;
                if (close && --depth == 0) return gt + 1;
                i = gt + 1;
            }
            return std::string::npos;
        }

        // Positions of every `<field value="` (field followed by whitespace, so fTractionCurveMax never matches
        // fTractionCurveMaxRatio) in the item's own fields: outside its <SubHandlingData>.
        inline std::vector<size_t> FieldHits(const std::string& item, const std::string& field, size_t subA, size_t subB) {
            std::vector<size_t> hits;
            const std::string open = "<" + field;
            for (size_t p = item.find(open); p != std::string::npos; p = item.find(open, p + 1)) {
                if (p >= subA && p < subB) continue;
                size_t q = p + open.size();
                if (q >= item.size() || !std::isspace((unsigned char)item[q])) continue;
                while (q < item.size() && std::isspace((unsigned char)item[q])) ++q;
                if (item.compare(q, 7, "value=\"") != 0) continue;
                hits.push_back(q + 7);   // the first character of the number
            }
            return hits;
        }
    }  // namespace tunedetail

    inline HandlingTune ParseHandlingTune(const std::string& json)
    {
        using namespace tunedetail;
        HandlingTune t;
        J j(json);
        auto fields = [&j](std::vector<std::pair<std::string, double>>& out) {
            return j.Obj([&j, &out](const std::string& f) {
                double v;
                if (!j.Num(v)) return false;
                out.emplace_back(f, v);
                return true;
            });
        };
        const bool ok = j.Obj([&](const std::string& key) {
            if (key.empty() || key[0] == '_') return j.Skip();
            HandlingRule r;
            const bool rok = j.Obj([&](const std::string& k) {
                if (k == "scale") return fields(r.scale);
                if (k == "set") return fields(r.set);
                return j.Skip();   // "_why" and anything else
            });
            if (!rok) return false;
            if (key == "*") { t.haveStar = true; t.star = r; }
            else t.named.emplace_back(key, r);
            return true;
        });
        j.Ws();
        if (ok && j.i != json.size()) j.Fail("trailing text");
        t.ok = ok && j.err.empty();
        t.error = j.err;
        return t;
    }

    inline HandlingTuneResult TuneHandlingMeta(const std::string& xml, const HandlingTune& t)
    {
        using namespace tunedetail;
        HandlingTuneResult res;
        const std::string tag = "<Item type=\"CHandlingData\"";
        for (size_t a = xml.find(tag); a != std::string::npos;) {
            const size_t b = ItemEnd(xml, a);
            if (b == std::string::npos) { res.skipped.push_back("?: an unterminated CHandlingData item"); break; }
            ++res.seen;
            std::string item = xml.substr(a, b - a);
            const size_t next = xml.find(tag, b);

            std::string name;
            {
                const size_t n0 = item.find("<handlingName>");
                const size_t n1 = n0 == std::string::npos ? n0 : item.find("</handlingName>", n0);
                if (n1 != std::string::npos) name = TrimWs(item.substr(n0 + 14, n1 - n0 - 14));
            }
            const HandlingRule* namedRule = nullptr;
            for (const auto& kv : t.named)
                if (IEq(kv.first, name)) { namedRule = &kv.second; break; }
            std::vector<const HandlingRule*> rules;
            if (t.haveStar) rules.push_back(&t.star);
            if (namedRule) rules.push_back(namedRule);

            std::string changes, reason;
            for (const HandlingRule* r : rules) {
                for (int op = 0; op < 2 && reason.empty(); ++op) {
                    for (const auto& fv : (op == 0 ? r->scale : r->set)) {
                        size_t subA = item.find("<SubHandlingData");
                        size_t subB = subA == std::string::npos ? subA : item.find("</SubHandlingData>", subA);
                        if (subA == std::string::npos) subA = subB = std::string::npos;
                        else if (subB == std::string::npos) subB = item.size();
                        const std::vector<size_t> hits = FieldHits(item, fv.first, subA, subB);
                        if (hits.size() != 1) {
                            reason = fv.first + (hits.empty() ? " not found" : " found " + std::to_string(hits.size()) + " times");
                            break;
                        }
                        const size_t p = hits[0];
                        const size_t q = item.find('"', p);
                        if (q == std::string::npos) { reason = fv.first + " has no closing quote"; break; }
                        const std::string old = item.substr(p, q - p);
                        char* end = nullptr;
                        const double v = std::strtod(old.c_str(), &end);
                        if (old.empty() || !end || *end != '\0') { reason = fv.first + " value \"" + old + "\" is not a number"; break; }
                        const std::string nv = FormatValue(op == 0 ? v * fv.second : fv.second);
                        item.replace(p, q - p, nv);
                        if (!changes.empty()) changes += ", ";
                        changes += fv.first + " " + old + "->" + nv;
                    }
                }
                if (!reason.empty()) break;
            }
            if (!reason.empty())
                res.skipped.push_back((name.empty() ? std::string("?") : name) + ": " + reason + " (item left as it is)");
            else if (!changes.empty())
                res.items.push_back({ name, item, changes });
            a = next;
        }
        return res;
    }

    // The override pack's files. Its device is dlc_gagetune, its one change set enables one HANDLING_FILE.
    inline std::string BuildTuneMeta(const std::vector<TunedHandlingItem>& items)
    {
        std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<CHandlingDataMgr>\n  <HandlingData>\n";
        for (const TunedHandlingItem& it : items) o += "    " + it.text + "\n";
        o += "  </HandlingData>\n</CHandlingDataMgr>\n";
        return o;
    }
    inline std::string BuildTuneSetup2()
    {
        return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<SSetupData>\n"
               "  <deviceName>dlc_gagetune</deviceName>\n"
               "  <datFile>content.xml</datFile>\n"
               "  <timeStamp>05/11/2026 12:00:00</timeStamp>\n"
               "  <nameHash>gagetune</nameHash>\n"
               "  <contentChangeSetGroups>\n"
               "    <Item>\n"
               "      <NameHash>GROUP_STARTUP</NameHash>\n"
               "      <ContentChangeSets>\n"
               "        <Item>gagetune_AUTOGEN</Item>\n"
               "      </ContentChangeSets>\n"
               "    </Item>\n"
               "  </contentChangeSetGroups>\n"
               "  <type>EXTRACONTENT_COMPAT_PACK</type>\n"
               "  <order value=\"9\" />\n"
               "</SSetupData>\n";
    }
    inline std::string BuildTuneContent()
    {
        return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<CDataFileMgr__ContentsOfDataFileXml>\n"
               "  <disabledFiles />\n"
               "  <includedXmlFiles />\n"
               "  <includedDataFiles />\n"
               "  <dataFiles>\n"
               "    <Item>\n"
               "      <filename>dlc_gagetune:/data/handling.meta</filename>\n"
               "      <fileType>HANDLING_FILE</fileType>\n"
               "      <overlay value=\"false\" />\n"
               "      <disabled value=\"true\" />\n"
               "      <persistent value=\"false\" />\n"
               "    </Item>\n"
               "  </dataFiles>\n"
               "  <contentChangeSets>\n"
               "    <Item>\n"
               "      <changeSetName>gagetune_AUTOGEN</changeSetName>\n"
               "      <filesToDisable />\n"
               "      <filesToEnable>\n"
               "        <Item>dlc_gagetune:/data/handling.meta</Item>\n"
               "      </filesToEnable>\n"
               "      <txdToLoad />\n"
               "      <txdToUnload />\n"
               "      <residentResources />\n"
               "      <unregisterResources />\n"
               "    </Item>\n"
               "  </contentChangeSets>\n"
               "  <patchFiles />\n"
               "</CDataFileMgr__ContentsOfDataFileXml>\n";
    }

}  // namespace gage::game::content
