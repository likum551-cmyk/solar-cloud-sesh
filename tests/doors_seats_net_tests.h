// doors_seats_net_tests.h: 0.4.22 doors and seats against the REAL server over loopback ENet and the real ClientSession, plus the VehicleStore
// rules behind them. Included by tests/t3_vehvisual_test.cpp after vehsignals_tests.h (one include) and run from its main (one call); it uses that
// file's helpers (Check, F, Kin, At, Drive, StoreRig, StoreRules, VCmd, PlaceAt, IdOf, kAdder, Rig, Probe, MsSince, Clock).
//
// NO PROTOCOL CHANGE: the lock is VehicleState.flags kVehLocked and VehicleOp::SetLocked, the seats are Enter / EnterIntent with a wire seat 0..7.
//
//   A. One scenario, three players and a late joiner (R1..R9):
//        R1 rear seats: Bob reserves the rear-left seat (EnterIntent seat 2), Carol's Enter of that seat is SeatTaken while he walks, his Enter is Ok,
//           Carol takes the rear-right seat (3); the seat list is [Alice, free, Bob, Carol] on the server and in Alice's view.
//        R2 the driver locks: Ok, the flag reaches all three views, nobody is thrown out.
//        R3 a passenger without the key cannot unlock; the occupants of a locked car can get out.
//        R4 a locked car refuses Enter and EnterIntent from players without the key (Locked), the seat list does not change.
//        R5 a late joiner's first state of the locked car already carries the flag.
//        R6 outside the car: a stranger is NotAllowed, the spawner 40 m away is TooFar, near it Ok; the unlock reaches all four views.
//        R7 the key follows the LAST DRIVER (car released first): a non-spawner who drove last can lock, enter it locked, and loses all of that when
//           the spawner drives it after him.
//        R8 the L key's loop through the real session: PickLockTarget on the replicated flag, SetLocked, the words, the flag in another view, twice.
//        R9 the server's counters and log lines for all of it.
//   B. Config: vehicleLocks=0 refuses SetLocked and says so at start; the server.json keys parse and are range-checked.
//   C. VehicleStore: the last driver's key ends with the disconnect, the lock distance and the switches, an idempotent SetLocked, the flag in
//      every full and motion-only entry.
//
// Evidence level: the server and the wire are REAL (loopback ENet); the game natives are not involved (live-unverified, see the engine lines).
#pragma once

#include "sync/vehicle_lock.h"

#include <filesystem>
#include <fstream>

constexpr int kDoorsSeatsNetChecks = 15;   // the number of Check() calls RunDoorsSeatsNetTests makes (a skipped section fails main's count)

static bool DS_Flag(const Probe& p, uint16_t vid, bool locked)
{
    const proto::VehicleState* st = p.Vehicle(vid);
    return st && ((st->flags & proto::kVehLocked) != 0) == locked;
}

static std::string DS_Codes(std::initializer_list<proto::VehicleResultCode> codes)
{
    std::string out;
    for (const proto::VehicleResultCode c : codes) out += std::string(out.empty() ? "" : " ") + proto::ToString(c);
    return out;
}

