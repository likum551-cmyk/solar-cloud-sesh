// 0.5.9 stage 4: the character select screen and the creator (libs/sync/char_screens.h). The cards and what each offers (GI: slots 1..3, a
// locked slot is bought in the shop, delete never the last one nor the one in play), the drafts (the name editable only until the first
// creator save, the sex only at creation), the client-side check, the rows each section shows, and the steppers (every step keeps the look
// valid; lists wrap, amounts stop). Included by tests/gameplay_test.cpp after its Check(); run by chscr::RunPure(), kChecks checks.
#pragma once

#include "sync/char_screens.h"

#include <string>

namespace chscr {

    namespace chr = gage::sync::chr;
    namespace cs = gage::sync::chr::screens;

    constexpr int kChecks = 12;

    inline chr::Character Ch(const char* first, const char* last, uint8_t flags = chr::kCharCustomised, uint32_t seed = 3)
    {
        chr::Character c;
        c.first = first;
        c.last = last;
        c.look = chr::DefaultAppearance(seed);
        c.flags = flags;
        return c;
    }

    inline void RunPure()
    {
        // A two-slot account playing slot 1, with a character in slot 0, slot 1, and a refunded third character kept in slot 2.
        chr::Roster r;
        r.slots = 2;
        r.chars[0] = Ch("Ann", "Lee");
        r.chars[1] = Ch("Bob", "Ray");
        r.used = 0x3;
        r.active = 1;
        {
            const bool cards = cs::CardOf(r, 0) == cs::Card::Ready && cs::CardOf(r, 1) == cs::Card::Playing && cs::CardOf(r, 2) == cs::Card::Locked;
            chr::Roster s = r;
            s.chars[2] = Ch("Cid", "Moe");
            s.used |= 0x4;
            const bool stored = cs::CardOf(s, 2) == cs::Card::Stored;
            chr::Roster f = r;
            f.slots = 3;
            const bool free = cs::CardOf(f, 2) == cs::Card::Free;
            Check("char screens: the cards (ready, playing, locked past the slots, stored when a character sits past them, free in a usable slot)",
                  cards && stored && free, std::string(cs::ToString(cs::CardOf(r, 0))) + " " + cs::ToString(cs::CardOf(r, 1)) + " " +
                                               cs::ToString(cs::CardOf(r, 2)));
        }
        {
            const uint8_t ready = cs::ActionsOf(r, 0), playing = cs::ActionsOf(r, 1), locked = cs::ActionsOf(r, 2);
            Check("char screens: a ready card offers PLAY, EDIT and DELETE; the one in play only EDIT (no delete, no play); a locked one the shop",
                  ready == (cs::kActPlay | cs::kActEdit | cs::kActDelete) && playing == cs::kActEdit && locked == cs::kActShop,
                  std::to_string(ready) + " " + std::to_string(playing) + " " + std::to_string(locked));
        }
        {
            // the last character: in play, so no delete anyway; and with one character there is never a delete on any card
            chr::Roster one;
            one.slots = 3;
            one.chars[0] = Ch("Ann", "Lee");
            one.used = 0x1;
            one.active = 0;
            const uint8_t a0 = cs::ActionsOf(one, 0), a1 = cs::ActionsOf(one, 1);
            // a roster whose only character is not the active one (cannot happen after Normalise, but the screen must still not offer it)
            chr::Roster odd = one;
            odd.active = 1;
            const uint8_t o0 = cs::ActionsOf(odd, 0);
            Check("char screens: the last character is never offered for DELETE; a free slot offers CREATE",
                  !(a0 & cs::kActDelete) && a1 == cs::kActCreate && !(o0 & cs::kActDelete) && (o0 & cs::kActPlay),
                  std::to_string(a0) + " " + std::to_string(a1) + " " + std::to_string(o0));
        }
        {
            const chr::Character c = Ch("Ann", "McLee");
            Check("char screens: the delete confirmation matches the full name in any case, and nothing else",
                  cs::ConfirmMatches("Ann McLee", c) && cs::ConfirmMatches("ann mclee", c) && !cs::ConfirmMatches("Ann", c) &&
                      !cs::ConfirmMatches("Ann McLe", c) && !cs::ConfirmMatches("Ann  McLee", c) && !cs::ConfirmMatches("", c) &&
                      !cs::ConfirmMatches("Ann McLee x", c),
                  "");
        }
        {
            chr::Roster f = r;
            f.slots = 3;
            cs::Draft d;
            const bool refusedUsed = !cs::BeginCreate(f, 0, chr::Sex::Female, 9, d);
            const bool refusedLocked = !cs::BeginCreate(r, 2, chr::Sex::Female, 9, d);
            const bool ok = cs::BeginCreate(f, 2, chr::Sex::Female, 9, d);
            Check("char screens: CREATE only in a free slot; the draft is the seeded face of the chosen sex with empty, editable names and an "
                  "editable sex",
                  refusedUsed && refusedLocked && ok && d.mode == cs::Mode::Create && d.slot == 2 && d.ch.look.sex == chr::Sex::Female &&
                      chr::ValidAppearance(d.ch.look) && d.ch.first.empty() && d.nameEditable && d.sexEditable &&
                      cs::OpOf(d) == chr::Op::Create,
                  std::to_string((int)d.ch.look.hair));
        }
        {
            chr::Roster e = r;
            e.chars[0].flags = 0;   // the instant character, never saved in the creator
            cs::Draft d0, d1, dl;
            const bool b0 = cs::BeginEdit(e, 0, d0), b1 = cs::BeginEdit(e, 1, d1), bl = cs::BeginEdit(e, 2, dl);
            Check("char screens: EDIT keeps the character; its name is editable only while it was never saved in the creator; the sex never; "
                  "no edit of a locked slot",
                  b0 && b1 && !bl && d0.nameEditable && !d1.nameEditable && !d0.sexEditable && !d1.sexEditable && d1.ch == e.chars[1] &&
                      cs::OpOf(d1) == chr::Op::Save,
                  "");
        }
        {
            chr::Roster f = r;
            f.slots = 3;
            cs::Draft d;
            cs::BeginCreate(f, 2, chr::Sex::Male, 4, d);
            const chr::Code empty = cs::Check(d);
            d.ch.first = "Joe";
            d.ch.last = "x";
            const chr::Code shortLast = cs::Check(d);
            d.ch.last = "Doe";
            const chr::Code ok = cs::Check(d);
            d.ch.look.eyeColor = 200;
            const chr::Code badLook = cs::Check(d);
            // a locked name is not the client's to judge: an Edit of a customised character with an odd stored name still checks Ok
            cs::Draft e;
            cs::BeginEdit(r, 1, e);
            e.ch.first = "x";
            const chr::Code locked = cs::Check(e);
            Check("char screens: the client check: names (only when editable) then the look",
                  empty == chr::Code::BadName && shortLast == chr::Code::BadName && ok == chr::Code::Ok && badLook == chr::Code::BadLook &&
                      locked == chr::Code::Ok,
                  std::string(chr::ToString(empty)) + " " + chr::ToString(ok) + " " + chr::ToString(badLook) + " " + chr::ToString(locked));
        }
        {
            std::string s;
            const bool a = cs::TypeNameChar(s, 'j');
            const bool b = cs::TypeNameChar(s, 'o');
            const bool digit = !cs::TypeNameChar(s, '1');
            const bool space = !cs::TypeNameChar(s, ' ');
            std::string full(chr::kNameMaxLen, 'a');
            const bool cap = !cs::TypeNameChar(full, 'b');
            Check("char screens: a name field takes letters only, capitalises the first, stops at 25",
                  a && b && digit && space && cap && s == "Jo" && std::string(cs::NameHint("Jo", chr::NamePart::First)).size() > 0 &&
                      std::string(cs::NameHint("Joe", chr::NamePart::First)).empty() &&
                      std::string(cs::NameHint("JoE", chr::NamePart::First)).size() > 0,
                  s);
        }
        {
            cs::Draft d;
            chr::Roster f = r;
            f.slots = 3;
            cs::BeginCreate(f, 2, chr::Sex::Male, 5, d);
            cs::Row rows[cs::kMaxRows];
            const int heritage = cs::Rows(cs::Section::Heritage, d, rows, cs::kMaxRows);
            const bool sexFirst = heritage == 5 && rows[0].f == cs::Field::Sex;
            cs::Draft e;
            cs::BeginEdit(r, 1, e);
            const int heritageEdit = cs::Rows(cs::Section::Heritage, e, rows, cs::kMaxRows);
            const bool noSex = heritageEdit == 4 && rows[0].f == cs::Field::Father;
            const int features = cs::Rows(cs::Section::Features, d, rows, cs::kMaxRows);
            // looks: male = eyebrows, facial hair, blemishes, ageing, complexion, sun damage, moles (no lipstick); a style set adds its opacity
            // (and colour for a coloured overlay)
            for (int o = 0; o < chr::kOverlays; ++o) d.ch.look.overlays[o].index = chr::kOverlayNone;
            const int looksNone = cs::Rows(cs::Section::Looks, d, rows, cs::kMaxRows);
            d.ch.look.overlays[2].index = 1;   // eyebrows: + opacity + colour
            d.ch.look.overlays[0].index = 1;   // blemishes: + opacity only
            const int looksTwo = cs::Rows(cs::Section::Looks, d, rows, cs::kMaxRows);
            bool lipstick = false;
            for (int i = 0; i < looksTwo; ++i) lipstick |= rows[i].f == cs::Field::OverlayIndex && rows[i].sub == 8;
            cs::SetSex(d.ch.look, chr::Sex::Female);
            const int looksFemale = cs::Rows(cs::Section::Looks, d, rows, cs::kMaxRows);
            bool lipF = false, beardF = false;
            for (int i = 0; i < looksFemale; ++i) {
                lipF |= rows[i].f == cs::Field::OverlayIndex && rows[i].sub == 8;
                beardF |= rows[i].f == cs::Field::OverlayIndex && rows[i].sub == 1;
            }
            const int hair = cs::Rows(cs::Section::Hair, d, rows, cs::kMaxRows);
            Check("char screens: the rows (sex only when creating; 20 features; an overlay's opacity / colour only when a style is set; lipstick "
                  "for women, facial hair for men; hair, colours, eyes)",
                  sexFirst && noSex && features == chr::kFeatures && looksNone == 7 && looksTwo == 7 + 2 + 1 && !lipstick && lipF && !beardF &&
                      hair == 4,
                  std::to_string(heritage) + " " + std::to_string(heritageEdit) + " " + std::to_string(looksNone) + " " + std::to_string(looksTwo) +
                      " " + std::to_string(looksFemale));
        }
        {
            // every step of every row in both directions, many times, keeps the look valid; lists wrap round; amounts stop at their ends
            cs::Draft d;
            chr::Roster f = r;
            f.slots = 3;
            cs::BeginCreate(f, 2, chr::Sex::Male, 6, d);
            bool valid = true;
            int steps = 0;
            for (int pass = 0; pass < 2; ++pass)
                for (int s = 0; s < cs::kSections; ++s)
                    for (int dir = -1; dir <= 1; dir += 2)
                        for (int k = 0; k < 90; ++k) {
                            cs::Row rows[cs::kMaxRows];
                            const int n = cs::Rows((cs::Section)s, d, rows, cs::kMaxRows);
                            for (int i = 0; i < n; ++i) {
                                if (rows[i].f == cs::Field::Sex && (k % 7) != 0) continue;
                                cs::Step(d.ch.look, rows[i], dir);
                                ++steps;
                                valid &= chr::ValidAppearance(d.ch.look);
                            }
                        }
            chr::Appearance a = chr::DefaultAppearance(1);
            a.shapeMix = 100;
            const bool stop = !cs::Step(a, cs::Row{cs::Field::ShapeMix, 0}, +1) && a.shapeMix == 100;
            a.features[3] = -100;
            const bool stopF = !cs::Step(a, cs::Row{cs::Field::Feature, 3}, -1) && a.features[3] == -100;
            a.eyeColor = chr::kCreatorEyeMax;
            const bool wrapEye = cs::Step(a, cs::Row{cs::Field::Eye, 0}, +1) && a.eyeColor == 0;
            a.father = 44;
            const bool wrapFather = cs::Step(a, cs::Row{cs::Field::Father, 0}, +1) && a.father == 0;
            a.mother = 21;
            const bool wrapMother = cs::Step(a, cs::Row{cs::Field::Mother, 0}, -1) && a.mother == 45;
            a.father = 20;
            const bool fatherDlc = cs::Step(a, cs::Row{cs::Field::Father, 0}, +1) && a.father == 42;
            Check("char screens: every step keeps the look valid (both sexes, every row, both ways); lists wrap (eyes 8 -> 0, fathers 44 -> 0 and "
                  "20 -> 42, mothers 21 -> 45); amounts stop at their ends",
                  valid && steps > 1000 && stop && stopF && wrapEye && wrapFather && wrapMother && fatherDlc, std::to_string(steps));
        }
        {
            chr::Appearance a = chr::DefaultAppearance(2);
            a.overlays[2].index = chr::kOverlayNone;
            a.overlays[2].opacity = 0;
            const bool fromNone = cs::Step(a, cs::Row{cs::Field::OverlayIndex, 2}, +1) && a.overlays[2].index == 0 && a.overlays[2].opacity == 100;
            const bool backToNone = cs::Step(a, cs::Row{cs::Field::OverlayIndex, 2}, -1) && a.overlays[2].index == chr::kOverlayNone;
            a.overlays[2].index = (uint8_t)(chr::kOverlayCount[2] - 1);
            const bool lastToNone = cs::Step(a, cs::Row{cs::Field::OverlayIndex, 2}, +1) && a.overlays[2].index == chr::kOverlayNone;
            // the hair keeps its place in the list across a sex change (male 23 = drawable 24, female 23 = drawable 25)
            chr::Appearance h = chr::DefaultAppearance(2);
            h.hair = (uint8_t)chr::HairAt(chr::Sex::Male, 23);
            h.overlays[1].index = 3;   // a beard
            cs::SetSex(h, chr::Sex::Female);
            const bool hairKept = h.hair == chr::HairAt(chr::Sex::Female, 23) && h.overlays[1].index == chr::kOverlayNone &&
                                  chr::ValidAppearance(h);
            // a colour row on a hair-palette overlay walks the palette (ids 9 -> 11, skipping 10)
            chr::Appearance c = chr::DefaultAppearance(2);
            c.overlays[2].index = 1;
            c.overlays[2].color = 9;
            const bool palette = cs::Step(c, cs::Row{cs::Field::OverlayColor, 2}, +1) && c.overlays[2].color == 11 && c.overlays[2].color2 == 11;
            Check("char screens: an overlay steps none -> style 1 (shown at full opacity) and back, the last style wraps to none; the hair keeps its "
                  "place across a sex change and the beard goes; a hair-palette colour skips the ids the creator does not offer",
                  fromNone && backToNone && lastToNone && hairKept && palette, std::to_string((int)h.hair));
        }
        {
            chr::Appearance a = chr::DefaultAppearance(3);
            a.overlays[2].index = 4;
            a.features[0] = -30;
            a.shapeMix = 35;
            const bool v = cs::Value(a, cs::Row{cs::Field::OverlayIndex, 2}) == "5 / 34" && cs::Value(a, cs::Row{cs::Field::Feature, 0}) == "-30" &&
                           cs::Value(a, cs::Row{cs::Field::ShapeMix, 0}) == "35 % mother" &&
                           cs::Value(a, cs::Row{cs::Field::Sex, 0}) == "Male" && cs::Label(cs::Row{cs::Field::Feature, 19}) == "Neck thickness" &&
                           cs::Fraction(a, cs::Row{cs::Field::Feature, 0}) > 0.34f && cs::Fraction(a, cs::Row{cs::Field::Feature, 0}) < 0.36f &&
                           cs::Fraction(a, cs::Row{cs::Field::Hair, 0}) < 0.0f;
            Check("char screens: the row texts and slider fractions", v, cs::Value(a, cs::Row{cs::Field::OverlayIndex, 2}));
        }
    }

}  // namespace chscr
