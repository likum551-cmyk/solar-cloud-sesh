// character.h (0.5.9 characters, GI 2026-11-07: "go to my old gamemode and find character creation. 1 character min insta creation if
// not created on server join. can buy additional 2 char slots. grab it out fully."). Pure C++17, no game, no network: the rules both the
// server (the authority: apps/server/server.cpp On*Char*, apps/server/character_store.*) and the engine (the creator's preview, the apply)
// use, and the byte codec the protocol frames (proto::CharRoster / CharOp / PeerFace carry these blobs; docs/api/protocol.md "0.5.9").
//
// The old system this ports is docs/porting/character-creation-old-gamemode.md (RedAge / NeptuneEvo on RAGE:MP). Kept: a character is a
// first + last name and a freemode appearance (sex, two parents with shape and skin mix, 20 face features, the head overlays with opacity
// and colours, hair with colour and highlight, eye colour); names follow the old UI's rules; uniqueness is checked by the server. Changed:
//   * slots: 1 per account + up to 2 bought on the platform (kBaseSlots, kMaxExtraSlots; the old game had 2 free + 1 for RedBucks);
//   * a player with no character gets one INSTANTLY on join (DefaultAppearance, DefaultName), editable afterwards in the creator;
//   * every value is validated (Valid*) and clamped (Clamp) here, on the server, where the old server checked only that a name was not empty;
//   * uniqueness is case-insensitive (NameKey; the old check was case-sensitive while its database collation was not);
//   * none of the old bugs (colours written to the wrong overlay ids, eye colour saved as 0, parents stored swapped, gender never stored).
//
// Units on the wire and on disk are integers: mixes and opacities are percent (0..100 for GTA's 0.0..1.0), face features are -100..100 for
// GTA's -1.0..1.0. The overlay ranges are GTA's own value counts per overlay (GET_PED_HEAD_OVERLAY_NUM, alloc8or PED doc); 255 = none.
// tests/character_tests.h pins the rules and the codec.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

namespace gage::sync::chr {

    // ---- slots --------------------------------------------------------------------------------------------------------------------------
    constexpr int kBaseSlots = 1;        // every account (and every guest) has one character
    constexpr int kMaxExtraSlots = 2;    // bought on the platform (`character_slot` entitlement)
    constexpr int kMaxSlots = kBaseSlots + kMaxExtraSlots;

    // The slots an account may use: what the platform said (verify `characterSlots`), held to 1..kMaxSlots. A server that got no number (an
    // older platform) passes 0 and gets kBaseSlots.
    inline int SlotsFor(int platformSlots)
    {
        if (platformSlots < kBaseSlots) return kBaseSlots;
        if (platformSlots > kMaxSlots) return kMaxSlots;
        return platformSlots;
    }

