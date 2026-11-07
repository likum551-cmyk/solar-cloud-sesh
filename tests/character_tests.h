// 0.5.9 characters (libs/sync/character.h + proto::CharRoster 56 / CharOp 57 / CharResult 58 / PeerFace 59): slots, the name rules, the
// appearance tables and ranges (the old gamemode's bugs pinned away), the instant character (default look and name), the roster rules
// (Normalise, Apply) and the codecs with their refusals. docs/porting/character-creation-old-gamemode.md is what these port.
// Included by tests/gameplay_test.cpp after its Check(); run by chartest::RunPure(), kChecks checks.
#pragma once

#include "sync/character.h"
#include "protocol/protocol.h"

#include <set>
#include <string>

namespace chartest {

    namespace chr = gage::sync::chr;
    namespace proto = gage::proto;

    constexpr int kChecks = 23;   // 0.5.9 stage 4: + 6 (Delete, the confirmation, PublicName, AdminRename, the player texts, PeerName)
    constexpr uint8_t kTestLip = 3;   // a lipstick value (overlay 8 has 10)

    inline chr::Character Named(const char* first, const char* last, uint32_t seed = 7)
    {
        chr::Character c;
        c.first = first;
        c.last = last;
        c.look = chr::DefaultAppearance(seed);
        return c;
    }

