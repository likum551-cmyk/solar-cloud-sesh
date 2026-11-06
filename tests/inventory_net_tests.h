#pragma once
// 0.6 inventory over the wire (protocol v8 InvState / InvOp / InvUsed; apps/server InventoryStore): the real server and the real ClientSession on
// loopback. Included by tests/server_test.cpp after account_tests.h (it reuses acct's fake platform for the verified-account saves). Every check
// runs on the passing path (kChecks of them). The item rules themselves are tests/inventory_tests.h (gameplay_test).

#include "sync/inventory.h"

#include <filesystem>

namespace invnet {

    constexpr int kChecks = 13;
    namespace inv = gage::sync::inv;

    constexpr inv::ItemId kWater = 5, kSandwich = 6, kMedkit = 7, kBandage = 8;

    struct Client
    {
        Rig& rig;
        Probe& p;
        uint32_t seq = 0;

        // The newest InvState's inventory (false when none came or it does not decode).
        bool Latest(inv::Character& out, uint32_t* ack = nullptr) const
        {
            for (auto it = p.events.rbegin(); it != p.events.rend(); ++it)
                if (it->type == ET::InvState)
                {
                    if (ack) *ack = it->invState.ackSeq;
                    return inv::Decode(it->invState.blob, out);
                }
            return false;
        }
        // One op, answered by the InvState carrying its seq; `out` = that state. False when no answer came.
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
                    if (e.type == ET::InvState && e.invState.ackSeq == want) got = true;
                return got;
            }, 2000);
            if (!got) return false;
            for (auto it = p.events.rbegin(); it != p.events.rend(); ++it)
                if (it->type == ET::InvState && it->invState.ackSeq == want) return inv::Decode(it->invState.blob, out);
            return false;
        }
        // Waits for the next InvState after `before` of them (a console give / clear sends one with ackSeq 0).
        bool NextState(int before, inv::Character& out)
        {
            rig.Until([&] { p.Drain(); return p.Count(ET::InvState) > before; }, 2000);
            return p.Count(ET::InvState) > before && Latest(out);
        }
    };

    static inv::Op MakeOp(inv::OpKind k, inv::Where src, int si, inv::Where dst = inv::Where::Pockets, int di = -1, uint32_t qty = 0)
    {
        inv::Op o;
        o.kind = k;
        o.src = src;
        o.si = (int16_t)si;
        o.dst = dst;
        o.di = (int16_t)di;
        o.qty = qty;
        return o;
    }

    static int PocketOf(const inv::Character& ch, inv::ItemId id)
    {
        for (size_t i = 0; i < ch.pockets.slots.size(); i++)
            if (ch.pockets.slots[i].item == id && !ch.pockets.slots[i].Empty()) return (int)i;
        return -1;
    }
    static uint32_t QtyOf(const inv::Character& ch, inv::ItemId id)
    {
        uint32_t n = 0;
        for (const inv::Slot& s : ch.pockets.slots)
            if (!s.Empty() && s.item == id) n += s.qty;
        if (ch.HasPack())
            for (const inv::Slot& s : ch.pack.slots)
                if (!s.Empty() && s.item == id) n += s.qty;
        return n;
    }
    static int FirstEmptyPocket(const inv::Character& ch)
    {
        for (size_t i = 0; i < ch.pockets.slots.size(); i++)
            if (ch.pockets.slots[i].Empty()) return (int)i;
        return -1;
    }
    static bool Same(const inv::Character& a, const inv::Character& b) { return inv::Encode(a) == inv::Encode(b); }

    static uint8_t HealthOf(Rig& rig, uint16_t id)
    {
        for (const server::PlayerInfo& i : rig.srv.Players())
            if (i.id == id) return i.health;
        return 0;
    }
    static int InvUsedCount(const Probe& p) { return p.Count(ET::InvUsed); }

    // ---- a guest on a server with the shipped item rules: the kit, ops, USE and its effects, the console, a malformed op ----
    static void GuestTests()
    {
        Rig rig;
        server::Config cfg = Rig::BaseConfig();   // playerHealth 100, inventoryDir "" (a guest is never saved anyway)
        cfg.maxHitDistance = 10;
        if (!rig.Start(cfg)) { Check("inventory live: server starts", false); return; }
        Probe gus(rig), alice(rig);
        if (!gus.Join("Gus") || !alice.Join("Alice")) { Check("inventory live: two players join", false); return; }
        Client g{rig, gus};
        rig.Until([&] { gus.Drain(); return gus.Count(ET::InvState) > 0; }, 2000);

        // 1. the burst: AccountInfo, then InvState (ack 0) with the starter kit
        {
            int acctAt = -1, invAt = -1, i = 0;
            for (const net::Event& e : gus.events)
            {
                if (e.type == ET::AccountInfo && acctAt < 0) acctAt = i;
                if (e.type == ET::InvState && invAt < 0) invAt = i;
                i++;
            }
            inv::Character got;
            uint32_t ack = 99;
            const bool dec = g.Latest(got, &ack);
            Check("inventory live: a joining player gets InvState right after AccountInfo, ackSeq 0, holding exactly the starter kit (guest)",
                  acctAt >= 0 && invAt == acctAt + 1 && dec && ack == 0 && Same(got, inv::StarterKit()) && gus.Count(ET::InvState) == 1,
                  gus.Describe());
        }

        // 2. a move: the answer carries its seq, the client's copy and the server's agree
        inv::Character kit = inv::StarterKit(), after, srvCopy;
        const int waterAt = PocketOf(kit, kWater), freeAt = FirstEmptyPocket(kit);
        const bool moved = g.Op(MakeOp(inv::OpKind::Move, inv::Where::Pockets, waterAt, inv::Where::Pockets, freeAt), after);
        const bool held = rig.srv.InventoryOf("Gus", srvCopy);
        Check("inventory live: InvOp Move is applied by the server and answered with InvState(ackSeq = its seq) equal to the server's copy",
              moved && held && Same(after, srvCopy) && after.pockets.slots[(size_t)freeAt].item == kWater && after.pockets.slots[(size_t)freeAt].qty == 2 &&
                  after.pockets.slots[(size_t)waterAt].Empty() && rig.srv.stats().invOpsApplied == 1,
              "water " + std::to_string(waterAt) + " -> " + std::to_string(freeAt));

        // 3. a refused op (from an empty slot): answered, nothing changes
        inv::Character refused;
        const int emptyAt = FirstEmptyPocket(after);
        const bool answered = g.Op(MakeOp(inv::OpKind::Move, inv::Where::Pockets, emptyAt, inv::Where::Pockets, 0), refused);
        Check("inventory live: a refused InvOp (an empty source) is still answered with its seq, and the inventory is unchanged",
              answered && Same(refused, after) && rig.srv.stats().invOpsRefused == 1);

        // 4. USE water: one unit gone, InvUsed with its satiety
        inv::Character drank;
        const int usedBefore = InvUsedCount(gus);
        const bool drunk = g.Op(MakeOp(inv::OpKind::Use, inv::Where::Pockets, freeAt), drank);
        rig.Until([&] { gus.Drain(); return InvUsedCount(gus) > usedBefore; }, 1000);
        const net::Event* used = nullptr;
        for (auto it = gus.events.rbegin(); it != gus.events.rend(); ++it)
            if (it->type == ET::InvUsed) { used = &*it; break; }
        Check("inventory live: USE on water consumes one and sends InvUsed(water, 15 satiety)",
              drunk && QtyOf(drank, kWater) == 1 && used && used->invUsed.item == kWater && used->invUsed.satiety == 15 &&
                  rig.srv.stats().invUses == 1);

        // 5. a health item at full health is kept (refused), no effect sent
        inv::Character kept;
        const int bandageAt = PocketOf(drank, kBandage);
        const int usedMid = InvUsedCount(gus);
        const bool askedFull = g.Op(MakeOp(inv::OpKind::Use, inv::Where::Pockets, bandageAt), kept);
        rig.Pump(100);
        gus.Drain();
        Check("inventory live: USE on a bandage at full health is refused: the bandage stays (2), no InvUsed, health stays 100",
              askedFull && QtyOf(kept, kBandage) == 2 && InvUsedCount(gus) == usedMid && HealthOf(rig, acct::MyId(gus)) == 100);

        // 6. hurt, then bandage (+10 % of playerHealth) and a given medkit (+35 %, capped at the maximum)
        const uint16_t gId = acct::MyId(gus);
        const bool placed = acct::Placed(rig, alice, 0.0f, 0.0f) && acct::Placed(rig, gus, 5.0f, 0.0f) &&
                            rig.Until([&] { return rig.srv.IsRelevant(acct::MyId(alice), gId); }, 3000);
        const bool fired = Fire(rig, alice, Kin(0, 0, 30));
        proto::Hit h;
        h.kind = proto::HitKind::Player;
        h.targetId = gId;
        h.weapon = 0x1B06D571u;   // WEAPON_PISTOL
        h.damage = 30;
        h.shotSeq = alice.shotSeq;
        alice.s.SendHit(h);
        const bool hurt = rig.Until([&] { return HealthOf(rig, gId) < 100; }, 2000);
        const uint8_t h0 = HealthOf(rig, gId);
        inv::Character bandaged;
        const bool bandageUsed = g.Op(MakeOp(inv::OpKind::Use, inv::Where::Pockets, bandageAt), bandaged);
        const uint8_t h1 = HealthOf(rig, gId);
        std::string out;
        const int statesBefore = gus.Count(ET::InvState);
        const server::CommandResult gave = rig.srv.Execute("give Gus medkit", out);
        inv::Character withKit;
        const bool gotKit = g.NextState(statesBefore, withKit);
        inv::Character healed;
        const bool medUsed = g.Op(MakeOp(inv::OpKind::Use, inv::Where::Pockets, PocketOf(withKit, kMedkit)), healed);
        const uint8_t h2 = HealthOf(rig, gId);
        Check("inventory live: hurt to H, a bandage heals to H + 10 (10 % of playerHealth 100) and is used up by one; console give adds a "
              "medkit (InvState), which heals 35 % capped at the maximum",
              placed && fired && hurt && h0 < 100 && bandageUsed && h1 == std::min(100, h0 + 10) && QtyOf(bandaged, kBandage) == 1 &&
                  gave == server::CommandResult::Ok && gotKit && QtyOf(withKit, kMedkit) == 1 && medUsed && QtyOf(healed, kMedkit) == 0 &&
                  h2 == std::min(100, h1 + 35),
              "health " + std::to_string(h0) + " -> " + std::to_string(h1) + " -> " + std::to_string(h2) + " | " + out);

        // 7. dead: USE is refused and the item kept
        rig.srv.Execute("kill Gus", out);
        rig.Until([&] { for (const server::PlayerInfo& i : rig.srv.Players()) if (i.id == gId) return i.dead; return false; }, 2000);
        inv::Character deadTry;
        const int usedDead = InvUsedCount(gus);
        const bool askedDead = g.Op(MakeOp(inv::OpKind::Use, inv::Where::Pockets, PocketOf(healed, kSandwich)), deadTry);
        rig.Pump(100);
        gus.Drain();
        Check("inventory live: a dead player's USE (a sandwich) is refused: the sandwich stays, no InvUsed",
              askedDead && QtyOf(deadTry, kSandwich) == 1 && InvUsedCount(gus) == usedDead);

        // 8. the console: errors, a partial give, clearinv
        std::string eItem, eWho, eUsage, partial, cleared;
        const bool noItem = rig.srv.Execute("give Gus unobtainium", eItem) == server::CommandResult::Error;
        const bool noWho = rig.srv.Execute("give Nobody water", eWho) == server::CommandResult::Error;
        const bool usage = rig.srv.Execute("give Gus water 0", eUsage) == server::CommandResult::Error;
        const int s0 = gus.Count(ET::InvState);
        const bool gavePart = rig.srv.Execute("give Gus fuel_can 20", partial) == server::CommandResult::Ok;
        inv::Character heavy;
        const bool gotHeavy = g.NextState(s0, heavy);
        const int s1 = gus.Count(ET::InvState);
        const bool clr = rig.srv.Execute("clearinv Gus", cleared) == server::CommandResult::Ok;
        inv::Character empty;
        const bool gotEmpty = g.NextState(s1, empty);
        const uint32_t cans = QtyOf(heavy, inv::FindKey("fuel_can") ? inv::FindKey("fuel_can")->id : 0);
        Check("inventory live: console give refuses an unknown item, an unknown player and qty 0; a give of 20 fuel cans gives what fits "
              "(fewer than 20, more than 0) and says so; clearinv empties everything; each sends InvState",
              noItem && noWho && usage && gavePart && gotHeavy && cans > 0 && cans < 20 && partial.rfind("gave " + std::to_string(cans) + " of 20", 0) == 0 &&
                  clr && gotEmpty && Same(empty, inv::Character()),
              partial + " | " + eItem);

        // 9. a malformed InvOp (3 op bytes) cuts the sender off (Protocol), like every malformed reliable message
        Probe bad(rig);
        const bool badIn = bad.Join("Mal");
        const proto::Bytes raw = {(uint8_t)proto::Msg::InvOp, 1, 0, 0, 0, 1, 2, 3};
        bad.s.SendRaw(proto::kChanReliable, raw, true);
        bad.WaitClosed(3000);
        bad.Drain();
        Check("inventory live: an InvOp whose op is not 11 bytes is refused (Protocol) and the connection ends",
              badIn && bad.RefusedWith(proto::RefuseReason::Protocol), bad.Describe());
    }

    // ---- verified accounts: the save across a reconnect, guests never saved, a corrupt save ----
    static void SaveTests()
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path(ec) / ("gage_inv_test_" + std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()));
        FakeMaster fake;
        if (ec || !fake.Start()) { Check("inventory saves: a scratch folder and the fake platform", false); return; }
        fake.Respond(acct::PlatformAnswer);
        server::Config cfg = acct::PlatformConfig(fake, false);
        cfg.inventoryDir = dir.string();
        Rig rig;
        if (!rig.Start(cfg)) { Check("inventory saves: server starts", false); return; }
        server::InventoryStore paths(dir.string());
        auto files = [&] {
            int n = 0;
            std::error_code e;
            for (fs::directory_iterator it(dir, e), end; !e && it != end; it.increment(e)) n++;
            return n;
        };

        // 1. verified: the kit, a move, leave -> the file; rejoin (the same account) -> the moved inventory, logged as saved
        inv::Character moved;
        {
            Probe pia(rig);
            const bool in = acct::JoinWith(rig, pia, "x", "tok-player");
            Client c{rig, pia};
            rig.Until([&] { pia.Drain(); return pia.Count(ET::InvState) > 0; }, 2000);
            inv::Character kit;
            const bool gotKit = c.Latest(kit);
            const bool mv = c.Op(MakeOp(inv::OpKind::Move, inv::Where::Pockets, PocketOf(kit, kWater), inv::Where::Pockets, 15), moved);
            pia.s.Disconnect();
            rig.Until([&] { return rig.srv.Players().empty(); }, 3000);
            const bool saved = fs::exists(paths.PathFor("acc-player"), ec);
            Probe again(rig);
            const bool in2 = acct::JoinWith(rig, again, "x", "tok-player2");
            Client c2{rig, again};
            rig.Until([&] { again.Drain(); return again.Count(ET::InvState) > 0; }, 2000);
            inv::Character back;
            const bool gotBack = c2.Latest(back);
            Check("inventory saves: a verified player's kit is changed, the player leaves, the save file appears, and the same account "
                  "joining again gets the changed inventory back (log: saved)",
                  in && gotKit && Same(kit, inv::StarterKit()) && mv && saved && in2 && gotBack && Same(back, moved) &&
                      back.pockets.slots[15].item == kWater && rig.LogHas("inventory: id=") && rig.LogHas(" saved") && rig.srv.stats().invSaves >= 1,
                  again.Describe());
            again.s.Disconnect();
            rig.Until([&] { return rig.srv.Players().empty(); }, 3000);
        }

        // 2. a guest on the same server: plays, changes, leaves; nothing is written
        {
            const int before = files();
            Probe guest(rig);
            const bool in = guest.Join("Guesty");
            Client c{rig, guest};
            rig.Until([&] { guest.Drain(); return guest.Count(ET::InvState) > 0; }, 2000);
            inv::Character kit, chg;
            const bool got = c.Latest(kit);
            const bool mv = c.Op(MakeOp(inv::OpKind::Move, inv::Where::Pockets, 0, inv::Where::Pockets, 14), chg);
            guest.s.Disconnect();
            rig.Until([&] { return rig.srv.Players().empty(); }, 3000);
            Check("inventory saves: a guest gets the kit and can change it, and leaving writes no save file",
                  in && got && Same(kit, inv::StarterKit()) && mv && files() == before, std::to_string(files()) + " files, " + std::to_string(before) + " before");
        }

        // 3. a corrupt save: renamed aside, the player starts EMPTY (no second kit), the reason is logged
        {
            const std::string p = paths.PathFor("acc-admin");
            WriteTextFile(p.c_str(), "not an inventory");
            Probe ada(rig);
            const bool in = acct::JoinWith(rig, ada, "Ada", "tok-admin");
            Client c{rig, ada};
            rig.Until([&] { ada.Drain(); return ada.Count(ET::InvState) > 0; }, 2000);
            inv::Character got;
            const bool dec = c.Latest(got);
            Check("inventory saves: a save that does not decode is kept as .corrupt, the player starts empty, and the log names it",
                  in && dec && Same(got, inv::Character()) && fs::exists(p + ".corrupt", ec) && rig.LogHas("save corrupt"), ada.Describe());
            ada.s.Disconnect();
            rig.Until([&] { return rig.srv.Players().empty(); }, 3000);
        }
        rig.srv.Stop();
        fs::remove_all(dir, ec);   // the folder this test made (unique name above)
    }

    static void ConfigTest()
    {
        const char* path = "gage_server_test_inventory.json";
        WriteTextFile(path, "{ \"inventoryDir\": \"saves/inv\" }");
        server::Config c;
        std::string err;
        const bool ok = server::LoadConfigFile(path, c, &err);
        std::remove(path);
        server::Config longDir = Rig::BaseConfig();
        longDir.inventoryDir = std::string(300, 'a');
        std::string e2;
        Check("inventory config: inventoryDir loads, defaults to \"inventories\", and Validate refuses an over-long one",
              ok && c.inventoryDir == "saves/inv" && server::Config().inventoryDir == "inventories" && !server::Validate(longDir, &e2), err);
    }

    static void Tests()
    {
        ConfigTest();
        GuestTests();
        SaveTests();
    }

}  // namespace invnet
