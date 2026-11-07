// char_screens.h (0.5.9 stage 4, GI 2026-11-07): the rules of the character SELECT screen and the CREATOR, pure C++17 (no ImGui, no game),
// so tests/char_screens_tests.h can pin them. The overlay (apps/engine/overlay_inventory.cpp, the CHARACTERS page of the Tab menu) draws what
// these say; the server (libs/sync/character.h Apply) stays the authority: everything here only decides what the screen OFFERS, and the
// answer to every op still comes back as a CharResult.
//
// GI's decisions (2026-11-07): slots 1..3, a locked slot says it is bought in the Solar RP shop; create in a free slot; switch; DELETE from the
// select screen, confirmed by typing the character's name, never the last character (nor the one being played: switch first); the NAME is
// free to edit in the creator until the character is first saved there (kCharCustomised), then locked (an admin can rename: console /
// /charname). The sex is chosen when the character is created and is fixed afterwards (the old creator did the same).
#pragma once

#include "sync/character.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace gage::sync::chr::screens {

    // ---- the select screen ------------------------------------------------------------------------------------------------------------------
    enum class Card : uint8_t {
        Playing,   // the active character
        Ready,     // a character in a usable slot, not played now
        Free,      // a usable slot without a character: CREATE
        Locked,    // a slot past the account's slots: bought in the Solar RP shop
        Stored,    // a character kept in a slot the account no longer has (a refunded slot): shown, not playable
    };
    inline const char* ToString(Card c)
    {
        switch (c) {
        case Card::Playing: return "playing";
        case Card::Ready: return "ready";
        case Card::Free: return "free";
        case Card::Locked: return "locked";
        case Card::Stored: return "stored";
        }
        return "?";
    }
    inline Card CardOf(const Roster& r, int slot)
    {
        const bool usable = slot >= 0 && slot < (int)r.slots;
        const bool has = slot >= 0 && slot < kMaxSlots && r.Has(slot);
        if (!usable) return has ? Card::Stored : Card::Locked;
        if (!has) return Card::Free;
        return slot == (int)r.active ? Card::Playing : Card::Ready;
    }

    // What a card offers (bits). The server refuses the same things (Apply): Playing / LastOne / Locked / Empty / SlotUsed.
    constexpr uint8_t kActPlay = 1, kActEdit = 2, kActDelete = 4, kActCreate = 8, kActShop = 16;
    inline uint8_t ActionsOf(const Roster& r, int slot)
    {
        switch (CardOf(r, slot)) {
        case Card::Playing: return kActEdit;
        case Card::Ready: return (uint8_t)(kActPlay | kActEdit | (r.Count() > 1 ? kActDelete : 0));
        case Card::Free: return kActCreate;
        case Card::Locked: return kActShop;
        case Card::Stored: return kActShop;
        }
        return 0;
    }
    // The line under a card that cannot be played.
    inline const char* CardNote(Card c)
    {
        switch (c) {
        case Card::Locked: return "Buy a character slot in the Solar RP shop.";
        case Card::Stored: return "Kept. Buy the slot again in the Solar RP shop to play it.";
        case Card::Free: return "An empty slot. Create a character here.";
        case Card::Playing: return "Playing now.";
        case Card::Ready: return "Ready to play.";
        }
        return "";
    }

    // The delete dialog's DELETE button: lit only when the typed text is the character's full name (case does not matter; the server checks
    // the same with ParseConfirm + NameKey).
    inline bool ConfirmMatches(const std::string& typed, const Character& c)
    {
        Name n;
        return ParseConfirm(typed, n) && NameKey(n.first, n.last) == NameKey(c.first, c.last);
    }

    // ---- the creator ------------------------------------------------------------------------------------------------------------------------
    enum class Mode : uint8_t { Create, Edit };
    struct Draft
    {
        Mode mode = Mode::Edit;
        int slot = -1;
        Character ch;
        bool nameEditable = false;   // Create, or Edit of a character never saved in the creator
        bool sexEditable = false;    // Create only
        uint32_t rev = 0;            // +1 per change (the preview re-dresses on a change)
    };

    // The sex of a draft changes: the hair keeps its place in the list (or the last style), the overlays the new sex does not offer go off.
    inline void SetSex(Appearance& a, Sex s)
    {
        if (a.sex == s) return;
        int idx = HairIndexOf(a.sex, a.hair);
        a.sex = s;
        if (idx < 0) idx = 1;
        if (idx >= HairCount(s)) idx = HairCount(s) - 1;
        a.hair = (uint8_t)HairAt(s, idx);
        for (int o = 0; o < kOverlays; ++o)
            if (!OverlayForSex(o, s)) a.overlays[o].index = kOverlayNone;
    }

    // A new character in a FREE slot: the seeded default face (DefaultAppearance) of the chosen sex, the names empty for the player to type.
    inline bool BeginCreate(const Roster& r, int slot, Sex sex, uint32_t seed, Draft& out)
    {
        if (CardOf(r, slot) != Card::Free) return false;
        out = Draft();
        out.mode = Mode::Create;
        out.slot = slot;
        out.ch.look = DefaultAppearance(seed);
        SetSex(out.ch.look, sex);
        out.nameEditable = true;
        out.sexEditable = true;
        return true;
    }
    // An existing character in a usable slot. The name is editable only while it was never saved in the creator (the instant character).
    inline bool BeginEdit(const Roster& r, int slot, Draft& out)
    {
        const Card c = CardOf(r, slot);
        if (c != Card::Playing && c != Card::Ready) return false;
        out = Draft();
        out.mode = Mode::Edit;
        out.slot = slot;
        out.ch = r.chars[slot];
        out.nameEditable = (out.ch.flags & kCharCustomised) == 0;
        out.sexEditable = false;
        return true;
    }

    // What the SAVE / CREATE button would get from the server, checked here first (the button is dimmed and the reason shown): the names
    // (only when editable: a locked name is the server's), the look. NameTaken / Locked / SlotUsed only the server can tell.
    inline Code Check(const Draft& d)
    {
        if (d.slot < 0 || d.slot >= kMaxSlots) return Code::BadSlot;
        if (d.nameEditable && (!ValidNamePart(d.ch.first, NamePart::First) || !ValidNamePart(d.ch.last, NamePart::Last))) return Code::BadName;
        if (!ValidAppearance(d.ch.look)) return Code::BadLook;
        return Code::Ok;
    }
    inline Op OpOf(const Draft& d) { return d.mode == Mode::Create ? Op::Create : Op::Save; }

    // A typed character for a name field: ASCII letters only, at most kNameMaxLen; the first letter is made upper case (the rule needs it).
    inline bool TypeNameChar(std::string& s, unsigned c)
    {
        const bool up = c >= 'A' && c <= 'Z', low = c >= 'a' && c <= 'z';
        if ((!up && !low) || s.size() >= kNameMaxLen) return false;
        char ch = (char)c;
        if (s.empty() && low) ch = (char)(ch - 'a' + 'A');
        s.push_back(ch);
        return true;
    }
    // Why a name part is not accepted yet, for the line under the field ("" = fine).
    inline const char* NameHint(const std::string& s, NamePart part)
    {
        if (s.size() < kNameMinLen) return "3 to 25 letters";
        if (ValidNamePart(s, part)) return "";
        return part == NamePart::First ? "Letters only, one capital" : "Letters only, at most two capitals";
    }

    // ---- the creator's rows -----------------------------------------------------------------------------------------------------------------
    enum class Section : uint8_t { Heritage, Features, Looks, Hair };
    constexpr int kSections = 4;
    inline const char* ToString(Section s)
    {
        switch (s) {
        case Section::Heritage: return "HERITAGE";
        case Section::Features: return "FEATURES";
        case Section::Looks: return "APPEARANCE";
        case Section::Hair: return "HAIR & EYES";
        }
        return "?";
    }
    enum class Field : uint8_t { Sex, Father, Mother, ShapeMix, SkinMix, Feature, OverlayIndex, OverlayOpacity, OverlayColor, Hair, HairColor, HairHighlight, Eye };
    struct Row
    {
        Field f = Field::Sex;
        int sub = 0;   // the feature index, or the overlay id
    };
    constexpr int kMaxRows = 32;

    // GTA's 20 face features (SET_PED_MICRO_MORPH index order, alloc8or PED doc).
    constexpr const char* kFeatureName[kFeatures] = {
        "Nose width",     "Nose peak height", "Nose peak length", "Nose bone height", "Nose peak lowering", "Nose bone twist", "Brow height",
        "Brow depth",     "Cheekbone height", "Cheekbone width",  "Cheek width",      "Eye opening",        "Lip thickness",   "Jaw width",
        "Jaw depth",      "Chin height",      "Chin depth",       "Chin width",       "Chin hole",          "Neck thickness"};

    // The parent heads each picker offers: GTA's male heads for the father (0-20, 42-44), its female heads for the mother (21-41, 45).
    inline int ParentCount(bool father) { return father ? 24 : 22; }
    inline int ParentAt(bool father, int i)
    {
        if (father) return i <= 20 ? i : 42 + (i - 21);
        return i <= 20 ? 21 + i : 45;
    }
    inline int ParentIndexOf(bool father, int id)
    {
        for (int i = 0; i < ParentCount(father); ++i)
            if (ParentAt(father, i) == id) return i;
        return -1;
    }

    // The rows of one section for this draft, in drawing order. An overlay set to none shows only its style row.
    inline int Rows(Section s, const Draft& d, Row* out, int cap)
    {
        int n = 0;
        auto add = [&](Field f, int sub) { if (n < cap) out[n++] = Row{f, sub}; };
        const Appearance& a = d.ch.look;
        switch (s) {
        case Section::Heritage:
            if (d.sexEditable) add(Field::Sex, 0);
            add(Field::Father, 0);
            add(Field::Mother, 0);
            add(Field::ShapeMix, 0);
            add(Field::SkinMix, 0);
            break;
        case Section::Features:
            for (int i = 0; i < kFeatures; ++i) add(Field::Feature, i);
            break;
        case Section::Looks:
            for (int o : kCreatorOverlays) {
                if (!OverlayForSex(o, a.sex)) continue;
                add(Field::OverlayIndex, o);
                if (a.overlays[o].index == kOverlayNone) continue;
                add(Field::OverlayOpacity, o);
                if (OverlayColorType(o) != 0) add(Field::OverlayColor, o);
            }
            break;
        case Section::Hair:
            add(Field::Hair, 0);
            add(Field::HairColor, 0);
            add(Field::HairHighlight, 0);
            add(Field::Eye, 0);
            break;
        }
        return n;
    }

    inline std::string Label(const Row& r)
    {
        switch (r.f) {
        case Field::Sex: return "Sex";
        case Field::Father: return "Father";
        case Field::Mother: return "Mother";
        case Field::ShapeMix: return "Face resemblance";
        case Field::SkinMix: return "Skin tone";
        case Field::Feature: return r.sub >= 0 && r.sub < kFeatures ? kFeatureName[r.sub] : "?";
        case Field::OverlayIndex: return r.sub >= 0 && r.sub < kOverlays ? kOverlayName[r.sub] : "?";
        case Field::OverlayOpacity: return "  Opacity";
        case Field::OverlayColor: return "  Colour";
        case Field::Hair: return "Hair style";
        case Field::HairColor: return "Hair colour";
        case Field::HairHighlight: return "Highlight";
        case Field::Eye: return "Eye colour";
        }
        return "?";
    }

    inline int PaletteIndexOf(int color)
    {
        for (int i = 0; i < (int)sizeof(kHairPalette); ++i)
            if (kHairPalette[i] == color) return i;
        return -1;
    }

    // The value as the row shows it ("3 / 24", "60 %", "None", "Male").
    inline std::string Value(const Appearance& a, const Row& r)
    {
        char b[48];
        auto of = [&](int i, int n) { std::snprintf(b, sizeof b, "%d / %d", i + 1, n); return std::string(b); };
        switch (r.f) {
        case Field::Sex: return a.sex == Sex::Female ? "Female" : "Male";
        case Field::Father: return of(ParentIndexOf(true, a.father), ParentCount(true));
        case Field::Mother: return of(ParentIndexOf(false, a.mother), ParentCount(false));
        case Field::ShapeMix:
            std::snprintf(b, sizeof b, "%d %% mother", (int)a.shapeMix);
            return b;
        case Field::SkinMix:
            std::snprintf(b, sizeof b, "%d %% mother", (int)a.skinMix);
            return b;
        case Field::Feature:
            std::snprintf(b, sizeof b, "%+d", r.sub >= 0 && r.sub < kFeatures ? (int)a.features[r.sub] : 0);
            return b;
        case Field::OverlayIndex: {
            if (r.sub < 0 || r.sub >= kOverlays) return "?";
            const Overlay& o = a.overlays[r.sub];
            if (o.index == kOverlayNone) return "None";
            return of(o.index, kOverlayCount[r.sub]);
        }
        case Field::OverlayOpacity:
            std::snprintf(b, sizeof b, "%d %%", r.sub >= 0 && r.sub < kOverlays ? (int)a.overlays[r.sub].opacity : 0);
            return b;
        case Field::OverlayColor: {
            if (r.sub < 0 || r.sub >= kOverlays) return "?";
            const int c = a.overlays[r.sub].color;
            if (OverlayColorType(r.sub) == 1) return of(PaletteIndexOf(c), (int)sizeof(kHairPalette));
            return of(c, kColorMax + 1);
        }
        case Field::Hair: return of(HairIndexOf(a.sex, a.hair), HairCount(a.sex));
        case Field::HairColor: return of(PaletteIndexOf(a.hairColor), (int)sizeof(kHairPalette));
        case Field::HairHighlight: return of(PaletteIndexOf(a.hairHighlight), (int)sizeof(kHairPalette));
        case Field::Eye: return of(a.eyeColor, kCreatorEyeMax + 1);
        }
        return "?";
    }

    // The row's value as a 0..1 fraction (the slider track under it); -1 = no track (a list choice).
    inline float Fraction(const Appearance& a, const Row& r)
    {
        switch (r.f) {
        case Field::ShapeMix: return a.shapeMix / 100.0f;
        case Field::SkinMix: return a.skinMix / 100.0f;
        case Field::Feature: return r.sub >= 0 && r.sub < kFeatures ? (a.features[r.sub] + kFeatureLimit) / (2.0f * kFeatureLimit) : -1.0f;
        case Field::OverlayOpacity: return r.sub >= 0 && r.sub < kOverlays ? a.overlays[r.sub].opacity / 100.0f : -1.0f;
        default: return -1.0f;
        }
    }

    // One step of a row (dir -1 / +1, the arrows or a click on < >). Lists wrap, amounts stop at their ends. True = the look changed (the
    // caller bumps Draft::rev). The result is always ValidAppearance when the input was.
    inline bool Step(Appearance& a, const Row& r, int dir)
    {
        dir = dir < 0 ? -1 : 1;
        auto wrap = [](int i, int n) { return ((i % n) + n) % n; };
        auto clampTo = [](int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; };
        const Appearance before = a;
        switch (r.f) {
        case Field::Sex: SetSex(a, a.sex == Sex::Male ? Sex::Female : Sex::Male); break;
        case Field::Father:
        case Field::Mother: {
            const bool fa = r.f == Field::Father;
            uint8_t& v = fa ? a.father : a.mother;
            const int i = ParentIndexOf(fa, v);
            v = (uint8_t)ParentAt(fa, i < 0 ? 0 : wrap(i + dir, ParentCount(fa)));
            break;
        }
        case Field::ShapeMix: a.shapeMix = (uint8_t)clampTo(a.shapeMix + 5 * dir, 0, 100); break;
        case Field::SkinMix: a.skinMix = (uint8_t)clampTo(a.skinMix + 5 * dir, 0, 100); break;
        case Field::Feature:
            if (r.sub < 0 || r.sub >= kFeatures) return false;
            a.features[r.sub] = (int8_t)clampTo(a.features[r.sub] + 10 * dir, -kFeatureLimit, kFeatureLimit);
            break;
        case Field::OverlayIndex: {
            if (r.sub < 0 || r.sub >= kOverlays) return false;
            Overlay& o = a.overlays[r.sub];
            const int n = kOverlayCount[r.sub] + 1;              // none is position 0, style k is k + 1
            const int pos = o.index == kOverlayNone ? 0 : o.index + 1;
            const int to = wrap(pos + dir, n);
            o.index = to == 0 ? kOverlayNone : (uint8_t)(to - 1);
            if (pos == 0 && to != 0 && o.opacity == 0) o.opacity = 100;   // a style picked from none shows at once
            break;
        }
        case Field::OverlayOpacity:
            if (r.sub < 0 || r.sub >= kOverlays) return false;
            a.overlays[r.sub].opacity = (uint8_t)clampTo(a.overlays[r.sub].opacity + 10 * dir, 0, 100);
            break;
        case Field::OverlayColor: {
            if (r.sub < 0 || r.sub >= kOverlays) return false;
            Overlay& o = a.overlays[r.sub];
            if (OverlayColorType(r.sub) == 1) {
                const int i = PaletteIndexOf(o.color);
                o.color = kHairPalette[i < 0 ? 0 : wrap(i + dir, (int)sizeof(kHairPalette))];
            } else {
                o.color = (uint8_t)wrap(o.color + dir, kColorMax + 1);
            }
            o.color2 = o.color;
            break;
        }
        case Field::Hair: {
            const int i = HairIndexOf(a.sex, a.hair);
            a.hair = (uint8_t)HairAt(a.sex, i < 0 ? 0 : wrap(i + dir, HairCount(a.sex)));
            break;
        }
        case Field::HairColor:
        case Field::HairHighlight: {
            uint8_t& v = r.f == Field::HairColor ? a.hairColor : a.hairHighlight;
            const int i = PaletteIndexOf(v);
            v = kHairPalette[i < 0 ? 0 : wrap(i + dir, (int)sizeof(kHairPalette))];
            break;
        }
        case Field::Eye: a.eyeColor = (uint8_t)(a.eyeColor > kCreatorEyeMax ? 0 : wrap(a.eyeColor + dir, kCreatorEyeMax + 1)); break;
        }
        return !(a == before);
    }

}  // namespace gage::sync::chr::screens
