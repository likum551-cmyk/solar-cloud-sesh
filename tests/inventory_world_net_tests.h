#pragma once
// 0.5.4 inventory world over the wire (protocol v8 InvSnap 52 / InvNearby 53 / InvReport 54, the 19-byte InvOp; apps/server WorldOp,
// OnInvReport, TickInventoryWorld): the real server and the real ClientSession on loopback. Included by tests/server_test.cpp after
// inventory_net_tests.h (it reuses v8acct's helpers). Every check runs on the passing path (kChecks of them). The pure rules (piles, trunks,
// the reconcile and its dupe race) are tests/inventory_world_tests.h in gameplay_test.

#include "sync/inventory_world.h"
#include "sync/outfit.h"   // 0.5.6: WornFeed, Desired (the engine's outfit source)

#include <cstdarg>
#include <cstdio>

namespace invworldnet {

    constexpr int kChecks = 18;   // 0.5.6: + 3 (OutfitAndQuickSlots)
    namespace inv = gage::sync::inv;
    namespace invw = gage::sync::invw;

    static int QtyOf(const inv::Character& ch) { return inv::UsedSlots(ch.pockets) + (ch.HasPack() ? inv::UsedSlots(ch.pack) : 0); }
    static int UsedSlotsOf(const inv::Character& ch) { return QtyOf(ch); }
    // server_test has no F(): a small printf formatter for the check details
    static std::string Fmt(const char* fmt, ...)
    {
        char buf[256];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        return buf;
    }

    constexpr uint32_t kPistol = 0x1B06D571u;   // WEAPON_PISTOL
    constexpr uint32_t kAdder = 0xB779A091u;    // a model hash the tests spawn (the server never interprets it)

    // A 0.5.4 client: answers the offer with InvReports, reads InvSnap / InvNearby, sends ops in either form.
    struct WClient
    {
        Rig& rig;
        Probe& p;
        uint32_t seq = 0;

        bool Offered() { p.Drain(); return p.Count(ET::InvNearby) > 0; }
        const proto::InvSnap* LastSnap() const
        {
            for (auto it = p.events.rbegin(); it != p.events.rend(); ++it)
                if (it->type == ET::InvSnap) return &it->invSnap;
            return nullptr;
        }
        uint32_t Rev() const { const proto::InvSnap* s = LastSnap(); return s ? s->rev : 0; }
        bool Latest(inv::Character& out) const { const proto::InvSnap* s = LastSnap(); return s && inv::Decode(s->blob, out); }
        const proto::InvNearby* LastNearby() const
        {
            for (auto it = p.events.rbegin(); it != p.events.rend(); ++it)
                if (it->type == ET::InvNearby) return &it->invNearby;
            return nullptr;
        }
        // The newest nearby box of `kind`, decoded (false when none).
        bool Box(inv::Kind kind, proto::InvBox& raw, inv::Container& c) const
        {
            const proto::InvNearby* n = LastNearby();
            if (!n) return false;
            for (const proto::InvBox& b : n->boxes)
                if (b.kind == (uint8_t)kind)
                {
                    raw = b;
                    c = inv::Container();
                    return b.blob.empty() || inv::DecodeBox(b.blob, c);
                }
            return false;
        }
        bool Report(uint32_t current, const std::vector<proto::InvPedWeapon>& w, const std::vector<proto::InvVehClass>& vehs = {}, bool valid = true,
                    int rev = -1)
        {
            proto::InvReport r;
            r.rev = rev < 0 ? Rev() : (uint32_t)rev;
            r.current = current;
            r.flags = valid ? proto::kInvReportPedValid : 0;
            r.weapons = w;
            r.vehicles = vehs;
            return p.s.SendInvReport(r);
        }
        // Waits until more than `before` InvSnaps arrived.
        bool WaitSnap(int before, int ms = 1500)
        {
            return rig.Until([&] { p.Drain(); return p.Count(ET::InvSnap) > before; }, ms);
        }
        bool WaitNearby(const std::function<bool(const proto::InvNearby&)>& pred, int ms = 1500)
        {
            return rig.Until([&] { p.Drain(); const proto::InvNearby* n = LastNearby(); return n && pred(*n); }, ms);
        }
        // One op answered by the InvSnap carrying its seq.
        bool Op(const inv::Op& op, inv::Character& out)
        {
            proto::InvOp m;
            m.seq = ++seq;
            m.op = inv::EncodeOp(op);
            if (!p.s.SendInvOp(m)) return false;
            const uint32_t want = m.seq;
            bool got = false;
            rig.Until([&] {
                p.Drain();
                for (const net::Event& e : p.events)
                    if (e.type == ET::InvSnap && e.invSnap.ackSeq == want) got = true;
                return got;
            }, 2000);
            if (!got) return false;
            for (auto it = p.events.rbegin(); it != p.events.rend(); ++it)
                if (it->type == ET::InvSnap && it->invSnap.ackSeq == want) return inv::Decode(it->invSnap.blob, out);
            return false;
        }
    };