static void DS_RealServer()
{
    using Code = proto::VehicleResultCode;
    using Op = proto::VehicleOp;
    std::printf("-- 0.4.22 A. doors and seats through the real server (Alice drives, Bob and Carol, Dave joins late)\n");
    Rig rig;
    if (!rig.Start()) { Check("doors net: the server starts", false); return; }
    Probe alice(rig), bob(rig), carol(rig);
    if (!alice.Join("Alice") || !bob.Join("Bob") || !carol.Join("Carol")) { Check("doors net: three players join", false); return; }
    const uint16_t aid = IdOf(alice), bid = IdOf(bob), cid = IdOf(carol);
    const bool placed = PlaceAt(rig, alice, Kin(100, 100, 30)) && PlaceAt(rig, bob, Kin(103, 100, 30)) && PlaceAt(rig, carol, Kin(106, 100, 30));
    const proto::VehicleResult spawn = VCmd(rig, alice, Op::Spawn, 0, kAdder);
    const uint16_t vid = spawn.vehId;
    const bool seen = rig.Until([&] { return alice.Vehicle(vid) && bob.Vehicle(vid) && carol.Vehicle(vid); }, 2000);
    const proto::VehicleResult enter0 = VCmd(rig, alice, Op::Enter, vid, 0);
    if (!(placed && spawn.code == Code::Ok && seen && enter0.code == Code::Ok))
    {
        Check("doors net: A spawns a car and sits in the driver's seat, B and C see it", false,
              F("placed %d spawn %s seen %d enter %s", (int)placed, proto::ToString(spawn.code), (int)seen, proto::ToString(enter0.code)));
        return;
    }
    auto veh = [&]() -> const server::VehicleStore::Vehicle* { return rig.srv.FindVehicle(vid); };
    const uint16_t nobody = proto::kNoPlayer;

    // ---- R1: rear seats through the T1 reservation ----
    {
        const proto::VehicleResult intent = VCmd(rig, bob, Op::EnterIntent, vid, 2);
        const proto::VehicleResult carolTaken = VCmd(rig, carol, Op::Enter, vid, 2);   // Bob walks to that door: reserved, so Carol is refused
        const proto::VehicleResult bobIn = VCmd(rig, bob, Op::Enter, vid, 2);
        const proto::VehicleResult carolIn = VCmd(rig, carol, Op::Enter, vid, 3);
        const bool aliceSees = rig.Until([&] { const proto::VehicleState* s = alice.Vehicle(vid); return s && s->seats[2] == bid && s->seats[3] == cid; }, 2000);
        const auto* v = veh();
        Check("doors net R1: a rear seat goes through the real server: Bob's EnterIntent for seat 2 is Ok, Carol's Enter of that reserved seat is SeatTaken, Bob's Enter is Ok, "
              "Carol takes seat 3; the seat list is [Alice, free, Bob, Carol] on the server and in Alice's view",
              intent.code == Code::Ok && carolTaken.code == Code::SeatTaken && bobIn.code == Code::Ok && carolIn.code == Code::Ok && aliceSees && v &&
                  v->s.seats[0] == aid && v->s.seats[1] == nobody && v->s.seats[2] == bid && v->s.seats[3] == cid,
              F("%s", DS_Codes({ intent.code, carolTaken.code, bobIn.code, carolIn.code }).c_str()));
    }

    // ---- R2: the driver locks ----
    {
        const auto t0 = Clock::now();
        const proto::VehicleResult lock = VCmd(rig, alice, Op::SetLocked, vid, 1);
        const bool all = rig.Until([&] { return DS_Flag(alice, vid, true) && DS_Flag(bob, vid, true) && DS_Flag(carol, vid, true); }, 1500);
        const int64_t took = MsSince(t0);
        const auto* v = veh();
        Check("doors net R2: the driver's SetLocked is Ok, the locked flag reaches all three views within 1.5 s, and the occupants stay in their seats",
              lock.code == Code::Ok && all && v && (v->s.flags & proto::kVehLocked) && v->s.seats[0] == aid && v->s.seats[2] == bid && v->s.seats[3] == cid,
              F("%s, all views after %lld ms", proto::ToString(lock.code), (long long)took));
    }

    // ---- R3: a passenger without the key cannot unlock; the occupants can leave ----
    {
        const proto::VehicleResult bobUnlock = VCmd(rig, bob, Op::SetLocked, vid, 0);
        const bool stillLocked = veh() && (veh()->s.flags & proto::kVehLocked);
        const proto::VehicleResult bobExit = VCmd(rig, bob, Op::Exit, vid, 0);
        const proto::VehicleResult carolExit = VCmd(rig, carol, Op::Exit, vid, 0);
        const auto* v = veh();
        Check("doors net R3: a passenger without the key cannot unlock (NotAllowed, still locked), and the occupants of the locked car can get out (both Exits Ok, only Alice left in it)",
              bobUnlock.code == Code::NotAllowed && stillLocked && bobExit.code == Code::Ok && carolExit.code == Code::Ok && v && v->s.seats[0] == aid &&
                  v->s.seats[1] == nobody && v->s.seats[2] == nobody && v->s.seats[3] == nobody,
              F("%s", DS_Codes({ bobUnlock.code, bobExit.code, carolExit.code }).c_str()));
    }

    // ---- R4: a locked car refuses the seat claims of players without the key ----
    {
        const proto::VehicleResult bobEnter = VCmd(rig, bob, Op::Enter, vid, 2);
        const proto::VehicleResult bobIntent = VCmd(rig, bob, Op::EnterIntent, vid, 2);
        const proto::VehicleResult carolEnter = VCmd(rig, carol, Op::Enter, vid, 1);
        const auto* v = veh();
        Check("doors net R4: a locked car refuses a rear-seat Enter, a rear-seat EnterIntent and a front-seat Enter from players without the key (all Locked) and the seat list is unchanged",
              bobEnter.code == Code::Locked && bobIntent.code == Code::Locked && carolEnter.code == Code::Locked && v && v->s.seats[0] == aid &&
                  v->s.seats[1] == nobody && v->s.seats[2] == nobody && v->s.seats[3] == nobody,
              F("%s", DS_Codes({ bobEnter.code, bobIntent.code, carolEnter.code }).c_str()));
    }

    // ---- R5: a late joiner ----
    Probe dave(rig);
    {
        const bool joined = dave.Join("Dave") && PlaceAt(rig, dave, Kin(104, 100, 30));
        const bool appeared = joined && rig.Until([&] { return dave.Vehicle(vid) != nullptr; }, 3000);
        const bool locked = appeared && DS_Flag(dave, vid, true);        // read at the first sight of the car
        Check("doors net R5: a player who joins after the lock sees the car locked in the first state he holds of it", joined && appeared && locked,
              F("joined %d appeared %d locked at first sight %d", (int)joined, (int)appeared, (int)locked));
    }

    // ---- R6: outside the car ----
    {
        const proto::VehicleResult aliceExit = VCmd(rig, alice, Op::Exit, vid, 0);
        const proto::VehicleResult carolUnlock = VCmd(rig, carol, Op::SetLocked, vid, 0);
        const bool movedFar = PlaceAt(rig, alice, Kin(150, 100, 30));
        const proto::VehicleResult farUnlock = VCmd(rig, alice, Op::SetLocked, vid, 0);
        const bool stillLocked = veh() && (veh()->s.flags & proto::kVehLocked);
        const bool movedBack = PlaceAt(rig, alice, Kin(101, 100, 30));
        const proto::VehicleResult nearUnlock = VCmd(rig, alice, Op::SetLocked, vid, 0);
        const bool all = rig.Until([&] { return DS_Flag(alice, vid, false) && DS_Flag(bob, vid, false) && DS_Flag(carol, vid, false) && DS_Flag(dave, vid, false); }, 1500);
        Check("doors net R6: outside the car a stranger is NotAllowed, the spawner 50 m away is TooFar (still locked), near it Ok, and the unlock reaches all four views",
              aliceExit.code == Code::Ok && carolUnlock.code == Code::NotAllowed && movedFar && farUnlock.code == Code::TooFar && stillLocked && movedBack &&
                  nearUnlock.code == Code::Ok && all,
              F("%s, all views unlocked %d", DS_Codes({ aliceExit.code, carolUnlock.code, farUnlock.code, nearUnlock.code }).c_str(), (int)all));
    }

    // ---- R7: the key follows the last driver ----
    {
        const proto::VehicleResult release = VCmd(rig, alice, Op::Release, vid, 0);
        const proto::VehicleResult bobDrive = VCmd(rig, bob, Op::Enter, vid, 0);
        const bool bobIsLast = veh() && veh()->lastDriver == bid;
        const proto::VehicleResult bobExit = VCmd(rig, bob, Op::Exit, vid, 0);
        const proto::VehicleResult carolLock = VCmd(rig, carol, Op::SetLocked, vid, 1);
        const proto::VehicleResult bobLock = VCmd(rig, bob, Op::SetLocked, vid, 1);            // last driver, not the spawner
        const bool carolSees = rig.Until([&] { return DS_Flag(carol, vid, true); }, 1500);
        const proto::VehicleResult carolEnter = VCmd(rig, carol, Op::Enter, vid, 0);
        const proto::VehicleResult bobReenter = VCmd(rig, bob, Op::Enter, vid, 0);              // the last driver may enter his locked car
        const proto::VehicleResult bobExit2 = VCmd(rig, bob, Op::Exit, vid, 0);
        Check("doors net R7a: after the owner releases the car and Bob drives it, Bob (last driver, not the spawner) can lock it, Carol cannot lock it or enter it (NotAllowed, Locked), and Bob "
              "can enter his own locked car",
              release.code == Code::Ok && bobDrive.code == Code::Ok && bobIsLast && bobExit.code == Code::Ok && carolLock.code == Code::NotAllowed && bobLock.code == Code::Ok &&
                  carolSees && carolEnter.code == Code::Locked && bobReenter.code == Code::Ok && bobExit2.code == Code::Ok,
              F("%s", DS_Codes({ release.code, bobDrive.code, bobExit.code, carolLock.code, bobLock.code, carolEnter.code, bobReenter.code, bobExit2.code }).c_str()));

        const proto::VehicleResult aliceDrive = VCmd(rig, alice, Op::Enter, vid, 0);            // the spawner drives it locked, and is the last driver now
        const bool aliceIsLast = veh() && veh()->lastDriver == aid;
        const proto::VehicleResult aliceExit = VCmd(rig, alice, Op::Exit, vid, 0);
        const proto::VehicleResult bobUnlock = VCmd(rig, bob, Op::SetLocked, vid, 0);
        const proto::VehicleResult bobEnter = VCmd(rig, bob, Op::Enter, vid, 0);
        const bool stillLocked = veh() && (veh()->s.flags & proto::kVehLocked);
        const proto::VehicleResult aliceUnlock = VCmd(rig, alice, Op::SetLocked, vid, 0);
        Check("doors net R7b: once Alice has driven it, Bob has lost the key: his unlock is NotAllowed and his Enter is Locked (still locked), while Alice's unlock is Ok",
              aliceDrive.code == Code::Ok && aliceIsLast && aliceExit.code == Code::Ok && bobUnlock.code == Code::NotAllowed && bobEnter.code == Code::Locked && stillLocked &&
                  aliceUnlock.code == Code::Ok,
              F("%s", DS_Codes({ aliceDrive.code, aliceExit.code, bobUnlock.code, bobEnter.code, aliceUnlock.code }).c_str()));
    }

    // ---- R8: the L key's loop (the engine's pick, the real session, the words) ----
    {
        std::string texts[2];
        bool seenBy[2] = { false, false };
        Code codes[2] = { Code::BadArg, Code::BadArg };
        bool settled[2] = { false, false };
        for (int press = 0; press < 2; ++press)
        {
            // A press reads the replicated flag, so the player's view must have caught up with the last change (R7b's unlock; the first press's lock).
            settled[press] = rig.Until([&] { return DS_Flag(alice, vid, press == 1); }, 1500);
            const proto::VehicleState* st = alice.Vehicle(vid);
            sync::LockCand c;                                    // what TickLockKey builds from the replicated state
            c.netId = vid;
            c.distM = 4.0f;
            c.locked = st && (st->flags & proto::kVehLocked);
            c.keyed = true;
            const sync::LockPick pick = sync::PickLockTarget(&c, 1);
            const proto::VehicleResult r = VCmd(rig, alice, Op::SetLocked, pick.netId, pick.wantLocked ? 1u : 0u);
            codes[press] = r.code;
            texts[press] = sync::LockResultText((uint8_t)r.code, pick.wantLocked);
            seenBy[press] = rig.Until([&] { return DS_Flag(bob, vid, pick.wantLocked); }, 1500);
        }
        Check("doors net R8: two L presses through the real session (the engine's PickLockTarget on the replicated flag): \"Vehicle locked.\" with Bob's view locked, then \"Vehicle unlocked.\" with "
              "Bob's view unlocked",
              settled[0] && settled[1] && codes[0] == Code::Ok && codes[1] == Code::Ok && texts[0] == "Vehicle locked." && texts[1] == "Vehicle unlocked." && seenBy[0] && seenBy[1],
              F("%s / %s, seen %d %d, views settled %d %d", texts[0].c_str(), texts[1].c_str(), (int)seenBy[0], (int)seenBy[1], (int)settled[0], (int)settled[1]));
    }

    // ---- R9: the server's accounting ----
    {
        const server::Stats& st = rig.srv.stats();
        const bool lines = rig.LogHas("doors: Alice (id " + std::to_string(aid) + ") lock vehicle id=" + std::to_string(vid) + " (the driver)") &&
                           rig.LogHas("doors: Bob (id " + std::to_string(bid) + ") lock vehicle id=" + std::to_string(vid) + " (its last driver)") &&
                           rig.LogHas("SetLocked refused: Alice tried to unlock vehicle id=" + std::to_string(vid) + ": TooFar") &&
                           rig.LogHas("SetLocked refused: Bob tried to unlock vehicle id=" + std::to_string(vid) + ": NotAllowed") &&
                           rig.LogHas("Enter refused: Carol (id " + std::to_string(cid) + ") at locked vehicle id=" + std::to_string(vid) + " seat 1: Locked (no key;");
        Check("doors net R9: the server counted 3 locks, 3 unlocks, 5 refused SetLocked and 5 entries refused Locked, and logged the lock, the unlock refusal (TooFar, NotAllowed) and the locked-entry refusal",
              st.vehicleLocks == 3 && st.vehicleUnlocks == 3 && st.lockRefused == 5 && st.lockedEntryRefused == 5 && lines && rig.LogHas("doors: vehicle locks on"),
              F("locks %llu unlocks %llu refused %llu entries refused %llu, log lines %d", (unsigned long long)st.vehicleLocks, (unsigned long long)st.vehicleUnlocks,
                (unsigned long long)st.lockRefused, (unsigned long long)st.lockedEntryRefused, (int)lines));
    }
}