    // ---- names (the old UI's rules: src_cef/src/api/validation.js:45-96) -------------------------------------------------------------------
    constexpr size_t kNameMinLen = 3;
    constexpr size_t kNameMaxLen = 25;
    enum class NamePart : uint8_t { First, Last };
    // 3..25 ASCII letters, the first one upper case; at most 1 capital in a first name, 2 in a last name ("McCry").
    inline bool ValidNamePart(const std::string& s, NamePart part)
    {
        if (s.size() < kNameMinLen || s.size() > kNameMaxLen) return false;
        int caps = 0;
        for (char ch : s)
        {
            const bool up = ch >= 'A' && ch <= 'Z';
            const bool low = ch >= 'a' && ch <= 'z';
            if (!up && !low) return false;
            caps += up ? 1 : 0;
        }
        if (!(s[0] >= 'A' && s[0] <= 'Z')) return false;
        return caps <= (part == NamePart::First ? 1 : 2);
    }
    // Case-insensitive identity of a full name ("john_smith"): two characters may not share it, on the whole server.
    inline std::string NameKey(const std::string& first, const std::string& last)
    {
        std::string k;
        k.reserve(first.size() + last.size() + 1);
        for (char ch : first) k.push_back((ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch);
        k.push_back('_');
        for (char ch : last) k.push_back((ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch);
        return k;
    }

    // ---- appearance -----------------------------------------------------------------------------------------------------------------------
    enum class Sex : uint8_t { Male = 0, Female = 1 };
    constexpr uint32_t kModelMale = 0x705E61F2;     // mp_m_freemode_01
    constexpr uint32_t kModelFemale = 0x9C9EFFD8;   // mp_f_freemode_01
    inline uint32_t ModelOf(Sex s) { return s == Sex::Female ? kModelFemale : kModelMale; }

    constexpr int kParentMax = 45;          // GTA head ids 0..45 (0-20 male, 21-41 female, 42-44 male DLC, 45 female DLC), both pickers
    constexpr int kFeatures = 20;           // SET_PED_FACE_FEATURE index 0..19
    constexpr int kFeatureLimit = 100;      // -100..100 = GTA -1.0..1.0
    constexpr int kOverlays = 13;           // SET_PED_HEAD_OVERLAY overlayID 0..12
    constexpr uint8_t kOverlayNone = 255;
    constexpr int kColorMax = 63;           // hair / makeup palette ids 0..63
    constexpr int kEyeMax = 31;             // SET_HEAD_BLEND_EYE_COLOR 0..31

    // GTA's value count per overlay (the highest index is count - 1).
    constexpr uint8_t kOverlayCount[kOverlays] = {24, 29, 34, 15, 75, 7, 12, 11, 10, 18, 17, 12, 2};
    // What each overlay is (log lines, the creator's labels).
    constexpr const char* kOverlayName[kOverlays] = {"Blemishes", "Facial hair", "Eyebrows",   "Ageing",   "Makeup",
                                                      "Blush",     "Complexion",  "Sun damage", "Lipstick", "Moles / freckles",
                                                      "Chest hair", "Body blemishes", "More body blemishes"};
    // Colour type of an overlay for SET_PED_HEAD_OVERLAY_COLOR: 1 = hair palette (facial hair, eyebrows, chest hair), 2 = makeup palette
    // (blush, lipstick), 0 = none. The old code sent the colours to the wrong overlay ids (porting doc, bug 1); this table is by overlay id.
    inline int OverlayColorType(int overlay)
    {
        if (overlay == 1 || overlay == 2 || overlay == 10) return 1;
        if (overlay == 5 || overlay == 8) return 2;
        return 0;
    }
    // The overlays the creator offers, in its order (the old creator's set, docs/porting section 3): eyebrows, facial hair (men),
    // blemishes, ageing, complexion, sun damage, moles, lipstick (women). The others keep their stored value (255 for a new character).
    constexpr int kCreatorOverlays[] = {2, 1, 0, 3, 6, 7, 9, 8};
    inline bool OverlayForSex(int overlay, Sex s)
    {
        if (overlay == 1 || overlay == 10) return s == Sex::Male;   // facial hair, chest hair
        if (overlay == 8 || overlay == 4 || overlay == 5) return s == Sex::Female;   // lipstick, makeup, blush
        return true;
    }

    // Hair styles (component 2 drawables) the creator offers: the old creator's lists (male 1-22, 24-36; female 1-22, 25-38: 23 / 24 are the
    // night-vision "hair" in each list), plus 0 (bald / the shortest) first.
    inline int HairCount(Sex s) { return s == Sex::Male ? 36 : 37; }
    inline int HairAt(Sex s, int i)
    {
        if (i <= 0) return 0;
        if (i <= 22) return i;
        return s == Sex::Male ? i + 1 : i + 2;   // male 23.. -> 24.., female 23.. -> 25..
    }
    inline int HairIndexOf(Sex s, int drawable)
    {
        for (int i = 0; i < HairCount(s); ++i)
            if (HairAt(s, i) == drawable) return i;
        return -1;
    }
    // The palette the creator offers for hair, eyebrows and beards (the old creator's: ids 0-9, 11, 12, 14-20, 23-27).
    constexpr uint8_t kHairPalette[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 14, 15, 16, 17, 18, 19, 20, 23, 24, 25, 26, 27};
    constexpr int kCreatorEyeMax = 8;   // the old creator offered eye colours 0..8 (the rest are unnatural: white, red, ...)

    struct Overlay
    {
        uint8_t index = kOverlayNone;   // 0..count-1 or kOverlayNone
        uint8_t opacity = 100;          // percent
        uint8_t color = 0, color2 = 0;  // palette ids (used by colour types 1 and 2)
        bool operator==(const Overlay& o) const { return index == o.index && opacity == o.opacity && color == o.color && color2 == o.color2; }
    };

    struct Appearance
    {
        Sex sex = Sex::Male;
        uint8_t father = 0, mother = 21;      // head ids 0..kParentMax; the face shape and skin take the father first (SET_PED_HEAD_BLEND_DATA)
        uint8_t shapeMix = 50, skinMix = 50;  // percent towards the mother
        int8_t features[kFeatures] = {};
        Overlay overlays[kOverlays];
        uint8_t hair = 1;                     // component 2 drawable (HairAt)
        uint8_t hairColor = 0, hairHighlight = 0;
        uint8_t eyeColor = 0;
        bool operator==(const Appearance& o) const
        {
            if (sex != o.sex || father != o.father || mother != o.mother || shapeMix != o.shapeMix || skinMix != o.skinMix || hair != o.hair ||
                hairColor != o.hairColor || hairHighlight != o.hairHighlight || eyeColor != o.eyeColor)
                return false;
            for (int i = 0; i < kFeatures; ++i)
                if (features[i] != o.features[i]) return false;
            for (int i = 0; i < kOverlays; ++i)
                if (!(overlays[i] == o.overlays[i])) return false;
            return true;
        }
        bool operator!=(const Appearance& o) const { return !(*this == o); }
    };

    // Every value in its range: what the server accepts from a client and what Decode accepts from a file. The hair must be one the
    // creator offers for that sex (a drawable the model lacks would show nothing).
    inline bool ValidAppearance(const Appearance& a)
    {
        if ((uint8_t)a.sex > 1 || a.father > kParentMax || a.mother > kParentMax || a.shapeMix > 100 || a.skinMix > 100) return false;
        for (int8_t f : a.features)
            if (f < -kFeatureLimit || f > kFeatureLimit) return false;
        for (int i = 0; i < kOverlays; ++i)
        {
            const Overlay& o = a.overlays[i];
            if (o.index != kOverlayNone && o.index >= kOverlayCount[i]) return false;
            if (o.opacity > 100 || o.color > kColorMax || o.color2 > kColorMax) return false;
        }
        return HairIndexOf(a.sex, a.hair) >= 0 && a.hairColor <= kColorMax && a.hairHighlight <= kColorMax && a.eyeColor <= kEyeMax;
    }

    // Every value pulled into its range (an out-of-range overlay index becomes none, a hair the sex lacks becomes the first style).
    inline Appearance Clamp(Appearance a)
    {
        auto clampI = [](int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; };
        if ((uint8_t)a.sex > 1) a.sex = Sex::Male;
        a.father = (uint8_t)clampI(a.father, 0, kParentMax);
        a.mother = (uint8_t)clampI(a.mother, 0, kParentMax);
        a.shapeMix = (uint8_t)clampI(a.shapeMix, 0, 100);
        a.skinMix = (uint8_t)clampI(a.skinMix, 0, 100);
        for (int8_t& f : a.features) f = (int8_t)clampI(f, -kFeatureLimit, kFeatureLimit);
        for (int i = 0; i < kOverlays; ++i)
        {
            Overlay& o = a.overlays[i];
            if (o.index != kOverlayNone && o.index >= kOverlayCount[i]) o.index = kOverlayNone;
            o.opacity = (uint8_t)clampI(o.opacity, 0, 100);
            o.color = (uint8_t)clampI(o.color, 0, kColorMax);
            o.color2 = (uint8_t)clampI(o.color2, 0, kColorMax);
        }
        if (HairIndexOf(a.sex, a.hair) < 0) a.hair = (uint8_t)HairAt(a.sex, 1);
        a.hairColor = (uint8_t)clampI(a.hairColor, 0, kColorMax);
        a.hairHighlight = (uint8_t)clampI(a.hairHighlight, 0, kColorMax);
        a.eyeColor = (uint8_t)clampI(a.eyeColor, 0, kEyeMax);
        return a;
    }

    // A seed for the defaults from any id (FNV-1a 32 of the account id or the guest's name): the same account always gets the same face.
    inline uint32_t SeedOf(const std::string& s)
    {
        uint32_t h = 2166136261u;
        for (unsigned char c : s)
        {
            h ^= c;
            h *= 16777619u;
        }
        return h;
    }

    // The instant character's look (GI: "a sensible default appearance"): male (the sex the spawn outfit dresses), a father from the male
    // heads and a mother from the female heads, half way between, neutral features, eyebrows and a short hair style in a natural colour,
    // a natural eye colour. Varied by `seed` so two new players do not share a face.
    inline Appearance DefaultAppearance(uint32_t seed)
    {
        Appearance a;
        auto pick = [&seed](uint32_t n) {
            seed = seed * 1103515245u + 12345u;
            return (seed >> 8) % n;
        };
        a.sex = Sex::Male;
        a.father = (uint8_t)pick(21);           // 0..20
        a.mother = (uint8_t)(21 + pick(21));    // 21..41
        a.shapeMix = 50;
        a.skinMix = 50;
        constexpr uint8_t kShortHair[] = {1, 2, 3, 4, 5, 9, 11, 12, 14, 18};   // short male cuts (1 = the Buzzcut the 0.5.x spawn outfit wore)
        a.hair = kShortHair[pick(sizeof(kShortHair))];
        a.hairColor = kHairPalette[pick(12)];   // the darker natural tones at the start of the palette
        a.hairHighlight = a.hairColor;
        a.overlays[2].index = (uint8_t)pick(kOverlayCount[2]);   // eyebrows
        a.overlays[2].opacity = 100;
        a.overlays[2].color = a.hairColor;
        a.eyeColor = (uint8_t)pick(kCreatorEyeMax + 1);
        return a;
    }

    // ---- the natives an appearance becomes (the engine's apply, apps/engine/engine.cpp ApplyFace) --------------------------------------------
    // One plan per appearance, in GTA's units, in the order the engine calls them: the head blend (father = the first parent of shape AND
    // skin, mother the second, the mixes toward the mother; the old code stored the parents swapped: porting doc bug 3), the 20 micro morphs
    // (SET_PED_MICRO_MORPH, -1.0..1.0), the 13 overlays (SET_PED_HEAD_OVERLAY: 255 = none, opacity 0.0..1.0) with a tint for the coloured
    // ones (SET_PED_HEAD_OVERLAY_TINT by overlay id: bug 1), the hair (component 2, texture 0) and its tint (SET_PED_HAIR_TINT), and the eye
    // colour (SET_HEAD_BLEND_EYE_COLOR: bug 2 saved it as 0).
    struct FacePlan
    {
        int shapeFirst = 0, shapeSecond = 0, skinFirst = 0, skinSecond = 0;
        float shapeMix = 0, skinMix = 0;
        float morph[kFeatures] = {};
        struct OverlayCall
        {
            int index = kOverlayNone;
            float opacity = 1.0f;
            int colorType = 0;           // 0 = no tint call
            int color = 0, color2 = 0;
        } overlay[kOverlays];
        int hairDrawable = 0;
        int hairColor = 0, hairHighlight = 0;
        int eyeColor = 0;
    };
    inline FacePlan PlanFace(const Appearance& in)
    {
        const Appearance a = Clamp(in);
        FacePlan p;
        p.shapeFirst = a.father;
        p.shapeSecond = a.mother;
        p.skinFirst = a.father;
        p.skinSecond = a.mother;
        p.shapeMix = a.shapeMix / 100.0f;
        p.skinMix = a.skinMix / 100.0f;
        for (int i = 0; i < kFeatures; ++i) p.morph[i] = a.features[i] / 100.0f;
        for (int i = 0; i < kOverlays; ++i)
        {
            const Overlay& o = a.overlays[i];
            FacePlan::OverlayCall& c = p.overlay[i];
            c.index = o.index;
            c.opacity = o.index == kOverlayNone ? 0.0f : o.opacity / 100.0f;
            c.colorType = OverlayColorType(i);
            c.color = o.color;
            c.color2 = o.color2;
        }
        p.hairDrawable = a.hair;
        p.hairColor = a.hairColor;
        p.hairHighlight = a.hairHighlight;
        p.eyeColor = a.eyeColor;
        return p;
    }

    // ---- a character and an account's roster ---------------------------------------------------------------------------------------------
    constexpr uint8_t kCharCustomised = 0x01;   // saved in the creator at least once: the name is fixed from then on
    constexpr uint8_t kCharFlagsKnown = kCharCustomised;

    struct Character
    {
        std::string first, last;
        Appearance look;
        uint8_t flags = 0;
        std::string Display() const { return first + " " + last; }
        bool operator==(const Character& o) const { return first == o.first && last == o.last && look == o.look && flags == o.flags; }
    };

    struct Roster
    {
        uint8_t slots = kBaseSlots;   // usable slots (SlotsFor); characters above it stay stored but cannot be played (a refunded slot)
        uint8_t active = 0;           // the slot being played
        uint8_t used = 0;             // bit s: slot s holds a character
        Character chars[kMaxSlots];
        bool Has(int s) const { return s >= 0 && s < kMaxSlots && (used >> s) & 1; }
        bool Usable(int s) const { return Has(s) && s < slots; }
        int Count() const { return (used & 1) + ((used >> 1) & 1) + ((used >> 2) & 1); }
        const Character* Active() const { return Usable(active) ? &chars[active] : nullptr; }
        bool operator==(const Roster& o) const
        {
            if (slots != o.slots || active != o.active || used != o.used) return false;
            for (int s = 0; s < kMaxSlots; ++s)
                if (Has(s) && !(chars[s] == o.chars[s])) return false;
            return true;
        }
    };

    // After the slot count changed (a join with another verify answer): the active slot must be a usable one. False when the roster has no
    // usable character at all (the caller creates one instantly in slot 0, which is always usable).
    inline bool Normalise(Roster& r)
    {
        if (r.slots < kBaseSlots) r.slots = kBaseSlots;
        if (r.slots > kMaxSlots) r.slots = kMaxSlots;
        if (r.Usable(r.active)) return true;
        for (int s = 0; s < r.slots; ++s)
            if (r.Has(s))
            {
                r.active = (uint8_t)s;
                return true;
            }
        r.active = 0;
        return false;
    }

    // ---- default names ---------------------------------------------------------------------------------------------------------------------
    struct Name { std::string first, last; };
    namespace detail {
        inline std::string Proper(const std::string& letters)
        {
            std::string s;
            for (size_t i = 0; i < letters.size() && s.size() < kNameMaxLen; ++i)
            {
                char ch = letters[i];
                if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
                s.push_back(i == 0 ? (char)(ch - 'a' + 'A') : ch);
            }
            return s;
        }
        // The letter runs of a name: split at anything that is not a letter and at a lower-to-upper step ("Pia_Player", "PiaPlayer").
        inline int Runs(const std::string& n, std::string out[2])
        {
            int k = 0;
            std::string cur;
            auto flush = [&] {
                if (!cur.empty() && k < 2) out[k++] = cur;
                cur.clear();
            };
            for (size_t i = 0; i < n.size(); ++i)
            {
                const char ch = n[i];
                const bool up = ch >= 'A' && ch <= 'Z', low = ch >= 'a' && ch <= 'z';
                if (!up && !low) { flush(); continue; }
                if (up && !cur.empty() && cur.back() >= 'a' && cur.back() <= 'z') flush();
                cur.push_back(ch);
            }
            flush();
            return k;
        }
        constexpr const char* kFirstNames[] = {"Alex", "Sam", "Jordan", "Casey", "Taylor", "Morgan", "Riley", "Jamie", "Avery", "Quinn",
                                               "Drew", "Reese", "Logan", "Parker", "Rowan", "Blake"};
        constexpr const char* kLastNames[] = {"Walker", "Reed", "Hayes", "Carter", "Brooks", "Parker", "Bennett", "Foster", "Hughes", "Ellis",
                                              "Porter", "Grant", "Mason", "Wells", "Turner", "Russell", "Barnes", "Fisher", "Hunt", "Lane"};
    }

    // The instant character's name from the account (or guest) name: its first two letter runs when both make valid name parts
    // ("Pia_Player" -> Pia Player), else its first run and a surname picked by `seed`, else a first name and a surname picked by `seed`. A
    // taken name (taken(NameKey) true) moves to the next surname, then to the next first name; after every combination, the surname with
    // three more letters; false only when all of those are taken too.
    inline bool DefaultName(const std::string& account, uint32_t seed, const std::function<bool(const std::string&)>& taken, Name& out)
    {
        using namespace detail;
        std::string runs[2];
        const int n = Runs(account, runs);
        const std::string a = n > 0 ? Proper(runs[0]) : std::string(), b = n > 1 ? Proper(runs[1]) : std::string();
        const size_t nf = sizeof(kFirstNames) / sizeof(kFirstNames[0]), nl = sizeof(kLastNames) / sizeof(kLastNames[0]);
        auto ok = [&](const std::string& f, const std::string& l) {
            return ValidNamePart(f, NamePart::First) && ValidNamePart(l, NamePart::Last) && !taken(NameKey(f, l));
        };
        if (ok(a, b)) { out = {a, b}; return true; }
        const bool ownFirst = ValidNamePart(a, NamePart::First);
        for (size_t fi = 0; fi < (ownFirst ? 1 : 0) + nf; ++fi)
        {
            const std::string f = ownFirst && fi == 0 ? a : std::string(kFirstNames[(seed + fi) % nf]);
            for (size_t li = 0; li < nl; ++li)
            {
                const std::string l = kLastNames[(seed / 7 + li) % nl];
                if (ok(f, l)) { out = {f, l}; return true; }
            }
        }
        // Every list combination taken (a server with thousands of characters): the seed's surname with three more letters ("Walkerabc").
        const std::string f = ownFirst ? a : std::string(kFirstNames[seed % nf]);
        const std::string base = kLastNames[(seed / 7) % nl];
        for (uint32_t i = 0; i < 26u * 26u * 26u; ++i)
        {
            std::string l = base;
            l.push_back((char)('a' + (i / 676) % 26));
            l.push_back((char)('a' + (i / 26) % 26));
            l.push_back((char)('a' + i % 26));
            if (ok(f, l)) { out = {f, l}; return true; }
        }
        return false;
    }

    // ---- what a client may ask (CharOp) and the server's answers ---------------------------------------------------------------------------
    enum class Op : uint8_t
    {
        Select = 1,   // play the character in `slot`
        Create = 2,   // a new character in the empty `slot` (names + look)
        Save = 3,     // the creator's result for the character in `slot` (the look; the names too while it is not customised yet)
    };
    constexpr uint8_t kOpLast = (uint8_t)Op::Save;
    enum class Code : uint8_t
    {
        Ok = 0,
        BadSlot = 1,      // no such slot
        Locked = 2,       // the slot is not owned (buy it on the platform)
        SlotUsed = 3,     // Create on a slot that holds a character
        Empty = 4,        // Select / Save on an empty slot
        BadName = 5,      // a name part breaks ValidNamePart
        NameTaken = 6,    // another character has this name
        BadLook = 7,      // an appearance value out of range
        NameFixed = 8,    // a rename of a character that was already customised
        NotNow = 9,       // the player cannot switch now (dead)
        RateLimited = 10,
        Failed = 11,      // the server could not save (the change was not made)
    };
    constexpr uint8_t kCodeLast = (uint8_t)Code::Failed;
    inline const char* ToString(Code c)
    {
        switch (c)
        {
        case Code::Ok: return "ok";
        case Code::BadSlot: return "no such slot";
        case Code::Locked: return "slot not owned";
        case Code::SlotUsed: return "slot already holds a character";
        case Code::Empty: return "slot is empty";
        case Code::BadName: return "invalid name";
        case Code::NameTaken: return "name taken";
        case Code::BadLook: return "appearance out of range";
        case Code::NameFixed: return "name already fixed";
        case Code::NotNow: return "not now";
        case Code::RateLimited: return "too fast";
        case Code::Failed: return "could not save";
        }
        return "?";
    }
    inline const char* ToString(Op o)
    {
        switch (o)
        {
        case Op::Select: return "select";
        case Op::Create: return "create";
        case Op::Save: return "save";
        }
        return "?";
    }

    // The rules of one op on a roster (the server's truth; the client may predict with it). `taken(key)` says whether ANOTHER character owns
    // that name (the server's index; the character's own current name is not "taken" by itself). On Ok the roster is changed.
    inline Code Apply(Roster& r, Op op, int slot, const Character& in, const std::function<bool(const std::string&)>& taken)
    {
        if (slot < 0 || slot >= kMaxSlots) return Code::BadSlot;
        if (slot >= r.slots) return Code::Locked;
        switch (op)
        {
        case Op::Select:
            if (!r.Has(slot)) return Code::Empty;
            r.active = (uint8_t)slot;
            return Code::Ok;
        case Op::Create:
        {
            if (r.Has(slot)) return Code::SlotUsed;
            if (!ValidNamePart(in.first, NamePart::First) || !ValidNamePart(in.last, NamePart::Last)) return Code::BadName;
            if (!ValidAppearance(in.look)) return Code::BadLook;
            if (taken(NameKey(in.first, in.last))) return Code::NameTaken;
            r.chars[slot] = in;
            r.chars[slot].flags = kCharCustomised;   // made in the creator: the name is the player's choice
            r.used |= (uint8_t)(1u << slot);
            return Code::Ok;
        }
        case Op::Save:
        {
            if (!r.Has(slot)) return Code::Empty;
            Character& c = r.chars[slot];
            if (!ValidAppearance(in.look)) return Code::BadLook;
            const bool rename = NameKey(in.first, in.last) != NameKey(c.first, c.last) || in.first != c.first || in.last != c.last;
            if (rename)
            {
                if (c.flags & kCharCustomised) return Code::NameFixed;
                if (!ValidNamePart(in.first, NamePart::First) || !ValidNamePart(in.last, NamePart::Last)) return Code::BadName;
                if (NameKey(in.first, in.last) != NameKey(c.first, c.last) && taken(NameKey(in.first, in.last))) return Code::NameTaken;
                c.first = in.first;
                c.last = in.last;
            }
            c.look = in.look;
            c.flags |= kCharCustomised;
            return Code::Ok;
        }
        }
        return Code::BadSlot;
    }

    // ---- codec (little-endian; version-prefixed) -----------------------------------------------------------------------------------------
    // Appearance: 81 bytes  [sex][father][mother][shapeMix][skinMix][20 x i8 feature][13 x (index, opacity, color, color2)][hair][hairColor]
    //                       [hairHighlight][eyeColor]
    // Character body:       [flags][u8 len][first][u8 len][last][appearance]
    // Character blob:       [kCodecVersion][character body]                           (CharOp Create / Save)
    // Roster blob:          [kCodecVersion][slots][active][used][a character body per set bit of `used`, slot order]   (CharRoster, saves)
    // Face blob:            [kCodecVersion][appearance]                               (PeerFace)
    constexpr uint8_t kCodecVersion = 1;
    constexpr size_t kAppearanceBytes = 5 + kFeatures + 4 * kOverlays + 4;
    constexpr size_t kMaxBlob = 512;   // a full roster is 4 + 3 x (1 + 26 + 26 + 81) = 406

    namespace codec {
        inline void PutLook(std::string& b, const Appearance& a)
        {
            b.push_back((char)a.sex);
            b.push_back((char)a.father);
            b.push_back((char)a.mother);
            b.push_back((char)a.shapeMix);
            b.push_back((char)a.skinMix);
            for (int8_t f : a.features) b.push_back((char)f);
            for (const Overlay& o : a.overlays)
            {
                b.push_back((char)o.index);
                b.push_back((char)o.opacity);
                b.push_back((char)o.color);
                b.push_back((char)o.color2);
            }
            b.push_back((char)a.hair);
            b.push_back((char)a.hairColor);
            b.push_back((char)a.hairHighlight);
            b.push_back((char)a.eyeColor);
        }
        inline void PutBody(std::string& b, const Character& c)
        {
            b.push_back((char)c.flags);
            b.push_back((char)c.first.size());
            b += c.first;
            b.push_back((char)c.last.size());
            b += c.last;
            PutLook(b, c.look);
        }
        struct Reader
        {
            const std::string& s;
            size_t i = 0;
            bool bad = false;
            uint8_t u8()
            {
                if (i >= s.size()) { bad = true; return 0; }
                return (uint8_t)s[i++];
            }
            std::string str()
            {
                const size_t n = u8();
                if (bad || i + n > s.size()) { bad = true; return std::string(); }
                std::string r = s.substr(i, n);
                i += n;
                return r;
            }
        };
        inline bool GetLook(Reader& r, Appearance& a)
        {
            const uint8_t sex = r.u8();
            if (sex > 1) return false;
            a.sex = (Sex)sex;
            a.father = r.u8();
            a.mother = r.u8();
            a.shapeMix = r.u8();
            a.skinMix = r.u8();
            for (int8_t& f : a.features) f = (int8_t)r.u8();
            for (Overlay& o : a.overlays)
            {
                o.index = r.u8();
                o.opacity = r.u8();
                o.color = r.u8();
                o.color2 = r.u8();
            }
            a.hair = r.u8();
            a.hairColor = r.u8();
            a.hairHighlight = r.u8();
            a.eyeColor = r.u8();
            return !r.bad && ValidAppearance(a);
        }
        // A stored or sent character: names may be anything ValidNamePart allows (an instant character's name is generated by the same rules).
        inline bool GetBody(Reader& r, Character& c)
        {
            c.flags = r.u8();
            c.first = r.str();
            c.last = r.str();
            if (r.bad || (c.flags & ~kCharFlagsKnown)) return false;
            if (!ValidNamePart(c.first, NamePart::First) || !ValidNamePart(c.last, NamePart::Last)) return false;
            return GetLook(r, c.look);
        }
    }

    inline std::string EncodeLook(const Appearance& a)
    {
        std::string b;
        b.push_back((char)kCodecVersion);
        codec::PutLook(b, a);
        return b;
    }
    inline bool DecodeLook(const std::string& bytes, Appearance& out)
    {
        codec::Reader r{bytes};
        if (r.u8() != kCodecVersion) return false;
        Appearance a;
        if (!codec::GetLook(r, a) || r.i != bytes.size()) return false;
        out = a;
        return true;
    }
    inline std::string EncodeCharacter(const Character& c)
    {
        std::string b;
        b.push_back((char)kCodecVersion);
        codec::PutBody(b, c);
        return b;
    }
    // A client's character (CharOp): the names are checked by Apply (a bad name is BadName, not a malformed message), so this decodes any
    // name of printable letters up to kNameMaxLen; the look must be in range.
    inline bool DecodeCharacter(const std::string& bytes, Character& out)
    {
        codec::Reader r{bytes};
        if (r.u8() != kCodecVersion) return false;
        Character c;
        c.flags = r.u8();
        c.first = r.str();
        c.last = r.str();
        if (r.bad || c.first.size() > kNameMaxLen || c.last.size() > kNameMaxLen) return false;
        if (!codec::GetLook(r, c.look) || r.i != bytes.size()) return false;
        out = c;
        return true;
    }
    inline std::string EncodeRoster(const Roster& ro)
    {
        std::string b;
        b.push_back((char)kCodecVersion);
        b.push_back((char)ro.slots);
        b.push_back((char)ro.active);
        b.push_back((char)ro.used);
        for (int s = 0; s < kMaxSlots; ++s)
            if (ro.Has(s)) codec::PutBody(b, ro.chars[s]);
        return b;
    }
    inline bool DecodeRoster(const std::string& bytes, Roster& out)
    {
        codec::Reader r{bytes};
        if (r.u8() != kCodecVersion) return false;
        Roster ro;
        ro.slots = r.u8();
        ro.active = r.u8();
        ro.used = r.u8();
        if (r.bad || ro.slots < kBaseSlots || ro.slots > kMaxSlots || ro.active >= kMaxSlots || (ro.used & ~((1u << kMaxSlots) - 1))) return false;
        for (int s = 0; s < kMaxSlots; ++s)
            if (ro.Has(s) && !codec::GetBody(r, ro.chars[s])) return false;
        if (r.i != bytes.size()) return false;
        out = ro;
        return true;
    }

} // namespace gage::sync::chr