    static inv::Op MkOp(inv::OpKind k, inv::Where src, int si, inv::Where dst = inv::Where::Pockets, int di = 0, uint32_t sb = 0, uint32_t db = 0)
    {
        inv::Op o;
        o.kind = k; o.src = src; o.si = (int16_t)si; o.dst = dst; o.di = (int16_t)di; o.qty = 0; o.srcBox = sb; o.dstBox = db;
        return o;
    }
    static proto::VehicleResult VCmd(Rig& rig, Probe& p, proto::VehicleOp op, uint16_t veh, uint32_t arg)
    {
        proto::VehicleCmd c;
        c.op = op;
        c.vehId = veh;
        c.arg = arg;
        proto::VehicleResult none;
        none.code = (proto::VehicleResultCode)255;
        const int before = p.Count(ET::VehicleResult);
        if (!p.s.SendVehicleCmd(c)) return none;
        rig.Until([&] { p.Drain(); return p.Count(ET::VehicleResult) > before; }, 2000);
        for (auto it = p.events.rbegin(); it != p.events.rend(); ++it)
            if (it->type == ET::VehicleResult) return it->vehicleResult;
        return none;
    }
    // Join, wait for the offer, answer it (an unarmed ped, nothing nearby) and wait for the first InvSnap.
    static bool JoinWorld(Rig& rig, Probe& p, WClient& w, const char* name)
    {
        if (!p.Join(name)) return false;
        if (!rig.Until([&] { return w.Offered(); }, 2000)) return false;
        const int before = p.Count(ET::InvSnap);
        return w.Report(inv::kWeaponUnarmed, {}, {}, true, 0) && w.WaitSnap(before);
    }

