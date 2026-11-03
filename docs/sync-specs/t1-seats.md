# T1 seats: server-authoritative enter / exit intent (desync overhaul round 2, wire v6)

GI's symptom 2: "Vehicle enter / exit / seat animations: getting into a car, sliding over a car, getting into a passenger seat of a car or
bike. Sometimes the player is teleported in, sometimes other players never see the enter animation at all."

## What was wrong (verified in the code, 4e2ef63)

* The real player NEVER played an enter animation on a networked car: `TickVehicleEntry` disabled F / G, ran to the door with
  `TASK_GO_STRAIGHT_TO_COORD` and called `SET_PED_INTO_VEHICLE` within 1.6 m (a teleport-in, by design since 0.4.2).
* The wire had no notion of "on the way to seat N": `PedState.move` 5 + `vehId` only, no seat, no door, and it is a state-channel snapshot
  at the render time.
* Receivers (Sync D, 0.4.6) defaulted to `puppet_enter=follow`: no enter task, the ordinary locomotion toward the sender's pose, and a warp
  into the seat the moment the seat list named the player. Before that (0.4.2..0.4.5) the puppet's own `TASK_ENTER_VEHICLE` was cut short by
  `EnterWarpDue` or timed out. So other screens saw a run and a pop, or nothing.
* Seat occupancy was the only server truth, so two players going for the same seat were only sorted out after both had been teleported in
  (the loser was ejected), and nothing could tell a screen that an entry had been given up.

## Design (confirms the orchestrator's suggestion; one change)

1. The server RESERVES the seat on an intent (`VehicleOp::EnterIntent`) and tells every relevant client (`SeatNotice`).
2. The player's own client starts the game's real `TASK_ENTER_VEHICLE` for the reserved seat only after the server's Ok.
3. Every other client replays the same task on the puppet, once per intent.
4. The seat warp is the BOUNDED FALLBACK (task never started / stopped / timed out / not callable), and it waits for the task inside
   `confirmGraceMs` after the server's word.
5. Exit: the normal exit animation is replayed from `SeatNotice::Exited`; bail-out above speed (existing `DecideUnseat`) is unchanged.

Change against the suggestion: the entry style (door / vault / bike) is NOT on the wire. The receiver cannot use it: the game picks the
animation from the seat and the car, and each client knows the car's class (`GET_VEHICLE_CLASS` 8 / 13) itself. The only per-intent
datum the receiver needs is how the sender approached (the run bit), because the task's speed argument (1 walk, 2 run) is the one
thing that makes the puppet arrive when the sender does. Keeping the wire at "seat + run" also keeps the reservation message a pure
account of occupancy intent, which is all the server can vouch for.

Vault / slide-over-a-car: belongs to T2 (generic on-foot: `kPfVaulting` / `kPfClimbing`, `ClimbWindow`, `TASK_CLIMB`). It is a climb of the
ped over an obstacle, not an entry into a seat; no seat, no reservation, no server state. T1 ends at the enter intent. (If GI's "sliding
over a car" is the GTA vehicle shuffle across the front seats, that is the already existing PCF 184 / revert path, not changed here.)

## Owner rules (unchanged, now enforced at the reservation too)

* F never jacks: `PickEntry` picks a free seat; "free" now also excludes a seat somebody else has reserved (the client's `SeatBook`), and the
  server refuses `SeatTaken` for a seat that is occupied or reserved; the enter task is issued with flags 1 (no jack flag).
* An owned car is driven only by its owner: `Reserve` of seat 0 of an owned car by anybody else is `NotOwner`, before everything else.
* Seat 0 stays the controller: unchanged (`AssignControllers` reads the seat list, which a reservation never touches).

## Live checks (GI)

See the T1 report. Switch: `HKCU\Software\GAGE-MP\seat_intent=0` restores the old entry (A/B).
