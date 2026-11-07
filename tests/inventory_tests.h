// 0.6 inventory (libs/sync/inventory.h) against GI's REV3 BUILD_SPEC.md (HUD_DESIGN_COMPONENTS/REV3): the catalog's reference weights, the move /
// merge / swap / split rules, weight caps and load states, backpacks, equipment, quick-slot references, vehicle storage and the quantity tag.
// Included by tests/gameplay_test.cpp after its Check() and F(); run by invtest::RunPure(), kChecks checks.
#pragma once

#include "sync/inventory.h"

#include <set>
#include <string>

namespace invtest {

    using namespace gage::sync::inv;

    constexpr int kChecks = 17;   // 0.5.6: + 2 (quick keys and the weapon in hand)

    inline ItemId Id(const char* key) { const ItemDef* d = FindKey(key); return d ? d->id : 0; }

    inline void RunPure()
    {
        {
            // Section 9's reference items and their unit weights; ids and keys unique; every glyph one of section 7's 46 names
            struct Ref { const char* key; const char* name; uint32_t grams; Category cat; };
            const Ref refs[] = {{"phone", "Phone", 200, Category::Tools}, {"id_card", "ID card", 20, Category::Docs}, {"car_key", "Car key", 50, Category::Tools},
                                {"cash", "Cash", 0, Category::Docs}, {"water", "Water", 500, Category::Food}, {"sandwich", "Sandwich", 300, Category::Food},
                                {"medkit", "Medkit", 600, Category::Medical}, {"bandage", "Bandage", 50, Category::Medical}, {"flashlight", "Flashlight", 400, Category::Tools},
                                {"radio", "Radio", 350, Category::Tools}, {"notebook", "Notebook", 200, Category::Docs}, {"city_map", "City map", 100, Category::Docs},
                                {"fuel_can", "Fuel can", 4000, Category::Tools}, {"toolkit", "Toolkit", 2200, Category::Tools}, {"battery", "Battery", 200, Category::Tools},
                                {"camera", "Camera", 500, Category::Tools}, {"supply_crate", "Supply crate", 1500, Category::Tools}};
            bool refsOk = true;
            std::string bad;
            for (const Ref& r : refs) {
                const ItemDef* d = FindKey(r.key);
                if (!d || std::string(d->name) != r.name || d->unitGrams != r.grams || d->cat != r.cat) {
                    refsOk = false;
                    bad += std::string(r.key) + " ";
                }
            }
            static const char* const kGlyphs[] = {"phone", "card", "key", "cash", "bottle", "medkit", "torch", "radio", "notebook", "bandage", "burger", "wrench",
                                                  "can", "box", "map", "camera", "battery", "hat", "glasses", "mask", "ears", "chain", "jacket", "tee", "vest",
                                                  "gloves", "pants", "shoe", "watch", "bracelet", "backpack", "car", "moto", "van", "drop", "stash", "search",
                                                  "person", "weight", "lock", "unlock", "plus", "minus", "chev", "x", "ring"};
            const std::set<std::string> glyphs(std::begin(kGlyphs), std::end(kGlyphs));
            std::set<int> ids;
            std::set<std::string> keys;
            bool unique = true, glyphOk = glyphs.size() == 46;
            for (const ItemDef& d : Catalog()) {
                unique = unique && d.id != 0 && ids.insert(d.id).second && keys.insert(d.key).second;
                glyphOk = glyphOk && glyphs.count(d.glyph) == 1;
                if (!glyphs.count(d.glyph))
                    bad += std::string("glyph:") + d.glyph + " ";
            }
            for (int i = 0; i < kEquipCount; ++i)
                glyphOk = glyphOk && glyphs.count(EquipGlyph(EquipAt(i))) == 1;
            Check("inventory: the 17 reference items carry section 9's names, unit weights and categories; ids and keys are unique; every glyph is one of the 46",
                  refsOk && unique && glyphOk, bad);
        }
        {
            // A stack moves whole when it fits; onto the same item it merges up to the stack size (the rest stays); a full stack is refused
            Container a = MakePockets(), b = MakePockets();
            Add(a, Id("water"), 3);
            Add(b, Id("water"), 9);
            const Result r1 = Move(a, 0, b, 0);   // 3 onto 9 of 10: 1 goes, 2 stay
            const bool part = r1 == Result::MovedPart && b.slots[0].qty == 10 && a.slots[0].qty == 2;
            const Result r2 = Move(a, 0, b, 0);
            const Result r3 = Move(a, 0, b, 1);   // into an empty slot: whole
            const bool whole = r2 == Result::StackFull && r3 == Result::Moved && a.slots[0].Empty() && b.slots[1].qty == 2;
            Container c = MakePockets();
            Add(c, Id("bandage"), 6);
            const Result r4 = Move(c, 0, c, 1, 2);   // 2 of 6 into an empty slot of the same container
            const Result r5 = Move(c, 1, c, 0);      // back: merge
            const bool same = r4 == Result::Moved && c.slots[1].qty == 0 && c.slots[0].qty == 6 && r5 == Result::Merged;
            Check("inventory: a move is whole when it fits, the part that fits onto a stack with less room (water 3 onto 9 / 10: 1 moves), refused on a full stack, "
                  "and a split part merges back",
                  part && whole && same, F("r1 %s r2 %s r3 %s r4 %s r5 %s", ResultName(r1), ResultName(r2), ResultName(r3), ResultName(r4), ResultName(r5)));
        }
        {
            // Weight: a stack that does not fit moves the part that fits; nothing fits = refused. Cash weighs nothing: always fits.
            Container a = MakePockets(), b = MakePockets();
            Add(b, Id("fuel_can"), 1);
            Add(b, Id("fuel_can"), 1);
            Add(b, Id("toolkit"), 1);
            Add(b, Id("toolkit"), 1);   // 12.4 kg of 15
            Add(a, Id("water"), 10);    // 5 kg
            const Result r1 = Move(a, 0, b, 5);   // room for 2.6 kg = 5 bottles
            const bool part = r1 == Result::MovedPart && b.slots[5].qty == 5 && a.slots[0].qty == 5 && Grams(b) == 14900;
            const Result r2 = Move(a, 0, b, 6);   // 0.1 kg left: none
            Add(a, Id("cash"), 5000);
            const int cashAt = QuickTarget(a, Id("cash"));
            const Result r3 = Move(a, (int)(cashAt >= 0 ? cashAt : 1), b, 7);
            const bool cash = (r3 == Result::Moved || r3 == Result::Merged) && Grams(b) == 14900;
            Check("inventory: over the weight cap only the part that fits moves (5 of 10 water into 2.6 kg of room); with no room nothing moves; cash weighs nothing",
                  part && r2 == Result::Weight && cash, F("r1 %s (%u moved, %llu g) r2 %s r3 %s", ResultName(r1), (unsigned)b.slots[5].qty,
                                                         (unsigned long long)Grams(b), ResultName(r2), ResultName(r3)));
        }
        {
            // A different item swaps (whole stacks only, both weights must still fit); a part of a stack onto a different item is refused
            Container a = MakePockets(), b = MakePockets();
            Add(a, Id("medkit"), 2);
            Add(b, Id("radio"), 1);
            const Result part = Move(a, 0, b, 0, 1);
            const Result sw = Move(a, 0, b, 0);
            const bool swapped = sw == Result::Swapped && a.slots[0].item == Id("radio") && b.slots[0].item == Id("medkit") && b.slots[0].qty == 2;
            // weight-blocked swap: b nearly full, a heavy fuel can would push it over
            Container c = MakePockets(), d = MakePockets();
            Add(c, Id("fuel_can"), 1);
            Add(d, Id("toolkit"), 1);
            Add(d, Id("toolkit"), 1);
            Add(d, Id("toolkit"), 1);
            Add(d, Id("toolkit"), 1);
            Add(d, Id("toolkit"), 1);   // 11 kg
            Add(d, Id("phone"), 1);     // 11.2 kg; swapping phone (0.2) for fuel (4) = 15.0: fits exactly
            const Result exact = Move(c, 0, d, 5);
            Add(d, Id("id_card"), 1);   // + 0.02: now the reverse swap of the can for the phone would leave d at 11.22 (fine) and c at 4.0 (fine)
            Container e = MakePockets(), f = MakePockets();
            Add(e, Id("fuel_can"), 1);
            for (int i = 0; i < 6; ++i)
                Add(f, Id("toolkit"), 1);   // 13.2 kg
            Add(f, Id("notebook"), 1);      // 13.4; swap notebook (0.2) for fuel (4) = 17.2: over
            const Result over = Move(e, 0, f, 6);
            Check("inventory: a different item swaps whole stacks (medkit x2 for a radio); a part onto it is refused; a swap that would put either side over its cap "
                  "is refused, one landing exactly on the cap is allowed",
                  part == Result::Occupied && swapped && exact == Result::Swapped && over == Result::Weight,
                  F("part %s swap %s exact %s over %s", ResultName(part), ResultName(sw), ResultName(exact), ResultName(over)));
        }
        {
            // Split: n of a stack into an empty slot (1 <= n < qty); the dialog's fractions round and stay within 1..qty-1
            Container a = MakePockets();
            Add(a, Id("bandage"), 6);
            const Result s1 = Split(a, 0, a, 3, 2);
            const bool split = s1 == Result::Moved && a.slots[0].qty == 4 && a.slots[3].qty == 2;
            const Result whole = Split(a, 0, a, 4, 4);
            const Result occ = Split(a, 0, a, 3, 1);
            const bool fr = SplitAmount(6, 1, 2) == 3 && SplitAmount(6, 1, 3) == 2 && SplitAmount(6, 2, 3) == 4 && SplitAmount(6, 1, 4) == 2 &&
                            SplitAmount(6, 3, 4) == 5 && SplitAmount(2, 3, 4) == 1 && SplitAmount(2, 1, 4) == 1 && SplitAmount(1, 1, 2) == 0;
            Check("inventory: a split moves n (1..qty-1) into an empty slot only; the 1/2, 1/3, 2/3, 1/4, 3/4 chips give 3, 2, 4, 2, 5 of 6 and stay within 1..qty-1",
                  split && whole == Result::Nothing && occ == Result::Occupied && fr, F("s1 %s whole %s occ %s", ResultName(s1), ResultName(whole), ResultName(occ)));
        }
        {
            // Add fills stacks with room first, then empty slots, within the weight cap; quick move targets a stack with room before an empty slot
            Container a = MakePockets();
            Add(a, Id("water"), 4);
            Add(a, Id("phone"), 1);
            const uint32_t added = Add(a, Id("water"), 12);   // 6 onto the stack (to 10), 6 in a new slot
            const bool fill = added == 12 && a.slots[0].qty == 10 && a.slots[1].item == Id("phone") && a.slots[2].qty == 6;
            const uint32_t capped = Add(a, Id("fuel_can"), 5);   // 8 kg + 0.2 used: room for 6.8 kg = 1 can
            const int qt = QuickTarget(a, Id("water"));
            const int qtNew = QuickTarget(a, Id("radio"));
            Check("inventory: adding fills stacks with room first (water 4 + 12 = 10 + 6), then empty slots, never past the weight cap (1 of 5 fuel cans); quick move "
                  "aims at a stack with room before the first empty slot",
                  fill && capped == 1 && qt == 2 && qtNew == 4, F("added %u capped %u qt %d qtNew %d", (unsigned)added, (unsigned)capped, qt, qtNew));
        }
        {
            // Load: under 90 percent normal, 90 or more heavy, the cap full
            const bool ok = LoadOf(13499, 15000) == Load::Normal && LoadOf(13500, 15000) == Load::Heavy && LoadOf(14999, 15000) == Load::Heavy &&
                            LoadOf(15000, 15000) == Load::Full && LoadOf(5, 0) == Load::Normal && LoadOf(25600, 27000) == Load::Heavy;
            Check("inventory: load is normal under 90 percent, heavy from 90 (13.5 of 15 kg; the spec's 25.6 / 27), full at the cap; no cap is never heavy", ok);
        }
        {
            // Backpacks: the tiers, carry caps 20 / 27 / 40 kg, a pack never goes into a pack, it must be empty to take off or replace
            Character ch;
            Add(ch.pockets, Id("daypack"), 1);
            Add(ch.pockets, Id("rucksack"), 1);
            const Result eq = EquipFrom(ch, ch.pockets, 0, Equip::Pack);
            const bool worn = eq == Result::Swapped && ch.PackTierNow() == 8 && ch.pack.slots.size() == 8 && ch.CarryCap() == 27000 && ch.pockets.slots[0].Empty();
            const Result packInPack = Move(ch.pockets, 1, ch.pack, 0);
            Add(ch.pack, Id("water"), 2);
            const Result full = EquipFrom(ch, ch.pockets, 1, Equip::Pack);
            const Result off = Unequip(ch, Equip::Pack, ch.pockets, 5);
            Move(ch.pack, 0, ch.pockets, 6);
            const Result rep = EquipFrom(ch, ch.pockets, 1, Equip::Pack);   // empty now: the rucksack goes on, the daypack comes back to slot 1
            const bool replaced = rep == Result::Swapped && ch.PackTierNow() == 16 && ch.CarryCap() == 40000 && ch.pockets.slots[1].item == Id("daypack") &&
                                  ch.pack.slots.size() == 16;
            const bool tiers = PackFor(4).slots == 4 && PackFor(4).maxGrams == 5000 && std::string(PackFor(8).name) == "DAYPACK" && PackFor(16).maxGrams == 25000 &&
                               PackFor(3).slots == 0;
            Check("inventory: a worn pack gives its tier's slots and carry cap (daypack 8 / 27 kg, rucksack 16 / 40 kg); a pack never goes into a pack; a pack with "
                  "items is neither taken off nor replaced; an empty one swaps",
                  worn && packInPack == Result::WrongSlot && full == Result::PackNotEmpty && off == Result::PackNotEmpty && replaced && tiers,
                  F("eq %s pip %s full %s off %s rep %s", ResultName(eq), ResultName(packInPack), ResultName(full), ResultName(off), ResultName(rep)));
        }
        {
            // Equipment: only an item worn in that slot; the worn one comes back; taking off needs an empty slot and room
            Character ch;
            Add(ch.pockets, Id("cap"), 1);
            Add(ch.pockets, Id("jeans"), 1);
            const Result wrong = EquipFrom(ch, ch.pockets, 0, Equip::Pants);
            const Result hat = EquipFrom(ch, ch.pockets, 0, Equip::Hat);
            const Result notWorn = EquipFrom(ch, ch.pockets, 1, Equip::Hat);
            const Result offTaken = Unequip(ch, Equip::Hat, ch.pockets, 1);
            const Result off = Unequip(ch, Equip::Hat, ch.pockets, 2);
            const bool ok = wrong == Result::WrongSlot && hat == Result::Swapped && notWorn == Result::WrongSlot && offTaken == Result::Occupied && off == Result::Moved &&
                            ch.pockets.slots[2].item == Id("cap") && ch.equip[EquipIndex(Equip::Hat)].Empty() && std::string(EquipCaption(Equip::Shoes)) == "SHOES" &&
                            std::string(EquipGlyph(Equip::Neck)) == "chain";
            Check("inventory: equipment takes only an item worn in that slot (a cap on HAT, not on PANTS); taking off needs an empty slot", ok,
                  F("wrong %s hat %s notWorn %s offTaken %s off %s", ResultName(wrong), ResultName(hat), ResultName(notWorn), ResultName(offTaken), ResultName(off)));
        }
        {
            // Quick slots are references: they follow the item when it moves, go dim when it runs out, re-link when the same item arrives
            Character ch;
            Add(ch.pockets, Id("medkit"), 2);
            QuickRef& q = ch.quick[1];
            q.item = Id("medkit");
            q.index = 0;
            Move(ch.pockets, 0, ch.pockets, 9);
            const bool followed = Resolve(ch, q) && q.kind == Kind::Pockets && q.index == 9;
            ch.pockets.slots[9].Clear();
            const bool dim = !Resolve(ch, q) && q.item == Id("medkit");
            Add(ch.pockets, Id("medkit"), 1);
            const bool relinked = Resolve(ch, q) && q.index == 0;
            Check("inventory: a quick slot follows its item when it moves (slot 0 -> 9), stays bound but unresolved when it runs out, and re-links when a medkit arrives",
                  followed && dim && relinked, F("index %d", q.index));
        }
        {
            // Vehicle storage by GTA class: SUV 32 / 110 kg, motorcycle top case 4 / 15 kg without a glovebox, van 48 / 160 kg, a bicycle nothing
            const VehicleStorage suv = StorageForClass(2), moto = StorageForClass(8), van = StorageForClass(12), bike = StorageForClass(13),
                                 compact = StorageForClass(0), sedan = StorageForClass(1), coupe = StorageForClass(3), boat = StorageForClass(14);
            const bool ok = suv.trunkSlots == 32 && suv.trunkGrams == 110000 && suv.glovebox && moto.trunkSlots == 4 && moto.trunkGrams == 15000 && !moto.glovebox &&
                            van.trunkSlots == 48 && van.trunkGrams == 160000 && bike.trunkSlots == 0 && !bike.glovebox && compact.trunkSlots == 12 &&
                            sedan.trunkSlots == 24 && coupe.trunkGrams == 60000 && boat.trunkSlots == 0 && kGloveboxSlots == 8 && kGloveboxGrams == 12000;
            Check("inventory: vehicle storage follows section 8's class table (compact 12, sedan 24, SUV 32, van 48; motorcycle 4 and no glovebox; bicycles and boats none)", ok);
        }
        {
            // The quantity tag and the kilogram texts
            const bool tag = QtyTag(1).empty() && QtyTag(0).empty() && QtyTag(6) == "\xC3\x97" "6" && QtyTag(999) == "\xC3\x97" "999" && QtyTag(1000) == "1k" &&
                             QtyTag(1240) == "1.2k" && QtyTag(1999) == "1.9k" && QtyTag(300) == "\xC3\x97" "300" && QtyTag(1000000) == "1M" && QtyTag(1250000) == "1.2M";
            const bool kg = Kg(14200) == "14.2" && Kg(27000) == "27.0" && Kg(0) == "0.0" && KgCap(27000) == "27" && KgCap(12500) == "12.5";
            Check("inventory: the quantity tag is empty for 1, x6 / x999 below 1000, then 1k / 1.2k / 1.9k (truncated, .0 dropped) and 1M; kilograms to one decimal",
                  tag && kg, QtyTag(1240) + " " + QtyTag(1999) + " " + Kg(14200));
        }
        {
            // Apply (what the server runs and the overlay predicts): a drag onto equipment wears it, a worn item dragged onto the same kind swaps,
            // Shift+click goes pockets -> pack, USE consumes one usable unit and names it, a tool is not usable, keys 1..4 use the bound stack
            const auto op = [](OpKind k, Where s, int si, Where d = Where::Pockets, int di = -1, uint32_t q = 0) {
                Op o;
                o.kind = k; o.src = s; o.si = (int16_t)si; o.dst = d; o.di = (int16_t)di; o.qty = q;
                return o;
            };
            Character ch;
            Add(ch.pockets, Id("daypack"), 1);   // slot 0
            Add(ch.pockets, Id("cap"), 1);       // 1
            Add(ch.pockets, Id("medkit"), 2);    // 2
            Add(ch.pockets, Id("radio"), 1);     // 3
            Add(ch.pockets, Id("sunglasses"), 1);   // 4
            const int hat = EquipIndex(Equip::Hat), pack = EquipIndex(Equip::Pack);
            const Result wear = Apply(ch, op(OpKind::Move, Where::Pockets, 0, Where::Equip, pack)).r;
            const bool packOn = wear == Result::Swapped && ch.HasPack() && ch.pack.slots.size() == 8;
            const Result wrong = Apply(ch, op(OpKind::Move, Where::Pockets, 1, Where::Equip, EquipIndex(Equip::Pants))).r;
            Apply(ch, op(OpKind::Use, Where::Pockets, 1));                                  // USE on a wearable wears it
            Add(ch.pockets, Id("cap"), 1);                                                    // a second cap -> slot 0 (the first empty)
            const Result swap = Apply(ch, op(OpKind::Move, Where::Equip, hat, Where::Pockets, 0)).r;   // worn cap onto the pocket cap: swapped
            const Result quick = Apply(ch, op(OpKind::QuickMove, Where::Pockets, 2)).r;     // medkit x2 -> pack slot 0
            const bool inPack = ch.pack.slots[0].item == Id("medkit") && ch.pack.slots[0].qty == 2 && ch.pockets.slots[2].Empty();
            const Result bind = Apply(ch, op(OpKind::Bind, Where::Pack, 0, Where::Quick, 1)).r;
            const ApplyOut used = Apply(ch, op(OpKind::UseQuick, Where::Quick, 1));
            const bool usedOne = used.r == Result::Used && used.used == Id("medkit") && ch.pack.slots[0].qty == 1;
            const Result tool = Apply(ch, op(OpKind::Use, Where::Pockets, 3)).r;
            const Result packOff = Apply(ch, op(OpKind::Use, Where::Equip, pack)).r;        // the pack holds a medkit: must be emptied first
            const Result dropPack = Apply(ch, op(OpKind::Drop, Where::Equip, pack)).r;
            Apply(ch, op(OpKind::Drop, Where::Pack, 0));
            const Result offNow = Apply(ch, op(OpKind::QuickMove, Where::Equip, pack)).r;
            const bool ok = packOn && wrong == Result::WrongSlot && swap == Result::Swapped && quick == Result::Moved && inPack && bind == Result::Bound && usedOne &&
                            tool == Result::NotUsable && packOff == Result::PackNotEmpty && dropPack == Result::PackNotEmpty && offNow == Result::Moved && !ch.HasPack();
            Check("inventory: Apply wears a daypack (8 slots), refuses a cap on PANTS, swaps a worn cap with a pocket cap, Shift+click moves pockets -> pack, key 2 uses "
                  "one bound medkit, a radio is not usable, a pack with a medkit cannot come off or drop, an empty one does",
                  ok, F("wear %s wrong %s swap %s quick %s bind %s used %s tool %s off %s drop %s offNow %s", ResultName(wear), ResultName(wrong), ResultName(swap),
                        ResultName(quick), ResultName(bind), ResultName(used.r), ResultName(tool), ResultName(packOff), ResultName(dropPack), ResultName(offNow)));
        }
        {
            // The wire / save form round-trips, and refuses a corrupt one: a stack over its size, an unknown item, a cap worn on SHOES, a pack of the
            // wrong size, a trailing byte, a different version
            Character ch;
            Add(ch.pockets, Id("cash"), 1240);
            Add(ch.pockets, Id("toolkit"), 1, 40);
            ch.equip[EquipIndex(Equip::Pack)] = Slot{Id("rucksack"), 1, kNoCondition};
            ch.pack = MakePack(16);
            Add(ch.pack, Id("water"), 6);
            ch.quick[2] = QuickRef{Id("water"), Kind::Pack, 0};
            const std::string b = Encode(ch);
            Character back;
            const bool round = Decode(b, back) && Encode(back) == b && back.pack.slots.size() == 16 && back.pockets.slots[1].cond == 40 &&
                               back.quick[2].kind == Kind::Pack && back.quick[2].item == Id("water");
            const auto bad = [&](size_t at, uint8_t v) { std::string c = b; c[at] = (char)v; Character x; return !Decode(c, x); };
            // byte 1..: pocket slot 0 = item(2) qty(4) cond(1); slot 0 holds cash
            Character over;
            over.pockets.slots[0] = Slot{Id("water"), 11, kNoCondition};
            Character unknown;
            unknown.pockets.slots[0] = Slot{999, 1, kNoCondition};
            Character worn;
            worn.equip[EquipIndex(Equip::Shoes)] = Slot{Id("cap"), 1, kNoCondition};
            Character x;
            const bool refuses = !Decode(Encode(over), x) && !Decode(Encode(unknown), x) && !Decode(Encode(worn), x) && bad(0, 3) &&
                                 bad(1 + 16 * 7 + 14 * 7, 8) && !Decode(b + std::string(1, '\0'), x) && !Decode(b.substr(0, b.size() - 1), x);
            Op o;
            o.kind = OpKind::Split; o.src = Where::Pack; o.si = 3; o.dst = Where::Pockets; o.di = 15; o.qty = 4;
            Op o2;
            const bool opRound = DecodeOp(EncodeOp(o), o2) && o2.kind == o.kind && o2.src == o.src && o2.si == 3 && o2.dst == o.dst && o2.di == 15 && o2.qty == 4 &&
                                 EncodeOp(o).size() == 11 && !DecodeOp(std::string(11, '\x09'), o2);
            Check("inventory: the wire / save form round-trips (cash 1240, a 40 % toolkit, a rucksack with water, a pack-bound quick slot) and refuses a stack over "
                  "its size, an unknown item, a cap worn as shoes, a pack of the wrong size, a short or long buffer and another version; an Op is 11 bytes",
                  round && refuses && opRound, F("bytes %zu round %d refuses %d op %d", b.size(), (int)round, (int)refuses, (int)opRound));
        }
        {
            // Grants (admin /give, the starter kit): the starter kit decodes (it is a state the rules accept) and has no pack; Give fills the
            // pockets first, then a worn pack, and stops at the weight cap (20 fuel cans of 4 kg: 3 fit in 15 kg of pockets, 6 more in a
            // rucksack's 25 kg); a pack is never given into the pack. 0.5.4 (GI: "remove dummy items from inventory"): the pockets start EMPTY.
            Character kit = StarterKit();
            Character back;
            const bool kitOk = Decode(Encode(kit), back) && !kit.HasPack() && UsedSlots(kit.pockets) == 0 &&
                               kit.equip[EquipIndex(Equip::Shirt)].item == Id("tshirt") && kit.equip[EquipIndex(Equip::Shoes)].item == Id("sneakers");
            const uint32_t noPack = Give(kit, Id("fuel_can"), 20);
            Character big = StarterKit();
            big.equip[EquipIndex(Equip::Pack)] = Slot{Id("rucksack"), 1, kNoCondition};
            big.pack = MakePack(16);
            const uint32_t withPack = Give(big, Id("fuel_can"), 20);
            Character p2 = StarterKit();
            p2.equip[EquipIndex(Equip::Pack)] = Slot{Id("pouch"), 1, kNoCondition};
            p2.pack = MakePack(4);
            for (int i = 0; i < kPocketSlots; ++i)
                if (p2.pockets.slots[(size_t)i].Empty())
                    p2.pockets.slots[(size_t)i] = Slot{Id("notebook"), 1, kNoCondition};
            const uint32_t packInPack = Give(p2, Id("daypack"), 1);
            Check("inventory: the starter kit is a valid state (empty pockets, tee / jeans / sneakers worn, no pack); Give fills pockets then the pack "
                  "within weight (20 fuel cans: 3 without a pack, 9 with a rucksack) and never puts a pack in the pack",
                  kitOk && noPack == 3 && withPack == 9 && packInPack == 0, F("kit %d noPack %u withPack %u packInPack %u", (int)kitOk, noPack, withPack, packInPack));
        }
        {
            // 0.5.6 (GI: "binding to slot and then clicking 1-2-3-4 doesn't bring the gun into active slot"). A pistol and a carbine in the
            // pockets, bound to quick slots 1 and 2: key 1 takes the pistol into the hand, key 2 swaps the carbine in (the pistol goes back),
            // key 2 again puts the carbine away (GTA's own "1 = unarmed" is held off with weapon_keys, so the bound key is the holster), and
            // key 1 brings the pistol back. Before 0.5.6 the third press was refused (NotUsable): no key could empty the hand.
            const ItemDef* pistol = FindWeapon(gage::sync::inv::Joaat("weapon_pistol"));
            const ItemDef* carbine = FindWeapon(gage::sync::inv::Joaat("weapon_carbinerifle"));
            Character ch = StarterKit();
            const bool gave = pistol && carbine && Give(ch, pistol->id, 1) == 1 && Give(ch, carbine->id, 1) == 1;
            const auto op = [&](OpKind k, Where src, int si, Where dst = Where::Pockets, int di = -1) {
                Op o;
                o.kind = k; o.src = src; o.si = (int16_t)si; o.dst = dst; o.di = (int16_t)di;
                return Apply(ch, o).r;
            };
            const auto pocketOf = [&](ItemId id) { for (size_t i = 0; i < ch.pockets.slots.size(); ++i) if (ch.pockets.slots[i].item == id) return (int)i; return -1; };
            const bool bound = gave && Ok(op(OpKind::Bind, Where::Pockets, pocketOf(pistol->id), Where::Quick, 0)) &&
                               Ok(op(OpKind::Bind, Where::Pockets, pocketOf(carbine->id), Where::Quick, 1));
            const Result r1 = bound ? op(OpKind::UseQuick, Where::Quick, 0) : Result::BadSlot;
            const bool k1 = Ok(r1) && ch.hand.item == pistol->id && pocketOf(pistol->id) < 0;
            const Result r2 = op(OpKind::UseQuick, Where::Quick, 1);
            const bool k2 = Ok(r2) && ch.hand.item == carbine->id && pocketOf(pistol->id) >= 0 && pocketOf(carbine->id) < 0;
            const Result r3 = op(OpKind::UseQuick, Where::Quick, 1);
            const bool k3 = Ok(r3) && ch.hand.Empty() && pocketOf(carbine->id) >= 0 && pocketOf(pistol->id) >= 0;
            const Result r4 = op(OpKind::UseQuick, Where::Quick, 0);
            const bool k4 = Ok(r4) && ch.hand.item == pistol->id;
            Check("inventory (0.5.6): quick keys bring a bound weapon into the hand (1: pistol, 2: the carbine swaps in), the key of the weapon in hand "
                  "puts it away, and the next key takes the pistol again",
                  k1 && k2 && k3 && k4, F("bound %d results %d %d %d %d", (int)bound, (int)r1, (int)r2, (int)r3, (int)r4));
        }
        {
            // The toggle never fires on an item that is not in the hand (food in slot 3 is still eaten), and an empty slot is still refused.
            Character ch = StarterKit();
            Give(ch, Id("water"), 2);
            Op b;
            b.kind = OpKind::Bind; b.src = Where::Pockets; b.si = 0; b.dst = Where::Quick; b.di = 2;
            Op q;
            q.kind = OpKind::UseQuick; q.src = Where::Quick; q.si = 2;
            Op e;
            e.kind = OpKind::UseQuick; e.src = Where::Quick; e.si = 3;
            const bool bound = Ok(Apply(ch, b).r);
            const ApplyOut used = Apply(ch, q);
            const Result empty = Apply(ch, e).r;
            Check("inventory (0.5.6): a quick slot with food still uses one unit (Used, 1 water left), an unbound slot is still NotUsable",
                  bound && used.r == Result::Used && used.used == Id("water") && ch.pockets.slots[0].qty == 1 && empty == Result::NotUsable,
                  F("used %d empty %d", (int)used.r, (int)empty));
        }
    }

} // namespace invtest