    // ---- the handshake, a 0.5.3-style client beside it, weapons from reports and /gun ----
    static void HandshakeAndWeapons()
    {
        Rig rig;
        if (!rig.Start()) { Check("inventory world live: server starts", false); return; }
        Probe ana(rig), old(rig);
        WClient a{rig, ana};

        // 1. the offer and the answer
        const bool joined = ana.Join("Ana");
        const bool offered = joined && rig.Until([&] { return a.Offered(); }, 2000);
        int invStateAt = -1, nearbyAt = -1, i = 0;
        for (const net::Event& e : ana.events)
        {
            if (e.type == ET::InvState && invStateAt < 0) invStateAt = i;
            if (e.type == ET::InvNearby && nearbyAt < 0) nearbyAt = i;
            i++;
        }
        const proto::InvNearby* offer = a.LastNearby();
        std::string refusedGive;
        const bool giveRefused = rig.srv.Execute("give Ana weapon_pistol", refusedGive) == server::CommandResult::Error;
        const int s0 = ana.Count(ET::InvSnap);
        const bool reported = a.Report(inv::kWeaponUnarmed, {}, {}, true, 0);
        const bool snapped = a.WaitSnap(s0);
        const proto::InvSnap* first = a.LastSnap();
        inv::Character ch;
        const bool v2 = first && (uint8_t)first->blob[0] == inv::kCodecVersion && a.Latest(ch);
        Check("inventory world live: after the join's InvState the server offers an (empty) InvNearby; a weapon give is refused before the answer; "
              "the client's InvReport switches it to InvSnap (codec v2, rev 1, from-report flag) and the log says so",
              offered && invStateAt >= 0 && nearbyAt > invStateAt && offer && offer->boxes.empty() && giveRefused && reported && snapped && v2 &&
                  first->rev == 1 && (first->flags & proto::kInvSnapFromReport) && rig.srv.stats().invWorldClients == 1 && rig.LogHas("speaks 0.5.4"),
              ana.Describe() + " | " + refusedGive);

        // 2. a 0.5.3-style client (it never answers the offer) keeps InvState in codec v1 and never gets an InvSnap
        const bool oldIn = old.Join("Olaf");
        rig.Until([&] { old.Drain(); return old.Count(ET::InvState) > 0 && old.Count(ET::InvNearby) > 0; }, 2000);
        std::string out;
        const int os = old.Count(ET::InvState);
        rig.srv.Execute("give Olaf water", out);
        rig.Until([&] { old.Drain(); return old.Count(ET::InvState) > os; }, 2000);
        const net::Event* st = nullptr;
        for (auto it = old.events.rbegin(); it != old.events.rend(); ++it)
            if (it->type == ET::InvState) { st = &*it; break; }
        Check("inventory world live: a client that never answers the offer keeps InvState (codec v1) and gets no InvSnap",
              oldIn && st && (uint8_t)st->invState.blob[0] == inv::kCodecVersionLegacy && old.Count(ET::InvSnap) == 0, old.Describe());

        // 3. a weapon the ped gained (a pickup, a script, an old-style /gun) becomes an item: in hand with its 60 rounds; the same report again
        //    (with the new rev) changes nothing and sends nothing back
        const int s1 = ana.Count(ET::InvSnap);
        a.Report(kPistol, {{kPistol, 0xA1, 60}});
        const bool gained = a.WaitSnap(s1);
        inv::Character g;
        const bool inHand = gained && a.Latest(g) && g.hand.item == inv::FindWeapon(kPistol)->id && g.hand.ammo == 60 &&
                            (a.LastSnap()->flags & proto::kInvSnapFromReport) && a.Rev() == 2;
        const int s2 = ana.Count(ET::InvSnap);
        a.Report(kPistol, {{kPistol, 0xA1, 60}});
        rig.Pump(300);
        ana.Drain();
        Check("inventory world live: a reported pistol becomes the hand item with 60 rounds (InvSnap rev 2, from-report); the same report again sends "
              "nothing back (no loop)",
              inHand && ana.Count(ET::InvSnap) == s2 && rig.srv.stats().invWeaponGains == 1, ana.Describe());

        // 4. a stale report (stamped with rev 1) that claims a knife is ignored: the rev gate
        const uint64_t staleBefore = rig.srv.stats().invReportsStale;
        a.Report(0x99B507EAu, {{kPistol, 0xA1, 60}, {0x99B507EAu, 0, 0}}, {}, true, 1);
        rig.Pump(300);
        inv::Character srvCopy;
        rig.srv.InventoryOf("Ana", srvCopy);
        Check("inventory world live: a report stamped with an older rev (a knife in it) is ignored: no knife, counted stale",
              !invw::Carries(srvCopy, 0x99B507EAu) && rig.srv.stats().invReportsStale == staleBefore + 1);

        // 5. the admin /gun goes through the inventory: the carbine lands in the pockets (the hand holds the pistol) with 250 rounds
        const int s3 = ana.Count(ET::InvSnap);
        const proto::AdminResult gun = v8acct::Admin(rig, ana, proto::AdminOp::Gun, "carbinerifle");
        const proto::AdminResult bad = v8acct::Admin(rig, ana, proto::AdminOp::Gun, "nosuchgun");
        a.WaitSnap(s3);
        inv::Character after;
        const bool hasCarbine = a.Latest(after) && invw::Carries(after, inv::Joaat("weapon_carbinerifle")) &&
                                invw::GroupTotalOf(after, inv::Joaat("weapon_carbinerifle"), invw::AmmoTypes()) == 250;
        Check("inventory world live: /gun carbinerifle puts the weapon in the inventory (pockets, 250 rounds, InvSnap) and answers Ok; an unknown "
              "weapon is BadArg",
              gun.code == proto::AdminCode::Ok && hasCarbine && bad.code == proto::AdminCode::BadArg && rig.LogHas("via the inventory"),
              ana.Describe());
    }

