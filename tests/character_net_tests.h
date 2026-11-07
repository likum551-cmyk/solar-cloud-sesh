#pragma once
// 0.5.9 characters over the wire (protocol v8 CharRoster 56 / CharOp 57 / CharResult 58 / PeerFace 59; apps/server CharacterStore, the
// per-character inventory saves): the real server and the real ClientSession on loopback. Included by tests/server_test.cpp after
// inventory_net_tests.h (it reuses v8acct's fake platform and invnet's helpers). Every check runs on the passing path (kChecks of them).
// The rules themselves are tests/character_tests.h (gameplay_test).

#include "sync/character.h"

#include <filesystem>

namespace charnet {

    constexpr int kChecks = 19;
    namespace chr = gage::sync::chr;
    namespace inv = gage::sync::inv;

    struct Client
    {
        Rig& rig;
        Probe& p;
        uint16_t seq = 0;

        bool WaitRoster(int n = 1)
        {
            rig.Until([&] { p.Drain(); return p.Count(ET::CharRoster) >= n; }, 2000);
            return p.Count(ET::CharRoster) >= n;
        }
        // The newest CharRoster (false when none came or it does not decode).
        bool Roster(chr::Roster& out, uint8_t* flags = nullptr) const
        {
            for (auto it = p.events.rbegin(); it != p.events.rend(); ++it)
                if (it->type == ET::CharRoster)
                {
                    if (flags) *flags = it->charRoster.flags;
                    return chr::DecodeRoster(it->charRoster.blob, out);
                }
            return false;
        }
        // Sends one op; the CharResult's code, -1 when no answer came, -2 when the session would not send it.
        int Send(chr::Op op, int slot, const std::string& blob, uint16_t* sentSeq = nullptr)
        {
            proto::CharOp m;
            m.seq = ++seq;
            m.op = (uint8_t)op;
            m.slot = (uint8_t)slot;
            m.blob = blob;
            if (sentSeq) *sentSeq = m.seq;
            return p.s.SendCharOp(m) ? 0 : -2;
        }
        int ResultOf(uint16_t want) const
        {
            for (const net::Event& e : p.events)
                if (e.type == ET::CharResult && e.charResult.seq == want) return e.charResult.code;
            return -1;
        }
        int Op(chr::Op op, int slot, const chr::Character* c = nullptr)
        {
            uint16_t want = 0;
            if (Send(op, slot, c ? chr::EncodeCharacter(*c) : std::string(), &want) == -2) return -2;
            rig.Until([&] { p.Drain(); return ResultOf(want) >= 0; }, 2000);
            return ResultOf(want);
        }
        int OpRaw(chr::Op op, int slot, const std::string& blob)
        {
            uint16_t want = 0;
            if (Send(op, slot, blob, &want) == -2) return -2;
            rig.Until([&] { p.Drain(); return ResultOf(want) >= 0; }, 2000);
            return ResultOf(want);
        }
        // The newest PeerFace for player `id` (false when none).
        bool FaceOf(uint16_t id, chr::Appearance& out) const
        {
            for (auto it = p.events.rbegin(); it != p.events.rend(); ++it)
                if (it->type == ET::PeerFace && it->face.id == id) return chr::DecodeLook(it->face.blob, out);
            return false;
        }
        int FacesOf(uint16_t id) const
        {
            int n = 0;
            for (const net::Event& e : p.events)
                if (e.type == ET::PeerFace && e.face.id == id) n++;
            return n;
        }
        bool LatestInv(inv::Character& out) const
        {
            for (auto it = p.events.rbegin(); it != p.events.rend(); ++it)
            {
                if (it->type == ET::InvState) return inv::Decode(it->invState.blob, out);
                if (it->type == ET::InvSnap) return inv::Decode(it->invSnap.blob, out);
            }
            return false;
        }
    };

    static int IndexOf(const Probe& p, ET t, uint16_t id = 0xFFFF)
    {
        int i = 0;
        for (const net::Event& e : p.events)
        {
            if (e.type == t && (id == 0xFFFF || (t == ET::PeerFace ? e.face.id == id : t == ET::PlayerJoined ? e.joined.id == id : true))) return i;
            i++;
        }
        return -1;
    }

    static chr::Code C(int code) { return (chr::Code)code; }