static void DS_Config()
{
    using Code = proto::VehicleResultCode;
    std::printf("-- 0.4.22 B. doors and seats: the server switches\n");
    {
        server::Config cfg = Rig::BaseConfig();
        cfg.vehicleLocks = 0;
        Rig rig;
        bool ok = false;
        std::string got = "setup failed";
        if (rig.Start(cfg))
        {
            Probe alice(rig);
            if (alice.Join("Alice") && PlaceAt(rig, alice, Kin(100, 100, 30)))
            {
                const proto::VehicleResult spawn = VCmd(rig, alice, proto::VehicleOp::Spawn, 0, kAdder);
                const proto::VehicleResult enter = VCmd(rig, alice, proto::VehicleOp::Enter, spawn.vehId, 0);
                const proto::VehicleResult lock = VCmd(rig, alice, proto::VehicleOp::SetLocked, spawn.vehId, 1);
                const server::VehicleStore::Vehicle* v = rig.srv.FindVehicle(spawn.vehId);
                ok = spawn.code == Code::Ok && enter.code == Code::Ok && lock.code == Code::NotAllowed && v && !(v->s.flags & proto::kVehLocked) &&
                     rig.LogHas("doors: vehicle locks OFF") && rig.srv.stats().lockRefused == 1;
                got = F("spawn %s enter %s lock %s", proto::ToString(spawn.code), proto::ToString(enter.code), proto::ToString(lock.code));
            }
        }
        Check("doors config: vehicleLocks=0 refuses even the driver's SetLocked (NotAllowed, nothing locked) and the start-up log says the locks are OFF", ok, got);
    }
    {
        const std::string path = (std::filesystem::temp_directory_path() / "doors_seats_server_cfg.json").string();
        auto load = [&](const std::string& text, server::Config& c) {
            std::ofstream(path, std::ios::binary) << text;
            std::string err;
            return server::LoadConfigFile(path, c, &err);
        };
        server::Config def, a, b, c, d, e, f;
        const bool defOk = def.vehicleLocks == 1 && def.vehicleLockDistance == 12;
        const bool off = load("{ \"vehicleLocks\": 0, \"vehicleLockDistance\": 0 }", a) && a.vehicleLocks == 0 && a.vehicleLockDistance == 0;
        const bool top = load("{ \"vehicleLocks\": 1, \"vehicleLockDistance\": 100 }", b) && b.vehicleLocks == 1 && b.vehicleLockDistance == 100;
        c.vehicleLockDistance = 77;
        const bool bad101 = !load("{ \"vehicleLockDistance\": 101 }", c) && c.vehicleLockDistance == 77;
        d.vehicleLocks = 1;
        const bool bad2 = !load("{ \"vehicleLocks\": 2 }", d) && d.vehicleLocks == 1;
        e.vehicleLockDistance = 77;
        const bool badText = !load("{ \"vehicleLockDistance\": \"12\" }", e) && e.vehicleLockDistance == 77;
        const bool valid = server::Validate(f, nullptr);
        f.vehicleLockDistance = 101;
        const bool validBad = !server::Validate(f, nullptr);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        Check("doors config: vehicleLocks defaults to 1 and vehicleLockDistance to 12 m, 0/0 and 1/100 load, 101, 2 and a string are refused with the config untouched, Validate refuses 101",
              defOk && off && top && bad101 && bad2 && badText && valid && validBad,
              F("default %d, 0/0 %d, 1/100 %d, 101 refused %d, locks 2 refused %d, string refused %d, validate %d/%d", (int)defOk, (int)off, (int)top, (int)bad101, (int)bad2,
                (int)badText, (int)valid, (int)validBad));
    }
}