    // 0.5.9 stage 4 (GI 2026-11-07): Delete from the select screen with a typed confirmation, the CHARACTER name shown to the others
    // (PublicName, PeerName 60), an admin's rename, and a player-facing text for every refusal.
    inline void RunStage4()
    {
        auto none = [](const std::string&) { return false; };
        auto confirm = [](const char* typed) {
            chr::Character in;
            chr::Name n;
            if (chr::ParseConfirm(typed, n)) { in.first = n.first; in.last = n.last; }
            return in;
        };
        {
            chr::Roster r;
            r.slots = 3;
            r.chars[0] = Named("Pia", "Player");
            r.chars[1] = Named("Ann", "Lee");
            r.used = 3;
            r.active = 1;
            chr::Roster one;
            one.chars[0] = Named("Solo", "Only");
            one.used = 1;
            chr::Roster t = r;
            const chr::Code playing = chr::Apply(t, chr::Op::Delete, 1, confirm("Ann Lee"), none);
            const chr::Code last = chr::Apply(one, chr::Op::Delete, 0, confirm("Solo Only"), none);
            const chr::Code wrong = chr::Apply(t, chr::Op::Delete, 0, confirm("Pia Playr"), none);
            const chr::Code blank = chr::Apply(t, chr::Op::Delete, 0, chr::Character(), none);
            const chr::Code empty = chr::Apply(t, chr::Op::Delete, 2, confirm("Pia Player"), none);
            const bool unchanged = t == r && one.Count() == 1;
            const chr::Code ok = chr::Apply(t, chr::Op::Delete, 0, confirm("PIA player"), none);
            const chr::Code reuse = chr::Apply(t, chr::Op::Create, 0, Named("Pia", "Player"), none);
            Check("characters: Delete refuses the only character (LastOne, before Playing), the one in play (Playing), a wrong or missing typed name "
                  "(NotConfirmed), an empty slot (Empty) and changes nothing; with the name typed in any case it frees the slot, which takes a new "
                  "character",
                  playing == chr::Code::Playing && last == chr::Code::LastOne && wrong == chr::Code::NotConfirmed && blank == chr::Code::NotConfirmed &&
                      empty == chr::Code::Empty && unchanged && ok == chr::Code::Ok && reuse == chr::Code::Ok && t.Count() == 2 && t.active == 1,
                  std::string(chr::ToString(playing)) + " " + chr::ToString(last) + " " + chr::ToString(wrong) + " " + chr::ToString(ok));
        }
        {
            chr::Name n;
            const bool good = chr::ParseConfirm("Ann Lee", n) && n.first == "Ann" && n.last == "Lee";
            const bool bad = !chr::ParseConfirm("AnnLee", n) && !chr::ParseConfirm(" Ann", n) && !chr::ParseConfirm("Ann ", n) &&
                             !chr::ParseConfirm("Ann Lee Smith", n) && !chr::ParseConfirm(std::string(60, 'a') + " b", n) && !chr::ParseConfirm("", n);
            Check("characters: the typed confirmation is exactly two words split at one space (no leading / trailing space, no third word, at most "
                  "51 bytes)",
                  good && bad);
        }
        {
            const std::string a = chr::PublicName(Named("Pia", "Player"));
            const std::string b = chr::PublicName(Named("Maximiliana", "Worthingtonshire"));
            const std::string c = chr::PublicName(Named("Bob", (std::string(1, 'L') + std::string(24, 'x')).c_str()));
            const bool valid = proto::ValidName(a) && proto::ValidName(b) && proto::ValidName(c);
            Check("characters: the public name is \"First Last\" when it fits 24 bytes, \"F. Last\" when not, cut to 24 when even that is long; "
                  "every result passes proto::ValidName",
                  a == "Pia Player" && b == "M. Worthingtonshire" && c.size() == 24 && c.rfind("B. L", 0) == 0 && valid, a + " | " + b + " | " + c);
        }
        {
            chr::Roster r;
            r.chars[0] = Named("Pia", "Player");
            r.chars[0].flags = chr::kCharCustomised;
            r.used = 1;
            chr::Roster t = r;
            const chr::Code ok = chr::AdminRename(t, 0, "Anna", "Leeds", none);
            const bool kept = t.chars[0].flags == chr::kCharCustomised && t.chars[0].look == r.chars[0].look && t.chars[0].Display() == "Anna Leeds";
            chr::Roster u = r;
            const chr::Code taken = chr::AdminRename(u, 0, "Ann", "Lee", [](const std::string& k) { return k == "ann_lee"; });
            const chr::Code same = chr::AdminRename(u, 0, "Pia", "PLayer", [](const std::string& k) { return k == "pia_player"; });
            const chr::Code badName = chr::AdminRename(u, 0, "an", "Lee", none);
            const chr::Code empty = chr::AdminRename(u, 1, "Ann", "Lee", none);
            Check("characters: an admin may rename a customised character (flags and look kept); a name another character owns is NameTaken, "
                  "its own name in another case is not; a bad name or an empty slot is refused",
                  ok == chr::Code::Ok && kept && taken == chr::Code::NameTaken && same == chr::Code::Ok && badName == chr::Code::BadName &&
                      empty == chr::Code::Empty,
                  std::string(chr::ToString(ok)) + " " + chr::ToString(taken) + " " + chr::ToString(same));
        }
        {
            int texts = 0;
            std::set<std::string> seen;
            for (int c = 0; c <= chr::kCodeLast; ++c)
            {
                const std::string t = chr::PlayerText((chr::Code)c);
                texts += !t.empty() && t != "Refused." ? 1 : 0;
                seen.insert(t);
            }
            const bool shop = std::string(chr::PlayerText(chr::Code::Locked)).find("Solar RP shop") != std::string::npos;
            Check("characters: every refusal code 0..14 has its own player-facing line; a locked slot points at the Solar RP shop",
                  texts == chr::kCodeLast + 1 && (int)seen.size() == chr::kCodeLast + 1 && shop, std::to_string(texts) + " texts");
        }
        {
            proto::PeerName pn{7, "Pia Player"}, back;
            const proto::Bytes b = proto::Encode(pn);
            const bool round = proto::Decode(b.data(), b.size(), back) && back.id == 7 && back.name == "Pia Player" && b[0] == 60;
            proto::PeerName bad1{7, ""}, bad2{7, "Bad\x01Name"}, bad3{(uint16_t)proto::kMaxPlayers, "Pia Player"}, bad4{7, std::string(25, 'a')};
            const bool refused = proto::Encode(bad1).empty() && proto::Encode(bad2).empty() && proto::Encode(bad3).empty() && proto::Encode(bad4).empty();
            proto::Bytes raw = b;
            raw.back() = 0x01;   // a control byte as the name's last character
            const bool rawRefused = !proto::Decode(raw.data(), raw.size(), back);
            Check("characters: PeerName (60) round-trips; Encode refuses an empty, control-character or 25-byte name and an id past kMaxPlayers; "
                  "Decode refuses a name that fails ValidName",
                  round && refused && rawRefused);
        }
    }