    // ---- the ground: a drop for everyone in reach, a pickup, a pickup from too far, expiry, a malformed report ----
    static void Ground()
    {
        Rig rig;
        if (!rig.Start()) { Check("inventory world live: server starts (ground)", false); return; }
        Probe ana(rig), ben(rig), cy(rig);
        WClient a{rig, ana}, b{rig, ben}, c{rig, cy};
        const bool in = JoinWorld(rig, ana, a, "Ana") && JoinWorld(rig, ben, b, "Ben") && JoinWorld(rig, cy, c, "Cy");
        const bool placed = in && v8acct::Placed(rig, ana, 0, 0) && v8acct::Placed(rig, ben, 1.0f, 0) && v8acct::Placed(rig, cy, 10.0f, 0);
        // Ana holds a pistol (60) from a report
        const int s0 = ana.Count(ET::InvSnap);
        a.Report(kPistol, {{kPistol, 0xA1, 60}});
        a.WaitSnap(s0);

        // 6. Ana drops it from the hand: gone from her inventory, a pile at her feet, in HER nearby at once and in Ben's (1 m) within a world pass
        inv::Character dropped;
        const bool dr = a.Op(MkOp(inv::OpKind::Drop, inv::Where::Hand, 0), dropped);
        proto::InvBox raw;
        inv::Container pile;
        const bool aSees = a.WaitNearby([](const proto::InvNearby& n) { return !n.boxes.empty(); }) && a.Box(inv::Kind::Ground, raw, pile);
        const bool bSees = b.WaitNearby([](const proto::InvNearby& n) { return !n.boxes.empty(); }, 1000);
        proto::InvBox braw;
        inv::Container bpile;
        b.Box(inv::Kind::Ground, braw, bpile);
        Check("inventory world live: a dropped hand pistol leaves the inventory and lies in a ground pile (with its 60 rounds) that the dropper and a "
              "player 1 m away both see in NEARBY",
              placed && dr && dropped.hand.Empty() && aSees && pile.slots[0].item == inv::FindWeapon(kPistol)->id && pile.slots[0].ammo == 60 && bSees &&
                  braw.id == raw.id && rig.LogHas("dropped 1x weapon_pistol"),
              ana.Describe());

        // 7. Cy (10 m away) asks for it: refused, nothing moves; the server never lists the pile to him
        inv::Character cTry;
        const uint64_t refusedBefore = rig.srv.stats().invWorldRefused;
        const bool cAnswered = c.Op(MkOp(inv::OpKind::QuickMove, inv::Where::Nearby, 0, inv::Where::Pockets, 0, raw.id, 0), cTry);
        cy.Drain();
        const proto::InvNearby* cn = c.LastNearby();
        Check("inventory world live: a player 10 m away cannot take from the pile (refused, answered, counted) and is never shown it",
              cAnswered && !invw::Carries(cTry, kPistol) && rig.srv.stats().invWorldRefused == refusedBefore + 1 && cn && cn->boxes.empty());

        // 8. Ben takes it straight into his hand: he holds the pistol with 60 rounds; the emptied pile is gone for both
        inv::Character bGot;
        const bool took = b.Op(MkOp(inv::OpKind::Move, inv::Where::Nearby, 0, inv::Where::Hand, 0, raw.id, 0), bGot);
        const bool aEmpty = a.WaitNearby([](const proto::InvNearby& n) { return n.boxes.empty(); });
        Check("inventory world live: a player in reach takes the pistol from the pile into his hand (60 rounds); the empty pile disappears for everyone",
              took && bGot.hand.item == inv::FindWeapon(kPistol)->id && bGot.hand.ammo == 60 && aEmpty && rig.srv.InventoryWorld().Piles().empty(),
              ben.Describe());

        // 9. expiry: a dropped water bottle's pile goes once it is older than invGroundExpireSeconds (aged by the test hook)
        std::string o;
        const int sb = ana.Count(ET::InvSnap);
        rig.srv.Execute("give Ana water", o);
        a.WaitSnap(sb);
        inv::Character wet, dry;
        a.Latest(wet);
        int waterAt = -1;
        for (size_t k = 0; k < wet.pockets.slots.size(); k++)
            if (!wet.pockets.slots[k].Empty() && waterAt < 0) waterAt = (int)k;
        const bool drop2 = a.Op(MkOp(inv::OpKind::Drop, inv::Where::Pockets, waterAt), dry);
        const bool pileUp = rig.Until([&] { return rig.srv.InventoryWorld().Piles().size() == 1; }, 1000);
        rig.Pump(300);
        const bool stillThere = rig.srv.InventoryWorld().Piles().size() == 1;
        rig.srv.ExpireGround();
        const bool expired = rig.Until([&] { return rig.srv.InventoryWorld().Piles().empty(); }, 1000) &&
                             a.WaitNearby([](const proto::InvNearby& n) { return n.boxes.empty(); });
        Check("inventory world live: a dropped stack's pile stays until it is invGroundExpireSeconds old, then the world pass removes it and NEARBY empties",
              drop2 && pileUp && stillThere && expired && rig.srv.stats().invGroundExpired == 1);

        // 10. a malformed InvReport (a weapon hash 0) cuts the sender off (Protocol), like every malformed reliable message
        Probe mal(rig);
        const bool malIn = mal.Join("Mal");
        // type, rev u32, current u32, flags 1, one weapon {hash 0, type 0, ammo 0}, no vehicles: well framed, but hash 0
        const proto::Bytes rawRep = {(uint8_t)proto::Msg::InvReport, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        mal.s.SendRaw(proto::kChanReliable, rawRep, true);
        mal.WaitClosed(3000);
        mal.Drain();
        Check("inventory world live: an InvReport with a weapon hash 0 is refused (Protocol) and the connection ends",
              malIn && mal.RefusedWith(proto::RefuseReason::Protocol), mal.Describe());
    }

    // ---- trunks: the case list against the real vehicle store ----
    static void Trunks()
    {
        Rig rig;
        if (!rig.Start()) { Check("inventory world live: server starts (trunks)", false); return; }
        Probe ana(rig), ben(rig);
        WClient a{rig, ana}, b{rig, ben};
        const bool in = JoinWorld(rig, ana, a, "Ana") && JoinWorld(rig, ben, b, "Ben");
        const bool placed = in && v8acct::Placed(rig, ana, 0, 0) && v8acct::Placed(rig, ben, 20.0f, 0);
        const proto::VehicleResult spawn = VCmd(rig, ana, proto::VehicleOp::Spawn, 0, kAdder);   // 4 m ahead: (0, 4)
        const uint16_t vid = spawn.vehId;
        // no class known yet: nothing listed
        rig.Pump(400);
        ana.Drain();
        const proto::InvNearby* before = a.LastNearby();
        const bool noneYet = before && before->boxes.empty();
        // Ana's client reports the GTA class (1 = sedan: 24 slots); she stands 2 m from the car
        const int n0 = ana.Count(ET::InvNearby);
        a.Report(inv::kWeaponUnarmed, {}, {{vid, 1}});
        v8acct::Placed(rig, ana, 0, 2.0f);
        const bool listed = rig.Until([&] { ana.Drain(); return ana.Count(ET::InvNearby) > n0 && !a.LastNearby()->boxes.empty(); }, 1500);
        proto::InvBox t;
        inv::Container trunk;
        const bool isTrunk = a.Box(inv::Kind::Trunk, t, trunk) && t.veh == vid && t.cls == 1 && trunk.slots.size() == 24 && !(t.flags & proto::kInvBoxLocked) &&
                             t.model == kAdder;
        Check("inventory world live: a car's trunk is listed only once a client reported its class, then to a player standing 2 m away (sedan: 24 "
              "slots, the server's vehicle id and model)",
              placed && spawn.code == proto::VehicleResultCode::Ok && noneYet && listed && isTrunk, ana.Describe());

        // 11. Ana puts water into it; Ben 20 m away sees no trunk; Ben comes to 2 m and sees the water (unlocked car of somebody else's)
        std::string o;
        const int sb = ana.Count(ET::InvSnap);
        rig.srv.Execute("give Ana water 3", o);
        a.WaitSnap(sb);
        inv::Character putAfter;
        const bool put = a.Op(MkOp(inv::OpKind::Move, inv::Where::Pockets, 0, inv::Where::Nearby, 5, 0, t.id), putAfter);
        ben.Drain();
        const bool benFar = b.LastNearby() && b.LastNearby()->boxes.empty();
        v8acct::Placed(rig, ben, 1.5f, 4.0f);
        proto::InvBox bt;
        inv::Container btr;
        const bool benNear = b.WaitNearby([](const proto::InvNearby& n) { return !n.boxes.empty(); }) && b.Box(inv::Kind::Trunk, bt, btr) &&
                             btr.slots[5].item == inv::FindKey("water")->id && btr.slots[5].qty == 3;
        Check("inventory world live: water moved into the trunk; a player 20 m away is shown no trunk, at 2 m he sees it with the water (a stranger's "
              "unlocked car)",
              put && putAfter.pockets.slots[0].Empty() && benFar && benNear, ben.Describe());

        // 12. locked: Ana (the spawner, the key holder) locks it from outside; Ben (no key, outside) sees it locked with no contents and his op is
        //     refused; Ana (key) still sees it open; Ben as a PASSENGER (unlocked, he gets in, Ana locks again) sees it open
        const proto::VehicleResult lock = VCmd(rig, ana, proto::VehicleOp::SetLocked, vid, 1);
        const bool benLocked = b.WaitNearby([](const proto::InvNearby& n) { return !n.boxes.empty() && (n.boxes[0].flags & proto::kInvBoxLocked); }) &&
                               b.LastNearby()->boxes[0].blob.empty();
        inv::Character bTry;
        const bool bAnswered = b.Op(MkOp(inv::OpKind::QuickMove, inv::Where::Nearby, 5, inv::Where::Pockets, 0, bt.id, 0), bTry);
        ana.Drain();
        const bool anaOpen = a.LastNearby() && !a.LastNearby()->boxes.empty() && !(a.LastNearby()->boxes[0].flags & proto::kInvBoxLocked);
        const proto::VehicleResult unlock = VCmd(rig, ana, proto::VehicleOp::SetLocked, vid, 0);
        const proto::VehicleResult benIn = VCmd(rig, ben, proto::VehicleOp::Enter, vid, 1);
        const proto::VehicleResult relock = VCmd(rig, ana, proto::VehicleOp::SetLocked, vid, 1);
        rig.Pump(400);
        ben.Drain();
        const bool passengerOpen = b.LastNearby() && !b.LastNearby()->boxes.empty() && !(b.LastNearby()->boxes[0].flags & proto::kInvBoxLocked) &&
                                   b.LastNearby()->boxes[0].distCm == 0;
        Check("inventory world live: a locked car's trunk is listed locked with no contents to a stranger outside (his op refused), open to the key "
              "holder, and open to the same stranger once he sits in it as a passenger (distance 0)",
              lock.code == proto::VehicleResultCode::Ok && benLocked && bAnswered && !invw::Carries(bTry, kPistol) && QtyOf(bTry) == 0 && anaOpen &&
                  unlock.code == proto::VehicleResultCode::Ok && benIn.code == proto::VehicleResultCode::Ok && relock.code == proto::VehicleResultCode::Ok &&
                  passengerOpen,
              ben.Describe());

        // 13. the car goes (Ben gets out, Ana, its spawner, leaves: her empty car is destroyed): the trunk's water lands in a pile where the car
        //     stood, and Ben next to it sees the pile
        VCmd(rig, ben, proto::VehicleOp::Exit, vid, 0);
        v8acct::Placed(rig, ben, 1.0f, 4.0f);
        ana.s.Disconnect();
        const bool gone = rig.Until([&] { return rig.srv.FindVehicle(vid) == nullptr; }, 3000);
        proto::InvBox gp;
        inv::Container gpile;
        const bool dumped = b.WaitNearby([](const proto::InvNearby& n) { return !n.boxes.empty() && n.boxes[0].kind == (uint8_t)inv::Kind::Ground; }) &&
                            b.Box(inv::Kind::Ground, gp, gpile) && gpile.slots[0].item == inv::FindKey("water")->id && gpile.slots[0].qty == 3 &&
                            rig.srv.InventoryWorld().Trunks().empty();
        Check("inventory world live: when the car is destroyed (its spawner left) the trunk's 3 water land in a ground pile where it stood, which the "
              "player beside it sees; no trunk is left",
              gone && dumped && rig.LogHas("trunk(s) of gone vehicles emptied"), ben.Describe());
    }

    // ---- invWorld 0 (the 0.5.3 behaviour) and the config keys ----
    static void Off()
    {
        Rig rig;
        server::Config cfg = Rig::BaseConfig();
        cfg.invWorld = 0;
        if (!rig.Start(cfg)) { Check("inventory world live: server starts (off)", false); return; }
        Probe ana(rig);
        const bool in = ana.Join("Ana") && v8acct::Placed(rig, ana, 0, 0);
        rig.Pump(400);
        ana.Drain();
        std::string o;
        rig.srv.Execute("give Ana water", o);
        rig.Until([&] { ana.Drain(); return ana.Count(ET::InvState) >= 2; }, 1500);
        proto::InvOp m;
        m.seq = 1;
        m.op = inv::EncodeOp(MkOp(inv::OpKind::Drop, inv::Where::Pockets, 0));
        ana.s.SendInvOp(m);
        rig.Until([&] { ana.Drain(); return ana.Count(ET::InvState) >= 3; }, 1500);
        inv::Character srvCopy;
        rig.srv.InventoryOf("Ana", srvCopy);
        const char* path = "gage_server_test_invworld.json";
        WriteTextFile(path, "{ \"invWorld\": 0, \"invGroundReach\": 3, \"invTrunkReach\": 6, \"invGroundExpireSeconds\": 120, \"invMaxPiles\": 100 }");
        server::Config c;
        std::string err, e2, e3;
        const bool loaded = server::LoadConfigFile(path, c, &err);
        WriteTextFile(path, "{ \"invGroundReach\": 21 }");
        server::Config c2;
        const bool farRefused = !server::LoadConfigFile(path, c2, &e2);
        std::remove(path);
        server::Config bad = Rig::BaseConfig();
        bad.invGroundExpireSeconds = 5;
        Check("inventory world live: with invWorld 0 nothing is offered (no InvNearby) and a Drop discards as in 0.5.3 (no pile); the five keys load, "
              "an out-of-range reach and a 5 s expiry are refused",
              in && ana.Count(ET::InvNearby) == 0 && UsedSlotsOf(srvCopy) == 0 && rig.srv.InventoryWorld().Piles().empty() && loaded && c.invWorld == 0 &&
                  c.invGroundReach == 3 && c.invTrunkReach == 6 && c.invGroundExpireSeconds == 120 && c.invMaxPiles == 100 && farRefused &&
                  !server::Validate(bad, &e3) && server::Config().invWorld == 1,
              err + " | " + e2 + " | " + ana.Describe());
    }

    // ---- 0.5.6: GI's two inventory bugs over the wire (the server and the session are the real ones; the engine's halves are the pure functions
    //      it runs: sync::outfit::WornFeed + Desired for the clothes, sync::invw::PlanSnapshot for the weapon in hand) ----
    // `wornFromSnaps` false replays the 0.5.4 / 0.5.5 engine (it fed the outfit from InvState only): the take-off check must fail with it.
    static int OutfitAndQuickSlots(bool wornFromSnaps = true)
    {
        namespace of = gage::sync::outfit;
        int failed = 0;
        const auto check = [&](const char* what, bool ok, const std::string& detail = std::string()) {
            Check(what, ok, detail);
            failed += ok ? 0 : 1;
        };
        Rig rig;
        if (!rig.Start()) { check("inventory 0.5.6 live: server starts", false); return 1; }
        Probe ana(rig);
        WClient a{rig, ana};
        const bool in = JoinWorld(rig, ana, a, "Ana");

        // 1. GI: "taking off clothing doesn't remove it off char". The shirt comes off on the server, the answer is an InvSnap (not an InvState:
        //    the world is answered), and the engine's outfit source, fed every inventory message in arrival order, now asks for the bare torso.
        const int shirt = inv::EquipIndex(inv::Equip::Shirt);
        inv::Character start;
        const bool wore = a.Latest(start) && !start.equip[shirt].Empty();
        const int statesBefore = ana.Count(ET::InvState);
        inv::Character off;
        const bool took = a.Op(MkOp(inv::OpKind::Use, inv::Where::Equip, shirt), off) && off.equip[shirt].Empty();
        of::WornFeed feed;
        for (const net::Event& e : ana.events) {
            if (e.type == ET::InvState) feed.OnInvState(e.invState.blob);
            if (e.type == ET::InvSnap && wornFromSnaps) feed.OnInvSnap(e.invSnap.blob);
        }
        const of::Want want = feed.have ? of::Desired(feed.ch, of::Sex::Male) : of::Want();
        check("inventory 0.5.6 live: taking the shirt off reaches the server and comes back as an InvSnap with the slot empty; the outfit source fed "
              "every inventory message (InvState AND InvSnap) asks for the bare torso (top 11/15, arms 3/15)",
              in && wore && took && ana.Count(ET::InvState) == statesBefore && feed.snaps > 0 && want.want[11] && want.drawable[11] == 15 &&
                  want.want[3] && want.drawable[3] == 15,
              Fmt("feed states %u snaps %u, top %d", feed.states, feed.snaps, (int)want.drawable[11]) + " | " + ana.Describe());

        // 2. GI: "binding to slot and then clicking 1-2-3-4 doesn't bring the gun into active slot". A pistol the ped picked up (current
        //    unarmed) lands in the pockets; bound to quick slot 1 and used, it is the hand item, and the engine's snapshot plan selects it on the
        //    ped. The same key again holsters it (0.5.6: GTA's own "1 = unarmed" is held off now) and the plan goes back to unarmed.
        const int s0 = ana.Count(ET::InvSnap);
        a.Report(inv::kWeaponUnarmed, {{kPistol, 0xA1, 24}});
        a.WaitSnap(s0);
        inv::Character got;
        int at = -1;
        if (a.Latest(got))
            for (size_t i = 0; i < got.pockets.slots.size(); ++i)
                if (got.pockets.slots[i].item == inv::FindWeapon(kPistol)->id) at = (int)i;
        inv::Character bound, held, put;
        const bool b = at >= 0 && a.Op(MkOp(inv::OpKind::Bind, inv::Where::Pockets, at, inv::Where::Quick, 0), bound) &&
                       bound.quick[0].item == inv::FindWeapon(kPistol)->id;
        const bool h = b && a.Op(MkOp(inv::OpKind::UseQuick, inv::Where::Quick, 0), held) && held.hand.item == inv::FindWeapon(kPistol)->id;
        invw::PedState ped;
        ped.valid = true;
        ped.current = inv::kWeaponUnarmed;
        ped.have.push_back({kPistol, 0xA1, 24});
        invw::AmmoTypes types;
        types.Learn(kPistol, 0xA1);
        bool selects = false;
        for (const invw::Act& x : invw::PlanSnapshot(&bound, held, false, {}, ped, types))
            selects |= x.k == invw::ActK::Select && x.hash == kPistol;
        check("inventory 0.5.6 live: a picked-up pistol bound to quick slot 1 and used (UseQuick) becomes the hand item on the server, and the "
              "engine's snapshot plan selects it on the ped",
              at >= 0 && b && h && selects, Fmt("pocket %d", at) + " | " + ana.Describe());

        ped.current = kPistol;
        bool unarms = false;
        const bool p = h && a.Op(MkOp(inv::OpKind::UseQuick, inv::Where::Quick, 0), put) && put.hand.Empty() && invw::Carries(put, kPistol);
        for (const invw::Act& x : invw::PlanSnapshot(&held, put, false, {}, ped, types))
            unarms |= x.k == invw::ActK::Select && x.hash == inv::kWeaponUnarmed;
        check("inventory 0.5.6 live: quick slot 1 again with the pistol in hand puts it away (back in the pockets, the hand empty) and the plan "
              "selects unarmed",
              p && unarms, ana.Describe());
        return failed;
    }

    static void Tests()
    {
        HandshakeAndWeapons();
        Ground();
        Trunks();
        Off();
        OutfitAndQuickSlots();
    }

}  // namespace invworldnet