static void DS_Store()
{
    using Code = server::VehicleStore::Code;
    std::printf("-- 0.4.22 C. doors and seats: VehicleStore\n");
    {
        // The last driver's key ends with the disconnect: a later player with the same id is a stranger.
        StoreRig R;                                              // player 0 drives (the last driver); the car is adopted: no spawner
        const Code exit0 = R.vs.Exit(0, R.veh, 600, R.tick++);
        const Code lock0 = R.vs.SetLocked(0, R.veh, true, At(3, 0), true, R.tick++);
        const Code lock1 = R.vs.SetLocked(1, R.veh, false, At(3, 0), true, R.tick++);
        const Code enter1 = R.vs.Enter(1, R.veh, 1, At(3, 0), 700, R.tick++);
        const Code enter0 = R.vs.Enter(0, R.veh, 0, At(3, 0), 710, R.tick++);
        const Code exit0b = R.vs.Exit(0, R.veh, 720, R.tick++);
        R.vs.PlayerGone(0, 800, R.tick++);
        const bool keyGone = R.vs.Get(R.veh) && R.vs.Get(R.veh)->lastDriver == proto::kNoPlayer;
        R.vs.AddReceiver(0);                                     // "player 0" again
        const Code reLock = R.vs.SetLocked(0, R.veh, false, At(3, 0), true, R.tick++);
        const Code reEnter = R.vs.Enter(0, R.veh, 0, At(3, 0), 900, R.tick++);
        Check("store: the last driver locks, a stranger cannot unlock or enter (NotAllowed, Locked), the last driver re-enters; after his disconnect nobody holds the key and a new player "
              "with his id is a stranger (NotAllowed, Locked)",
              exit0 == Code::Ok && lock0 == Code::Ok && lock1 == Code::NotAllowed && enter1 == Code::Locked && enter0 == Code::Ok && exit0b == Code::Ok && keyGone &&
                  reLock == Code::NotAllowed && reEnter == Code::Locked,
              F("%s %s %s %s %s %s, key gone %d, %s %s", proto::ToString(exit0), proto::ToString(lock0), proto::ToString(lock1), proto::ToString(enter1), proto::ToString(enter0),
                proto::ToString(exit0b), (int)keyGone, proto::ToString(reLock), proto::ToString(reEnter)));
    }
    {
        // The lock distance (default 12 m), no position, an occupant needing none, the unlimited distance and the switch.
        StoreRig R;
        R.vs.Exit(0, R.veh, 600, R.tick++);
        const Code at11 = R.vs.SetLocked(0, R.veh, true, At(11, 0), true, R.tick++);
        const Code at13 = R.vs.SetLocked(0, R.veh, false, At(13, 0), true, R.tick++);
        const Code noPos = R.vs.SetLocked(0, R.veh, false, At(0, 0), false, R.tick++);
        R.vs.Enter(0, R.veh, 0, At(2, 0), 700, R.tick++);
        const Code inside = R.vs.SetLocked(0, R.veh, false, At(900, 900), false, R.tick++);      // seated: the position is not looked at
        server::VehicleStore::Rules any = StoreRules();
        any.lockDistance = 0;
        server::VehicleStore vs2;
        vs2.Configure(any, 8);
        for (uint16_t p = 0; p < 4; p++) vs2.AddReceiver(p);
        uint16_t v2 = 0;
        vs2.Create(1, At(0, 0), proto::QuatFromHeading(0.0f), 0, 0, proto::kNoPlayer, 0, true, 0, 1, &v2);
        vs2.Enter(0, v2, 0, At(0, 0), 500, 2);
        vs2.Exit(0, v2, 600, 3);
        const Code anywhere = vs2.SetLocked(0, v2, true, At(5000, 5000), true, 4);
        server::VehicleStore::Rules off = StoreRules();
        off.locksEnabled = false;
        server::VehicleStore vs3;
        vs3.Configure(off, 8);
        for (uint16_t p = 0; p < 4; p++) vs3.AddReceiver(p);
        uint16_t v3 = 0;
        vs3.Create(1, At(0, 0), proto::QuatFromHeading(0.0f), 0, 0, proto::kNoPlayer, 0, true, 0, 1, &v3);
        vs3.Enter(0, v3, 0, At(0, 0), 500, 2);
        const Code disabled = vs3.SetLocked(0, v3, true, At(0, 0), true, 3);
        const Code missing = R.vs.SetLocked(0, 60, true, At(0, 0), true, R.tick++);
        Check("store: a key holder outside is Ok at 11 m and TooFar at 13 m (default 12), NoState without a position, an occupant needs none; lockDistance 0 means any distance; "
              "locksEnabled=false refuses the driver; a missing car is NoVehicle",
              at11 == Code::Ok && at13 == Code::TooFar && noPos == Code::NoState && inside == Code::Ok && anywhere == Code::Ok && disabled == Code::NotAllowed &&
                  missing == Code::NoVehicle,
              F("%s %s %s %s | any %s | off %s | missing %s", proto::ToString(at11), proto::ToString(at13), proto::ToString(noPos), proto::ToString(inside),
                proto::ToString(anywhere), proto::ToString(disabled), proto::ToString(missing)));
    }
    {
        // Idempotent (no fresh frames for a repeat) and the flag rides every entry, motion-only ones included.
        StoreRig R;
        R.vs.Update(0, Drive(R.veh, 0, 1, 10), 1000);
        const Code first = R.vs.SetLocked(0, R.veh, true, At(0, 0), true, R.tick++);
        const uint32_t changed = R.vs.Get(R.veh)->changedTick;
        R.tick += 10;
        const Code again = R.vs.SetLocked(0, R.veh, true, At(0, 0), true, R.tick++);
        const bool unchanged = R.vs.Get(R.veh)->changedTick == changed;
        proto::VehicleWorld w;
        int seenEntries = 0, withFlag = 0, motionOnly = 0, motionOnlyWithFlag = 0;
        for (int i = 0; i < 12; i++)
        {
            R.vs.Update(0, Drive(R.veh, 0, 2.0f + i, 10), 1050 + i * 50);
            R.Frame(1, 1080 + i * 50, w);
            if (const proto::VehicleState* e = R.Entry(w))
            {
                ++seenEntries;
                if (e->flags & proto::kVehLocked) ++withFlag;
                if (e->motionOnly)
                {
                    ++motionOnly;
                    if (e->flags & proto::kVehLocked) ++motionOnlyWithFlag;
                }
            }
        }
        Check("store: locking twice is Ok and changes nothing the second time (no fresh full entries), and the locked flag is in every entry the observer is sent, motion-only ones included",
              first == Code::Ok && again == Code::Ok && unchanged && seenEntries >= 10 && withFlag == seenEntries && motionOnly >= 3 && motionOnlyWithFlag == motionOnly,
              F("%s %s unchanged %d, %d entries, %d with the flag, %d motion-only (%d with it)", proto::ToString(first), proto::ToString(again), (int)unchanged, seenEntries,
                withFlag, motionOnly, motionOnlyWithFlag));
    }
}

static void RunDoorsSeatsNetTests()
{
    DS_RealServer();
    DS_Config();
    DS_Store();
}