    // ---- guests on a server without saves: the burst, faces, Save, refusals, the rate, names freed, a malformed op, characters 0 ----
    static void GuestTests()
    {
        Rig rig;
        if (!rig.Start(Rig::BaseConfig())) { Check("characters live: server starts", false); return; }
        Probe gus(rig);
        Client g{rig, gus};
        const bool in = gus.Join("Gus");
        const bool got = g.WaitRoster();
        rig.Pump(100);
        gus.Drain();
        chr::Roster gr;
        uint8_t flags = 0;
        const bool dec = g.Roster(gr, &flags);
        // 1. the burst: inventory, WorldClock, then the roster (a guest's, 1 slot, the instant character named from "Gus")
        {
            const int invAt = IndexOf(gus, ET::InvState), clockAt = IndexOf(gus, ET::WorldClock), rosAt = IndexOf(gus, ET::CharRoster);
            const chr::Character* a = gr.Active();
            Check("characters live: a guest's join burst ends InvState, WorldClock, CharRoster (guest flag, 1 slot, 1 character) and the "
                  "character was made at once, named from the player name (Gus + a surname) with a valid look (log: had no character)",
                  in && got && dec && invAt >= 0 && clockAt > invAt && rosAt > clockAt && flags == proto::kCharRosterGuest && gr.slots == 1 &&
                      gr.Count() == 1 && a && a->first == "Gus" && chr::ValidNamePart(a->last, chr::NamePart::Last) && a->flags == 0 &&
                      chr::ValidAppearance(a->look) && rig.LogHas("had no character: made \"Gus ") && rig.srv.stats().charsInstant == 1,
                  gus.Describe());
        }
        // 2. a second player: each sees the other's face, after the other's PlayerJoined
        Probe alice(rig);
        Client al{rig, alice};
        const bool aIn = alice.Join("Alice");
        al.WaitRoster();
        rig.Until([&] { gus.Drain(); return g.FacesOf(v8acct::MyId(alice)) > 0; }, 2000);
        rig.Pump(100);
        alice.Drain();
        gus.Drain();
        chr::Roster ar;
        al.Roster(ar);
        const uint16_t gusId = v8acct::MyId(gus), aliceId = v8acct::MyId(alice);
        {
            chr::Appearance seenByGus, seenByAlice;
            const bool faces = g.FaceOf(aliceId, seenByGus) && ar.Active() && seenByGus == ar.Active()->look && al.FaceOf(gusId, seenByAlice) &&
                               gr.Active() && seenByAlice == gr.Active()->look;
            const bool order = IndexOf(gus, ET::PeerFace, aliceId) > IndexOf(gus, ET::PlayerJoined, aliceId) &&
                               IndexOf(alice, ET::PeerFace, gusId) > IndexOf(alice, ET::PlayerJoined, gusId) &&
                               IndexOf(alice, ET::PeerFace, gusId) > IndexOf(alice, ET::CharRoster);
            Check("characters live: a newcomer gets every player's face (PeerFace) after its own roster, the others get the newcomer's after "
                  "its PlayerJoined, and each equals that player's active character",
                  aIn && faces && order, gus.Describe() + " | " + alice.Describe());
        }
        // 3. Save: the instant character renamed and restyled once; the peer sees the new face; a second rename is NameFixed
        {
            chr::Character edit = gr.Active() ? *gr.Active() : chr::Character();   // no roster: the checks fail, nothing crashes
            edit.first = "Gus";
            edit.last = "Newman";
            edit.look.hairColor = 7;
            edit.look.features[3] = -40;
            const int facesBefore = al.FacesOf(gusId);
            const int r1 = g.Op(chr::Op::Save, 0, &edit);
            rig.Until([&] { alice.Drain(); return al.FacesOf(gusId) > facesBefore; }, 2000);
            chr::Roster after;
            g.Roster(after);
            chr::Appearance face;
            const bool seen = al.FaceOf(gusId, face) && face == edit.look;
            chr::Character again = edit;
            again.last = "Oldman";
            const int r2 = g.Op(chr::Op::Save, 0, &again);
            chr::Roster still;
            g.Roster(still);
            Check("characters live: Save renames the instant character and changes its look (Ok + a new CharRoster), the other player gets the "
                  "new face; a second rename is NameFixed and changes nothing",
                  C(r1) == chr::Code::Ok && after.Active() && after.Active()->last == "Newman" && after.Active()->look == edit.look &&
                      (after.Active()->flags & chr::kCharCustomised) && seen && C(r2) == chr::Code::NameFixed && still.Active() &&
                      still.Active()->last == "Newman",
                  "r1=" + std::to_string(r1) + " r2=" + std::to_string(r2));
        }
        // 4. refusals: a guest's second slot, a slot past it, another player's name, a blob that does not decode (the connection stays)
        {
            const chr::Character ann = []{ chr::Character c; c.first = "Ann"; c.last = "Lee"; c.look = chr::DefaultAppearance(3); return c; }();
            const int create = g.Op(chr::Op::Create, 1, &ann);
            const int select = g.Op(chr::Op::Select, 2);
            chr::Character steal = ar.Active() ? *ar.Active() : chr::Character();
            steal.first = "Gus";
            steal.last = "Newman";
            const int taken = al.Op(chr::Op::Save, 0, &steal);
            const int broken = g.OpRaw(chr::Op::Save, 0, "zz");
            Check("characters live: a guest's Create in slot 1 and Select of slot 2 are Locked, another player's full name is NameTaken, a blob "
                  "that does not decode is BadLook, and the players stay connected",
                  C(create) == chr::Code::Locked && C(select) == chr::Code::Locked && C(taken) == chr::Code::NameTaken &&
                      C(broken) == chr::Code::BadLook && gus.Joined() && alice.Joined(),
                  std::to_string(create) + " " + std::to_string(select) + " " + std::to_string(taken) + " " + std::to_string(broken));
        }
        // 5. the rate: 8 selects at once (Alice has 4 of 5 tokens left): some Ok, the rest RateLimited, every one answered
        {
            uint16_t seqs[8];
            for (uint16_t& s : seqs) al.Send(chr::Op::Select, 0, std::string(), &s);
            rig.Until([&] { alice.Drain(); return al.ResultOf(seqs[7]) >= 0; }, 3000);
            int ok = 0, limited = 0;
            for (uint16_t s : seqs)
            {
                const int r = al.ResultOf(s);
                ok += C(r) == chr::Code::Ok ? 1 : 0;
                limited += C(r) == chr::Code::RateLimited ? 1 : 0;
            }
            Check("characters live: 8 CharOps at once are all answered, the ones past the bucket (5 at once, 1 a second) as RateLimited",
                  ok >= 3 && ok <= 5 && limited >= 3 && ok + limited == 8, std::to_string(ok) + " ok, " + std::to_string(limited) + " limited");
        }
        // 6. a guest's names are given back when it leaves
        {
            gus.s.Disconnect();
            rig.Until([&] { return rig.srv.Players().size() == 1; }, 3000);
            rig.Pump(1100);   // a token back
            chr::Character mine = ar.Active() ? *ar.Active() : chr::Character();
            mine.first = "Gus";
            mine.last = "Newman";
            const int r = al.Op(chr::Op::Save, 0, &mine);
            Check("characters live: after the guest Gus Newman leaves, another player may take that name (a guest's names are not kept)",
                  C(r) == chr::Code::Ok, std::to_string(r));
        }
        // 7. a malformed CharOp (op 9) cuts the sender off (Protocol)
        {
            Probe bad(rig);
            const bool badIn = bad.Join("Mal");
            Client b{rig, bad};
            b.WaitRoster();
            const proto::Bytes raw = {(uint8_t)proto::Msg::CharOp, 1, 0, 9, 0, 0};
            bad.s.SendRaw(proto::kChanReliable, raw, true);
            bad.WaitClosed(3000);
            bad.Drain();
            Check("characters live: a CharOp with op 9 is refused (Protocol) and the connection ends", badIn && bad.RefusedWith(proto::RefuseReason::Protocol),
                  bad.Describe());
        }
        rig.srv.Stop();

        // 8. characters 0: no roster, no faces, and the client will not send a CharOp to a server that never offered characters
        {
            Rig off;
            server::Config cfg = Rig::BaseConfig();
            cfg.characters = 0;
            if (!off.Start(cfg)) { Check("characters live: a server with characters 0 starts", false); return; }
            Probe x(off), y(off);
            Client cx{off, x};
            const bool both = x.Join("Xavi") && y.Join("Yuki");
            off.Pump(400);
            x.Drain();
            y.Drain();
            const int sent = cx.Send(chr::Op::Select, 0, std::string());
            Check("characters live: with characters 0 nobody gets a CharRoster or a PeerFace, SendCharOp refuses (no offer), the start logs "
                  "'characters: off' and nothing is counted",
                  both && x.Count(ET::CharRoster) == 0 && y.Count(ET::CharRoster) == 0 && x.Count(ET::PeerFace) == 0 && y.Count(ET::PeerFace) == 0 &&
                      sent == -2 && !x.s.charactersOffered() && off.LogHas("characters: off") && off.srv.stats().charsInstant == 0,
                  x.Describe());
            off.srv.Stop();
        }
    }