    inline void RunPure()
    {
        auto none = [](const std::string&) { return false; };
        {
            const bool ok = chr::SlotsFor(-1) == 1 && chr::SlotsFor(0) == 1 && chr::SlotsFor(1) == 1 && chr::SlotsFor(2) == 2 &&
                            chr::SlotsFor(3) == 3 && chr::SlotsFor(4) == 3 && chr::SlotsFor(99) == 3 && chr::kMaxSlots == 3;
            Check("characters: slots are 1 + up to 2 bought (SlotsFor holds the platform's number to 1..3; 0 = not said = 1)", ok);
        }
        {
            const bool ok = (uint8_t)proto::Msg::CharRoster == 56 && (uint8_t)proto::Msg::CharOp == 57 && (uint8_t)proto::Msg::CharResult == 58 &&
                            (uint8_t)proto::Msg::PeerFace == 59 && (uint8_t)proto::Msg::PeerName == 60 && proto::kMsgLast == (uint8_t)proto::Msg::PeerName && proto::kCharOpLast == chr::kOpLast &&
                            proto::kCharCodeLast == chr::kCodeLast && proto::kMaxCharBlob == chr::kMaxBlob && proto::kCharSlots == chr::kMaxSlots;
            Check("characters: the wire constants equal the rules' (types 56..60 the last, op / code ranges, blob cap, 3 slots)", ok);
        }
        {
            using P = chr::NamePart;
            const bool good = chr::ValidNamePart("John", P::First) && chr::ValidNamePart("Smith", P::Last) && chr::ValidNamePart("McCry", P::Last) &&
                              chr::ValidNamePart("Abc", P::First) && chr::ValidNamePart(std::string(1, 'A') + std::string(24, 'b'), P::Last);
            const bool bad = !chr::ValidNamePart("Jo", P::First) && !chr::ValidNamePart(std::string(1, 'A') + std::string(25, 'b'), P::First) &&
                             !chr::ValidNamePart("john", P::First) && !chr::ValidNamePart("JOhn", P::First) && !chr::ValidNamePart("McCry", P::First) &&
                             !chr::ValidNamePart("McDoNald", P::Last) && !chr::ValidNamePart("Jo3n", P::First) && !chr::ValidNamePart("Jo n", P::First) &&
                             !chr::ValidNamePart("J\xC3\xB6hn", P::First) && !chr::ValidNamePart("", P::Last);
            const bool key = chr::NameKey("John", "Smith") == "john_smith" && chr::NameKey("JOHN", "SMITH") == chr::NameKey("john", "smith") &&
                             chr::NameKey("Ann", "Lee") != chr::NameKey("Anne", "Lee");
            Check("characters: names are 3..25 ASCII letters, first upper, <= 1 capital in a first name and <= 2 in a last (the old UI's rules); "
                  "the identity is case-insensitive",
                  good && bad && key, std::string(good ? "" : "good ") + (bad ? "" : "bad ") + (key ? "" : "key"));
        }
        {
            // GTA's overlay value counts and the colour types BY OVERLAY ID (the old code coloured ids 1 / 2 / 10 with the indices of
            // its own array: porting doc, bug 1).
            int sum = 0;
            for (uint8_t n : chr::kOverlayCount) sum += n;
            bool types = true;
            for (int i = 0; i < chr::kOverlays; ++i)
            {
                const int want = (i == 1 || i == 2 || i == 10) ? 1 : (i == 5 || i == 8) ? 2 : 0;
                types = types && chr::OverlayColorType(i) == want;
            }
            const bool sexes = chr::OverlayForSex(1, chr::Sex::Male) && !chr::OverlayForSex(1, chr::Sex::Female) &&
                               chr::OverlayForSex(8, chr::Sex::Female) && !chr::OverlayForSex(8, chr::Sex::Male) &&
                               chr::OverlayForSex(2, chr::Sex::Female) && chr::OverlayForSex(2, chr::Sex::Male);
            Check("characters: 13 overlays with GTA's counts (24 29 34 15 75 7 12 11 10 18 17 12 2 = 266); hair-coloured 1 2 10, makeup-coloured "
                  "5 8, by overlay id; beards for men, lipstick for women",
                  sum == 266 && types && sexes, "sum " + std::to_string(sum));
        }
        {
            using S = chr::Sex;
            const bool counts = chr::HairCount(S::Male) == 36 && chr::HairCount(S::Female) == 37;
            const bool male = chr::HairAt(S::Male, 0) == 0 && chr::HairAt(S::Male, 22) == 22 && chr::HairAt(S::Male, 23) == 24 &&
                              chr::HairAt(S::Male, 35) == 36 && chr::HairIndexOf(S::Male, 23) < 0 && chr::HairIndexOf(S::Male, 37) < 0;
            const bool female = chr::HairAt(S::Female, 23) == 25 && chr::HairAt(S::Female, 36) == 38 && chr::HairIndexOf(S::Female, 23) < 0 &&
                                chr::HairIndexOf(S::Female, 24) < 0 && chr::HairIndexOf(S::Female, 39) < 0;
            bool round = true;
            for (S s : {S::Male, S::Female})
                for (int i = 0; i < chr::HairCount(s); ++i) round = round && chr::HairIndexOf(s, chr::HairAt(s, i)) == i;
            Check("characters: hair lists are the old creator's (male 0-22, 24-36; female 0-22, 25-38: the night-vision drawables 23 / 24 are "
                  "skipped) and index <-> drawable round-trips",
                  counts && male && female && round);
        }
        {
            const chr::Appearance base = chr::DefaultAppearance(1);
            int refused = 0, clamped = 0, n = 0;
            auto probe = [&](chr::Appearance a) {
                ++n;
                if (!chr::ValidAppearance(a)) ++refused;
                if (chr::ValidAppearance(chr::Clamp(a))) ++clamped;
            };
            chr::Appearance a = base; a.father = 46; probe(a);
            a = base; a.mother = 200; probe(a);
            a = base; a.shapeMix = 101; probe(a);
            a = base; a.skinMix = 255; probe(a);
            a = base; a.features[7] = 101; probe(a);
            a = base; a.features[19] = -101; probe(a);
            a = base; a.overlays[2].index = 34; probe(a);
            a = base; a.overlays[12].index = 2; probe(a);
            a = base; a.overlays[0].opacity = 101; probe(a);
            a = base; a.overlays[1].color = 64; probe(a);
            a = base; a.hair = 23; probe(a);
            a = base; a.hairHighlight = 64; probe(a);
            a = base; a.eyeColor = 32; probe(a);
            a = base; a.sex = chr::Sex::Female; a.hair = 24; probe(a);
            const bool edges = [&] {
                chr::Appearance e = base;
                e.father = 45; e.mother = 0; e.shapeMix = 100; e.skinMix = 0; e.features[0] = -100; e.features[1] = 100;
                e.overlays[2].index = 33; e.overlays[2].opacity = 0; e.overlays[2].color = 63; e.eyeColor = 31; e.hairColor = 63;
                e.overlays[4].index = chr::kOverlayNone;
                return chr::ValidAppearance(e) && chr::Clamp(e) == e;
            }();
            Check("characters: ValidAppearance refuses each of 14 out-of-range values (parents > 45, mixes > 100, features beyond +-100, an "
                  "overlay index past its count, opacity, colours > 63, a hair the sex lacks, eye > 31); Clamp makes every one valid and leaves "
                  "the range edges alone",
                  refused == n && clamped == n && edges, std::to_string(refused) + " refused, " + std::to_string(clamped) + " clamped of " +
                                                             std::to_string(n));
        }
        {
            bool valid = true, shape = true;
            std::set<std::string> faces;
            for (uint32_t s = 0; s < 1000; ++s)
            {
                const chr::Appearance a = chr::DefaultAppearance(s * 2654435761u);
                valid = valid && chr::ValidAppearance(a);
                shape = shape && a.sex == chr::Sex::Male && a.father <= 20 && a.mother >= 21 && a.mother <= 41 && a.shapeMix == 50 &&
                        a.overlays[2].index != chr::kOverlayNone && a.eyeColor <= chr::kCreatorEyeMax && a.overlays[1].index == chr::kOverlayNone;
                if (s < 100) faces.insert(chr::EncodeLook(a));
            }
            const bool same = chr::DefaultAppearance(42) == chr::DefaultAppearance(42) &&
                              chr::SeedOf("acc-player") == chr::SeedOf("acc-player") && chr::SeedOf("a") != chr::SeedOf("b");
            Check("characters: the instant look is valid for 1000 seeds, male, a male father and a female mother at 50 %, eyebrows, no beard, a "
                  "natural eye; the same seed gives the same face and 100 seeds give >= 95 different faces",
                  valid && shape && same && faces.size() >= 95, std::to_string(faces.size()) + " faces");
        }
        {
            chr::Name a, b, c, d, e, f;
            const bool split = chr::DefaultName("Pia_Player", 1, none, a) && a.first == "Pia" && a.last == "Player" &&
                               chr::DefaultName("PiaPlayer", 1, none, b) && b.first == "Pia" && b.last == "Player" &&
                               chr::DefaultName("JOHN smith", 1, none, c) && c.first == "John" && c.last == "Smith";
            const bool fill = chr::DefaultName("Zed", 3, none, d) && d.first == "Zed" && chr::ValidNamePart(d.last, chr::NamePart::Last) &&
                              chr::DefaultName("x1", 3, none, e) && chr::ValidNamePart(e.first, chr::NamePart::First) &&
                              chr::ValidNamePart(e.last, chr::NamePart::Last);
            const bool moved = chr::DefaultName("Pia_Player", 1, [](const std::string& k) { return k == "pia_player"; }, f) && f.first == "Pia" &&
                               f.last != "Player" && chr::ValidNamePart(f.last, chr::NamePart::Last);
            // every list name taken: the suffixed surname; everything taken: false
            chr::Name g, h;
            auto onlyAab = [](const std::string& k) { return !(k.size() > 3 && k.compare(k.size() - 3, 3, "aab") == 0); };
            const bool suffixed = chr::DefaultName("Pia_Player", 1, onlyAab, g) &&
                                  g.first == "Pia" && g.last.size() > 3 && g.last.compare(g.last.size() - 3, 3, "aab") == 0 &&
                                  chr::ValidNamePart(g.last, chr::NamePart::Last);
            const bool exhausted = !chr::DefaultName("Pia_Player", 1, [](const std::string&) { return true; }, h);
            Check("characters: the instant name is the account name's two letter runs (Pia_Player, PiaPlayer, 'JOHN smith' -> Pia Player / John "
                  "Smith), else its first run + a surname, else list names; a taken name moves on; with every list name taken a suffixed "
                  "surname; with everything taken, false",
                  split && fill && moved && suffixed && exhausted,
                  std::string(split ? "" : "split ") + (fill ? "" : "fill ") + (moved ? "" : "moved ") + (suffixed ? "" : "suffixed ") +
                      (exhausted ? "" : "exhausted"));
        }
        {
            chr::Roster r;
            r.slots = 1;
            r.used = 0x4;   // only slot 2 holds a character, and it is not owned
            r.chars[2] = Named("Ann", "Lee");
            const bool locked = !chr::Normalise(r) && r.active == 0;
            r.slots = 3;
            const bool moved = chr::Normalise(r) && r.active == 2 && r.Active() && r.Active()->first == "Ann";
            r.used = 0x5;
            r.chars[0] = Named("Bob", "Ray");
            r.active = 2;
            const bool kept = chr::Normalise(r) && r.active == 2;
            r.slots = 9;
            const bool held = chr::Normalise(r) && r.slots == 3;
            r.slots = 1;   // the slots bought were refunded while slot 2 was active: back to slot 0
            const bool refunded = chr::Normalise(r) && r.active == 0;
            Check("characters: Normalise moves the active slot to an owned character, says false when none is playable (a refunded slot locks "
                  "its character), keeps a playable active slot, leaves an active slot that lost its ownership and holds slots to 1..3",
                  locked && moved && kept && held && refunded);
        }
        {
            chr::Roster r;
            r.slots = 2;
            const chr::Character ann = Named("Ann", "Lee");
            auto takenAnn = [](const std::string& k) { return k == "ann_lee"; };
            const bool refusals = chr::Apply(r, chr::Op::Create, 3, ann, none) == chr::Code::BadSlot &&
                                  chr::Apply(r, chr::Op::Create, 2, ann, none) == chr::Code::Locked &&
                                  chr::Apply(r, chr::Op::Create, 0, Named("ann", "Lee"), none) == chr::Code::BadName &&
                                  chr::Apply(r, chr::Op::Create, 0, ann, takenAnn) == chr::Code::NameTaken &&
                                  [&] { chr::Character bad = ann; bad.look.eyeColor = 40; return chr::Apply(r, chr::Op::Create, 0, bad, none) == chr::Code::BadLook; }() &&
                                  r.used == 0;
            const bool made = chr::Apply(r, chr::Op::Create, 1, ann, none) == chr::Code::Ok && r.Has(1) && r.chars[1].first == "Ann" &&
                              (r.chars[1].flags & chr::kCharCustomised) && r.active == 0;
            const bool used = chr::Apply(r, chr::Op::Create, 1, Named("Bob", "Ray"), none) == chr::Code::SlotUsed;
            Check("characters: Create refuses a slot past 3, an unowned slot, a bad name, a taken name and a bad look (changing nothing), then "
                  "fills an empty owned slot as customised without switching to it; a second Create there is SlotUsed",
                  refusals && made && used);
        }
        {
            chr::Roster r;
            r.slots = 1;
            r.used = 1;
            r.chars[0] = Named("Pia", "Player");   // the instant character: not customised
            auto takenRay = [](const std::string& k) { return k == "bob_ray"; };
            chr::Character edit = Named("Bob", "Ray", 99);
            const bool taken = chr::Apply(r, chr::Op::Save, 0, edit, takenRay) == chr::Code::NameTaken && r.chars[0].first == "Pia";
            edit.first = "Pia";
            edit.last = "Smith";
            const bool renamed = chr::Apply(r, chr::Op::Save, 0, edit, none) == chr::Code::Ok && r.chars[0].last == "Smith" &&
                                 r.chars[0].look == edit.look && (r.chars[0].flags & chr::kCharCustomised);
            chr::Character again = edit;
            again.last = "Jones";
            const bool fixed = chr::Apply(r, chr::Op::Save, 0, again, none) == chr::Code::NameFixed && r.chars[0].last == "Smith";
            chr::Character look = edit;
            look.look.hairColor = 5;
            const bool restyle = chr::Apply(r, chr::Op::Save, 0, look, [](const std::string&) { return true; }) == chr::Code::Ok &&
                                 r.chars[0].look.hairColor == 5;
            const bool empty = chr::Apply(r, chr::Op::Save, 2, look, none) == chr::Code::Locked &&
                               [&] { chr::Roster q = r; q.slots = 3; return chr::Apply(q, chr::Op::Save, 2, look, none) == chr::Code::Empty; }();
            Check("characters: Save renames the instant character once (a taken name refused) and marks it customised; a later rename is "
                  "NameFixed; a look change is always allowed (its own name is never 'taken'); an unowned / empty slot is refused",
                  taken && renamed && fixed && restyle && empty);
        }
        {
            chr::Roster r;
            r.slots = 2;
            r.used = 0x5;
            r.chars[0] = Named("Pia", "Player");
            r.chars[2] = Named("Ann", "Lee");
            const chr::Character any;
            const bool ok = chr::Apply(r, chr::Op::Select, 1, any, none) == chr::Code::Empty &&
                            chr::Apply(r, chr::Op::Select, 2, any, none) == chr::Code::Locked && r.active == 0;
            r.used = 0x3;
            r.chars[1] = Named("Ann", "Lee");
            const bool sel = chr::Apply(r, chr::Op::Select, 1, any, none) == chr::Code::Ok && r.active == 1 && r.Active()->first == "Ann";
            Check("characters: Select refuses an empty slot and a stored character in an unowned slot, and switches to an owned one", ok && sel);
        }
        {
            const chr::Appearance a = chr::DefaultAppearance(5);
            chr::Appearance back;
            const std::string look = chr::EncodeLook(a);
            const bool lookOk = look.size() == 1 + chr::kAppearanceBytes && chr::kAppearanceBytes == 81 && chr::DecodeLook(look, back) && back == a;
            chr::Roster r;
            r.slots = 3;
            r.active = 1;
            r.used = 0x7;
            r.chars[0] = Named("Pia", "Player", 1);
            r.chars[1] = Named("Annabellemariejosephinexy", "MacDonaldsmithjonesbrown", 2);   // 25 / 24 letters
            r.chars[1].flags = chr::kCharCustomised;
            r.chars[2] = Named("Bob", "Ray", 3);
            r.chars[2].look.sex = chr::Sex::Female;
            r.chars[2].look.hair = 38;
            r.chars[2].look.overlays[8] = {3, 80, 12, 0};
            const std::string blob = chr::EncodeRoster(r);
            chr::Roster rb;
            const bool rosterOk = chr::DecodeRoster(blob, rb) && rb == r && blob.size() <= chr::kMaxBlob;
            Check("characters: a look is 1 + 81 bytes and round-trips; a full roster (3 characters, a 25-letter name, a woman with lipstick) "
                  "round-trips within the 512-byte blob cap",
                  lookOk && rosterOk, std::to_string(blob.size()) + " bytes");
        }
        {
            chr::Roster r;
            r.slots = 2;
            r.used = 1;
            r.chars[0] = Named("Pia", "Player");
            const std::string good = chr::EncodeRoster(r);
            chr::Roster out;
            int refused = 0, n = 0;
            auto bad = [&](std::string b) {
                ++n;
                if (!chr::DecodeRoster(b, out)) ++refused;
            };
            std::string b = good; b[0] = 2; bad(b);                 // version
            b = good; b[1] = 0; bad(b);                             // 0 slots
            b = good; b[1] = 4; bad(b);                             // 4 slots
            b = good; b[2] = 3; bad(b);                             // active 3
            b = good; b[3] = 0x9; bad(b);                           // a fourth slot bit
            bad(good + '\0');                                       // trailing byte
            bad(good.substr(0, good.size() - 1));                   // truncated
            b = good; b[4] = 0x2; bad(b);                           // an unknown character flag
            b = good; b[6] = 'p'; bad(b);                           // a stored name that breaks the rules ("pia")
            b = good; b[4 + 1 + 1 + 3 + 1 + 6] = 2; bad(b);         // sex 2
            const bool decodes = chr::DecodeRoster(good, out) && out == r;
            Check("characters: DecodeRoster refuses 10 broken rosters (version, 0 / 4 slots, active 3, a fourth slot bit, trailing / missing "
                  "bytes, an unknown flag, a stored lower-case name, sex 2) and takes the good one",
                  refused == n && decodes, std::to_string(refused) + " of " + std::to_string(n));
        }
        {
            chr::Character c = Named("pia", "player");   // a client may send any letters: Apply judges the name, not the decoder
            chr::Character back;
            const std::string blob = chr::EncodeCharacter(c);
            chr::Roster empty;
            const bool lower = chr::DecodeCharacter(blob, back) && back.first == "pia" &&
                               chr::Apply(empty, chr::Op::Create, 0, back, none) == chr::Code::BadName;
            chr::Character longName = c;
            longName.first = std::string(26, 'a');
            chr::Character badLook = c;
            badLook.look.father = 46;
            const bool refused = !chr::DecodeCharacter(chr::EncodeCharacter(longName), back) && !chr::DecodeCharacter(chr::EncodeCharacter(badLook), back) &&
                                 !chr::DecodeCharacter(blob + 'x', back) && !chr::DecodeCharacter(std::string(), back);
            Check("characters: DecodeCharacter (a CharOp's blob) takes any name up to 25 bytes for Apply to judge (BadName), and refuses a "
                  "longer name, a look out of range, a trailing byte and nothing",
                  lower && refused);
        }
        {
            // The apply plan pins the old gamemode's three apply bugs away (porting doc: colours by overlay id, eye colour kept,
            // parents not swapped) and GTA's units.
            chr::Appearance a = chr::DefaultAppearance(9);
            a.father = 4;
            a.mother = 33;
            a.shapeMix = 25;
            a.skinMix = 80;
            a.features[0] = -50;
            a.features[19] = 100;
            a.overlays[1] = {5, 60, 12, 13};    // facial hair: hair palette
            a.overlays[10] = {2, 100, 7, 0};    // chest hair: hair palette
            a.overlays[3] = {4, 30, 9, 9};      // ageing: no tint
            a.overlays[8] = {kTestLip, 90, 21, 22};
            a.overlays[0].index = chr::kOverlayNone;
            a.eyeColor = 6;
            a.hairColor = 11;
            a.hairHighlight = 3;
            const chr::FacePlan p = chr::PlanFace(a);
            const bool parents = p.shapeFirst == 4 && p.skinFirst == 4 && p.shapeSecond == 33 && p.skinSecond == 33 && p.shapeMix == 0.25f &&
                                 p.skinMix == 0.8f;
            const bool morph = p.morph[0] == -0.5f && p.morph[19] == 1.0f && p.morph[5] == 0.0f;
            const bool tints = p.overlay[1].colorType == 1 && p.overlay[1].color == 12 && p.overlay[1].color2 == 13 && p.overlay[1].index == 5 &&
                               p.overlay[1].opacity == 0.6f && p.overlay[10].colorType == 1 && p.overlay[10].color == 7 &&
                               p.overlay[3].colorType == 0 && p.overlay[8].colorType == 2 && p.overlay[8].color == 21 &&
                               p.overlay[0].index == 255 && p.overlay[0].opacity == 0.0f;
            const bool rest = p.eyeColor == 6 && p.hairColor == 11 && p.hairHighlight == 3 && p.hairDrawable == a.hair;
            Check("characters: the apply plan puts the father first and the mixes toward the mother, features and opacities in GTA's -1..1 / "
                  "0..1, a tint for the hair-coloured overlays 1 / 2 / 10 and the makeup-coloured 5 / 8 only (by overlay id), none = 255, and "
                  "keeps the eye and hair colours",
                  parents && morph && tints && rest,
                  std::string(parents ? "" : "parents ") + (morph ? "" : "morph ") + (tints ? "" : "tints ") + (rest ? "" : "rest"));
        }
        {
            proto::CharRoster ro{proto::kCharRosterGuest, std::string("\x01\x01\x00\x00", 4)}, ro2;
            proto::CharOp op, op2;
            op.seq = 7; op.op = 3; op.slot = 2; op.blob = "x";
            proto::CharResult res, res2;
            res.seq = 7; res.op = 3; res.slot = 2; res.code = 6; res.text = "name taken";
            proto::PeerFace pf{12, "abc"}, pf2;
            const proto::Bytes b1 = proto::Encode(ro), b2 = proto::Encode(op), b3 = proto::Encode(res), b4 = proto::Encode(pf);
            const bool round = proto::Decode(b1.data(), b1.size(), ro2) && ro2.flags == ro.flags && ro2.blob == ro.blob &&
                               proto::Decode(b2.data(), b2.size(), op2) && op2.seq == 7 && op2.op == 3 && op2.slot == 2 && op2.blob == "x" &&
                               proto::Decode(b3.data(), b3.size(), res2) && res2.code == 6 && res2.text == "name taken" &&
                               proto::Decode(b4.data(), b4.size(), pf2) && pf2.id == 12 && pf2.blob == "abc";
            int refused = 0;
            auto none0 = [&](const proto::Bytes& b) { refused += b.empty() ? 1 : 0; };
            proto::CharRoster r1 = ro; r1.flags = 0x2; none0(proto::Encode(r1));
            proto::CharRoster r2 = ro; r2.blob.clear(); none0(proto::Encode(r2));
            proto::CharOp o1 = op; o1.seq = 0; none0(proto::Encode(o1));
            proto::CharOp o2 = op; o2.op = 0; none0(proto::Encode(o2));
            proto::CharOp o3 = op; o3.op = 5; none0(proto::Encode(o3));
            proto::CharOp o4 = op; o4.slot = 3; none0(proto::Encode(o4));
            proto::CharResult c1 = res; c1.code = 15; none0(proto::Encode(c1));
            proto::PeerFace p1 = pf; p1.id = (uint16_t)proto::kMaxPlayers; none0(proto::Encode(p1));
            proto::PeerFace p2 = pf; p2.blob.clear(); none0(proto::Encode(p2));
            Check("characters: CharRoster / CharOp / CharResult / PeerFace round-trip; Encode refuses 9 invalid ones (an unknown roster flag, "
                  "empty blobs, seq 0, op 0 / 5, slot 3, code 15, an id past kMaxPlayers)",
                  round && refused == 9, std::to_string(refused) + " refused");
        }
        RunStage4();
    }

} // namespace chartest