    // A platform that also answers tok-pia3: Pia's account with 3 character slots (two bought).
    static FakeMaster::Reply Answer(const FakeMaster::Req& rq)
    {
        if (rq.head.rfind("POST /internal/game/verify ", 0) == 0 && rq.body == "{\"token\":\"tok-pia3\"}")
        {
            FakeMaster::Reply r;
            r.body = "{\"ok\":true,\"accountId\":\"acc-player\",\"username\":\"Pia_Player\",\"roles\":[\"player\"],\"emailVerified\":true,"
                     "\"sessionId\":\"ses-acc-player\",\"ban\":null,\"characterSlots\":3}";
            return r;
        }
        return v8acct::PlatformAnswer(rq);
    }

    // ---- verified accounts: saved rosters, slots from the platform, per-character inventories, names across accounts and restarts ----
    static void SaveTests()
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path(ec) / ("gage_chr_test_" + std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()));
        FakeMaster fake;
        if (ec || !fake.Start()) { Check("characters saves: a scratch folder and the fake platform", false); return; }
        fake.Respond(Answer);
        server::Config cfg = v8acct::PlatformConfig(fake, false);
        cfg.inventoryDir = (dir / "inv").string();
        cfg.characterDir = (dir / "chr").string();
        server::CharacterStore paths(cfg.characterDir);
        server::InventoryStore invPaths(cfg.inventoryDir);
        chr::Roster first;
        {
            Rig rig;
            if (!rig.Start(cfg)) { Check("characters saves: server starts", false); return; }
            // 1. the instant character of a verified account is saved at the join
            {
                Probe pia(rig);
                Client c{rig, pia};
                const bool in = v8acct::JoinWith(rig, pia, "x", "tok-player");
                c.WaitRoster();
                uint8_t flags = 1;
                const bool dec = c.Roster(first, &flags);
                const bool file = fs::exists(paths.PathFor("acc-player"), ec);
                Check("characters saves: a verified account with no roster gets \"Pia Player\" (from its username Pia_Player) at once, saved to "
                      "<hex>.chr during the join; the platform said no slot count, so 1 slot (logged)",
                      in && dec && flags == 0 && first.slots == 1 && first.Count() == 1 && first.Active() && first.Active()->first == "Pia" &&
                          first.Active()->last == "Player" && file && rig.LogHas("(the platform did not say: 1)") && rig.srv.stats().charSaves >= 1,
                      pia.Describe());
                pia.s.Disconnect();
                rig.Until([&] { return rig.srv.Players().empty(); }, 3000);
            }
            // 2. the same account again: the same character, nothing made
            {
                Probe pia(rig);
                Client c{rig, pia};
                const bool in = v8acct::JoinWith(rig, pia, "x", "tok-player2");
                c.WaitRoster();
                chr::Roster back;
                const bool dec = c.Roster(back);
                Check("characters saves: the same account joining again gets the saved roster back unchanged and no new character is made",
                      in && dec && back == first && rig.srv.stats().charsInstant == 1 && rig.LogHas(" saved, 1 character(s), 1 slot(s)"), pia.Describe());
                pia.s.Disconnect();
                rig.Until([&] { return rig.srv.Players().empty(); }, 3000);
            }
            // 3. three slots from the platform: Create in slot 1; each character has its own items; dead = no switch
            {
                Probe pia(rig);
                Client c{rig, pia};
                const bool in = v8acct::JoinWith(rig, pia, "x", "tok-pia3");
                c.WaitRoster();
                chr::Roster r3;
                c.Roster(r3);
                chr::Character ann;
                ann.first = "Ann";
                ann.last = "Lee";
                ann.look = chr::DefaultAppearance(77);
                ann.look.sex = chr::Sex::Female;
                ann.look.hair = 38;
                const int made = c.Op(chr::Op::Create, 1, &ann);
                chr::Roster r4;
                c.Roster(r4);
                Check("characters saves: with characterSlots 3 the roster has 3 slots; Create puts Ann Lee in slot 1 (customised, saved) without "
                      "switching to her",
                      in && r3.slots == 3 && C(made) == chr::Code::Ok && r4.Has(1) && r4.chars[1].first == "Ann" && r4.chars[1].look == ann.look &&
                          r4.active == 0 && rig.srv.stats().charSaves >= 2,
                      "made=" + std::to_string(made));

                // slot 0 gets water; switching to slot 1 brings its own (fresh) items, switching back brings the water back
                std::string out;
                const uint16_t id = v8acct::MyId(pia);
                rig.srv.Execute("give " + std::to_string(id) + " water 2", out);
                rig.Pump(200);
                pia.Drain();
                const int invBefore = pia.Count(ET::InvState) + pia.Count(ET::InvSnap);
                const int to1 = c.Op(chr::Op::Select, 1);
                rig.Until([&] { pia.Drain(); return pia.Count(ET::InvState) + pia.Count(ET::InvSnap) > invBefore; }, 2000);
                inv::Character inv1;
                const bool got1 = c.LatestInv(inv1);
                chr::Roster on1;
                c.Roster(on1);
                const int invMid = pia.Count(ET::InvState) + pia.Count(ET::InvSnap);
                const int to0 = c.Op(chr::Op::Select, 0);
                rig.Until([&] { pia.Drain(); return pia.Count(ET::InvState) + pia.Count(ET::InvSnap) > invMid; }, 2000);
                inv::Character inv0;
                const bool got0 = c.LatestInv(inv0);
                const bool files = fs::exists(invPaths.PathFor("acc-player", 0), ec) && fs::exists(invPaths.PathFor("acc-player", 1), ec);
                Check("characters saves: Select switches the active character and its items: slot 1 starts with the starter kit, slot 0 still has "
                      "its 2 water when switched back, and each has its own save file (<hex>.inv, <hex>.c1.inv)",
                      C(to1) == chr::Code::Ok && on1.active == 1 && got1 && invnet::QtyOf(inv1, invnet::kWater) == 0 &&
                          invnet::Same(inv1, inv::StarterKit()) && C(to0) == chr::Code::Ok && got0 && invnet::QtyOf(inv0, invnet::kWater) == 2 && files,
                      "to1=" + std::to_string(to1) + " to0=" + std::to_string(to0) + " " + pia.Describe());

                // end on slot 1, then die: a switch while dead is NotNow
                rig.Pump(1100);
                const int back1 = c.Op(chr::Op::Select, 1);
                std::string kerr;
                rig.srv.KillPlayer(std::to_string(id), &kerr);
                rig.Pump(100);
                const int dead = c.Op(chr::Op::Select, 0);
                chr::Roster stay;
                c.Roster(stay);
                Check("characters saves: while dead a Select of another character is NotNow and the active one stays",
                      C(back1) == chr::Code::Ok && C(dead) == chr::Code::NotNow && stay.active == 1, std::to_string(back1) + " " + std::to_string(dead) + " " + kerr);
                pia.s.Disconnect();
                rig.Until([&] { return rig.srv.Players().empty(); }, 3000);
            }
            // 4. another account cannot take a saved name, online or not
            {
                Probe ada(rig);
                Client c{rig, ada};
                const bool in = v8acct::JoinWith(rig, ada, "Ada", "tok-admin");
                c.WaitRoster();
                chr::Roster ar;
                c.Roster(ar);
                chr::Character take = ar.Active() ? *ar.Active() : chr::Character();
                take.first = "Pia";
                take.last = "Player";
                const int r1 = c.Op(chr::Op::Save, 0, &take);
                take.first = "ANN";
                take.first = "Ann";
                take.last = "Lee";
                const int r2 = c.Op(chr::Op::Save, 0, &take);
                Check("characters saves: a second account (instant \"Ada Admin\") may not rename to Pia Player or Ann Lee (the offline account's "
                      "saved names): NameTaken",
                      in && ar.Active() && ar.Active()->first == "Ada" && C(r1) == chr::Code::NameTaken && C(r2) == chr::Code::NameTaken,
                      std::to_string(r1) + " " + std::to_string(r2));
                ada.s.Disconnect();
                rig.Until([&] { return rig.srv.Players().empty(); }, 3000);
            }
            rig.srv.Stop();
        }
        // 5. a restart: the name index comes from the files; the slot count follows the platform (1 again: the active slot 1 is locked)
        {
            Rig rig;
            if (!rig.Start(cfg)) { Check("characters saves: server restarts", false); return; }
            const bool scanned = rig.LogHas("name index: 3 names from 2 rosters");
            Probe ada(rig);
            Client ca{rig, ada};
            v8acct::JoinWith(rig, ada, "Ada", "tok-admin");
            ca.WaitRoster();
            chr::Roster ar;
            ca.Roster(ar);
            chr::Character take = ar.Active() ? *ar.Active() : chr::Character();
            take.first = "Ann";
            take.last = "Lee";
            const int r = ca.Op(chr::Op::Save, 0, &take);
            Check("characters saves: after a restart the name index is read from the saved rosters (3 names from 2 rosters, logged) and a "
                  "saved name is still NameTaken",
                  scanned && C(r) == chr::Code::NameTaken, std::to_string(r));

            Probe pia(rig);
            Client cp{rig, pia};
            const bool in = v8acct::JoinWith(rig, pia, "x", "tok-player");
            cp.WaitRoster();
            chr::Roster one;
            const bool dec = cp.Roster(one);
            Check("characters saves: back on 1 slot (the platform's answer), the account plays slot 0 again and Ann Lee stays stored in her "
                  "locked slot",
                  in && dec && one.slots == 1 && one.active == 0 && one.Has(1) && one.chars[1].first == "Ann" && rig.srv.stats().charsInstant == 0,
                  pia.Describe());

            // 6. a corrupt roster is kept aside and the account gets a new instant character
            const std::string p = paths.PathFor("acc-mod");
            WriteTextFile(p.c_str(), "not a roster");
            Probe mo(rig);
            Client cm{rig, mo};
            const bool moIn = v8acct::JoinWith(rig, mo, "Mo", "tok-mod");
            cm.WaitRoster();
            chr::Roster mr;
            const bool mdec = cm.Roster(mr);
            Check("characters saves: a roster that does not decode is kept as .corrupt, the log names it, and the account gets an instant "
                  "character (saved over the empty place; Mo_Mod's 'Mo' is too short for a first name, so a list name)",
                  moIn && mdec && mr.Count() == 1 && mr.Active() && chr::ValidNamePart(mr.Active()->first, chr::NamePart::First) &&
                      mr.Active()->first != "Mo" && fs::exists(p + ".corrupt", ec) &&
                      rig.LogHas("roster corrupt") && fs::exists(p, ec),
                  mo.Describe());
            rig.srv.Stop();
        }
        fs::remove_all(dir, ec);   // the folder this test made (unique name above)
    }

    static void PureTests()
    {
        auto slotsOf = [](const char* extra) {
            const std::string body = std::string("{\"ok\":true,\"accountId\":\"a\",\"username\":\"Pia_Player\",\"roles\":[],\"sessionId\":\"s\"") + extra + "}";
            const server::VerifyResult r = server::ParseVerifyAnswer(200, body);
            return r.outcome == server::VerifyResult::Outcome::Ok ? r.characterSlots : -1;
        };
        const bool ok = slotsOf("") == 0 && slotsOf(",\"characterSlots\":1") == 1 && slotsOf(",\"characterSlots\":3") == 3 &&
                        slotsOf(",\"characterSlots\":4") == 0 && slotsOf(",\"characterSlots\":0") == 0 && slotsOf(",\"characterSlots\":2.5") == 0 &&
                        slotsOf(",\"characterSlots\":\"2\"") == 0 && slotsOf(",\"characterSlots\":null") == 0;
        Check("characters: the verify answer's characterSlots is read as a whole number 1..3; absent, 0, 4, 2.5, a string or null is 0 (not said) "
              "and the verify still succeeds",
              ok);

        const char* path = "gage_server_test_characters.json";
        WriteTextFile(path, "{ \"characters\": 0, \"characterDir\": \"saves/chr\" }");
        server::Config c;
        std::string err;
        const bool loaded = server::LoadConfigFile(path, c, &err);
        WriteTextFile(path, "{ \"characters\": 2 }");
        server::Config c2;
        std::string err2;
        const bool refused = !server::LoadConfigFile(path, c2, &err2);
        std::remove(path);
        server::Config longDir = Rig::BaseConfig();
        longDir.characterDir = std::string(300, 'a');
        std::string e3;
        const server::Config d;
        Check("characters config: characters / characterDir load, default to 1 / \"characters\", characters 2 is refused and so is an "
              "over-long characterDir",
              loaded && c.characters == 0 && c.characterDir == "saves/chr" && d.characters == 1 && d.characterDir == "characters" && refused &&
                  !server::Validate(longDir, &e3),
              err + " | " + err2);
    }

    static void Tests()
    {
        PureTests();
        GuestTests();
        SaveTests();
    }

}  // namespace charnet
