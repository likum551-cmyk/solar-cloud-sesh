// GAGE-MP in-game engine: per-frame multiplayer driver — implementation.
//
// `Frame` is installed as the game main-loop detour (see hook.cpp), so it runs
// every frame on the game's main/render thread — the only place game calls are
// safe. It keeps the GAGE session alive (join.cpp), sends the local player's
// state (throttled), and turns the other players' positions into in-world peds
// it moves each frame. A single "companion" ped mirrors the local player to prove
// the remote-entity pipeline even before a live server shows peers.
//
// Every primitive degrades: if LocalPlayer/EntityPos/SpawnPed are unresolved
// the frame simply does less. Nothing here blocks the render thread.
#include "engine.h"
#include "overlay.h"
#include "brand_apparel.h"
#include "pack_stream.h"
#include "pack_meta.h"
#include "minimap_size.h"
#include "radar_mask.h"        // 0.5.5 ring_map_round: the radar's own mask swapped for a circle
#include "sync/hud_ring.h"     // 0.5.2 ring_map: where the map square goes (MapCentrePx / MapSquarePx)
#include "sync/ring_round.h"   // 0.5.5 ring_map_round: the gate, the box fit
#include "quit_gate.h"
#include "ambient.h"
#include "world_clock.h"       // 0.5.7: the server's time of day and weather
#include "characters.h"        // 0.5.9: the server's characters (roster, our face, the peers' faces)
#include "common/common.h"
#include "common/logger.h"
#include "sync/smoother.h"
#include "sync/locomotion.h"
#include "sync/oneshot.h"
#include "sync/state_mirror.h"
#include "sync/vehicle_drive.h"
#include "sync/vehicle_link.h"
#include "sync/vehicle_body.h"
#include "sync/vehicle_visual.h"
#include "sync/vehicle_signals.h"
#include "sync/vehicle_controls.h"
#include "sync/indicator_fallback.h"
#include "sync/vehicle_state_row.h"
#include "sync/vehicle_damage.h"
#include "sync/pedstate.h"
#include "sync/cover.h"
#include "sync/vehicle_entry.h"
#include "sync/vehicle_lock.h"
#include "sync/vehicle_w9.h"
#include "sync/vehicle_contact.h"
#include "sync/vehicle_impact.h"
#include "sync/vehicle_position.h"
#include "sync/vehicle_syncd.h"
#include "sync/seat_intent.h"
#include "sync/nametag.h"
#include "sync/combat.h"
#include "sync/emote.h"
#include "sync/swing.h"
#include "sync/gtaids.h"
#include "sync/hypertune.h"
#include "sync/vehicle_shake.h"
#include "sync/pausemenu.h"
#include "sync/overlay_input.h"   // 0.5.4 overlay_combat_block: the controls an open panel holds disabled
#include "sync/radio_off.h"
#include "sync/mapclear.h"
#include "sync/minimap.h"
#include "sync/minimap_size.h"
#include "sync/noclip.h"
#include "sync/inventory.h"    // 0.5.4 outfit_items: the server's inventory, decoded for the ped's clothes
#include "sync/outfit.h"       // 0.5.4 outfit_items: worn items -> ped components
#include "sync/ped_view.h"     // 0.5.4 inv_ped_view: the CHARACTER panel's camera
#include "sync/ped_preview.h"  // 0.5.8 inv_ped_mode: the CHARACTER panel's ped on GTA's frontend ped renderer
#include "sync/inventory_world.h"   // 0.5.4 inventory world: PlanSnapshot / PlanRestore / BuildReport
#include "common/registry.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace gage;

// The mp.* bridge dispatch (defined in gage_mp.cpp as extern "C").
extern "C" void GageMP_Dispatch(const char* event, const char* payload);

namespace {

    game::World* g_world = nullptr;
    join::Config g_cfg;
    std::atomic<bool> g_active{false};
    std::atomic<bool> g_connecting{false};
    std::atomic<bool> g_giveUp{false}; // the server refused us for a reason retrying cannot fix (join::RefusalIsFinal)
    std::atomic<ULONGLONG> g_refusedRetryAt{0}; // GetTickCount64: after a refusal the server may lift (a taken name, a full server), no attempt before
    std::atomic<ULONGLONG> g_refusedNoticeAt{0}; // the last chat line about such a refusal (rate limit)
    std::thread g_connectThread;

    // The server's own sentence for a refusal the player can act on: today only Refuse(VersionMismatch)
    // ("Solar RP was updated. Close the game; Solar Launcher installs the update." / "This server has not been
    // updated yet. Try again in a minute."). Empty = show the generic "The server refused the connection".
    // Written by the connect thread before g_giveUp is set, read by the render thread.
    std::mutex g_refuseMu;
    std::string g_refuseText;
    std::atomic<ULONGLONG> g_refuseSaidAt{0};            // GetTickCount64 of the last chat line with that text
    constexpr ULONGLONG kRefuseRepeatMs = 12000;         // chat lines fade; say it again while the player stays

    // Throttle timers (GetTickCount(), ms) so the render thread stays cheap.
    std::atomic<ULONG> g_lastSpawnTried{0};
    std::atomic<ULONG> g_lastConnectAttempt{0};

    // Game-thread health for the watchdog: frames run so far, and when the current
    // one began (0 = not inside Frame). Tells "Present is never called" apart from
    // "the game thread is stuck inside our frame".
    std::atomic<uint64_t> g_frames{0};
    std::atomic<ULONGLONG> g_frameEnteredAt{0};
    std::atomic<bool> g_watchdogStarted{false};
    // The local player as last read on the game thread, for the watchdog line
    // (handle 0 = not read yet).
    std::atomic<int32_t> g_localPed{0};
    std::atomic<float> g_localX{0}, g_localY{0}, g_localZ{0}, g_localHeading{0};

    // ---- wire protocol -----------------------------------------------------
    // GAGE protocol v1 (docs/ENGINE-PLAN.md), carried by join.cpp:
    //   out local state: proto::Kinematics (position, heading, velocity)
    //   out chat:        a text line
    //   in  players:     PlayerJoined / PlayerLeft, and a WorldState of the others' positions
    //   in  chat:        ChatMessage (from a player or the server)

    // ---- remote peer state (driven by server messages) ---------------------
    // Where a peer is comes from g_sync (libs/sync RemoteSync), not from the Peer: the
    // join worker feeds every WorldState into it, the script tick samples a smoothed
    // pose per frame. Peer is the per-player bookkeeping around that pose.
    //
    // THREADS (sync plan N6a). Two maps per kind, so that no native call is ever made under g_peersMtx:
    //   g_peerNet / g_vehNet  what the join worker learned (PeerNet, VehNet); guarded by g_peersMtx; the join
    //                         worker's callbacks write them and do nothing else (no natives, no logging).
    //   g_peers / g_vehs      the game thread's own entries (Peer, NetVeh), with the entity handles and every
    //                         native-driven state; touched by the game thread only, with no lock.
    // Once per script tick SyncNet() (under the lock, copies and smoother arithmetic only) moves what the join
    // worker wrote into the game thread's entries and samples the smoother for every peer; when the tick has
    // removed an entry it tells the net map under the lock again (CommitNet). So a callback that arrives while
    // the tick is busy with natives is never held up, and is seen by the next tick (one frame later at most).
    // W8 item 3 (a): the places that hand a puppet's locomotion controller back (Reset: the task list was cleared or
    // replaced, the gait task must be issued again) or ask it to re-evaluate (SoftReset: the gait task may still run),
    // tallied per peer and printed on the 10 s "engine: loco" line. Add a site here, in kLocoSiteName and at the call.
    enum LocoSite : size_t
    {
        kSiteHitReact, kSiteEmoteEnd, kSiteCoverExit, kSiteEnterWalk, kSiteEnterTimeout, kSiteEnterGaveUp, kSiteDriveMode,
        kSiteLeaveVeh, kSiteTierBDrift, kSiteBackFromVeh, kSiteAimEnd, kSiteCollisionBack, kSiteClimbEnd, kSiteUnseat,
        kSiteWater, kSiteAnchorLost, kSiteCoverFail, kSiteRepair, kLocoSites
    };
    const char* const kLocoSiteName[kLocoSites] = {
        "hit-react-end", "emote-end", "cover-exit", "enter-walk", "enter-timeout", "enter-gave-up", "drive-mode",
        "leave-vehicle", "tier-b-drift", "back-from-vehicle", "aim-end", "collision-back", "climb-end", "unseat", "water-edge",
        "anchor-lost", "cover-fail", "repair"
    };

    // W11 (docs/CLOUD-HANDOFF.md item 0b): loco_mode = anchor (default) | straight, re-read at every Welcome like peer_drive. Anchor: each
    // on-foot puppet runs TASK_GO_TO_ENTITY once to an invisible, collision-less, frozen, non-networked helper ped that DrivePeer moves
    // to the controller's predicted point every frame (libs/sync/locomotion.h item 8). Straight: the old far-point
    // TASK_GO_STRAIGHT_TO_COORD, re-issued on turn / drift / arrival. The controller falls back to straight by itself for a
    // frame without an anchor entity, while aiming, and for 20 s after a lost task.
    // Read by the join worker's OnWelcome and by the game thread's SyncNet (Peer construction): atomic.
    //
    // The walking-variant test series: the build's DEFAULT variant is baked here, one flip per release, so every player on a version runs the
    // same one with nothing in the registry. A registry value (loco_mode) still overrides it and the Welcome log line says which side decided.
    // 0.4.8 = anchor (anchor task + coordinate nudge), 0.4.9 = straight (straight task + velocity push), R4 T1 (0.4.10) = blend (straight task as
    // the gait source only + a velocity write every frame, libs/sync/locomotion.h "R4 T1"). One tri-state instead of the two bools of 0.4.9:
    // anchor and straight keep the push they were tested with (loco_push still overrides it for those two).
    enum LocoSel : int { kLocoSelAnchor = 0, kLocoSelStraight = 1, kLocoSelBlend = 2 };
    constexpr int kBakedLocoMode = kLocoSelBlend;
    std::atomic<bool> g_locoAnchor{kBakedLocoMode == kLocoSelAnchor};
    std::atomic<bool> g_locoBlend{kBakedLocoMode == kLocoSelBlend};
    // T2 (desync round 2): loco_push = nudge (default) | velocity: how a walking puppet's position error is closed. Nudge = a few mm of
    // SET_ENTITY_COORDS_NO_OFFSET per frame (exact whatever SET_ENTITY_VELOCITY does to a ped under a task); velocity = the 0.4.6 push.
    // loco_selfcheck = 0 turns the degradation ladder off (anchor -> straight -> blend-carried speed -> rigid follow, each step
    // logged as "engine: loco level"). Both re-read at every Welcome like loco_mode.
    std::atomic<bool> g_locoPushNudge{kBakedLocoMode == kLocoSelAnchor};   // (anchor: nudge, straight: velocity; unused by blend)
    std::atomic<bool> g_locoSelfCheck{true};
    // R3 T2 (desync round 3): loco_r3 = 0 switches every round-3 rule off at once (the quiet window and write verdict, the lost task needing
    // the ped's own speed, the re-issue spacing, the ladder hold and cap: sync::Round2Behaviour); loco_repair = 0 only the repairs of a
    // puppet no task moves (ClearTasks, then the lean and the alt creation recipe). Re-read at every Welcome like loco_mode.
    std::atomic<bool> g_locoR3{true};
    std::atomic<bool> g_locoRepair{true};
    std::atomic<bool> g_locoGaitClip{true};   // 0.4.17: loco_gait_clip=0 = the 0.4.15 speed-derived blend ratio
    // 0.4.29: loco_face (default on) = a Blend puppet with a moving gait gets its HEADING written outright every frame (the controller's
    // 50 ms step toward the sender's heading) and the sender's heading as its desired heading, as FiveM's ped blender does. The old path fed
    // SET_PED_DESIRED_HEADING with "the ped's own heading plus a small step", and the live R5 lines (0.4.27, both editions) showed the puppet facing
    // 100-116 degrees (p95 up to 175) away from where it moved: the moonwalk. loco_face=0 = that old path. Re-read at every Welcome.
    std::atomic<bool> g_locoFace{true};
    // R5 (on-foot sync): loco_blend_grace = 0 puts the round-4 Blend rules back whole (no grace window after a task-changing Reset, no probation, no trim gate,
    // the 30 s / 300 s Placed hold, the 8 s ladder hold, 12 level changes: sync::Round4SelfCheck). loco_placed_ms = N sets the Placed hold after a
    // blend fall that a task-changing event explains to N ms (cap 4 N; a fall nothing explains keeps 30 s / 300 s); 0 = the old 30 s hold for every fall;
    // unset = the shipped 6 s. Both re-read at every Welcome. Blend only (the anchor and straight configs never had a grace).
    std::atomic<bool> g_locoBlendGrace{true};
    std::atomic<int>  g_locoPlacedMs{-1};     // -1 = unset (shipped); 0 = the round-4 30 s; N > 0 = N ms
    // 0.5.7: loco_swing_walk (default on) = a peer's upper-body / secondary melee clip (SwingVariant Upper, UpperHard, Looped, once seen playing)
    // no longer holds the gait task: the go-straight / stand-still issue follows the pose through the swing and its 700 ms hold (repairs stay
    // held, a full-body / plain clip or the melee task still holds everything). Live 0.5.5 08:00:51: a standing attacker punched and walked off,
    // the puppet kept its stand-still task, IS_PED_STILL read true on 179 of 179 asks and our velocity writes slid it 1.7 m. 0 = the 0.5.6 hold.
    std::atomic<bool> g_locoSwingWalk{true};
    sync::LevelWitness g_locoDegrade;     // script tick only
    std::atomic<bool> g_locoDegradeReset{false};   // OnWelcome (join worker) asks, the next DrivePeer clears
    sync::LocoConfig LocoCfg()
    {
        sync::LocoConfig c = g_locoBlend.load() ? sync::ShippedLocoConfig(sync::LocoMode::Blend) : sync::ShippedLocoConfig(g_locoAnchor.load());
        if (!g_locoBlend.load() && !g_locoPushNudge.load())
            c.push = sync::PushMode::Velocity;
        c.selfCheck = g_locoSelfCheck.load();
        if (g_locoBlend.load())
        {
            if (!g_locoBlendGrace.load())
                c = sync::Round4SelfCheck(c);
            if (g_locoPlacedMs.load() >= 0)
                c = sync::WithPlacedHold(c, g_locoPlacedMs.load());
        }
        if (!g_locoR3.load())
            c = sync::Round2Behaviour(c);
        if (!g_locoRepair.load())
            c.maxRepairs = 0;
        c.blendGaitClip = g_locoGaitClip.load();
        c.swingWalkThrough = g_locoSwingWalk.load();
        return c;
    }

    // W10 (player name tags): nametags=0 turns them off, nametags_los=<flags> changes the line-of-sight trace (R*'s scripts pass 17).
    // Both are re-read at every Welcome. Read by the game thread (TickNameTags), written by the join worker: atomic.
    std::atomic<bool> g_nameTags{true};
    std::atomic<int>  g_nameTagTrace{17};

    struct Peer
    {
        std::string name;        // copied from PeerNet by SyncNet
        bool  gone = false;      // left the server; the frame deletes the ped and the entry (script tick)
        bool  despawn = false;   // out of relevance (WorldState::left) or forgotten: the frame deletes
                                 // the ped but keeps the entry; it respawns when the peer returns
        int32_t ped = 0;         // in-world script handle for this peer
        bool  loggedSeen = false; // logged the first sample for this peer
        double lastDriveMs = 0;  // NowMs() of the last DrivePeer (0 = not driven since the spawn)
        // Sync health since the last 10 s log line (script tick).
        uint32_t samples = 0, extrapolated = 0, held = 0, snaps = 0;
        // Stage 2: turns the pose into ped tasks / velocity / heading (libs/sync/locomotion.h).
        // ResetForNewPed on every (re)spawn, Reset on a peer_drive mode change. Script tick only.
        sync::LocomotionController loco{LocoCfg()};
        // W11: the anchor entity this puppet's go-to-entity task follows (0 = none yet / lost), when its creation was last tried
        // and how many times the create call failed since the last log line. Deleted with the puppet. Script tick only.
        int32_t anchor = 0;
        double anchorTryMs = -1e9;
        uint32_t anchorSpawnFails = 0;
        int driveMode = -1;      // the PeerDrive the ped was last driven with (-1 = not yet)
        uint32_t locoSnapsSeen = 0, locoGoSeen = 0, locoStandSeen = 0; // LocoStats at the last log line
        // W8: the controller's churn per trigger (LocoStats at the last "engine: loco" line, and the Reset / SoftReset callers
        // since then), the sender's swimming edges (item 4) and its climb / vault as one TASK_CLIMB window (item 5).
        sync::LocoStats locoSeen;
        sync::LocoDiag slide;    // Sync A: the slide metric of the last 10 s ("engine: loco slide"); script tick only
        // R5 telemetry: the sender's swim / ragdoll flag against what IS_PED_SWIMMING / IS_PED_RAGDOLL say the puppet does ("engine: edges"); script tick only
        sync::StateMirror swimMirror, ragMirror;
        int32_t mirrorPed = 0;   // the ped the two mirrors were last based on (a new ped re-bases them)
        sync::SiteTally<kLocoSites> hardSites, softSites;
        sync::WaterEdge water;
        sync::ClimbWindow climb;
        sync::OneShotAudit audit;   // T2: did the game SHOW the jump / reload / stealth / swim / ragdoll we asked for (script tick)
        // R3 T2: the creation recipe of the puppet's ped (0 full, 1 lean, 2 alt: World::SpawnRemotePed), the repairs done on it
        // (LocoInput::repairsDone), the gait diagnostic of the last 10 s ("engine: gait") and its log-line budgets. Script tick only.
        int recipe = 0;
        int repairsDone = 0;
        sync::GaitDiag gaitDiag;
        // 0.5.7: the still-slide read-backs by cause and the clip read after a walk-through issue ("engine: on-foot swing walk (0.5.7)")
        sync::SwingWalkDiag swingWalkDiag;
        double swingWalkClipAtMs = -1.0;   // NowMs() of a walk-through issue whose clip read is still due (-1 = none)
        uint32_t verdictLogged = 0;
        bool capLogged = false;

        // Stage 3 (protocol v3). Appearance: the newest PeerLook (PeerNet::look, copied by SyncNet, applied
        // by the script tick); lookDirty until it is on the ped. pedModel is the model the ped was spawned with.
        proto::Look look;
        bool hasLook = false, lookDirty = false, lookFailLogged = false;
        bool lookModelFailed = false; // look.model did not stream in: the kCompanionModel stand-in, retried
        double lookFailedAtMs = 0;    // when it failed / the last retry gave up (NowMs)
        uint32_t pedModel = 0;
        // 0.5.9 characters: the player's face (PeerNet::face, copied by SyncNet); faceDirty until it is on the ped. Put on right after
        // every look (ApplyPeerLook) and on its own when only the face changed (characters::ApplyPeer).
        sync::chr::Appearance face;
        bool hasFace = false, faceDirty = false;
        uint32_t netFaceSeq = 0;
        double spawnSinceMs = 0;  // NowMs() of the first spawn attempt of the current ped (0 = none)
        // A model we asked GTA to stream (SpawnRemotePed's REQUEST_MODEL, or a stand-in retry) and
        // have not handed back: released (SET_MODEL_AS_NO_LONGER_NEEDED) whenever we stop trying it.
        // SpawnRemotePed releases it itself on success. Script tick only.
        uint32_t requestedModel = 0;
        // What the ped shows of the remote PedState (script tick): the weapon given (0 = none yet),
        // whether an aim task runs and when it is re-issued (sync::AimGate), the last gait target, the last move.
        uint32_t weaponShown = 0;
        bool aimTask = false;
        sync::AimGate aimGate;
        // Seated drive-by (sync plan W2 F): when the drive-by (or its fallback aim) was last issued, whether
        // TASK_DRIVE_BY was uncallable so the plain aim task is used, and whether `weaponShown` is a gun.
        double driveByAtMs = 0;
        bool driveByFallback = false, driveByLogged = false;
        bool weaponIsGun = false;    // IsGunWeapon of the shown weapon, refreshed each seated frame
        bool collisionOff = false;   // climbing / parachute: collision is off and the ped is placed (DrivePeer)
        double leaveUntilMs = 0;      // NowMs() when the TASK_LEAVE_VEHICLE exit of UnseatPeer should be done (0 = none)
        sync::LocoVec lastTarget;
        bool haveLastTarget = false;
        float lastMbr = 1.0f;
        uint8_t lastMove = proto::kMoveIdle;
        uint32_t shootingFrames = 0;  // frames whose pose had kPfShooting, since the last log line
        proto::PedState lastPed;      // the last pose's PedState (for the log line)

        // ---- protocol v4 (script tick unless marked "join worker") ----
        int lastShotSeq = -1;         // the pose's shotSeq at the last frame (-1 = none since the spawn)
        int hpPool = 0;               // the local health the ped is held at for hit detection (0 = invincible)
        bool deadShown = false;       // the ped is playing dead
        bool respawnPed = false;      // delete and respawn the ped (a death ended, or it died locally)
        double killedAtMs = 0;        // a Killed for this player, when to show it (NowMs; 0 = none; from PeerNet)
        int seatedVeh = -1;           // the server vehicle the ped sits in on this machine (-1 = none)
        int seatedSeat = -1;          // its wire seat there
        double seatTryMs = 0;         // the last SetPedIntoVehicle attempt
        // Emotes: the join worker stores the newest event and bumps PeerNet::emoteSeq; SyncNet copies it here
        // and the tick applies it.
        uint32_t emoteSeq = 0, emoteSeen = 0;
        int emoteKind = 0;            // of the newest event: 0 stop, 1 anim, 2 scenario
        double emoteAtMs = 0;         // when it arrived (a one-shot older than 2 s is not started)
        proto::Anim emoteIn;          // the newest PeerAnim
        std::string scenarioIn;       // the newest PeerScenario
        proto::Anim emoteAnim;        // what the arbiter plays (script tick)
        std::string emoteScenario;
        sync::EmoteArbiter emote = sync::EmoteArbiter(sync::PeerEmoteConfig());   // 0e: a loop the game cancelled comes back (twice)
        sync::EmoteDictHold emoteDict;   // the animation dictionary this peer's emote keeps loaded (g_animDicts)

        // ---- sync plan W6, on-foot Tier B (script tick) ----
        sync::PedStateEdges edges;       // the edges of the sender's reload / stealth / cover / entering / exiting
        bool stealthOn = false;          // SET_PED_STEALTH_MOVEMENT is on for this ped
        double stealthAtMs = 0;          // ... when it was last (re)asserted
        sync::NameTagTrack nameTag;      // W10: this peer's tag across frames (eased alpha, last position, LOS cache)
        sync::CoverPuppet cover;         // R3 T3: the verified cover window (Wait / Pending / Held / crouch-pose fallback), libs/sync/cover.h
        float coverIssuePos[3] = {0, 0, 0};   // ... where the puppet stood when the cover task went out (how far the game moved it)
        double coverIssueMs = 0;              // ... and when
        int enterVeh = -1;               // the server vehicle id a TASK_ENTER_VEHICLE walks this ped to (-1 = none)
        double enterTaskMs = 0;          // ... when it was issued (> 0 while it runs); EnterWarpDue's clock
        // ---- W9 (vehicles, passengers, collision; script tick) ----
        sync::SeatRewarpLimiter seatLimiter;   // item 2: a ped that stays shuffled is re-warped on a growing gap, not every 100 ms
        double carryFromMs = 0;                // item 5: when the ped bailed out of a moving vehicle (0 = none) ...
        double carryOutMs = 0;                 // ... when it was first seen outside it (the fade starts then) ...
        float carryVel[3] = {0, 0, 0};         // ... and the vehicle's velocity it is thrown with, fading over kCarryVelocityMs
        double lastImpactMs = -1e9;            // item 6: when a vehicle impact of ours was last shown on this puppet

        // ---- sync plan W3, combat client (script tick unless marked "join worker") ----
        sync::ShotDebt shotDebt;         // shots owed to this ped (shotSeq stepped) and not fired yet: 3 a frame
        sync::MeleeSwingGate swingGate;  // 0c: a shotSeq step or a rising kPfMelee is ONE swing to show (300 ms dedupe)
        sync::PendingSwing swingPending; // 0e: a swing that waited for its dictionary is still shown if it loads within 250 ms
        sync::SwingProbe swingProbe;     // 0e: IS_ENTITY_PLAYING_ANIM after the swing task (did the clip really play?)
        bool swingProbeArmed = false;    // ... and which candidate list it belongs to (false unarmed, true armed)
        // R3 T6: the swing in flight, for the read-back's reasons and the bounded retry (script tick)
        sync::SwingVariant swingVariant = sync::SwingVariant::Upper;   // how it was issued (the ladder's rung at the time)
        uint32_t swingLocoMark = 0;      // the locomotion controller's task count at the issue: more later = a task replaced the swing
        double swingIssuedMs = 0;
        bool swingRetried = false;       // the one bounded re-issue was spent
        // R4 T3: the swing in flight, for the progress check and the per-swing line
        sync::SwingAttempt swingAttempt; // variant + duration it was issued with (the survey's attempt or the ladder's rung)
        int swingPlanIdx = -1;           // the survey plan entry (-1: not a survey swing)
        uint32_t swingNo = 0;            // this session's swing number
        sync::SwingProbe::Result swingFirst = sync::SwingProbe::Result::Idle;   // what the first sighting said
        bool swingSay = false;           // the overlay names this swing (survey / a /swing test)
        bool swingResetPending = false;  // a full-body swing ran: pe.loco.Reset() when SwingProbe::Active() ends
        float poseX = 0, poseY = 0, poseZ = 0;   // R4 T3: the pose DrivePeer was given this frame (where the owner really is), for the swing line
        bool poseKnown = false;
        int swingLocalHp = 0;            // melee-task rung: the local player's health + armour at the issue (harm breaker)
        double swingMeleeUntilMs = 0;    // melee-task rung: when the puppet's melee task is ended again (0 = none running)
        // R5 melee_loop: a looped swing clip (flags 49) is ended by us (STOP_ANIM_TASK, then a read-back); the melee-task rung of a puppet without a
        // locomotion anchor (Blend / Straight) borrows a short-lived anchor ped in front of it
        sync::SwingLoopStop loopStop;
        const char* loopDict = nullptr;  // ... the clip being looped (the swing tables are static: the pointers stay valid)
        const char* loopClip = nullptr;
        int32_t swingAnchor = 0;         // ... the borrowed anchor ped (0 = none)
        double swingAnchorUntilMs = 0;   // ... and when it is removed again
        sync::MeleeEventGate meleeEvents; // 0.4.5: the damage event of THIS puppet is cleared on the first frame of a swing window (stale ones)
        bool infiniteAmmo = false;       // real peer fire: SET_PED_INFINITE_AMMO_CLIP is on this ped
        sync::HitReactArbiter hitReact;  // a cosmetic reaction owns the ped for its window (DrivePeer holds the gait)
        bool reactLifted = false;        // PCF 188 (DisableHurt) and the ragdoll block are lifted for the reaction
        bool reactRagdolled = false;     // ... and the reaction is a knockdown (the end clears the ped's tasks)
        bool reactClip = false;          // 0.4.30: the reaction is the stagger clip (a full-body TASK_PLAY_ANIM replaced the gait: re-issue it at the end)
        // Damage the server reported on this player (our own Hit confirmed, or protocol v5's onlooker Damage:
        // attacker and victim both other players), not yet reacted to. PeerNet::react* are added here by SyncNet:
        // the damage sum, the OR of the Damage.flags (HitReact::kFlag*), the newest bone and the newest attacker.
        uint32_t reactDamage = 0;
        uint8_t  reactFlags = 0;
        int16_t  reactBone = -1;
        uint16_t reactAttacker = proto::kNoPlayer;

        // ---- set by SyncNet each script tick ----
        bool poseOk = false;          // the smoother had a pose for this peer at this tick's time
        sync::Pose pose;              // ... this one
        bool haveSyncStats = false;   // (report ticks only) the smoother's numbers for the sync log line
        double syncDelayMs = 0, syncJitterMs = 0;
        sync::SmootherStats syncStats;
        // Sync pipeline review (the `engine: pipe` line): the window the smoother kept since the last report (stored-snapshot
        // arrival gaps, the longest run of extrapolation), its class extra, and the stats as of the previous report.
        sync::ArrivalWindow pipeArrivals;
        double pipeLongestExtrapMs = 0, pipeExtraMs = 0;
        int pipeClass = 1;
        sync::SmootherStats pipeSeen;
        // What of the PeerNet has been taken over (see SyncNet).
        uint32_t netEpoch = 0, netLookSeq = 0, netEmoteSeq = 0;
    };
    // Last coarse (WorldCoarse, ~1 Hz) position per player id, for map blips later. Never drives a ped.

    // What the join worker knows about one remote player (see THREADS above). The worker writes it under
    // g_peersMtx; the script tick reads it only in SyncNet. Counters (lookSeq, emoteSeq, epoch) tell SyncNet what
    // is new; `despawn` and `killedAtMs` and `reactDamage` are taken over and cleared there.
    struct PeerNet
    {
        std::string name;
        bool gone = false;           // PlayerLeft (or the session ended): the tick deletes the ped and both entries
        bool despawn = false;        // out of relevance: the tick deletes the ped but keeps the entry
        proto::Look look;
        bool hasLook = false;
        uint32_t lookSeq = 0;        // bumped when `look` changed
        double killedAtMs = 0;       // a Killed for this player (NowMs; 0 = none pending)
        uint32_t emoteSeq = 0;       // bumped by every emote event; the newest one is in the fields below
        int emoteKind = 0;           // 0 stop, 1 anim, 2 scenario
        double emoteAtMs = 0;
        proto::Anim emoteIn;
        std::string scenarioIn;
        uint32_t reactDamage = 0;    // Damage to react to on this puppet (ours or an onlooker's), not yet taken over
        uint8_t  reactFlags = 0;     // ... the OR of its Damage.flags
        int16_t  reactBone = -1;     // ... the newest bone
        uint16_t reactAttacker = proto::kNoPlayer; // ... the newest attacker
        uint32_t epoch = 0;          // bumped when a new player takes over an id before the tick removed the old one
        sync::chr::Appearance face;  // 0.5.9: the newest PeerFace (the player's active character), decoded
        bool hasFace = false;
        bool faceBadLogged = false;  // ... one "does not decode" line per peer entry
        uint32_t faceSeq = 0;        // bumped when `face` changed
    };

    // g_peersMtx guards g_peerNet, g_vehNet, g_sync (with the connection's one render clock, g_sync.Clock(), which the
    // peds AND the vehicles read: sync plan W7), g_syncLive. Writers: the join
    // worker thread (OnWelcome / OnJoined / OnLeft / OnWorld / OnEnded and the v4 sinks) and, for a
    // few lines at a time, the script tick (SyncNet, CommitNet, the vehicle interpolators). RemoteSync is
    // single-threaded: every call to it is made under this lock. No native is called while it is held.
    std::mutex g_peersMtx;
    std::map<int, PeerNet> g_peerNet;
    sync::RemoteSync g_sync;
    bool g_syncLive = false;                   // a Welcome configured g_sync for the current session
    // The game thread's peers (script tick only, no lock). Entries are made by SyncNet, removed by TickPeers.
    std::map<int, Peer> g_peers;
    // The script tick's copy of g_syncLive, and the time of the frame (the peds were sampled at it; the cars are
    // sampled at it too, so one frame shows one instant of the server's timeline), taken in SyncNet.
    bool s_syncLive = false;
    double s_frameMs = 0;
    // The connection clock's pipeline numbers for the report tick's `engine: pipe clock` line (SyncNet fills it under the lock).
    struct PipeClockReport
    {
        bool have = false;
        sync::ArrivalWindow frameGaps;
        double baselineDriftMs = 0, stepMaxMs = 0, delayMs = 0, targetMs = 0, excessP95Ms = 0, ageP95Ms = 0, riseMaxMs = 0;
        sync::NetClockStats stats, seen;   // seen: as of the previous report
    };
    PipeClockReport s_pipeClock;
    // Serialises the game thread's work (a script tick or a render-thread fallback frame) with Stop(), which
    // clears the script-owned maps from another thread. Uncontended in play.
    std::mutex g_scriptMtx;
    // The server's tick rate from the last Welcome (0 = none yet): paces the sender.
    std::atomic<uint16_t> g_tickHz{0};

    // How remote peds follow their smoothed pose, A/B-switchable live: HKCU\Software\GAGE-MP
    // peer_drive = "tasks" (default: the locomotion controller walks/runs them) or "place" (stage 1:
    // the ped is set onto the pose every frame, no animation). Re-read at every Welcome, so changing
    // the value and reconnecting switches it without a restart.
    enum PeerDrive : int { kDriveTasks = 0, kDrivePlace = 1 };
    std::atomic<int> g_peerDrive{kDriveTasks};

    // SET_PED_MOVE_RATE_OVERRIDE (the locomotion animation speed, libs/sync LocoOutput::moveRate) came
    // back false once: the native is unresolved on this build, so the controller falls back to its
    // quantised move blend ratio from then on. Script tick only; logged once.
    bool g_moveRateFailed = false;

    // How long a remote ped's locomotion is paused while it climbs out of a vehicle (TASK_LEAVE_VEHICLE,
    // flags 0 plays the exit animation; ~1.2 s in the game).
    constexpr double kUnseatPauseMs = sync::kLeavePauseMs;

    // ---- protocol v4 state ---------------------------------------------------------------------
    // Server vehicles (VehicleWorld), keyed by the server's id. The join worker writes presence and full
    // states into VehNet (g_vehNet, under g_peersMtx); the script tick owns NetVeh (g_vehs, no lock): the
    // entity handles, the drive objects and SyncNet's copy of presence and state. The interpolator stays in VehNet
    // (the join worker pushes snapshots into it) and the tick samples it under the lock, a few lines at a time.
    struct VehNet
    {
        bool present = true;          // in the newest presence list
        bool removed = false;         // destroyed on the server
        bool haveState = false;
        proto::VehicleState st;       // the newest full state
        uint32_t stVersion = 0;
        sync::VehicleInterpolator interp;
        sync::ClassDelay extra;       // this car's class extra on the shared render clock (the tick, under the lock)
        uint32_t ctrlSeq = 0;         // counts hand-overs (and session starts); the tick resets the drive controller on a change
        uint32_t driveResetSeq = 0;   // Sync D: counts the hand-overs / session starts that must snap the drive controller (sync::DecideHandover)
    };
    std::map<uint16_t, VehNet> g_vehNet;          // g_peersMtx
    uint32_t g_vehSession = 0;                    // g_peersMtx: numbers the server sessions (each Welcome) for the interpolators
    std::atomic<uint8_t> g_playerHealth{proto::kDefaultHealth}; // the server's Welcome.playerHealth (v5): 100 + it is the ped's maximum
    struct NetVeh
    {
        bool present = true;          // in the newest presence list (copied by SyncNet)
        bool removed = false;         // destroyed on the server: delete the entity, drop the entry (copied)
        bool haveState = false;
        proto::VehicleState st;       // the newest full state (copied when stVersion changed)
        uint32_t stVersion = 0, appliedVersion = 0;
        uint32_t ctrlSeq = 0, appliedCtrlSeq = 0;   // VehNet::ctrlSeq (copied by SyncNet) and the one the tick has handled
        uint32_t driveResetSeq = 0, appliedDriveResetSeq = 0;   // Sync D: VehNet::driveResetSeq and the one the tick has handled
        uint32_t syncDPersistentSeen = 0, syncDUnderFloorSeen = 0, syncDBlendsSeen = 0, syncDGracesSeen = 0;   // drive stats at the last "(D)" line
        sync::DriveErrWindow errWin;  // Sync D: this copy's drive error against the sender's, per 10 s (the "vehicle drive (D)" line)
        // 0.5.7: the copy at rest against our commands (the "vehicle rest (0.5.7)" line) and the commands of the previous drive frame
        sync::VehRestDiag restDiag;
        bool restCmdYaw = false, restCmdVel = false;
        double restYawRate = 0, restLastMs = 0;
        sync::DriveVec restVel;
        sync::shake::CopyMeter shake; // 0.4.22: this copy's high-frequency error and error flips against its owner's stream (the "vehicle shake (0.4.22)" line)
        uint32_t angWrites = 0;       // 0.4.22: angular commands written to this copy (every kShakeAngCheckEvery-th is read back)
        int32_t handle = 0;           // the local entity (0 = not spawned)
        uint32_t spawnedModel = 0;
        bool adopted = false;         // `handle` is our own car, bound by a Register answer
        uint32_t requestedModel = 0;
        double spawnSinceMs = 0, spawnRetryAtMs = 0;
        bool badModelLogged = false;
        double leaveAtMs = 0;         // removed while we sit in it: when we were told to get out
        sync::VehicleDriveController drive;
        double lastDriveMs = 0;
        int appliedC1 = -1, appliedC2 = -1, appliedLock = -1, appliedLights = -1, appliedEngine = -1, appliedSiren = -1;
        uint16_t appliedBody = 0xFFFF, appliedEngineHp = 0xFFFF;
        sync::BodyShown bodyShown;    // the flags2 / doors bits this copy shows (a copy somebody else controls)
        // Controller side (we are st.ctrl, SendControlledVehicles): the cadence gate, when the motion and the body
        // were last read (a still car every kVehReadStillMs, the body at most every kBodyReadMs) and what went out.
        sync::VehicleSendGate sendGate;
        double lastReadMs = -1e9, bodyReadMs = -1e9;
        bool moving = false;
        uint8_t bodyFlags2 = 0, bodyDoors = 0;
        uint8_t sentFlags = 0, sentFlags2 = 0, sentDoors = 0;
        bool sentOnce = false;
        float steerPrev = 0.0f;       // the steering estimate's held value (below 1 m/s)
        sync::SirenHornHold sirenHornHold;   // 0.4.30: siren on + E counts as the horn only after a 150 ms hold (a tap switches the siren mode)
        float steerFieldWritten = NAN; // 0.4.30: the steering angle written to this copy last frame (NaN = none), for the held / overwritten read-back
        // ---- T3 (vehicle visual state, protocol v6) ----
        sync::VisualSync vis;         // tyres / windows: the controller's edge apply and a copy's level apply (libs/sync/vehicle_visual.h)
        sync::DriveQuat steerPrevRot; // the rotation of the previous steering sample (the yaw rate comes from successive rotations)
        double steerPrevRotMs = -1e9;
        sync::VisualMask visReport;   // controller: what the car showed at the last read (visReadMs), sent in VehicleUpdate
        sync::VisualMask sentVis;
        double visReadMs = -1e9;
        float sentSteer = 0.0f;       // the steering that went out last (a change of it is sent at once)
        // ---- R3 T4 (vehicle damage state, protocol v7; libs/sync/vehicle_damage.h) ----
        sync::VisualGuard visGuard;   // the per-slot baselines of the tyre / window reads (a slot that is "smashed" at the first sample is not a window)
        sync::PedalGuard pedalGuard;  // a stuck handbrake read is not sent
        sync::DamageReporter dmgRep;  // controller: petrol / doors / lamps / dents read off the car, through the baselines
        sync::DamageApplier dmgApply; // copy: the server's block brought to the entity (closed loop for the dents)
        sync::HandbrakeLimiter handLimit;   // copy: a handbrake claim that outlives a moving car is released
        proto::VehicleDamage dmgReport;            // controller: the report (the block)
        sync::DamageSendPolicy dmgSend;            // controller: when the block rides an update (a change, repeats, a 3 s refresh)
        sync::DamageRaw dmgRaw;       // the last read of the entity (controller or copy), for the 10 s line
        double dmgReadMs = -1e9;
        float lastBodyHp = -1.0f;
        double bodyFellMs = -1e9;     // when the controller's body health last fell (corroborates damage)
        int appliedDmgGuard = -1;     // -1 unknown, 1 = copy guards on (cannot damage itself), 0 = a controlled car's defaults
        int handEff = -1;             // the handbrake state the limiter last applied to this copy (-1 = nothing yet: the next check applies it)
        bool dmgHullKnown = false;
        sync::DentHull dmgHull;
        // ---- W9 (script tick) ----
        sync::PresentLead lead;       // item 6: how far past the interpolation time this copy is drawn (near the local player)
        double leadMs = 0;            // ... the lead of this frame (for the 10 s line)
        int vehClass = -2;            // GET_VEHICLE_CLASS (-2 = not read yet, -1 = not callable); 8 / 13 = a bike
        sync::RollTracker roll;       // item 7: this copy's roll error against the sender's, per 10 s
        uint32_t rollSnapsSeen = 0, rollBlendsSeen = 0, rollGracesSeen = 0;   // drive stats at the last bike line
        sync::EngineReassert engineAssert;   // item 4: the engine is re-asserted while the local ped rides in this copy
        bool lastContactFlag = false; // item 6: HAS_ENTITY_COLLIDED_WITH_ANYTHING at the previous frame (a rising edge opens a grace)
        sync::ContactWindow contact;  // Sync B: this copy's contact diagnostics of the last 10 s (the collision flag's duty, kicks, distances)
        uint32_t contactKickMutesSeen = 0;   // drive stats at the last collision line
        float contactLastPosX = 0, contactLastPosY = 0, contactLastPosZ = 0;   // where the copy was this frame (the pair pass)
        bool contactPosValid = false;
        // ---- T4 (vehicle collisions, libs/sync/vehicle_impact.h; script tick) ----
        sync::impact::ImpactTracker impactTracker;   // a car we control: the velocity jumps the game's physics gave it (a contact with a copy)
        uint32_t lastImpactSeq = 0;                  // the newest server VehicleImpact applied / judged for this car
        double divCheckAtMs = 0;                     // a copy whose response we adopted: when to compare it with its controller's stream (0 = none)
        uint32_t proxUpgradesSeen = 0;               // R3 T5: drive stats at the last frame (proximity graces that a real contact upgraded)
        // ---- 0.4.19 vehicle signals (libs/sync/vehicle_signals.h; a copy somebody else controls, script tick) ----
        sync::HornSync horn;                         // the controller's horn on this copy: a rise, renewals, the release (veh_horn_sync)
        sync::LightsKeeper lightsKeep;               // the headlights / high beam kept against the GET_VEHICLE_LIGHTS_STATE read-back (veh_lights_keep)
        double sigStateMs = -1e9;                    // when the newest state was applied here (a horn without fresh states is let go)
        // ---- 0.4.22 doors (libs/sync/vehicle_lock.h; script tick) ----
        sync::LockKeeper lockKeep;                   // the door lock kept against the GET_VEHICLE_DOOR_LOCK_STATUS read-back (veh_locks)
        // ---- 0.4.22 vehicle controls (libs/sync/vehicle_controls.h; script tick) ----
        uint8_t ctlInd = 0;                          // the indicator bits (kVeh2IndicatorL / R) a controller sends for this car: our own state machine's
    };
    std::map<uint16_t, NetVeh> g_vehs;            // script tick only (no lock)
    // 0.4.22 vehicle controls: Left / Right Arrow indicators, Down Arrow hazards, Y engine, B seatbelt (H is GTA's own headlight control). The machine
    // (key -> state, auto-cancel, engine guard, belt, fuel_use) is libs/sync/vehicle_controls.h; TickVehicleControls (below) feeds it and calls
    // the natives it names. Switches (HKCU\Software\GAGE-MP, read at every Welcome): veh_keys (default on; 0 = nothing read, issued or drawn),
    // veh_indicator_autocancel (default on), veh_indicator_sync (default on; 0 = indicator bits neither sent nor applied, the 0.4.21 wire),
    // fuel_use (default OFF; 1 = an empty tank cuts the engine of the car we drive and it will not start).
    sync::VehControls g_vc;                       // script tick
    std::atomic<bool> g_vcKeys{true}, g_vcAutoCancel{true}, g_vcIndSync{true}, g_vcFuelUse{false};
    std::atomic<bool> g_vcReset{false};           // set by the Welcome (join worker): the script tick resets g_vc
    sync::FuelHud g_fuelHud;                     // script tick: the gauge on the speedometer plate (set by TickVehicleControls, pushed by DrawHud)
    struct VcExtraStats
    {
        uint32_t copyIndApplied = 0, copyIndFailed = 0;     // SET_VEHICLE_INDICATOR_LIGHTS calls on copies (from the controller's bits)
        uint32_t copyEngineApplied = 0;                      // SET_VEHICLE_ENGINE_ON calls on copies from the controller's engine bit
        uint32_t indCalls = 0, indFailed = 0, engineCalls = 0, engineFailed = 0, beltCalls = 0, beltFailed = 0;   // our own car's natives
        uint32_t ticksInVeh = 0, ticksBlocked = 0, beltReads = 0, beltReadFail = 0, beltFlagTrue = 0, lightsLow = 0, lightsHigh = 0, engineStopped = 0;
        uint32_t keyLines = 0;                               // per-key log lines written in this session (capped)
        uint32_t lightsRaw[4] = {};                          // 0.4.30: raw GET_VEHICLE_LIGHTS_STATE (lightsOn | highbeamsOn << 1): 0/0, 1/0, 0/1, 1/1
        uint32_t headlightPresses = 0;                       // 0.4.30: H (INPUT_VEH_HEADLIGHT 74), IS_DISABLED_CONTROL_JUST_PRESSED
    };
    VcExtraStats g_vcx;
    double g_vcReportMs = 0, g_vcBeltReadMs = -1e9;
    bool g_vcFirstReport = true;
    sync::VehControlsConfig VehControlsConfigNow()
    {
        sync::VehControlsConfig c;
        c.keys = g_vcKeys.load(std::memory_order_relaxed);
        c.autoCancel = g_vcAutoCancel.load(std::memory_order_relaxed);
        c.fuelUse = g_vcFuelUse.load(std::memory_order_relaxed);
        return c;
    }

    // ---- 0.4.27 indicator fallback (libs/sync/indicator_fallback.h; script tick) ----
    // veh_ind_fallback in HKCU\Software\GAGE-MP (read at every Welcome): unset / 1 = pack cars get our own blinking amber light at each lit corner,
    // 0 = off, 2 = every car. Per model (cached by hash): the four indicator bones as the GAME resolves them on an entity of that model, the
    // model's box, whether a pack carries it, and one log line the first time an indicator is applied to a car of that model.
    std::atomic<int> g_indFallbackMode{(int)sync::IndFallbackMode::PackCars};
    struct IndModelInfo
    {
        uint32_t model = 0;
        int bone[sync::kIndCorners] = {-1, -1, -1, -1};
        bool dimsCallable = false, dimsOk = false;
        sync::V3 mn, mx;
        std::string pack;                    // empty = not a pack model (a stock car)
        bool logged = false;
    };
    std::vector<IndModelInfo> g_indModels;   // script tick; grows by one per model seen with an indicator (a few dozen at most)
    sync::IndPhaseTable g_indPhase;
    struct IndFallbackStats
    {
        uint32_t frames = 0, carFrames = 0, litFrames = 0, lights = 0, lightFailed = 0, fromBone = 0, fromDims = 0, noCorner = 0, posFailed = 0;
        uint32_t skippedStock = 0, skippedFar = 0;
    };
    IndFallbackStats g_indx;
    double g_indReportMs = 0;
    // The on-foot lock flash (the vehicle state row): set by HandleLockResult on an Ok, shown by TickVehicleControls while we are not seated.
    double g_lockFlashUntilMs = -1e18;
    bool g_lockFlashLocked = false;

    IndModelInfo& IndInfoFor(int32_t veh, uint32_t model)
    {
        for (IndModelInfo& m : g_indModels)
            if (m.model == model)
                return m;
        IndModelInfo m;
        m.model = model;
        for (int i = 0; i < sync::kIndCorners; ++i)
            m.bone[i] = g_world->GetEntityBoneIndexByName(veh, sync::kIndBoneNames[i]);
        float a[3] = {}, b[3] = {};
        m.dimsCallable = g_world->GetModelDimensions(model, a, b);
        m.mn = sync::V3{a[0], a[1], a[2]};
        m.mx = sync::V3{b[0], b[1], b[2]};
        m.dimsOk = m.dimsCallable && sync::IndDimsOk(m.mn, m.mx);
        if (const char* p = game::content::PackOfModel(model))
            m.pack = p;
        g_indModels.push_back(m);
        return g_indModels.back();
    }

    // Once per model, the first time SET_VEHICLE_INDICATOR_LIGHTS is applied to a car of it (ours or a copy): the bones as the game resolves them,
    // the box, the calls' results and what the fallback does with that model. The line that proves the live picture of a custom car's indicators.
    void IndNoteCalls(int32_t veh, const char* who, uint8_t ind, int callsOk, int callsFailed)
    {
        if (!veh)
            return;
        const uint32_t model = g_world->GetEntityModel(veh);
        if (!model)
            return;
        IndModelInfo& m = IndInfoFor(veh, model);
        if (m.logged)
            return;
        m.logged = true;
        const sync::IndFallbackMode mode = (sync::IndFallbackMode)g_indFallbackMode.load(std::memory_order_relaxed);
        const bool use = sync::UseIndFallback(mode, !m.pack.empty());
        int bones = 0;
        for (const int b : m.bone)
            bones += b >= 0 ? 1 : 0;
        GLOGI("engine: indicators: model 0x%08X (%s%s%s) first indicator call on %s vehicle %d (state %s): SET_VEHICLE_INDICATOR_LIGHTS %d ok / %d failed; "
              "GET_ENTITY_BONE_INDEX_BY_NAME indicator_lf=%d indicator_lr=%d indicator_rf=%d indicator_rr=%d (%d of 4 present, -1 = the game has no such bone); "
              "GET_MODEL_DIMENSIONS %s min (%.2f %.2f %.2f) max (%.2f %.2f %.2f); fallback light (veh_ind_fallback=%s): %s",
              (unsigned)model, m.pack.empty() ? "stock" : "pack '", m.pack.c_str(), m.pack.empty() ? "" : "'", who, veh,
              ind == sync::kVcIndBoth ? "hazards" : (ind == proto::kVeh2IndicatorL ? "left" : (ind == proto::kVeh2IndicatorR ? "right" : "off")),
              callsOk, callsFailed, m.bone[0], m.bone[1], m.bone[2], m.bone[3], bones,
              m.dimsCallable ? (m.dimsOk ? "ok" : "REFUSED (not a car-sized box)") : "not callable", (double)m.mn.x, (double)m.mn.y, (double)m.mn.z,
              (double)m.mx.x, (double)m.mx.y, (double)m.mx.z, sync::IndFallbackModeName(mode),
              use ? (bones == 4 ? "ON, at the 4 bones" : (m.dimsOk ? "ON, at the bones the game has and the box corners for the rest"
                                                                     : "ON, at the bones the game has only (no usable box)"))
                  : (mode == sync::IndFallbackMode::Off ? "off (switched off)" : "off (a stock car: the game's own indicators)"));
    }
    // The vehicles' timeline and render time are the connection's NetClock (g_sync.Clock()): snapshots are stamped
    // tick * tickMs - ageMs on its timeline and drawn at its RenderT(now) minus the car's class extra (VehNet::extra).
    // Until the W7 change of the sync plan they had a clock of their own (a decaying maximum offset, a fixed 120 ms).
    constexpr size_t kMaxLocalVehicles = 64;      // the server's maxVehicles default
    // Seating a remote occupant (TickVehicles): the retry gap for cars within this distance of us (m, by
    // the server position) and the car we sit in, and for the rest.
    constexpr float  kSeatNearM = 15.0f;
    constexpr double kSeatRetryNearMs = 100.0, kSeatRetryFarMs = 500.0;
    std::map<uint16_t, double> g_seatNoPedLogMs;  // player id -> NowMs of the last "seat target has no ped" line (script tick)
    // veh_angvel=1 trusts the sender's GET_ENTITY_ROTATION_VELOCITY as a world-frame angular velocity;
    // default 0 derives it from successive rotations (the frame of that native is unverified,
    // vehicle_drive.h RISK). veh_angframe=body applies the controller's body-frame output instead.
    std::atomic<bool> g_vehUseAngVel{false}, g_vehAngBody{false};
    // 0.4.22 cars shake (libs/sync/vehicle_shake.h): GI's live log proved that SET_ENTITY_ANGULAR_VELOCITY takes its vector in the car's BODY
    // frame while GET_ENTITY_ROTATION_VELOCITY returns the WORLD frame; the hyper assists' anti-roll and every remote copy's angular command
    // used to hand the setter a world vector, which turns a car's pitch / roll rates by its heading every tick. veh_shake_fix=0 in
    // HKCU\Software\GAGE-MP restores those writes (default on); veh_angframe=world keeps the world frame with the fix on. The judge follows the
    // read-backs (it flips frames if the setter turns out to take the other one). All of it is the script tick's; the switches are atomics.
    std::atomic<bool> g_shakeFix{true}, g_shakeWorldPinned{false};
    sync::shake::AngFrameJudge g_angJudge;
    sync::shake::LocalMeter g_localShake;
    constexpr uint32_t kShakeAngCheckEvery = 30;   // a copy's angular command is read back every 30th write
    struct ShakeStats                              // the counters of the 10 s "engine: vehicle shake (0.4.22)" lines
    {
        uint32_t assistVelWrites = 0, assistAngWrites = 0, assistAngBody = 0, assistAngWorld = 0;
        double dvSum = 0, dvMax = 0, dwSum = 0, dwMax = 0;        // what the hyper assist changed per write: m/s, rad/s
        uint32_t assistMask = 0;                                   // the assists in force on the car we drive (0 = none / not a tuned car)
        float power = 0;                                           // the power multiplier written every tick (0 = none)
        uint32_t copyAngWrites = 0, copyAngBody = 0, copyAngWorld = 0, impactAngWrites = 0;
        int32_t localVeh = 0;                                      // the car the shake meter is following and its model
        uint32_t localModel = 0;
    };
    ShakeStats g_shk;
    // R3 T4: veh_damage_sync=0 in HKCU\Software\GAGE-MP switches the vehicle damage state off (no damage block sent or applied, copies keep
    // their own local damage rules: the v6 behaviour); veh_visual_guard=0 switches the sanity layer off (tyre / window / handbrake reads reach the
    // wire as in v6). Both default on and are re-read at every Welcome.
    std::atomic<bool> g_vehDamageSync{true}, g_vehVisualGuard{true};
    // veh_driveby=aim shows a seated remote player's raised gun with the plain aim task (never fires by
    // itself) instead of TASK_DRIVE_BY (which may fire real bullets on its own: INFERRED, unchecked live).
    std::atomic<bool> g_vehDriveByAimOnly{false};
    // peer_hits=0 keeps remote peds invincible (no local hit detection); default 1.
    std::atomic<bool> g_peerHits{true};
    // T4 (vehicle collisions): impact_sync=0 in HKCU\Software\GAGE-MP keeps the contact grace (W9 / Sync B) as the only contact handling and
    // ignores the server's VehicleImpact messages. Default on; re-read at every Welcome.
    std::atomic<bool> g_impactSync{true};
    // R3 T5 (vehicle position, docs/sync-specs/vehicle-position.md). veh_near_clock=0 draws near copies as round 2 did (the lead = the shared delay + the class
    // extra, 30 / 45 m, 250 ms cap, no link lag); veh_prox_contact=0 switches the proximity contact path off. Default on; re-read at every Welcome.
    std::atomic<bool> g_nearClock{true}, g_proxContact{true};
    // R4 T2 (docs/sync-specs/vehicle-collision-authority.md). veh_contact_gate=0: a copy's flag / speed loss / kick opens the contact grace whatever it touched (round 3);
    // default on = only with a vehicle we control within kContactNearM. veh_accel_extrap=0: constant-velocity extrapolation of the copies (round 3). Re-read at every Welcome.
    std::atomic<bool> g_contactGate{true}, g_accelExtrap{true};
    std::atomic<bool> g_omegaGuard{true};   // veh_omega_guard=0 (0.4.30): derive a copy's omega across sources / from a standing pair again (0.4.29)
    // 0.4.30 veh_ride_free (default on): a copy's own suspension keeps its ride height and pitch / roll within sync::kRideTolM / kTiltTolDeg of the
    // owner's (libs/sync/vehicle_drive.h, VehicleDriveConfig::rideTolM); 0 = the full 3D correction of 0.4.29. Re-read at every Welcome.
    std::atomic<bool> g_rideFree{true};
    std::atomic<bool> g_hyperYaw0430{true};   // hyper_yaw=0: the 0.4.29 yaw cap (30 / 18.4 m/s^2 from 20 m/s), see sync::RestoreYawCap0429
    // 0.4.17 passenger ride (GI: "desync is crazy when sitting on the passenger seat"). veh_ride_gate=0: the car we ride in as a passenger counts as a
    // vehicle we control for the R4 T2 contact gate (0.4.15: its distance to itself is 0 m, so every kerb / brake jolt of the copy we sit in opened the
    // 400 ms soft-correction grace and the own-physics adoption: rubber-banding every ~1.2 s). veh_ride_lead=0: the car we ride in gets the near-car
    // lead (0.4.15: up to 450 ms of extrapolation ahead of the driver's last report, with the camera inside it). Default on = both fixes. Re-read at Welcome.
    std::atomic<bool> g_rideGate{true}, g_rideLead{true};
    // 0.4.22 doors and seats (GI: "locking / unlocking doors, and being able to sit in the back seats if the car has 4 doors"; rules in
    // libs/sync/vehicle_lock.h, tests in tests/doors_seats_tests.h and tests/doors_seats_net_tests.h). No protocol change: the lock is
    // VehicleState.flags kVehLocked and VehicleOp::SetLocked, the seats are Enter / EnterIntent with a wire seat 0..7.
    //   veh_rear_seats=0  F and G see only the driver's seat and the front passenger's (wire seats 0 and 1), as a two-door picks; default on
    //   veh_locks=0       no L key and no lock read-back on the copies (the flag the server sends is still applied once per change); default on
    // Both re-read at every Welcome. DoorsSeatsStats is the 10 s line "engine: doors and seats (0.4.22)" (script tick only).
    std::atomic<bool> g_vehRearSeats{true}, g_vehLocks{true};
    struct DoorsSeatsStats
    {
        uint32_t pressF = 0, pressG = 0;                         // entry presses we took (a GTA-started entry we stopped counts as F)
        uint32_t pickSeat[4] = {};                               // the seat of each pick that began an entry, by sync::SeatClass
        uint32_t refusedLocked = 0, refusedFull = 0, refusedOwned = 0, refusedFar = 0;   // picks that found no seat, by reason
        uint32_t freeYes = 0, freeNo = 0, freeUnknown = 0;       // IS_VEHICLE_SEAT_FREE(seat, entering counts) at a pick the server's list called free
        uint32_t seatMatch = 0, seatDiffers = 0, seatNotSeated = 0;   // the entry's read-back: GET_PED_IN_VEHICLE_SEAT against the seat picked
        uint32_t lockPress = 0, lockHeld = 0, lockSent = 0, lockNotSent = 0;   // the L key (only counted while a networked car is in reach)
        uint32_t lockOk = 0, lockOkLocked = 0, lockOkUnlocked = 0, lockRefused = 0, lockRefusedBy[16] = {};   // the server's answers
        uint32_t soundOk = 0, soundFail = 0;
        sync::LockKeepStats keep;                                // the copies' lock read-back (this window)
    };
    DoorsSeatsStats g_ds;
    struct DoorsSeatsState
    {
        struct Expect { bool active = false; int netId = -1; int seat = -1; double atMs = 0; } expect;   // the seat the last pick named
        struct Pending { bool active = false; int netId = -1; bool wantLocked = false; double sentMs = 0; } lockPending;
        sync::KeyRing keys;                                      // cars we drove or own: only orders the L pick between two cars
        uint32_t reissueLogged = 0;                              // lock re-issue lines written this session (the 10 s line counts the rest)
    };
    DoorsSeatsState g_dsState;
    constexpr int kControlLockKey = 182;                         // INPUT_CELLPHONE_CAMERA_FOCUS_LOCK, the L key (INFERRED: confirm live)
    constexpr int kControlThrowGrenade = 58;                     // the G key's other control on foot (INFERRED): disabled with the entry keys
    constexpr double kLockPendingMs = 1500.0;                    // a second L press while an answer is out is ignored for this long
    void LogDoorsSeatsSwitches();                                // below, with the F / G entry (needs its constants)

    // 0.4.19 spawn reveal (GI: "sometimes I spawn in with a green hue, mid air", mostly on the first launch after the launcher downloaded
    // packs). The car packs mount on the game thread from the entry on, and the 0.4.18 reveal (collision loaded, or 4 s) fell in the
    // middle of it (live 0.4.18: reveal 46.5 s, mounts 45.5-47.8 s). spawn_wait_packs=0: reveal on collision only, as before.
    // spawn_clear_fx=0: leave the timecycle modifier / animated post-fx as the boot left them. Re-read at Welcome.
    std::atomic<bool> g_spawnWaitPacks{true}, g_spawnClearFx{true};
    std::atomic<int>  g_packStatus{0};             // game::content::Status of the last content tick (0 = Idle)
    ULONG g_revealCheckAt = 0;                     // when the +5 s screen-state line after a reveal is due (0 = none)
    constexpr ULONG kPackRevealMaxMs  = 30000;     // never hold the loading screen longer than this for the packs
    constexpr ULONG kPackIdleGraceMs  = 2000;      // Idle this long into the Loading phase = no packs to wait for
    // 0.4.19 vehicle signals (GI: "make everyone hear the horns from the car, see headlights turn on/off"; libs/sync/vehicle_signals.h). Each one
    // default on, "0" = the 0.4.17 behaviour, re-read at every Welcome:
    //   veh_horn_sync=0   a copy's horn is START_VEHICLE_HORN(400 ms, mode 0) for every state that arrives with the horn bit (0.4.17), instead of
    //                     HornSync (a rise at once, a 300 ms hold renewed every 120 ms, the release, a stale car let go, IS_HORN_ACTIVE read-back)
    //   veh_horn_held=0   HornSync calls with mode 0 instead of the hash of "HELDDOWN" (alloc8or: "NORMAL" or "HELDDOWN", or 0)
    //   veh_horn_stop=0   no release call (a 1 ms START_VEHICLE_HORN): the last 300 ms hold runs out by itself
    //   veh_lights_keep=0 the headlights / high beam of a copy are set on a change only (0.4.17), with no GET_VEHICLE_LIGHTS_STATE read-back and
    //                     no re-issue of a state the game dropped
    // veh_lights_beam_lit=0 (0.4.30): lightsOn alone says "lit" (0.4.29); default lightsOn OR highbeamsOn (the game reads a high beam as 0 / 1)
    std::atomic<bool> g_hornSync{true}, g_hornHeld{true}, g_hornStop{true}, g_lightsKeep{true}, g_lightsBeamLit{true},
        g_sirenHorn{true};   // veh_siren_horn=0 (0.4.30): the horn bit from IS_HORN_ACTIVE only (0.4.29); default: siren on + E held also sends it
    constexpr uint32_t kHornModeHeldDown = sync::Joaat("HELDDOWN");
    sync::HornConfig HornConfigNow()
    {
        sync::HornConfig c;
        if (!g_hornStop.load(std::memory_order_relaxed))
            c.stopMs = 0;
        return c;
    }
    sync::LightsConfig LightsConfigNow()
    {
        sync::LightsConfig c;
        c.highIsLit = g_lightsBeamLit.load(std::memory_order_relaxed);
        return c;
    }
    // The 10 s line "engine: vehicle signals (0.4.19)" (ReportVehSignals): this window's counters (script tick).
    struct SigStats
    {
        uint32_t hornStates = 0;                 // copies: states applied with the horn bit (0.4.17 made one START_VEHICLE_HORN of each)
        uint32_t oldHornCalls = 0;               // veh_horn_sync=0: those calls
        uint32_t sentUpdates = 0, sentHorn = 0, sentHeadlights = 0, sentHighBeam = 0;   // as controller: VehicleUpdates, and those with each bit
        uint32_t sentSiren = 0, sentSirenHorn = 0;     // 0.4.30: with the siren on, and horn bits from E held with the siren (IS_HORN_ACTIVE false)
        uint32_t lightsChanged = 0;              // copies: SET_VEHICLE_LIGHTS issued on a change (ApplyVehicleFields)
        uint32_t lightsReissueCalls = 0, lightsReissueFail = 0;   // the keeper's SET_VEHICLE_LIGHTS / SET_VEHICLE_FULLBEAM calls (issued, could not be called)
        sync::HornStats horn;                    // every copy's HornSync counters of this window (drained each tick)
        sync::LightsStats lights;                // every copy's LightsKeeper counters of this window
    };
    SigStats g_sig;
    // 0.4.20 map clean-up (the sweep is beside EnterServer). map_clear_blips=0: the 0.4.18 blips (ClearSpBlips at the entry and every 5 s only while stock
    // scripts run, REMOVE_BLIP). map_reveal=0: the fog of war stays as the story save has it. Unset / 1 = on; re-read at every Welcome.
    std::atomic<bool> g_mapClearBlips{true}, g_mapReveal{true};
    void MapReadSwitches()
    {
        g_mapClearBlips.store(reg::Get(L"map_clear_blips") != L"0");
        g_mapReveal.store(reg::Get(L"map_reveal") != L"0");
        GLOGI("engine: map (0.4.20): map_clear_blips=%s map_reveal=%s",
              g_mapClearBlips.load() ? "on (every blip but the player arrow, north, a waypoint and our own hidden every 3 s after the entry; the stale waypoint cleared once)"
                                     : "OFF (0.4.18: ClearSpBlips at the entry)",
              g_mapReveal.load() ? "on (SET_MINIMAP_HIDE_FOW(true) every 3 s after the entry)" : "OFF (the save's fog of war)");
    }
    // 0.4.22 minimap (GI: "add a minimap"; libs/sync/minimap.h has the rules and the evidence). minimap: unset / anything but "0" = GTA's own radar shown
    // after the entry (bottom-left, on foot and in a vehicle; hidden while the pause menu is up), "0" = hidden as before. minimap_bars: unset / 1 = GTA's
    // health / armour bars hidden (SETUP_HEALTH_ARMOUR(3) every tick + one big-radar flip per entry), 2 = the method call without the flip, 0 = the bars as
    // the game draws them. Both re-read at every Welcome; the bars' mode reaches the gate on the script tick (g_barsReset), never from this thread.
    std::atomic<bool> g_minimap{true};
    std::atomic<int>  g_barsMode{(int)sync::BarsMode::SetupRefresh};
    std::atomic<bool> g_barsReset{true};
    sync::BarsGate g_barsGate;          // script tick only
    // 0.4.25 minimap_scale (libs/sync/minimap_size.h, apps/engine/minimap_size.cpp): the radar's three components shrunk in the game's own table. Unset = 0.80,
    // 1 / 1.0 = untouched, held to 0.5..1.0. Read at the first minimap tick and again at every Welcome; the value reaches minimapsize on the script tick.
    std::atomic<float> g_miniScaleReq{sync::kMinimapScaleDefault};
    std::atomic<int>   g_miniScaleSrc{(int)sync::ScaleSource::Unset};
    std::atomic<bool>  g_miniScaleDirty{false};
    bool               g_miniScaleInit = false;       // script tick only
    float              g_miniAppliedScale = 1.0f;     // what the game's table holds now (read back); the overlay's radar rectangle and the clearance check use it
    std::atomic<bool>  g_hudRing{true};               // 0.4.31 hud_ring (MinimapReadSwitches)
    std::atomic<bool>  g_ringMap{true};               // 0.5.2 ring_map: GTA's radar moved into the ring (MinimapReadSwitches)
    std::atomic<bool>  g_ringMapBig{true};            // 0.5.4 ring_map_big: the placement also written to the big radar's slots (MinimapReadSwitches)
    std::atomic<int>   g_ringMapZoom{sync::kRingMapZoomDefault};   // 0.5.4 ring_map_zoom: SET_RADAR_ZOOM level each tick, 0 = the game's own zoom
    std::atomic<bool>  g_radarBigmapOff{true};        // 0.5.8 radar_bigmap_off: SET_BIGMAP_ACTIVE(false) held while the ring map shows (outside our flip)
    std::atomic<bool>  g_ringMapRound{true};          // 0.5.5 ring_map_round: GTA's radar clipped round by our mask, the map grown to the arcs
    std::atomic<bool>  g_ringBlipClip{false};          // 0.5.5 ring_map_blip_clip: off-radar blips clamp to the circle while the map is round
    std::atomic<float> g_ringFitX{sync::ringround::kBoxFitX}, g_ringFitY{sync::ringround::kBoxFitY};   // 0.5.5 ring_map_round_fit
    std::atomic<bool>  g_overlayCombatBlock{true};    // 0.5.4 overlay_combat_block (sync/overlay_input.h)
    bool               g_overlayCombatLogged = false; // script tick only
    void MinimapScaleRead()
    {
        const sync::ParsedScale p = sync::ParseMinimapScale(reg::Get(L"minimap_scale"));
        g_miniScaleReq.store(p.scale);
        g_miniScaleSrc.store((int)p.src);
        g_miniScaleDirty.store(true);
    }
    void MinimapReadSwitches()
    {
        MinimapScaleRead();
        // 0.4.31 hud_ring (default on; 0 = the 0.4.30 vitals plate / strip under the radar): GI's REV1 vitals ring at the bottom-left, with GTA's
        // radar HIDDEN (GI's pick 2026-11-06: the design's place and size, map off, until the radar can be moved into the circle).
        g_hudRing.store(reg::Get(L"hud_ring") != L"0");
        game::Overlay::SetHudRing(g_hudRing.load());
        // 0.6 inv_rev3 (default on; 0 = the 0.5.2 Tab menu only): the Tab menu opens on GI's REV3 inventory, admins get the old menu as ADMIN.
        {
            const bool rev3 = reg::Get(L"inv_rev3") != L"0";
            game::Overlay::SetInventoryScreen(rev3);
            GLOGI("engine: inventory (0.6): inv_rev3=%s", rev3 ? "on" : "OFF (the old Tab menu)");
        }
        // 0.5.2 ring_map (default on; 0 = 0.5.0's ring without a map): GTA's radar is shown again and moved into the ring through the game's own
        // minimap table (minimapsize::ConfigurePlacement), the overlay hides the square's corners. GI 2026-11-06: "do the real round map".
        g_ringMap.store(reg::Get(L"ring_map") != L"0");
        g_minimap.store(g_hudRing.load() ? g_ringMap.load() : reg::Get(L"minimap") != L"0");
        GLOGI("engine: HUD (0.4.31): hud_ring=%s; ring_map (0.5.2)=%s",
              g_hudRing.load() ? "on (the REV1 vitals ring bottom-left; minimap ignored)" : "OFF (the 0.4.30 vitals and GTA's radar per minimap)",
              !g_hudRing.load() ? "n/a (no ring)" : g_ringMap.load() ? "on (GTA's radar moved into the ring, its corners covered; LIVE-UNVERIFIED)"
                                                                   : "OFF (the ring without a map, GTA's radar hidden)");
        // 0.5.4 (GI 2026-11-07: "map is just a square over the hp and sat bar, not a circle inside of it + make it more zoomed out"). Each default
        // on, 0 = the 0.5.3 behaviour. ring_map_big: the 0.5.2 probes show GTA's BIG radar (the expanded one, table slots bigmap / _mask / _blur,
        // never written by 0.5.2) on the safe-zone corner, about 620 px square on 1440p, over the whole ring; the ring placement goes into those
        // slots too, so the radar lands in the ring in either mode. ring_map_zoom: SET_RADAR_ZOOM(level) every tick while the ring map shows.
        g_ringMapBig.store(reg::Get(L"ring_map_big") != L"0");
        {
            const sync::RingZoom z = sync::ParseRingMapZoom(reg::Get(L"ring_map_zoom"));
            g_ringMapZoom.store(z.level);
            GLOGI("engine: ring map round 2 (0.5.4): ring_map_big=%s; ring_map_zoom=%d (%s)%s",
                  g_ringMapBig.load() ? "on (the placement mirrored into the big radar's slots; LIVE-UNVERIFIED)" : "OFF (minimap slots only, as 0.5.3)",
                  z.level, z.level == 0 ? "the game's own zoom (0.5.8 default)" : "SET_RADAR_ZOOM every tick while the ring map shows; LIVE-UNVERIFIED",
                  z.why ? z.why : "");
        }
        // 0.5.8 radar_bigmap_off (default on; 0 = 0.5.7): GI after 0.5.7, "again too zoomed out". The 0.5.7 run bound radarmasklg (the EXPANDED
        // radar's mask) about once a frame and radarmasksm never, the probes showed the same wide view at zoom 600 as at 1100, and the 0.5.2 probes
        // showed the expanded radar's own box on the corner: the radar stays expanded, where SET_RADAR_ZOOM does not set the span. No native reads
        // the mode back, so SET_BIGMAP_ACTIVE(false, false) is HELD every tick while the ring map shows, except during our own big-radar flip.
        g_radarBigmapOff.store(reg::Get(L"radar_bigmap_off") != L"0");
        GLOGI("engine: ring map (0.5.8): radar_bigmap_off=%s", g_radarBigmapOff.load()
                                                                    ? "on (SET_BIGMAP_ACTIVE(false, false) held every tick while the ring map shows, outside "
                                                                      "our flip; LIVE-UNVERIFIED)"
                                                                    : "OFF (0.5.7: the expanded radar is left as the game has it)");
        // 0.5.5 (GI 2026-11-07: "fill out the current design HUD circle to the point where it doesn't go over hp and satiety and there's no gap
        // either"). ring_map_round (default on; 0 = the 0.5.4 map): the radar's own mask texture is swapped for a circle (radar_mask.h) and, only
        // while that swap is confirmed by the bind hook, the map grows to the arcs. ring_map_blip_clip (default OFF for the first live round, one
        // live-unverified change at a time; 1 = FiveM's minimapIsRect flag set to round while the map is round, so blips clamp to the circle
        // instead of the square's edge). ring_map_round_fit = "fx,fy": the radar box fit.
        g_ringMapRound.store(reg::Get(L"ring_map_round") != L"0");
        g_ringBlipClip.store(reg::Get(L"ring_map_blip_clip") == L"1");
        const sync::ringround::BoxFit fit = sync::ringround::ParseBoxFit(reg::Get(L"ring_map_round_fit"));
        g_ringFitX.store(fit.fx);
        g_ringFitY.store(fit.fy);
        if (g_hudRing.load() && g_ringMap.load() && g_ringMapRound.load())
            game::radarmask::Enable(true);
        GLOGI("engine: ring map round mask (0.5.6): ring_map_round=%s; ring_map_blip_clip=%s; ring_map_round_fit=%.3f,%.3f (%s%s)",
              g_ringMapRound.load() ? "on (the radar's mask swapped for a circle, the map grown to the arcs once the swap is confirmed; LIVE-UNVERIFIED)"
                                    : "OFF (the 0.5.4 map: the square's inscribed circle)",
              g_ringBlipClip.load() ? "on (blips clamp to the circle while the map is round; LIVE-UNVERIFIED)" : "off (default; 1 = blips clamp to the circle)",
              (double)fit.fx, (double)fit.fy, fit.overridden ? "override" : "default, from the 0.5.4 radar probes", fit.why ? fit.why : "");
        g_overlayCombatBlock.store(reg::Get(L"overlay_combat_block") != L"0");
        GLOGI("engine: overlay input (0.5.4): overlay_combat_block=%s", g_overlayCombatBlock.load()
                  ? "on (an open panel also holds attack2 / melee / weapon / sniper / vehicle-melee controls above 226; LIVE-UNVERIFIED)"
                  : "OFF (controls 0-226 only, as 0.5.3)");
        const sync::BarsMode bm = sync::ParseBarsMode(reg::Get(L"minimap_bars"));
        g_barsMode.store((int)bm);
        g_barsReset.store(true);
        GLOGI("engine: minimap (0.4.22): minimap=%s minimap_bars=%s",
              g_minimap.load() ? "on (GTA's radar, bottom-left, after the entry; hidden while the pause menu is up; the player arrow and a waypoint show)"
                               : "OFF (hidden as before)",
              bm == sync::BarsMode::Off ? "0 (GTA's health / armour bars as the game draws them)"
              : bm == sync::BarsMode::Setup ? "2 (SETUP_HEALTH_ARMOUR(3) each tick, no big-radar flip)"
                                            : "on (one big-radar flip per entry, then SETUP_HEALTH_ARMOUR(3) each tick; LIVE-UNVERIFIED)");
    }
    constexpr double kContactNearM = 10.0;       // origin to origin: a vehicle we control this close is the likely toucher of a copy
    // The round trip (ms) the server measures to each vehicle controller (Msg::LinkInfo, join worker -> script tick), by player id.
    std::mutex g_linkMtx;
    std::map<uint16_t, uint32_t> g_linkRtt;   // g_linkMtx
    // R3 T5 counters / windows (script tick).
    struct T5Stats
    {
        sync::PositionDiag pos;                  // drawn vs the owner's report now, per speed / distance band
        uint32_t proxOpened = 0, proxUpgraded = 0, proxFrames = 0;   // proximity graces opened / turned into a real contact / copies predicted to overlap
        uint32_t linkInfoIn = 0;                 // LinkInfo messages received (join worker: atomic in effect, read for the log only)
        double lagSum = 0; uint32_t lagN = 0; double lagMax = 0;     // the link lag used (ms) over the driven frames
        uint32_t ownerRttUnknown = 0;            // driven frames whose controller's round trip the server had not told us (ours stood in)
        uint32_t sentFrames[4] = {};             // as controller: updates sent per speed band (the other machine's line is its view of these)
        double sentSpeedSum = 0; uint32_t sentN = 0;
    };
    T5Stats g_t5;
    std::atomic<uint32_t> g_linkInfoIn{0};
    // The server's VehicleImpact messages (join worker -> script tick), newest last, at most kMaxImpactQueue waiting.
    std::mutex g_impactMtx;
    std::vector<proto::VehicleImpact> g_impactQueue;   // g_impactMtx
    constexpr size_t kMaxImpactQueue = 16;
    // T4 counters (script tick except the atomic ones), reported every 10 s with the other vehicle lines.
    struct T4Stats
    {
        uint32_t adopted = 0, adoptRefused = 0, guardRefused = 0;      // copies whose response became their trajectory / contacts left to the grace / stream pushes refused
        uint32_t jumpEpisodes = 0;                                      // velocity jumps (contacts) our controlled cars felt
        uint32_t impactsIn = 0, applied = 0, noLocal = 0, tiny = 0, stale = 0, dup = 0, noVehicle = 0;   // server impacts: received and what became of them
        double appliedDvSum = 0, appliedDvMax = 0;
        uint32_t divN = 0;                                              // copies compared with their controller 500 ms after the adoption
        double divSum = 0, divMax = 0;
        uint32_t dropped = 0;                                           // queue overflow
    };
    T4Stats g_t4;                                                       // script tick (dropped: g_impactMtx)
    // R4 T2 (script tick): what the copies' collision flags touched, the contact gate's verdicts, the divergence check after EVERY contact, and the arbiter's numbers as the
    // controller of the hit car sees them (the authoritative change and the closing speed behind it).
    struct R4Stats
    {
        sync::ContactWatch watch;
        sync::TouchTally tally;
        uint32_t gatedEdges = 0, gatedKick = 0, gatedLoss = 0;         // contact evidence ignored because no vehicle we control was near (flag edges / kicks / speed losses)
        uint32_t impactsAuth = 0;                                      // server impacts for cars we control, received
        double authDvMax = 0, closingMax = 0, depthMax = 0;            // ... the largest authoritative change, closing speed and depth among them
        double ownMinM = 1e9;                                          // the closest a copy came to a vehicle we control (origin to origin) in the window
        uint32_t ownUnder3m = 0;                                       // ... copy-frames closer than 3 m
    };
    R4Stats g_r4;
    constexpr int kPeerHpPool = 1000;             // a remote ped's local health, restored after every hit
    // Sync plan W3 (combat client), re-read at every Welcome:
    //   hit_react = off | bullet | ragdoll | both (default both): bullet = a flinch from a 1-damage point-blank
    //     bullet, ragdoll = a knockdown by SET_PED_TO_RAGDOLL; reactions are cosmetic and never feed a Hit.
    //   peer_muzzle = 0 turns off the muzzle flash on remote shots (default on).
    //   peer_shoot  = a puppet fires a REAL shot through SET_PED_SHOOTS_AT_COORD (the default since the 0.4.3 fixes, W8
    //     item 6: the AI damage modifiers are set to 0 at Welcome, so it hurts nobody here and the clear-line check is
    //     lifted; with the modifiers not applied the shot still needs a line that passes nobody). peer_shoot=script keeps
    //     the old zero-damage script bullet.
    enum HitReactMode : int { kReactOff = 0, kReactBullet = 1, kReactRagdoll = 2, kReactBoth = 3 };
    std::atomic<int> g_hitReact{kReactBoth};
    static_assert(kReactOff == sync::kHitReactOff && kReactBullet == sync::kHitReactBullet &&
                  kReactRagdoll == sync::kHitReactRagdoll && kReactBoth == sync::kHitReactBoth,
                  "the hit_react modes must match sync::ReactionToShow (libs/sync/combat.h)");
    std::atomic<bool> g_peerMuzzle{true}, g_peerShootPed{true};
    // W8 item 6: the join worker (Welcome true, session end false) asks, the script tick (TickAiDamage) does: the
    // game-wide AI weapon and melee damage modifiers are 0 while a session is on and reset when it ends.
    std::atomic<bool> g_aiDamageWanted{false};
    bool g_aiDamageApplied = false;                    // script tick (and the PlayPeerShots it calls)
    // 0.4.4 item 0c: SET_AI_MELEE_WEAPON_DAMAGE_MODIFIER is NOT set by default (ai_melee_mod=1 sets it again). A puppet
    // only PLAYS a swing animation (PlayPeerSwing: TaskPlayAnim, no melee task, no TASK_COMBAT anywhere), so there is no
    // melee blow that could hurt the local player, and the 0.0 modifier zeroed OUR fists on the puppet (the 0.4.3
    // regression: swings counted, no hit ever seen). Registry read at Welcome; applied only after the tick set it.
    std::atomic<bool> g_aiMeleeMod{false};
    bool g_aiMeleeApplied = false;                     // script tick
    // R3 T6: melee_anim = auto | play | melee_task | off (re-read at every Welcome like the other switches); the ladder and the
    // counters are script-tick only. g_meleeConfirm* are written by the join worker: the server's own Damage with kDmgMelee on a
    // player WE hit (adjudicated by the server or claimed by us, the client cannot tell which).
    std::atomic<int> g_meleeAnimWanted{(int)sync::SwingMode::Auto};
    sync::SwingLadder g_swingLadder;
    sync::SwingDiag g_swingDiag, g_swingDiagLast;
    // R4 T3: melee_survey = N (default 1): the first N passes over the survey plan (libs/sync/swing.h SwingSurvey) issue each swing as the next
    // attempt whatever was seen before, then the ladder is pinned to the attempt that PROGRESSED; 0 = off (the R3 ladder alone). melee_follow = 0
    // turns the +100 / +250 / +450 ms progress check off (the 0e first-sighting probe alone). Both re-read at Welcome.
    // 0.4.30: the survey is off by default (melee_survey=N restores it): with the start phase fixed every variant progresses, and the tie-break
    // would pin the first entry (the 480 ms loop = the wind-up only).
    std::atomic<int> g_meleeSurveyWanted{0};
    std::atomic<bool> g_meleeFollowWanted{true};
    // 0.4.30 anim_phase_fix (default on; 0 = 0.4.29): TASK_PLAY_ANIM's 8th argument is the START PHASE; swings and emotes start at 0, not 1.0
    // (their end). stagger_clip (default on; 0 = the W8 ragdoll): a melee stagger is a stumble clip on the feet (sync::ReactionToShow).
    std::atomic<bool> g_animPhaseFix{true}, g_staggerClip{true};
    float AnimPhase() { return sync::AnimStartPhase(g_animPhaseFix.load(std::memory_order_relaxed)); }
    // an emote's 8th argument: the start phase, or with anim_phase_fix=0 what 0.4.29 passed there (the wire's playback rate / 100)
    float EmotePhase(const proto::Anim& a) { return g_animPhaseFix.load(std::memory_order_relaxed) ? AnimPhase() : a.playbackRate / 100.0f; }
    // a non-survey swing's duration: the whole clip, or with anim_phase_fix=0 the 0.4.29 400 ms
    int SwingDurationDefault() { return g_animPhaseFix.load(std::memory_order_relaxed) ? sync::kSwingClipLength : 400; }
    // R5: melee_loop (default on; 0 = the round-4 survey plan, ladder and melee task exactly as they were): the plan and the ladder start with the LOOPED clip (flags 49,
    // ended by us with STOP_ANIM_TASK) and the plain one (flags 0), and the melee-task rung borrows an anchor ped where the puppet has none. Re-read at Welcome.
    // 0.4.30: default OFF (melee_loop=1 restores it): the loop was the only variant that showed because of the start phase, and it is ended at 480 ms.
    std::atomic<bool> g_meleeLoopWanted{false};
    sync::SwingSurvey g_swingSurvey;
    uint32_t g_swingNo = 0, g_ownSwingNo = 0;
    int g_swingDurationMs = sync::kSwingClipLength;    // the duration of a non-survey swing (0.4.30: the whole clip; was 400 ms = the wind-up)
    bool g_swingTestForce = false;                     // a /swing test: the overlay names the attempt
    double g_meleeTaskModUntilMs = 0;                  // the AI melee modifier is held at 0 until then (the melee-task rung), 0 = not held
    bool g_meleeTaskModSet = false;
    std::atomic<uint32_t> g_meleeConfirmCount{0}, g_meleeConfirmDamage{0}, g_meleeConfirmKills{0};
    // 0c, written by TickLocalMelee every script frame, read by ProbePeerHits (same thread): the melee state of OUR ped.
    bool   g_meleeHeld = false;                        // a melee-class weapon in hand, on foot, alive
    bool   g_meleeActionNow = false;                   // IS_PED_PERFORMING_MELEE_ACTION(local ped)
    double g_lastSwingMs = -1e9;                       // when TickLocalMelee last counted a swing
    // W8 item 8: the last PedState the script tick read, for the render-thread fallback (GameWork !inScript).
    std::mutex g_realPedMtx;
    proto::PedState g_realPed;                         // g_realPedMtx
    bool g_haveRealPed = false;                        // g_realPedMtx
    std::atomic<uint32_t> g_fallbackStates{0};         // fallback states sent (render thread)

    // Results and combat events from the join worker, drained by the script tick. Everything that must
    // not be lost has its own place: Damage about us is folded into g_inbox (sums add up, the newest
    // health wins), RespawnOk is a latest-wins slot, and only the kill feed and the vehicle answers use
    // the bounded queue, where an answer evicts a feed line when it is full and a feed line is refused.
    struct NetEvent
    {
        enum Kind { Killed, VehResult, SeatNote } kind = Killed;
        proto::Killed killed;
        proto::VehicleResult veh;
        proto::SeatNotice seat;       // T1 seats (wire v6)
    };
    std::mutex g_evMtx;
    constexpr size_t kMaxEvents = 256;
    sync::DropQueue<NetEvent> g_evQueue(kMaxEvents);   // g_evMtx
    sync::VitalsInbox g_inbox;                         // g_evMtx
    bool g_respawnSlotFull = false;                    // g_evMtx
    proto::RespawnOk g_respawnSlot;                    // g_evMtx
    std::atomic<uint32_t> g_hitDamageConfirmed{0};     // Damage the server credited to our Hits (join worker)
    std::atomic<uint32_t> g_hitConfirmCount{0};        // ... and how many Damage messages that was (0.4.5 melee audit)
    std::atomic<bool> g_v4Reset{true};            // a session ended or began: the tick resets the units below
    std::atomic<bool> g_v4ResetEnded{false};      // R3 T1: the pending g_v4Reset is a session END (no probe, no health written to the offline ped)
    std::atomic<uint8_t> g_healthWish{0};         // R3 T1: HKCU health_mode: 0 auto, 1 direct, 2 scaled (read at Welcome)

    // Script tick only.
    sync::LocalVehicleLink g_link;
    sync::VitalsTracker g_vitals;
    sync::HealthController g_health;              // R3 T1: measures what GTA holds (DIRECT / SCALED), keeps the read-backs
    uint32_t g_healthTick = 0;                    // R3 T1: TickVitals runs once per script tick; numbers the probe's "one frame later"
    sync::RespawnGate g_respawnGate;
    sync::HitBatcher g_hits;
    sync::EmoteArbiter g_localEmote;
    sync::HitReactArbiter g_localReact;           // sync plan W3: when our own ped last reacted to a hit (Decide's inputs)
    sync::MeleeSwingGate g_meleeGate;             // sync plan W3: counts our melee swings into g_shotSeq
    // Animation dictionaries are reference-counted across every arbiter (ours and each peer's): a
    // dictionary is streamed out (RemoveAnimDict) only when the last emote using it has ended, been
    // given up or lost its ped. Script tick only.
    sync::AnimDictRefs g_animDicts;
    sync::AnimDiag g_animDiag, g_animDiagLast;   // 0e: the 10 s per-kind animation line (script tick)
    sync::FirstUse g_animFirstUse;               // 0e: one log line per animation the first time it is used
    sync::EmoteDictHold g_localEmoteDict;
    proto::Anim g_localAnim;
    std::string g_localScenario;
    bool g_haveRespawnOk = false;
    proto::RespawnOk g_respawnOk;
    int g_vitalsPrevTotal = -1;                   // health above 100 + armour at the last vitals tick (-1 = none)
    bool g_serverDeadNoticed = false;             // the "server still has you dead" chat line was shown for this death
    uint8_t g_shotSeq = 0;                        // our shots, only the low 4 bits travel
    double g_lastShotMs = -1e9;
    int32_t g_lastMarkedVeh = 0;                  // the last vehicle we MarkVehicleOwned before reading seats
    int g_localSeat = -1;
    double g_localSeatAtMs = 0;
    // Sync plan W2 A1: PCF 184 (PreventAutoShuffleToDriversSeat) was last set on this vehicle at this time.
    int32_t g_noShuffleVeh = 0;
    double g_noShuffleAtMs = -1e9;
    // Sync plan W2 A4-A6: the F / G entry walk (TickVehicleEntry). Script tick only.
    struct EntryWalk
    {
        bool active = false;
        int netId = -1;            // the car we walk to
        int seat = -1;             // the wire seat picked last frame (re-picked every frame)
        bool wantDriver = false;  // F (true) or G (false): the intent kept while the seat is re-picked
        double startMs = 0;        // when the walk began
        double taskMs = -1e9;      // when TASK_GO_STRAIGHT_TO_COORD was last issued
        bool tasked = false;       // a go-to task was issued and is not cleared yet
        sync::EntryVec target;     // its last target
    };
    EntryWalk g_entryWalk;
    // T1 seats (wire v6, libs/sync/seat_intent.h): the server's enter / exit intents. g_seats holds the reservation book and one replay per
    // remote player; g_localEnter is OUR entry (intent -> the game's TASK_ENTER_VEHICLE -> bounded fallback warp). HKCU seat_intent=0
    // (read at Welcome) restores the pre-v6 behaviour: walk + teleport-in locally, follow + seat warp on the other screens.
    // Script tick only (the net sinks queue their events).
    sync::SeatClientModel g_seats;
    sync::LocalEnterFlow g_localEnter;
    std::atomic<bool> g_seatIntentOn{true};
    struct T1Stats
    {
        uint32_t intentsSent = 0, intentsOk = 0, intentsRefused = 0, cancelsSent = 0, noticesIn = 0, noticesAboutUs = 0, exitsReplayed = 0, exitsNoPed = 0;
        uint32_t refusedBy[16] = {};                 // by VehicleResultCode
        uint32_t localIssueFailed = 0;
        uint32_t evictLog = 0;
    };
    T1Stats g_t1;
    // W9 item 3: INPUT_ENTER (23) stays disabled from our handled F / G press until 500 ms after the ped sat down, and a
    // quick exit after an entry is counted (the 0.4.2 log: enter, exit 1.4 s later, enter, exit). Script tick only.
    sync::EntryPressGuard g_entryGuard;
    int g_exitNetId = -1;          // the networked car the link last sent Exit for, and when (PedState move 6)
    double g_exitAtMs = -1e9;
    // 0.4.14 diagnostic (the puppet's vehicle exit "drops to the ground"): when OUR ped's CTaskExitVehicle (GET_IS_TASK_ACTIVE 2) began
    // while GET_VEHICLE_PED_IS_IN still named the car. The Exit line says how long after it the link sent Exit: a delay about the
    // exit animation's length means the others start their climb-out that late, behind a pose already on the ground. -1 = not seen.
    double g_exitTaskAtMs = -1.0;
    constexpr int kTaskExitVehicle = 2;                // eTaskTypeIndex TASK_EXIT_VEHICLE (alloc8or GET_IS_TASK_ACTIVE doc)
    double g_entryChatMs = -1e9;   // the last F / G refusal line (rate limit)
    // Per vehicle model: the seat bones' indices (-1 = not found) and the seat count, read once.
    struct SeatInfo
    {
        int bone[sync::kEntrySeats] = { -1, -1, -1, -1, -1, -1, -1, -1 };
        int maxSeats = 2;          // wire seats (passengers + the driver); 2 until GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS reads
        bool read = false;
    };
    std::map<uint32_t, SeatInfo> g_seatInfo;
    // The sender's steering estimate: wheelbase per model (0 = not measured yet / no wheel bones), and the
    // last value (held below 1 m/s) for the vehicle it belongs to.
    std::map<uint32_t, float> g_wheelbase;

    // 10 s summaries (script tick, except the atomics the join worker bumps).
    struct V4Stats
    {
        uint32_t vehSpawned = 0, vehDeleted = 0, vehSpawnFail = 0, vehUpdates = 0, vehCmds = 0, vehOk = 0,
                 vehRefused = 0, seatWarps = 0, seatRewarps = 0, unseats = 0, ejects = 0, vehSnaps = 0, reverts = 0, entryPicks = 0,
                 entryRefused = 0;
        uint32_t shots = 0, peerShots = 0, hitsSent = 0, hitDamage = 0, hitsRefusedLocal = 0, damageIn = 0,
                 damageInTotal = 0, kills = 0, respawnReqs = 0, peerDeaths = 0, peerRecoveries = 0, peerLocalDeaths = 0;
        uint32_t emotesStarted = 0, emoteDictTimeouts = 0, emoteNotPlaying = 0, emoteMissingDict = 0;
        // sync plan W3: melee swings counted / peer swings shown, our own hit reactions (flinch, knockdown) and
        // the ones shown on puppets, real peer shots, muzzle flashes started
        uint32_t meleeSwings = 0, peerSwings = 0, reactFlinch = 0, reactKnock = 0, puppetFlinch = 0, puppetKnock = 0,
                 peerShotsReal = 0, muzzleFlashes = 0;
        // W8: melee stagger (ours / on puppets), TASK_CLIMB issued and the placement fallbacks, dead-puppet melee reports
        // capped, puppets that died of a takedown, the local health losses a puppet ped caused, AI modifier sets
        uint32_t reactStagger = 0, puppetStagger = 0, climbTasks = 0, climbFallbacks = 0, deadCapped = 0, takedownKills = 0,
                 puppetFireLoss = 0, aiModSets = 0;
        // 0.4.30 stagger_clip: a stagger NOT shown because the clip's dictionary does not exist / was still streaming / TASK_PLAY_ANIM failed
        uint32_t staggerNoDict = 0, staggerStreaming = 0, staggerFailed = 0;
        // 0c: melee Hits reported by the damage event / by the pool drop; damage events read in a swing window; swing
        // steps seen on puppets and why one was not shown (the ped was owned by something else, the dictionary was
        // still streaming or missing, the anim task failed)
        uint32_t meleeHitsEvent = 0, meleeHitsPool = 0, meleeEventsRead = 0, peerSwingSteps = 0, peerSwingOwned = 0,
                 peerSwingNoDict = 0, peerSwingFailed = 0;
        // 0.4.5 melee audit: swing windows opened on a puppet, of which a STALE damage event stood (cleared, not a hit),
        // swings that opened a window on a puppet within 3 m (a blow could have landed), the damage claimed by each melee
        // detector (event / pool), and the Damage messages the server confirmed on our Hits
        uint32_t meleeWindows = 0, meleeStaleEvents = 0, meleeNearSwings = 0, meleePoolDamage = 0, meleeEventDamage = 0, hitsConfirmed = 0;
        // 0d: TASK_CLIMB windows that really started (the puppet climbs / vaults) versus the placement fallbacks (climbTasks,
        // climbFallbacks above), the puppet put on the sender's pose before a task (nudged / set outright), and the cover
        // windows: task issued, the puppet really in cover, never in cover (fallback), ended with the sender's flag
        uint32_t climbStarted = 0, prePlaceNudged = 0, prePlaceSet = 0, coverIssued = 0, coverStarted = 0, coverFailed = 0,
                 coverEnded = 0;
        // W9: re-warps held back by the limiter, F / G presses we took, quick exits after an entry, passenger engine
        // re-asserts, bail-outs of a moving vehicle (remote / our own forced exit), vehicle impacts shown on puppets,
        // contact graces opened (flag / speed loss), frames the lead drew a car in the present, shots at seated puppets
        // (the free-aim target), at a puppet's vehicle, hits reported on seated puppets
        uint32_t seatRewarpsHeld = 0, entryPresses = 0, quickExits = 0, engineAsserts = 0, bailOuts = 0, localBailOuts = 0,
                 vehImpacts = 0, contactsFlag = 0, contactsDecel = 0, contactsKick = 0, leadFrames = 0, shotsAtSeated = 0, shotsAtPuppetCar = 0,
                 shotsAtFoot = 0, hitsOnSeated = 0, seatedHpDrops = 0;
    };
    V4Stats g_v4;
    // W9 item 4: the engine state of the car the local ped rides in as a passenger (script tick), for the 10 s vehicles line.
    struct PassengerEngineView
    {
        double atMs = -1e9;              // when it was last read (a passenger frame)
        uint16_t id = 0;
        bool serverOn = false, running = false;
        bool rpmLogged = false;          // the one-time line about the missing throttle / RPM native
    };
    PassengerEngineView g_pax;
    // Sync D (libs/sync/vehicle_syncd.h): what the offline audit changed or could not decide, for the 10 s "(D)" lines.
    // Script tick, except the hand-over counters (the join worker bumps them): [0] a hand-over that kept the buffer and the
    // drive controller (between two other controllers), [1] one that cleared / snapped (to or from us, or a new session).
    struct SyncDStats
    {
        uint32_t enterFollowed = 0, enterTasked = 0;                                     // door walks shown by following the pose / by TASK_ENTER_VEHICLE
        uint32_t displacedOtherSeat = 0, displacedOtherVeh = 0, displacedOutside = 0, displacedUnknown = 0;   // a warped puppet found out of its seat
        uint32_t unseatLeave = 0, unseatBail = 0, unseatClear = 0, bailFallback = 0;    // exits by TASK_LEAVE_VEHICLE flag 0 / 4160 / the pop / the flag-16 fallback
        uint32_t carryExits = 0;
    };
    SyncDStats g_syncD;
    std::atomic<uint32_t> g_syncDHandovers[2] = { 0, 0 };
    // puppet_enter=task (HKCU, read at Welcome) restores the W6 door walk (TASK_ENTER_VEHICLE on the puppet); the default
    // follows the sender's pose and warps into the seat when the seat list names the player (sync::EnterWalkMode).
    std::atomic<bool> g_peerEnterTask{false};
    std::atomic<uint32_t> g_vehWorldsIn{0}, g_emotesIn{0};
    // R3 T3 (libs/sync/cover.h): cover_sync=task|pose|off (HKCU, read at Welcome; default task with the verified window and the crouch-pose
    // fallback), the breaker that sends every hold to the pose fallback after consecutive refused tasks, and the SENDER's cover classifier
    // (ReadLocalPedState, script tick) with its 10 s counters.
    std::atomic<int> g_coverSyncMode{(int)sync::CoverSyncMode::Task};
    sync::CoverBreaker g_coverBreaker;
    sync::CoverSense g_coverSense;
    uint32_t g_coverSenderLogged = 0;

    // A single "companion" ped that mirrors the local player at a fixed offset:
    // proves the SpawnPed/EntityPos pipeline end-to-end before any live peer.
    int32_t g_companion = 0;

    // Model for every remote player and the companion: the MP freemode male, the
    // same model we switch the local player to (proven to stream in — see the
    // "freemode character" entry log). A story-only ped hash may never load in this
    // session, which would make SpawnPed fail silently. The protocol carries no
    // per-player model yet.
    const int kCompanionModel = 0x705E61F2; // mp_m_freemode_01

    // The sender (GameWork): when the next state is due (NowMs), and the last sent position for
    // the finite-difference velocity fallback.
    std::atomic<double> g_nextSendMs{0};
    bool   g_haveLastPos = false;
    float  g_lastPos[3] = { 0, 0, 0 };
    double g_lastPosMs = 0;

    // Server entry (model swap + spawn teleport) runs once per connection; a
    // reconnect (kick, server restart) resets it so the player is re-entered.
    enum class Entry { Waiting, Done };
    std::atomic<Entry> g_entry{Entry::Waiting};
    std::atomic<ULONG> g_entryStarted{0};   // first script tick with a session and a ped (GetTickCount); written by the join worker (OnEnded) and the script tick (EnterServer)
    // When EnterServer's readiness gate first passed (GetTickCount64; 0 = not yet). The model-stream
    // cap runs from here, not from the connect, and the loader's give-up clock follows it (LoaderDone).
    std::atomic<ULONGLONG> g_entryReadyAt{0};
    // After the entry (script tick only): when it finished, which one-shot ped logs are written
    // (bit0 = 1 s, bit1 = 10 s) and when the default-ped guard last looked.
    ULONG g_entryDoneAt = 0;
    unsigned g_entryLogged = 0;
    ULONG g_pedGuardAt = 0;

    // The overlay's full-screen loading state (UiFrame, render thread) lasts until the entry is
    // done. Two timers keep it from covering the game for ever: GetTickCount64 when this loading
    // episode was armed, and when the local ped was first seen in it (set on the script tick).
    // OnEnded zeroes both, so a reconnect shows the loader again.
    std::atomic<ULONGLONG> g_loaderArmed{0};
    std::atomic<ULONGLONG> g_loaderPedAt{0};
    constexpr ULONGLONG kLoaderAfterPedMs = 25000;   // player exists but the entry never finished
    constexpr ULONGLONG kLoaderMaxMs      = 240000;  // the game never produced a player

    // Self-driven respawn (0.1.53): the stock respawn_controller leaves a freemode
    // ped on a black screen after death (it re-enables the post-death fade-in only
    // for the three story characters), so the engine runs its own MP-style respawn.
    //   Alive   -> waiting; flips to Dying when the local ped dies.
    //   Dying   -> hold the game's death machine disabled, show the death briefly,
    //              fade to black, then revive the ped at spawn (frozen).
    //   Loading -> wait for the spawn's collision to stream in, then unfreeze and
    //              fade back in.
    // Protocol v4 (connected, entered): the SERVER decides when we come back. Dying holds the death
    // view and asks for a respawn (RespawnGate: not before the server's 5 s, then once a second);
    // RespawnOk moves to Respawning (fade to black, revive at the server's point and values), then
    // Loading as above. No RespawnOk within 30 s: the old local revive at kSpawn, logged as a WARN.
    // Touched only on the script tick (TickLife, EnterServer), so it need not be atomic.
    enum class Life { Alive, Dying, Respawning, Loading };
    Life  g_life = Life::Alive;
    ULONG g_lifePhaseAt = 0;                // when the current phase began (GetTickCount)
    float g_lifeSpawn[4] = { 195.2f, -933.8f, 30.7f, 140.0f }; // where Respawning / Loading put the ped
    constexpr ULONG kDeathHoldMs   = 2500;  // show the death before fading out
    constexpr ULONG kFadeOutMs     = 400;   // death -> black
    constexpr ULONG kDyingGiveUpMs = 9000;  // revive even if the screen never reports faded
    constexpr ULONG kLoadMaxMs     = 4000;  // give collision this long, then fade in anyway
    constexpr ULONG kFadeInMs      = 600;   // black -> world

    // Client-side commands (0.1.54): /heal /kill /tp /car /gun /fix. The chat input
    // path (SendChat) parses the line off the game thread and queues it; the script
    // tick drains and runs it, because the vehicle/weapon natives must run there.
    struct Command {
        enum Kind { Heal, Kill, Tp, Car, Gun, Fix, Emote, Scenario, StopAnim, Swing, Noclip } kind = Heal;
        float x = 0, y = 0, z = 0;    // /tp
        std::string arg;              // /car, /gun model/weapon name; /e dictionary; /scenario name
        std::string arg2;             // /e clip name
        bool loop = false, upper = false; // /e options
        bool approved = false;        // v8: the server answered AdminResult Ok for it (run it, do not ask again)
        bool tpSpawn = false;         // v8: a /spawn (a Tp to kSpawn), asked as AdminOp::Spawn
    };
    std::mutex g_cmdMtx;
    std::vector<Command> g_cmdQueue;
    // v8: admin commands waiting for the server's AdminResult, by AdminCmd seq (g_cmdMtx). Bounded: an answer that never
    // comes (the session ended) is dropped with the oldest when the map is full, and the map is cleared per session.
    std::map<uint16_t, Command> g_adminParked;
    uint16_t g_adminSeq = 0;
    constexpr size_t kMaxAdminParked = 16;

    // One deferred vehicle spawn (/car): the model streams in over a few frames, so
    // the tick requests it and creates the vehicle once it has loaded. Script tick only.
    struct PendingCar { bool active = false; uint32_t model = 0; ULONG since = 0; };
    PendingCar g_pendingCar;

    // Vehicles the local player spawned with /car (handles, oldest first, bounded). Any later
    // ambient/world cleanup must leave these alone, and the vehicle-authority phase starts from
    // them. Script tick only.
    std::vector<int32_t> g_ownedVehicles;
    constexpr size_t kMaxOwnedVehicles = 16;

    // Satiety (0.1.54): GTA has no hunger stat, so this is a client-local value that
    // decays slowly (full -> empty in ~30 min). A placeholder for a future server food
    // system; reset to full on entry. Script-tick only.
    float g_satiety = 100.0f;
    ULONG g_satietyAt = 0;
    // 0.6: satiety from food the server used for us (InvUsed, join worker thread), added on the next vitals tick.
    std::atomic<int> g_satietyAdd{0};

    // 0.6 inventory: the name for the CHARACTER chip (the verified account's, else the name we joined under) and the counters of the
    // 10 s line. The name is written by the join worker (OnAccount) and read by it (OnInvState): g_invMtx keeps it whole for OnEnded.
    std::mutex g_invMtx;
    std::string g_invName;
    std::atomic<uint32_t> g_invStates{0}, g_invOpsSent{0}, g_invOpsUnsent{0}, g_invUsed{0};

    // ===== 0.5.4 outfit_items + inv_ped_view (GI 2026-11-07) =====
    // outfit_items (HKCU\Software\GAGE-MP, default on; 0 = 0.5.3: the worn clothing items change nothing on the ped): the SERVER's inventory
    // decides the shirt, pants and shoes on the local freemode ped (libs/sync/outfit.h); TickOutfitItems sets what differs, PublishLook sends it.
    // inv_ped_view (default on; 0 = 0.5.3: the mannequin): a scripted camera shows the player's own ped in the CHARACTER panel
    // (libs/sync/ped_view.h, TickInvPedView). Both are read with the session's switches (InvCReadSwitches).
    std::atomic<bool> g_outfitItems{true};
    std::atomic<bool> g_invPedView{true};
    // 0.5.8 inv_ped_mode (default on; 0 = the 0.5.4 camera) and inv_ped_slot (0 / 1 / 2 forced; unset = search): the ped preview on GTA's
    // frontend ped renderer (libs/sync/ped_preview.h, TickInvPedPreview). g_ppClone is the hidden clone (script tick only): declared here so the
    // ambient clean-up's exemption list (AmbientGatherExempt) can keep it.
    std::atomic<bool> g_invPedPreview{true};
    std::atomic<int> g_invPedSlot{-1};
    int32_t g_ppClone = 0;
    int32_t g_ppCloneOld = 0;                      // the clone a look change replaced, kept until the new one is given
    bool g_ppOwnsFrontend = false;                // the preview owns the active frontend (TickPauseMenu must not treat it as the pause menu)
    std::mutex g_outfitMtx;                       // the join worker writes the server's inventory, the script tick reads it
    sync::outfit::WornFeed g_outfitFeed;           // 0.5.6: fed by InvState AND InvSnap (0.5.4 / 0.5.5 read InvState only: GI's shirt stayed on)
    std::atomic<bool> g_outfitRecheck{false};      // DressPlayer re-dressed the ped: compare at once, not at the next 1 s check

    // ===== 0.5.6 overlay_fire_block + weapon_keys (libs/sync/overlay_input.h; GI 2026-11-07) =====
    // overlay_fire_block (HKCU\Software\GAGE-MP, default on; 0 = 0.5.5): an open panel also calls DISABLE_PLAYER_FIRING every tick.
    // weapon_keys (default on; 0 = 0.5.5): once the server owns the weapons, GTA's own weapon selection (Tab, 1-9, the mouse wheel) is held
    // disabled every tick and the number keys 1-4 use our quick slots with the page closed too. Both read with the session's switches.
    std::atomic<bool> g_overlayFireBlock{true};
    std::atomic<bool> g_weaponKeys{true};
    struct InputHoldDiag                           // script tick only; the 10 s line "engine: input holds (0.5.6)"
    {
        sync::overlayinput::FrameCadence cadence;  // GET_FRAME_COUNT per tick, every tick
        sync::overlayinput::SwitchWatch sw;        // the ped's selected weapon, attributed
        uint32_t panelTicks = 0, panelFrames = 0, fireCalls = 0, fireFailed = 0, lmbHeld = 0, shotsOpen = 0, meleeOpen = 0;
        uint32_t keyTicks = 0, wheelFailed = 0, foreignFrom = 0, foreignTo = 0;
        bool logged = false;                       // the first weapon-keys tick of the session was logged
        double lastLogMs = 0;
        int64_t panelFrameAt = -1;
    };
    InputHoldDiag g_inHold;

    void InvCReadSwitches()
    {
        g_overlayFireBlock.store(reg::Get(L"overlay_fire_block") != L"0");
        g_weaponKeys.store(reg::Get(L"weapon_keys") != L"0");
        game::Overlay::SetQuickKeys(g_weaponKeys.load());
        GLOGI("engine: input holds (0.5.6): overlay_fire_block=%s; weapon_keys=%s", g_overlayFireBlock.load()
              ? "on (an open panel also calls DISABLE_PLAYER_FIRING every tick: no shot or punch through a click; LIVE-UNVERIFIED)"
              : "OFF (the panel's control list only, as 0.5.5)",
              g_weaponKeys.load()
              ? "on (once the server owns the weapons: GTA's weapon wheel / Tab / number keys 1-9 / mouse-wheel switch held disabled every tick, "
                "the wheel's result suppressed; keys 1-4 use the quick slots with the page closed; LIVE-UNVERIFIED)"
              : "OFF (GTA's own weapon selection, quick slots only on the open page, as 0.5.5)");
        g_outfitItems.store(reg::Get(L"outfit_items") != L"0");
        g_invPedView.store(reg::Get(L"inv_ped_view") != L"0");
        GLOGI("engine: outfit items (0.5.4): outfit_items=%s", g_outfitItems.load()
              ? "on (the server's worn shirt / pants / shoes items decide the ped's components; an empty slot is bare; LIVE-UNVERIFIED)"
              : "OFF (the 0.5.3 behaviour: the items change nothing on the ped)");
        GLOGI("engine: inventory ped view (0.5.4): inv_ped_view=%s", g_invPedView.load()
              ? "on (a ped in the CHARACTER panel while the page is open; how: inv_ped_mode below)"
              : "OFF (the 0.5.3 mannequin; inv_ped_mode does nothing)");
        g_invPedPreview.store(reg::Get(L"inv_ped_mode") != L"0");
        const std::wstring slot = reg::Get(L"inv_ped_slot");
        g_invPedSlot.store(sync::pedpreview::ParseSlot(slot));
        GLOGI("engine: inventory ped preview (0.5.8): inv_ped_mode=%s; inv_ped_slot=%s%s", g_invPedPreview.load()
              ? "on (a hidden clone on GTA's frontend ped renderer, FE_MENU_VERSION_EMPTY_NO_BACKGROUND without pausing, our backdrop behind it: "
                "only the character, in any seat; torn down while dead or dying and while the player's own pause menu is up; a ragdoll does not "
                "matter (the clone stands); after 3 failed opens or no slot that puts the ped in the window: the 0.5.4 camera for the session; "
                "LIVE-UNVERIFIED)"
              : "OFF (the 0.5.4 scripted camera: on foot only, the world behind the ped)",
              g_invPedSlot.load() < 0 ? "auto (search 0, 1, 2 by reading the window back; D3D12: slot 0 unverified)" : "forced ",
              g_invPedSlot.load() < 0 ? "" : (g_invPedSlot.load() == 0 ? "0" : g_invPedSlot.load() == 1 ? "1" : "2"));
    }
    // Join worker thread (OnInvState and, since 0.5.6, OnInvSnap): keep the server's inventory for the outfit. One line per change of the
    // worn shirt / pants / shoes the server sent (at most 200 a session): the live proof that a take-off reached the engine.
    void OutfitNoteInventory(const std::string& blob, bool snap)
    {
        namespace of = sync::outfit;
        std::string before, after;
        uint32_t states = 0, snaps = 0;
        bool took = false;
        {
            std::lock_guard<std::mutex> lk(g_outfitMtx);
            const auto trio = [](const sync::inv::Character& c) {
                return std::string("shirt=") + of::WornKey(c, sync::inv::Equip::Shirt) + " pants=" + of::WornKey(c, sync::inv::Equip::Pants) +
                       " shoes=" + of::WornKey(c, sync::inv::Equip::Shoes);
            };
            if (g_outfitFeed.have)
                before = trio(g_outfitFeed.ch);
            took = snap ? g_outfitFeed.OnInvSnap(blob) : g_outfitFeed.OnInvState(blob);   // undecodable: the overlay logs it, the ped keeps the last
            if (took)
                after = trio(g_outfitFeed.ch);
            states = g_outfitFeed.states;
            snaps = g_outfitFeed.snaps;
        }
        static std::atomic<int> lines{0};
        if (took && after != before && lines.fetch_add(1) < 200)
            GLOGI("engine: outfit items (0.5.6): the server's worn items %s %s (from %s; %u InvState / %u InvSnap so far)",
                  before.empty() ? "are" : "changed to", after.c_str(), snap ? "an InvSnap" : "an InvState", states, snaps);
    }
    void OutfitClear()
    {
        std::lock_guard<std::mutex> lk(g_outfitMtx);
        g_outfitFeed.End();
    }

    // 0.5.4 inventory world (libs/sync/inventory_world.h, docs/api/protocol.md "0.5.4"): weapons are inventory items (the ped's weapon wheel
    // follows the server's inventory and the inventory follows the ped), dropped items lie on the ground, trunks hold items. inv_world
    // (HKCU\Software\GAGE-MP, read at every Welcome, default on; 0 = the 0.5.3 behaviour: the server's offer is never answered, so the
    // server keeps treating us as a 0.5.3 client and /gun gives the weapon locally). The join worker writes, the script tick reads.
    std::atomic<bool> g_invWorldOn{true};
    std::atomic<bool> g_invWorldOffered{false};   // this session's server sent its offer (an InvNearby): it speaks 0.5.4
    std::atomic<bool> g_invWorldLive{false};      // we answered it (our first InvReport went out): the server sends InvSnap, /gun is the server's
    std::atomic<bool> g_invwReset{true};          // a session began or ended: the script tick drops its state
    std::mutex g_invwMtx;                         // guards the two below
    bool g_invwSnapFresh = false;
    proto::InvSnap g_invwSnap;                    // the newest InvSnap not yet applied to the ped (strips merged)
    std::atomic<uint32_t> g_invwSnaps{0}, g_invwNearby{0}, g_invwReports{0}, g_invwReportsUnsent{0}, g_invwGives{0}, g_invwRemoves{0},
        g_invwAmmoSets{0}, g_invwSelects{0}, g_invwRestores{0};

    // With stock scripts off the per-tick entry-boot suppression has no script to kill; it then runs
    // once per entry (for its FORCE_GAME_STATE_PLAYING). Script tick only; EnterServer re-arms it.
    bool g_entryBootOnce = false;

    // ---- frame-time probe (always on) --------------------------------------
    // Says from gage.log whether GTA or GAGE cost a frame. No lock and no allocation per frame: the
    // render thread (Frame, once per Present) and the game thread (ScriptTick) only add to relaxed
    // atomics; the watchdog thread swaps them to zero every 10 s, folds a summary into its "game
    // thread alive" line, and logs a rate-limited WARN for a single hitch over 100 ms (the render
    // thread only records the hitch, so no file I/O lands in a slow frame).
    const int64_t g_qpcFreq = [] {
        LARGE_INTEGER f;
        return QueryPerformanceFrequency(&f) && f.QuadPart > 0 ? (int64_t)f.QuadPart : (int64_t)1;
    }();
    inline int64_t QpcNow()
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return t.QuadPart;
    }
    // The engine's one monotonic millisecond clock (QPC since the DLL loaded, sub-ms): the remote-
    // player smoother and the sender both run on it. GetTickCount moves in 15.6 ms steps.
    const int64_t g_qpcEpoch = QpcNow();
    inline double NowMs()
    {
        const int64_t t = QpcNow() - g_qpcEpoch;
        return (double)(t / g_qpcFreq) * 1000.0 + (double)(t % g_qpcFreq) * 1000.0 / (double)g_qpcFreq;
    }
    // QPC ticks -> microseconds, saturated to 32 bits (~71 min).
    inline uint32_t QpcUs(int64_t ticks)
    {
        if (ticks <= 0)
            return 0;
        const int64_t us = ticks / g_qpcFreq * 1000000 + ticks % g_qpcFreq * 1000000 / g_qpcFreq;
        return us > 0xFFFFFFFFll ? 0xFFFFFFFFu : (uint32_t)us;
    }

    // ---- 0.4.19 radio off (libs/sync/radio_off.h has the rules; GI: no vehicle radio ever, no radio wheel and its slow-mo) ----
    // no_radio (HKCU\Software\GAGE-MP): unset / 1 = on, 0 = the old behaviour (nothing issued; the read-backs still run, so a
    // no_radio=0 log shows what the game does by itself). Read at every Welcome; on until the first Welcome.
    std::atomic<bool> g_noRadio{true};
    sync::RadioOffGate g_radioGate;
    struct RadioStats
    {
        // issued (what we called; "uncallable" = a setter the build could not call)
        uint32_t globals = 0, vehApplyNew = 0, vehApplyReadback = 0, vehApplyPeriodic = 0, copiesAtSpawn = 0, copiesReapplied = 0,
                 holdTicks = 0, uncallable = 0;
        // read-backs (ticks in or entering a vehicle; every one of them is passive)
        uint32_t ticksInVeh = 0, vehRadioOn = 0, stationOn = 0, wheelKeyHeld = 0, wheelHudActive = 0, slowFrames = 0, timedFrames = 0,
                 copiesAudited = 0, copiesRadioOn = 0;
        int lastStation = -1;           // GET_PLAYER_RADIO_STATION_INDEX at the last seated read (-1 = none / not callable)
    };
    RadioStats g_radio;
    double g_radioReportMs = 0;
    double g_radioPrevMs = -1;          // the wall clock / game timer of the previous tick in a vehicle (slow-mo ground truth)
    int g_radioPrevTimer = -1;
    bool g_radioFirstReport = true;

    void ReadRadioOffSwitch()
    {
        g_noRadio.store(reg::Get(L"no_radio") != L"0");
        GLOGI("engine: radio off (0.4.19): no_radio=%s", g_noRadio.load()
              ? "on (vehicle radio OFF + disabled on the car we sit in / enter and on every car we create, user radio control and mobile "
                "radio off, flight music off, controls 81-85 incl. the radio wheel (85, its slow-mo) held disabled in a vehicle)"
              : "OFF (0 = the game's radio and radio wheel as before; read-backs only)");
    }

    // The per-vehicle "off": the station first (a disabled radio may ignore it), then the radio itself.
    bool RadioOffVehicle(int32_t veh)
    {
        static const char kOff[] = "OFF";
        const bool a = g_world->SetVehRadioStation(veh, kOff);
        const bool b = g_world->SetVehicleRadioEnabled(veh, false);
        if (!a || !b)
            ++g_radio.uncallable;
        return a && b;
    }

    // Right after CREATE_VEHICLE (a server copy, /car): a remote driver's car plays nothing either.
    void RadioOffNewVehicle(int32_t veh)
    {
        if (!veh || !g_noRadio.load())
            return;
        RadioOffVehicle(veh);
        ++g_radio.copiesAtSpawn;
    }

    // Every script tick (state may change). lp = the local ped.
    void TickRadioOff(int32_t lp, double nowMs)
    {
        static const char kOff[] = "OFF";
        static const char kFlightMusic[] = "DisableFlightMusic";
        const bool on = g_noRadio.load();
        const int32_t vehIn = lp ? g_world->GetVehiclePedIsIn(lp) : 0;
        const int32_t vehUsing = lp ? g_world->GetVehiclePedIsUsing(lp) : 0;
        const int32_t target = sync::RadioTarget(vehIn, vehUsing);

        // Read-backs first: the state this tick, before anything is applied.
        bool radioOn = false;
        if (target)
        {
            ++g_radio.ticksInVeh;
            bool vehOn = false;
            if (g_world->IsVehicleRadioOn(target, vehOn) && vehOn)
                ++g_radio.vehRadioOn;
            int station = -1;
            if (vehIn && g_world->GetPlayerRadioStationIndex(station))
            {
                g_radio.lastStation = station;
                if (station != sync::kRadioStationOff)
                    ++g_radio.stationOn;
            }
            radioOn = sync::RadioReadsOn(vehOn, vehIn != 0, station);
            bool held = false;
            if (g_world->IsDisabledControlPressed(0, sync::kControlVehRadioWheel, held) && held)
                ++g_radio.wheelKeyHeld;
            bool hud = false;
            if (g_world->IsHudComponentActive(sync::kHudRadioStations, hud) && hud)
                ++g_radio.wheelHudActive;
            const int timer = g_world->GetGameTimer();
            if (g_radioPrevMs >= 0 && g_radioPrevTimer >= 0 && timer >= 0)
            {
                ++g_radio.timedFrames;
                if (sync::IsSlowFrame((double)(timer - g_radioPrevTimer), nowMs - g_radioPrevMs))
                    ++g_radio.slowFrames;
            }
            g_radioPrevMs = nowMs;
            g_radioPrevTimer = timer;
        }
        else
        {
            g_radioPrevMs = -1;
            g_radioPrevTimer = -1;
        }

        sync::RadioOffIn in;
        in.nowMs = nowMs;
        in.enabled = on;
        in.vehIn = vehIn;
        in.vehUsing = vehUsing;
        in.radioReadsOn = radioOn;
        const sync::RadioOffOut out = g_radioGate.Tick(in);
        if (out.applyGlobal)
        {
            const bool a = g_world->SetUserRadioControlEnabled(false);
            const bool b = g_world->SetMobileRadioEnabledDuringGameplay(false);
            const bool c = g_world->SetAudioFlag(kFlightMusic, true);
            if (!a || !b || !c)
                ++g_radio.uncallable;
            ++g_radio.globals;
        }
        if (out.applyVehicle)
        {
            RadioOffVehicle(out.applyVehicle);
            if (vehIn == out.applyVehicle && !g_world->SetRadioToStationName(kOff))   // the player's own station, seated only
                ++g_radio.uncallable;
            if (out.why == sync::RadioApplyWhy::NewVehicle) ++g_radio.vehApplyNew;
            else if (out.why == sync::RadioApplyWhy::ReadBackOn) ++g_radio.vehApplyReadback;
            else ++g_radio.vehApplyPeriodic;
        }
        if (out.holdControls)
        {
            for (const int control : sync::kRadioControls)
            {
                uint64_t args[] = { 0, (uint64_t)control, 1 };  // DISABLE_CONTROL_ACTION(0, control, true): a per-frame effect
                g_world->CallNative(game::native::DISABLE_CONTROL_ACTION, args, 3, nullptr);
            }
            ++g_radio.holdTicks;
        }

        if (nowMs - g_radioReportMs < 10000.0)
            return;
        g_radioReportMs = nowMs;
        // The server's copies: does any of them play a radio (a remote driver's car)? Off again if so.
        for (auto& kv : g_vehs)
        {
            const int32_t h = kv.second.handle;
            bool vehOn = false;
            if (!h || !g_world->IsVehicleRadioOn(h, vehOn))
                continue;
            ++g_radio.copiesAudited;
            if (!vehOn)
                continue;
            ++g_radio.copiesRadioOn;
            if (on && h != target)
            {
                RadioOffVehicle(h);
                ++g_radio.copiesReapplied;
            }
        }
        const RadioStats& s = g_radio;
        const bool quiet = !s.ticksInVeh && !s.globals && !s.copiesAtSpawn && !s.copiesRadioOn;
        if (!quiet || g_radioFirstReport)
        {
            g_radioFirstReport = false;
            GLOGI("engine: radio off (0.4.19), last 10 s: no_radio=%s; issued: globals %u, vehicle off %u (new %u, read-back on %u, periodic %u), "
                  "copies off at spawn %u, copies off again %u, control-hold ticks %u (5 controls each), uncallable %u; read-back: %u ticks in a "
                  "vehicle, vehicle radio ON %u, player station not off %u (last index %d, 255 = off), radio-wheel key held %u, radio HUD active %u, "
                  "slow-mo frames %u of %u (game timer < %.0f%% of the wall clock), copies audited %u / radio ON %u",
                  on ? "on" : "OFF", s.globals, s.vehApplyNew + s.vehApplyReadback + s.vehApplyPeriodic, s.vehApplyNew, s.vehApplyReadback,
                  s.vehApplyPeriodic, s.copiesAtSpawn, s.copiesReapplied, s.holdTicks, s.uncallable, s.ticksInVeh, s.vehRadioOn, s.stationOn,
                  s.lastStation, s.wheelKeyHeld, s.wheelHudActive, s.slowFrames, s.timedFrames, sync::kSlowFrameRatio * 100.0, s.copiesAudited,
                  s.copiesRadioOn);
        }
        g_radio = RadioStats();
    }
    // ---- 0.4.22 noclip (libs/sync/noclip.h has the rules and the keys; GI: noclip in the Tab menu's quick actions, also /noclip) ----
    // noclip (HKCU\Software\GAGE-MP): unset / 1 = available (the Tab menu row, /noclip), 0 = the row is hidden and noclip cannot
    // start (one already flying lands and gives its flags back). Read at every Welcome; on until the first Welcome.
    std::atomic<bool> g_noclipSwitch{true};
    sync::noclip::Gate g_noclip;
    bool g_noclipToggle = false;            // a request from RunCommands (/noclip, the Tab menu row), consumed by TickNoclip the same tick
    struct NoclipStats
    {
        // flying (script ticks)
        uint32_t onTicks = 0, moves = 0, keysHeld = 0, fastTicks = 0, slowTicks = 0, holdTicks = 0, controlHolds = 0, resyncs = 0;
        // starts / ends / refusals
        uint32_t starts = 0, endToggle = 0, endSwitch = 0, endNotReady = 0, endGone = 0;
        uint32_t refusedDisabled = 0, refusedNotReady = 0, refusedPassenger = 0;
        // landings
        uint32_t placed = 0, fellBack = 0, skipped = 0;
        // issued vs read back (samples every kProbeEveryMs while flying)
        uint32_t samples = 0, collisionOff = 0, collisionBackOn = 0, invincibleOn = 0, invincibleLost = 0, groundProbes = 0, groundValid = 0,
                 uncallable = 0;
        float cmdSpeedMax = 0, gameSpeedMax = 0, driftMax = 0, heightLast = 0, camPitchLast = 0, camYawLast = 0;
    };
    NoclipStats g_noclipStats;
    double g_noclipReportMs = 0, g_noclipProbeMs = 0;
    bool g_noclipFirstReport = true, g_noclipHaveWritten = false, g_noclipWasActive = false;
    sync::noclip::Vec3 g_noclipLastWritten;

    void ReadNoclipSwitch()
    {
        g_noclipSwitch.store(reg::Get(L"noclip") != L"0");
        GLOGI("engine: noclip (0.4.22): noclip=%s", g_noclipSwitch.load()
              ? "on (Tab menu > Quick actions > Noclip, or /noclip: W A S D fly along the view, Space up, Ctrl down, Shift fast x5, Alt slow x0.2; "
                "no collision, no gravity, invincible while on; the flags are read first and given back, and it lands on the ground under you)"
              : "OFF (0 = the Tab menu row is hidden and noclip cannot start)");
    }

    template <typename T> void AtomicMax(std::atomic<T>& a, T v)
    {
        T cur = a.load(std::memory_order_relaxed);
        while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
    }

    // g_peersMtx, with the time spent waiting for it (and, on the script side, holding it) measured: the join
    // worker's 10 s heartbeat line reports the worst of each (sync plan N6a). QPC ticks, swapped to zero by the
    // report. The lock only ever covers copies and smoother arithmetic, never a native call (see SyncNet).
    struct PeersLockStats
    {
        std::atomic<int64_t> joinWaitMax{0}, scriptWaitMax{0}, scriptHoldMax{0};
        std::atomic<uint32_t> joinLocks{0}, scriptLocks{0};
    };
    PeersLockStats g_lockStats;
    class PeersLock
    {
    public:
        explicit PeersLock(std::mutex& m, bool script) : m_(m), script_(script)
        {
            const int64_t t0 = QpcNow();
            m_.lock();
            t1_ = QpcNow();
            AtomicMax(script ? g_lockStats.scriptWaitMax : g_lockStats.joinWaitMax, t1_ - t0);
            (script ? g_lockStats.scriptLocks : g_lockStats.joinLocks).fetch_add(1, std::memory_order_relaxed);
        }
        ~PeersLock()
        {
            if (script_)
                AtomicMax(g_lockStats.scriptHoldMax, QpcNow() - t1_);
            m_.unlock();
        }
        PeersLock(const PeersLock&) = delete;
        PeersLock& operator=(const PeersLock&) = delete;

    private:
        std::mutex& m_;
        bool script_;
        int64_t t1_ = 0;
    };
    // The join worker's 10 s heartbeat line carries this (PeerSinks::beatNote): the worst waits and holds of the
    // window, then the counters start over.
    std::string PeersLockNote()
    {
        auto ms = [](int64_t ticks) { return (double)QpcUs(ticks) / 1000.0; };
        const double workerWait = ms(g_lockStats.joinWaitMax.exchange(0));
        const double gameWait = ms(g_lockStats.scriptWaitMax.exchange(0));
        const double gameHold = ms(g_lockStats.scriptHoldMax.exchange(0));
        const uint32_t workerLocks = g_lockStats.joinLocks.exchange(0), gameLocks = g_lockStats.scriptLocks.exchange(0);
        char buf[240];
        std::snprintf(buf, sizeof(buf),
                      "peer lock: worker wait max %.2f ms over %u locks, game thread wait max %.2f ms / hold max %.2f ms over %u locks",
                      workerWait, (unsigned)workerLocks, gameWait, gameHold, (unsigned)gameLocks);
        return buf;
    }

    constexpr uint32_t kSlowFrameUs = 33000;    // a frame under 30 fps
    constexpr uint32_t kHitchUs     = 100000;   // a visible hitch: counted, and named in a WARN
    constexpr ULONGLONG kHitchLogEveryMs = 5000;

    // Intervals in microseconds: 0.1 ms buckets to 20 ms, 1 ms to 100 ms, 10 ms to 1 s, then one
    // overflow bucket. A percentile reads as its bucket's upper edge (capped at the exact max).
    struct IntervalHist
    {
        static constexpr int kFine = 200, kMid = 80, kCoarse = 90, kBuckets = kFine + kMid + kCoarse + 1;
        std::atomic<uint32_t> bucket[kBuckets] = {};
        std::atomic<uint32_t> slow{0}, hitches{0}, maxUs{0};

        static int Index(uint32_t us)
        {
            if (us < 20000)   return (int)(us / 100);
            if (us < 100000)  return kFine + (int)((us - 20000) / 1000);
            if (us < 1000000) return kFine + kMid + (int)((us - 100000) / 10000);
            return kBuckets - 1;
        }
        static uint32_t UpperUs(int i)
        {
            if (i < kFine)        return (uint32_t)(i + 1) * 100;
            if (i < kFine + kMid) return 20000 + (uint32_t)(i - kFine + 1) * 1000;
            if (i < kBuckets - 1) return 100000 + (uint32_t)(i - kFine - kMid + 1) * 10000;
            return 0xFFFFFFFFu;
        }
        // One writer thread.
        void Add(uint32_t us)
        {
            bucket[Index(us)].fetch_add(1, std::memory_order_relaxed);
            if (us > kSlowFrameUs) slow.fetch_add(1, std::memory_order_relaxed);
            if (us > kHitchUs) hitches.fetch_add(1, std::memory_order_relaxed);
            AtomicMax(maxUs, us);
        }
    };
    // What the watchdog swapped out of an IntervalHist (watchdog thread only).
    struct HistSnap
    {
        uint32_t bucket[IntervalHist::kBuckets];
        uint32_t count = 0, slow = 0, hitches = 0, maxUs = 0;

        void Take(IntervalHist& h)
        {
            count = 0;
            for (int i = 0; i < IntervalHist::kBuckets; ++i)
                count += bucket[i] = h.bucket[i].exchange(0, std::memory_order_relaxed);
            slow = h.slow.exchange(0, std::memory_order_relaxed);
            hitches = h.hitches.exchange(0, std::memory_order_relaxed);
            maxUs = h.maxUs.exchange(0, std::memory_order_relaxed);
        }
        double PctMs(double q) const
        {
            if (!count)
                return 0.0;
            uint64_t need = (uint64_t)std::ceil(q * count), seen = 0;
            if (need < 1) need = 1;
            for (int i = 0; i < IntervalHist::kBuckets; ++i)
            {
                seen += bucket[i];
                if (seen >= need)
                {
                    const uint32_t up = IntervalHist::UpperUs(i);
                    return (up < maxUs ? up : maxUs) / 1000.0;
                }
            }
            return maxUs / 1000.0;
        }
    };

    // GameWork's parts, timed on the script tick; the slowest one of a tick is reported with it.
    enum WorkSec : uint8_t {
        kSecNone, kSecMenuControls, kSecUiTick, kSecEntry, kSecReadSend, kSecSpFrame, kSecAmbientFrame,
        kSecEntryBoot, kSecHijackers, kSecLife, kSecPedGuard, kSecCommands, kSecHud, kSecSpSystems,
        kSecBlips, kSecDumpScripts, kSecPeers, kSecCount
    };
    const char* const kSecName[kSecCount] = {
        "-", "HoldMenuControls", "UiTick", "EnterServer", "read+send", "SuppressSPFrame",
        "SuppressAmbientFrame", "SuppressEntryBoot", "SuppressPedHijackers", "TickLife", "TickEntryGuard",
        "RunCommands", "DrawHud", "SuppressSPSystems+DisableWanted", "ClearSpBlips", "DumpLiveScripts", "peers"
    };
    struct WorkProbe { int64_t slowest = 0; uint8_t sec = kSecNone; };
    // Times one part of GameWork into `probe` (null = not timed: the render-thread fallback).
    struct WorkSection
    {
        WorkProbe* p;
        uint8_t id;
        int64_t t0;
        WorkSection(WorkProbe* probe, uint8_t sec) : p(probe), id(sec), t0(probe ? QpcNow() : 0) {}
        ~WorkSection()
        {
            if (!p)
                return;
            const int64_t d = QpcNow() - t0;
            if (d > p->slowest) { p->slowest = d; p->sec = id; }
        }
        WorkSection(const WorkSection&) = delete;
        WorkSection& operator=(const WorkSection&) = delete;
    };
    // A tick's cost packed so one atomic max keeps it whole: GameWork us (bits 63..32), the slowest
    // part's us (31..8, capped at ~16.7 s) and that part's WorkSec (7..0).
    inline uint64_t PackWork(uint32_t workUs, uint32_t partUs, uint8_t sec)
    {
        return ((uint64_t)workUs << 32) | ((uint64_t)(partUs > 0xFFFFFFu ? 0xFFFFFFu : partUs) << 8) | sec;
    }
    inline uint32_t PackedWorkUs(uint64_t p) { return (uint32_t)(p >> 32); }
    inline uint32_t PackedPartUs(uint64_t p) { return (uint32_t)((p >> 8) & 0xFFFFFFu); }
    inline const char* PackedPartName(uint64_t p) { return kSecName[(p & 0xFF) < kSecCount ? (p & 0xFF) : 0]; }

    struct FrameStats
    {
        IntervalHist present;   // Present to Present (render thread)
        IntervalHist tick;      // script tick to script tick (game thread)
        // Our own time inside Frame(): the overlay (UiFrame) and the rest (render thread).
        std::atomic<uint32_t> frames{0}, uiMaxUs{0}, restMaxUs{0};
        std::atomic<uint64_t> uiSumUs{0}, restSumUs{0};
        // Our own time inside ScriptTick (GameWork; game thread).
        std::atomic<uint32_t> works{0};
        std::atomic<uint64_t> workSumUs{0}, workMax{0};   // workMax: PackWork
        // Game-thread cost since the last Present (game thread adds, Frame swaps to zero), so a
        // hitch can say what the script tick did inside it.
        std::atomic<uint64_t> workSincePresentUs{0}, workMaxSincePresent{0};
        std::atomic<uint32_t> ticksSincePresent{0}, tickGapSincePresentUs{0};
        // Our previous Frame() (render thread), the start of the next Present gap.
        std::atomic<uint32_t> prevFrameUs{0}, prevUiUs{0};
    };
    FrameStats g_fs;

    // The worst unlogged hitch, a seqlock: the render thread writes (seq odd while it does), the
    // watchdog reads and marks it logged. A bigger hitch replaces an unlogged one.
    struct HitchRecord
    {
        std::atomic<uint32_t> seq{0}, loggedSeq{0};
        std::atomic<int64_t>  at{0};                      // QPC of the Present that ended the gap
        std::atomic<uint32_t> gapUs{0}, frameUs{0}, uiUs{0}, ticks{0}, tickGapUs{0};
        std::atomic<uint64_t> workUs{0}, workMax{0};
    };
    HitchRecord g_hitch;

    // Render thread, at the top of every Frame(): the Present interval and, past kHitchUs, the hitch.
    void NotePresent(int64_t now)
    {
        static int64_t last = 0;
        const int64_t prev = last;
        last = now;
        const uint64_t workUs  = g_fs.workSincePresentUs.exchange(0, std::memory_order_relaxed);
        const uint64_t workMax = g_fs.workMaxSincePresent.exchange(0, std::memory_order_relaxed);
        const uint32_t ticks   = g_fs.ticksSincePresent.exchange(0, std::memory_order_relaxed);
        const uint32_t tickGap = g_fs.tickGapSincePresentUs.exchange(0, std::memory_order_relaxed);
        if (!prev)
            return;
        const uint32_t gap = QpcUs(now - prev);
        g_fs.present.Add(gap);
        if (gap <= kHitchUs)
            return;
        HitchRecord& h = g_hitch;
        const uint32_t s = h.seq.load(std::memory_order_relaxed);
        if (s != h.loggedSeq.load(std::memory_order_acquire) && gap <= h.gapUs.load(std::memory_order_relaxed))
            return;                                    // a worse one is still waiting to be logged
        h.seq.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        h.at.store(now, std::memory_order_relaxed);
        h.gapUs.store(gap, std::memory_order_relaxed);
        h.frameUs.store(g_fs.prevFrameUs.load(std::memory_order_relaxed), std::memory_order_relaxed);
        h.uiUs.store(g_fs.prevUiUs.load(std::memory_order_relaxed), std::memory_order_relaxed);
        h.ticks.store(ticks, std::memory_order_relaxed);
        h.tickGapUs.store(tickGap, std::memory_order_relaxed);
        h.workUs.store(workUs, std::memory_order_relaxed);
        h.workMax.store(workMax, std::memory_order_relaxed);
        h.seq.store(s + 2, std::memory_order_release);
    }

    // Render thread, at the end of every Frame(): our own time in it (t0 = entry, ui = UiFrame ticks).
    void NoteFrameDone(int64_t t0, int64_t ui)
    {
        const uint32_t total = QpcUs(QpcNow() - t0), uiUs = QpcUs(ui);
        const uint32_t rest = total > uiUs ? total - uiUs : 0;
        g_fs.frames.fetch_add(1, std::memory_order_relaxed);
        g_fs.uiSumUs.fetch_add(uiUs, std::memory_order_relaxed);
        g_fs.restSumUs.fetch_add(rest, std::memory_order_relaxed);
        AtomicMax(g_fs.uiMaxUs, uiUs);
        AtomicMax(g_fs.restMaxUs, rest);
        g_fs.prevFrameUs.store(total, std::memory_order_relaxed);
        g_fs.prevUiUs.store(uiUs, std::memory_order_relaxed);
    }

    // Game thread, after every ScriptTick's GameWork (t0..t1).
    void NoteScriptTick(int64_t t0, int64_t t1, const WorkProbe& probe)
    {
        static int64_t last = 0;
        if (last)
        {
            const uint32_t gap = QpcUs(t0 - last);
            g_fs.tick.Add(gap);
            AtomicMax(g_fs.tickGapSincePresentUs, gap);
        }
        last = t0;
        const uint32_t us = QpcUs(t1 - t0);
        const uint64_t packed = PackWork(us, QpcUs(probe.slowest), probe.sec);
        g_fs.works.fetch_add(1, std::memory_order_relaxed);
        g_fs.workSumUs.fetch_add(us, std::memory_order_relaxed);
        AtomicMax(g_fs.workMax, packed);
        g_fs.workSincePresentUs.fetch_add(us, std::memory_order_relaxed);
        AtomicMax(g_fs.workMaxSincePresent, packed);
        g_fs.ticksSincePresent.fetch_add(1, std::memory_order_relaxed);
    }

    // Watchdog thread: swap the window's figures to zero and format them as the watchdog line's tail.
    void TakeFrameStats(char* out, size_t cap, double windowS)
    {
        static HistSnap p, t;   // watchdog thread only; static keeps 3 KB off its stack
        p.Take(g_fs.present);
        t.Take(g_fs.tick);
        const uint32_t frames = g_fs.frames.exchange(0, std::memory_order_relaxed);
        const uint64_t uiSum = g_fs.uiSumUs.exchange(0, std::memory_order_relaxed);
        const uint64_t restSum = g_fs.restSumUs.exchange(0, std::memory_order_relaxed);
        const uint32_t uiMax = g_fs.uiMaxUs.exchange(0, std::memory_order_relaxed);
        const uint32_t restMax = g_fs.restMaxUs.exchange(0, std::memory_order_relaxed);
        const uint32_t works = g_fs.works.exchange(0, std::memory_order_relaxed);
        const uint64_t workSum = g_fs.workSumUs.exchange(0, std::memory_order_relaxed);
        const uint64_t workMax = g_fs.workMax.exchange(0, std::memory_order_relaxed);
        const double perFrame = frames ? 1.0 / frames : 0.0;
        std::snprintf(out, cap,
            "; present %.1f fps, p50 %.1f p95 %.1f p99 %.1f max %.1f ms, %u >33 ms, %u >100 ms"
            "; ours per present: overlay %.2f avg %.2f max, rest %.2f avg %.2f max ms"
            "; script tick %u (p50 %.1f p95 %.1f max %.1f ms apart), GameWork %.1f ms total, "
            "max %.2f ms (slowest part %s %.2f ms)",
            windowS > 0.0 ? p.count / windowS : 0.0, p.PctMs(0.50), p.PctMs(0.95), p.PctMs(0.99),
            p.maxUs / 1000.0, p.slow, p.hitches,
            uiSum * perFrame / 1000.0, uiMax / 1000.0, restSum * perFrame / 1000.0, restMax / 1000.0,
            works, t.PctMs(0.50), t.PctMs(0.95), t.maxUs / 1000.0,
            workSum / 1000.0, PackedWorkUs(workMax) / 1000.0, PackedPartName(workMax),
            PackedPartUs(workMax) / 1000.0);
    }

    // Watchdog thread, every pass: log the worst unlogged hitch, at most once per kHitchLogEveryMs.
    void LogHitch(ULONGLONG nowMs)
    {
        static ULONGLONG lastLog = 0;
        if (lastLog && nowMs - lastLog < kHitchLogEveryMs)
            return;                                    // the render thread keeps the worst meanwhile
        HitchRecord& h = g_hitch;
        const uint32_t s = h.seq.load(std::memory_order_acquire);
        if ((s & 1) || s == h.loggedSeq.load(std::memory_order_relaxed))
            return;
        const int64_t at = h.at.load(std::memory_order_relaxed);
        const uint32_t gap = h.gapUs.load(std::memory_order_relaxed);
        const uint32_t frameUs = h.frameUs.load(std::memory_order_relaxed);
        const uint32_t uiUs = h.uiUs.load(std::memory_order_relaxed);
        const uint32_t ticks = h.ticks.load(std::memory_order_relaxed);
        const uint32_t tickGap = h.tickGapUs.load(std::memory_order_relaxed);
        const uint64_t workUs = h.workUs.load(std::memory_order_relaxed);
        const uint64_t workMax = h.workMax.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (h.seq.load(std::memory_order_relaxed) != s)
            return;                                    // rewritten while read: next pass
        h.loggedSeq.store(s, std::memory_order_release);
        lastLog = nowMs;

        // Our share is an upper bound: the script tick runs on the game thread, beside the render one.
        const uint64_t ours = (uint64_t)frameUs + workUs;
        const unsigned share = gap ? (unsigned)(ours * 100 / gap) : 0;
        char verdict[96];
        if (share >= 50)
            std::snprintf(verdict, sizeof(verdict), "mostly Solar RP (%s)",
                          frameUs >= workUs ? "our Present-side frame" : PackedPartName(workMax));
        else if (share >= 10)
            std::snprintf(verdict, sizeof(verdict), "partly Solar RP (%s)",
                          frameUs >= workUs ? "our Present-side frame" : PackedPartName(workMax));
        else if (!ticks)
            std::snprintf(verdict, sizeof(verdict), "not Solar RP: no script tick ran in it (game thread stalled or loading)");
        else if ((uint64_t)tickGap * 2 >= gap)
            std::snprintf(verdict, sizeof(verdict), "not Solar RP: GTA's game thread stalled too");
        else
            std::snprintf(verdict, sizeof(verdict), "not Solar RP: render side (game thread kept ticking)");
        GLOGW("engine: hitch %.1f ms between two Presents (%.1f s ago): Solar RP at most %.2f ms (%u %%) -- our "
              "previous frame %.2f ms (overlay %.2f), GameWork %.2f ms over %u script tick(s) (slowest %.2f ms, "
              "part %s %.2f ms); longest script-tick gap %.1f ms -> %s",
              gap / 1000.0, QpcUs(QpcNow() - at) / 1e6, ours / 1000.0, share, frameUs / 1000.0, uiUs / 1000.0,
              workUs / 1000.0, ticks, PackedWorkUs(workMax) / 1000.0, PackedPartName(workMax),
              PackedPartUs(workMax) / 1000.0, tickGap / 1000.0, verdict);
    }

    std::string RefuseText()
    {
        std::lock_guard<std::mutex> lk(g_refuseMu);
        return g_refuseText;
    }

    // The server refused our Hello (connect thread). A version mismatch is shown to the player three ways: the
    // loading screen's status line (PushLoaderStatus), a chat line now, and the same line again every
    // kRefuseRepeatMs once the loading screen has gone (RepeatRefusalLine), so it cannot be missed.
    void OnJoinRefused(const join::Session& sess)
    {
        if (sess.refuseReason == proto::RefuseReason::VersionMismatch && !sess.refuseText.empty())
        {
            {
                std::lock_guard<std::mutex> lk(g_refuseMu);
                g_refuseText = sess.refuseText;
            }
            game::Overlay::PushSystemLine(sess.refuseText.c_str());
            g_refuseSaidAt.store(GetTickCount64());
        }
        if (join::RefusalIsFinal(sess.refuseReason))
        {
            g_giveUp.store(true);   // after the text is stored: the loader never shows the generic line for it
            return;
        }
        // A refusal the server lifts by itself (our own dropped session's ghost holding the name, a full server, the
        // per-address connect rate): wait and try again instead of giving up for the rest of the run.
        const ULONGLONG now = GetTickCount64();
        g_refusedRetryAt.store(now + join::kRefusedRetryMs);
        if (now - g_refusedNoticeAt.load() >= 30000)
        {
            g_refusedNoticeAt.store(now);
            const std::string line = "The server refused the connection (" + (sess.refuseText.empty() ? std::string(proto::ToString(sess.refuseReason)) : sess.refuseText) +
                                     "). Trying again every few seconds.";
            game::Overlay::PushSystemLine(line.c_str());
        }
    }

    // Render thread, once per Present while the world is shown: repeat the refusal text in chat.
    void RepeatRefusalLine()
    {
        if (!g_giveUp.load())
            return;
        const ULONGLONG now = GetTickCount64();
        if (now - g_refuseSaidAt.load() < kRefuseRepeatMs)
            return;
        const std::string why = RefuseText();
        if (why.empty())
            return;
        g_refuseSaidAt.store(now);
        game::Overlay::PushSystemLine(why.c_str());
    }

    void EnsureConnected()
    {
        if (g_cfg.host.empty())
            return;                                    // no server configured
        if (g_giveUp.load())
            return;
        if (join::IsConnected())
            return;
        if (g_connecting.load())
            return;
        if (GetTickCount64() < g_refusedRetryAt.load())
            return;                                    // a refusal the server may lift: not before the pause is over
        ULONG now = GetTickCount();
        if (now - g_lastConnectAttempt.load() < 1000)
            return;                                   // retry at most ~1/s
        g_lastConnectAttempt.store(now);
        if (!g_connecting.exchange(true))
        {
            g_connectThread = std::thread([] {
                join::Session sess;
                // v8: with a join token the server asks the platform before its Welcome (up to ~2.5 s), so the wait is longer.
                if (join::Connect(g_cfg, g_cfg.joinToken.empty() ? 4000 : 8000, sess))   // was 6000; a failed attempt is retried after 1 s
                {
                    GLOGI("engine: joined %s:%d (server '%s', id %u)",
                        Narrow(g_cfg.host).c_str(), g_cfg.port,
                        sess.serverName.c_str(), (unsigned)sess.myId);
                    // v8: the token is single use and now redeemed; a reconnect goes without it (the launcher's Play mints a new one).
                    g_cfg.joinToken.clear();
                }
                else if (sess.refused)
                {
                    if (join::RefusalIsFinal(sess.refuseReason))
                        GLOGE("engine: join refused (%ls); not retrying", sess.error.c_str());
                    else
                        GLOGW("engine: join refused (%ls); trying again in %u s", sess.error.c_str(), join::kRefusedRetryMs / 1000);
                    OnJoinRefused(sess);
                }
                else
                    GLOGW("engine: join failed (%ls); retrying", sess.error.c_str());
                g_connecting.store(false);
            });
            g_connectThread.detach();
        }
    }

    // ---- session events (worker thread of join.cpp) ------------------------

    // A new session: a fresh smoother timed by this server's tick rate (ForTickHz falls back to
    // 20 Hz for 0). Runs before any WorldState of the session (join.h PeerSinks::onWelcome).
    void OnWelcome(uint16_t myId, uint16_t tickHz, uint8_t playerHealth)
    {
        g_tickHz.store(tickHz);
        // T6: the server's maximum health BEFORE g_v4Reset is raised below (and before the g_peersMtx wait further down): the script tick
        // that handles the reset sizes the ped and the tracker from it, and used to be able to read the previous session's value.
        g_playerHealth.store(playerHealth ? playerHealth : proto::kDefaultHealth);
        {
            // R3 T1: health_mode=direct|scaled|auto (default auto): direct is the 0.4.7 behaviour, the roll-back.
            const std::wstring hm = reg::Get(L"health_mode");
            std::string narrow;
            for (wchar_t ch : hm)
                narrow.push_back(ch < 128 ? (char)ch : '?');
            g_healthWish.store((uint8_t)sync::ParseHealthMode(narrow));
        }
        GLOGI("engine: health: Welcome.playerHealth %u -> the ped's maximum will be %d (100 + it); health_mode=%s (the probe at session "
              "start measures what GTA really holds)",
              (unsigned)playerHealth, sync::PedMaxHealth(playerHealth), sync::HealthModeName((sync::HealthMode)g_healthWish.load()));
        // peer_drive: anything but "place" (absent, empty, a typo) is the default "tasks".
        const std::wstring drive = reg::Get(L"peer_drive");
        g_peerDrive.store(drive == L"place" ? kDrivePlace : kDriveTasks);
        GLOGI("engine: remote peds driven by %s (peer_drive=%ls)",
              g_peerDrive.load() == kDrivePlace ? "PLACEMENT (stage 1: set onto the pose, no animation)"
                                                : "TASKS (locomotion controller)",
              drive.empty() ? L"<unset>" : drive.c_str());
        // W11 / R4 T1: loco_mode = anchor | straight | blend; unset or anything else = this build's baked variant (kBakedLocoMode).
        const std::wstring locoModeReg = reg::Get(L"loco_mode");
        const bool locoModeFromReg = locoModeReg == L"anchor" || locoModeReg == L"straight" || locoModeReg == L"blend";
        const int locoSel = locoModeFromReg ? (locoModeReg == L"anchor" ? kLocoSelAnchor : locoModeReg == L"blend" ? kLocoSelBlend : kLocoSelStraight)
                                            : kBakedLocoMode;
        g_locoAnchor.store(locoSel == kLocoSelAnchor);
        g_locoBlend.store(locoSel == kLocoSelBlend);
        GLOGI("engine: remote on-foot walking via %s (loco_mode, %s)",
              locoSel == kLocoSelBlend ? "BLEND: TASK_GO_STRAIGHT_TO_COORD at a far point as the gait animation only, the position carried by a velocity write on every frame "
                                         "(pose velocity + a capped correction; FiveM's ped blender); the rigid follow of the pose when the read-back shows the writes are not honoured"
              : locoSel == kLocoSelAnchor ? "TASK_GO_TO_ENTITY on an invisible anchor (falls back to straight while aiming, without the entity and after a lost task)"
                                          : "TASK_GO_STRAIGHT_TO_COORD at a far point",
              locoModeFromReg ? "REGISTRY override" : "build default");
        // T2: the position-error push and the degradation ladder (see LocoCfg). loco_push = nudge | velocity (anchor and straight only; blend has its own
        // drive); unset = the push that variant was tested with (anchor: nudge, straight: velocity).
        const std::wstring locoPushReg = reg::Get(L"loco_push");
        const bool locoPushFromReg = locoPushReg == L"nudge" || locoPushReg == L"velocity";
        g_locoPushNudge.store(locoPushFromReg ? locoPushReg == L"nudge" : locoSel == kLocoSelAnchor);
        g_locoSelfCheck.store(reg::Get(L"loco_selfcheck") != L"0");
        g_locoR3.store(reg::Get(L"loco_r3") != L"0");
        g_locoRepair.store(reg::Get(L"loco_repair") != L"0");
        g_locoGaitClip.store(reg::Get(L"loco_gait_clip") != L"0");
        GLOGI("engine: on-foot clip (0.4.17): loco_gait_clip=%s", g_locoGaitClip.load() ? "on (blend ratio = the sender's gait 1/2/3)" : "OFF (0.4.15 speed knob)");
        g_locoFace.store(reg::Get(L"loco_face") != L"0");
        GLOGI("engine: on-foot facing (0.4.29): loco_face=%s", g_locoFace.load()
                  ? "on (a walking Blend puppet's heading is written every frame toward the sender's; the 'R5: ... heading error' numbers are the proof)"
                  : "OFF (SET_PED_DESIRED_HEADING with the ped's own heading plus a step, as 0.4.28)");
        g_locoSwingWalk.store(reg::Get(L"loco_swing_walk") != L"0");
        GLOGI("engine: on-foot swing walk (0.5.7): loco_swing_walk=%s", g_locoSwingWalk.load()
                  ? "on (an upper-body / secondary melee clip seen playing no longer holds the gait task: the puppet walks off through the swing; full-body / plain clips and repairs stay held)"
                  : "OFF (every melee swing holds the gait task for the swing + 700 ms, as 0.5.6)");
        {
            // R5: the grace window after a task-changing Reset and the shorter Placed hold (Blend only)
            g_locoBlendGrace.store(reg::Get(L"loco_blend_grace") != L"0");
            const std::wstring placedReg = reg::Get(L"loco_placed_ms");
            long placedMs = -1;
            if (!placedReg.empty())
            {
                wchar_t* endp = nullptr;
                const long v = std::wcstol(placedReg.c_str(), &endp, 10);
                if (endp && *endp == 0 && v >= 0 && v <= 600000)
                    placedMs = v;
                else
                    GLOGW("engine: loco_placed_ms=\"%ls\" is not a number of milliseconds (0..600000): ignored, the shipped hold stays", placedReg.c_str());
            }
            g_locoPlacedMs.store((int)placedMs);
            const sync::LocoConfig rc = LocoCfg();
            GLOGI("engine: remote on-foot Blend grace (R5): loco_blend_grace=%s, loco_placed_ms=%s -> grace %.0f ms (place-once if the error is over %.1f m, chain cap %.0f ms), "
                  "probation %.0f ms, Placed hold after an explained fall %.0f ms (cap %.0f), after an unexplained one %.0f ms (cap %.0f), ladder hold %.0f ms, %d level changes%s",
                  g_locoBlendGrace.load() ? "on" : "OFF (round-4 Blend)", placedMs < 0 ? "unset" : std::to_string(placedMs).c_str(),
                  (double)rc.blendGraceMs, (double)rc.blendGracePlaceM, (double)rc.blendGraceMaxMs, (double)rc.blendProbationMs,
                  (double)rc.placedMs, (double)rc.placedDurMaxMs, (double)(rc.placedUnexplainedMs > 0.0f ? rc.placedUnexplainedMs : rc.placedMs),
                  (double)(rc.placedUnexplainedMaxMs > 0.0f ? rc.placedUnexplainedMaxMs : rc.placedDurMaxMs), (double)rc.levelHoldMs, (int)rc.maxLevelChanges,
                  g_locoBlend.load() ? "" : " (not Blend: no effect)");
        }
        GLOGI("engine: remote on-foot round-3 rules %s (loco_r3): quiet window + write verdict, re-issue spacing, ladder hold / cap; repairs of a puppet "
              "no task moves %s (loco_repair)", g_locoR3.load() ? "ON" : "OFF (round-2 controller)", g_locoRepair.load() ? "ON" : "OFF");
        GLOGI("engine: remote on-foot position error closed by %s (loco_push, %s), self-checks / degradation ladder %s (loco_selfcheck)",
              g_locoBlend.load() ? "the BLEND velocity (pose velocity + at most 2.5 m/s, never more than 3 m/s above it; a coordinate write only for a snap beyond 6 m or a teleport; loco_push is not used)"
              : g_locoPushNudge.load() ? "a coordinate NUDGE (SET_ENTITY_COORDS_NO_OFFSET, a few mm per frame)" : "SET_ENTITY_VELOCITY (0.4.6 push)",
              g_locoBlend.load() ? "loco_mode=blend" : locoPushFromReg ? "REGISTRY override" : "build default",
              g_locoSelfCheck.load() ? "ON (anchor -> straight -> blend-carried speed -> rigid follow, each step logged)" : "OFF");
        g_locoDegradeReset.store(true);
        // W10: name tags on by default; nametags=0 turns them off, nametags_los=<int> the LOS trace flags.
        g_nameTags.store(reg::Get(L"nametags") != L"0");
        {
            const std::wstring tr = reg::Get(L"nametags_los");
            long v = tr.empty() ? 17 : std::wcstol(tr.c_str(), nullptr, 10);
            if (v <= 0 || v > 65535)
                v = 17;
            g_nameTagTrace.store((int)v);
        }
        GLOGI("engine: name tags %s (nametags), line-of-sight trace %d (nametags_los)", g_nameTags.load() ? "on" : "off",
              g_nameTagTrace.load());
        // v4 switches, re-read at every Welcome like peer_drive.
        g_vehUseAngVel.store(reg::Get(L"veh_angvel") == L"1");
        g_vehAngBody.store(reg::Get(L"veh_angframe") == L"body");
        g_shakeFix.store(reg::Get(L"veh_shake_fix") != L"0");         // 0.4.22: the angular writes in the setter's own (body) frame (0 = the pre-0.4.22 world vectors)
        g_shakeWorldPinned.store(reg::Get(L"veh_angframe") == L"world");
        GLOGI("engine: vehicle shake (0.4.22): veh_shake_fix=%s, angular writes in the %s frame (SET_ENTITY_ANGULAR_VELOCITY takes the BODY frame, "
              "GET_ENTITY_ROTATION_VELOCITY returns the WORLD frame: the live read-backs of 2026-11-05 are R(q) * wrote); a 10 s line reports the high-frequency "
              "pitch / roll / vertical-speed jitter of the car we drive and of the remote copies' pose error, and the angular write read-backs",
              g_shakeFix.load() ? "on" : "OFF (pre-0.4.22 world-vector writes)", !g_shakeFix.load() ? (g_vehAngBody.load() ? "copies: body (veh_angframe), assists: world" : "world") :
              (g_shakeWorldPinned.load() ? "world (veh_angframe=world)" : "body, following the read-backs"));
        g_peerHits.store(reg::Get(L"peer_hits") != L"0");
        g_vehDriveByAimOnly.store(reg::Get(L"veh_driveby") == L"aim");
        g_vehDamageSync.store(reg::Get(L"veh_damage_sync") != L"0");     // R3 T4: the vehicle damage state (default on; 0 = the v6 behaviour)
        g_vehVisualGuard.store(reg::Get(L"veh_visual_guard") != L"0");   // R3 T4: the sanity layer of the tyre / window / handbrake reads (default on)
        g_impactSync.store(reg::Get(L"impact_sync") != L"0");   // T4: the vehicle impact handling (default on; 0 = the W9 contact grace only)
        g_nearClock.store(reg::Get(L"veh_near_clock") != L"0");     // R3 T5: the near-car clock + link lag (0 = round 2's lead)
        g_proxContact.store(reg::Get(L"veh_prox_contact") != L"0"); // R3 T5: the proximity contact path (0 = the flag / speed loss / kick triggers only)
        g_contactGate.store(reg::Get(L"veh_contact_gate") != L"0"); // R4 T2: a copy's contact counts only with a vehicle we control near (0 = any contact, round 3)
        g_accelExtrap.store(reg::Get(L"veh_accel_extrap") != L"0"); // R4 T2: acceleration-aware extrapolation of copies (0 = constant velocity, round 3)
        g_rideFree.store(reg::Get(L"veh_ride_free") != L"0");       // 0.4.30: copies keep their own ride height / tilt within tolerance (0 = 0.4.29)
        g_omegaGuard.store(reg::Get(L"veh_omega_guard") != L"0");   // 0.4.30: no derived omega across sources or from a standing pair (0 = 0.4.29)
        GLOGI("engine: vehicle omega guard (0.4.30): veh_omega_guard=%s", g_omegaGuard.load()
              ? "on (a copy's derived angular velocity never pairs two machines' poses and is 0 for a standing pair: parked copies hold their pose)"
              : "OFF (0.4.29: derived from any neighbour pair)");
        GLOGI("engine: vehicle ride freedom (0.4.30): veh_ride_free=%s (copies of wheeled ground vehicles, not airborne: vertical error up to %.2f m and "
              "pitch / roll up to %.0f deg left to the copy's own suspension, only the excess corrected)", g_rideFree.load() ? "on" : "OFF (0.4.29: full 3D correction)",
              (double)sync::kRideTolM, (double)sync::kTiltTolDeg);
        g_rideGate.store(reg::Get(L"veh_ride_gate") != L"0");       // 0.4.17: a car we only ride in is not "a vehicle we control" (0 = 0.4.15)
        g_rideLead.store(reg::Get(L"veh_ride_lead") != L"0");       // 0.4.17: no near-car lead on the car we ride in (0 = 0.4.15)
        GLOGI("engine: passenger ride (0.4.17): veh_ride_gate=%s veh_ride_lead=%s", g_rideGate.load() ? "on" : "OFF (0.4.15)",
              g_rideLead.load() ? "on" : "OFF (0.4.15)");
        ReadRadioOffSwitch();                                           // 0.4.19: no_radio (0 = the radio and its wheel as before)
        ReadNoclipSwitch();                                             // 0.4.22: noclip (0 = no Tab menu row, noclip cannot start)
        g_spawnWaitPacks.store(reg::Get(L"spawn_wait_packs") != L"0");  // 0.4.19: the reveal waits for the car packs (0 = collision only)
        g_spawnClearFx.store(reg::Get(L"spawn_clear_fx") != L"0");      // 0.4.19: clear timecycle modifier + post-fx at the reveal
        GLOGI("engine: spawn reveal (0.4.19): spawn_wait_packs=%s spawn_clear_fx=%s", g_spawnWaitPacks.load() ? "on" : "OFF (0.4.18)",
              g_spawnClearFx.load() ? "on" : "OFF (0.4.18)");
        g_hornSync.store(reg::Get(L"veh_horn_sync") != L"0");       // 0.4.19: a copy's horn follows the state (0 = a 400 ms horn per received state)
        g_hornHeld.store(reg::Get(L"veh_horn_held") != L"0");       // 0.4.19: mode "HELDDOWN" (0 = mode 0)
        g_hornStop.store(reg::Get(L"veh_horn_stop") != L"0");       // 0.4.19: the release call (0 = the hold runs out)
        g_lightsKeep.store(reg::Get(L"veh_lights_keep") != L"0");   // 0.4.19: the lights read-back and re-issue (0 = on change only)
        g_lightsBeamLit.store(reg::Get(L"veh_lights_beam_lit") != L"0");   // 0.4.30: a high beam read as lightsOn 0 / highbeamsOn 1 is lit
        g_sirenHorn.store(reg::Get(L"veh_siren_horn") != L"0");    // 0.4.30: siren on + E held sends the horn bit (0 = IS_HORN_ACTIVE only)
        GLOGI("engine: vehicle signals (0.4.30): veh_lights_beam_lit=%s; veh_siren_horn=%s", g_lightsBeamLit.load()
              ? "on (GET_VEHICLE_LIGHTS_STATE lightsOn OR highbeamsOn = lit: the HUD lamp, the headlights bit sent, the copies' keeper)"
              : "OFF (0.4.29: lightsOn alone; a high beam read as lightsOn 0 is off)",
              g_sirenHorn.load() ? "on (siren on + E held by the driver = the horn bit; the siren's second tone leaves IS_HORN_ACTIVE false)"
                                 : "OFF (0.4.29: IS_HORN_ACTIVE alone)");
        {
            const sync::HornConfig hc = HornConfigNow();
            GLOGI("engine: vehicle signals (0.4.19): veh_horn_sync=%s, veh_horn_held=%s, veh_horn_stop=%s, veh_lights_keep=%s; indicators: the 3889 "
                  "script API has no getter (GET_VEHICLE_INDICATOR_LIGHTS is in neither crossmap), so since 0.4.22 the controller's own key state "
                  "machine is what goes in the flags2 bits (veh_keys / veh_indicator_sync, below); wheel spin: a copy moves by SET_ENTITY_VELOCITY every frame (whether its wheels turn is UNPROVEN: 0.4.30 logs the brake / handbrake fields of moving copies; no "
                  "script read-back of a wheel's speed exists); steering: the CVehicle steering-angle field when veh_steer_field resolved it (hook: [vehfields]), else SET_VEHICLE_STEER_BIAS (0.4.29; it does not turn a script-driven copy)",
                  g_hornSync.load() ? "on (a rise starts the horn at once, renewed while held)" : "OFF (0.4.17: START_VEHICLE_HORN 400 ms per received state)",
                  g_hornHeld.load() ? "on (mode HELDDOWN)" : "OFF (mode 0)",
                  g_hornStop.load() ? "on (a 1 ms call at the release)" : "OFF (the hold runs out)",
                  g_lightsKeep.load() ? "on (GET_VEHICLE_LIGHTS_STATE read-back on near copies, re-issued on a confirmed difference)" : "OFF (0.4.17: on change only)");
            GLOGI("engine: vehicle signals (0.4.19): horn hold %d ms renewed every %.0f ms, let go after %.0f ms without a state; mode hash 0x%08x",
                  hc.holdMs, hc.renewMs, hc.staleMs, g_hornHeld.load() ? (unsigned)kHornModeHeldDown : 0u);
        }
        g_vcKeys.store(reg::Get(L"veh_keys") != L"0");                          // 0.4.22: the vehicle keys and their mechanics (0 = none of it)
        g_vcAutoCancel.store(reg::Get(L"veh_indicator_autocancel") != L"0");    // 0.4.22: a single indicator goes out after a completed turn
        g_vcIndSync.store(reg::Get(L"veh_indicator_sync") != L"0");             // 0.4.22: the indicator bits go on the wire and onto copies
        g_vcFuelUse.store(reg::Get(L"fuel_use") == L"1");                        // 0.4.22: an empty tank cuts / refuses the engine (default OFF: no pumps yet)
        g_vcReset.store(true);                                                  // the script tick forgets the cars it switched off (g_vc is its own)
        GLOGI("engine: vehicle controls (0.4.22): veh_keys=%s, veh_indicator_autocancel=%s, veh_indicator_sync=%s, fuel_use=%s; keys: Left / Right Arrow "
              "indicators, Down Arrow hazards, Y engine, B seatbelt (H = GTA's own headlights); GTA controls held disabled in a vehicle: 172-175 (the arrow "
              "keys' cellphone controls), 29 (B), 246 (Y)",
              g_vcKeys.load() ? "on" : "OFF (no key read, nothing issued or drawn)",
              g_vcAutoCancel.load() ? "on (45 degrees of turn, the wheel centred 0.4 s)" : "OFF",
              g_vcIndSync.load() ? "on (kVeh2IndicatorL / R sent and applied on copies)" : "OFF (the bits are neither sent nor applied)",
              g_vcFuelUse.load() ? "ON (an empty tank cuts the engine of our own car and it will not start)" : "off (the gauge only shows the level)");
        {
            const sync::IndFallbackMode im = sync::IndFallbackModeFrom(reg::Get(L"veh_ind_fallback").c_str());   // 0.4.27
            g_indFallbackMode.store((int)im);
            GLOGI("engine: indicator fallback (0.4.27): veh_ind_fallback=%s (our own blinking amber light, DRAW_LIGHT_WITH_RANGE every frame at the "
                  "indicator bones the game resolves, else the model box's corners, on %s); one 'engine: indicators: model' line per model at its first "
                  "indicator call names its bones and the calls' results",
                  sync::IndFallbackModeName(im), im == sync::IndFallbackMode::Off ? "no car" : (im == sync::IndFallbackMode::AllCars ? "every car" : "the cars a content pack carries"));
        }
        MapReadSwitches();                                           // 0.4.20: map_clear_blips / map_reveal
        MinimapReadSwitches();                                       // 0.4.22: minimap / minimap_bars
        {
            // 0.5.4 inv_world (default on; 0 = 0.5.3: the server's offer is not answered, weapons stay off the inventory)
            const bool on = reg::Get(L"inv_world") != L"0";
            g_invWorldOn.store(on);
            g_invWorldOffered.store(false);
            g_invWorldLive.store(false);
            g_invwReset.store(true);
            GLOGI("engine: inventory world (0.5.4): inv_world=%s", on ? "on (weapons as items, ground drops, trunks when the server offers them)"
                                                                       : "OFF (0.5.3: the offer is ignored, /gun gives locally)");
        }
        g_peerEnterTask.store(reg::Get(L"puppet_enter") == L"task");   // Sync D: the W6 door walk back (default: follow the pose)
        g_seatIntentOn.store(reg::Get(L"seat_intent") != L"0");        // T1 seats: 0 = the pre-v6 entry (walk + teleport-in; follow + seat warp)
        g_vehRearSeats.store(reg::Get(L"veh_rear_seats") != L"0");     // 0.4.22: F / G may pick the rear seats (0 = driver + front passenger only)
        g_vehLocks.store(reg::Get(L"veh_locks") != L"0");              // 0.4.22: the L key and the lock read-back on the copies (0 = neither)
        g_dsState = DoorsSeatsState();                                 // a new session: no pick waiting for a read-back, no lock answer pending, no keys
        LogDoorsSeatsSwitches();                                       // 0.4.22 (defined with the F / G entry)
        InvCReadSwitches();                                            // 0.5.4: outfit_items / inv_ped_view
        game::worldclock::ReadSwitch();                                // 0.5.7: world_clock
        game::characters::ReadSwitch();                                // 0.5.9: char_face
        {
            // R3 T3: cover_sync=task (default: verified TASK_PUT_PED_DIRECTLY_INTO_COVER, crouch-pose fallback) | pose (never a cover task) | off.
            const sync::CoverSyncMode cm = sync::ParseCoverSync(reg::Get(L"cover_sync"));
            g_coverSyncMode.store((int)cm);
            g_coverBreaker.Reset();
            GLOGI("engine: cover (R3 T3): cover_sync=%s (%s)", sync::CoverSyncName(cm),
                  cm == sync::CoverSyncMode::Task ? "a puppet is put into cover only when it is within 0.30 m of the sender's pose and the game confirms it with "
                                                    "IS_PED_IN_COVER; otherwise it stands / crouches on the sender's position"
                  : cm == sync::CoverSyncMode::Pose ? "no cover task, the crouch pose on the sender's position"
                                                     : "the cover flag is ignored");
        }
        {
            // Sync plan W3: hit_react off|bullet|ragdoll|both (anything else, the default: both),
            // peer_muzzle=0, peer_shoot=script (real fire is the default since the 0.4.3 fixes).
            const std::wstring react = reg::Get(L"hit_react");
            g_hitReact.store(react == L"off" ? kReactOff : react == L"bullet" ? kReactBullet
                             : react == L"ragdoll" ? kReactRagdoll : kReactBoth);
            g_peerMuzzle.store(reg::Get(L"peer_muzzle") != L"0");
            // W8 item 6: real fire is the default; peer_shoot=script keeps the old zero-damage script bullet. Either way the
            // AI damage modifiers go to 0 (TickAiDamage, script tick): a puppet's shot or punch hurts nobody on this machine.
            g_peerShootPed.store(reg::Get(L"peer_shoot") != L"script");
            g_aiDamageWanted.store(true);
            // 0c: the AI MELEE modifier stays untouched unless ai_melee_mod=1 (see g_aiMeleeMod).
            g_aiMeleeMod.store(reg::Get(L"ai_melee_mod") == L"1");
            {
                // R3 T6: how a remote swing is shown (HKCU\Software\GAGE-MP melee_anim): auto climbs a ladder by measured effect,
                // play = animations only, melee_task = the melee task only, off = no swing is shown.
                const std::wstring ma = reg::Get(L"melee_anim");
                g_meleeAnimWanted.store((int)(ma == L"play" ? sync::SwingMode::Play : ma == L"melee_task" ? sync::SwingMode::MeleeTask
                                                                                  : ma == L"off" ? sync::SwingMode::Off : sync::SwingMode::Auto));
                // R4 T3: melee_survey (cycles over the survey plan, default 1, 0 = off) and melee_follow (0 = no progress check)
                const std::wstring sv = reg::Get(L"melee_survey");
                int cycles = 0;                        // 0.4.30: off by default
                if (!sv.empty() && sv[0] >= L'0' && sv[0] <= L'9')
                    cycles = sv[0] - L'0';
                g_meleeSurveyWanted.store(cycles > 3 ? 3 : cycles);
                g_meleeFollowWanted.store(reg::Get(L"melee_follow") != L"0");
                g_meleeLoopWanted.store(reg::Get(L"melee_loop") == L"1");   // 0.4.30: off by default
                g_animPhaseFix.store(reg::Get(L"anim_phase_fix") != L"0");
                g_swingDurationMs = SwingDurationDefault();
                g_staggerClip.store(reg::Get(L"stagger_clip") != L"0");
                GLOGI("engine: punches (0.4.30): anim_phase_fix=%s, stagger_clip=%s; swing duration the whole clip, melee_survey default 0, melee_loop default off",
                      g_animPhaseFix.load() ? "on (TASK_PLAY_ANIM start phase 0)" : "OFF (start phase 1.0, 0.4.29)",
                      g_staggerClip.load() ? "on (a melee stagger is the reaction@shove clip, never a ragdoll)" : "OFF (the W8 ragdoll stagger)");
                GLOGI("engine: melee swing (R5): melee_loop=%s (%s); melee_survey=%d, melee_follow=%s", g_meleeLoopWanted.load() ? "ON" : "off",
                      g_meleeLoopWanted.load() ? "plan: looped clip flags 49 ended by STOP_ANIM_TASK, plain flags 0, then the round-4 attempts; melee task may borrow an anchor ped"
                                               : "the round-4 plan and ladder, the melee task only with a locomotion anchor", g_meleeSurveyWanted.load(),
                      g_meleeFollowWanted.load() ? "on" : "off");
            }
            GLOGI("engine: combat client: hit reactions %s (a melee blow: a short stagger), muzzle flash %s, remote shots via %s "
                  "(hit_react / peer_muzzle / peer_shoot); AI weapon damage modifier 0.0 once the tick sets it, AI melee modifier %s "
                  "(ai_melee_mod; off: puppets only play a swing animation, and the modifier zeroed our own fists on them)",
                  react == L"off" ? "OFF" : react == L"bullet" ? "bullet flinch only" : react == L"ragdoll" ? "knockdown / stagger only"
                                                                                                           : "flinch + knockdown + stagger",
                  g_peerMuzzle.load() ? "ON" : "OFF",
                  g_peerShootPed.load() ? "SET_PED_SHOOTS_AT_COORD (real shots; the clear-line check is lifted once the AI "
                                          "damage modifiers are set)"
                                        : "the zero-damage script bullet (peer_shoot=script)",
                  g_aiMeleeMod.load() ? "0.0 (ai_melee_mod=1)" : "left alone");
        }
        g_v4ResetEnded.store(false);
        g_v4Reset.store(true);
        GLOGI("engine: v4: remote vehicles take angular velocity %s, applied in the %s frame; local hit detection "
              "on remote peds %s (veh_angvel / veh_angframe / peer_hits); seated drive-by via %s (veh_driveby)",
              g_vehUseAngVel.load() ? "from the sender" : "derived from rotations",
              g_vehAngBody.load() ? "body" : "world", g_peerHits.load() ? "ON" : "OFF (peds invincible)",
              g_vehDriveByAimOnly.load() ? "the plain aim task" : "TASK_DRIVE_BY");
        const sync::SmootherConfig cfg = sync::SmootherConfig::ForTickHz(tickHz);
        {
            PeersLock lk(g_peersMtx, false);
            g_sync = sync::RemoteSync(cfg);
            g_sync.SetLocalId(myId);
            g_syncLive = true;
            // A new session restarts the server's tick counter, so its vehicle snapshots carry LOW stamps that
            // the old session's interpolators would drop as Stale for as long as the old session had run
            // (W4 review, fix 2). Every entry gets a fresh interpolator session (which empties its buffer) and
            // forgets its state; the MAP stays: clearing it would orphan the g_vehs entities the tick still has.
            // The render clock of the old timeline is meaningless too: the new RemoteSync above brought a fresh
            // NetClock (one clock for peds and cars), and each car's class extra starts over.
            ++g_vehSession;
            for (auto& kv : g_vehNet)
            {
                kv.second.interp.SetSession(g_vehSession);
                kv.second.interp.Clear();
                kv.second.extra.Reset();
                kv.second.haveState = false;
                ++kv.second.ctrlSeq;
                ++kv.second.driveResetSeq;             // a new source of motion: the drive controller snaps onto its first sample
            }
        }
        GLOGI("engine: remote-player sync for %u Hz (smoother tick %.0f Hz, one render clock for peds and cars, shared delay "
              "%.0f..%.0f ms, start %.0f ms); own id %u", (unsigned)tickHz, cfg.tickHz, cfg.minDelayMs, cfg.maxDelayMs,
              cfg.initialDelayMs, (unsigned)myId);
    }

    // The join worker never reads what the game thread owns (ped handles, requested models), so it cannot tell
    // whether a peer has a ped: a departure or a despawn is recorded unconditionally and the script tick, which
    // knows, deletes what exists (and drops an entry that never had a ped, which costs it nothing).
    void OnJoined(uint16_t id, const std::string& name)
    {
        PeersLock lk(g_peersMtx, false);
        PeerNet& pe = g_peerNet[id];
        if (pe.gone)
        {
            // The id is reused (a new player, or the same one after a reconnect) before the script
            // tick removed the old entry: none of the old player's look or ped carries over (SyncNet
            // resets the tick's side when it sees the epoch change).
            pe.hasLook = false;
            pe.hasFace = false;                        // 0.5.9: nor its character's face
            pe.despawn = true;
            pe.killedAtMs = 0;                         // nor its death or emote
            pe.emoteKind = 0;
            ++pe.emoteSeq;
            ++pe.epoch;
        }
        pe.name = name;
        pe.gone = false;
    }

    void OnLeft(uint16_t id)
    {
        PeersLock lk(g_peersMtx, false);
        g_sync.Remove(id);
        auto it = g_peerNet.find(id);
        if (it != g_peerNet.end())
            it->second.gone = true;                    // the script tick deletes the ped, releases the model, drops both entries
    }

    // PlayerJoined seen and no PlayerLeft since (caller holds g_peersMtx). g_peerNet entries are made
    // only by OnJoined. WorldState / WorldCoarse travel on the unreliable state channel and
    // PlayerJoined / PlayerLeft / PeerLook on the reliable one; ENet orders each channel on its own,
    // so a state packet can arrive after a PlayerLeft (or before a PlayerJoined). Such entries are
    // ignored instead of resurrecting a nameless ghost peer.
    bool IsJoinedPeer(uint16_t id)
    {
        auto it = g_peerNet.find(id);
        return it != g_peerNet.end() && !it->second.gone;
    }

    // The session is over: every remote player is gone with it, and the next
    // connection must re-enter (swap model, teleport to spawn) from scratch.
    void OnEnded()
    {
        // v8: the next session's server decides again; nothing asked of this one is answered any more.
        game::Overlay::SetIsAdmin(false);
        {
            std::lock_guard<std::mutex> lk(g_cmdMtx);
            g_adminParked.clear();
        }
        game::worldclock::OnEnded();       // 0.5.7: the next script tick gives the clock and the weather back to the game
        game::characters::OnEnded();       // 0.5.9: no roster until the next server sends one
        game::Overlay::ClearInventory();   // 0.6: the inventory page shows "unavailable" until the next server sends one
        OutfitClear();                     // 0.5.4: no server inventory, no opinion about the clothes (the next entry dresses the ped in full)
        g_satietyAdd.store(0);
        GLOGI("engine: inventory (0.6) this session: %u states received, %u ops sent, %u not sent, %u food used", g_invStates.exchange(0),
              g_invOpsSent.exchange(0), g_invOpsUnsent.exchange(0), g_invUsed.exchange(0));
        if (g_invWorldOffered.exchange(false) || g_invwSnaps.load())
            GLOGI("engine: inventory world (0.5.4) this session: %s, %u snapshots, %u nearby updates, %u reports (%u not sent); ped: %u gives, "
                  "%u removes, %u ammo sets, %u selects, %u restores", g_invWorldLive.load() ? "answered" : "NOT answered", g_invwSnaps.exchange(0),
                  g_invwNearby.exchange(0), g_invwReports.exchange(0), g_invwReportsUnsent.exchange(0), g_invwGives.exchange(0),
                  g_invwRemoves.exchange(0), g_invwAmmoSets.exchange(0), g_invwSelects.exchange(0), g_invwRestores.exchange(0));
        g_invWorldLive.store(false);
        g_invwReset.store(true);
        {
            std::lock_guard<std::mutex> lk(g_invwMtx);
            g_invwSnapFresh = false;
            g_invwSnap = proto::InvSnap();
        }
        g_entry.store(Entry::Waiting);
        g_entryStarted.store(0);
        g_entryReadyAt.store(0);
        g_loaderArmed.store(0);
        g_loaderPedAt.store(0);
        g_v4ResetEnded.store(true);                    // R3 T1: this reset is a session end
        g_v4Reset.store(true);                         // link, vitals, hits, emotes start over
        g_aiDamageWanted.store(false);                 // the script tick resets the AI damage modifiers
        {
            std::lock_guard<std::mutex> ek(g_evMtx);
            g_evQueue.Clear();
            g_inbox.Clear();
            g_respawnSlotFull = false;
        }
        PeersLock lk(g_peersMtx, false);
        g_sync.Clear();
        g_syncLive = false;                            // the next Welcome sets it up again
        for (auto& kv : g_vehNet)
            kv.second.removed = true;                  // the script tick deletes the entities
        for (auto& kv : g_peerNet)
            kv.second.gone = true;                     // the script tick deletes / releases and drops the entries
    }

    // Every entry goes into the smoother (with its ageMs), stamped on arrival; the script tick
    // samples it. The server leaves our own entry out of a WorldState; the id check here (and
    // RemoteSync::SetLocalId) is a second guard against ever puppeteering a ped as ourselves.
    // An id in ws.left is out of our relevance: forgotten by the smoother, its ped despawned.
    void OnWorld(const proto::WorldState& ws)
    {
        const uint16_t me = join::MyId();
        // Stamped BEFORE the lock: the time the packet arrived is not the time the worker got the lock, and the
        // script tick's critical section is short but not nothing. The smoother tolerates a stamp a few
        // microseconds behind the tick's last Sample (a sample that steps the clock back is placed by its own
        // age, repeats are recognised by value).
        const double nowMs = NowMs();
        PeersLock lk(g_peersMtx, false);
        if (!g_syncLive)
            return;                                    // no Welcome for this session yet: nothing to time it by
        g_sync.OnFrame(ws.tick, nowMs);                // the connection's one clock hears every frame, entries or not
        for (const proto::WorldEntry& e : ws.players)
        {
            if (e.id == me || e.id >= proto::kMaxPlayers || !IsJoinedPeer(e.id))
                continue;                              // ourselves, a bad id, or not (or no longer) joined
            g_sync.OnPlayerState(e.id, ws.tick, e.k, nowMs, (double)e.ageMs, e.ped);
            g_peerNet[e.id].despawn = false;           // back in relevance: keep or respawn its ped
        }
        for (uint16_t id : ws.left)
        {
            g_sync.Remove(id);
            auto it = g_peerNet.find(id);
            if (it != g_peerNet.end())
                it->second.despawn = true;             // the script tick deletes the ped (if it has one)
        }
    }

    // Another player's look (PeerLook, ValidLook already checked by the session). Stored; the
    // script tick puts it on the ped (respawning it when the model differs).
    void OnLook(const proto::PeerLook& pl)
    {
        if (pl.id >= proto::kMaxPlayers || pl.id == join::MyId())
            return;
        PeersLock lk(g_peersMtx, false);
        if (!IsJoinedPeer(pl.id))
            return;                                    // PeerLook follows its PlayerJoined on the same reliable channel
        PeerNet& pe = g_peerNet[pl.id];
        if (pe.hasLook && pe.look == pl.look)
            return;
        pe.look = pl.look;
        pe.hasLook = true;
        ++pe.lookSeq;                                  // SyncNet: lookDirty, and a new model gets another try
    }

    // 0.5.9: another player's character face (PeerFace). Decoded here; a blob that does not decode is dropped with one line per peer
    // and session (the server only sends what it validated, so that is a version skew). The script tick puts it on the ped.
    void OnPeerFace(const proto::PeerFace& pf)
    {
        if (pf.id >= proto::kMaxPlayers || pf.id == join::MyId())
            return;
        sync::chr::Appearance a;
        const bool ok = sync::chr::DecodeLook(pf.blob, a);
        PeersLock lk(g_peersMtx, false);
        if (!IsJoinedPeer(pf.id))
            return;                                    // PeerFace follows its PlayerJoined on the same reliable channel
        PeerNet& pe = g_peerNet[pf.id];
        if (!ok)
        {
            if (!pe.faceBadLogged)
                GLOGW("engine: character face (0.5.9): peer id=%u (%s): a face of %u bytes does not decode; ignored", (unsigned)pf.id,
                      pe.name.c_str(), (unsigned)pf.blob.size());
            pe.faceBadLogged = true;
            return;
        }
        if (pe.hasFace && pe.face == a)
            return;
        pe.face = a;
        pe.hasFace = true;
        ++pe.faceSeq;
    }

    // ---- protocol v4 sinks (join worker) ----------------------------------------------------

    // `important`: a vehicle answer (evicts a kill-feed line at the cap); the feed itself may be refused.
    void PushEvent(NetEvent&& e, bool important)
    {
        std::lock_guard<std::mutex> lk(g_evMtx);
        g_evQueue.Push(std::move(e), important);
    }

    // VehicleWorld: removals first, then presence, then the full states (protocol section 4.14).
    // Snapshots go into each vehicle's interpolator on the server timeline (tick * tickMs) unless we
    // drive it ourselves. No natives, no logging: the script tick does both.
    void OnVehicles(const proto::VehicleWorld& vw)
    {
        g_vehWorldsIn.fetch_add(1, std::memory_order_relaxed);
        const uint16_t me = join::MyId();
        const double nowMs = NowMs();                  // before the lock, like OnWorld's stamp
        PeersLock lk(g_peersMtx, false);
        if (!g_syncLive)
            return;
        // The connection's one clock hears this frame too (a VehicleWorld is a frame of the same server timeline as a
        // WorldState), and the cars' snapshots are stamped on ITS timeline: the frame's tick time minus the age.
        g_sync.OnFrame(vw.tick, nowMs);
        sync::NetClock& clock = g_sync.Clock();
        const double tickMs = clock.TickMs();
        const int64_t frameTick = clock.Unwrap(vw.tick);

        for (uint16_t id : vw.removed)
        {
            auto it = g_vehNet.find(id);
            if (it != g_vehNet.end())
                it->second.removed = true;
        }
        for (auto& kv : g_vehNet)
            kv.second.present = std::binary_search(vw.present.begin(), vw.present.end(), kv.first);
        for (uint16_t id : vw.present)
        {
            VehNet& v = g_vehNet[id];
            v.present = true;
            v.removed = false;
            v.interp.SetSession(g_vehSession);         // an entry made after the Welcome joins its session
        }
        for (const proto::VehicleState& entry : vw.vehicles)
        {
            VehNet& v = g_vehNet[entry.id];
            v.interp.SetSession(g_vehSession);
            proto::VehicleState st = entry;
            if (entry.motionOnly)
            {
                // v5 motion-only entry: just the motion (with its own ageMs) and the driver flags; the statics
                // (model, health, fuel, colours, seats, ctrl, ownerId, flags2, doors) stay what the last full state
                // said: a change in any of them is sent as a full entry (protocol.h VehicleWorld), so the overlay
                // never hides one. Without a full state there is nothing to lay it on (its full entry was lost):
                // the next full entry, at most a second away, starts the vehicle.
                if (!v.haveState)
                    continue;
                st = v.st;
                st.k = entry.k;
                st.rot = entry.rot;
                st.avx = entry.avx;
                st.avy = entry.avy;
                st.avz = entry.avz;
                st.steer = entry.steer;
                st.flags = entry.flags;
                st.ageMs = entry.ageMs;
            }
            // A hand-over (the controlling client changed): the old controller's snapshots do not continue into
            // the new one's. Empty the buffer here (the interpolator is this side's) and count it, so the tick
            // resets the drive controller (the new controller starts from where its car is: nobody snaps it).
            if (v.haveState && v.st.ctrl != st.ctrl)
            {
                // Sync D: only a hand-over to or from US empties the buffer and snaps the drive controller; between two other
                // controllers the car is the same car on the same road (sync::DecideHandover): the buffer stays, the
                // controller's own snap rules judge the new samples, and the car is pulled over the small step instead of
                // being popped a whole render delay ahead.
                const sync::HandoverAction ha = sync::DecideHandover(v.st.ctrl, st.ctrl, me);
                if (ha.clearInterp)
                    v.interp.Clear();
                if (ha.resetDrive)
                    ++v.driveResetSeq;
                g_syncDHandovers[ha.resetDrive ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
                ++v.ctrlSeq;
            }
            v.st = st;
            v.haveState = true;
            ++v.stVersion;
            if (st.ctrl == me)
                continue;                              // we control this vehicle: its motion is ours, not a snapshot
            sync::VehicleSnapshot s;
            // Snapshot time = the frame's tick time minus the sample's age (the controller's sample reached the
            // server ageMs before this frame was built), so a car shows the instant it was sampled (W6).
            s.tMs = sync::SnapshotStampMs(frameTick, tickMs, st.ageMs);
            s.pos = { st.k.x, st.k.y, st.k.z };
            s.vel = { st.k.vx, st.k.vy, st.k.vz };
            s.rot = { st.rot.x, st.rot.y, st.rot.z, st.rot.w };
            s.angVel = { st.avx, st.avy, st.avz };
            s.hasAngVel = g_vehUseAngVel.load(std::memory_order_relaxed);
            s.steering = st.steer;
            s.src = st.ctrl;                           // 0.4.30: omega is derived within one controller's stream only
            // Only a sample that was new information asks the shared clock for delay (as for peds).
            const sync::VehPush pushed = v.interp.Push(s);
            if (pushed == sync::VehPush::Accepted || pushed == sync::VehPush::Teleported)
                clock.NoteAge((double)st.ageMs);
        }
    }

    void OnSeatNotice(const proto::SeatNotice& n)
    {
        NetEvent e;
        e.kind = NetEvent::SeatNote;
        e.seat = n;
        PushEvent(std::move(e), true);                 // reliable and ordered with the VehicleResults: never the one dropped
    }

    // R3 T5: the round trip the server measures to each vehicle controller (join worker); the near-car lead reads it under g_linkMtx.
    void OnLinkInfo(const proto::LinkInfo& m)
    {
        std::lock_guard<std::mutex> lk(g_linkMtx);
        g_linkRtt.clear();
        for (const proto::LinkEntry& e : m.e)
            g_linkRtt[e.id] = e.rttMs;
        g_linkInfoIn.fetch_add(1, std::memory_order_relaxed);
    }

    // T4: the server's resolution of a contact of a vehicle we control; queued for the script tick (TickVehicles applies it).
    void OnVehicleImpact(const proto::VehicleImpact& m)
    {
        std::lock_guard<std::mutex> lk(g_impactMtx);
        if (g_impactQueue.size() >= kMaxImpactQueue)
        {
            g_impactQueue.erase(g_impactQueue.begin());
            ++g_t4.dropped;
        }
        g_impactQueue.push_back(m);
    }

    void OnVehicleResult(const proto::VehicleResult& r)
    {
        NetEvent e;
        e.kind = NetEvent::VehResult;
        e.veh = r;
        PushEvent(std::move(e), true);
    }

    // Damage about us goes into the inbox, which folds any number of them into one record (never
    // dropped, whatever the queue holds); the damage we dealt is only a counter.
    void OnDamage(const proto::Damage& d)
    {
        const uint16_t me = join::MyId();
        if (d.victimId == me)
        {
            std::lock_guard<std::mutex> lk(g_evMtx);
            g_inbox.OnDamage(d.health, d.armor, d.damage, d.attackerId == me, d.killed, d.attackerId, d.weapon, d.bone, d.flags);
        }
        else
        {
            const sync::DamageAbout about = sync::ClassifyDamage(me, d.victimId, d.attackerId);
            if (about == sync::DamageAbout::MyHit)
            {
                g_hitDamageConfirmed.fetch_add(d.damage, std::memory_order_relaxed);
                g_hitConfirmCount.fetch_add(1, std::memory_order_relaxed);
                if (d.flags & proto::kDmgMelee)         // R3 T6: a melee blow of ours the SERVER counted (it judges swings itself now)
                {
                    g_meleeConfirmCount.fetch_add(1, std::memory_order_relaxed);
                    g_meleeConfirmDamage.fetch_add(d.damage, std::memory_order_relaxed);
                    if (d.killed)
                        g_meleeConfirmKills.fetch_add(1, std::memory_order_relaxed);
                }
            }
            // Sync plan W3 / W6: the server confirms a hit of ours on a puppet, and (protocol v5) tells every relevant
            // player about the damage between two others: the victim's puppet flinches / falls on our screen the same
            // way. Queued for the script tick (natives are not callable here) with the damage flags and the attacker;
            // a killing blow is the death display's, and a self-inflicted or server hit has nobody to react at.
            if (sync::ReactsPuppet(about, d.killed, d.damage, d.victimId, d.attackerId))
            {
                PeersLock lk(g_peersMtx, false);
                auto it = g_peerNet.find(d.victimId);
                if (it != g_peerNet.end() && !it->second.gone)
                {
                    PeerNet& pn = it->second;
                    pn.reactDamage += d.damage;
                    pn.reactFlags = (uint8_t)(pn.reactFlags | d.flags);
                    pn.reactBone = d.bone;
                    pn.reactAttacker = d.attackerId;
                }
            }
        }
    }

    void OnKilled(const proto::Killed& k)
    {
        {
            PeersLock lk(g_peersMtx, false);
            auto it = g_peerNet.find(k.victimId);
            if (it != g_peerNet.end() && !it->second.gone)
            {
                // Shown dead before its kPfDead pose arrives, but not before the puppet's own pose has reached
                // the shot: the display waits for this peer's current interpolation delay (sync plan W3).
                const sync::PlayerSmoother* sm = g_sync.Find(k.victimId);
                it->second.killedAtMs = sync::DeathDisplayAtMs(NowMs(), sm ? sm->DelayMs() : 0.0);
            }
        }
        if (k.victimId == join::MyId())
        {
            std::lock_guard<std::mutex> lk(g_evMtx);
            g_inbox.OnKilled();                        // our death is never queued behind a flood
        }
        NetEvent e;
        e.kind = NetEvent::Killed;
        e.killed = k;
        PushEvent(std::move(e), false);                // the feed line is the droppable part
    }

    // Latest wins: one pending RespawnOk is all the tick can use. A Damage still in the inbox is older
    // than it (the respawn restores the values), so it is dropped with it.
    void OnRespawnOk(const proto::RespawnOk& r)
    {
        std::lock_guard<std::mutex> lk(g_evMtx);
        g_respawnSlot = r;
        g_respawnSlotFull = true;
        g_inbox.Clear();
    }

    // Emotes: the newest event per player wins; the script tick starts it once the ped exists.
    void StoreEmote(uint16_t id, int kind, const proto::Anim* a, const std::string* scenario)
    {
        if (id >= proto::kMaxPlayers || id == join::MyId())
            return;
        g_emotesIn.fetch_add(1, std::memory_order_relaxed);
        const double nowMs = NowMs();
        PeersLock lk(g_peersMtx, false);
        if (!IsJoinedPeer(id))
            return;
        PeerNet& pe = g_peerNet[id];
        pe.emoteKind = kind;
        if (a)
            pe.emoteIn = *a;
        if (scenario)
            pe.scenarioIn = *scenario;
        pe.emoteAtMs = nowMs;
        ++pe.emoteSeq;
    }
    void OnPeerAnim(const proto::PeerAnim& m) { StoreEmote(m.id, 1, &m.anim, nullptr); }
    void OnPeerScenario(const proto::PeerScenario& m) { StoreEmote(m.id, 2, nullptr, &m.name); }
    void OnPeerAnimStop(const proto::PeerAnimStop& m) { StoreEmote(m.id, 0, nullptr, nullptr); }

    void OnChat(uint16_t fromId, const std::string& text)
    {
        std::string who; // empty for the server
        if (fromId != proto::kServerId)
        {
            if (fromId == join::MyId())
                who = join::SanitizeName(g_cfg.player);
            else
            {
                PeersLock lk(g_peersMtx, false);
                auto it = g_peerNet.find(fromId);
                who = it != g_peerNet.end() ? it->second.name : "player " + std::to_string(fromId);
            }
        }
        GLOGI("engine: chat %s%s%s", who.c_str(), who.empty() ? "" : ": ", text.c_str());
        // The overlay's chat log (an empty name is a server line).
        game::Overlay::PushChatLine(who.c_str(), text.c_str(), fromId != proto::kServerId && fromId == join::MyId());
        // The in-game UI reads {name, text} (apps/ingame-ui/js/main.js); both come from other
        // players, so they are escaped.
        const std::string out = "{\"name\":\"" + join::JsonEscape(who) + "\",\"text\":\"" + join::JsonEscape(text) + "\"}";
        GageMP_Dispatch("chat:push", out.c_str());
    }

    // ---- server entry ------------------------------------------------------
    // Once joined and in the world (script tick only): the multiplayer character,
    // at the server spawn, police off. The protocol carries no spawn point yet, so
    // every player starts at Legion Square.
    const uint32_t kFreemodeMale = 0x705E61F2;  // mp_m_freemode_01
    const float kSpawn[4] = { 195.2f, -933.8f, 30.7f, 140.0f };  // Legion Square x, y, z, heading

    // The player's look: a plain guy in the GAGE-MP palette (launcher-ui styles/tokens.css: ground
    // #07080a, ink #f2f1ee, blue #2d6eb9) -- a blue tee, ground-black trousers, ink-white shoes.
    // Every slot is a base-game mp_m_freemode_01 item (global drawable ids, which DLC only appends
    // to), applied by DressPlayer after each successful SetPlayerModel(kFreemodeMale). Change the
    // look HERE only. Sources: item names root-cause/v-clothingnames male_*.json (GXT U_FMM_8_10,
    // L_FMM_10_0, F_FMM_8_0, H_FMM_1_0); arms root-cause/v-besttorso (the "Tee" tops pair with
    // torso 8/0); colours and arm shapes checked on the Colbss/FiveM-ClothingData base renders
    // (images/mp_m_freemode_01/base/D_<component>_<drawable>_<texture>.webp). Not yet seen in GTA:
    // ApplyPedOutfit validates each slot at run time and an invalid one keeps the default for that
    // component. The tee carries the GAGE-MP mark: brand_apparel.cpp swaps the texture of exactly this
    // item (top 11/8/10, jbib_diff_008_k_uni) for a branded copy at draw time on every GAGE client, so
    // the look stays plain indices on the wire. Change the top and brand::kTop* (brand_apparel.h) together.
    const game::PedHeadBlend kPlayerHead = {
        0, 0, 0,                  // face shape: the first male parent (IDs start at male non-DLC 0)
        0, 0, 0,                  // skin tone: the same parent
        0.0f, 0.0f, 0.0f          // mix 0.0 = all first parent
    };
    const game::PedComponent kPlayerOutfit[] = {
        {  0,  0,  0, "head: default face" },
        {  1,  0,  0, "mask: none" },
        {  2,  1,  0, "hair: Buzzcut" },                        // H_FMM_1; hair tint left at the game default
        {  3,  8,  0, "arms: forearms (pair with Tee tops)" },  // v-besttorso: Tee tops -> torso 8/0
        {  4, 10,  0, "legs: Black Pants" },                    // L_FMM_10_0, near the ground #07080a
        {  5,  0,  0, "bag: none" },
        {  6,  8,  0, "shoes: All-White Athletic Shoes" },      // F_FMM_8_0, the ink #f2f1ee
        {  7,  0,  0, "neck: No Tie" },
        {  8, 15,  0, "undershirt: none" },                     // 15 = empty mesh: nothing under the tee
        {  9,  0,  0, "armor: none" },
        { 10,  0,  0, "decal: none" },
        { 11,  8, 10, "top: Sky Blue Tee" },                    // U_FMM_8_10: raglan tee, blue body, grey sleeves
    };

    // The ped maximum health is 100 + the server playerHealth (Welcome.playerHealth, protocol v5): the death line
    // is 100, so a ped left at the game default of 200 would hold a server value of 150+ only after the vitals tracker
    // clamped it to 100. Set at the entry, on every respawn, after a model swap and whenever the vitals units start
    // over. Script tick.
    // R3 T1: live 0.4.7 showed SET_ENTITY_MAX_HEALTH reading back 300 while the player ped's HEALTH never rose above 200, so this
    // is now a PROBE: HealthController writes SET_ENTITY_MAX_HEALTH, SET_ENTITY_HEALTH, SET_PED_MAX_HEALTH, SET_ENTITY_HEALTH,
    // reads the real health after each, measures the ceiling, chooses DIRECT or SCALED and tells the tracker; one line names every
    // step, and the +1 frame / +1 s read-backs follow from TickVitals (HealthController::Pre). `why` names the caller; `beforeVitals`
    // is true from ResetV4Units, which runs ahead of this tick's TickVitals.
    struct HealthPort
    {
        int32_t ped;
        bool SetEntityMax(int m) { return g_world->SetEntityMaxHealth(ped, m); }
        bool SetPedMax(int m) { return g_world->SetPedMaxHealth(ped, m); }
        bool SetHealth(int h) { return g_world->SetEntityHealth(ped, h); }
        int  GetHealth() { return g_world->GetEntityHealth(ped); }
        int  GetMax() { return g_world->GetEntityMaxHealth(ped); }
    };

    void DrainHealthLog()
    {
        sync::HealthController::Line l;
        while (g_health.TakeLine(l))
        {
            if (l.warn)
                GLOGW("%s", l.text.c_str());
            else
                GLOGI("%s", l.text.c_str());
        }
    }

    void ApplyServerMaxHealth(int32_t ped, const char* why = "?", bool beforeVitals = false)
    {
        if (!ped)
            return;
        g_health.wish = (sync::HealthMode)g_healthWish.load();
        HealthPort port{ped};
        g_health.Begin(port, ped, g_playerHealth.load(), g_vitals, g_healthTick + (beforeVitals ? 1u : 0u), NowMs(), why);
        DrainHealthLog();
    }

    // Dress the freemode ped GTA just gave the player (script tick only). Called right after every
    // SetPlayerModel(kFreemodeMale) that took: the server entry and the default-ped guard.
    void DressPlayer(const char* when)
    {
        const int32_t ped = g_world->LocalPlayer();
        if (!ped)
            return;
        // Only a ped that is verifiably the freemode male takes this look: the head blend and the
        // indices mean nothing on any other model.
        const uint32_t model = g_world->GetEntityModel(ped);
        if (model != kFreemodeMale)
        {
            static bool loggedWrongModel = false;
            if (!loggedWrongModel)
            {
                loggedWrongModel = true;
                GLOGW("engine: outfit (%s) skipped: local ped model is 0x%08x, not freemode 0x%08x",
                      when, (unsigned)model, (unsigned)kFreemodeMale);
            }
            return;
        }
        const bool dressed = g_world->ApplyPedOutfit(ped, kPlayerHead, kPlayerOutfit,
                                                     sizeof(kPlayerOutfit) / sizeof(kPlayerOutfit[0]));
        static int logged = 0;
        if (logged < 10)
        {
            ++logged;
            GLOGI("engine: outfit (%s): %s", when, dressed ? "applied" : "not applied (see the hook log line above)");
        }
        if (dressed)
        {
            // The branded tee only exists for the one stock item brand_apparel.cpp fingerprints: say so loudly
            // if someone changes the outfit's top without changing the brand constants.
            for (const game::PedComponent& c : kPlayerOutfit)
            {
                if (c.component != game::brand::kTopComponent)
                    continue;
                if (c.drawable != game::brand::kTopDrawable || c.texture != game::brand::kTopTexture)
                {
                    static bool loggedMismatch = false;
                    if (!loggedMismatch)
                    {
                        loggedMismatch = true;
                        GLOGW("brand apparel: the outfit's top is %d/%d/%d but the branded tee is %d/%d/%d (component/drawable/texture): no logo until they match",
                              c.component, c.drawable, c.texture, game::brand::kTopComponent, game::brand::kTopDrawable,
                              game::brand::kTopTexture);
                    }
                }
                else
                    game::brand::NoteDressed(when);
            }
            g_outfitRecheck.store(true);   // 0.5.4 outfit_items: the full outfit is back on; the worn items decide again at once
            game::characters::NoteRedressed();   // 0.5.9: the fixed head is back on; the active character's face goes on again
        }
    }

    // ===== T5 ambient: no ambient NPCs and no ambient traffic for anyone (apps/engine/ambient.h) =====
    // The suppression used to sit behind EnterServer() (join::IsConnected() and the entry Done), so it never ran
    // before a session, during join retries, after "session ended", or while the script tick was not live. It now
    // runs from the top of GameWork, every tick, whatever the connection state (TickAmbient). The remote players'
    // puppets, the server's cars and everything else the engine itself made are on the exemption list
    // (AmbientGatherExempt): any NEW entity the engine creates must be added there, or a sweep would take it.
    ambient::Scheduler g_ambSched;
    ambient::ExemptSet g_ambExempt;
    ambient::Report g_ambRep;                      // this window's numbers (reset at each report)
    ambient::Tally g_ambTallySeen;                 // World::ambientTally at the previous report
    double g_ambReportMs = 0;                      // NowMs of the last `engine: ambient` line (0 = none yet)
    bool g_ambEverConnected = false;
    int g_ambLinkSeen = -1;
    std::once_flag g_ambConfigured;

    // The handles no clean-up may touch. Script tick only (g_peers / g_vehs are its own). Complete only with a
    // local ped: with no ped nothing is provable and MayDelete() refuses everything.
    void AmbientGatherExempt(int32_t lp)
    {
        g_ambExempt.Clear();
        g_ambExempt.Add(lp);
        g_ambExempt.Add(g_localPed.load());
        for (const auto& kv : g_peers)
        {
            g_ambExempt.Add(kv.second.ped);            // the puppet
            g_ambExempt.Add(kv.second.anchor);         // its go-to-entity anchor (a real mission ped)
            g_ambExempt.Add(kv.second.swingAnchor);    // R5: ... and the melee task's borrowed one
        }
        for (const auto& kv : g_vehs)
            g_ambExempt.Add(kv.second.handle);         // server vehicles (their occupants are the puppets above)
        g_ambExempt.Add(g_companion);                  // the render-thread test ped
        g_ambExempt.Add(g_ppClone);                    // 0.5.8: the inventory preview's hidden clone (10 m under us, inside every sweep's reach)
        g_ambExempt.Add(g_ppCloneOld);                 // ... and the one a look change replaced, until the new one is given
        for (const int32_t v : g_ownedVehicles)
            g_ambExempt.Add(v);                        // /car vehicles
        if (lp)
        {
            g_ambExempt.Add(g_world->GetVehiclePedIsIn(lp));      // the car we sit in
            g_ambExempt.Add(g_world->GetVehiclePedIsUsing(lp));   // ... or are entering / last used
        }
        g_ambExempt.SetComplete(lp != 0);
    }

    // How many exempt handles exist right now (an unresolved DOES_ENTITY_EXIST counts as existing: the breaker then
    // cannot see a loss, which the `engine: ambient` line shows as calls with failed counts on the other natives).
    int AmbientAliveCount()
    {
        int n = 0;
        for (const int32_t h : g_ambExempt.Handles())
            if (g_world->EntityExists(h) != 0)
                ++n;
        return n;
    }

    // The breaker: an exempt handle that existed before a clean-up and is gone right after it (same tick) means the
    // clean-up took it. Clears and sweeps then stay off for the rest of the process.
    void AmbientCheckProtected(int before, int after, const char* what)
    {
        if (!g_ambSched.CheckProtected(before, after))
            return;
        GLOGW("ambient: %s removed %d exempt entity(ies) (a puppet, a server car, our own ped or car) -> area clears and "
              "sweeps are now OFF for this run (their semantics on mission entities are unverified live); "
              "send this line and the 10 s `engine: ambient` lines", what, before - after);
    }

    // CLEAR_AREA_OF_PEDS / COPS + REMOVE_VEHICLES_FROM_GENERATORS_IN_AREA around (x, y, z): the entry, the respawn and
    // the periodic clear all come through here, guarded by the registry switch (ambient_clear), the exemption list and
    // the breaker. Script tick only.
    void AmbientClearGuarded(int32_t lp, float x, float y, float z, float radius, const char* why)
    {
        if (!g_ambSched.Sw().master || !g_ambSched.Sw().areaClear || g_ambSched.BreakerTripped())
            return;
        AmbientGatherExempt(lp);
        const int before = AmbientAliveCount();
        g_world->ClearAmbientAround(x, y, z, radius);
        const int after = AmbientAliveCount();
        ++g_ambRep.clears;
        g_ambRep.exemptCount = (int)g_ambExempt.Size();
        g_ambRep.protectedSeen += (uint32_t)before;
        AmbientCheckProtected(before, after, why);
    }

    // The entity sweep (enumeration natives; inert until their hashes exist): delete what is not exempt. Script tick only.
    void AmbientSweep(int32_t lp)
    {
        int32_t found[ambient::kNearbyCap];
        std::vector<int32_t> plan;
        AmbientGatherExempt(lp);
        const int before = AmbientAliveCount();
        g_ambRep.exemptCount = (int)g_ambExempt.Size();
        int budget = ambient::kSweepMaxDeletes;
        for (int pass = 0; pass < 2; ++pass)           // 0: peds, 1: vehicles
        {
            ambient::g_lastNearbyEcho = false;
            const int n = pass == 0 ? g_world->NearbyPeds(lp, found, ambient::kNearbyCap)
                                    : g_world->NearbyVehicles(lp, found, ambient::kNearbyCap);
            (pass == 0 ? g_ambRep.nearPeds : g_ambRep.nearVehs) = n;
            if (ambient::g_lastNearbyEcho)
                ++g_ambRep.enumCapEcho[pass];
            if (n <= 0)
                continue;
            const ambient::SweepStats st = ambient::PlanSweep(found, n, g_ambExempt, budget, plan);
            g_ambRep.protectedSeen += (uint32_t)st.exempt;
            g_ambRep.enumSeen[pass] += (uint32_t)st.seen;       // R3 T5: what each enumeration was made of (the live `near[peds=64 vehs=64]` was the capacity)
            g_ambRep.enumExempt[pass] += (uint32_t)st.exempt;
            g_ambRep.enumUnproven[pass] += (uint32_t)st.unproven;
            g_ambRep.enumInvalid[pass] += (uint32_t)st.invalid;
            g_ambRep.enumPlanned[pass] += (uint32_t)st.planned;
            for (const int32_t h : plan)
                if (g_world->EntityExists(h) == 1 && g_world->DeleteAmbientEntity(h))
                {
                    ++g_ambRep.deleted;
                    --budget;
                }
        }
        ++g_ambRep.sweeps;
        AmbientCheckProtected(before, AmbientAliveCount(), "the entity sweep");
    }

    // Every tick, from the top of GameWork (script tick or render-thread fallback), whatever the connection.
    void TickAmbient(bool inScript, int32_t lp, WorkProbe* probe)
    {
        std::call_once(g_ambConfigured, [] {
            g_ambSched.Configure(ambient::LoadSwitches([](const char* name) {
                return reg::Get(std::wstring(name, name + std::strlen(name)));
            }));
            const ambient::Switches& sw = g_ambSched.Sw();
            GLOGI("ambient: suppression %s; render-thread fallback %s; periodic area clear %s (breaker armed); entity sweep %s "
                  "(registry: ambient / ambient_render / ambient_clear / ambient_sweep = 0 turns one off)",
                  sw.master ? "ON" : "OFF (ambient=0)", sw.renderFallback ? "on" : "off",
                  sw.areaClear ? "on" : "off", sw.sweep ? "on" : "off");
        });
        const double nowMs = NowMs();
        const bool connected = join::IsConnected();
        g_ambEverConnected = g_ambEverConnected || connected;
        const ambient::Link link = ambient::LinkOf(connected, g_entry.load() == Entry::Done, g_ambEverConnected);
        if (inScript && (int)link != g_ambLinkSeen)
        {
            if (g_ambLinkSeen >= 0)
                GLOGI("ambient: connection %s -> %s (the suppression keeps running)",
                      ambient::Name((ambient::Link)g_ambLinkSeen), ambient::Name(link));
            g_ambLinkSeen = (int)link;
        }
        ambient::Ctx c;
        c.inScript = inScript;
        c.spawnMode = g_world->spawnMode;
        c.nativesUp = g_world->HaveNatives();
        c.havePlayer = inScript && lp != 0;
        c.sweepAvailable = g_world->AmbientSweepAvailable();
        c.nowMs = (uint64_t)nowMs;
        const ambient::Plan p = g_ambSched.Next(c);
        if (!p.frame)
            ++g_ambRep.skipped[(int)p.skip];
        else
        {
            static bool loggedFirst = false;
            if (!loggedFirst)
            {
                loggedFirst = true;
                GLOGI("ambient: first suppression frame via the %s (connection %s, spawn mode %d, player %s)",
                      inScript ? "script tick" : "render-thread fallback", ambient::Name(link), c.spawnMode,
                      lp ? "known" : "not yet");
            }
            {
                WorkSection s(probe, kSecAmbientFrame);
                g_world->SuppressAmbientFrame();       // the five density multipliers, every frame, at the source
            }
            if (inScript)
            {
                ++g_ambRep.frameScript;
                ++g_ambRep.ticks[(int)link];
            }
            else
                ++g_ambRep.frameRender;
            float x = 0, y = 0, z = 0;
            const bool haveCoords = (p.periodic || p.clear) && g_world->GetEntityCoords(lp, x, y, z);
            if (p.periodic)
            {
                WorkSection s(probe, kSecAmbientFrame);
                g_world->SuppressAmbientSystems(x, y, z, haveCoords);
                ++g_ambRep.periodic;
            }
            if (p.clear && haveCoords)
            {
                WorkSection s(probe, kSecAmbientFrame);
                AmbientClearGuarded(lp, x, y, z, ambient::kClearRadiusM, "the periodic area clear");
            }
            if (p.sweep)
            {
                WorkSection s(probe, kSecAmbientFrame);
                AmbientSweep(lp);
            }
        }
        // The 10 s line: key=value, parseable; written in every connection state, also while nothing runs.
        if (g_ambReportMs == 0)
            g_ambReportMs = nowMs;
        else if (nowMs - g_ambReportMs >= 10000.0)
        {
            ambient::Report& r = g_ambRep;
            r.windowSec = (nowMs - g_ambReportMs) / 1000.0;
            r.link = link;
            r.calls = ambient::Delta(g_world->ambientTally, g_ambTallySeen);
            g_ambTallySeen = g_world->ambientTally;
            r.breaker = g_ambSched.BreakerTripped();
            r.breakerLost = g_ambSched.ExemptLost();
            r.sw = g_ambSched.Sw();
            r.sweepAvailable = g_world->AmbientSweepAvailable();
            char line[1400];
            ambient::FormatLine(line, sizeof(line), r);
            GLOGI("%s", line);
            const int keepPeds = r.nearPeds, keepVehs = r.nearVehs, keepExempt = r.exemptCount;
            r = ambient::Report();
            r.nearPeds = keepPeds;
            r.nearVehs = keepVehs;
            r.exemptCount = keepExempt;
            g_ambReportMs = nowMs;
        }
    }

    // Longstops, so the entry can never block for ever: the readiness gate below gives GTA this
    // long (from the first script tick with a session and a ped) to finish its own load, and the
    // freemode model this long (from readiness) to stream in before the story ped is kept.
    constexpr ULONG      kEntryReadyMaxMs   = 90000;
    constexpr ULONGLONG  kModelStreamMaxMs  = 10000;
    // Radius of the one-shot ambient clear-out around the spawn (entry and respawn).
    constexpr float      kSpawnClearRadius  = 200.0f;

    // Why the entry is not ready, as a bit set (0 = ready). We take the player as soon as a ped
    // exists and no character switch is mid-flight, then drive the world reveal ourselves (freeze,
    // stream the spawn, dismiss the loading screen, grant control, fade in -- see EnterServer and
    // the Loading phase). We do NOT wait on the loading screen, the fade or player control: with the
    // SP mission flow killed those never come true (the Prologue is what would clear them), so the
    // gate would always time out. The loading/fade/control bits remain for WaitNames diagnostics
    // but no longer gate the entry.
    enum : unsigned { kWaitPed = 1, kWaitLoading = 2, kWaitFade = 4, kWaitSwitch = 8, kWaitControl = 16 };

    unsigned EntryNotReady(int32_t ped)
    {
        unsigned w = 0;
        if (!ped)
            w |= kWaitPed;
        if (g_world->IsPlayerSwitchInProgress())
            w |= kWaitSwitch;
        return w;
    }

    std::string WaitNames(unsigned w)
    {
        std::string s;
        auto add = [&](unsigned bit, const char* name) {
            if (!(w & bit))
                return;
            if (!s.empty())
                s += ", ";
            s += name;
        };
        add(kWaitPed, "no player ped");
        add(kWaitLoading, "loading screen up");
        add(kWaitFade, "screen not faded in");
        add(kWaitSwitch, "player switch running");
        add(kWaitControl, "player control off");
        return s;
    }

    // ---- 0.4.20 map clean-up (GI: "clear the map from all waypoints, and reveal all of it for everyone") ----------------------------------
    // sync/mapclear.h has the rules and the evidence; the switches (g_mapClearBlips, g_mapReveal) are read by MapReadSwitches at Welcome.
    // Everything below runs on the script tick.
    sync::MapClearSchedule g_mapSched;
    sync::OwnedBlips g_ownedBlips;      // the engine's own blips: none yet; every ADD_BLIP_FOR_* site must Add its handle here
    struct MapStats                     // one 10 s window
    {
        uint32_t sweeps = 0, walkFailed = 0;
        uint32_t keptPlayer = 0, keptNorth = 0, keptWaypoint = 0, keptOwned = 0;
        uint32_t fowIssued = 0, fowFailed = 0;
        double sweepMsMax = 0;
        sync::BlipTally tally;
    };
    MapStats g_mapStats;
    int g_mapActive = -1;               // GET_NUMBER_OF_ACTIVE_BLIPS after the last sweep (-1 unresolved)
    const char* g_mapWaypoint = "not yet";   // the entry's stale-waypoint clear, for the 10 s line
    bool g_mapAnyScript = false;        // SET_THIS_SCRIPT_CAN_REMOVE_BLIPS_CREATED_BY_ANY_SCRIPT resolved on the last sweep

    void SweepMapBlips(bool clearWaypoint, bool census)
    {
        MapStats& s = g_mapStats;
        const int64_t q0 = QpcNow();
        if (clearWaypoint)
        {
            bool was = false, after = false;
            g_mapWaypoint = !g_world->ClearWaypoint(was, after) ? "UNRESOLVED" : !was ? "none active" : after ? "STILL ACTIVE after SET_WAYPOINT_OFF"
                                                                                                           : "cleared (inactive on read-back)";
            GLOGI("engine: map (0.4.20): stale waypoint at the entry: %s", g_mapWaypoint);
        }
        g_mapAnyScript = g_world->AllowAnyScriptBlips();
        sync::BlipKeepSet keep;
        int wpEnum = 0;
        bool wpOk = false;
        g_world->MapKeepHandles(keep.mainPlayer, keep.north, wpEnum, wpOk);
        keep.waypointSprite = sync::WaypointSpriteFrom(wpEnum, wpOk);
        static std::vector<game::World::MapBlip> blips;
        ++s.sweeps;
        if (!g_world->CollectBlips(sync::kBlipSpriteFirst, sync::kBlipSpriteLast, blips))
        {
            ++s.walkFailed;
            static bool warned = false;
            if (!warned)
                GLOGW("engine: map (0.4.20): a blip native did not resolve; SP blips left on the map");
            warned = true;
            return;
        }
        sync::BlipTally one;            // this sweep only (the census)
        const sync::MapSweepPlan plan = sync::PlanMapSweep(blips, keep, g_ownedBlips, one);
        for (const sync::MapSweepPlan::Target& t : plan.hide)
        {
            if (!sync::BlipDisplayShows(g_world->HideBlip(t.handle)))
                one.Hidden(t.sprite);   // read back hidden
            else
                one.Refused(t.sprite);  // still shows (or unreadable)
        }
        s.keptPlayer += plan.keptPlayer;
        s.keptNorth += plan.keptNorth;
        s.keptWaypoint += plan.keptWaypoint;
        s.keptOwned += plan.keptOwned;
        s.tally.Merge(one);
        g_mapActive = g_world->ActiveBlipCount();
        const double ms = QpcUs(QpcNow() - q0) / 1000.0;
        if (ms > s.sweepMsMax)
            s.sweepMsMax = ms;
        if (census)
            GLOGI("engine: map (0.4.20): sweep census: %zu blip(s) walked over sprites %d-%d in %.2f ms, the game counts %d active; keep: player blip %d, "
                  "north blip %d, waypoint sprite %d (%s), %zu own; per sprite: %s",
                  blips.size(), sync::kBlipSpriteFirst, sync::kBlipSpriteLast, ms, g_mapActive, keep.mainPlayer, keep.north, keep.waypointSprite,
                  wpOk ? "GET_WAYPOINT_BLIP_ENUM_ID" : "default, enum unreadable", g_ownedBlips.Size(), one.Format(24).c_str());
    }

    void LogMap()
    {
        MapStats& s = g_mapStats;
        const sync::BlipTally::Row& tot = s.tally.Totals();
        float ratio = 0.0f;
        bool farRevealed = false;
        const bool fowRead = g_world->MapFowReadback(ratio, farRevealed);
        GLOGI("engine: map (0.4.20), last 10 s: map_clear_blips=%s map_reveal=%s; %u sweep(s) (%u walk failed, max %.2f ms), %u blip(s) walked: hidden %u "
              "(read back hidden), REFUSED %u (still shows), already hidden %u, kept player %u / north %u / waypoint %u / own %u; game counts %d active; "
              "any-script blips %s; stale waypoint: %s; fog: SET_MINIMAP_HIDE_FOW(true) issued %u, failed %u; fog data (no getter for the hide flag) "
              "%s discovered %.3f, Paleto Bay %s; sprites: %s",
              g_mapClearBlips.load() ? "on" : "OFF", g_mapReveal.load() ? "on" : "OFF", s.sweeps, s.walkFailed, s.sweepMsMax, tot.seen, tot.hidden,
              tot.refused, tot.already, s.keptPlayer, s.keptNorth, s.keptWaypoint, s.keptOwned, g_mapActive, g_mapAnyScript ? "allowed" : "NOT allowed",
              g_mapWaypoint, s.fowIssued, s.fowFailed, fowRead ? "read:" : "UNREADABLE:", (double)ratio, farRevealed ? "revealed" : "fogged",
              s.tally.Format(12).c_str());
        s = MapStats();
    }

    // The entry is done (EnterServer): sweep now, then on the schedule. map_clear_blips=0 runs the 0.4.18 one-shot instead.
    void MapOnEntry(ULONG now)
    {
        if (g_mapClearBlips.load() || g_mapReveal.load())
            g_mapSched.OnEntry((double)now);
        else
            g_mapSched.Reset();
        g_mapWaypoint = "not yet";
        if (!g_mapClearBlips.load())
            g_world->ClearSpBlips();
    }

    // Every script tick (cheap until a sweep is due).
    void TickMap(ULONG now)
    {
        const double t = (double)now;
        if (g_mapSched.SweepDue(t))
        {
            if (g_mapReveal.load())
                ++(g_world->SetMapFowHidden(true) ? g_mapStats.fowIssued : g_mapStats.fowFailed);
            if (g_mapClearBlips.load())
            {
                const bool wp = g_mapSched.TakeWaypointClear();
                SweepMapBlips(wp, g_mapSched.TakeCensus());
            }
        }
        if (g_mapSched.LogDue(t))
            LogMap();
    }

    // True when the entry is finished (or given up); called every script tick.
    bool EnterServer()
    {
        if (g_entry.load() == Entry::Done)
            return true;
        if (!join::IsConnected())
            return false;
        const ULONG now = GetTickCount();
        {
            ULONG none = 0;
            g_entryStarted.compare_exchange_strong(none, now ? now : 1);   // the join worker's OnEnded may zero it concurrently
        }
        const ULONG started = g_entryStarted.load();
        if (!started)
            return false;                  // the session ended under us (OnEnded zeroes it)

        // Readiness gate: wait for GTA to finish loading before touching the player. The join is
        // usually up long before that (the bulk of the wait is the game's own load), so the model
        // is requested meanwhile and is ready to swap the moment the gate opens.
        if (!g_entryReadyAt.load())
        {
            const unsigned wait = EntryNotReady(g_world->LocalPlayer());
            const ULONG waited = now - started;
            if (wait && waited < kEntryReadyMaxMs)
            {
                g_world->RequestModel(kFreemodeMale);
                // Kill the SP mission flow before it runs. A fresh Story Mode boot's flow_controller
                // launches the Prologue (North Yankton), which forces a story character and teleports
                // there -- so terminate flow_controller + prologue1 every tick from the first live
                // tick (terminating a dead script is a no-op) and hold the ambient world off. The
                // freemode model keeps streaming meanwhile for the swap at entry. With stock scripts
                // off (no_sp_scripts=1) no SP script executes, so there is no flow to kill: the
                // terminate loop is skipped (the entry runs SuppressEntryBoot once instead).
                // (the ambient density multipliers run from TickAmbient at the top of GameWork, T5)
                if (!g_world->StockScriptsOff())
                    g_world->SuppressEntryBoot();
                static ULONG lastLog = 0;
                if (now - lastLog >= 5000)
                {
                    lastLog = now;
                    GLOGI("engine: entry waiting for the game (%s), %lu s so far",
                          WaitNames(wait).c_str(), (unsigned long)(waited / 1000));
                }
                return false;
            }
            if (wait)
                GLOGW("engine: entry gate not open after %lu s (%s); entering anyway",
                      (unsigned long)(waited / 1000), WaitNames(wait).c_str());
            else
                GLOGI("engine: game ready after %lu s (loading screen gone, faded in, no switch, control on)",
                      (unsigned long)(waited / 1000));
            g_entryReadyAt.store(GetTickCount64());
        }

        // The model streams in over a few frames; past kModelStreamMaxMs from readiness keep the
        // story ped (the default-ped guard keeps retrying the swap after the entry).
        const bool modelOk = g_world->SetPlayerModel(kFreemodeMale);
        if (!modelOk && GetTickCount64() - g_entryReadyAt.load() < kModelStreamMaxMs)
            return false;
        if (modelOk)
            DressPlayer("entry");           // the swap took: put the GAGE-MP look on the new ped
        const int32_t ped = g_world->LocalPlayer();
        if (!ped)
            return false;
        g_world->SetEntityCoords(ped, kSpawn[0], kSpawn[1], kSpawn[2]);
        g_world->SetEntityHeading(ped, kSpawn[3]);
        g_world->FreezeEntityPosition(ped, true);   // held hidden until the spawn's collision streams in
        g_world->RequestCollisionAt(kSpawn[0], kSpawn[1], kSpawn[2]);
        AmbientClearGuarded(ped, kSpawn[0], kSpawn[1], kSpawn[2], kSpawnClearRadius, "the entry area clear");
        MapOnEntry(now);                    // 0.4.20: arms the map sweep (map_clear_blips=0: the 0.4.18 ClearSpBlips one-shot)
        g_world->DisableWanted();
        g_world->DumpLiveScripts("pristine");   // which SP scripts are running at entry (diagnostic)
        g_world->SuppressSPSystems();
        g_entry.store(Entry::Done);
        // With the SP mission flow killed, GTA never dismisses its own loading screen, fades in or
        // hands over control -- drive the reveal ourselves. Hand to the Loading phase (shared with
        // the self-driven respawn): it waits for the spawn collision, then dismisses the loading
        // screen, unfreezes, grants control and fades in. The frozen ped stays hidden behind GTA's
        // loading screen until then, so there is never a drop into an unstreamed world.
        g_life = Life::Loading;
        g_lifePhaseAt = now;
        std::memcpy(g_lifeSpawn, kSpawn, sizeof(g_lifeSpawn));
        g_v4ResetEnded.store(false);
        g_v4Reset.store(true);            // server-owned health starts from the server's defaults
        g_satiety = 100.0f;               // reset the placeholder hunger stat on entry
        g_satietyAt = 0;
        g_pendingCar = {};
        g_entryDoneAt = now ? now : 1;     // arms the default-ped guard and its one-shot logs
        g_entryLogged = 0;
        g_entryBootOnce = false;
        g_pedGuardAt = now;
        { std::lock_guard<std::mutex> lk(g_cmdMtx); g_cmdQueue.clear(); }
        GLOGI("engine: entered the server: %s, at spawn %.1f, %.1f, %.1f, wanted level off",
              modelOk ? "freemode character" : "story character (freemode model did not load in 10 s)",
              kSpawn[0], kSpawn[1], kSpawn[2]);
        return true;
    }

    // After the entry (script tick, life Alive): the one-shot ped logs at ~1 s and ~10 s, and the
    // default-ped guard — if GTA replaces the ped with a story character, put the freemode back.
    // True when it swapped the ped (the caller must re-read the local ped handle).
    bool TickEntryGuard(int32_t lp, ULONG now)
    {
        if (!g_entryDoneAt)
            return false;
        const ULONG since = now - g_entryDoneAt;
        auto logPed = [&](const char* when) {
            GLOGI("engine: ped %s after entry: model 0x%08x (freemode 0x%08x), health %d / max %d",
                  when, (unsigned)g_world->GetEntityModel(lp), (unsigned)kFreemodeMale,
                  g_world->GetEntityHealth(lp), g_world->GetEntityMaxHealth(lp));
        };
        if (!(g_entryLogged & 1) && since >= 1000) { g_entryLogged |= 1; logPed("1 s"); }
        if (!(g_entryLogged & 2) && since >= 10000) { g_entryLogged |= 2; logPed("10 s"); }

        if (now - g_pedGuardAt < 1000)
            return false;
        g_pedGuardAt = now;
        const uint32_t model = g_world->GetEntityModel(lp);
        if (model == 0)
        {
            // Unreadable (0), which is not "wrong": swapping the player on a failed read would
            // re-create a healthy ped every second.
            static bool loggedUnreadable = false;
            if (!loggedUnreadable)
            {
                loggedUnreadable = true;
                GLOGW("engine: ped guard: the local ped's model is unreadable; guard idle");
            }
            return false;
        }
        if (model == kFreemodeMale)
            return false;
        const bool swapped = g_world->SetPlayerModel(kFreemodeMale);  // also re-sets the default components
        if (swapped)
        {
            DressPlayer("ped guard");       // the new ped needs the GAGE-MP look again
            const int32_t np = g_world->LocalPlayer();
            GLOGI("engine: health: ped guard swap: new ped %d has health %d / max %d (GTA defaults) before the server maximum %d",
                  (int)np, g_world->GetEntityHealth(np), g_world->GetEntityMaxHealth(np),
                  sync::PedMaxHealth(g_playerHealth.load()));
            ApplyServerMaxHealth(np, "ped guard swap");     // ... and the server maximum health
            g_vitals.NotePedReplaced();     // T6: the new ped's default 200 is no loss; the server's value goes on it
        }
        static int loggedFixes = 0;
        if (loggedFixes < 10)
        {
            ++loggedFixes;
            GLOGW("engine: ped guard: local ped is model 0x%08x, not freemode; %s", (unsigned)model,
                  swapped ? "freemode re-applied" : "freemode re-apply pending (model still streaming)");
        }
        return swapped;
    }

    // One step of the self-driven respawn (see Life above). Called every script
    // tick while in the world. lp is the current local ped, now is GetTickCount().
    // Protocol v4 is live: connected and entered. Health, death and respawn are then the server's.
    bool ServerMode()
    {
        return join::IsConnected() && g_entry.load() == Entry::Done;
    }

    // Revive the local ped hidden at g_lifeSpawn and hand over to Loading (shared by the local and the
    // server-driven respawn). Script tick.
    void ReviveAtLifeSpawn(ULONG now)
    {
        const float* s = g_lifeSpawn;
        g_world->ReviveLocalPlayer(s[0], s[1], s[2], s[3]);
        AmbientClearGuarded(g_world->LocalPlayer(), s[0], s[1], s[2], kSpawnClearRadius, "the respawn area clear");
        g_world->ClearDeathEffects();
        g_world->RestoreDeathDefaults();
        g_world->RequestCollisionAt(s[0], s[1], s[2]);
        g_life = Life::Loading;
        g_lifePhaseAt = now;
    }

    // Keep a hold's dictionary in step with its arbiter: wanted while the emote is an active animation
    // (and, for a peer, while its ped exists), released otherwise; the last release streams it out.
    void SyncEmoteDict(sync::EmoteDictHold& hold, bool wanted, const std::string& dict)
    {
        static const std::string kNone;   // both arms lvalues: no copy of the dictionary name per peer per frame
        const std::string remove = hold.Sync(g_animDicts, wanted ? dict : kNone);
        if (!remove.empty())
            g_world->RemoveAnimDict(remove.c_str());
    }
    void SyncLocalEmoteDict()
    {
        SyncEmoteDict(g_localEmoteDict, g_localEmote.Active() && g_localEmote.Kind() == sync::EmoteKind::Anim,
                      g_localAnim.dict);
    }
    void SyncPeerEmoteDict(Peer& pe)
    {
        SyncEmoteDict(pe.emoteDict,
                      pe.ped != 0 && pe.emote.Active() && pe.emote.Kind() == sync::EmoteKind::Anim, pe.emoteAnim.dict);
    }

    void TickLife(int32_t lp, ULONG now)
    {
        const double nowMs = NowMs();
        switch (g_life)
        {
        case Life::Alive:
            if (g_revealCheckAt && now >= g_revealCheckAt && lp)
            {
                // 0.4.19: the screen state 5 s after a reveal (GI's green hue mid-air shows here if it comes back).
                g_revealCheckAt = 0;
                int tc = -2;
                g_world->GetTimecycleModifierIndex(tc);
                GLOGI("engine: 5 s after the reveal: height above ground %.1f m, player switch %s, timecycle modifier %d",
                      g_world->GetEntityHeightAboveGround(lp), g_world->IsPlayerSwitchInProgress() ? "IN PROGRESS" : "none", tc);
            }
            if (g_haveRespawnOk)
            {
                // A RespawnOk while our ped lives (the server had us dead, the local kill did not take):
                // the server's word wins, put the ped where it says.
                g_life = Life::Respawning;
                g_lifePhaseAt = now;
                GLOGI("engine: RespawnOk while alive: moving to the server's spawn point");
                break;
            }
            if (lp && g_world->IsPedDeadOrDying(lp))
            {
                g_life = Life::Dying;
                g_lifePhaseAt = now;
                g_world->HoldDeathRestart();    // stop the game's own (broken) restart at once
                g_respawnGate.OnDeath(nowMs);
                g_serverDeadNoticed = false;
                GLOGI("engine: local player died -- %s", ServerMode()
                      ? "waiting for the server's respawn (Respawn requests from 5 s)" : "starting respawn");
            }
            break;

        case Life::Dying:
        {
            const ULONG el = now - g_lifePhaseAt;
            g_world->HoldDeathRestart();        // reassert every tick while dying
            if (ServerMode())
            {
                // The death view stays up until the server brings us back.
                if (g_haveRespawnOk)
                {
                    g_life = Life::Respawning;
                    g_lifePhaseAt = now;
                    break;
                }
                if (g_respawnGate.ShouldSend(nowMs))
                {
                    const bool sent = join::SendRespawn();
                    ++g_v4.respawnReqs;
                    const int n = g_respawnGate.Attempts();
                    if (n == 1 || n % 5 == 0)
                        GLOGI("engine: respawn request %d %s (%.1f s after the death)", n,
                              sent ? "queued" : "NOT queued (no session)", (nowMs - g_respawnGate.DeathMs()) / 1000.0);
                }
                const sync::GiveUpAction giveUp = g_respawnGate.GiveUp(nowMs, g_vitals.ServerDead());
                if (giveUp == sync::GiveUpAction::Wait)
                    break;
                if (giveUp == sync::GiveUpAction::KeepAsking)
                {
                    // The server says we are dead and has not answered in 30 s: reviving locally would
                    // only be undone (the tracker would kill the ped again) and the other players would
                    // see a corpse that walks. Keep asking, backed off (1, 2, 4 ... 10 s), and say so once.
                    if (!g_serverDeadNoticed)
                    {
                        g_serverDeadNoticed = true;
                        game::Overlay::PushSystemLine("The server has not respawned you yet. Still asking...");
                        GLOGW("engine: no RespawnOk %d s after the death (%d requests) and the server says we are dead: "
                              "NOT reviving locally, still asking (backoff up to 10 s)",
                              (int)((nowMs - g_respawnGate.DeathMs()) / 1000.0), g_respawnGate.Attempts());
                    }
                    break;
                }
                GLOGW("engine: no RespawnOk %d s after the death (%d requests); reviving locally at the default "
                      "spawn -- the server may still have us dead", (int)((nowMs - g_respawnGate.DeathMs()) / 1000.0),
                      g_respawnGate.Attempts());
                std::memcpy(g_lifeSpawn, kSpawn, sizeof(g_lifeSpawn));
                g_life = Life::Respawning;
                g_lifePhaseAt = now;
                break;
            }
            if (el < kDeathHoldMs)
                break;                          // let the death moment play out
            if (!g_world->IsScreenFadedOut() && el < kDyingGiveUpMs)
            {
                g_world->DoScreenFadeOut(kFadeOutMs);  // fade to black before the teleport
                break;
            }
            // Black (or we gave up waiting on the fade): revive hidden, at spawn.
            std::memcpy(g_lifeSpawn, kSpawn, sizeof(g_lifeSpawn));
            ReviveAtLifeSpawn(now);
            g_respawnGate.OnAlive();
            break;
        }

        case Life::Respawning:
        {
            // Fade out, then revive at the server's point (or the default after a give-up).
            const ULONG el = now - g_lifePhaseAt;
            g_world->HoldDeathRestart();
            if (g_haveRespawnOk)
            {
                g_haveRespawnOk = false;
                const proto::RespawnOk& r = g_respawnOk;
                g_lifeSpawn[0] = r.x; g_lifeSpawn[1] = r.y; g_lifeSpawn[2] = r.z; g_lifeSpawn[3] = r.heading;
                g_vitals.OnRespawn(r.health, r.armor);
            }
            if (!g_world->IsScreenFadedOut() && el < 1500)
            {
                g_world->DoScreenFadeOut(kFadeOutMs);
                break;
            }
            ReviveAtLifeSpawn(now);
            g_respawnGate.OnAlive();
            // The server's health and armour go on at once (the tracker re-applies them next tick too).
            // After a give-up the server may still have us dead: the tracker then kills the ped again
            // and the respawn requests start over (the server's word wins).
            const int32_t ped = g_world->LocalPlayer();
            if (ped && !g_vitals.ServerDead())
            {
                ApplyServerMaxHealth(ped, "respawn");  // before the health: a value above the old maximum would be cut
                g_world->SetEntityHealth(ped, 100 + g_vitals.Scale().ToGta((int)g_vitals.Health()));   // R3 T1: in the units the ped runs in
                g_world->SetPedArmour(ped, (int)g_vitals.Armour());
            }
            GLOGI("engine: respawned at %.1f, %.1f, %.1f heading %.0f with health %u armour %u", g_lifeSpawn[0],
                  g_lifeSpawn[1], g_lifeSpawn[2], g_lifeSpawn[3], (unsigned)g_vitals.Health(), (unsigned)g_vitals.Armour());
            break;
        }

        case Life::Loading:
        {
            const ULONG el = now - g_lifePhaseAt;
            const int32_t ped = g_world->LocalPlayer();
            g_world->RequestCollisionAt(g_lifeSpawn[0], g_lifeSpawn[1], g_lifeSpawn[2]);
            // 0.4.19: the car packs mount on this thread from the entry on; reveal only once they settled (Ready / Failed,
            // or still Idle after kPackIdleGraceMs = nothing armed), so no mount stall lands inside the reveal.
            const int ps = g_packStatus.load();
            const bool packsSettled = !g_spawnWaitPacks.load() || ps == (int)game::content::Status::Ready ||
                                      ps == (int)game::content::Status::Failed ||
                                      (ps == (int)game::content::Status::Idle && el >= kPackIdleGraceMs) || el >= kPackRevealMaxMs;
            const bool collision = g_world->HasCollisionLoadedAround(ped);
            if (packsSettled && (collision || el >= kLoadMaxMs))
            {
                int tc = -2;                        // read before any clear: what the boot left (-1 = none)
                g_world->GetTimecycleModifierIndex(tc);
                if (g_spawnClearFx.load())
                {
                    // Whatever the story-mode boot left on screen (a switch scene's post-fx, a timecycle modifier) goes.
                    g_world->StopAllPostFx();
                    g_world->ClearTimecycleModifier();
                }
                g_world->RequestCollisionAt(g_lifeSpawn[0], g_lifeSpawn[1], g_lifeSpawn[2]);
                g_world->ShutdownLoadingScreen();   // dismiss GTA's loading screen (a no-op on respawn)
                g_world->FreezeEntityPosition(ped, false);
                g_world->SetPlayerControl(true);    // grant control (a harmless re-assert on respawn)
                g_world->DoScreenFadeIn(kFadeInMs);
                g_life = Life::Alive;
                GLOGI("engine: world reveal complete (%s) after %lu ms; packs %s (status %d); height above ground %.1f m, "
                      "player switch %s, timecycle modifier %d%s",
                      collision ? "collision loaded" : "collision timeout", (unsigned long)el,
                      !g_spawnWaitPacks.load() ? "not waited for (spawn_wait_packs=0)"
                          : (el >= kPackRevealMaxMs && ps == (int)game::content::Status::Pending ? "STILL MOUNTING (30 s cap)" : "settled"),
                      ps, g_world->GetEntityHeightAboveGround(ped), g_world->IsPlayerSwitchInProgress() ? "IN PROGRESS" : "none", tc,
                      g_spawnClearFx.load() ? " (post-fx and modifier cleared)" : "");
                g_revealCheckAt = now ? now + 5000 : 5000;
            }
            break;
        }
        }
    }

    // Lower-case ASCII in place (model/weapon names, command verbs).
    void AsciiLower(std::string& s) {
        for (char& c : s)
            if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }

    // Parse a "/command args" line and queue it. Returns true if it is a known
    // command (handled locally, not sent to the server). Runs on the chat thread.
    bool QueueCommand(const char* text) {
        if (!text || text[0] != '/') return false;
        std::vector<std::string> tok;
        std::string cur;
        for (const char* p = text + 1; *p; ++p) {
            if (*p == ' ' || *p == '\t') { if (!cur.empty()) { tok.push_back(cur); cur.clear(); } }
            else cur.push_back(*p);
        }
        if (!cur.empty()) tok.push_back(cur);
        if (tok.empty()) return false;
        std::string verb = tok[0];
        AsciiLower(verb);

        Command c;
        if (verb == "heal") c.kind = Command::Heal;
        else if (verb == "kill") c.kind = Command::Kill;
        else if (verb == "fix") c.kind = Command::Fix;
        else if (verb == "spawn") {                  // back to the server spawn (the quick-actions palette)
            c.kind = Command::Tp;
            c.tpSpawn = true;
            c.x = kSpawn[0]; c.y = kSpawn[1]; c.z = kSpawn[2];
        }
        else if (verb == "tp") {
            if (tok.size() < 4) return false;
            c.kind = Command::Tp;
            c.x = (float)atof(tok[1].c_str());
            c.y = (float)atof(tok[2].c_str());
            c.z = (float)atof(tok[3].c_str());
        }
        else if (verb == "car" || verb == "veh") {
            c.kind = Command::Car;
            c.arg = tok.size() > 1 ? tok[1] : std::string("adder");
        }
        else if (verb == "gun" || verb == "weapon") {
            if (tok.size() < 2) return false;
            c.kind = Command::Gun;
            c.arg = tok[1];
        }
        // Emotes (protocol v4): /e <dict> <clip> [loop] [upper]; /scenario <NAME>; /stopanim.
        // Names are passed as typed (GTA's dictionaries and clips are case-sensitive paths).
        else if (verb == "e" || verb == "emote" || verb == "anim") {
            if (tok.size() == 2 && (tok[1] == "c" || tok[1] == "stop")) {
                c.kind = Command::StopAnim;
            } else {
                if (tok.size() < 3) return false;
                c.kind = Command::Emote;
                c.arg = tok[1];
                c.arg2 = tok[2];
                for (size_t i = 3; i < tok.size(); ++i) {
                    std::string opt = tok[i];
                    AsciiLower(opt);
                    if (opt == "loop") c.loop = true;
                    else if (opt == "upper") c.upper = true;
                }
            }
        }
        else if (verb == "scenario") {
            if (tok.size() < 2) return false;
            c.kind = Command::Scenario;
            c.arg = tok[1];
        }
        else if (verb == "stopanim") c.kind = Command::StopAnim;
        else if (verb == "swing") c.kind = Command::Swing;     // R4 T3: a punch on the nearest other player's puppet, the next survey attempt (local only)
        else if (verb == "noclip") c.kind = Command::Noclip;   // 0.4.22: the Tab menu's Noclip row sends this; toggles (libs/sync/noclip.h)
        else return false;  // unknown: let it go to the server as chat

        std::lock_guard<std::mutex> lk(g_cmdMtx);
        if (g_cmdQueue.size() < 32)   // bound the queue
            g_cmdQueue.push_back(c);
        return true;
    }

    void RunSwingTest(int32_t lp);   // R4 T3, defined with the swing display (PlayPeerSwing)

    // v8: the admin command a queued command is on a server (none for emotes, /kill, /swing: those are everybody's).
    bool AdminOpOf(const Command& c, proto::AdminOp& op, std::string& arg) {
        arg.clear();
        switch (c.kind) {
        case Command::Heal:   op = proto::AdminOp::Heal; return true;
        case Command::Fix:    op = proto::AdminOp::Fix; return true;
        case Command::Noclip: op = proto::AdminOp::Noclip; return true;
        case Command::Gun:    op = proto::AdminOp::Gun; arg = c.arg; break;
        case Command::Car:    op = proto::AdminOp::Car; arg = c.arg; break;
        case Command::Tp:
            if (c.tpSpawn) { op = proto::AdminOp::Spawn; return true; }
            op = proto::AdminOp::Tp;
            {
                char b[64];
                std::snprintf(b, sizeof b, "%.1f %.1f %.1f", c.x, c.y, c.z);
                arg = b;
            }
            break;
        default: return false;
        }
        // The wire carries printable ASCII up to kMaxAdminArg: anything else is cut to what fits (the name is the player's own typing).
        std::string clean;
        for (char ch : arg)
            if ((unsigned char)ch >= 0x20 && (unsigned char)ch <= 0x7E && clean.size() < proto::kMaxAdminArg) clean.push_back(ch);
        arg = clean;
        return true;
    }

    // v8: on a server, an admin command is asked for first (AdminCmd); OnAdminResult queues it again, approved, or says why not.
    // True when `c` was parked (the caller skips it this tick). With no live session it runs locally as before.
    bool ParkForServer(const Command& c) {
        if (c.approved || !join::IsConnected()) return false;
        proto::AdminCmd m;
        if (!AdminOpOf(c, m.op, m.arg)) return false;
        {
            std::lock_guard<std::mutex> lk(g_cmdMtx);
            m.seq = ++g_adminSeq;
            if (g_adminParked.size() >= kMaxAdminParked) g_adminParked.erase(g_adminParked.begin());
            g_adminParked[m.seq] = c;
        }
        if (!join::SendAdminCmd(m)) {
            {
                std::lock_guard<std::mutex> lk(g_cmdMtx);
                g_adminParked.erase(m.seq);
            }
            game::Overlay::PushSystemLine("The server did not get that command. Try again.");
            GLOGW("engine: /%s not sent to the server (queue full or the session ended)", proto::ToString(m.op));
            return true;
        }
        GLOGI("engine: /%s asked of the server (seq %u)", proto::ToString(m.op), (unsigned)m.seq);
        return true;
    }

    // v8, join worker thread: the server's answer to a ParkForServer.
    void OnAdminResult(const proto::AdminResult& r) {
        Command c;
        {
            std::lock_guard<std::mutex> lk(g_cmdMtx);
            auto it = g_adminParked.find(r.seq);
            if (it == g_adminParked.end()) {
                GLOGW("engine: AdminResult seq %u (/%s) matches no command waiting", (unsigned)r.seq, proto::ToString(r.op));
                return;
            }
            c = it->second;
            g_adminParked.erase(it);
            if (r.code == proto::AdminCode::Ok && r.op != proto::AdminOp::Heal) {
                c.approved = true;             // run it on the next script tick (heal: the server already did it)
                if (g_cmdQueue.size() < 32) g_cmdQueue.push_back(c);
            }
        }
        GLOGI("engine: /%s answered by the server: %s%s%s", proto::ToString(r.op), proto::ToString(r.code), r.text.empty() ? "" : " - ",
              r.text.c_str());
        if (r.code != proto::AdminCode::Ok)
            game::Overlay::PushSystemLine(r.text.empty() ? "The server refused that command." : r.text.c_str());
    }

    // v8, join worker thread: the account the server verified; the overlay offers admin actions when it may use them.
    void OnAccount(const proto::AccountInfo& a) {
        game::Overlay::SetIsAdmin((a.flags & proto::kAcctCommands) != 0);
        std::string name = a.username;
        if (name.empty())
            for (wchar_t ch : g_cfg.player)   // the Hello name (ASCII by the protocol's own rule)
                if (ch >= 0x20 && ch < 0x7F) name.push_back((char)ch);
        std::lock_guard<std::mutex> lk(g_invMtx);
        g_invName = name;
    }

    // 0.6, join worker thread: our inventory as the server holds it (after each of our ops, and after a console give / clear).
    void OnInvState(const proto::InvState& s) {
        std::string name;
        {
            std::lock_guard<std::mutex> lk(g_invMtx);
            name = g_invName;
        }
        if (g_invStates.fetch_add(1) == 0)
            GLOGI("engine: inventory (0.6): first InvState, %zu bytes", s.blob.size());
        game::Overlay::SetInventory(s.blob, s.ackSeq, name.c_str());
        OutfitNoteInventory(s.blob, false);   // 0.5.4 outfit_items: the ped follows the server's worn items
    }

    // 0.6, join worker thread: food the server used for us.
    void OnInvUsed(const proto::InvUsed& u) {
        g_invUsed.fetch_add(1);
        g_satietyAdd.fetch_add((int)u.satiety);
    }

    // ---- 0.5.4 inventory world (engine side; the rules are libs/sync/inventory_world.h) ----------------------------------------------------
    // Join worker thread: our inventory WITH weapons (the server sends InvSnap instead of InvState once we answered its offer). The overlay
    // takes it like an InvState; the script tick applies its weapons to the ped (TickInventoryWorld). A snapshot the tick has not applied yet
    // is replaced by the newer one, keeping its strip list and its "not from our report" (both must still reach the ped).
    void OnInvSnap(const proto::InvSnap& s) {
        std::string name;
        {
            std::lock_guard<std::mutex> lk(g_invMtx);
            name = g_invName;
        }
        if (g_invwSnaps.fetch_add(1) == 0)
            GLOGI("engine: inventory world (0.5.4): first InvSnap, %zu bytes, rev %u", s.blob.size(), (unsigned)s.rev);
        game::Overlay::SetInventory(s.blob, s.ackSeq, name.c_str());
        OutfitNoteInventory(s.blob, true);    // 0.5.6: an InvSnap REPLACES InvState once the world is answered: the outfit must follow it too
        std::lock_guard<std::mutex> lk(g_invwMtx);
        if (g_invwSnapFresh) {
            proto::InvSnap merged = s;
            for (uint32_t h : g_invwSnap.strip)
                if (std::find(merged.strip.begin(), merged.strip.end(), h) == merged.strip.end())
                    merged.strip.push_back(h);
            if (!(g_invwSnap.flags & proto::kInvSnapFromReport))
                merged.flags &= (uint8_t)~proto::kInvSnapFromReport;
            g_invwSnap = std::move(merged);
        } else {
            g_invwSnap = s;
        }
        g_invwSnapFresh = true;
    }

    // Join worker thread: what is in reach (the first one, empty, is the server's offer). inv_world=0 ignores it: no answer, so the server
    // goes on treating us as a 0.5.3 client.
    void OnInvNearby(const proto::InvNearby& n) {
        if (!g_invWorldOn.load()) {
            if (!g_invWorldOffered.exchange(true))
                GLOGI("engine: inventory world (0.5.4): the server offers it; inv_world=0, not answered (0.5.3 behaviour)");
            return;
        }
        if (!g_invWorldOffered.exchange(true))
            GLOGI("engine: inventory world (0.5.4): the server offers it; the first report answers it");
        g_invwNearby.fetch_add(1);
        std::vector<game::Overlay::InvNearbyBox> v;
        for (const proto::InvBox& b : n.boxes) {
            game::Overlay::InvNearbyBox o;
            o.id = b.id;
            o.kind = b.kind;
            o.locked = (b.flags & proto::kInvBoxLocked) != 0;
            o.veh = b.veh;
            o.model = b.model;
            o.cls = b.cls == 255 ? -1 : (int)b.cls;
            o.piles = b.piles;
            o.distM = (float)b.distCm / 100.0f;
            o.encoded = b.blob;
            v.push_back(std::move(o));
        }
        game::Overlay::SetNearby(v);
    }

    // Script tick only.
    struct InvWorldTick {
        bool haveSnap = false;              // `cur` holds a snapshot applied to the ped
        sync::inv::Character cur;           // the newest snapshot applied to the ped: what the ped must carry
        uint32_t rev = 0;                   // its rev (every report carries it: the server reconciles only a report of its current rev)
        bool havePending = false;           // a snapshot arrived while the ped could not take it (dead, new): applied once it can
        proto::InvSnap pending;
        sync::invw::AmmoTypes types;        // weapon -> ammo type (GET_PED_AMMO_TYPE_FROM_WEAPON), learned once per weapon
        std::vector<uint32_t> typeAsked;
        sync::invw::ReportGate gate;        // when to report (inventory_world.h)
        double readAtMs = -1e9, appliedAtMs = -1e9;
        int32_t ped = 0;
        double pedSinceMs = 0;              // when this ped handle (or its life after a death) began
        bool wasDead = false;
        bool afterRespawn = false;          // the next restore may also give back thrown weapons (GTA cleared the ped)
        std::map<uint32_t, double> missingSince, restoredAt;   // weapon -> when the ped was first seen lacking it / last given it back
        std::map<uint16_t, std::pair<int32_t, int>> cls;      // server vehicle id -> (handle, GET_VEHICLE_CLASS) read for the trunk
        std::set<uint16_t> clsSent;         // vehicle ids whose class went out in a report this session
    };
    InvWorldTick g_invw;
    constexpr double kInvwReadEveryMs = 250, kInvwPedSettleMs = 3000, kInvwRestoreAfterMs = 1000, kInvwRestoreGapMs = 2000, kInvwVehReachM = 6.0;

    uint32_t InvwAmmoType(int32_t lp, uint32_t hash) {
        if (std::find(g_invw.typeAsked.begin(), g_invw.typeAsked.end(), hash) == g_invw.typeAsked.end()) {
            g_invw.typeAsked.push_back(hash);
            g_invw.types.Learn(hash, g_world->GetPedAmmoTypeFromWeapon(lp, hash));
        }
        return g_invw.types.Of(hash);
    }

    // The ped as a report sees it: every catalog weapon it has, with its ammo (and type), and the selected weapon. Not valid while dead,
    // within kInvwPedSettleMs of a new ped / a respawn, or when a native cannot be called (nothing in an invalid read is used).
    sync::invw::PedState InvwReadPed(int32_t lp, bool dead, double nowMs) {
        sync::invw::PedState st;
        if (!lp || dead || nowMs - g_invw.pedSinceMs < kInvwPedSettleMs)
            return st;
        st.current = g_world->GetSelectedPedWeapon(lp);
        if (!st.current)
            return st;
        for (const sync::inv::ItemDef& d : sync::inv::WeaponCatalog()) {
            const int has = g_world->HasPedGotWeapon(lp, d.weapon);
            if (has < 0)
                return sync::invw::PedState();
            if (!has)
                continue;
            sync::invw::PedWeapon w;
            w.hash = d.weapon;
            w.ammoType = InvwAmmoType(lp, d.weapon);
            const int a = g_world->GetAmmoInPedWeapon(lp, d.weapon);
            w.ammo = (uint16_t)std::max(0, std::min(a, 65535));
            st.have.push_back(w);
        }
        st.valid = true;
        return st;
    }

    void InvwExecute(int32_t lp, const std::vector<sync::invw::Act>& acts) {
        if (!acts.empty())
            g_inHold.sw.OursNow(NowMs());   // 0.5.6: a weapon change the next ticks see is the inventory's, not GTA's own selection
        for (const sync::invw::Act& a : acts) {
            switch (a.k) {
            case sync::invw::ActK::Remove: g_world->RemoveWeaponFromPed(lp, a.hash); g_invwRemoves.fetch_add(1); break;
            case sync::invw::ActK::Give: g_world->GiveWeaponHolstered(lp, a.hash, 0); g_invwGives.fetch_add(1); break;
            case sync::invw::ActK::SetAmmo: g_world->SetPedAmmo(lp, a.hash, a.ammo); g_invwAmmoSets.fetch_add(1); break;
            case sync::invw::ActK::Select: g_world->SetCurrentPedWeapon(lp, a.hash, true); g_invwSelects.fetch_add(1); break;
            }
        }
    }

    // Server vehicles within kInvwVehReachM whose class the server has not had from us this session: the trunk needs it.
    std::vector<proto::InvVehClass> InvwVehicleClasses(int32_t lp, bool& anyNew) {
        anyNew = false;
        std::vector<proto::InvVehClass> out;
        float px = 0, py = 0, pz = 0;
        if (!g_world->GetEntityCoords(lp, px, py, pz))
            return out;
        for (auto& kv : g_vehs) {
            const int32_t h = kv.second.handle;
            if (!h || out.size() >= proto::kMaxInvReportVehicles)
                continue;
            float x = 0, y = 0, z = 0;
            if (!g_world->GetEntityCoords(h, x, y, z))
                continue;
            const float dx = x - px, dy = y - py, dz = z - pz;
            if (dx * dx + dy * dy + dz * dz > kInvwVehReachM * kInvwVehReachM)
                continue;
            auto it = g_invw.cls.find(kv.first);
            if (it == g_invw.cls.end() || it->second.first != h) {
                g_invw.cls[kv.first] = {h, g_world->GetVehicleClass(h)};
                g_invw.clsSent.erase(kv.first);   // a recycled id is another car
                it = g_invw.cls.find(kv.first);
            }
            const int c = it->second.second;
            if (c < 0 || c > 22)
                continue;
            proto::InvVehClass v;
            v.veh = kv.first;
            v.cls = (uint8_t)c;
            out.push_back(v);
            if (!g_invw.clsSent.count(kv.first))
                anyNew = true;
        }
        return out;
    }

    // Script tick: the ped <-> inventory loop (inventory_world.h section 3). Applies the newest snapshot to the ped, gives back carried
    // weapons the ped lost (death, a script), and reports the ped when it changed. Runs only once the server offered the inventory world.
    void TickInventoryWorld(int32_t lp, bool dead, double nowMs) {
        if (g_invwReset.exchange(false)) {
            g_invw = InvWorldTick();
            game::Overlay::SetInventoryWorld(false);
        }
        if (!g_invWorldOn.load() || !g_invWorldOffered.load() || !lp)
            return;
        if (lp != g_invw.ped) {
            g_invw.ped = lp;
            g_invw.pedSinceMs = nowMs;
            g_invw.afterRespawn = true;
        }
        if (g_invw.wasDead && !dead) {
            g_invw.pedSinceMs = nowMs;   // a respawn: GTA may have cleared the weapons
            g_invw.afterRespawn = true;
        }
        g_invw.wasDead = dead;
        {
            std::lock_guard<std::mutex> lk(g_invwMtx);
            if (g_invwSnapFresh) {
                if (g_invw.havePending) {   // two in a row before the ped could take either: keep both strips, the stronger flag
                    for (uint32_t h : g_invw.pending.strip)
                        if (std::find(g_invwSnap.strip.begin(), g_invwSnap.strip.end(), h) == g_invwSnap.strip.end())
                            g_invwSnap.strip.push_back(h);
                    if (!(g_invw.pending.flags & proto::kInvSnapFromReport))
                        g_invwSnap.flags &= (uint8_t)~proto::kInvSnapFromReport;
                }
                g_invw.pending = std::move(g_invwSnap);
                g_invwSnap = proto::InvSnap();
                g_invw.havePending = true;
                g_invwSnapFresh = false;
            }
        }
        if (!g_invw.havePending && nowMs - g_invw.readAtMs < kInvwReadEveryMs)
            return;
        g_invw.readAtMs = nowMs;
        sync::invw::PedState ped = InvwReadPed(lp, dead, nowMs);

        // 1. a new snapshot reaches the ped (only a ped that can take it: a removal missed now would come back as a "gain")
        if (g_invw.havePending && ped.valid) {
            sync::inv::Character c;
            if (sync::inv::Decode(g_invw.pending.blob, c)) {
                for (uint32_t h : sync::invw::CarriedHashes(c))
                    InvwAmmoType(lp, h);
                const bool fromReport = (g_invw.pending.flags & proto::kInvSnapFromReport) != 0;
                InvwExecute(lp, sync::invw::PlanSnapshot(g_invw.haveSnap ? &g_invw.cur : nullptr, c, fromReport, g_invw.pending.strip, ped, g_invw.types));
                g_invw.cur = std::move(c);
                g_invw.haveSnap = true;
                g_invw.rev = g_invw.pending.rev;
                g_invw.appliedAtMs = nowMs;
                g_invw.missingSince.clear();
            } else {
                GLOGW("engine: inventory world (0.5.4): an InvSnap did not decode (%zu bytes); kept the last one", g_invw.pending.blob.size());
            }
            g_invw.havePending = false;
            g_invw.pending = proto::InvSnap();
            return;   // the ped is read again next time, after GTA took the changes
        }
        if (!ped.valid || g_invw.havePending)
            return;

        // 2. carried weapons the ped lost come back (after kInvwRestoreAfterMs, at most every kInvwRestoreGapMs each). A thrown weapon only
        //    after a respawn: otherwise its loss is the last one thrown, which the next report tells the server (it is used up there).
        if (g_invw.haveSnap) {
            std::vector<sync::invw::Act> acts;
            std::vector<uint32_t> held;   // weapons whose give-back waits (both its Give and its SetAmmo)
            for (const sync::invw::Act& a : sync::invw::PlanRestore(g_invw.cur, ped, g_invw.types)) {
                if (a.k == sync::invw::ActK::Give) {
                    const sync::inv::ItemDef* d = sync::inv::FindWeapon(a.hash);
                    auto ms = g_invw.missingSince.emplace(a.hash, nowMs).first;
                    auto ra = g_invw.restoredAt.find(a.hash);
                    const bool wait = (d && sync::inv::IsThrown(*d) && !g_invw.afterRespawn) ||
                                      (nowMs - ms->second < kInvwRestoreAfterMs && !g_invw.afterRespawn) ||
                                      (ra != g_invw.restoredAt.end() && nowMs - ra->second < kInvwRestoreGapMs);
                    if (wait) {
                        held.push_back(a.hash);
                        continue;
                    }
                    g_invw.restoredAt[a.hash] = nowMs;
                    g_invwRestores.fetch_add(1);
                } else if (std::find(held.begin(), held.end(), a.hash) != held.end()) {
                    continue;
                }
                acts.push_back(a);
            }
            for (auto it = g_invw.missingSince.begin(); it != g_invw.missingSince.end();)
                it = ped.Has(it->first) ? g_invw.missingSince.erase(it) : std::next(it);
            g_invw.afterRespawn = false;
            if (!acts.empty()) {
                InvwExecute(lp, acts);
                return;   // read again before reporting: the ped just changed
            }
        }

        // 3. the report (sync::invw::ReportGate): the first one answers the offer; then a ped that changed since the last report, or one that
        //    still differs from a newer snapshot; also when a car came near whose class the server lacks (its trunk).
        bool newVeh = false;
        const std::vector<proto::InvVehClass> vehs = InvwVehicleClasses(lp, newVeh);
        bool send = g_invw.gate.Due(ped, g_invw.rev, g_invw.haveSnap ? &g_invw.cur : nullptr, g_invw.appliedAtMs, g_invw.types, nowMs);
        if (!send && newVeh && nowMs - g_invw.gate.lastSentMs >= sync::invw::ReportGate::kPresenceGapMs)
            send = true;
        if (!send)
            return;
        const sync::invw::Report r = sync::invw::BuildReport(g_invw.rev, ped);
        proto::InvReport m;
        m.rev = r.rev;
        m.current = r.current;
        m.flags = r.valid ? proto::kInvReportPedValid : 0;
        for (const sync::invw::PedWeapon& w : r.weapons)
            if (m.weapons.size() < proto::kMaxInvReportWeapons)
                m.weapons.push_back({w.hash, w.ammoType, w.ammo});
        m.vehicles = vehs;
        if (!join::SendInvReport(m)) {
            if (g_invwReportsUnsent.fetch_add(1) < 4)
                GLOGW("engine: inventory world (0.5.4): report not sent (no live session)");
            return;
        }
        g_invwReports.fetch_add(1);
        for (const proto::InvVehClass& v : vehs)
            g_invw.clsSent.insert(v.veh);
        if (!g_invw.gate.sentOnce) {
            g_invWorldLive.store(true);
            game::Overlay::SetInventoryWorld(true);
            GLOGI("engine: inventory world (0.5.4): answered (rev %u, %zu weapon(s) on the ped, current 0x%08X)", (unsigned)m.rev,
                  m.weapons.size(), (unsigned)m.current);
        }
        g_invw.gate.Sent(ped, g_invw.rev, nowMs);
    }

    // 0.6, script tick: the ops the player made on the inventory page, to the server in order. Without a live session there is no
    // inventory to change (the page shows "unavailable"), so an op left over from a session that just ended is dropped.
    void PumpInventoryOps() {
        std::string op;
        uint32_t seq = 0;
        while (game::Overlay::PopInventoryOp(&op, &seq)) {
            proto::InvOp m;
            m.seq = seq;
            m.op = op;
            if (join::SendInvOp(m))
                g_invOpsSent.fetch_add(1);
            else if (g_invOpsUnsent.fetch_add(1) < 8)
                GLOGW("engine: inventory op seq %u not sent (no live session, or the queue is full)", (unsigned)seq);
        }
    }

    // Run any queued commands and advance a pending vehicle spawn. Script tick only.
    // lp is the local ped, now is GetTickCount().
    void RunCommands(int32_t lp, ULONG now) {
        PumpInventoryOps();
        std::vector<Command> cmds;
        {
            std::lock_guard<std::mutex> lk(g_cmdMtx);
            cmds.swap(g_cmdQueue);
        }
        // v8: admin commands wait for the server's yes (ParkForServer); the rest run now.
        cmds.erase(std::remove_if(cmds.begin(), cmds.end(), [](const Command& c) { return ParkForServer(c); }), cmds.end());
        float px = 0, py = 0, pz = 0, heading = 0;
        if (!cmds.empty() || g_pendingCar.active) {
            g_world->GetEntityCoords(lp, px, py, pz);
            heading = g_world->GetEntityHeading(lp);
        }
        for (const Command& c : cmds) {
            switch (c.kind) {
            case Command::Heal:
                if (ServerMode()) {
                    // v4: health is the server's; a local heal would be undone by the vitals tracker.
                    game::Overlay::PushSystemLine("Health is server-owned: /heal works offline only.");
                    GLOGI("engine: /heal refused (server-owned health)");
                    break;
                }
                g_world->HealLocal();
                GLOGI("engine: /heal");
                break;
            case Command::Emote: {
                proto::Anim a;
                a.dict = c.arg;
                a.name = c.arg2;
                a.durationMs = c.loop ? -1 : 0;
                a.flags = (c.loop ? 1u : 0u) | (c.upper ? 48u : 0u);  // 1 loop, 16 upper body + 32 player control
                if (!proto::ValidAnim(a)) {
                    game::Overlay::PushSystemLine("Usage: /e <dict> <clip> [loop] [upper] (A-Z a-z 0-9 _ @ / . -, 63 max)");
                    break;
                }
                if (g_world->AnimDictExists(a.dict.c_str()) == 0) {
                    ++g_animDiag.Of(sync::AnimKind::LocalEmote).requested;
                    ++g_animDiag.Of(sync::AnimKind::LocalEmote).dictMissing;
                    game::Overlay::PushSystemLine("No such animation dictionary.");
                    GLOGI("engine: /e %s %s refused: the dictionary does not exist", a.dict.c_str(), a.name.c_str());
                    break;
                }
                g_localAnim = a;
                ++g_animDiag.Of(sync::AnimKind::LocalEmote).requested;
                if (g_localEmote.StartAnim(NowMs(), a.durationMs, a.flags))
                    ++g_animDiag.Of(sync::AnimKind::LocalEmote).restarted;
                SyncLocalEmoteDict();                  // counted at once: a dictionary an ending emote frees stays
                ++g_v4.emotesStarted;
                GLOGI("engine: /e %s %s%s%s (streaming)", a.dict.c_str(), a.name.c_str(), c.loop ? " loop" : "",
                      c.upper ? " upper" : "");
                break;
            }
            case Command::Scenario:
                if (!proto::ValidAnimName(c.arg)) {
                    game::Overlay::PushSystemLine("Usage: /scenario <NAME> (e.g. WORLD_HUMAN_SMOKING)");
                    break;
                }
                g_localScenario = c.arg;
                ++g_animDiag.Of(sync::AnimKind::LocalEmote).requested;
                if (g_localEmote.StartScenario(NowMs()))
                    ++g_animDiag.Of(sync::AnimKind::LocalEmote).restarted;
                ++g_v4.emotesStarted;
                GLOGI("engine: /scenario %s", c.arg.c_str());
                break;
            case Command::Swing:
                RunSwingTest(lp);
                break;
            case Command::Noclip:
                g_noclipToggle = true;                 // TickNoclip (right after this) decides: start, stop, or refuse with a reason
                break;
            case Command::StopAnim:
                g_localEmote.Stop();
                if (!g_localEmote.Active())
                    join::SendStopAnim();      // nothing local to stop: still clear the server's copy
                GLOGI("engine: /stopanim");
                break;
            case Command::Kill:
                g_world->SetEntityHealth(lp, 0);
                GLOGI("engine: /kill");
                break;
            case Command::Tp:
                g_world->SetEntityCoords(lp, c.x, c.y, c.z);
                GLOGI("engine: /tp %.1f %.1f %.1f", c.x, c.y, c.z);
                break;
            case Command::Fix: {
                int32_t veh = g_world->GetVehiclePedIsIn(lp);
                if (veh) { g_world->FixVehicle(veh); GLOGI("engine: /fix"); }
                else GLOGI("engine: /fix ignored (not in a vehicle)");
                break;
            }
            case Command::Gun: {
                std::string name = c.arg;
                AsciiLower(name);
                if (name.rfind("weapon_", 0) != 0) name = "weapon_" + name;
                if (g_invWorldLive.load()) {
                    // 0.5.4: the server put it into the inventory (the hand when free); its InvSnap gives it to the ped
                    GLOGI("engine: /gun %s: through the inventory (0.5.4), not given locally", name.c_str());
                    break;
                }
                g_world->GiveWeapon(lp, g_world->HashKey(name.c_str()), 250);
                GLOGI("engine: /gun %s", name.c_str());
                break;
            }
            case Command::Car: {
                uint32_t h = g_world->HashKey(c.arg.c_str());
                if (!g_world->IsVehicleModel(h)) {
                    GLOGW("engine: /car '%s' is not a vehicle model", c.arg.c_str());
                    break;
                }
                g_world->RequestModel(h);
                g_pendingCar = { true, h, now };
                GLOGI("engine: /car %s (streaming)", c.arg.c_str());
                break;
            }
            }
        }

        if (g_pendingCar.active) {
            g_world->RequestModel(g_pendingCar.model);
            if (g_world->HasModelLoaded(g_pendingCar.model)) {
                const float rad = heading * 3.14159265f / 180.0f;
                const float vx = px - std::sin(rad) * 5.0f, vy = py + std::cos(rad) * 5.0f;
                int32_t veh = g_world->CreateVehicle(g_pendingCar.model, vx, vy, pz, heading);
                if (veh) {
                    if (g_ownedVehicles.size() >= kMaxOwnedVehicles)
                        g_ownedVehicles.erase(g_ownedVehicles.begin());  // bounded: drop the oldest
                    g_ownedVehicles.push_back(veh);
                    RadioOffNewVehicle(veh);           // 0.4.19: no radio (before the engine starts it)
                    g_world->PlaceVehicleOnGround(veh);
                    g_world->SetVehicleEngineOn(veh, true);
                    g_world->SetPedIntoVehicle(lp, veh, -1);  // driver seat
                    GLOGI("engine: /car spawned vehicle %d", veh);
                }
                g_world->ModelNoLongerNeeded(g_pendingCar.model);
                g_pendingCar = {};
            } else if (now - g_pendingCar.since > 5000) {
                GLOGW("engine: /car model 0x%08x did not stream in 5 s", (unsigned)g_pendingCar.model);
                g_world->ModelNoLongerNeeded(g_pendingCar.model);
                g_pendingCar = {};
            }
        }
    }

    // Push the health and satiety fractions to the overlay, which draws the bars (ImGui, render
    // thread; nothing is drawn from here). GTA's own minimap is hidden (hook.cpp,
    // DISPLAY_RADAR(false)). Script tick only. lp is the local ped, now is GetTickCount().
    // Called every script tick: it also tells the overlay to hide the bars while the player is not
    // alive (death, respawn), which a call only on the alive path could not do.
    void DrawHud(int32_t lp, ULONG now) {
        // Satiety decays slowly (full -> empty in ~30 min).
        if (g_satietyAt) {
            const float dt = (float)(now - g_satietyAt) / 1000.0f;
            g_satiety -= dt * (100.0f / 1800.0f);
            if (g_satiety < 0) g_satiety = 0;
        }
        g_satietyAt = now;
        if (const int add = g_satietyAdd.exchange(0))   // 0.6: food used through the inventory (InvUsed)
            g_satiety = std::fmin(100.0f, g_satiety + (float)add);

        // GTA ped health runs from 100 (dead) up to maxHp (200 for an MP freemode ped), so the
        // fraction is taken over that span: hp 100 is an empty bar, not half of one. A max at or
        // below 100 cannot have that span; fall back to hp / maxHp.
        // The ped maximum is 100 + the server playerHealth (ApplyServerMaxHealth), so the bar follows the server.
        const int hp = g_world->GetEntityHealth(lp);
        const int maxHp = g_world->GetEntityMaxHealth(lp);
        // T6: a frame whose max reads 0 (native failure) draws the last good / the server's maximum, not an empty bar.
        float hpFrac = sync::HudHealthFraction(hp, maxHp, g_playerHealth.load(), g_vitals.LastGoodMax());
        // R3 T1: SCALED (GTA cannot hold 100 + playerHealth): the server's number draws the bar, less the self-damage in flight.
        if (ServerMode() && g_vitals.Scale().Scaled() && !g_vitals.ServerDead())
            hpFrac = sync::ServerBarFraction((int)g_vitals.Health(), g_vitals.Pending(), (int)g_playerHealth.load());
        float satFrac = g_satiety / 100.0f;
        satFrac = satFrac < 0 ? 0 : (satFrac > 1 ? 1 : satFrac);

        // Fuel is server-owned (VehicleState.fuelPermille; no native reads one in 3889: GET_VEHICLE_FUEL_LEVEL is FiveM-only). 0.4.22: the gauge on the
        // speedometer plate shows the level of the networked car we sit in (TickVehicleControls sets g_fuelHud earlier in this tick).
        // 0.4.31: the ring stays up through the death hold (its HP 0 state is designed); the old plate hid at the death as before
        game::Overlay::SetVitals(hpFrac, satFrac, g_fuelHud.frac, g_life == Life::Alive || (g_hudRing.load() && g_life == Life::Dying), g_fuelHud.show);
    }

    // ---- the in-game UI (overlay) -----------------------------------------
    // Data flow: the overlay (render thread, Present detour) only draws what is pushed to it
    // and never calls a native. UiFrame (render thread) pushes the loading phase and runs the
    // overlay frame; UiTick (script tick, where natives are safe) pushes the speed, drains what
    // the player typed or clicked into SendChat, and holds the game's controls off while a
    // panel is open.

    // True while a live session's entry is held by EnterServer's readiness gate (it waits for GTA's
    // own load to finish). Render thread safe: atomics and config only.
    bool EntryWaitsForGame()
    {
        return g_world && g_world->spawnMode != 0 && g_world->ScriptTickActive() && join::IsConnected();
    }

    // True once the player is in the GAGE world (the entry finished), or a timeout says to stop
    // covering the game: 25 s after the local ped appears (the entry never finished: no server,
    // refused, model not streaming) or 4 min from the start when no ped ever appears. While the
    // entry waits for the game's own load the 25 s runs from the moment that wait ends instead,
    // because the ped can exist for most of a minute before the game is ready (the wait itself is
    // capped by kEntryReadyMaxMs).
    bool LoaderDone()
    {
        if (g_entry.load() == Entry::Done)
            return true;
        const ULONGLONG now = GetTickCount64();
        ULONGLONG armed = g_loaderArmed.load();
        if (!armed)
        {
            armed = now;
            g_loaderArmed.store(armed);
        }
        const ULONGLONG since = EntryWaitsForGame() ? g_entryReadyAt.load() : g_loaderPedAt.load();
        if (since && now - since > kLoaderAfterPedMs)
            return true;
        return now - armed > kLoaderMaxMs;
    }

    // What the loader says it is doing: only states the engine really knows.
    void PushLoaderStatus()
    {
        char text[160];
        bool failed = false;     // a dead end: the loader stops its progress animation
        if (g_cfg.host.empty())
        {
            std::snprintf(text, sizeof(text), "No server configured");
            failed = true;
        }
        else if (g_giveUp.load())
        {
            // A version mismatch carries the server's own sentence ("Solar RP was updated. Close the game; ...").
            const std::string why = RefuseText();
            std::snprintf(text, sizeof(text), "%s", why.empty() ? "The server refused the connection" : why.c_str());
            failed = true;
        }
        else if (!join::IsConnected())
            std::snprintf(text, sizeof(text), "Connecting to %s:%d", Narrow(g_cfg.host).c_str(), g_cfg.port);
        else if (!g_loaderPedAt.load() || (EntryWaitsForGame() && !g_entryReadyAt.load()))
            std::snprintf(text, sizeof(text), "Loading the world");
        else
            std::snprintf(text, sizeof(text), "Entering the world");
        game::Overlay::SetLoaderStatus(text, failed);
    }

    // Render thread, once per Present, before the script-tick early return in Frame(): no natives.
    void UiFrame()
    {
        void* sc = game::GetSwapChain();
        if (!sc)
            return;                                   // Present has not run yet: draw nothing
        const bool done = LoaderDone();
        game::Overlay::SetPhase(done ? game::Overlay::Phase::InWorld : game::Overlay::Phase::Loading);
        if (!done)
        {
            // The status changes only on a connect / refuse / entry step: refresh it 10 times a second,
            // not on each of the thousands of Presents a second the loading screen runs at (each push
            // formats, allocates and takes the overlay's data lock).
            static ULONGLONG lastStatus = 0;
            const ULONGLONG t = GetTickCount64();
            if (!lastStatus || t - lastStatus >= 100)
            {
                lastStatus = t;
                PushLoaderStatus();
            }
        }
        else
            RepeatRefusalLine();                      // in the world and refused: keep the reason in chat
        if (!game::Overlay::Ready())
            game::Overlay::Init(sc);                  // backs off by itself after a failure
        game::Overlay::Render(sc);
        game::brand::Poll(sc);                        // the GAGE-MP mark on the tee: D3D11 hooks + readbacks, no natives
        game::radarmask::Poll(sc);                    // 0.5.5 ring_map_round: the census (0.5.6), the readbacks, the pick and the swap (same hooks)
    }

    // Script tick only. lp is the local ped.
    // The no-pause pause menu (TickPauseMenu, below). Script tick only.
    sync::PauseMenuGate g_pauseGate;
    bool g_pauseMenuActive = false;       // IS_PAUSE_MENU_ACTIVE as of the last tick (read while connected or open)
    int g_pauseTimerBase = -1;            // GET_GAME_TIMER when the menu was first seen open (-1 unreadable)
    double g_pauseOpenedAtMs = 0;         // NowMs() then

    void TickInputHolds(int32_t lp);   // 0.5.6, below

    void UiTick(int32_t lp)
    {
        // Speedometer: shown only while the player is in a vehicle.
        const bool inVeh = g_world->IsPedInVehicle(lp);
        float kmh = 0.0f;
        if (inVeh)
        {
            const int32_t veh = g_world->GetVehiclePedIsIn(lp);
            kmh = g_world->GetEntitySpeed(veh ? veh : lp) * 3.6f;   // m/s -> km/h
        }
        game::Overlay::SetStats(inVeh, kmh);

        // What the player typed or clicked: /commands run locally, anything else goes to the server.
        char line[256];                               // the protocol caps a chat line at 200 bytes
        for (int n = 0; n < 8 && game::Overlay::PopOutgoing(line, sizeof(line)); ++n)
        {
            if (line[0] != '/' && !join::IsConnected())
            {
                game::Overlay::PushSystemLine("Not connected: your message was not sent.");
                continue;
            }
            gage::engine::SendChat(line);
        }

        // An open panel owns the keyboard and mouse: hold the game's pad-0 controls disabled (this
        // frame only, so it is repeated every tick). The loop (not DISABLE_ALL_CONTROL_ACTIONS, which the
        // 0.5.6 natlookup run found in both crossmaps after all) keeps the pause menu's keys exempt below.
        // While the pause menu (opened by TickPauseMenu, not pausing) is up, 199 / 200 / 202 stay
        // enabled: Esc, P and Backspace are how the player closes it.
        // 0.5.4 overlay_combat_block (sync/overlay_input.h): the same loop also holds the attack / melee / weapon controls above 226 (INPUT_ATTACK2
        // 257 is the left mouse button too: GI's shots fired through the Tab inventory).
        if (game::Overlay::Interactive())
        {
            int held[sync::overlayinput::kMaxHeld];
            const int n = sync::overlayinput::HeldControls(g_overlayCombatBlock.load(), g_pauseMenuActive, held, sync::overlayinput::kMaxHeld);
            for (int i = 0; i < n; ++i)
            {
                uint64_t args[] = { 0, (uint64_t)held[i], 1 };  // DISABLE_CONTROL_ACTION(0, control, true)
                g_world->CallNative(game::native::DISABLE_CONTROL_ACTION, args, 3, nullptr);
            }
            if (!g_overlayCombatLogged)
            {
                g_overlayCombatLogged = true;
                GLOGI("engine: overlay input (0.5.4): a panel is open, %d pad-0 controls held disabled each tick (overlay_combat_block=%s)", n,
                      g_overlayCombatBlock.load() ? "on: 0-226 plus attack2 257, weapon 261/262, melee 263/264, sniper zoom 274-277, veh fly attack2 331, "
                                                    "veh melee 345-347"
                                                  : "OFF: 0-226 only, the 0.5.3 set");
            }
        }
        TickInputHolds(lp);
    }

    // 0.5.6 (libs/sync/overlay_input.h), script tick, right after the panel's control loop: the player-level fire block while a panel is open,
    // GTA's own weapon selection held while the server owns the weapons, and the counters that prove both live (and say how many game frames
    // each script tick covers: the per-frame natives above hold nothing on a frame the tick skipped).
    void TickInputHolds(int32_t lp)
    {
        namespace oi = sync::overlayinput;
        InputHoldDiag& d = g_inHold;
        const double nowMs = NowMs();
        const bool panel = game::Overlay::Interactive();
        const oi::TickPlan plan = oi::Plan(panel, g_overlayFireBlock.load(), g_weaponKeys.load(), g_invWorldLive.load());
        const int64_t frame = g_world->GetFrameCount();
        d.cadence.Note(frame);
        if (plan.disableFiring) {
            ++d.fireCalls;
            if (!g_world->DisableLocalPlayerFiring())
                ++d.fireFailed;
        }
        if (plan.holdPanel) {
            ++d.panelTicks;
            if (frame >= 0 && d.panelFrameAt >= 0 && frame > d.panelFrameAt)
                d.panelFrames += (uint32_t)std::min<int64_t>(frame - d.panelFrameAt, 1000);
            d.panelFrameAt = frame;
            // What got through anyway: a left click the game saw (read from the disabled control), a shot, a melee move.
            if (g_world->IsDisabledControlJustPressed(0, 24))
                ++d.lmbHeld;
            if (g_world->IsPedShooting(lp))
                ++d.shotsOpen;
            if (g_world->IsPedPerformingMeleeAction(lp))
                ++d.meleeOpen;
        } else {
            d.panelFrameAt = -1;
        }
        if (plan.holdWeaponKeys) {
            ++d.keyTicks;
            for (int i = 0; i < oi::kGameWeaponSelectCount; ++i) {
                uint64_t args[] = { 0, (uint64_t)oi::kGameWeaponSelect[i], 1 };   // DISABLE_CONTROL_ACTION(0, control, true)
                g_world->CallNative(game::native::DISABLE_CONTROL_ACTION, args, 3, nullptr);
            }
            if (!g_world->SuppressWeaponWheelResults())
                ++d.wheelFailed;
            if (!d.logged) {
                d.logged = true;
                GLOGI("engine: input holds (0.5.6): the server owns the weapons: %d GTA weapon-select controls held every tick (Tab 37, wheel 12-17, "
                      "number keys 157-165, vehicle 99 / 100 / 115 / 116, mouse wheel 261 / 262) + HUD_SUPPRESS_WEAPON_WHEEL_RESULTS_THIS_FRAME; "
                      "1-4 are the quick slots", oi::kGameWeaponSelectCount);
            }
        }
        else {
            d.logged = false;                      // the next session that hands the weapons to the server logs it again
        }
        const uint32_t from = d.sw.last;
        if (d.sw.Note(g_world->GetSelectedPedWeapon(lp), nowMs)) {
            d.foreignFrom = from;
            d.foreignTo = d.sw.last;
        }
        if (nowMs - d.lastLogMs < 10000)
            return;
        d.lastLogMs = nowMs;
        uint32_t qSent = 0, qRefused = 0;
        game::Overlay::QuickKeyCounts(&qSent, &qRefused);
        if (d.panelTicks || d.keyTicks || d.sw.foreign || d.sw.ours || qSent || qRefused) {
            char last[48] = "";
            if (d.sw.foreign)
                std::snprintf(last, sizeof last, " (last 0x%08X -> 0x%08X)", (unsigned)d.foreignFrom, (unsigned)d.foreignTo);
            GLOGI("engine: input holds (0.5.6), last 10 s: cadence %u script ticks over %u game frames (ticks covering more than 1 frame %u, most "
                  "%u, GET_FRAME_COUNT unreadable %u); panel open %u ticks / %u frames: DISABLE_PLAYER_FIRING %u (%u failed), left clicks the game "
                  "saw held %u, got through: shots %u, melee %u; weapon keys held %u ticks (wheel suppress failed %u); ped weapon switches: ours %u, "
                  "FOREIGN %u%s; quick keys (page closed) sent %u, refused %u",
                  d.cadence.ticks, d.cadence.frames, d.cadence.gapsOver1, d.cadence.maxGap, d.cadence.unreadable, d.panelTicks, d.panelFrames,
                  d.fireCalls, d.fireFailed, d.lmbHeld, d.shotsOpen, d.meleeOpen, d.keyTicks, d.wheelFailed, d.sw.ours, d.sw.foreign, last, qSent,
                  qRefused);
        }
        d.cadence.ResetWindow();
        d.panelTicks = d.panelFrames = d.fireCalls = d.fireFailed = d.lmbHeld = d.shotsOpen = d.meleeOpen = 0;
        d.keyTicks = d.wheelFailed = 0;
        d.sw.ours = d.sw.foreign = 0;
    }

    // Held disabled on pad 0 every script tick, from the very first one (before the player or the
    // entry exist) and for good: 244 the interaction (M) menu, 19 the character-switch wheel and
    // 166-169 the character-select F-keys (Michael / Franklin / Trevor / Multiplayer). During the
    // game's own load they would otherwise open the menu, switch character or start GTA Online.
    // 243 is INPUT_ENTER_CHEAT_CODE (the ~ key's cheat entry box): no single-player cheats on a server.
    // 199 (P) and 200 (Esc) are NOT here any more: while connected TickPauseMenu holds them disabled
    // and opens the pause menu itself without pausing the world (the game's own menu stops the script
    // thread and our sender, so the other players saw us frozen); offline they keep their normal job.
    // DISABLE_CONTROL_ACTION is a per-frame effect, hence every tick.
    void HoldMenuControls()
    {
        static const uint64_t kHeld[] = { 244, 19, 166, 167, 168, 169, 243 };
        for (uint64_t control : kHeld)
        {
            uint64_t args[] = { 0, control, 1 };  // DISABLE_CONTROL_ACTION(0, control, true)
            g_world->CallNative(game::native::DISABLE_CONTROL_ACTION, args, 3, nullptr);
        }
    }

    // The no-pause pause menu (sync/pausemenu.h has the rules and the sources). Every script tick,
    // right after HoldMenuControls. While connected and the menu is closed: 199 and 200 are held
    // disabled and the game's own open is blocked this frame (DISABLE_FRONTEND_THIS_FRAME; the
    // SET_PAUSE_MENU_ACTIVE(false) fallback if that native cannot be called); a press of either,
    // still readable from the disabled control, opens it with ACTIVATE_FRONTEND_MENU(
    // FE_MENU_VERSION_SP_PAUSE, togglePause=false, -1). While it is open nothing is disabled so Esc /
    // P / Backspace close it, and one proof line per 2 s shows the world keeps running.
    // 0.5.8: while the inventory's ped preview owns the frontend (g_ppOwnsFrontend, from its claim until the frontend reads closed again)
    // that frontend is not the player's pause menu: menuActive stays false (199 / 200 stay held, g_pauseMenuActive stays false for the
    // radar, the name tags and the input holds) and the gate neither blocks it nor counts it (PauseMenuIn::frontendIsOurs).
    void TickPauseMenu(double nowMs)
    {
        const bool connected = join::IsConnected();
        const bool ours = g_ppOwnsFrontend;
        const bool menuActive = !ours && (connected || g_pauseMenuActive) && g_world->IsPauseMenuActive();
        sync::PauseMenuIn in;
        in.nowMs = nowMs;
        in.connected = connected;
        in.canOpen = ServerMode();
        in.menuActive = menuActive;
        in.frontendIsOurs = ours;
        // The Alt+F4 card counts as a panel here only: its Esc must not also open the pause menu (the
        // gate's debounce covers the tick after the WndProc closed it). It takes no game controls.
        in.overlayInteractive = game::Overlay::Interactive() || game::Overlay::QuitPromptOpen();
        if (sync::PauseMenuGate::ControlsHeld(connected, menuActive))
        {
            // Disable first: IS_DISABLED_CONTROL_JUST_PRESSED reads the control in its disabled state.
            for (const int control : { sync::kControlFrontendPause, sync::kControlFrontendPauseAlt })
            {
                uint64_t args[] = { 0, (uint64_t)control, 1 };  // DISABLE_CONTROL_ACTION(0, control, true)
                g_world->CallNative(game::native::DISABLE_CONTROL_ACTION, args, 3, nullptr);
            }
            in.openPressed = g_world->IsDisabledControlJustPressed(0, sync::kControlFrontendPause) ||
                             g_world->IsDisabledControlJustPressed(0, sync::kControlFrontendPauseAlt);
        }
        const sync::PauseMenuOut out = g_pauseGate.Tick(in);
        g_pauseMenuActive = menuActive;
        if (out.disableFrontend && !g_world->DisableFrontendThisFrame())
            g_world->SetPauseMenuActive(false);
        if (out.activate)
        {
            const bool ok = g_world->ActivateFrontendMenu(sync::kFeMenuVersionSpPause, false, -1);
            GLOGI("engine: pause menu: opening without pausing (ACTIVATE_FRONTEND_MENU 0x%08x, togglePause=false) %s",
                  (unsigned)sync::kFeMenuVersionSpPause, ok ? "called" : "FAILED (native not callable)");
        }
        if (out.justOpened)
        {
            g_pauseTimerBase = g_world->GetGameTimer();
            g_pauseOpenedAtMs = nowMs;
        }
        if (out.logProof)
        {
            const int timer = g_world->GetGameTimer();
            float speed = 0.0f;
            if (const int32_t lp = g_world->LocalPlayer())
            {
                int32_t e = lp;
                if (g_world->IsPedInVehicle(lp))
                    if (const int32_t veh = g_world->GetVehiclePedIsIn(lp))
                        e = veh;
                speed = g_world->GetEntitySpeed(e);
            }
            GLOGI("engine: pause menu open: game timer +%d ms, local speed %.2f m/s (wall clock +%.0f ms)",
                  timer >= 0 && g_pauseTimerBase >= 0 ? timer - g_pauseTimerBase : -1, speed, nowMs - g_pauseOpenedAtMs);
        }
        if (out.justClosed)
            GLOGI("engine: pause menu closed after %.1f s", (nowMs - g_pauseOpenedAtMs) / 1000.0);
    }

    // ---- 0.4.22 minimap (GI: "add a minimap"). sync/minimap.h decides; this is the only DISPLAY_RADAR caller. Script tick only. ----
    // Every tick (from the first one with a ped): DISPLAY_RADAR(wanted), GTA's bars hidden through the "minimap" movie while it shows, and passive read-backs
    // every 100 ms. The 10 s line counts what the read-backs said once a decision has stood for kMiniSettleMs (the render side lags a toggle by a frame or two).
    struct MinimapStats
    {
        uint32_t ticks = 0, wantedTicks = 0, radarOn = 0, radarOff = 0, radarUncallable = 0;
        uint32_t samples = 0, hudHidden = 0, prefOff = 0, waypointSamples = 0, readUnresolved = 0;
        uint32_t wSettled = 0, wHiddenTrue = 0, wRenderFalse = 0;     // radar wanted: IS_RADAR_HIDDEN reading true / IS_MINIMAP_RENDERING reading false are the bad answers
        uint32_t uSettled = 0, uHiddenFalse = 0;                        // radar not wanted: IS_RADAR_HIDDEN reading false is the bad answer
        uint32_t setupOk = 0, setupBeginRefused = 0, barsUncallable = 0, requestFailed = 0;
        uint32_t zoomCalls = 0, zoomUncallable = 0;                     // 0.5.4 ring_map_zoom
        uint32_t bigmapOffCalls = 0, bigmapOffUncallable = 0;           // 0.5.8 radar_bigmap_off
    };
    constexpr double kMiniSettleMs = 500.0, kMiniSampleMs = 100.0, kMiniLogMs = 10000.0, kMiniSafeZoneMs = 2000.0;
    MinimapStats g_mini;
    bool g_ringZoomLogged = false;            // 0.5.4: the first SET_RADAR_ZOOM's line (script tick only)
    bool g_ringZoomHeld = false;              // 0.5.4: our level was set last tick (SET_RADAR_ZOOM(0) once when the ring map stops showing)
    float g_miniSafe = 1.0f;                  // GET_SAFE_ZONE_SIZE (read every kMiniSafeZoneMs; the player can change it in the settings)
    bool g_miniSafeOk = false;
    double g_miniSafeMs = -1e18;
    bool g_miniWanted = false;
    double g_miniChangeMs = 0, g_miniSampleMs = 0, g_miniLogMs = 0;
    int g_miniMovie = 0;                      // the "minimap" scaleform handle (0 = not yet requested / refused)
    bool g_miniMovieLoaded = false;           // HAS_SCALEFORM_MOVIE_LOADED at the last check
    int g_miniTransitions = 0;                // radar on / off changes logged (capped)

    // Where GTA puts the radar against what our overlay drew last frame. The persistent plates (chat, mark, speedometer, vitals) must stay clear of it; the Tab
    // menu is modal (the scrim dims the radar under it), so it is reported apart.
    std::string MinimapClearance()
    {
        using game::Overlay::UiState;
        // LastUiState copies a struct the render thread rewrites each frame: take it twice and use it only when the fields we read agree.
        const UiState a = game::Overlay::LastUiState();
        const UiState b = game::Overlay::LastUiState();
        auto same = [](float x, float y) { return x == y; };
        if (!a.valid || !same(a.displayW, b.displayW) || !same(a.displayH, b.displayH) || !same(a.chatX, b.chatX) || !same(a.chatH, b.chatH) ||
            !same(a.logoX, b.logoX) || !same(a.speedoX, b.speedoX) || !same(a.vitalsX, b.vitalsX) || !same(a.invX, b.invX))
            return "overlay layout not read (no frame drawn yet, or it changed under the read)";
        const float sz = g_miniSafe;             // the value the overlay was given (TickMinimap reads it every 2 s)
        const bool szOk = g_miniSafeOk;
        const sync::ScreenRect keep = sync::RadarKeepOut(a.displayW, a.displayH, sz, szOk, 0.02f, g_miniAppliedScale);
        const sync::ScreenRect radar = sync::RadarKeepOut(a.displayW, a.displayH, sz, szOk, 0.0f, g_miniAppliedScale);
        auto rect = [](float x, float y, float w, float h) { sync::ScreenRect r; r.x0 = x; r.y0 = y; r.x1 = x + w; r.y1 = y + h; return r; };
        std::string hit;
        auto test = [&](const char* name, bool present, const sync::ScreenRect& r) {
            if (present && sync::RectsOverlap(keep, r))
                hit += (hit.empty() ? "" : ", ") + std::string(name);
        };
        test("chat", a.chatW > 0.0f && a.chatH > 0.0f, rect(a.chatX, a.chatY, a.chatW, a.chatH));
        test("mark", a.logoW > 0.0f, rect(a.logoX, a.logoY, a.logoW, a.logoH));
        test("speedometer", a.speedoVisible, rect(a.speedoX, a.speedoY, a.speedoW, a.speedoH));
        // The vitals plate is meant to touch the radar while it is docked under it (it takes its left edge and width); only a plate that
        // stayed in the bottom-right cluster is tested like the other plates.
        test("vitals", a.vitalsVisible && !a.vitalsDocked, rect(a.vitalsX, a.vitalsY, a.vitalsW, a.vitalsH));
        const bool tabOver = a.invOpen && sync::RectsOverlap(keep, rect(a.invX, a.invY, a.invW, a.invH));
        char buf[900];
        std::snprintf(buf, sizeof(buf),
                      "overlay %s the radar (display %.0fx%.0f, safe zone %.2f%s, radar x %.0f-%.0f y %.0f-%.0f, kept clear incl. a 2 %% gutter x %.0f-%.0f y %.0f-%.0f)%s%s%s",
                      hit.empty() ? "CLEAR of" : "OVERLAPS", (double)a.displayW, (double)a.displayH, (double)sync::SafeZoneClamped(sz, szOk),
                      szOk ? " read" : " UNREADABLE, worst case taken", (double)radar.x0, (double)radar.x1, (double)radar.y0, (double)radar.y1,
                      (double)keep.x0, (double)keep.x1, (double)keep.y0, (double)keep.y1, hit.empty() ? "" : ": ", hit.c_str(),
                      a.invOpen ? (tabOver ? "; Tab menu open and over the radar (modal, dimmed)" : "; Tab menu open, clear") : "");
        std::string out = buf;
        // Where the vitals plate went and why (the overlay's own layout, read back).
        const char* why = a.vitalsDockWhy >= 0 && a.vitalsDockWhy <= (int)sync::DockWhy::CoversRadar ? sync::DockWhyName((sync::DockWhy)a.vitalsDockWhy)
                                                                                                     : "dock not asked";
        if (!a.vitalsVisible)
            out += "; vitals plate hidden now (" + std::string(why) + ")";
        else
        {
            char vb[360];
            std::snprintf(vb, sizeof(vb), "; vitals plate %s at x %.0f-%.0f y %.0f-%.0f (%s)", a.vitalsDocked ? "DOCKED under the radar" : "bottom-right",
                          (double)a.vitalsX, (double)(a.vitalsX + a.vitalsW), (double)a.vitalsY, (double)(a.vitalsY + a.vitalsH), why);
            out += vb;
            if (a.vitalsDocked)
            {
                const float rh = a.radarH > 0.0f ? a.radarH : (radar.y1 - radar.y0);
                std::snprintf(vb, sizeof(vb), "; it reaches %.0f px (%.0f %%) up into the radar, radar left edge %.0f width %.0f, plate left edge %.0f width %.0f",
                              (double)a.vitalsCoverPx, rh > 0.0f ? (double)(100.0f * a.vitalsCoverPx / rh) : 0.0, (double)a.radarX, (double)a.radarW,
                              (double)a.vitalsX, (double)a.vitalsW);
                out += vb;
            }
        }
        return out;
    }

    void LogMinimap(double nowMs, bool wanted)
    {
        MinimapStats& s = g_mini;
        const sync::BarsGate::Stats& bs = g_barsGate.Counters();
        const sync::BarsMode bm = g_barsGate.Mode();
        const std::string clearance = wanted ? MinimapClearance() : std::string("radar not shown now");
        GLOGI("engine: minimap (0.4.22), last 10 s: minimap=%s minimap_bars=%s; radar wanted %u of %u ticks (DISPLAY_RADAR true %u / false %u / uncallable %u); "
              "%u read-back samples, those %.0f ms after a change: radar wanted %u -> IS_RADAR_HIDDEN true %u (expect 0), IS_MINIMAP_RENDERING false %u (expect 0); "
              "radar not wanted %u -> IS_RADAR_HIDDEN false %u (expect 0); IS_HUD_HIDDEN true in %u; profile radar preference OFF in %u (the game draws no radar then); "
              "natives unresolved %u; waypoint active in %u samples; bars: movie handle %d %s, SETUP_HEALTH_ARMOUR called %u (begin refused %u, uncallable %u), "
              "movie requests %u (failed %u), big-radar flip on %u / off %u, left on %s (no getter shows the bars themselves); %s",
              g_minimap.load() ? "on" : "OFF", sync::BarsModeName(bm), s.wantedTicks, s.ticks, s.radarOn, s.radarOff, s.radarUncallable, s.samples, kMiniSettleMs,
              s.wSettled, s.wHiddenTrue, s.wRenderFalse, s.uSettled, s.uHiddenFalse, s.hudHidden, s.prefOff, s.readUnresolved, s.waypointSamples, g_miniMovie,
              g_miniMovieLoaded ? "loaded" : "NOT loaded", s.setupOk, s.setupBeginRefused, s.barsUncallable, bs.requests, s.requestFailed, bs.refreshOn,
              bs.refreshOff, g_barsGate.BigmapOn() ? "YES" : "no", clearance.c_str());
        const uint32_t zoomCalls = s.zoomCalls, zoomUncallable = s.zoomUncallable, offCalls = s.bigmapOffCalls, offUncallable = s.bigmapOffUncallable;
        g_barsGate.ClearCounters();
        s = MinimapStats();
        GLOGI("engine: minimap size (0.4.25), last 10 s: %s; radar rectangle scale %.2f (what the overlay's vitals strip and the clearance check use); "
              "ring map zoom (0.5.4) SET_RADAR_ZOOM(%d) calls %u, uncallable %u; expanded radar held off (0.5.8 radar_bigmap_off=%s) SET_BIGMAP_ACTIVE(false) "
              "calls %u, uncallable %u",
              game::minimapsize::Status().c_str(), (double)g_miniAppliedScale, g_ringMapZoom.load(), zoomCalls, zoomUncallable,
              g_radarBigmapOff.load() ? "on" : "OFF", offCalls, offUncallable);
    }

    // ---- 0.5.5 ring_map_round (libs/sync/ring_round.h, apps/engine/radar_mask.h) -------------------------------------------------------------
    // The script half of the round mask: the sizes of the game's two radar masks (GET_TEXTURE_RESOLUTION), the census (0.5.6: radar_mask.cpp
    // keeps every view of those sizes the game binds and picks by content; the radar is NOT hidden, each mask is drawn as an invisible sprite
    // meanwhile so the game binds it), and the gate that lets the placement grow only while the bind hook keeps handing the radar our circle.
    // Script tick only.
    enum class RoundStep { Idle, Sizing, Census, Done, GaveUp };
    const char* RoundStepName(RoundStep s)
    {
        switch (s)
        {
        case RoundStep::Idle: return "idle";
        case RoundStep::Sizing: return "asking the mask sizes";
        case RoundStep::Census: return "census";
        case RoundStep::Done: return "done";
        case RoundStep::GaveUp: return "gave up";
        }
        return "?";
    }
    struct RoundState
    {
        RoundStep step = RoundStep::Idle;
        int runs = 0, sizeTries = 0;
        double nextMs = 0.0, stepMs = 0.0;
        const char* dict = nullptr;              // the dictionary name GET_TEXTURE_RESOLUTION knew
        uint32_t size[2][2] = {};                // radarmasksm / radarmasklg w x h (0 = unknown)
        std::string outcome = "not started";     // the last census's result, for the 10 s line
        sync::ringround::RoundGate gate;
        sync::ringround::RoundGateOut out;       // the gate's verdict of the last tick (TickMinimapSize places from it on the next)
        bool cfgRound = false;                   // the placement handed to minimapsize is the grown one
        bool written = false;                    // ... and has been written since it was handed over
        double cfgMs = 0.0;
        bool landed = false;                     // written, read back, not in the big-radar flip (what the round hole and a shrink wait for)
        double shrinkSinceMs = -1.0;
        bool shrinkStuck = false;                // a shrink that never landed: our masks stay handed out from then on (logged)
        bool active = false;                     // our masks handed out (radarmask::SetActive)
        uint32_t activeCalls = 0;
        bool shown = false;
        double shownSinceMs = 0.0;
        uint64_t bindsAtLog = 0, substAtLog = 0, bigBindsAtLog = 0;
        uint32_t transitionsLogged = 0;
        bool probesRearmed = false;              // the two radar probe images retaken once the map first went round (radar_probe_3 / _4)
        double roundOpenMs = 0.0;
        int roundProbes = 0;
    };
    RoundState g_round;
    // 0.5.6: a census hides nothing, so a failed one is retried (3 runs, 20 s apart); one run reads back every kept view (dozens at 512 x 512,
    // one per render frame) inside kRoundCensusTimeoutMs
    constexpr int kRoundMaxRuns = 3, kRoundMaxSizeTries = 4;
    constexpr double kRoundRetryMs = 20000.0, kRoundCensusTimeoutMs = 60000.0, kRoundShrinkMaxMs = 3000.0;
    constexpr const char* kRoundDicts[2] = {"platform:/textures/graphics", "graphics"};

    // The size of `name` in the first dictionary GET_TEXTURE_RESOLUTION knows it in (the game answers about 4 x 4 for a texture it has not got).
    bool RoundTextureSize(const char* name, const char*& dict, uint32_t& w, uint32_t& h)
    {
        for (const char* d : kRoundDicts)
        {
            if (dict && d != dict)
                continue;
            float fw = 0.0f, fh = 0.0f;
            if (!g_world->TextureResolution(d, name, fw, fh))
                return false;
            if (fw > 4.5f && fh > 4.5f && fw < 8192.0f && fh < 8192.0f)
            {
                dict = d;
                w = (uint32_t)(fw + 0.5f);
                h = (uint32_t)(fh + 0.5f);
                return true;
            }
        }
        return false;
    }

    // Returns true while the radar must stay hidden: only while a shrink back to 0.5.4 has not landed (RoundGate; the census never hides it).
    // wantedBase: the radar would show this tick.
    bool TickRingRound(double nowMs, bool wantedBase)
    {
        namespace rr = sync::ringround;
        namespace rm = game::radarmask;
        RoundState& R = g_round;
        const bool on = g_hudRing.load() && g_ringMap.load() && g_ringMapRound.load();
        const rm::Status st = rm::GetStatus();
        auto fail = [&](const std::string& why) {
            ++R.runs;
            rm::Cancel();
            R.outcome = why;
            R.step = R.runs >= kRoundMaxRuns ? RoundStep::GaveUp : RoundStep::Idle;
            R.nextMs = nowMs + kRoundRetryMs;
            GLOGW("engine: ring map round mask (0.5.6): census run %d %s: %s", R.runs, R.step == RoundStep::GaveUp ? "failed, GIVING UP (the 0.5.4 map stays)" : "failed, retrying in 20 s",
                  why.c_str());
        };
        if (on && !st.notD3D11 && st.hooks)
        {
            switch (R.step)
            {
            case RoundStep::Idle:
            {
                const bool needSmall = !rm::Live(rm::kSmall);
                const bool needBig = !rm::Live(rm::kBig);
                if ((needSmall || needBig) && wantedBase && nowMs >= R.nextMs && R.runs < kRoundMaxRuns)
                {
                    R.step = RoundStep::Sizing;
                    R.stepMs = nowMs;
                }
                break;
            }
            case RoundStep::Sizing:
            {
                const char* dict = nullptr;
                uint32_t w = 0, h = 0;
                if (!RoundTextureSize(rm::kTexName[0], dict, w, h))
                {
                    if (++R.sizeTries >= kRoundMaxSizeTries)
                    {
                        R.runs = kRoundMaxRuns - 1;   // nothing a retry would change
                        fail("GET_TEXTURE_RESOLUTION knows no radarmasksm in \"platform:/textures/graphics\" or \"graphics\" (or the native is not callable)");
                    }
                    else
                    {
                        R.step = RoundStep::Idle;
                        R.nextMs = nowMs + 2000.0;
                    }
                    break;
                }
                R.dict = dict;
                R.size[0][0] = w;
                R.size[0][1] = h;
                uint32_t bw = 0, bh = 0;
                if (RoundTextureSize(rm::kTexName[1], dict, bw, bh) && !(bw == w && bh == h))
                {
                    R.size[1][0] = bw;
                    R.size[1][1] = bh;
                }
                GLOGI("engine: ring map round mask (0.5.6): GET_TEXTURE_RESOLUTION(\"%s\"): radarmasksm %ux%u, radarmasklg %s; census run %d (the radar stays shown)",
                      R.dict, w, h, R.size[1][0] ? (std::to_string(R.size[1][0]) + "x" + std::to_string(R.size[1][1])).c_str() : "unknown (not looked for)",
                      R.runs + 1);
                const bool smallLive = rm::Live(rm::kSmall), bigLive = rm::Live(rm::kBig);
                // the first run logs every candidate's analysis, later ones only the mask-like views and the failures
                if (!rm::RequestCensus(smallLive ? 0 : R.size[0][0], smallLive ? 0 : R.size[0][1], bigLive ? 0 : R.size[1][0], bigLive ? 0 : R.size[1][1], R.runs == 0))
                {
                    fail("the census could not be requested (radar_mask busy, or nothing to look for)");
                    break;
                }
                R.step = RoundStep::Census;
                R.stepMs = nowMs;
                break;
            }
            case RoundStep::Census:
            {
                if (st.phase == rm::Phase::Requested || st.phase == rm::Phase::Recording)
                {
                    // each mask still looked for as an invisible sprite (alpha 1 of 255, 1 % of the screen, under the ring's glass), so the game binds
                    // it while the census records even if the radar's own draw does not
                    const game::Overlay::UiState ui = game::Overlay::LastUiState();
                    const float H = ui.valid && ui.displayH > 0.0f ? ui.displayH : 1080.0f, W = ui.valid && ui.displayW > 0.0f ? ui.displayW : 1920.0f;
                    const sync::hudring::Pt c = sync::hudring::MapCentrePx(H);
                    for (int kind = 0; kind < 2; ++kind)
                        if (R.size[kind][0] && !rm::Live(kind))
                            g_world->DrawSprite(R.dict, rm::kTexName[kind], c.x / W, c.y / H, 0.01f, 0.01f, 1);
                }
                if (st.phase == rm::Phase::Done)
                {
                    const bool smallOk = rm::Live(rm::kSmall);
                    const bool bigOk = rm::Live(rm::kBig) || !R.size[1][0] || !g_ringMapBig.load();
                    R.outcome = "radarmasksm: " + st.k[0].why + (st.k[1].probed ? "; radarmasklg: " + st.k[1].why : "");
                    if (smallOk && bigOk)
                    {
                        R.step = RoundStep::Done;
                        GLOGI("engine: ring map round mask (0.5.6): census run %d done: %s", R.runs + 1, R.outcome.c_str());
                    }
                    else
                        fail(R.outcome + (smallOk ? " (radarmasklg is needed while ring_map_big mirrors the placement)" : ""));
                }
                else if (st.phase == rm::Phase::Failed)
                    fail("the census failed (" + st.why + ")");
                else if (nowMs - R.stepMs > kRoundCensusTimeoutMs)
                    fail(std::string("the census did not finish in 60 s (") + rm::PhaseName(st.phase) + ": " + st.why + ")");
                break;
            }
            case RoundStep::Done:
                // a device change kills the replacements: look again
                if (!rm::Live(rm::kSmall))
                {
                    R.step = RoundStep::Idle;
                    R.runs = 0;
                    R.sizeTries = 0;
                    R.outcome = "the replacement died (device change?): looking again";
                }
                break;
            case RoundStep::GaveUp:
                break;
            }
        }
        else if (R.step == RoundStep::Census || R.step == RoundStep::Sizing)
        {
            rm::Cancel();   // switched off (or no hooks) in the middle of a census
            R.step = RoundStep::Idle;
        }

        // the gate (libs/sync/ring_round.h RoundGate): what was configured last has landed when it was written, the table still holds it and
        // the big-radar flip that re-reads it is over
        R.landed = R.written && game::minimapsize::PlacementApplied() && !g_barsGate.BigmapOn();
        // shown, for the staleness rule: not during our own big-radar flip (the radar draws with radarmasklg then, and re-lays itself out), and
        // a fresh grace after it
        const bool shownBase = wantedBase && !g_barsGate.BigmapOn();
        if (shownBase && !R.shown)
            R.shownSinceMs = nowMs;
        R.shown = shownBase;
        rr::RoundInputs in;
        in.switchOn = on;
        in.d3d11 = st.hooks && !st.notD3D11;
        in.smallReplaced = game::radarmask::Live(game::radarmask::kSmall);
        in.bigReplaced = game::radarmask::Live(game::radarmask::kBig);
        in.bigMirror = g_ringMapBig.load();
        const game::brand::swap::SwapCounters sc = game::brand::swap::Counters(st.k[0].handle);
        const game::brand::swap::SwapCounters bcIn = game::brand::swap::Counters(st.k[1].handle);
        // 0.5.8: the binds of EITHER identified mask (0.5.7 live: the radar bound only radarmasklg, so the small-only count never confirmed)
        in.maskBinds = rr::MaskBinds(in.smallReplaced, sc.seen, in.bigReplaced, bcIn.seen);
        in.lastBindMs = -1.0;
        // the last bind of EITHER identified mask (the player's own big radar draws with radarmasklg, and that is no reason to shrink)
        ULONGLONG lastSeen = in.smallReplaced ? sc.lastSeenTick : 0;
        if (in.bigReplaced)
        {
            const ULONGLONG b = bcIn.lastSeenTick;
            if (b > lastSeen)
                lastSeen = b;
        }
        if (lastSeen)
        {
            const ULONGLONG t = GetTickCount64();
            in.lastBindMs = nowMs - (double)(t > lastSeen ? t - lastSeen : 0);
        }
        in.radarShown = shownBase;
        in.shownSinceMs = R.shownSinceMs;
        in.nowMs = nowMs;
        bool landed = R.landed;
        if (R.out.shrinking && !landed && R.shrinkSinceMs >= 0.0 && nowMs - R.shrinkSinceMs > kRoundShrinkMaxMs)
        {
            // the 0.5.4 placement did not land in time (minimapsize gave up?): stop hiding the radar, keep our circle handed out so whatever box
            // the game holds is at least round under the annulus
            landed = true;
            if (!R.shrinkStuck)
                GLOGW("engine: ring map round mask (0.5.6): the shrink back to 0.5.4 did not land in %.0f ms (%s); the radar shows again with our circle "
                      "kept on", kRoundShrinkMaxMs, game::minimapsize::Status().c_str());
            R.shrinkStuck = true;
        }
        const rr::RoundGateOut o = R.gate.Update(in, landed);
        if (o.changed)
        {
            R.shrinkSinceMs = o.round ? -1.0 : nowMs;
            if (R.transitionsLogged < 16)
            {
                ++R.transitionsLogged;
                GLOGI("engine: ring map round mask (0.5.6): map %s: %s (binds of the masks %llu: small %llu, big %llu; small substitutions %llu, read back %u ok / %u missed; "
                      "big substitutions %llu)",
                      o.round ? "GROWS to the arcs" : "back to the 0.5.4 size (radar held hidden until it lands)", rr::RoundWhyName(o.why),
                      (unsigned long long)in.maskBinds, (unsigned long long)sc.seen, (unsigned long long)bcIn.seen, (unsigned long long)sc.subst, sc.verified,
                      sc.verifyMiss, (unsigned long long)bcIn.subst);
            }
        }
        R.out = o;
        // our circle is handed out once the grown placement is written (before the flip re-reads it), and until a shrink has landed
        const bool active = (o.round && R.cfgRound && R.written) || o.shrinking || R.shrinkStuck;
        if (active != R.active)
        {
            R.active = active;
            ++R.activeCalls;
            const int n = game::radarmask::SetActive(active);
            if (R.activeCalls <= 16)
                GLOGI("engine: ring map round mask (0.5.6): our circle %s (%d mask(s))", active ? "HANDED OUT" : "withdrawn, the game's own mask is bound", n);
        }
        game::minimapsize::SetBlipClipRound(g_ringBlipClip.load() && o.round && R.cfgRound && R.landed);
        return o.hold;
    }

    // The round hole in the overlay: the grown placement has landed and our circle is handed out.
    bool RingRoundOpen()
    {
        return g_round.out.round && g_round.cfgRound && g_round.landed && g_round.active;
    }

    // The 10 s line (TickMinimap, right after LogMinimap's).
    void LogRingRound(float displayH)
    {
        namespace rr = sync::ringround;
        RoundState& R = g_round;
        const game::radarmask::Status st = game::radarmask::GetStatus();
        const game::brand::swap::SwapCounters sc = game::brand::swap::Counters(st.k[0].handle);
        const game::brand::swap::SwapCounters bc = game::brand::swap::Counters(st.k[1].handle);
        const bool smallLive = game::radarmask::Live(game::radarmask::kSmall), bigLive = game::radarmask::Live(game::radarmask::kBig);
        const uint64_t subst = smallLive ? sc.subst : 0, binds = smallLive ? sc.seen : 0;
        const uint64_t delta = subst >= R.substAtLog ? subst - R.substAtLog : subst;
        const uint64_t bindsDelta = binds >= R.bindsAtLog ? binds - R.bindsAtLog : binds;
        R.substAtLog = subst;
        R.bindsAtLog = binds;
        const uint64_t bigBinds = bigLive ? bc.seen : 0;
        const uint64_t bigBindsDelta = bigBinds >= R.bigBindsAtLog ? bigBinds - R.bigBindsAtLog : bigBinds;
        R.bigBindsAtLog = bigBinds;
        const ULONGLONG t = GetTickCount64();
        char place[260];
        if (RingRoundOpen() && displayH > 0.0f)
        {
            const rr::BoxFit fit = {g_ringFitX.load(), g_ringFitY.load(), false, nullptr};
            const rr::BoxPx ask = rr::RoundAskPx(displayH, fit);
            std::snprintf(place, sizeof(place), "ROUND: hole r %.1f px (design %.1f), GTA's circle r %.1f px (design %.2f), asked box %.1f x %.1f px (fit %.3f, %.3f)",
                          (double)sync::hudring::MapRoundRadiusPx(displayH), (double)(sync::hudring::MapRoundRadiusPx(displayH) / sync::hudring::Scale(displayH)),
                          (double)(rr::kMaskCircleR * sync::hudring::Scale(displayH)), (double)rr::kMaskCircleR, (double)ask.w, (double)ask.h, (double)fit.fx,
                          (double)fit.fy);
        }
        else
            std::snprintf(place, sizeof(place), "0.5.4 size: hole r %.1f px (the square's inscribed circle)%s",
                          displayH > 0.0f ? (double)sync::hudring::MapRadiusPx(displayH) : 0.0,
                          R.out.round ? " (grown placement on its way: not landed yet)" : R.out.shrinking ? " (shrinking: radar held hidden until it lands)" : "");
        const std::string lastSubst = sc.lastTick ? std::to_string((unsigned long long)(t > sc.lastTick ? t - sc.lastTick : 0)) + " ms ago" : "never";
        std::string census = R.outcome;
        if (R.step == RoundStep::Census)
        {
            char b[200];
            std::snprintf(b, sizeof(b), "in progress, %s: radarmasksm %d of %d read back, radarmasklg %d of %d", game::radarmask::PhaseName(st.phase), st.k[0].read,
                          st.k[0].views, st.k[1].read, st.k[1].views);
            census = b;
        }
        GLOGI("engine: ring map round mask (0.5.6), last 10 s: %s; grows %u / shrinks %u%s; mask found %s (census %s, run %d: %s); radarmasksm %s %ux%u %s %u mip(s) "
              "stock hash %016llx: radar binds %llu (+%llu in 10 s), our circle %s, binds swapped %llu (+%llu in 10 s, last %s), read back %u ok / %u missed; "
              "radarmasklg %s (binds %llu (+%llu in 10 s), swapped %llu); %s; placement %s; hooks %s",
              rr::RoundWhyName(R.out.why), R.gate.Grows(), R.gate.Shrinks(), R.shrinkStuck ? " (a shrink never landed: our circle kept on)" : "", smallLive ? "YES" : "no",
              RoundStepName(R.step), R.runs + (R.step == RoundStep::Done || R.step == RoundStep::Census ? 1 : 0), census.c_str(),
              smallLive ? "REPLACED" : st.k[0].found ? "found, not replaced" : "not found", st.k[0].w, st.k[0].h, sync::ringround::TexFmtName(st.k[0].fmt), st.k[0].mips,
              (unsigned long long)st.k[0].stockHash, (unsigned long long)binds, (unsigned long long)bindsDelta, R.active ? "HANDED OUT" : "not handed out",
              (unsigned long long)subst, (unsigned long long)delta, lastSubst.c_str(), sc.verified, sc.verifyMiss,
              bigLive ? "REPLACED" : st.k[1].found ? "found, not replaced" : st.k[1].probed ? "not found" : "not looked for", (unsigned long long)bigBinds,
              (unsigned long long)bigBindsDelta, (unsigned long long)(bigLive ? bc.subst : 0), game::minimapsize::BlipClipStatus().c_str(), place,
              st.notD3D11 ? "NOT D3D11 (Enhanced: skipped)" : st.hooks ? "installed" : "not installed");
    }

    // 0.4.25 minimap_scale: configure the table writer from the registry value (minimap=0 means the radar is hidden, so nothing is touched), let it run, and
    // ask the radar's movie to flip once after every write so the "minimap" component is re-read. Script tick only.
    float g_miniCfgScale = -1.0f;                 // the factor last handed to minimapsize (-1 = never)
    void TickMinimapSize(double nowMs)
    {
        if (!g_miniScaleInit)
        {
            g_miniScaleInit = true;
            MinimapScaleRead();
        }
        const bool dirty = g_miniScaleDirty.exchange(false);
        if (g_hudRing.load() && g_ringMap.load())
        {
            // 0.5.2 ring_map: the visible map as a square in the ring, from the display the overlay drew last (no frame yet = wait) and the safe zone.
            const game::Overlay::UiState ui = game::Overlay::LastUiState();
            if (ui.valid && ui.displayW > 0.0f && ui.displayH > 0.0f)
            {
                const sync::hudring::Pt c = sync::hudring::MapCentrePx(ui.displayH);
                // 0.5.5 ring_map_round: the grown box while the round gate says so (its previous tick; libs/sync/ring_round.h RoundGate)
                const bool round = g_round.out.round;
                sync::MinimapPlacement p;
                if (round)
                {
                    const sync::ringround::BoxFit fit = {g_ringFitX.load(), g_ringFitY.load(), false, nullptr};
                    const sync::ringround::BoxPx ask = sync::ringround::RoundAskPx(ui.displayH, fit);
                    p = sync::RingPlacementBox(ui.displayW, ui.displayH, g_miniSafe, g_miniSafeOk, c.x, c.y, ask.w, ask.h);
                }
                else
                    p = sync::RingPlacement(ui.displayW, ui.displayH, g_miniSafe, g_miniSafeOk, c.x, c.y, sync::hudring::MapSquarePx(ui.displayH));
                if (round != g_round.cfgRound)
                {
                    g_round.cfgRound = round;
                    g_round.written = false;             // the table holds the other placement until the next write
                    g_round.cfgMs = nowMs;
                }
                game::minimapsize::SetBigMirror(g_ringMapBig.load());   // 0.5.4 ring_map_big: the big radar's slots get the same geometry
                game::minimapsize::ConfigurePlacement(p, round ? "ring_map_round" : "ring_map");   // the same placement is not written again
                g_miniCfgScale = -1.0f;
            }
        }
        else
        {
            const float want = g_minimap.load() ? g_miniScaleReq.load() : 1.0f;
            if (dirty || want != g_miniCfgScale)
            {
                g_miniCfgScale = want;
                game::minimapsize::Configure(want, g_minimap.load() ? sync::ScaleSourceName((sync::ScaleSource)g_miniScaleSrc.load()) : "minimap=0: no radar, nothing to resize");
            }
        }
        game::minimapsize::Tick(nowMs);
        if (game::minimapsize::TakeWritten())
        {
            g_barsGate.RequestFlip();
            g_round.written = true;                      // 0.5.5: the configured placement (round or 0.5.4) is in the table now
        }
        g_miniAppliedScale = game::minimapsize::AppliedScale();
    }

    // 0.5.2 ring_map: the overlay opens the ring's map hole only while the table holds the placement, the radar is shown, the big radar is off (a
    // flip re-lays the radar out: it is the big map for its duration) and all of that has held for sync::kHoleSettleMs. Round 1 of the round map also
    // saves the bottom-left of the frame twice (Overlay::RequestRadarProbe, D3D11 only) so the radar's real landing place can be measured.
    // 0.5.8: the settle is sync::RingHoleTimer (libs/sync/minimap.h, kHoleSettleMs 800 ms); a hole closed only by the radar's hide (the
    // inventory, the pause menu) reopens kHoleReshowMs after the re-show (GI: "pressing tab and closing lags out the map for 0.5s").
    constexpr double kRingProbeAtMs[2] = {2500.0, 20000.0};
    sync::RingHoleTimer g_ringHole;
    double g_ringMapSinceMs = -1.0;     // when all the conditions first held (-1 = not now; backdated on a quick re-show)
    int g_ringProbes = 0;
    bool g_ringOpenLogged = false;
    uint32_t g_ringReshowsLogged = 0;
    void TickRingMap(double nowMs, bool wanted)
    {
        const bool layoutOk = g_hudRing.load() && g_ringMap.load() && game::minimapsize::PlacementApplied() && !g_barsGate.BigmapOn();
        const uint32_t reshowsBefore = g_ringHole.Reshows();
        const bool open = g_ringHole.Update(layoutOk, wanted, nowMs);
        g_ringMapSinceMs = g_ringHole.Since();
        if (!layoutOk || !wanted)
        {
            game::Overlay::SetRingMap(false);
            game::Overlay::SetRingMapRound(false);
            return;
        }
        if (g_ringHole.Reshows() != reshowsBefore && g_ringReshowsLogged < 4)
        {
            ++g_ringReshowsLogged;
            GLOGI("engine: ring map (0.5.8): the radar shows again with nothing re-laid out since the hole was open: it reopens in %.0f ms, not %.0f "
                  "(%u quick re-shows so far; only the first 4 are logged)",
                  sync::kHoleReshowMs, sync::kHoleSettleMs, g_ringHole.Reshows());
        }
        // 0.5.5 ring_map_round: the hole grows to the arcs only while the grown placement has landed and our circle is handed out
        const bool round = RingRoundOpen();
        game::Overlay::SetRingMapRound(round);
        game::Overlay::SetRingMap(open);
        if (open && !g_ringOpenLogged)
        {
            g_ringOpenLogged = true;
            GLOGI("engine: ring map (0.5.2): the ring's map hole is open (the table holds the placement, the radar shows, the big radar is off)");
        }
        if (g_ringProbes < 2 && nowMs - g_ringMapSinceMs >= kRingProbeAtMs[g_ringProbes])
            game::Overlay::RequestRadarProbe(++g_ringProbes);
        // 0.5.5: the same two captures once the ROUND map has been open for as long (radar_probe_3 / _4.bmp), so its circle can be measured
        if (round && open && !g_round.probesRearmed && g_ringProbes >= 2)
        {
            g_round.probesRearmed = true;
            g_round.roundOpenMs = nowMs;
            GLOGI("engine: ring map round mask (0.5.6): the ROUND map hole is open; radar probes 3 and 4 follow at 2.5 s and 20 s");
        }
        if (round && open && g_round.probesRearmed && g_round.roundProbes < 2 && nowMs - g_round.roundOpenMs >= kRingProbeAtMs[g_round.roundProbes])
            game::Overlay::RequestRadarProbe(3 + g_round.roundProbes++);
    }

    void TickMinimap(double nowMs)
    {
        MinimapStats& s = g_mini;
        if (g_barsReset.exchange(false))
        {
            g_barsGate.SetMode((sync::BarsMode)g_barsMode.load());
            {
                // 0.4.27: the big-radar flip is held this long so the radar re-lays out the resized table (minimap_flip_ms, 0 = one tick as before).
                const std::wstring v = reg::Get(L"minimap_flip_ms");
                const double ms = v.empty() ? 500.0 : _wtof(v.c_str());
                g_barsGate.SetFlipHoldMs(ms);
                GLOGI("engine: minimap size (0.4.27): big-radar flip held %.0f ms (minimap_flip_ms%s)", ms, v.empty() ? " unset, default" : "");
            }
            g_barsGate.Reset();
        }
        TickMinimapSize(nowMs);
        sync::RadarInputs in;
        in.switchOn = g_minimap.load();
        in.entryDone = g_entry.load() == Entry::Done;
        in.pauseMenuActive = g_pauseMenuActive;
        // 0.6: the REV3 inventory page covers the screen; GTA's radar would show through its dim, so it is hidden while the page is open.
        const bool invOpen = game::Overlay::InventoryOpen();
        const bool wantedBase = sync::RadarWanted(in) && !invOpen;
        // 0.5.5 ring_map_round: a shrink that has not landed yet holds the radar hidden (TickRingRound; 0.5.6: the census never hides it)
        const bool roundHold = TickRingRound(nowMs, wantedBase);
        const bool wanted = wantedBase && !roundHold;
        if (wanted != g_miniWanted)
        {
            g_miniWanted = wanted;
            g_miniChangeMs = nowMs;
            if (g_miniTransitions < 24)
            {
                ++g_miniTransitions;
                GLOGI("engine: minimap (0.4.22): radar %s (minimap=%s, entry %s, pause menu %s, inventory %s%s)", wanted ? "SHOWN" : "hidden", in.switchOn ? "on" : "OFF",
                      in.entryDone ? "done" : "not done", in.pauseMenuActive ? "up" : "closed", invOpen ? "open" : "closed",
                      roundHold && wantedBase ? ", held hidden by the 0.5.5 round mask probe / shrink" : "");
            }
        }
        if (!in.entryDone)
            g_miniLogMs = 0;                     // the 10 s line is for play, not for loading
        // The overlay docks the vitals plate under the radar while the minimap switch is on (it cannot call a native, so the safe zone comes from here).
        if (nowMs - g_miniSafeMs >= kMiniSafeZoneMs)
        {
            g_miniSafeMs = nowMs;
            g_miniSafeOk = g_world->SafeZoneSize(g_miniSafe);
        }
        game::Overlay::SetRadarDock(in.switchOn, g_miniSafe, g_miniSafeOk, g_miniAppliedScale);
        TickRingMap(nowMs, wanted);
        ++s.ticks;
        if (wanted)
            ++s.wantedTicks;
        if (g_world->DisplayRadar(wanted))
            ++(wanted ? s.radarOn : s.radarOff);
        else
            ++s.radarUncallable;
        // 0.5.4 ring_map_zoom: the ring's map further out (GI: "make it more zoomed out"), held every tick while the radar shows in the ring.
        const int zoom = g_ringMapZoom.load();
        if (wanted && zoom > 0 && g_hudRing.load() && g_ringMap.load())
        {
            const bool ok = g_world->SetRadarZoom(zoom);
            ++(ok ? s.zoomCalls : s.zoomUncallable);
            if (!g_ringZoomLogged)
            {
                g_ringZoomLogged = true;
                GLOGI("engine: ring map zoom (0.5.4): SET_RADAR_ZOOM(%d) %s, held every tick while the ring map shows", zoom,
                      ok ? "called" : "FAILED (native not callable)");
            }
            g_ringZoomHeld = ok;
        }
        else if (g_ringZoomHeld)
        {
            // The ring map stopped showing (radar hidden, ring_map / hud_ring off): hand the zoom back to the game once, in case the level persists.
            g_ringZoomHeld = false;
            g_world->SetRadarZoom(0);
        }

        // GTA's health / armour bars: the gate decides, this performs. The movie's loaded flag is read only when the gate could use it.
        sync::BarsInputs bi;
        bi.radarOn = wanted;
        bi.haveHandle = g_miniMovie != 0;
        if (wanted && g_barsGate.Mode() != sync::BarsMode::Off && bi.haveHandle)
            g_miniMovieLoaded = g_world->ScaleformLoaded(g_miniMovie);
        bi.loaded = bi.haveHandle && g_miniMovieLoaded;
        const sync::BarsAction barsAct = g_barsGate.Next(bi, nowMs);
        switch (barsAct)
        {
        case sync::BarsAction::None:
            break;
        case sync::BarsAction::Request:
            g_miniMovie = g_world->RequestScaleform("minimap");
            g_miniMovieLoaded = false;
            if (!g_miniMovie)
                ++s.requestFailed;
            break;
        case sync::BarsAction::RefreshOn:
            if (!g_world->SetBigmap(true))
                ++s.barsUncallable;
            break;
        case sync::BarsAction::RefreshOff:
            if (!g_world->SetBigmap(false))
                ++s.barsUncallable;
            break;
        case sync::BarsAction::Setup:
        {
            bool began = false;
            if (!g_world->ScaleformCallInt(g_miniMovie, "SETUP_HEALTH_ARMOUR", sync::kBarsHideType, began))
                ++s.barsUncallable;
            else if (!began)
                ++s.setupBeginRefused;
            else
                ++s.setupOk;
            break;
        }
        }
        // 0.5.8 radar_bigmap_off: the expanded radar held off while the ring map shows; never during our own flip (BigmapOn, which also covers a
        // RefreshOn issued this tick), and not on the tick the flip's own OFF was just issued
        if (g_radarBigmapOff.load() && wanted && g_hudRing.load() && g_ringMap.load() && !g_barsGate.BigmapOn() && barsAct != sync::BarsAction::RefreshOff)
            ++(g_world->SetBigmap(false) ? s.bigmapOffCalls : s.bigmapOffUncallable);

        // Passive read-backs (every mode, so minimap=0 shows the radar really is hidden).
        if (nowMs - g_miniSampleMs >= kMiniSampleMs)
        {
            g_miniSampleMs = nowMs;
            game::World::RadarReadback rb;
            g_world->ReadRadar(rb);
            ++s.samples;
            if (!rb.hiddenOk || !rb.renderingOk || !rb.prefOk || !rb.hudOk || !rb.waypointOk)
                ++s.readUnresolved;
            if (rb.hudOk && rb.hudHidden)
                ++s.hudHidden;
            if (rb.prefOk && !rb.pref)
                ++s.prefOff;
            if (rb.waypointOk && rb.waypoint)
                ++s.waypointSamples;
            if (nowMs - g_miniChangeMs >= kMiniSettleMs)
            {
                if (wanted)
                {
                    ++s.wSettled;
                    if (rb.hiddenOk && rb.hidden)
                        ++s.wHiddenTrue;
                    if (rb.renderingOk && !rb.rendering)
                        ++s.wRenderFalse;
                }
                else
                {
                    ++s.uSettled;
                    if (rb.hiddenOk && !rb.hidden)
                        ++s.uHiddenFalse;
                }
            }
        }

        if (in.entryDone)
        {
            if (g_miniLogMs == 0)
                g_miniLogMs = nowMs;
            else if (nowMs - g_miniLogMs >= kMiniLogMs)
            {
                g_miniLogMs = nowMs;
                LogMinimap(nowMs, wanted);
                if (g_hudRing.load() && g_ringMap.load())
                {
                    const game::Overlay::UiState ui = game::Overlay::LastUiState();
                    LogRingRound(ui.valid ? ui.displayH : 0.0f);
                }
            }
        }
    }

    // A Pose heading ((-180, 180]) as the game's [0, 360).
    inline float Heading360(float h)
    {
        float r = std::fmod(h, 360.0f);
        if (r < 0.0f)
            r += 360.0f;
        return r >= 360.0f ? 0.0f : r;                 // -1e-6 + 360 rounds to 360
    }

    // ---- deferred peer log lines (script tick) -------------------------------------
    // The peer tick's log lines are collected here and written once its loops are done (script tick only; the
    // join worker never logs through this), so no file I/O sits in the middle of the peer work.
    struct PeerLogLine { bool warn; std::string text; };
    std::vector<PeerLogLine> g_peerLog;
    void PeerLog(bool warn, const char* fmt, ...)
    {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        g_peerLog.push_back({ warn, buf });
    }
    void FlushPeerLog()
    {
        for (const PeerLogLine& l : g_peerLog)
        {
            if (l.warn)
                GLOGW("%s", l.text.c_str());
            else
                GLOGI("%s", l.text.c_str());
        }
        g_peerLog.clear();
    }

    // Hands back a model this peer asked GTA to stream and no longer will use (script tick).
    void ReleaseRequest(Peer& pe)
    {
        if (pe.requestedModel)
            g_world->ModelNoLongerNeeded(pe.requestedModel);
        pe.requestedModel = 0;
    }

    // ---- appearance (protocol v3 Look) <-> game::PedLook -------------------------
    const uint32_t kFreemodeFemale = 0x9C9EFFD8;  // mp_f_freemode_01
    // The wire has no "has a head blend" bit: a freemode model always has one, anything else none.
    bool IsFreemodeModel(uint32_t model) { return model == kFreemodeMale || model == kFreemodeFemale; }

    uint8_t Mix255(float f)
    {
        if (!(f > 0.0f))
            return 0;                                  // also NaN
        return f >= 1.0f ? 255 : (uint8_t)std::lround(f * 255.0f);
    }

    // What we read off our ped, clamped into ValidLook's ranges (an out-of-range prop becomes "none").
    proto::Look ToProtoLook(const game::PedLook& in)
    {
        proto::Look out;
        out.model = in.model;
        for (size_t i = 0; i < proto::kLookComponents; ++i)
        {
            const int d = in.drawable[i];
            out.components[i].drawable = (uint16_t)(d < 0 ? 0 : d > proto::kMaxLookDrawable ? proto::kMaxLookDrawable : d);
            out.components[i].texture = in.texture[i] > proto::kMaxLookTexture ? proto::kMaxLookTexture : in.texture[i];
            out.components[i].palette = in.palette[i] > proto::kMaxLookPalette ? proto::kMaxLookPalette : in.palette[i];
        }
        for (size_t i = 0; i < proto::kLookProps; ++i)
        {
            const int d = in.propDrawable[i];
            if (d < 0 || d > proto::kMaxLookDrawable)
                continue;                              // stays drawable -1, texture 0
            out.props[i].drawable = (int16_t)d;
            out.props[i].texture = in.propTexture[i] > proto::kMaxLookTexture ? proto::kMaxLookTexture : in.propTexture[i];
        }
        if (in.hasHead)
        {
            auto id = [](int v) { return (uint8_t)(v < 0 ? 0 : v > proto::kMaxHeadBlendId ? proto::kMaxHeadBlendId : v); };
            out.head.shape1 = id(in.head.shapeFirst); out.head.shape2 = id(in.head.shapeSecond);
            out.head.shape3 = id(in.head.shapeThird);
            out.head.skin1 = id(in.head.skinFirst); out.head.skin2 = id(in.head.skinSecond);
            out.head.skin3 = id(in.head.skinThird);
            out.head.shapeMix = Mix255(in.head.shapeMix); out.head.skinMix = Mix255(in.head.skinMix);
            out.head.thirdMix = Mix255(in.head.thirdMix);
        }
        return out;
    }

    game::PedLook FromProtoLook(const proto::Look& in)
    {
        game::PedLook out{};
        out.model = in.model;
        for (size_t i = 0; i < proto::kLookComponents; ++i)
        {
            out.drawable[i] = (int16_t)in.components[i].drawable;
            out.texture[i] = in.components[i].texture;
            out.palette[i] = in.components[i].palette;
        }
        for (size_t i = 0; i < proto::kLookProps; ++i)
        {
            out.propDrawable[i] = in.props[i].drawable;
            out.propTexture[i] = in.props[i].texture;
        }
        out.hasHead = IsFreemodeModel(in.model);
        out.head = { in.head.shape1, in.head.shape2, in.head.shape3, in.head.skin1, in.head.skin2, in.head.skin3,
                     in.head.shapeMix / 255.0f, in.head.skinMix / 255.0f, in.head.thirdMix / 255.0f };
        return out;
    }

    // Our look to the server (script tick, after the entry): read every 2 s, sent when it differs
    // from what this connection last sent (the session skips an identical one).
    void PublishLook(int32_t lp, ULONG now)
    {
        static ULONG lastAt = 0;
        static proto::Look lastLogged;
        static bool invalidLogged = false;
        if (now - lastAt < 2000 || !join::IsConnected())
            return;
        lastAt = now;
        game::PedLook pl{};
        if (!g_world->ReadPedLook(lp, pl))
            return;
        const proto::Look look = ToProtoLook(pl);
        if (!proto::ValidLook(look))
        {
            if (!invalidLogged)
                GLOGW("engine: our look (model 0x%08x) fails ValidLook; not sent", (unsigned)look.model);
            invalidLogged = true;
            return;
        }
        if (join::SendLook(look) && look != lastLogged)
        {
            lastLogged = look;
            GLOGI("engine: look published (model 0x%08x, top %u/%u, legs %u/%u, head %s)", (unsigned)look.model,
                  (unsigned)look.components[11].drawable, (unsigned)look.components[11].texture,
                  (unsigned)look.components[4].drawable, (unsigned)look.components[4].texture,
                  pl.hasHead ? "blend" : "none");
        }
    }

    // ===== 0.5.4 outfit_items: the worn items decide the ped's shirt, pants and shoes (script tick) =====
    // The server's inventory (OutfitNoteInventory) -> sync::outfit::Desired -> compared with what the ped REALLY wears (ReadPedLook) -> each
    // component that differs is set (validated first). Checked when the inventory changes, when the ped handle changes, right after DressPlayer
    // (entry, ped guard) and once a second (a respawn or anything else that re-dresses the ped). A component that does not take after 3 tries
    // for the same inventory is reported and left alone until the inventory or the ped changes. PublishLook (every 2 s) sends the result to the
    // other players: the look sync itself is unchanged (PlayerLook / PeerLook, protocol v3).
    void TickOutfitItems(int32_t lp, ULONG now)
    {
        if (!g_outfitItems.load() || !lp)
            return;
        static bool starterChecked = false;
        if (!starterChecked) {
            // The starter items ARE the spawn outfit's pieces: say so once, loudly if someone changed one side only.
            starterChecked = true;
            int checked = 0, differ = 0;
            for (const char* key : {"tshirt", "jeans", "sneakers"}) {
                const sync::inv::ItemDef* d = sync::inv::FindKey(key);
                const sync::outfit::Parts p = sync::outfit::Worn(d ? d->id : 0, sync::outfit::Sex::Male);
                for (int i = 0; i < p.n; ++i)
                    for (const game::PedComponent& c : kPlayerOutfit)
                        if (c.component == p.p[i].component) {
                            ++checked;
                            if (c.drawable != p.p[i].drawable || c.texture != p.p[i].texture) {
                                ++differ;
                                GLOGW("engine: outfit items (0.5.4): item %s sets component %d to %d/%d but the spawn outfit has %d/%d (%s): the two "
                                      "must agree (libs/sync/outfit.h Worn, engine.cpp kPlayerOutfit)",
                                      key, (int)p.p[i].component, (int)p.p[i].drawable, (int)p.p[i].texture, c.drawable, c.texture, c.what);
                            }
                        }
            }
            GLOGI("engine: outfit items (0.5.4): the starter items against the spawn outfit: %d components compared, %d differ", checked, differ);
        }
        sync::inv::Character ch;
        bool have;
        uint32_t seq;
        {
            std::lock_guard<std::mutex> lk(g_outfitMtx);
            have = g_outfitFeed.have;
            seq = g_outfitFeed.seq;
            if (have)
                ch = g_outfitFeed.ch;
        }
        if (!have)
            return;                                // no server inventory: the spawn outfit stands (0.5.3)
        static uint32_t seenSeq = ~0u;
        static int32_t seenPed = 0;
        static ULONG lastCheck = 0;
        static int tries = 0;
        static int lines = 0;
        const bool changed = seq != seenSeq || lp != seenPed;
        const bool redressed = g_outfitRecheck.exchange(false);
        if (!changed && !redressed && now - lastCheck < 1000)
            return;
        if (changed || redressed)
            tries = 0;
        lastCheck = now;
        seenSeq = seq;
        seenPed = lp;
        sync::outfit::Sex sex;
        const uint32_t model = g_world->GetEntityModel(lp);
        if (!sync::outfit::SexOfModel(model, sex)) {
            static uint32_t loggedModel = 0;
            if (loggedModel != model) {
                loggedModel = model;
                GLOGI("engine: outfit items (0.5.4): the ped is model 0x%08x, not a freemode ped: the items change nothing on it", (unsigned)model);
            }
            return;
        }
        const sync::outfit::Want want = sync::outfit::Desired(ch, sex);
        game::PedLook pl{};
        if (!g_world->ReadPedLook(lp, pl))
            return;
        const uint32_t mismatch = sync::outfit::Mismatch(want, pl.drawable, pl.texture);
        static bool awaitingReadBack = false;      // 0.5.6: the components set by the last pass, not yet seen on the ped
        if (!mismatch) {
            if (awaitingReadBack && lines < 200) {
                ++lines;
                GLOGI("engine: outfit items (0.5.6): read back: the ped wears what the items ask for (shirt=%s pants=%s shoes=%s, %d components "
                      "checked)", sync::outfit::WornKey(ch, sync::inv::Equip::Shirt), sync::outfit::WornKey(ch, sync::inv::Equip::Pants),
                      sync::outfit::WornKey(ch, sync::inv::Equip::Shoes), want.Count());
            }
            awaitingReadBack = false;
            return;
        }
        awaitingReadBack = true;
        if (tries >= 3) {
            if (tries++ == 3)
                GLOGW("engine: outfit items (0.5.4): components 0x%03x still differ after 3 tries; left alone until the inventory or the ped changes",
                      (unsigned)mismatch);
            return;
        }
        ++tries;
        std::string what;
        int set = 0, invalid = 0, failed = 0;
        for (int c = 0; c < sync::outfit::kComponents; ++c) {
            if (!(mismatch & (1u << c)))
                continue;
            const int r = g_world->SetPedComponentChecked(lp, c, want.drawable[c], want.texture[c]);
            set += r == 1;
            invalid += r == 0;
            failed += r < 0;
            char b[64];
            std::snprintf(b, sizeof b, "%s%d: %d/%d -> %d/%d%s", what.empty() ? "" : ", ", c, (int)pl.drawable[c], (int)pl.texture[c],
                          (int)want.drawable[c], (int)want.texture[c], r == 1 ? "" : (r == 0 ? " NOT VALID" : " CANNOT CALL"));
            what += b;
        }
        if (lines < 200) {
            ++lines;
            const auto worn = [&](sync::inv::Equip e) {
                const sync::inv::ItemDef* d = sync::inv::Find(ch.equip[sync::inv::EquipIndex(e)].item);
                return d ? d->key : "bare";
            };
            GLOGI("engine: outfit items (0.5.4): %s (try %d): shirt=%s pants=%s shoes=%s; set %d, not valid %d, cannot call %d: %s",
                  changed ? "inventory or ped changed" : (redressed ? "re-dressed" : "the ped drifted"), tries, worn(sync::inv::Equip::Shirt),
                  worn(sync::inv::Equip::Pants), worn(sync::inv::Equip::Shoes), set, invalid, failed, what.c_str());
        }
    }

    // ===== 0.5.4 inv_ped_view: the player's own ped in the CHARACTER panel (script tick) =====
    // While the inventory page is open (and drawn), the ped alive and on foot: a scripted camera in front of the ped (libs/sync/ped_view.h
    // Frame), the ped's heading frozen when it opened, its position followed every tick (a ragdoll or a push keeps it in the window). Anything
    // else restores the gameplay camera at once: RENDER_SCRIPT_CAMS(false) + DESTROY_CAM (the gameplay camera itself was never touched, so it
    // comes back exactly as it was, first person included). The overlay draws the window only while IS_CAM_RENDERING reads back yes.
    struct PedViewCam
    {
        int32_t cam = 0;
        float heading = 0;
        sync::pedview::CamOut last;
        float lastW = 0, lastH = 0;
        int rendering = -2;                        // the last IS_CAM_RENDERING read-back (-2 = none yet)
        sync::pedview::Gate gate = sync::pedview::Gate::Closed;
        uint32_t opened = 0, createFails = 0;
    };
    PedViewCam g_pv;

    void PedViewRestore(const char* why)
    {
        if (g_pv.cam) {
            g_world->SetCamActive(g_pv.cam, false);
            const bool back = g_world->RenderScriptCams(false);
            g_world->DestroyCam(g_pv.cam);
            const bool gone = !g_world->DoesCamExist(g_pv.cam);
            GLOGI("engine: inventory ped view (0.5.4): camera off (%s): gameplay camera %s, cam %s", why, back ? "back" : "NOT RESTORED (cannot call)",
                  gone ? "destroyed" : "STILL EXISTS");
        }
        g_pv.cam = 0;
        g_pv.rendering = -2;
        game::Overlay::SetInventoryPedView(false);
    }

    void TickInvPedView(int32_t lp)
    {
        namespace pv = sync::pedview;
        float W = 0, H = 0;
        double ago = 1e9;
        const bool drawn = game::Overlay::InventoryPedViewFrame(&W, &H, &ago);
        pv::GateIn gi;
        gi.switchOn = g_invPedView.load();
        gi.pageOpen = game::Overlay::InventoryOpen();
        gi.drawnAgoMs = drawn ? ago : 1e9;
        gi.ped = lp != 0;
        if (gi.switchOn && gi.pageOpen && lp) {    // the natives only when it could matter
            gi.dead = g_life != Life::Alive || g_world->IsPedDeadOrDying(lp);
            gi.vehicle = g_world->IsPedInVehicle(lp) || g_world->IsPedGettingIntoVehicle(lp);
        }
        const pv::Gate gate = pv::Decide(gi);
        if (gate != g_pv.gate) {
            if (gate == pv::Gate::Live || g_pv.gate == pv::Gate::Live || gi.pageOpen)
                GLOGI("engine: inventory ped view (0.5.4): %s -> %s", pv::GateName(g_pv.gate), pv::GateName(gate));
            g_pv.gate = gate;
        }
        if (gate != pv::Gate::Live) {
            if (g_pv.cam)
                PedViewRestore(pv::GateName(gate));
            return;
        }
        float x = 0, y = 0, z = 0;
        if (!g_world->GetEntityCoords(lp, x, y, z)) {
            if (g_pv.cam)
                PedViewRestore("ped position unreadable");
            return;
        }
        if (!g_pv.cam)
            g_pv.heading = g_world->GetEntityHeading(lp);   // frozen while the page is open: the camera does not swing with the ped
        pv::CamIn ci;
        ci.W = W;
        ci.H = H;
        ci.px = x;
        ci.py = y;
        ci.pz = z;
        ci.headingDeg = g_pv.heading;
        const pv::CamOut o = pv::Frame(ci);
        if (!o.ok) {
            if (g_pv.cam)
                PedViewRestore("no framing for this display");
            return;
        }
        if (!g_pv.cam) {
            if (g_pv.createFails >= 3)
                return;                            // reported below; the mannequin stays
            g_pv.cam = g_world->CreateScriptCam(o.x, o.y, o.z, o.pitchDeg, o.yawDeg, o.fovDeg);
            if (!g_pv.cam) {
                if (++g_pv.createFails == 3)
                    GLOGW("engine: inventory ped view (0.5.4): CREATE_CAM_WITH_PARAMS failed 3 times; the mannequin stays this session");
                return;
            }
            const bool active = g_world->SetCamActive(g_pv.cam, true);
            const bool render = g_world->RenderScriptCams(true);
            ++g_pv.opened;
            g_pv.last = o;
            g_pv.lastW = W;
            g_pv.lastH = H;
            float fx = 0, fy = 0, hx = 0, hy = 0;
            const bool feet = pv::Project(o, W, H, x, y, z + pv::kFeetZ, fx, fy);
            const bool head = pv::Project(o, W, H, x, y, z + pv::kHeadZ, hx, hy);
            GLOGI("engine: inventory ped view (0.5.4): camera on #%u (cam %d, active %s, render %s): %.0fx%.0f, ped %.1f %.1f %.1f heading %.0f, "
                  "camera %.2f m away, pitch %.1f yaw %.1f fov %.0f, ped middle at ndc %.2f %.2f, feet %s %.2f, head %s %.2f; "
                  "follow-cam view mode %d (4 = first person)",
                  g_pv.opened, g_pv.cam, active ? "yes" : "NO", render ? "yes" : "NO", W, H, x, y, z, g_pv.heading, o.dist, o.pitchDeg, o.yawDeg,
                  o.fovDeg, o.ndcX, o.ndcY, feet ? "ndc" : "BEHIND", fy, head ? "ndc" : "BEHIND", hy, g_world->FollowPedCamViewMode());
        } else {
            const bool moved = std::fabs(o.x - g_pv.last.x) + std::fabs(o.y - g_pv.last.y) + std::fabs(o.z - g_pv.last.z) > 0.01f ||
                               W != g_pv.lastW || H != g_pv.lastH;
            if (moved) {
                g_world->SetScriptCamParams(g_pv.cam, o.x, o.y, o.z, o.pitchDeg, o.yawDeg, o.fovDeg);
                g_pv.last = o;
                g_pv.lastW = W;
                g_pv.lastH = H;
            }
        }
        // the read-back: the window opens only while our camera is what the game renders
        const int r = g_world->IsCamRendering(g_pv.cam);
        if (r != g_pv.rendering) {
            GLOGI("engine: inventory ped view (0.5.4): IS_CAM_RENDERING %s", r == 1 ? "yes (the panel shows the ped)" : (r == 0 ? "NO (the mannequin stays)" : "cannot call (the mannequin stays)"));
            g_pv.rendering = r;
        }
        game::Overlay::SetInventoryPedView(r == 1);
    }

    // ===== 0.5.8 inv_ped_mode: the CHARACTER panel's ped on GTA's frontend ped renderer (script tick) =====
    // GI 2026-11-07 after 0.5.7: "the character in inventory should be only char no BG" and "that preview doesn't work in cars". The rules,
    // the sources and the slot search are in libs/sync/ped_preview.h; this is the glue: every native call, its read-back and the log.
    struct PedPreviewState
    {
        sync::pedpreview::Machine m;
        sync::pedpreview::LookWatch look;
        sync::pedpreview::Gate gate = sync::pedpreview::Gate::Closed;
        int forcedSeen = -2;                       // the inv_ped_slot the machine was given
        bool colourChanged = false;                // HUD colour 117 is ours (transparent) until restored
        bool colourRead = false;                   // ... and `colour` was read from the game (else the documented default)
        int colour[4] = {0, 0, 0, 0};
        bool behind = false;                       // SET_SCRIPT_GFX_DRAW_BEHIND_PAUSEMENU(true) is set on our thread
        uint32_t probeSeq = 0;
        bool probeWaiting = false;
        double lookAtMs = -1e9, openAtMs = 0, activeAtMs = -1, teardownAtMs = 0;
        int givesThisOpen = 0;
        uint32_t opens = 0, clones = 0, gives = 0;
        // the 10 s line, from read-backs (a name says what read it)
        double logAtMs = 0;
        uint32_t ticks = 0, menuActiveTicks = 0, menuOursTicks = 0, menuOtherTicks = 0, versionUnread = 0, cloneExistsTicks = 0, vehicleTicks = 0;
        uint32_t windowTicks = 0, backdropDraws = 0, behindFailed = 0, cursorFailed = 0, reclones = 0;
    };
    PedPreviewState g_pp;

    // The ped's look as one signature (components, props; the head blend does not change in a session).
    bool PedPreviewLookSig(int32_t ped, uint64_t& sig)
    {
        game::PedLook look{};
        if (!g_world->ReadPedLook(ped, look))
            return false;
        int v[1 + 12 * 3 + 8 * 2];
        int n = 0;
        v[n++] = (int)look.model;
        for (int i = 0; i < 12; ++i) { v[n++] = look.drawable[i]; v[n++] = look.texture[i]; v[n++] = look.palette[i]; }
        for (int i = 0; i < 8; ++i) { v[n++] = look.propDrawable[i]; v[n++] = look.propTexture[i]; }
        sig = sync::pedpreview::SigOf(v, n);
        return true;
    }

    void PedPreviewDeleteClone(int32_t& handle, const char* why)
    {
        if (!handle)
            return;
        int32_t c = handle;
        const int32_t was = c;
        bool del = g_world->DeleteEntity(c);
        int exists = g_world->EntityExists(was);
        if (exists == 1) {
            // DELETE_ENTITY needs a mission entity of this script: claim it and try once more
            uint64_t args[] = { (uint64_t)(uint32_t)was, 1, 1 };
            g_world->CallNative(game::native::SET_ENTITY_AS_MISSION_ENTITY, args, 3, nullptr);
            c = was;
            del = g_world->DeleteEntity(c) || del;
            exists = g_world->EntityExists(was);
        }
        GLOGI("engine: inventory ped preview (0.5.8): clone %d deleted (%s): DELETE_ENTITY %s, DOES_ENTITY_EXIST after: %s", was, why,
              del ? "called" : "CANNOT CALL", exists == 0 ? "no" : (exists == 1 ? "YES (left in the world, hidden)" : "unreadable"));
        handle = 0;
    }

    void PedPreviewTeardown(const char* why, double now)
    {
        const bool cleared = g_world->ClearPedInPauseMenu();
        const bool off = g_world->SetFrontendActive(false);
        bool behindOff = true;
        if (g_pp.behind) {
            behindOff = g_world->SetScriptGfxDrawBehindPauseMenu(false);
            g_pp.behind = false;
        }
        const char* colour = "untouched";
        if (g_pp.colourChanged) {
            const bool ok = g_world->ReplaceHudColour(sync::pedpreview::kPauseBgHudColour, g_pp.colour[0], g_pp.colour[1], g_pp.colour[2], g_pp.colour[3]);
            colour = ok ? (g_pp.colourRead ? "restored to the value read at the open" : "set to the documented default (31, 31, 31, 100)") : "NOT RESTORED (cannot call)";
            g_pp.colourChanged = !ok;
        }
        g_pp.teardownAtMs = now;
        g_pp.probeWaiting = false;
        GLOGI("engine: inventory ped preview (0.5.8): closed (%s) after %.1f s: CLEAR_PED_IN_PAUSE_MENU %s, SET_FRONTEND_ACTIVE(false) %s, "
              "draw-behind flag %s, HUD colour 117 %s", why, (now - g_pp.openAtMs) / 1000.0, cleared ? "called" : "CANNOT CALL",
              off ? "called" : "CANNOT CALL", behindOff ? "reset" : "NOT RESET (cannot call)", colour);
        PedPreviewDeleteClone(g_ppCloneOld, why);
        PedPreviewDeleteClone(g_ppClone, why);
        g_pp.look.Reset();
    }

    void PedPreviewOpen(double now)
    {
        namespace pp = sync::pedpreview;
        ++g_pp.opens;
        g_pp.openAtMs = now;
        if (now - g_pp.logAtMs >= 10000.0)
            g_pp.logAtMs = now;                    // the first 10 s line comes 10 s after the open, not on its first tick
        g_pp.activeAtMs = -1;
        g_pp.givesThisOpen = 0;
        const char* colour = "already ours";
        if (!g_pp.colourChanged) {
            int r = 0, g = 0, b = 0, a = 0;
            g_pp.colourRead = g_world->GetHudColour(pp::kPauseBgHudColour, r, g, b, a);
            if (g_pp.colourRead) {
                g_pp.colour[0] = r; g_pp.colour[1] = g; g_pp.colour[2] = b; g_pp.colour[3] = a;
            } else
                for (int i = 0; i < 4; ++i) g_pp.colour[i] = pp::kDefaultPauseBg[i];
            g_pp.colourChanged = g_world->ReplaceHudColour(pp::kPauseBgHudColour, 0, 0, 0, 0);
            colour = g_pp.colourChanged ? "-> transparent" : "NOT CHANGED (cannot call)";
        }
        const bool act = g_world->ActivateFrontendMenu(pp::kFeMenuEmptyNoBackground, false, -1);
        GLOGI("engine: inventory ped preview (0.5.8): open #%u: ACTIVATE_FRONTEND_MENU(0x%08x FE_MENU_VERSION_EMPTY_NO_BACKGROUND, togglePause=false, -1) "
              "%s; HUD colour 117 was %d,%d,%d,%d (%s) %s; slot %d (%s)", g_pp.opens, (unsigned)pp::kFeMenuEmptyNoBackground,
              act ? "called" : "CANNOT CALL", g_pp.colour[0], g_pp.colour[1], g_pp.colour[2], g_pp.colour[3],
              g_pp.colourRead ? "read" : "unreadable, the default assumed", colour, g_pp.m.Slot(),
              g_invPedSlot.load() >= 0 ? "forced" : (g_pp.m.SearchDone() ? "found by this session's search" : "searching"));
    }

    void PedPreviewClone(int32_t lp, double now, bool again)
    {
        // A new clone for a changed look: the old one stays until the new one is given (the frontend may still draw from it); otherwise any
        // leftover goes now.
        PedPreviewDeleteClone(g_ppCloneOld, "an older clone left over");
        if (again) {
            g_ppCloneOld = g_ppClone;
            g_ppClone = 0;
        } else
            PedPreviewDeleteClone(g_ppClone, "a leftover at a new open");
        float x = 0, y = 0, z = 0;
        const bool at = g_world->GetEntityCoords(lp, x, y, z);
        const bool inVeh = g_world->IsPedInVehicle(lp);
        const int32_t c = g_world->ClonePed(lp);
        g_ppClone = c;
        ++g_pp.clones;
        int failed = 6;
        if (c)
            failed = g_world->HidePreviewClone(c, x, y, z - 10.0f);
        const int exists = c ? g_world->EntityExists(c) : 0;
        uint64_t sig = 0;
        const bool sigOk = PedPreviewLookSig(lp, sig);
        if (sigOk)
            g_pp.look.Cloned(sig, now);
        static int lines = 0;
        if (lines < 60) {
            ++lines;
            GLOGI("engine: inventory ped preview (0.5.8): clone #%u (%s): CLONE_PED -> %d, DOES_ENTITY_EXIST %s; the player is %s; hidden 10 m under "
                  "%s (%d of 6 hide calls could not be made); look signature %s",
                  g_pp.clones, again ? "the look changed" : "open", c, exists == 1 ? "yes" : (exists == 0 ? "NO" : "unreadable"),
                  inVeh ? "IN A VEHICLE" : "on foot", at ? "the player" : "an UNREADABLE position", failed, sigOk ? "read" : "UNREADABLE (no re-clone on look changes)");
        }
    }

    void PedPreviewGive(int slot, bool searching)
    {
        const bool cleared = g_pp.givesThisOpen > 0 ? g_world->ClearPedInPauseMenu() : true;
        const bool give = g_world->GivePedToPauseMenu(g_ppClone, slot);
        const bool light = g_world->SetPauseMenuPedLighting(true);
        const bool wake = g_world->SetPauseMenuPedSleepState(true);
        ++g_pp.givesThisOpen;
        ++g_pp.gives;
        static int lines = 0;
        if (lines < 60) {
            ++lines;
            GLOGI("engine: inventory ped preview (0.5.8): GIVE_PED_TO_PAUSE_MENU(clone %d, slot %d) %s%s; SET_PAUSE_MENU_PED_LIGHTING(true) %s, "
                  "SET_PAUSE_MENU_PED_SLEEP_STATE(true = wake) %s%s", g_ppClone, slot, give ? "called" : "CANNOT CALL",
                  g_pp.givesThisOpen > 1 ? (cleared ? " (after CLEAR_PED_IN_PAUSE_MENU)" : " (CLEAR_PED_IN_PAUSE_MENU CANNOT CALL)") : "",
                  light ? "called" : "CANNOT CALL", wake ? "called" : "CANNOT CALL",
                  searching ? "; the window is read back in 1.5 s" : "");
        }
        PedPreviewDeleteClone(g_ppCloneOld, "replaced by the clone just given");
    }

    void TickInvPedPreview(int32_t lp)
    {
        namespace pp = sync::pedpreview;
        const double now = NowMs();
        if (g_invPedSlot.load() != g_pp.forcedSeen) {
            g_pp.forcedSeen = g_invPedSlot.load();
            g_pp.m.SetForcedSlot(g_pp.forcedSeen);
        }
        float W = 0, H = 0;
        double ago = 1e9;
        const bool drawn = game::Overlay::InventoryPedViewFrame(&W, &H, &ago);
        pp::GateIn gi;
        gi.switchOn = g_invPedView.load() && g_invPedPreview.load();
        gi.pageOpen = game::Overlay::InventoryOpen();
        gi.drawnAgoMs = drawn ? ago : 1e9;
        gi.ped = lp != 0;
        if (gi.switchOn && gi.pageOpen && lp)
            gi.dead = g_life != Life::Alive || g_world->IsPedDeadOrDying(lp);
        gi.playerPauseMenu = g_pauseMenuActive;     // TickPauseMenu ran first this tick and never counts our own frontend
        gi.gaveUp = g_pp.m.GaveUp();
        const pp::Gate gate = pp::Decide(gi);
        if (gate != g_pp.gate) {
            if (gate == pp::Gate::Live || g_pp.gate == pp::Gate::Live || gi.pageOpen)
                GLOGI("engine: inventory ped preview (0.5.8): %s -> %s", pp::GateName(g_pp.gate), pp::GateName(gate));
            g_pp.gate = gate;
        }

        pp::MachineIn in;
        in.nowMs = now;
        in.want = gate == pp::Gate::Live;
        in.W = W;
        in.H = H;
        const pp::Step step = g_pp.m.StepNow();
        if (step != pp::Step::Idle && step != pp::Step::Claim) {
            in.menuActive = g_world->IsPauseMenuActive();
            ++g_pp.ticks;
            if (in.menuActive) {
                ++g_pp.menuActiveTicks;
                if (g_pp.activeAtMs < 0) {
                    g_pp.activeAtMs = now;
                    uint32_t ver = 0;
                    const bool vr = g_world->CurrentFrontendMenuVersion(ver);
                    GLOGI("engine: inventory ped preview (0.5.8): IS_PAUSE_MENU_ACTIVE yes %.0f ms after the open; GET_CURRENT_FRONTEND_MENU_VERSION %s",
                          now - g_pp.openAtMs, !vr ? "unreadable" : (ver == pp::kFeMenuEmptyNoBackground ? "is ours" : "is ANOTHER menu"));
                }
                uint32_t ver = 0;
                if (!g_world->CurrentFrontendMenuVersion(ver))
                    ++g_pp.versionUnread;
                else if (ver == pp::kFeMenuEmptyNoBackground)
                    ++g_pp.menuOursTicks;
                else
                    ++g_pp.menuOtherTicks;
            }
        }
        if (g_ppClone) {
            in.cloneExists = g_world->EntityExists(g_ppClone) == 1;
            g_pp.cloneExistsTicks += in.cloneExists ? 1u : 0u;
        }
        if (step == pp::Step::Showing && lp && now - g_pp.lookAtMs >= pp::kLookSampleMs) {
            g_pp.lookAtMs = now;
            uint64_t sig = 0;
            if (PedPreviewLookSig(lp, sig))
                in.lookDue = g_pp.look.Feed(sig, now);
        }
        if (g_pp.probeWaiting) {
            game::Overlay::PedProbe p;
            if (game::Overlay::TakePedProbe(&p) && p.seq == g_pp.probeSeq) {
                g_pp.probeWaiting = false;
                in.probeAnswered = true;
                in.probe.verdict = (pp::Verdict)p.verdict;
                in.probe.w = p.w; in.probe.h = p.h; in.probe.pedPx = p.pedPx;
                in.probe.x0 = p.x0; in.probe.y0 = p.y0; in.probe.x1 = p.x1; in.probe.y1 = p.y1;
                // the box in design px (1920 x 1080, y down) too, so a miss can be laid against the layout
                const float s = sync::pedview::DesignScale((float)p.bbW, (float)p.bbH);
                const float ox = std::floor(((float)p.bbW - 1920.0f * s) * 0.5f), oy = std::floor(((float)p.bbH - 1080.0f * s) * 0.5f);
                const auto dx = [&](int v) { return ((float)(p.winX + v) - ox) / s; };
                const auto dy = [&](int v) { return ((float)(p.winY + v) - oy) / s; };
                GLOGI("engine: inventory ped preview (0.5.8): probe #%u slot %d on a %dx%d back buffer: window %dx%d at %d,%d; background %d,%d,%d "
                      "(our backdrop %d,%d,%d); ped pixels %d (%.1f %%), box x %d..%d y %d..%d (design x %.0f..%.0f y %.0f..%.0f from the top)%s%s%s%s; "
                      "%s; inv_ped_probe_%d.bmp %s",
                      p.seq, g_pp.m.Slot(), p.bbW, p.bbH, p.w, p.h, p.winX, p.winY, p.bg[0], p.bg[1], p.bg[2], pp::kBackdrop[0], pp::kBackdrop[1],
                      pp::kBackdrop[2], p.pedPx, p.w * p.h > 0 ? 100.0 * p.pedPx / ((double)p.w * p.h) : 0.0, p.x0, p.x1, p.y0, p.y1, dx(p.x0), dx(p.x1),
                      dy(p.y0), dy(p.y1), p.cutTop ? ", touches the TOP" : "", p.cutBottom ? ", touches the BOTTOM" : "", p.cutLeft ? ", touches the LEFT" : "",
                      p.cutRight ? ", touches the RIGHT" : "", pp::VerdictName((pp::Verdict)p.verdict), g_pp.m.Slot() + 1,
                      p.bmpSaved ? "saved (%LOCALAPPDATA%\\GAGE-MP)" : "not saved");
            }
        }

        const pp::MachineOut o = g_pp.m.Tick(in);
        if (o.teardown)
            PedPreviewTeardown(o.why, now);
        if (o.open)
            PedPreviewOpen(now);
        if (o.nudge) {
            const bool ok = g_world->SetFrontendActive(true);
            GLOGI("engine: inventory ped preview (0.5.8): IS_PAUSE_MENU_ACTIVE still no %.0f ms after the open: SET_FRONTEND_ACTIVE(true) %s",
                  now - g_pp.openAtMs, ok ? "called" : "CANNOT CALL");
        }
        if (o.clone) {
            const bool again = g_pp.m.StepNow() == pp::Step::Cloning && g_pp.givesThisOpen > 0;
            if (again)
                ++g_pp.reclones;
            PedPreviewClone(lp, now, again);
        }
        if (o.give)
            PedPreviewGive(o.slot, !g_pp.m.SearchDone() && g_invPedSlot.load() < 0);
        if (o.probe) {
            ++g_pp.probeSeq;
            g_pp.probeWaiting = true;
            game::Overlay::TakePedProbe(nullptr);   // drop a stale answer
            game::Overlay::RequestPedProbe(g_pp.probeSeq, o.slot);
        }
        if (o.forceClose) {
            const bool ok = g_world->SetPauseMenuActive(false);
            GLOGW("engine: inventory ped preview (0.5.8): the frontend still read active %.0f ms after the teardown: SET_PAUSE_MENU_ACTIVE(false) %s",
                  now - g_pp.teardownAtMs, ok ? "called" : "CANNOT CALL");
        } else if (o.released) {
            GLOGI("engine: inventory ped preview (0.5.8): IS_PAUSE_MENU_ACTIVE no %.0f ms after the teardown (the frontend is released)",
                  now - g_pp.teardownAtMs);
        }
        if (o.gaveUpNow)
            GLOGW("engine: inventory ped preview (0.5.8): GAVE UP for this session (%s; %d failures, last probe: %s): the 0.5.4 camera from now on; "
                  "send the probe lines and %%LOCALAPPDATA%%\\GAGE-MP\\inv_ped_probe_*.bmp", o.why, g_pp.m.Failures(), pp::VerdictName(g_pp.m.LastVerdict()));

        // while the frontend is ours and not being released: its cursor hidden and our backdrop behind it, every tick
        g_ppOwnsFrontend = g_pp.m.OwnsFrontend();
        const pp::Step now2 = g_pp.m.StepNow();
        if (now2 == pp::Step::Opening || now2 == pp::Step::Cloning || now2 == pp::Step::Showing) {
            if (!g_world->SetMouseCursorVisible(false))
                ++g_pp.cursorFailed;
            const pp::NormRect r = pp::BackdropRect(W, H);
            if (r.ok) {
                if (g_world->SetScriptGfxDrawBehindPauseMenu(true))
                    g_pp.behind = true;
                else
                    ++g_pp.behindFailed;
                g_world->DrawRect(r.cx, r.cy, r.w, r.h, pp::kBackdrop[0], pp::kBackdrop[1], pp::kBackdrop[2], 255);
                ++g_pp.backdropDraws;
            }
            if (lp && g_world->IsPedInVehicle(lp))
                ++g_pp.vehicleTicks;
        }
        const bool window = g_pp.m.WindowOpen();
        g_pp.windowTicks += window ? 1u : 0u;
        game::Overlay::SetInventoryPedView(window);

        if (g_pp.ticks && now - g_pp.logAtMs >= 10000.0) {
            g_pp.logAtMs = now;
            GLOGI("engine: inventory ped preview (0.5.8), last 10 s: step %s, slot %d (%s, verdict %s); %u ticks with the frontend ours: "
                  "IS_PAUSE_MENU_ACTIVE yes %u, GET_CURRENT_FRONTEND_MENU_VERSION ours %u / another %u / unreadable %u; DOES_ENTITY_EXIST(clone) yes %u; "
                  "in a vehicle %u; window open %u; backdrop rects %u (draw-behind flag failed %u), cursor hide failed %u; re-clones for a look change %u; "
                  "totals: opens %u, clones %u, gives %u",
                  pp::StepName(g_pp.m.StepNow()), g_pp.m.Slot(), g_invPedSlot.load() >= 0 ? "forced" : (g_pp.m.SearchDone() ? "searched" : "searching"),
                  pp::VerdictName(g_pp.m.LastVerdict()), g_pp.ticks, g_pp.menuActiveTicks, g_pp.menuOursTicks, g_pp.menuOtherTicks, g_pp.versionUnread,
                  g_pp.cloneExistsTicks, g_pp.vehicleTicks, g_pp.windowTicks, g_pp.backdropDraws, g_pp.behindFailed, g_pp.cursorFailed, g_pp.reclones,
                  g_pp.opens, g_pp.clones, g_pp.gives);
            g_pp.ticks = g_pp.menuActiveTicks = g_pp.menuOursTicks = g_pp.menuOtherTicks = g_pp.versionUnread = g_pp.cloneExistsTicks = 0;
            g_pp.vehicleTicks = g_pp.windowTicks = g_pp.backdropDraws = g_pp.behindFailed = g_pp.cursorFailed = g_pp.reclones = 0;
        }
    }

    // The CHARACTER panel's ped, either way (script tick, every tick, before GameWork's early returns): the 0.5.8 preview while inv_ped_mode
    // is on and it has not given up this session, else the 0.5.4 camera. Exactly one of them reports SetInventoryPedView each tick.
    void TickInvPed(int32_t lp)
    {
        const bool preview = g_invPedView.load() && g_invPedPreview.load() && !g_pp.m.GaveUp();
        if (preview || g_pp.m.OwnsFrontend()) {
            if (g_pv.cam)
                PedViewRestore("inv_ped_mode: the preview instead");
            TickInvPedPreview(lp);              // also finishes a release after a give-up
            if (preview)
                return;
        }
        TickInvPedView(lp);
    }

    // WEAPON_UNARMED by GET_HASH_KEY (script tick), checked once against the known constant.
    uint32_t UnarmedHash()
    {
        constexpr uint32_t kKnown = 0xA2719263;
        static uint32_t h = 0;
        if (!h)
        {
            const uint32_t got = g_world->HashKey("WEAPON_UNARMED");
            if (!got)
                return kKnown;                         // native not callable yet: try again next time
            if (got != kKnown)
                PeerLog(true, "engine: GET_HASH_KEY(WEAPON_UNARMED) = 0x%08x, expected 0x%08x; using the game's",
                        (unsigned)got, (unsigned)kKnown);
            h = got;
        }
        return h;
    }

    // The remote player's selected weapon in the ped's hand (script tick). One round: GAGE has no
    // damage sync yet, and nothing here makes the ped fire.
    void ShowPeerWeapon(Peer& pe, uint32_t weapon)
    {
        if (!weapon || weapon == pe.weaponShown)
            return;                                    // 0 = not reported
        // The hash is another client's word: only a real weapon reaches GIVE_WEAPON_TO_PED.
        if (!g_world->IsWeaponValid(weapon))
        {
            pe.weaponShown = weapon;                   // don't re-ask every frame
            return;
        }
        if (weapon != UnarmedHash())
            g_world->GiveWeapon(pe.ped, weapon, 1);
        g_world->SetCurrentPedWeapon(pe.ped, weapon, true);
        pe.weaponShown = weapon;
    }

    // 0.5.9: the peer's character face on its ped (characters::ApplyPeer: nothing when char_face=0 or the ped's model is not the freemode
    // model of the face's sex). Script tick.
    void ApplyPeerFace(Peer& pe, int id)
    {
        pe.faceDirty = false;
        if (pe.hasFace && pe.ped)
            game::characters::ApplyPeer(*g_world, pe.ped, pe.pedModel, pe.face, id, pe.name);
    }

    // Puts the peer's stored look on its ped (its model must be look.model), then its face. Script tick.
    void ApplyPeerLook(Peer& pe, int id)
    {
        pe.lookDirty = false;
        if (!g_world->ApplyPedLook(pe.ped, FromProtoLook(pe.look)) && !pe.lookFailLogged)
        {
            pe.lookFailLogged = true;
            PeerLog(true, "engine: remote peer id=%d (%s): look not applied (model 0x%08x)", id, pe.name.c_str(),
                    (unsigned)pe.look.model);
        }
        ApplyPeerFace(pe, id);                         // 0.5.9: the look's fixed head and hair are replaced by the character's
    }

    // peer_drive=place (stage 1): the ped is set onto the smoothed pose, every frame. No tasks, so
    // no walk animation: the A/B baseline, and the fallback when the ped cannot be read.
    void PlacePeer(Peer& pe, const sync::Pose& pose)
    {
        g_world->SetEntityCoordsNoOffset(pe.ped, pose.k.x, pose.k.y, pose.k.z);
        g_world->SetEntityHeading(pe.ped, Heading360(pose.k.heading));
    }

    // CLEAR_PED_TASKS_IMMEDIATELY (hook.h has only the blending ClearPedTasks): drops a ragdoll or a
    // running task at once.
    void ClearPedTasksNow(int32_t ped)
    {
        uint64_t arg[] = { (uint64_t)(uint32_t)ped };
        g_world->CallNative(game::native::CLEAR_PED_TASKS_IMMEDIATELY, arg, 1, nullptr);
    }

    // 0d: the puppet onto the sender's pose of the edge before TASK_CLIMB / TASK_PUT_PED_DIRECTLY_INTO_COVER goes out the same
    // frame (sync::DecidePrePlace: left, nudged with its task and momentum kept, or set outright with its task cleared). The
    // pose is the interpolated one, i.e. where the sender was ~one render delay ago, at the obstacle. Script tick, ped valid.
    void PrePlacePeer(Peer& pe, const sync::Pose& pose)
    {
        float cx = 0, cy = 0, cz = 0;
        float dist = -1.0f, headErr = 0.0f;
        const float wantHeading = Heading360(pose.k.heading);
        if (g_world->GetEntityCoords(pe.ped, cx, cy, cz))
        {
            const float dx = cx - pose.k.x, dy = cy - pose.k.y, dz = cz - pose.k.z;
            dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            float dh = std::fabs(g_world->GetEntityHeading(pe.ped) - wantHeading);
            dh = std::fmod(dh, 360.0f);
            headErr = dh > 180.0f ? 360.0f - dh : dh;
        }
        const sync::PrePlace pp = sync::DecidePrePlace(dist, headErr);
        if (pp.clearTasks)
            ClearPedTasksNow(pe.ped);
        if (pp.mode != sync::PrePlaceMode::Keep)
            g_world->SetEntityCoordsNoOffset(pe.ped, pose.k.x, pose.k.y, pose.k.z);
        if (pp.setHeading)
            g_world->SetEntityHeading(pe.ped, wantHeading);
        if (pp.mode == sync::PrePlaceMode::Blend)
            ++g_v4.prePlaceNudged;
        else if (pp.mode == sync::PrePlaceMode::Set)
            ++g_v4.prePlaceSet;
    }

    // ---- protocol v4 on remote peds (script tick, no lock) -----------------------------------------

    // A weapon whose shots are shown: valid, not unarmed, NOT explosive or thrown (sync/gtaids.h: the
    // group GROUP_THROWN, or a listed launcher, grenade or bomb: a bullet fired as one would be a REAL
    // rocket or grenade on this machine; a base damage > 0 alone does not exclude them), and a positive
    // base damage (melee reads 0; an uncallable read is not trusted either).
    // Sync plan W3: only CALLABLE answers are cached (sync::WeaponShowCache). The old cache stored "not a gun"
    // for good when IsWeaponValid / WeaponDamage could not be called yet (natives still resolving), so a peer's
    // shots were then never shown for the whole session. IS_WEAPON_VALID reads false both for a hash this build
    // lacks and for an unresolved native, so a false is not trusted either: such a weapon is simply asked again
    // (one cheap native) instead of being remembered.
    bool IsGunWeapon(uint32_t weapon)
    {
        static sync::WeaponShowCache cache;        // script tick only
        return cache.Show(weapon, [](uint32_t w) {
            sync::WeaponProbe p;
            if (w == UnarmedHash())
            {
                p.callable = true;                 // the fists: a real answer, "no"
                return p;
            }
            if (!g_world->IsWeaponValid(w))
                return p;
            const float dmg = g_world->WeaponDamage(w);              // -1 = cannot call; melee / explosive read 0
            if (dmg < 0.0f)
                return p;
            const uint32_t group = g_world->GetWeaponTypeGroup(w);   // 0 = cannot call: the exclusion list still applies
            p.callable = true;
            p.show = !sync::IsExplosiveOrThrownWeapon(w, group) && dmg > 0.0f;
            return p;
        });
    }

    // A melee-class weapon (the fists, GROUP_MELEE / GROUP_UNARMED); only callable answers are cached.
    bool IsMeleeClassWeapon(uint32_t weapon)
    {
        static sync::WeaponShowCache cache;        // script tick only
        return cache.Show(weapon, [](uint32_t w) {
            sync::WeaponProbe p;
            if (w == UnarmedHash())
            {
                p.callable = true;
                p.show = true;
                return p;
            }
            const uint32_t group = g_world->GetWeaponTypeGroup(w);
            if (!group)
                return p;                          // cannot call (or no group): asked again
            p.callable = true;
            p.show = sync::IsMeleeWeapon(w, group);
            return p;
        });
    }

    // The server maximum health in wire units (GTA max health minus the death line at 100): Welcome.playerHealth
    // (protocol v5), which is also what the ped maximum was set to (ApplyServerMaxHealth). The reaction decisions
    // take their percentages of it.
    int ServerMaxHp(int32_t lp)
    {
        (void)lp;
        const int ph = g_playerHealth.load();
        return ph >= 1 ? ph : 100;
    }

    // R4 T3. A swing in flight whose ped is gone, dead or seated is nobody's to judge: the probe is cancelled (so SwingProbe::Active() cannot
    // stay true and hold the locomotion controller off for good) and its survey entry is handed back as not evidence.
    void AbandonPeerSwing(Peer& pe)
    {
        pe.swingProbe.Cancel();
        if (pe.loopStop.Armed())
        {
            // R5: a looped clip in flight never ends by itself: end it now (the next swing, if any, replaces it anyway)
            if (g_world && pe.ped && pe.loopDict && pe.loopClip)
                g_world->StopAnimTask(pe.ped, pe.loopDict, pe.loopClip, -8.0f);
            pe.loopStop.Cancel();
            ++g_swingDiag.loopAbandoned;
        }
        if (pe.swingPlanIdx >= 0)
        {
            g_swingSurvey.OnResult((size_t)pe.swingPlanIdx, false, sync::SwingProbe::Progress::None, true);
            pe.swingPlanIdx = -1;
        }
    }

    // A fresh remote ped: hit detection needs it damageable, so it is held at kPeerHpPool and
    // restored after every hit (ProbePeerHits). If the pool cannot be set (or read back) the ped
    // stays invincible and nothing is detected on it.
    void SetupPeerPed(Peer& pe, int id)
    {
        pe.hpPool = 0;
        pe.lastShotSeq = -1;
        pe.deadShown = false;
        pe.seatedVeh = -1;
        pe.seatedSeat = -1;
        pe.collisionOff = false;                       // a fresh ped has its collision
        pe.leaveUntilMs = 0;
        pe.shotDebt.Clear();                           // sync plan W3: a fresh ped owes no shots, has no reaction
        pe.meleeEvents.Reset();
        pe.infiniteAmmo = false;
        pe.hitReact.Clear();
        pe.reactLifted = false;
        pe.reactRagdolled = false;
        pe.reactClip = false;
        pe.reactDamage = 0;
        pe.reactFlags = 0;
        pe.edges.Reset();                              // W6: a fresh ped has done no reload / stealth / cover / entering yet
        pe.stealthOn = false;
        pe.stealthAtMs = 0;
        pe.cover.Reset();
        pe.enterVeh = -1;
        pe.enterTaskMs = 0;
        pe.emote.OnNewPed(NowMs());
        AbandonPeerSwing(pe);                          // 0e: a swing issued on the old ped is nobody's to judge
        pe.swingPending.Clear();
        pe.audit.CancelAll();                          // T2: a one-shot asked of the old ped is nobody's to judge
        pe.water.Reset();                              // W8: a fresh ped has no water edge and no climb in progress
        pe.climb.Reset();
        pe.seatLimiter.Reset();                        // W9: a fresh ped has no warp history, no bail-out velocity, no impact
        pe.carryFromMs = pe.carryOutMs = 0;
        pe.lastImpactMs = -1e9;
        if (!g_peerHits.load(std::memory_order_relaxed))
            return;
        g_world->SetEntityMaxHealth(pe.ped, kPeerHpPool);
        g_world->SetEntityHealth(pe.ped, kPeerHpPool);
        const int got = g_world->GetEntityHealth(pe.ped);
        if (got == kPeerHpPool)
        {
            g_world->SetEntityInvincible(pe.ped, false);
            // No head-shot multiplier: a headshot would take the whole pool at once and kill the puppet
            // before the damage is counted. (ProbePeerHits reports what a one-blow kill still takes.)
            g_world->SetPedSuffersCriticalHits(pe.ped, false);
            pe.hpPool = kPeerHpPool;
            return;
        }
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            PeerLog(true, "engine: remote peer id=%d: health pool %d did not take (reads %d): remote peds stay "
                    "invincible, local hit detection OFF", id, kPeerHpPool, got);
        }
    }

    // T2: the puppet one-shots the game was asked to show, and whether it did (libs/sync/oneshot.h). Totals for the 10 s line
    // "engine: anims onfoot (T2)"; script tick only.
    sync::OneShotStats g_oneShot[(size_t)sync::OneShot::kCount], g_oneShotLast[(size_t)sync::OneShot::kCount];
    void TallyOneShot(sync::OneShot k, sync::OneShotEvent ev, double showMs)
    {
        sync::OneShotStats& t = g_oneShot[(size_t)k];
        if (ev == sync::OneShotEvent::Shown)
        {
            ++t.shown;
            t.showMsSum += showMs;
            if (showMs > t.showMsMax) t.showMsMax = showMs;
        }
        else if (ev == sync::OneShotEvent::Missed) ++t.missed;
        else if (ev == sync::OneShotEvent::Unknown) ++t.unknown;
    }
    void ArmOneShot(Peer& pe, sync::OneShot k, double nowMs, bool expect = true)
    {
        ++g_oneShot[(size_t)k].issued;
        TallyOneShot(k, pe.audit.Arm(k, nowMs, expect), 0.0);   // a request this one replaced is judged on the spot
    }
    // Every frame a kind is armed: the game's own readback on the puppet. Script tick, ped valid.
    void ObservePeerOneShots(Peer& pe, double nowMs)
    {
        for (size_t i = 0; i < (size_t)sync::OneShot::kCount; ++i)
        {
            const sync::OneShot k = (sync::OneShot)i;
            if (!pe.audit.Armed(k))
                continue;
            int playing = 0;
            switch (k)
            {
            case sync::OneShot::Jump:    playing = g_world->IsPedJumping(pe.ped) ? 1 : 0; break;
            case sync::OneShot::Reload:  playing = g_world->IsPedReloading(pe.ped) ? 1 : 0; break;
            case sync::OneShot::Stealth: playing = g_world->GetPedStealthMovement(pe.ped) ? 1 : 0; break;
            case sync::OneShot::Swim:    playing = g_world->IsPedSwimming(pe.ped) ? 1 : 0; break;
            case sync::OneShot::Ragdoll: playing = g_world->IsPedRagdoll(pe.ped) ? 1 : 0; break;
            case sync::OneShot::kCount:  break;
            }
            const sync::OneShotEvent ev = pe.audit.Observe(k, playing, nowMs);
            if (ev != sync::OneShotEvent::None)
                TallyOneShot(k, ev, pe.audit.LastShowMs());
        }
    }
    // R5 telemetry: every frame, the sender's swim / ragdoll flag (the pose's, at the render time) against the game's own read-back of the puppet. The reads happen
    // only when StateMirror::WantRead says so (a flag on, an edge being judged, otherwise twice a second), so an upright puppet on land costs 4 native calls a second.
    // Counts only: nothing here acts on the result. Script tick, ped valid.
    void ObservePeerEdges(Peer& pe, const proto::PedState& ps, double nowMs)
    {
        if (pe.mirrorPed != pe.ped)
        {
            pe.swimMirror.Rebase();
            pe.ragMirror.Rebase();
            pe.mirrorPed = pe.ped;
        }
        int swimRead = -2, ragRead = -2;
        if (pe.swimMirror.WantRead(nowMs))
            swimRead = g_world->IsPedSwimming(pe.ped) ? 1 : 0;
        if (pe.ragMirror.WantRead(nowMs))
            ragRead = g_world->IsPedRagdoll(pe.ped) ? 1 : 0;
        pe.swimMirror.Update(nowMs, (ps.flags & proto::kPfSwimming) != 0, swimRead);
        pe.ragMirror.Update(nowMs, (ps.flags & proto::kPfRagdoll) != 0, ragRead);
    }
    // The 10 s line of the two mirrors; empty when neither had an edge or an unasked rise in the window (the window is taken either way).
    std::string EdgeLine(int id, Peer& pe)
    {
        const std::string sw = sync::StateMirrorLine("swim", pe.swimMirror.TakeWindow());
        const std::string rg = sync::StateMirrorLine("ragdoll", pe.ragMirror.TakeWindow());
        if (sw.empty() && rg.empty())
            return std::string();
        char head[160];
        std::snprintf(head, sizeof(head), "engine: edges id=%d (%s), last 10 s (sender flag against IS_PED_SWIMMING / IS_PED_RAGDOLL on the puppet): ", id, pe.name.c_str());
        return std::string(head) + (sw.empty() ? "swim: no edge" : sw) + " | " + (rg.empty() ? "ragdoll: no edge" : rg);
    }

    std::string OneShotLine()
    {
        std::string out;
        char buf[160];
        for (size_t i = 0; i < (size_t)sync::OneShot::kCount; ++i)
        {
            const sync::OneShotStats& a = g_oneShot[i];
            const sync::OneShotStats& b = g_oneShotLast[i];
            const uint32_t issued = a.issued - b.issued, shown = a.shown - b.shown, missed = a.missed - b.missed, unknown = a.unknown - b.unknown;
            if (!issued && !shown && !missed && !unknown)
                continue;
            const double avg = shown ? (a.showMsSum - b.showMsSum) / shown : 0.0;
            std::snprintf(buf, sizeof(buf), "%s%s asked %u, SHOWN %u (avg %.0f ms), NOT shown %u, unreadable %u", out.empty() ? "" : "; ",
                          sync::OneShotName((sync::OneShot)i), issued, shown, avg, missed, unknown);
            out += buf;
        }
        return out.empty() ? std::string("none asked") : out;
    }

    // W8 item 3: every locomotion Reset goes through here so the 10 s line can name the caller. Reset = the task list was
    // cleared or replaced (an aim task, an emote, a vehicle walk, ClearPedTasks): the gait task must be issued again.
    void LocoReset(Peer& pe, LocoSite site)
    {
        pe.hardSites.Add((size_t)site);
        pe.loco.Reset();
    }
    // SoftReset = the ped was touched but its gait task may still run: re-issue only when the turn / drift / arrival rule says so.
    void LocoSoft(Peer& pe, LocoSite site)
    {
        pe.softSites.Add((size_t)site);
        pe.loco.SoftReset();
    }

    // W11 item 2: the anchor entity of a puppet. DeleteAnchor with the puppet (every place its ped is deleted); EnsureAnchor
    // once per DrivePeer in anchor mode: true = the entity exists and can be moved this frame. A failed create (the model
    // is not in memory yet, the pool is full) is retried every kAnchorRetryMs and meanwhile the controller walks straight.
    constexpr double kAnchorRetryMs = 1000.0;
    bool g_anchorBroken = false;   // TASK_GO_TO_ENTITY could not be called at all: straight for the rest of the run (script tick)
    void DeleteAnchor(Peer& pe)
    {
        if (pe.anchor && g_world)
            g_world->DeleteEntity(pe.anchor);          // zeroes the handle
        pe.anchor = 0;
        if (pe.swingAnchor && g_world)
        {
            g_world->DeleteEntity(pe.swingAnchor);     // R5: the melee task's borrowed anchor goes with the puppet
            ++g_swingDiag.anchorlessRemoved;
        }
        pe.swingAnchor = 0;
    }
    bool EnsureAnchor(Peer& pe, const sync::Pose& pose, double nowMs)
    {
        if (pe.anchor && !g_world->EntityExists(pe.anchor))
        {
            pe.anchor = 0;                             // deleted under us (a game cleanup): the task is lost with it
            LocoReset(pe, kSiteAnchorLost);
        }
        if (pe.anchor)
            return true;
        if (nowMs - pe.anchorTryMs < kAnchorRetryMs)
            return false;
        pe.anchorTryMs = nowMs;
        // SpawnPed: CREATE_PED not networked, mission ped, invincible, frozen; then hidden and without collision. Never
        // registered anywhere else (no peer, no hit detection).
        const int32_t h = g_world->SpawnPed(kCompanionModel, pose.k.x, pose.k.y, pose.k.z);
        if (!h)
        {
            ++pe.anchorSpawnFails;
            return false;
        }
        g_world->SetEntityVisible(h, false);
        g_world->SetEntityCollision(h, false);
        pe.anchor = h;
        return true;
    }

    // ---- sync plan W3, combat client: melee swing, muzzle flash, real shots (script tick, no lock) ----

    // The swing a remote ped plays when its shotSeq steps with a melee-class weapon in hand. Every dictionary and
    // clip name is INFERRED (not checked against the game files): each dictionary is asked with
    // DOES_ANIM_DICT_EXIST once and the answer logged, the first usable candidate is used, and a ped is never touched
    // when none exists. Flags 16|32 (upper body + secondary) leave the legs and the gait task alone.
    // 0e: the dictionary request is REPEATED until HAS_ANIM_DICT_LOADED (sync::DictStream; it used to be sent once and
    // counted sent even when the call failed), a swing that finds the dictionary still streaming is kept for 250 ms
    // (sync::PendingSwing), and after the task IS_ENTITY_PLAYING_ANIM says whether the ped really plays the clip
    // (sync::SwingProbe): a clip never seen playing twice is demoted for the next candidate (sync::SwingBook).
    struct SwingClip
    {
        const char* dict;
        const char* clip;
        int exists = -2;       // DOES_ANIM_DICT_EXIST: -2 not asked yet, 1 yes, 0 no, -1 cannot call (asked again)
        bool held = false;     // g_animDicts holds a reference (the dictionary is wanted until the session ends)
        sync::DictStream stream;
    };
    SwingClip g_swingUnarmed[] = { { "melee@unarmed@streamed_core", "heavy_punch_a" },
                                   { "melee@unarmed@streamed_variations", "heavy_punch_a" } };
    SwingClip g_swingArmed[] = { { "melee@large_wpn@streamed_core", "ground_attack_on_spot" },
                                 { "melee@knife@streamed_core", "ground_attack_on_spot" } };
    struct SwingSet
    {
        SwingClip* list;
        size_t n;
        sync::SwingBook book;
        bool init = false;
    };
    SwingSet g_swingSetUnarmed{ g_swingUnarmed, sizeof(g_swingUnarmed) / sizeof(g_swingUnarmed[0]) };
    SwingSet g_swingSetArmed{ g_swingArmed, sizeof(g_swingArmed) / sizeof(g_swingArmed[0]) };
    constexpr int kSwingMs = 400;                      // INFERRED: about one punch
    constexpr int kSwingFlags = 16 | 32;               // AF_UPPERBODY | AF_SECONDARY

    // A session ended: the swing dictionaries are handed back (script tick).
    void ReleaseSwingDicts()
    {
        for (SwingSet* set : { &g_swingSetUnarmed, &g_swingSetArmed })
            for (size_t i = 0; i < set->n; ++i)
            {
                SwingClip& c = set->list[i];
                if (c.held)
                {
                    c.held = false;
                    if (g_animDicts.Release(c.dict))
                        g_world->RemoveAnimDict(c.dict);
                }
                c.stream.Reset();
            }
    }

    void NoteDictReady(sync::AnimKind kind, double ms)
    {
        sync::AnimKindStats& k = g_animDiag.Of(kind);
        ++k.dictReady;
        k.dictReadyMsSum += ms;
        if (ms > k.dictReadyMsMax)
            k.dictReadyMsMax = ms;
    }

    // The candidate clip of a swing to use, its dictionary kept requested until it has loaded (and again if the game
    // streams it out). nullptr = none exists in this game (or not checkable yet). Logs once per dictionary what
    // DOES_ANIM_DICT_EXIST said, and once per dictionary how long it took to load.
    SwingClip* SwingClipFor(uint32_t weapon, double nowMs, SwingSet** setOut = nullptr, size_t* idxOut = nullptr)
    {
        SwingSet& set = weapon == UnarmedHash() ? g_swingSetUnarmed : g_swingSetArmed;
        if (!set.init)
        {
            set.init = true;
            set.book.Reset(set.n);
        }
        for (size_t i = 0; i < set.n; ++i)
        {
            SwingClip& c = set.list[i];
            if (c.exists >= 0)
                continue;
            const int was = c.exists;
            c.exists = g_world->AnimDictExists(c.dict);
            set.book.SetExists(i, c.exists);
            if (c.exists != was && c.exists != -2)
                PeerLog(c.exists != 1, "engine: melee swing: dictionary %s (clip %s, INFERRED names) %s in this game",
                        c.dict, c.clip, c.exists == 1 ? "exists" : c.exists == 0 ? "does NOT exist" : "cannot be checked");
        }
        const int pick = set.book.Pick();
        if (pick < 0)
            return nullptr;
        SwingClip& c = set.list[(size_t)pick];
        if (!c.held)
        {
            g_animDicts.Acquire(c.dict);
            c.held = true;
        }
        if (c.stream.Due(nowMs))
        {
            const uint32_t loadsBefore = c.stream.Loads();
            if (c.stream.Tick(nowMs, g_world->HasAnimDictLoaded(c.dict)) && !g_world->RequestAnimDict(c.dict))
                c.stream.NoteRequestFailed();
            if (c.stream.Loads() != loadsBefore)
            {
                NoteDictReady(sync::AnimKind::PeerSwing, c.stream.LastLoadMs());
                if (g_animFirstUse.Note(std::string("load:") + c.dict))
                    PeerLog(false, "engine: anim first use: swing dictionary %s loaded %.0f ms after the first request (%u requests "
                            "sent, %u calls failed)", c.dict, c.stream.LastLoadMs(), c.stream.Requests(), c.stream.FailedRequests());
            }
        }
        if (setOut)
            *setOut = &set;
        if (idxOut)
            *idxOut = (size_t)pick;
        return &c;
    }

    // R3 T6. The locomotion controller's task count on a puppet (go-straight / go-to-anchor / stand-still issues): a rise between the
    // swing's issue and its read-back means a locomotion task was issued on the ped meanwhile, the usual way a secondary clip is lost.
    uint32_t LocoTaskCount(const Peer& pe)
    {
        const sync::LocoStats& ls = pe.loco.Stats();
        return ls.goStraight + ls.standStill;
    }

    // The melee-task rung: TASK_PUT_PED_DIRECTLY_INTO_MELEE against the puppet's own invisible, invincible, collision-less anchor ped, so
    // its blows can land on nobody that matters; the AI melee damage modifier is held at 0 for the window as well (the puppet's fists cannot
    // hurt the local player even if its AI picks a new target), and PeerSwingService trips the breaker if our health moves meanwhile.
    // R5 melee_loop: the melee-task rung needs a ped to fight. A puppet in Anchor mode has its locomotion anchor; one in Blend / Straight mode has none, so the
    // rung used to be impossible there (blend mode never has an anchor). A short-lived one is made in front of it (the locomotion anchor's recipe: not
    // networked, mission ped, invincible, frozen, invisible, no collision; never registered anywhere) and removed kSwingAnchorKeepMs after the last melee task.
    // 0 = none could be had (melee_loop=0, no model in memory, the pool is full): the rung is skipped and counted, never forced.
    constexpr double kSwingAnchorKeepMs = 3000.0;
    int32_t SwingTargetFor(Peer& pe, double nowMs)
    {
        if (pe.anchor && g_world->EntityExists(pe.anchor))
            return pe.anchor;
        if (!g_meleeLoopWanted.load(std::memory_order_relaxed))
            return 0;
        if (pe.swingAnchor && !g_world->EntityExists(pe.swingAnchor))
            pe.swingAnchor = 0;                        // deleted under us (a game clean-up)
        if (!pe.swingAnchor)
        {
            float x = 0, y = 0, z = 0;
            if (!pe.ped || !g_world->GetEntityCoords(pe.ped, x, y, z))
                return 0;
            const float h = g_world->GetEntityHeading(pe.ped) * 0.0174532925f;      // GTA heading: 0 = +y, counter-clockwise
            const int32_t a = g_world->SpawnPed(kCompanionModel, x - std::sin(h) * 1.5f, y + std::cos(h) * 1.5f, z);
            if (!a)
            {
                ++g_swingDiag.anchorlessFailed;
                return 0;
            }
            g_world->SetEntityVisible(a, false);
            g_world->SetEntityCollision(a, false);
            pe.swingAnchor = a;
            ++g_swingDiag.anchorlessSpawned;
        }
        pe.swingAnchorUntilMs = nowMs + kSwingAnchorKeepMs;
        return pe.swingAnchor;
    }

    bool IssuePeerMeleeTask(Peer& pe, double nowMs)
    {
        const int32_t target = SwingTargetFor(pe, nowMs);
        if (!target)
        {
            ++g_swingDiag.meleeTaskNoTarget;           // no anchor ped, and none could be made: nothing safe to aim at
            return false;
        }
        const int32_t lp = g_localPed.load();
        pe.swingLocalHp = lp ? g_world->GetEntityHealth(lp) + g_world->GetPedArmour(lp) : 0;
        if (!g_aiMeleeApplied)
        {
            g_meleeTaskModSet = g_world->SetAiMeleeWeaponDamageModifier(0.0f);
            g_meleeTaskModUntilMs = nowMs + 1200.0;
        }
        if (!g_world->TaskPutPedDirectlyIntoMelee(pe.ped, target, 0.2f, 0.6f, 0.0f, 0))
            return false;
        pe.swingMeleeUntilMs = nowMs + kSwingMs + 150.0;
        return true;
    }

    // R4 T3. Whether the survey may issue its melee-task entry on this puppet: auto mode, the breaker closed, the puppet's own anchor ped exists
    // (not in loco_mode=straight, the 0.4.9 default: the entry is then skipped and counted).
    bool SurveyMeleeTaskOk(const Peer& pe)
    {
        return g_swingLadder.Mode() == sync::SwingMode::Auto && !g_swingLadder.MeleeTripped() &&
               ((pe.anchor && g_world->EntityExists(pe.anchor)) || g_meleeLoopWanted.load(std::memory_order_relaxed));   // R5: or one can be borrowed
    }

    // R4 T3. Once the survey plan is done and every attempt has reported: pin the ladder to the attempt whose swings PROGRESSED best and log the table.
    void SurveyFinishCheck()
    {
        const int w = g_swingSurvey.TakeWinner();
        if (w == -2)
            return;
        std::string tab;
        for (size_t i = 0; i < g_swingSurvey.PlanSize(); ++i)
        {
            char t[200];
            std::snprintf(t, sizeof(t), "%s[%s: issued %u, progressed %u, blended out / flicker / frozen %u, not seen %u]", tab.empty() ? "" : " ",
                          sync::SurveyAttemptName(g_swingSurvey.PlanAt(i)).c_str(), g_swingSurvey.Issued(i), g_swingSurvey.Progressed(i),
                          g_swingSurvey.Failed(i), g_swingSurvey.Unseen(i));
            tab += t;
        }
        if (w >= 0)
        {
            const sync::SwingAttempt win = g_swingSurvey.PlanAt((size_t)w);
            g_swingLadder.PinTo(win.variant);
            g_swingDurationMs = win.durationMs;
            char line[1600];
            std::snprintf(line, sizeof(line), "engine: melee survey DONE (%u planned swings, %u entries skipped): the swing that PROGRESSED best is '%s'; the ladder is pinned there. %s",
                          (unsigned)g_swingSurvey.Planned(), g_swingSurvey.Skipped(), sync::SurveyAttemptName(win).c_str(), tab.c_str());
            g_peerLog.push_back({ false, line });
        }
        else
        {
            char line[1600];
            std::snprintf(line, sizeof(line), "engine: melee survey DONE (%u planned swings, %u entries skipped): NO attempt was still playing at +100 / +250 ms (or none could be read): %s "
                          "-- see the per-swing lines for what took it off the ped, and when", (unsigned)g_swingSurvey.Planned(), g_swingSurvey.Skipped(), tab.c_str());
            g_peerLog.push_back({ true, line });
        }
    }

    // ---- 0.4.13 diagnostics: WHY a puppet's clip / ragdoll / vehicle exit does not show (reads only, nothing here changes the game) ----
    // The 0.4.12 logs: every swing and every full-body emote read "playing" on the first frame after the task (IS_ENTITY_PLAYING_ANIM) and
    // the player saw nothing; the arbiter then logged "no longer playing" at +1.0 s (its 600 ms grace + 400 ms), on our own ped too. A one-frame
    // read cannot tell a clip that stays from one that is gone a frame later. A trace follows ONE issue on every script tick and logs: the first
    // frame the probe flips (playing -> not playing, "gone at +N ms": the same few ms for every clip = something cancels it; the clip's own
    // length = it finished), the task types GET_IS_TASK_ACTIVE reports right after the issue, at the marks and at the moment it is gone (what
    // replaced it), and the ped's ragdoll / falling / speed / distance moved and distance to the sender's pose. Probe: the clip (anim), the melee
    // task, a ragdoll, or an exit from a vehicle (3 s). At most 6 at once and 48 per run; anim_trace=0 (HKCU\Software\GAGE-MP) switches it off.
    enum class TraceProbe : uint8_t { Anim, Melee, Ragdoll, Exit };
    struct ClipTrace
    {
        bool used = false;
        TraceProbe probe = TraceProbe::Anim;
        int32_t ped = 0;
        std::string tag, dict, clip;
        double startMs = 0, seenMs = -1, goneMs = -1;
        size_t mark = 0;
        int last = -2;                                 // the probe's last read (-2 = none yet)
        uint32_t no = 0, frames = 0, flips = 0;
        float x0 = 0, y0 = 0, z0 = 0;
        // 0.4.14, Anim only: the last phase (0..1) and length (s) read while the clip was on the ped (-1 = never read). A clip gone
        // at phase ~1 FINISHED; gone at a low phase was CUT (the task types / script status at that frame say by what).
        float lastPhase = -1, lenS = -1;
    };
    std::string TraceAnimState(const ClipTrace& t)
    {
        char b[160];
        const int st = g_world->GetScriptTaskStatus(t.ped, game::World::kScriptTaskPlayAnim);
        if (t.lastPhase < 0.0f)
            std::snprintf(b, sizeof(b), "phase never read, PLAY_ANIM script status %d", st);
        else
            std::snprintf(b, sizeof(b), "last phase %.2f of %.2f s (%s), PLAY_ANIM script status %d", (double)t.lastPhase, (double)t.lenS,
                          t.lastPhase >= 0.9f ? "near the end: FINISHED" : "CUT before its end", st);
        return b;
    }
    constexpr size_t kTraceSlots = 6;
    constexpr uint32_t kTraceRunMax = 48;
    constexpr double kTraceClipMarks[] = { 20.0, 100.0, 250.0, 600.0, 1300.0 };
    constexpr double kTraceExitMarks[] = { 100.0, 500.0, 1000.0, 1500.0, 2000.0, 3000.0 };
    ClipTrace g_clipTraces[kTraceSlots];
    uint32_t g_clipTraceCount = 0;
    int g_clipTraceOn = -1;                            // -1: the registry was not read yet
    std::string ActiveTaskTypes(int32_t ped);          // defined with the gait line below

    void BeginTrace(TraceProbe probe, int32_t ped, const std::string& tag, const char* dict = "", const char* clip = "")
    {
        if (!ped)
            return;
        if (g_clipTraceOn < 0)
            g_clipTraceOn = reg::Get(L"anim_trace") != L"0" ? 1 : 0;
        if (!g_clipTraceOn || g_clipTraceCount >= kTraceRunMax)
            return;
        ClipTrace* t = nullptr;
        for (ClipTrace& s : g_clipTraces)
            if (!s.used)
            {
                t = &s;
                break;
            }
        if (!t)
            return;                                    // every slot is busy: this issue is not traced
        *t = ClipTrace();
        t->used = true;
        t->probe = probe;
        t->ped = ped;
        t->tag = tag;
        t->dict = dict ? dict : "";
        t->clip = clip ? clip : "";
        t->startMs = NowMs();
        t->no = ++g_clipTraceCount;
        g_world->GetEntityCoords(ped, t->x0, t->y0, t->z0);
        std::string clipInfo;
        if (probe == TraceProbe::Anim)
        {
            float raw = -1.0f;
            const bool have = g_world->GetAnimDuration(t->dict.c_str(), t->clip.c_str(), raw);
            char b[160];
            std::snprintf(b, sizeof(b), "; GET_ANIM_DURATION %s, PLAY_ANIM script status %d", have ? std::to_string(raw).c_str() : "not callable",
                          g_world->GetScriptTaskStatus(ped, game::World::kScriptTaskPlayAnim));
            clipInfo = b;
        }
        GLOGI("engine: trace #%u begins: %s; task types right after the issue [%s]%s%s", t->no, tag.c_str(), ActiveTaskTypes(ped).c_str(),
              clipInfo.c_str(), g_clipTraceCount == kTraceRunMax ? " (the last trace of this run)" : "");
    }

    // The probe's read: 1 / 0, -1 unreadable. An exit reads "still in a vehicle".
    int TraceRead(const ClipTrace& t)
    {
        switch (t.probe)
        {
        case TraceProbe::Anim:    return g_world->IsEntityPlayingAnim(t.ped, t.dict.c_str(), t.clip.c_str());
        case TraceProbe::Melee:   return g_world->IsPedInMeleeCombat(t.ped);
        case TraceProbe::Ragdoll: return g_world->IsPedRagdoll(t.ped) ? 1 : 0;
        case TraceProbe::Exit:    return g_world->IsPedInVehicle(t.ped) ? 1 : 0;
        }
        return -1;
    }

    const char* TraceProbeName(TraceProbe p)
    {
        switch (p)
        {
        case TraceProbe::Anim:    return "IS_ENTITY_PLAYING_ANIM";
        case TraceProbe::Melee:   return "IS_PED_IN_MELEE_COMBAT";
        case TraceProbe::Ragdoll: return "IS_PED_RAGDOLL";
        case TraceProbe::Exit:    return "IS_PED_IN_ANY_VEHICLE";
        }
        return "?";
    }

    // Script tick, once per frame (a few natives per active trace; the task-type scan only at marks and at the flip).
    void TickClipTraces(double nowMs)
    {
        for (ClipTrace& t : g_clipTraces)
        {
            if (!t.used)
                continue;
            if (!g_world->EntityExists(t.ped))
            {
                GLOGI("engine: trace #%u (%s): the ped no longer exists at +%.0f ms", t.no, t.tag.c_str(), nowMs - t.startMs);
                t.used = false;
                continue;
            }
            const double el = nowMs - t.startMs;
            ++t.frames;
            const int now = TraceRead(t);
            const bool animKind = t.probe == TraceProbe::Anim;
            if (animKind && now == 1)
            {
                float cur = -1.0f, len = -1.0f, ph = -1.0f;
                if (g_world->GetEntityAnimTimes(t.ped, t.dict.c_str(), t.clip.c_str(), cur, len, &ph))
                {
                    t.lastPhase = ph;
                    t.lenS = len;
                }
            }
            if (now != t.last)
            {
                const bool firstRead = t.last == -2;
                if (now == 1 && t.seenMs < 0)
                    t.seenMs = el;
                const bool gone = !firstRead && t.last == 1 && now == 0 && t.goneMs < 0 && t.probe != TraceProbe::Exit;
                if (gone)
                    t.goneMs = el;
                if (t.flips++ < 10)
                    GLOGI("engine: trace #%u (%s): %s %s -> %d at +%.0f ms (frame %u)%s%s%s%s%s", t.no, t.tag.c_str(), TraceProbeName(t.probe),
                          firstRead ? "first read" : (t.last == 1 ? "1" : t.last == 0 ? "0" : "-1"), now, el, t.frames,
                          gone ? "; GONE, task types now [" : "", gone ? ActiveTaskTypes(t.ped).c_str() : "", gone ? "]" : "",
                          gone && animKind ? "; " : "", gone && animKind ? TraceAnimState(t).c_str() : "");
                t.last = now;
            }
            const bool exitKind = t.probe == TraceProbe::Exit;
            const double* marks = exitKind ? kTraceExitMarks : kTraceClipMarks;
            const size_t nMarks = exitKind ? sizeof kTraceExitMarks / sizeof kTraceExitMarks[0] : sizeof kTraceClipMarks / sizeof kTraceClipMarks[0];
            while (t.mark < nMarks && el >= marks[t.mark])
            {
                float x = 0, y = 0, z = 0;
                const bool haveXyz = g_world->GetEntityCoords(t.ped, x, y, z);
                float toPose = -1.0f;
                for (const auto& kv : g_peers)
                    if (kv.second.ped == t.ped && kv.second.poseOk && haveXyz)
                    {
                        const float dx = x - kv.second.pose.k.x, dy = y - kv.second.pose.k.y, dz = z - kv.second.pose.k.z;
                        toPose = std::sqrt(dx * dx + dy * dy + dz * dz);
                    }
                GLOGI("engine: trace #%u (%s) +%.0f ms (mark %.0f, frame %u): %s %d, ragdoll %d, falling %d, speed %.2f m/s, moved %.2f m (z %+.2f), "
                      "%s%.2f m from the sender's pose; task types [%s]%s%s", t.no, t.tag.c_str(), el, marks[t.mark], t.frames, TraceProbeName(t.probe), now,
                      g_world->IsPedRagdoll(t.ped) ? 1 : 0, g_world->IsPedFalling(t.ped) ? 1 : 0, (double)g_world->GetEntitySpeed(t.ped),
                      haveXyz ? std::sqrt((x - t.x0) * (x - t.x0) + (y - t.y0) * (y - t.y0) + (z - t.z0) * (z - t.z0)) : -1.0, haveXyz ? (double)(z - t.z0) : 0.0,
                      toPose < 0.0f ? "no pose: " : "", (double)toPose, ActiveTaskTypes(t.ped).c_str(),
                      animKind ? "; " : "", animKind ? TraceAnimState(t).c_str() : "");
                ++t.mark;
            }
            if (t.mark >= nMarks)
            {
                GLOGI("engine: trace #%u (%s) done: %s first read 1 at %s, %s; %u frames in %.0f ms (%.1f ms a frame)%s%s", t.no, t.tag.c_str(),
                      TraceProbeName(t.probe), t.seenMs >= 0 ? (std::to_string((int)t.seenMs) + " ms").c_str() : "never",
                      t.goneMs >= 0 ? ("GONE at +" + std::to_string((int)t.goneMs) + " ms").c_str()
                              : (exitKind ? (t.last == 1 ? "still in a vehicle at the end" : "out of the vehicle at the end")
                                          : (t.last == 1 ? "still 1 at the end" : "never went 1 -> 0")),
                      t.frames, el, t.frames ? el / t.frames : 0.0, animKind ? "; " : "", animKind ? TraceAnimState(t).c_str() : "");
                t.used = false;
            }
        }
    }

    // TASK_PLAY_ANIM of the swing (or the melee task); the probe follows it (PeerSwingService). `retry`: the one bounded re-issue after a
    // locomotion task replaced it. R4 T3: while the survey is on, the attempt (variant + duration) is the survey's next plan entry, otherwise the
    // ladder's rung with the current duration; the probe now FOLLOWS the swing (+100 / +250 / +450 ms) so SwingProbe::Active() covers the window.
    void IssuePeerSwing(Peer& pe, SwingSet& set, size_t idx, double nowMs, bool late, bool retry = false)
    {
        SwingClip& c = set.list[idx];
        sync::AnimKindStats& dk = g_animDiag.Of(sync::AnimKind::PeerSwing);
        sync::SwingAttempt att;
        size_t planIdx = (size_t)-1;
        if (retry)
            att = pe.swingAttempt;
        else
        {
            if (pe.swingProbe.Active() || pe.loopStop.Armed())
                AbandonPeerSwing(pe);                  // the previous swing's follow was not over: it is overtaken (a survey entry of it is queued again)
            att.variant = g_swingLadder.Current();
            att.durationMs = g_swingDurationMs;
            const sync::SwingMode sm = g_swingLadder.Mode();
            if (g_swingSurvey.Active() && (sm == sync::SwingMode::Auto || sm == sync::SwingMode::Play))
            {
                const sync::SwingAttempt sa = g_swingSurvey.Next(SurveyMeleeTaskOk(pe), &planIdx);
                if (planIdx != (size_t)-1)
                    att = sa;
                else
                    SurveyFinishCheck();               // only skipped entries were left: the survey is over
            }
        }
        // R5: the looped clip runs for the clip's length and is ended by us; the plain one plays its whole length (the survey plan says the same; the ladder's rung does not)
        if (att.variant == sync::SwingVariant::Looped)
        {
            att.durationMs = sync::kSwingClipLength;
            att.stopAfterMs = sync::kLoopStopAfterMs;
        }
        else if (att.variant == sync::SwingVariant::Plain)
            att.durationMs = sync::kSwingClipLength;
        const sync::SwingVariant var = att.variant;
        const bool ok = var == sync::SwingVariant::MeleeTask
                            ? IssuePeerMeleeTask(pe, nowMs)
                            : g_world->TaskPlayAnim(pe.ped, c.dict, c.clip, 8.0f, -8.0f, att.durationMs, sync::SwingVariantFlags(var), AnimPhase());
        if (ok)
        {
            ++g_v4.peerSwings;
            ++dk.issued;
            if (late)
                ++dk.late;
            if (retry)
                ++g_swingDiag.retried;
            else
            {
                ++g_swingDiag.issued, ++g_swingDiag.variantIssued[(size_t)var];
                pe.swingNo = ++g_swingNo;
                pe.swingPlanIdx = planIdx != (size_t)-1 ? (int)planIdx : -1;
                pe.swingFirst = sync::SwingProbe::Result::Idle;
                pe.swingSay = pe.swingPlanIdx >= 0 || g_swingTestForce;
                g_swingTestForce = false;
                if (pe.swingPlanIdx >= 0)
                    ++g_swingDiag.surveyed;
                if (pe.swingSay)
                {
                    char say[200];
                    std::snprintf(say, sizeof(say), "Swing test %u: %s", (unsigned)(pe.swingPlanIdx >= 0 ? g_swingSurvey.Step() : 0u),
                                  sync::SurveyAttemptName(att).c_str());
                    game::Overlay::PushSystemLine(say);
                }
            }
            pe.swingAttempt = att;
            {
                char tg[200];
                std::snprintf(tg, sizeof(tg), "peer %s swing '%s', TASK_PLAY_ANIM flags %u, duration %d ms%s", pe.name.c_str(),
                              sync::SurveyAttemptName(att).c_str(), var == sync::SwingVariant::MeleeTask ? 0u : (unsigned)sync::SwingVariantFlags(var),
                              att.durationMs, retry ? " (retry)" : "");
                if (var == sync::SwingVariant::MeleeTask)
                    BeginTrace(TraceProbe::Melee, pe.ped, tg);
                else
                    BeginTrace(TraceProbe::Anim, pe.ped, tg, c.dict, c.clip);
            }
            // the window the locomotion controller stays away: the swing's duration (the melee task's own end, 150 ms past kSwingMs, for that rung)
            pe.swingProbe.Begin(nowMs, idx, g_meleeFollowWanted.load(std::memory_order_relaxed), att.durationMs,
                                var == sync::SwingVariant::MeleeTask ? kSwingMs + 200.0 : att.stopAfterMs > 0 ? att.stopAfterMs + 60.0 : -1.0);
            pe.swingResetPending = sync::SwingVariantReplacesGait(var);   // a whole-body clip replaced the gait task: the controller re-issues it when the swing is over
            if (pe.swingWalkClipAtMs >= 0.0)
            {
                pe.swingWalkClipAtMs = -1.0;
                pe.swingWalkDiag.ClipRead(-1);         // 0.5.7: a new swing began before the previous clip read was due
            }
            if (att.stopAfterMs > 0 && var != sync::SwingVariant::MeleeTask)
            {
                pe.loopStop.Arm(nowMs, att.stopAfterMs);   // R5: a looped clip is ended by us (PeerSwingService)
                pe.loopDict = c.dict;
                pe.loopClip = c.clip;
            }
            else
                pe.loopStop.Cancel();
            pe.swingProbeArmed = &set == &g_swingSetArmed;
            pe.swingVariant = var;
            pe.swingLocoMark = LocoTaskCount(pe);
            pe.swingIssuedMs = nowMs;
            if (!retry)
                pe.swingRetried = false;
        }
        else
        {
            ++g_v4.peerSwingFailed;
            ++dk.failed;
            ++g_swingDiag.notIssuedFailed;
            if (planIdx != (size_t)-1)
                g_swingSurvey.OnResult(planIdx, false, sync::SwingProbe::Progress::None, true);   // not issued: not evidence
        }
    }

    // A remote ped swings: a shotSeq step or a rising kPfMelee with a melee-class weapon in hand (combat 4, receiver;
    // the pair is one swing, see PlayPeerShots). Every way a swing is not shown is counted (0c: the live test showed
    // "0 peer swings shown" with no hint why).
    void PlayPeerSwing(Peer& pe, int id, uint32_t weapon, double nowMs)
    {
        ++g_v4.peerSwingSteps;
        ++g_swingDiag.seen;
        sync::AnimKindStats& dk = g_animDiag.Of(sync::AnimKind::PeerSwing);
        ++dk.requested;
        {
            const sync::SwingMode want = (sync::SwingMode)g_meleeAnimWanted.load(std::memory_order_relaxed);
            if (want != g_swingLadder.Mode() && !(want == sync::SwingMode::MeleeTask && g_swingLadder.Mode() == sync::SwingMode::Play && g_swingLadder.MeleeTripped()))
            {
                g_swingLadder.SetMode(want, g_meleeLoopWanted.load(std::memory_order_relaxed));
                PeerLog(false, "engine: melee swing: melee_anim=%s (auto: upper-body clip, then not-interruptable, full body, the melee task, each rung only "
                        "after the read-back says the swing was not seen playing)", sync::SwingModeName(want));
            }
        }
        if (!g_swingLadder.Enabled())
        {
            ++g_swingDiag.notIssuedOff;
            return;
        }
        if (pe.emote.Active() || pe.seatedVeh >= 0 || pe.collisionOff || pe.hitReact.Active())
        {
            ++g_v4.peerSwingOwned;                     // an emote, a seat, a climb or a reaction owns the ped
            ++dk.owned;
            ++g_swingDiag.notIssuedOwned;
            return;
        }
        if (g_world->IsPedRagdoll(pe.ped))
        {
            ++g_swingDiag.notIssuedRagdoll;            // a ragdolling ped plays no clip: not the swing's fault
            return;
        }
        SwingSet* set = nullptr;
        size_t idx = 0;
        SwingClip* c = SwingClipFor(weapon, nowMs, &set, &idx);
        if (!c && g_swingLadder.OnNoClips())
        {
            g_swingSurvey.Start(0);                    // R4 T3: no clip exists, nothing to survey: the melee task is the only rung
            ++g_swingDiag.escalations;
            PeerLog(true, "engine: melee swing: none of the candidate clip dictionaries exists in this game: the ladder goes straight to the melee task "
                    "(melee_anim=%s)", sync::SwingModeName(g_swingLadder.Mode()));
        }
        if (!c && g_swingLadder.Current() == sync::SwingVariant::MeleeTask)
        {
            set = weapon == UnarmedHash() ? &g_swingSetUnarmed : &g_swingSetArmed;   // the melee task needs no clip: the set only carries the read-back
            IssuePeerSwing(pe, *set, 0, nowMs, false);
            return;
        }
        if (!c)
        {
            ++g_v4.peerSwingNoDict;
            ++dk.dictMissing;
            ++g_swingDiag.notIssuedDictMissing;
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                PeerLog(true, "engine: melee swing: peer id=%d swung but none of the candidate dictionaries exists in this "
                        "game (names are INFERRED): remote melee swings are not shown", id);
            }
            return;
        }
        if (g_swingLadder.Current() != sync::SwingVariant::MeleeTask && !g_world->HasAnimDictLoaded(c->dict))
        {
            ++g_v4.peerSwingNoDict;                    // still streaming: kept for 250 ms (PeerSwingService plays it when it loads)
            pe.swingPending.Arm(nowMs);
            return;
        }
        IssuePeerSwing(pe, *set, idx, nowMs, false);
    }

    // Once per frame per remote ped, after the swing trigger: the swing that waited for its dictionary, and the read-back of the last
    // issued one (IS_ENTITY_PLAYING_ANIM, or IS_PED_IN_MELEE_COMBAT for the melee task). R3 T6: the read-back is the EFFECT counter, with
    // the reason when it says no (a locomotion task issued meanwhile, a busy ped, nothing explains it), one bounded re-issue after a
    // locomotion task replaced the swing, and the ladder (sync::SwingLadder) climbs on a streak of unexplained misses.
    void PeerSwingService(Peer& pe, int id, uint32_t weapon, bool meleeWeapon, double nowMs)
    {
        sync::AnimKindStats& dk = g_animDiag.Of(sync::AnimKind::PeerSwing);
        // the melee-task rung's global modifier is handed back after its window (unless ai_melee_mod=1 holds it), and its puppet's task ends
        if (g_meleeTaskModUntilMs > 0.0 && nowMs >= g_meleeTaskModUntilMs)
        {
            g_meleeTaskModUntilMs = 0.0;
            if (g_meleeTaskModSet && !g_aiMeleeApplied)
                g_world->ResetAiMeleeWeaponDamageModifier();
            g_meleeTaskModSet = false;
        }
        if (pe.swingMeleeUntilMs > 0.0)
        {
            const int32_t lp = g_localPed.load();
            if (lp && g_world->GetEntityHealth(lp) + g_world->GetPedArmour(lp) < pe.swingLocalHp)
            {
                // The local player lost health while a puppet was in melee: whatever else caused it, this rung does not run again.
                ++g_swingDiag.meleeTaskHarm;
                g_swingLadder.TripMeleeTask();
                pe.swingLocalHp = 0;
                PeerLog(true, "engine: melee swing: our health dropped while peer id=%d was in a melee task: the melee-task rung is DISABLED for this "
                        "session (the ladder falls back to the animations)", id);
            }
            if (nowMs >= pe.swingMeleeUntilMs)
            {
                pe.swingMeleeUntilMs = 0.0;
                g_world->ClearPedTasks(pe.ped);        // the puppet leaves the melee task ...
                pe.loco.Reset();                       // ... and the locomotion controller issues its gait task again
            }
        }
        if (pe.swingAnchor && pe.swingMeleeUntilMs <= 0.0 && nowMs >= pe.swingAnchorUntilMs)
        {
            g_world->DeleteEntity(pe.swingAnchor);     // R5: the melee task is over: the borrowed anchor ped goes
            pe.swingAnchor = 0;
            ++g_swingDiag.anchorlessRemoved;
        }
        if (pe.loopStop.Armed())
        {
            // R5: a looped swing clip never ends by itself. At the swing's end STOP_ANIM_TASK ends THAT clip; the next frames read it back: gone = done; still
            // playing or unreadable = the puppet's tasks are cleared instead and the locomotion controller issues its gait task again (a loop must not run on)
            if (pe.loopStop.Due(nowMs))
            {
                ++g_swingDiag.loopStops;
                g_world->StopAnimTask(pe.ped, pe.loopDict, pe.loopClip, -8.0f);
                pe.loopStop.Stopped(nowMs);
            }
            else if (pe.loopStop.Awaiting())
            {
                const sync::SwingLoopStop::Outcome lo = pe.loopStop.Check(nowMs, g_world->IsEntityPlayingAnim(pe.ped, pe.loopDict, pe.loopClip));
                if (lo == sync::SwingLoopStop::Outcome::Gone)
                    ++g_swingDiag.loopGone;
                else if (lo == sync::SwingLoopStop::Outcome::StillPlaying || lo == sync::SwingLoopStop::Outcome::Unread)
                {
                    const bool still = lo == sync::SwingLoopStop::Outcome::StillPlaying;
                    ++(still ? g_swingDiag.loopStill : g_swingDiag.loopUnread);
                    g_world->ClearPedTasks(pe.ped);
                    pe.loco.Reset();
                    if (g_animFirstUse.Note(still ? "loop-stop-still" : "loop-stop-unread"))
                        PeerLog(true, "engine: melee swing: the looped clip %s / %s on peer id=%d was %s %d ms after STOP_ANIM_TASK: its tasks were cleared instead "
                                "(melee_loop=0 puts the round-4 plan back)", pe.loopDict ? pe.loopDict : "?", pe.loopClip ? pe.loopClip : "?", id,
                                still ? "STILL PLAYING" : "unreadable", (int)sync::SwingLoopStop::kCheckAfterMs);
                }
            }
        }
        if (pe.swingPending.Armed())
        {
            if (!meleeWeapon || pe.seatedVeh >= 0 || pe.collisionOff || pe.hitReact.Active() || pe.emote.Active())
                pe.swingPending.Clear();
            else
            {
                SwingSet* set = nullptr;
                size_t idx = 0;
                SwingClip* c = SwingClipFor(weapon, nowMs, &set, &idx);
                if (c && pe.swingPending.Take(nowMs, g_world->HasAnimDictLoaded(c->dict)))
                    IssuePeerSwing(pe, *set, idx, nowMs, true);
                else if (!pe.swingPending.Armed())
                    ++g_swingDiag.notIssuedDictLate;   // it waited 250 ms and the dictionary did not load
            }
        }
        const bool swingActive = pe.swingProbe.Active();
        if (pe.swingResetPending && !swingActive)
        {
            pe.swingResetPending = false;
            pe.loco.Reset();                           // R4 T3: the whole-body clip took the gait task: re-issue it now that the swing is over
        }
        if (!swingActive && pe.swingWalkClipAtMs >= 0.0)
        {
            pe.swingWalkClipAtMs = -1.0;
            pe.swingWalkDiag.ClipRead(-1);             // 0.5.7: the swing ended before the clip read after a walk-through issue was due
        }
        if (!swingActive)
            return;
        SwingSet& set = pe.swingProbeArmed ? g_swingSetArmed : g_swingSetUnarmed;
        const size_t ci = pe.swingProbe.Candidate();
        SwingClip& c = set.list[ci];
        const bool viaMelee = pe.swingVariant == sync::SwingVariant::MeleeTask;
        const int playing = viaMelee ? g_world->IsPedInMeleeCombat(pe.ped) : g_world->IsEntityPlayingAnim(pe.ped, c.dict, c.clip);
        if (pe.swingWalkClipAtMs >= 0.0 && nowMs - pe.swingWalkClipAtMs >= 100.0)
        {
            pe.swingWalkClipAtMs = -1.0;
            pe.swingWalkDiag.ClipRead(viaMelee ? -1 : playing);   // 0.5.7: the clip 100+ ms after our gait task went out under it
        }
        float animCur = -1.0f, animTotal = -1.0f;
        if (!viaMelee && playing == 1)
            g_world->GetEntityAnimTimes(pe.ped, c.dict, c.clip, animCur, animTotal);   // neutral (-1) until GET_ENTITY_ANIM_CURRENT_TIME / _TOTAL_TIME exist
        const sync::SwingProbe::Result r = pe.swingProbe.Tick(nowMs, playing, animCur, animTotal);
        if (r == sync::SwingProbe::Result::Pending)
        {
            // One bounded retry: a locomotion task was issued on the ped since the swing and the clip is not playing (yet): the task most
            // likely replaced it, so it is issued once more, within the first 150 ms (a punch is shown late rather than not at all).
            if (sync::SwingRetryDue(viaMelee, pe.swingRetried, playing, nowMs - pe.swingIssuedMs, LocoTaskCount(pe), pe.swingLocoMark))
            {
                pe.swingRetried = true;
                IssuePeerSwing(pe, set, ci, nowMs, false, true);
            }
            return;
        }
        if (r == sync::SwingProbe::Result::Idle || r == sync::SwingProbe::Result::Following)
            return;
        // The ladder's reaction to a swing, once its outcome is final: a survey swing never moves it (the survey pins the winner); a swing seen
        // AND progressed pins the variant; one not seen, or seen and blended out, counts as a miss and may move it up a rung.
        const auto settleLadder = [&](bool good, bool busy, const char* howBad) {
            if (pe.swingPlanIdx >= 0)
                return;
            if (good)
            {
                if (g_swingLadder.Mode() != sync::SwingMode::Off && pe.swingVariant == g_swingLadder.Current() && !g_swingLadder.Pinned())
                    PeerLog(false, "engine: melee swing: variant '%s' is SEEN playing on peer id=%d (read-back%s): pinned", sync::SwingVariantName(pe.swingVariant), id,
                            g_meleeFollowWanted.load() ? ", and still playing at +100 / +250 ms" : "");
                g_swingLadder.OnResult(sync::SwingProbe::Result::Shown, false);
                return;
            }
            const bool up = g_swingLadder.OnResult(sync::SwingProbe::Result::NotShown, busy);
            if (up)
            {
                ++g_swingDiag.escalations;
                PeerLog(true, "engine: melee swing: the swing was %s on peer id=%d: the ladder moves up to '%s' (melee_anim=%s)", howBad, id,
                        sync::SwingVariantName(g_swingLadder.Current()), sync::SwingModeName(g_swingLadder.Mode()));
            }
        };
        const auto surveyFinish = [&]() { SurveyFinishCheck(); };
        // the per-swing line (the first 60 of a session, then one per 5 s)
        const auto swingLine = [&](sync::SwingProbe::Result first, bool busy) {
            static uint32_t lines = 0;
            static double lastMs = -1e9;
            if (lines >= 60 && nowMs - lastMs < 5000.0)
                return;
            ++lines;
            lastMs = nowMs;
            sync::SwingLineIn li;
            li.peerId = id;
            li.index = pe.swingNo;
            li.surveyStep = pe.swingPlanIdx >= 0 ? (int)(g_swingSurvey.Step() > 0 ? g_swingSurvey.Step() - 1 : 0) : -1;
            li.surveySteps = (int)g_swingSurvey.Planned();
            li.variant = pe.swingVariant;
            li.flags = sync::SwingVariantFlags(pe.swingVariant);
            li.durationMs = pe.swingAttempt.durationMs;
            li.stopAfterMs = pe.swingAttempt.stopAfterMs;
            li.dict = viaMelee ? "(melee task, no clip)" : c.dict;
            li.clip = viaMelee ? "-" : c.clip;
            li.startMs = pe.swingIssuedMs;
            li.retried = pe.swingRetried;
            li.locoTasksMeanwhile = LocoTaskCount(pe) - pe.swingLocoMark;
            li.busy = busy;
            li.firstResult = first;
            li.progress = pe.swingProbe.Verdict();
            li.tailPlaying = pe.swingProbe.TailStillPlaying();
            li.cutShort = pe.swingProbe.ClipCutShort();
            li.first = pe.swingProbe.First();
            for (size_t k = 0; k < sync::SwingProbe::kSamples; ++k)
                li.at[k] = pe.swingProbe.At(k);
            g_peerLog.push_back({ false, sync::SwingLine(li) });
        };
        if (r == sync::SwingProbe::Result::Shown || r == sync::SwingProbe::Result::Followed)
        {
            const bool followed = r == sync::SwingProbe::Result::Followed;
            if (!followed)
            {
                // the first sighting (IS_ENTITY_PLAYING_ANIM / IS_PED_IN_MELEE_COMBAT true): the effect counter of R3, kept as it was
                ++dk.shown;
                ++g_swingDiag.shown;
                ++g_swingDiag.variantShown[(size_t)pe.swingVariant];
                if (pe.swingRetried)
                    ++g_swingDiag.retriedShown;
                pe.swingFirst = sync::SwingProbe::Result::Shown;
                if (!viaMelee)
                    set.book.OnProbe(ci, r);           // the clip is verified for good
                if (g_animFirstUse.Note(std::string("swing-shown:") + c.dict + "/" + c.clip))
                    PeerLog(false, "engine: anim first use: swing clip %s / %s is SEEN playing (IS_ENTITY_PLAYING_ANIM true) on peer id=%d: the "
                            "names are right", c.dict, c.clip, id);
                if (g_meleeFollowWanted.load(std::memory_order_relaxed))
                    return;                            // the progress check decides the ladder (Followed)
                settleLadder(true, false, "");
                return;
            }
            // the progress check is over
            const sync::SwingProbe::Progress pv = pe.swingProbe.Verdict();
            using PG = sync::SwingProbe::Progress;
            const bool busy = pe.seatedVeh >= 0 || pe.hitReact.Active() || pe.emote.Active() || g_world->IsPedRagdoll(pe.ped);
            if (pv == PG::Progressed)
                ++g_swingDiag.progressed, ++g_swingDiag.variantProgressed[(size_t)pe.swingVariant];
            else if (pv == PG::BlendedOut)
                ++g_swingDiag.blendedOut;
            else if (pv == PG::Flicker)
                ++g_swingDiag.flicker;
            else if (pv == PG::Frozen)
                ++g_swingDiag.frozen;
            else
                ++g_swingDiag.progressUnread;
            if (pe.swingProbe.TailStillPlaying())
                ++g_swingDiag.tail;
            if (pe.swingProbe.ClipCutShort())
                ++g_swingDiag.cutShort;
            swingLine(sync::SwingProbe::Result::Shown, busy);
            if (pe.swingSay)
            {
                char say[200];
                std::snprintf(say, sizeof(say), "  -> %s", sync::SwingProbe::ProgressName(pv));
                game::Overlay::PushSystemLine(say);
            }
            if (pe.swingPlanIdx >= 0)
            {
                g_swingSurvey.OnResult((size_t)pe.swingPlanIdx, true, pv, busy);
                pe.swingPlanIdx = -1;
                surveyFinish();
                return;
            }
            settleLadder(pv == PG::Progressed || pv == PG::Unread, busy,
                         pv == PG::BlendedOut ? "seen playing at the first frame and BLENDED OUT within 250 ms" : pv == PG::Flicker ? "seen playing and then lost" : "seen playing but its anim time did not advance");
            return;
        }
        if (r == sync::SwingProbe::Result::NotShown)
        {
            ++dk.unseen;
            ++g_swingDiag.unseen;
            // Why: busy (ragdoll, a reaction, a seat: not the swing's fault) > a locomotion task issued meanwhile > nothing explains it.
            const bool busy = pe.seatedVeh >= 0 || pe.hitReact.Active() || pe.emote.Active() || g_world->IsPedRagdoll(pe.ped);
            const sync::SwingMiss why = sync::ClassifySwingMiss(busy, pe.swingRetried, LocoTaskCount(pe), pe.swingLocoMark);
            const bool overwritten = why == sync::SwingMiss::Overwritten;
            if (why == sync::SwingMiss::Busy)
                ++g_swingDiag.unseenBusy;
            else if (overwritten)
                ++g_swingDiag.unseenOverwritten;
            else
                ++g_swingDiag.unseenPlain;
            // The clip is blamed only for a miss nothing else explains: a busy ped or a locomotion task is not the name's fault.
            const bool demoted = !viaMelee && why == sync::SwingMiss::Plain && pe.swingPlanIdx < 0 ? set.book.OnProbe(ci, r) : false;
            swingLine(sync::SwingProbe::Result::NotShown, busy);
            if (pe.swingPlanIdx >= 0)
            {
                g_swingSurvey.OnResult((size_t)pe.swingPlanIdx, false, sync::SwingProbe::Progress::None, busy);
                pe.swingPlanIdx = -1;
                surveyFinish();
            }
            else if (!demoted)
                settleLadder(false, busy, busy ? "not seen playing (ped busy)" : overwritten ? "not seen playing (a locomotion task was issued meanwhile)" : "not seen playing (nothing explains it)");
            if (g_animFirstUse.Note(std::string("swing-unseen:") + c.dict + "/" + c.clip + "/" + std::to_string((int)pe.swingVariant)))
                PeerLog(true, "engine: anim first use: swing %s %s / %s [%s] was issued on peer id=%d but the read-back never saw it in 250 ms (%s): "
                        "the clip name is probably WRONG (names are INFERRED; /e <dict> <clip> tests one by hand), a task replaced it, or that native cannot "
                        "see an upper-body secondary task", viaMelee ? "melee task" : "clip", c.dict, c.clip, sync::SwingVariantName(pe.swingVariant), id,
                        busy ? "ped busy" : overwritten ? "a locomotion task was issued meanwhile" : "nothing else explains it");
            if (demoted)
                PeerLog(true, "engine: melee swing: clip %s / %s demoted after two swings never seen playing: the next candidate is used",
                        c.dict, c.clip);
            return;
        }
        ++g_swingDiag.unreadable;                      // Unknown: the read-back native never answered
        set.book.OnProbe(ci, r);
        swingLine(sync::SwingProbe::Result::Unknown, false);
        if (pe.swingPlanIdx >= 0)
        {
            g_swingSurvey.OnResult((size_t)pe.swingPlanIdx, false, sync::SwingProbe::Progress::None, true);   // unreadable: not evidence
            pe.swingPlanIdx = -1;
            surveyFinish();
        }
    }

    // R4 T3: /swing. A punch on the nearest other player's puppet within 25 m, through the same path a real swing takes (PlayPeerSwing), LOCAL ONLY:
    // the friend sees nothing and nobody is hurt. While the survey is on it plays the next plan entry and the overlay names it ("Swing test N: ...")
    // and then what the read-back said, so one person alone with one puppet in view can say which attempt he SAW. After the survey it plays the
    // pinned variant; typing /swing again starts a new pass.
    void RunSwingTest(int32_t lp)
    {
        float lx = 0, ly = 0, lz = 0;
        if (!join::IsConnected() || !g_world->GetEntityCoords(lp, lx, ly, lz))
        {
            game::Overlay::PushSystemLine("Swing test: join a server first.");
            return;
        }
        Peer* best = nullptr;
        int bestId = -1;
        float bestD2 = 25.0f * 25.0f;
        for (auto& kv : g_peers)
        {
            Peer& pe = kv.second;
            float px, py, pz;
            if (!pe.ped || pe.deadShown || pe.seatedVeh >= 0 || !g_world->GetEntityCoords(pe.ped, px, py, pz))
                continue;
            const float d2 = (px - lx) * (px - lx) + (py - ly) * (py - ly) + (pz - lz) * (pz - lz);
            if (d2 < bestD2)
            {
                bestD2 = d2;
                best = &pe;
                bestId = kv.first;
            }
        }
        if (!best)
        {
            game::Overlay::PushSystemLine("Swing test: no other player within 25 m.");
            return;
        }
        const sync::SwingMode sm = g_swingLadder.Mode();
        if ((sm == sync::SwingMode::Auto || sm == sync::SwingMode::Play) && !g_swingSurvey.Active() && g_swingLadder.Enabled())
            g_swingSurvey.Start(1, g_swingLadder.Loop());   // a finished survey: a new pass
        g_swingTestForce = true;
        PlayPeerSwing(*best, bestId, UnarmedHash(), NowMs());
        g_swingTestForce = false;
        GLOGI("engine: /swing on peer id=%d (%.1f m away)", bestId, std::sqrt(bestD2));
    }

    // A muzzle flash for a remote shot: the "core" particle asset's per-class effect (sync::MuzzleFlashEffect,
    // INFERRED names) started on the ped at the muzzle. The effect is anchored to the ped, not the weapon
    // object, with the muzzle's offset taken into the ped's frame and pointed along the shot. One per call (a
    // burst frame shows one flash). Whether the game started each effect is logged once per name. peer_muzzle=0
    // turns it off.
    void PlayPeerMuzzleFlash(Peer& pe, uint32_t weapon, float ox, float oy, float oz, float tx, float ty, float tz,
                             double nowMs)
    {
        if (!g_peerMuzzle.load(std::memory_order_relaxed))
            return;
        static const char* const kAsset = "core";
        static double requestedAtMs = -1e9;
        if (!g_world->HasNamedPtfxAssetLoaded(kAsset))
        {
            if (nowMs - requestedAtMs >= 2000.0)       // idempotent; asked again until the asset is in
            {
                requestedAtMs = nowMs;
                g_world->RequestNamedPtfxAsset(kAsset);
            }
            return;
        }
        float px, py, pz;
        if (!g_world->GetEntityCoords(pe.ped, px, py, pz))
            return;
        const float kRad = 3.14159265f / 180.0f;
        const float headingDeg = g_world->GetEntityHeading(pe.ped);
        const float c = std::cos(headingDeg * kRad), s = std::sin(headingDeg * kRad);
        const float dx = ox - px, dy = oy - py;
        // The ped's frame: x to its right (cos h, sin h), y forward (-sin h, cos h), z up.
        const float offX = dx * c + dy * s, offY = -dx * s + dy * c, offZ = oz - pz;
        const float ax = tx - ox, ay = ty - oy, az = tz - oz;
        const float pitch = std::atan2(az, std::sqrt(ax * ax + ay * ay)) / kRad;
        const float yaw = std::atan2(-ax, ay) / kRad - headingDeg;      // the shot's heading relative to the ped's
        g_world->UseParticleFxAsset(kAsset);
        const char* fx = sync::MuzzleFlashEffect(g_world->GetWeaponTypeGroup(weapon));
        const bool ok = g_world->StartParticleFxNonLoopedOnEntity(fx, pe.ped, offX, offY, offZ, pitch, 0.0f, yaw, 1.0f,
                                                                  false, false, false);
        if (ok)
            ++g_v4.muzzleFlashes;
        static std::set<std::string> logged;           // script tick only
        if (logged.insert(fx).second)
            PeerLog(!ok, "engine: muzzle flash: effect '%s' (asset core, INFERRED name) %s", fx,
                    ok ? "started" : "was NOT started by the game (wrong name, or the asset is not in)");
    }

    // True when the shot muzzle -> target passes no player body on this screen: not the local player, not any
    // other peer's ped. A real peer shot damages whatever it hits unless the AI damage modifiers are 0 (TickAiDamage); it must not hurt anybody.
    bool ShotLineClear(const Peer& shooter, float ox, float oy, float oz, float tx, float ty, float tz)
    {
        constexpr float kBodyRadiusM = 0.6f, kBodyHalfHeightM = 1.0f;
        if (g_localPed.load() &&
            sync::SegmentTouchesCapsule(ox, oy, oz, tx, ty, tz, g_localX.load(), g_localY.load(), g_localZ.load(),
                                        kBodyRadiusM, kBodyHalfHeightM))
            return false;
        for (const auto& kv : g_peers)
        {
            const Peer& o = kv.second;
            float x, y, z;
            if (&o == &shooter || o.gone || !o.ped || !g_world->GetEntityCoords(o.ped, x, y, z))
                continue;
            if (sync::SegmentTouchesCapsule(ox, oy, oz, tx, ty, tz, x, y, z, kBodyRadiusM, kBodyHalfHeightM))
                return false;
        }
        return true;
    }

    // Shots: every change of the pose's 4-bit shotSeq is that many shots, owed in the ped's ShotDebt and fired
    // three a frame (none are discarded: the old code dropped everything past 3), shown as zero-damage bullets
    // from the ped's muzzle toward its aim point (damage is the server's), plus a muzzle flash. With
    // real fire (the default; peer_shoot=script keeps the bullet) a shot is a REAL shot of the ped's weapon instead (the clear-line check is lifted once the AI damage modifiers are 0). A melee-class
    // weapon's step is a swing (PlayPeerSwing).
    void PlayPeerShots(Peer& pe, int id, const sync::Pose& pose, double nowMs)
    {
        const proto::PedState& ps = pose.ped;
        const int seq = ps.shotSeq & 15;
        if (pe.lastShotSeq < 0)
        {
            pe.lastShotSeq = seq;                      // a new ped replays nothing
            return;
        }
        const int n = (seq - pe.lastShotSeq) & 15;
        pe.lastShotSeq = seq;
        if (pe.deadShown)
        {
            pe.shotDebt.Clear();
            AbandonPeerSwing(pe);                      // R4 T3: a dead puppet judges no swing (and holds no locomotion task off)
            return;
        }
        // 0c: a swing is a shotSeq step OR the rising edge of the sender's kPfMelee (the flag was sent since v5 and the
        // receiver ignored it, so a swing whose counter step was missed was never shown); one swing within 300 ms.
        const bool meleeWeapon = IsMeleeClassWeapon(ps.weapon);
        if (sync::PeerSwingDue(pe.swingGate, nowMs, n, meleeWeapon, (ps.flags & proto::kPfMelee) != 0))
            PlayPeerSwing(pe, id, ps.weapon, nowMs);
        else if (meleeWeapon)
            SwingClipFor(ps.weapon, nowMs);            // load the swing dictionary before the first swing, not during it
        PeerSwingService(pe, id, ps.weapon, meleeWeapon, nowMs);   // 0e: a swing that waited, the IS_ENTITY_PLAYING_ANIM probe
        if (meleeWeapon)
        {
            pe.shotDebt.Clear();                       // fists and clubs owe no bullets
            return;
        }
        if (n == 0 && pe.shotDebt.Owed() == 0)
            return;
        if (!IsGunWeapon(ps.weapon))
        {
            pe.shotDebt.Clear();                       // the weapon changed or is not a gun: nothing is owed
            return;
        }
        pe.shotDebt.Add(n);
        const int fire = pe.shotDebt.Take();
        if (fire <= 0)
            return;
        float ox, oy, oz;
        if (!g_world->GetWeaponMuzzlePos(pe.ped, ox, oy, oz))
        {
            if (!g_world->GetEntityCoords(pe.ped, ox, oy, oz))
                return;
            oz += 0.5f;
        }
        float tx, ty, tz;
        if (ps.flags & proto::kPfAiming)
        {
            tx = pose.k.x + ps.aimX;
            ty = pose.k.y + ps.aimY;
            tz = pose.k.z + ps.aimZ;
        }
        else
        {
            const float r = pose.k.heading * 3.14159265f / 180.0f;
            tx = ox - std::sin(r) * 50.0f;
            ty = oy + std::cos(r) * 50.0f;
            tz = oz;
        }
        // W8 item 6: with the AI damage modifiers at 0 (TickAiDamage) a real shot hurts nobody on this machine, so the
        // clear-line restriction is lifted; without them it stays (a shot through a body would hurt it).
        bool real = false;
        if (g_peerShootPed.load(std::memory_order_relaxed))
        {
            real = g_aiDamageApplied || ShotLineClear(pe, ox, oy, oz, tx, ty, tz);
            static bool loggedLift = false;
            if (g_aiDamageApplied && !loggedLift)
            {
                loggedLift = true;
                PeerLog(false, "engine: real peer shots: the AI damage modifiers are 0, so the clear-line check is lifted");
            }
        }
        if (real && !pe.infiniteAmmo)
            pe.infiniteAmmo = g_world->SetPedInfiniteAmmoClip(pe.ped, true);   // the weapon object really fires
        for (int i = 0; i < fire; ++i)
        {
            bool done = false;
            if (real && pe.infiniteAmmo)
            {
                done = g_world->PedShootAtCoord(pe.ped, tx, ty, tz);
                if (done)
                    ++g_v4.peerShotsReal;
            }
            if (!done)
                g_world->ShootBulletBetween(ox, oy, oz, tx, ty, tz, 0, ps.weapon, pe.ped, true, false, -1.0f);
        }
        g_v4.peerShots += (uint32_t)fire;
        PlayPeerMuzzleFlash(pe, ps.weapon, ox, oy, oz, tx, ty, tz, nowMs);
    }

    // Death: the pose's kPfDead (server-dead or client-reported) or a Killed in the last 1.5 s (it
    // arrives reliably before the pose catches up). On the edge the ped is killed locally (it plays
    // the death); back alive it is respawned at its pose. Returns true while dead.
    bool UpdatePeerDeath(Peer& pe, int id, const sync::Pose& pose, double nowMs)
    {
        // killedAtMs is when the death is SHOWN: the Killed's receipt plus this peer's pose delay (OnKilled), so the
        // puppet does not die ahead of its own interpolated pose.
        const bool dead = (pose.ped.flags & proto::kPfDead) ||
                          (pe.killedAtMs > 0 && nowMs >= pe.killedAtMs && nowMs - pe.killedAtMs < 1500.0);
        if (dead && !pe.deadShown)
        {
            pe.deadShown = true;
            pe.emote.Clear();
            SyncPeerEmoteDict(pe);
            if (pe.seatedVeh < 0)
                ClearPedTasksNow(pe.ped);
            g_world->SetPedCanRagdoll(pe.ped, true);
            g_world->SetEntityInvincible(pe.ped, false);
            g_world->SetEntityHealth(pe.ped, 0);
            ++g_v4.peerDeaths;
            PeerLog(false, "engine: remote peer id=%d (%s) died (server health %u)", id, pe.name.c_str(),
                    (unsigned)pose.ped.health);
        }
        else if (!dead && pe.deadShown)
        {
            pe.deadShown = false;
            pe.killedAtMs = 0;
            pe.respawnPed = true;                      // a clean ped at its pose next frame
            ++g_v4.peerRecoveries;
            PeerLog(false, "engine: remote peer id=%d (%s) alive again: respawning its ped", id, pe.name.c_str());
        }
        return pe.deadShown;
    }

    // Hits WE dealt to this remote ped: its health below the pool, damaged by our ped or our vehicle.
    // The pool is restored every time; the damage goes to the server as a Hit (via g_hits). A ped the
    // game killed anyway (one blow above the pool) is respawned.
    void ApplyPuppetVehicleImpact(Peer& pe, int id, const sync::ImpactOut& io, int32_t lpVeh, double nowMs);   // below, with the hit reactions

    // W9 item 8 (hit rate on drivers): what the local player's shot was aimed at (IS_PLAYER_FREE_AIMING_AT_ENTITY's target),
    // counted per 10 s: a seated puppet, the vehicle a puppet sits in, a puppet on foot. With the hits reported on seated
    // puppets (ProbePeerHits) and the pool drops they took, the next live test shows whether seated puppets are hittable.
    void NoteShotTarget()
    {
        int32_t aimed = 0;
        if (!g_world->GetEntityLocalPlayerIsFreeAimingAt(aimed) || !aimed)
            return;
        for (const auto& kv : g_peers)                 // the game thread's own maps: no lock
            if (kv.second.ped == aimed)
            {
                if (kv.second.seatedVeh >= 0)
                    ++g_v4.shotsAtSeated;
                else
                    ++g_v4.shotsAtFoot;
                return;
            }
        for (const auto& kv : g_vehs)
            if (kv.second.handle == aimed)
            {
                for (const auto& pk : g_peers)
                    if (pk.second.seatedVeh == (int)kv.first)
                    {
                        ++g_v4.shotsAtPuppetCar;
                        return;
                    }
                return;
            }
    }

    void ProbePeerHits(Peer& pe, int id, int32_t lp, int32_t lpVeh, float lpVehSpeed, double nowMs)
    {
        if (!pe.hpPool || pe.deadShown || !lp)
            return;
        // W9 item 6: the local player's vehicle runs a puppet over: it ragdolls (a cosmetic reaction; the damage itself goes
        // through the pool probe below). The damage query is asked only for a vehicle above 5 m/s with the puppet within 6 m.
        if (lpVeh && lpVehSpeed >= sync::kImpactMinSpeedMps && pe.seatedVeh < 0 && nowMs - pe.lastImpactMs >= sync::kImpactRepeatMs)
        {
            float px, py, pz, vx, vy, vz;
            if (g_world->GetEntityCoords(pe.ped, px, py, pz) && g_world->GetEntityCoords(lpVeh, vx, vy, vz) &&
                (px - vx) * (px - vx) + (py - vy) * (py - vy) + (pz - vz) * (pz - vz) <= sync::kImpactReachM * sync::kImpactReachM)
            {
                sync::ImpactIn ii;
                ii.vehSpeedMps = lpVehSpeed;
                ii.damagedByVehicle = g_world->DamagedBy(pe.ped, lpVeh);
                ii.puppetSeated = false;
                ii.puppetDead = false;
                ii.sinceLastImpactMs = nowMs - pe.lastImpactMs;
                const sync::ImpactOut io = sync::DecideVehicleImpact(ii);
                if (io.ragdoll)
                    ApplyPuppetVehicleImpact(pe, id, io, lpVeh, nowMs);
            }
        }
        const int h = g_world->GetEntityHealth(pe.ped);
        if (pe.seatedVeh >= 0 && h < pe.hpPool)
            ++g_v4.seatedHpDrops;                      // W9 item 8: a seated puppet lost pool health (whoever caused it): it is hittable
        // 0c: a melee blow is read from the damage EVENT as well, in the window of our own swing only (a melee-class weapon
        // in hand and a swing counted or a melee action running: sync::MeleeEventWindow keeps the native call out of every
        // other frame). The pool alone cannot see a fist that took nothing off it (the AI melee modifier at 0 did exactly
        // that in 0.4.3), and the early return below must not hide the event.
        const double sinceSwing = nowMs - g_lastSwingMs;
        const bool eventWindow = sync::MeleeEventWindow(g_meleeHeld, sinceSwing, g_meleeActionNow);
        bool eventDamaged = false;
        // 0.4.5: the first frame a window opens on this puppet, the standing event (if any) is a STALE one (a blow that landed
        // outside every window and was never cleared): it is read for the log and cleared, never reported. Without it the
        // first frame of the next swing, a miss included, reported the old blow as a hit.
        switch (pe.meleeEvents.Tick(eventWindow))
        {
        case sync::MeleeEventGate::Step::ClearStale:
        {
            ++g_v4.meleeWindows;
            float px, py, pz;
            if (g_world->GetEntityCoords(pe.ped, px, py, pz) &&
                (px - g_localX.load()) * (px - g_localX.load()) + (py - g_localY.load()) * (py - g_localY.load()) <= 9.0f)
                ++g_v4.meleeNearSwings;            // a puppet within 3 m when a swing began: a blow could have landed
            if (g_world->DamagedBy(pe.ped, lp))
            {
                ++g_v4.meleeStaleEvents;
                g_world->ClearLastDamage(pe.ped);
                g_world->ClearLastDamageBone(pe.ped);
            }
            break;
        }
        case sync::MeleeEventGate::Step::Read:
            ++g_v4.meleeEventsRead;
            eventDamaged = g_world->DamagedBy(pe.ped, lp);
            break;
        case sync::MeleeEventGate::Step::Closed:
            break;
        }
        if (h == pe.hpPool && !eventDamaged)
            return;
        // A puppet the game killed in one blow above the pool (a headshot would, but critical hits are
        // off for puppets; a rocket or a ram can) is still ours to report when we did it.
        const bool dead = g_world->IsPedDeadOrDying(pe.ped);
        // Our car's hits are ours only from the driver's seat as the server sees it: a passenger's car
        // damage is the driver's.
        const bool blameVeh = lpVeh && sync::VehicleBlameAllowed(g_localSeat, g_link.Driver());
        bool byVeh = false, byMe = false;
        if ((h > 0 && h < pe.hpPool) || dead)
        {
            byMe = g_world->DamagedBy(pe.ped, lp);
            if (!byMe && blameVeh)
                byMe = byVeh = g_world->DamagedBy(pe.ped, lpVeh);
        }
        // W8 item 7 (live test 0.4.2, "a punch while running one-shots and kills"): a puppet GTA kills with a melee blow
        // (a running punch, a takedown) has its health set to 0, not lowered by a damage number, so the pool-minus-health
        // rule reported ~the whole pool. A melee-class weapon in our hand caps a dead puppet's report (the server clamps
        // again by weapon); a gun or a car keeps the pool.
        const uint32_t myWeapon = byMe && !byVeh ? g_world->GetSelectedPedWeapon(lp) : 0;
        const int deadMax = (dead && myWeapon && IsMeleeClassWeapon(myWeapon)) ? sync::kDeadMeleeCapDamage : sync::kNoDeadCap;
        const bool takedown = dead && byMe && g_world->WasPedKilledByTakedown(pe.ped);   // diagnostic only
        sync::PeerDamageOut o = sync::ProbePeerDamage(h, pe.hpPool, byMe, dead, deadMax);
        if (deadMax != sync::kNoDeadCap && o.report && pe.hpPool - (h > 0 ? h : 0) > deadMax)
            ++g_v4.deadCapped;
        if (takedown)
            ++g_v4.takedownKills;
        // 0c: the melee decision (sync::DecideMeleeHit): the damage event in the swing window is the hit, at the weapon's
        // nominal damage (the server's melee ceiling decides what it is worth); it replaces the pool number for that blow.
        // A pool drop with no event keeps reporting as before (guns, a melee drop outside the window).
        sync::MeleeHitIn mi;
        mi.meleeWeaponHeld = g_meleeHeld;
        mi.eventDamaged = eventDamaged;
        mi.sinceSwingMs = sinceSwing;
        mi.meleeActionNow = g_meleeActionNow;
        mi.poolReport = o.report;
        mi.poolDamage = o.damage;
        const sync::MeleeHitOut md = sync::DecideMeleeHit(mi);
        o.report = md.source != sync::MeleeHitSource::None;
        if (o.report)
            o.damage = md.damage;
        if (md.source == sync::MeleeHitSource::Event)
        {
            ++g_v4.meleeHitsEvent;
            g_v4.meleeEventDamage += (uint32_t)md.damage;
        }
        else if (md.source == sync::MeleeHitSource::Pool && g_meleeHeld)
        {
            ++g_v4.meleeHitsPool;
            g_v4.meleePoolDamage += (uint32_t)md.damage;
        }
        if (o.report && pe.seatedVeh >= 0)
            ++g_v4.hitsOnSeated;                       // W9 item 8: a Hit reported on a puppet that sits in a vehicle
        if (o.report)
        {
            int bone = -1;
            g_world->LastDamageBone(pe.ped, bone);
            static uint32_t runOver = 0;
            if (!runOver)
                runOver = g_world->HashKey("WEAPON_RUN_OVER_BY_CAR");
            sync::BatchedHit hit;
            hit.target = (uint16_t)id;
            hit.kind = (uint8_t)proto::HitKind::Player;
            hit.weapon = byVeh ? runOver : g_world->GetSelectedPedWeapon(lp);
            hit.damage = (uint16_t)(o.damage > 65535 ? 65535 : o.damage);
            hit.bone = (int16_t)(bone >= -32768 && bone <= 32767 ? bone : -1);
            hit.shotSeq = (uint8_t)(g_shotSeq & 15);
            g_hits.Add(hit, nowMs);
            static int loggedMeleeHits = 0;
            if (g_meleeHeld && loggedMeleeHits < 12)    // 0.4.5: the first melee reports with their timing, to read the server's answer against
            {
                ++loggedMeleeHits;
                PeerLog(false, "engine: melee hit on peer id=%d (%s): %s, claimed %u (weapon 0x%08x), %.0f ms after our last counted swing, "
                        "shotSeq %u, puppet health %d of pool %d, action running %d", id, pe.name.c_str(),
                        md.source == sync::MeleeHitSource::Event ? "damage EVENT" : "pool drop", (unsigned)hit.damage, (unsigned)hit.weapon,
                        sinceSwing, (unsigned)hit.shotSeq, h, pe.hpPool, g_meleeActionNow ? 1 : 0);
            }
        }
        if (o.damage > 0 || h <= 0 || md.clearEvent)   // md.clearEvent: a melee event is read once, never fires the next swing too
        {
            g_world->ClearLastDamage(pe.ped);
            g_world->ClearLastDamageBone(pe.ped);
        }
        if (o.restore)
            g_world->SetEntityHealth(pe.ped, pe.hpPool);
        if (o.respawn)
        {
            pe.respawnPed = true;                      // killed on this machine only: the server decides
            ++g_v4.peerLocalDeaths;
            PeerLog(true, "engine: remote peer id=%d (%s) died locally (one blow above the %d pool%s%s): %s%s, respawning",
                    id, pe.name.c_str(), pe.hpPool, byMe ? ", ours" : "",
                    takedown ? ", a TAKEDOWN (WAS_PED_KILLED_BY_TAKEDOWN)" : "",
                    o.report ? "reported as a hit of " : "not reported", o.report ? std::to_string(o.damage).c_str() : "");
        }
    }

    // ---- sync plan W3, combat 2: cosmetic hit reactions -------------------------------------------------------
    // Model: shooter-reported hits, server-validated, the server owns health and armour. A reaction only moves a
    // ped: it never feeds a Hit and never changes server-owned health. The decision is sync::HitReact::Decide, the
    // window in which a reaction owns a puppet is sync::HitReactArbiter (DrivePeer holds the gait and aim over it).
    // The tunables are INFERRED; hit_react=off|bullet|ragdoll|both selects what is shown.

    constexpr float kKnockSpeedMps = 3.0f, kKnockUpMps = 1.0f;     // INFERRED: a small push away from the attacker
    constexpr float kStaggerPushScale = 0.5f;                      // W8: a melee stagger pushes half as far as a knockdown

    // The horizontal unit vector pointing from (fx, fy) to (tx, ty); false when they (nearly) coincide.
    bool AwayFrom(float fx, float fy, float tx, float ty, float& dx, float& dy)
    {
        dx = tx - fx;
        dy = ty - fy;
        const float l = std::sqrt(dx * dx + dy * dy);
        if (l < 0.05f)
            return false;
        dx /= l;
        dy /= l;
        return true;
    }

    // The weapon a flinch bullet is shot with: the attacker's when it is a bullet weapon (its blood and impact),
    // else a pistol. Never an explosive or thrown one.
    uint32_t FlinchWeapon(uint32_t preferred)
    {
        return preferred && IsGunWeapon(preferred) ? preferred : sync::Joaat("WEAPON_PISTOL");
    }

    // A flinch: ONE bullet of damage 1 at `victim`'s chest (SKEL_Spine2, else 0.2 m above its origin), started just
    // outside the body on the side `from` (the attacker's shown position) lies, so it crosses no third party and
    // never reaches the shooter. The game then plays its own hit reaction, blood and impact (INFERRED live).
    // `owner` is the bullet's owner ped: the attacker's puppet when the victim is us (the vitals tracker then reads
    // the 1-HP loss as a puppet loss and puts the server's value back), 0 when the victim is a puppet (a bullet
    // owned by our own ped would be reported as a Hit by ProbePeerHits).
    bool FlinchBullet(int32_t victim, float fromX, float fromY, float fromZ, int32_t owner, uint32_t weapon)
    {
        float tx, ty, tz;
        const int bone = g_world->GetEntityBoneIndexByName(victim, "SKEL_Spine2");
        if (bone < 0 || !g_world->GetWorldPositionOfEntityBone(victim, bone, tx, ty, tz))
        {
            if (!g_world->GetEntityCoords(victim, tx, ty, tz))
                return false;
            tz += 0.2f;
        }
        float dx = fromX - tx, dy = fromY - ty, dz = fromZ - tz;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist < 0.05f)
        {
            dx = 0.0f;
            dy = -1.0f;
            dz = 0.0f;
            dist = 1.0f;
        }
        const float back = std::min(0.7f, dist * 0.5f);
        const float sx = tx + dx / dist * back, sy = ty + dy / dist * back, sz = tz + dz / dist * back;
        return g_world->ShootBulletBetween(sx, sy, sz, tx, ty, tz, 1, weapon, owner, false, false, -1.0f);
    }

    // What hit_react lets a decision show is sync::ReactionToShow (libs/sync/combat.h, pure and tested): a knockdown or a
    // melee stagger ("ragdoll"/"both") as a ragdoll, a flinch bullet ("bullet"/"both"; in "bullet" a knockdown or a
    // stagger is shown as a flinch). Neither = nothing is shown.
    const char* ReactName(sync::ReactKind k)
    {
        switch (k)
        {
        case sync::ReactKind::None:      return "none";
        case sync::ReactKind::Flinch:    return "flinch bullet";
        case sync::ReactKind::Knockdown: return "knockdown";
        case sync::ReactKind::Stagger:   return "melee stagger";
        }
        return "?";
    }

    // Where a puppet's attacker is on our screen (sync plan W6): us (our own Hit confirmed), or another remote
    // player's shown ped (protocol v5's onlooker Damage). `weapon` is the weapon it holds (the flinch bullet's
    // look). False when the attacker is not known here (no ped yet, or none: a server hit).
    bool PuppetAttackerAt(uint16_t attacker, float& ax, float& ay, float& az, uint32_t& weapon)
    {
        if (attacker == join::MyId())
        {
            ax = g_localX.load();
            ay = g_localY.load();
            az = g_localZ.load();
            weapon = g_world->GetSelectedPedWeapon(g_localPed.load());
            return true;
        }
        const auto it = g_peers.find(attacker);          // the game thread's own map: no lock
        if (it == g_peers.end() || it->second.gone || !it->second.ped)
            return false;
        weapon = it->second.lastPed.weapon;
        return g_world->GetEntityCoords(it->second.ped, ax, ay, az);
    }

    // The reaction on a remote puppet, on our screen: its guards lifted (PCF 188 DisableHurt, the ragdoll block)
    // for the window only, then a knockdown (SET_PED_TO_RAGDOLL and a small push away from the attacker, when
    // known) or a flinch bullet (from the attacker's position; with no attacker known there is nothing to shoot
    // from, so only a knockdown shows). Script tick, no lock, pe.ped != 0. Called by TickPeerHitReact for the
    // Damage the server confirmed on a Hit of ours and for the onlooker Damage of protocol v5.
    // 0.4.30 stagger_clip: the stumble clip of a melee stagger. INFERRED names: DOES_ANIM_DICT_EXIST decides, the trace reads the clip back.
    constexpr const char* kStaggerDict = "reaction@shove";
    constexpr const char* kStaggerClip = "shoved_back";
    // 0 = playing, 1 = no such dictionary, 2 = still streaming (requested now), 3 = the call failed. Counted when not 0.
    int PlayStaggerClip(int32_t ped, const char* who)
    {
        int rc = 0;
        if (g_world->AnimDictExists(kStaggerDict) == 0)
            rc = 1;
        else if (!g_world->HasAnimDictLoaded(kStaggerDict))
        {
            g_world->RequestAnimDict(kStaggerDict);
            rc = 2;
        }
        else if (!g_world->TaskPlayAnim(ped, kStaggerDict, kStaggerClip, 8.0f, -8.0f, -1, 0, AnimPhase()))
            rc = 3;
        if (rc == 1) ++g_v4.staggerNoDict;
        else if (rc == 2) ++g_v4.staggerStreaming;
        else if (rc == 3) ++g_v4.staggerFailed;
        else
            BeginTrace(TraceProbe::Anim, ped, std::string(who) + " hit reaction 'melee stagger' clip", kStaggerDict, kStaggerClip);
        return rc;
    }

    void ApplyPuppetHitReact(Peer& pe, int peerId, const sync::HitReaction& r, double nowMs, bool haveAttacker, float ax,
                             float ay, float az, uint32_t attackerWeapon)
    {
        bool ragdoll = false, bullet = false, clip = false;
        sync::ReactionToShow(g_hitReact.load(std::memory_order_relaxed), r, ragdoll, bullet, clip, g_staggerClip.load(std::memory_order_relaxed));
        if (bullet && !haveAttacker)
            bullet = false;
        if (!pe.ped || (!ragdoll && !bullet && !clip))
            return;
        const int32_t ped = pe.ped;
        if (clip)
        {
            // on its feet: PCF 188 and the ragdoll block stay as they are, so GTA's own fist cannot ragdoll it either
            if (PlayStaggerClip(ped, "puppet") != 0)
                return;                                // nothing shown rather than the ragdoll GI complained about (counted)
            pe.hitReact.Start(nowMs, r);
            pe.reactClip = true;                       // the window's end re-issues the gait the clip replaced
            ++g_v4.puppetStagger;
            return;
        }
        g_world->SetPedConfigFlag(ped, 188, false);
        pe.reactLifted = true;
        sync::HitReaction shown;
        if (ragdoll)
        {
            g_world->SetPedCanRagdoll(ped, true);
            g_world->SetPedToRagdoll(ped, r.ragdollMs, r.ragdollMs);
            BeginTrace(TraceProbe::Ragdoll, ped, std::string("puppet hit reaction '") + ReactName(r.kind) + "', SET_PED_TO_RAGDOLL " +
                                                     std::to_string((int)r.ragdollMs) + " ms");
            float px, py, pz, dx, dy;
            const float push = r.kind == sync::ReactKind::Stagger ? kStaggerPushScale : 1.0f;
            if (haveAttacker && g_world->GetEntityCoords(ped, px, py, pz) && AwayFrom(ax, ay, px, py, dx, dy))
                g_world->SetEntityVelocity(ped, dx * kKnockSpeedMps * push, dy * kKnockSpeedMps * push, kKnockUpMps * push);
            pe.reactRagdolled = true;
            shown = r;
            if (r.kind == sync::ReactKind::Stagger)
                ++g_v4.puppetStagger;
            else
                ++g_v4.puppetKnock;
        }
        else
        {
            FlinchBullet(ped, ax, ay, az + 0.4f, 0, FlinchWeapon(attackerWeapon));
            shown.kind = sync::ReactKind::Flinch;
            ++g_v4.puppetFlinch;
        }
        pe.hitReact.Start(nowMs, shown);
        static int logged = 0;
        if (logged < 8)
        {
            ++logged;
            PeerLog(false, "engine: hit reaction on peer id=%d (%s): %s%s (hit_react mode %d; look and feel INFERRED)",
                    peerId, pe.name.c_str(), ReactName(shown.kind),
                    shown.ragdollMs > 0 ? (std::string(" ") + std::to_string(shown.ragdollMs) + " ms").c_str() : "",
                    g_hitReact.load());
        }
    }

    // The window ended (or was cancelled): the puppet's guards are put back and its gait is re-issued. A knockdown
    // also clears the ped's tasks so it stands up (clearTasks false while seated: that would pull it out of the car).
    void EndPuppetHitReact(Peer& pe, bool clearTasks)
    {
        // W8 item 3: only a knockdown / stagger that cleared the task list needs the gait task issued again; a flinch bullet
        // (or a reaction cut short while seated) left it running, so the controller only re-evaluates (SoftReset).
        const bool cleared = pe.reactLifted && pe.ped && pe.reactRagdolled && clearTasks;
        if (pe.reactLifted && pe.ped)
        {
            if (pe.reactRagdolled && clearTasks)
                ClearPedTasksNow(pe.ped);
            g_world->SetPedCanRagdoll(pe.ped, false);
            g_world->SetPedConfigFlag(pe.ped, 188, true);
        }
        const bool clip = pe.reactClip && pe.ped;
        pe.reactLifted = false;
        pe.reactRagdolled = false;
        pe.reactClip = false;
        pe.aimTask = false;
        if (cleared || clip)
            LocoReset(pe, kSiteHitReact);              // the controller re-issues its gait (a real drift still snaps)
        else
            LocoSoft(pe, kSiteHitReact);
    }

    // W9 item 6: a puppet the local player's vehicle ran over ragdolls for io.ragdollMs and is thrown with a share of the
    // vehicle's velocity. The same cosmetic window as a server-confirmed knockdown (ApplyPuppetHitReact): its guards are
    // lifted, the hit-reaction arbiter holds the ped (no gait, no position correction) until the window ends, then the
    // guards go back and the locomotion re-issues. The sender (the owner) stays authoritative: its own ragdoll arrives
    // through the Tier B edge and its next states put the puppet back. Obeys hit_react (off / bullet: no ragdoll).
    void ApplyPuppetVehicleImpact(Peer& pe, int id, const sync::ImpactOut& io, int32_t lpVeh, double nowMs)
    {
        sync::HitReaction r;
        r.kind = sync::ReactKind::Knockdown;
        r.ragdollMs = io.ragdollMs;
        bool ragdoll = false, bullet = false;
        sync::ReactionToShow(g_hitReact.load(std::memory_order_relaxed), r, ragdoll, bullet);
        if (!ragdoll || !pe.ped)
            return;
        const int32_t ped = pe.ped;
        g_world->SetPedConfigFlag(ped, 188, false);
        pe.reactLifted = true;
        g_world->SetPedCanRagdoll(ped, true);
        g_world->SetPedToRagdoll(ped, io.ragdollMs, io.ragdollMs);
        float vx = 0, vy = 0, vz = 0;
        if (g_world->GetEntityVelocity(lpVeh, vx, vy, vz))
            g_world->SetEntityVelocity(ped, vx * io.velocityScale, vy * io.velocityScale, vz * io.velocityScale + 2.0f);
        pe.reactRagdolled = true;
        pe.aimTask = false;
        pe.hitReact.Start(nowMs, r);
        pe.lastImpactMs = nowMs;
        ++g_v4.vehImpacts;
        static int logged = 0;
        if (logged < 8)
        {
            ++logged;
            PeerLog(false, "engine: vehicle impact on peer id=%d (%s): our vehicle at %.1f m/s: ragdoll %d ms, thrown with %.0f%% of its "
                    "velocity (look and feel INFERRED)", id, pe.name.c_str(), std::sqrt((double)vx * vx + (double)vy * vy + (double)vz * vz),
                    io.ragdollMs, io.velocityScale * 100.0);
        }
    }

    // Per frame in DrivePeer: turn the Damage the join worker queued for this puppet into a reaction, run the
    // arbiter. True while a reaction owns the ped (the caller issues no gait / aim task over it).
    bool TickPeerHitReact(Peer& pe, int id, const sync::Pose& pose, double nowMs)
    {
        const proto::PedState& ps = pose.ped;
        if (pe.deadShown)
        {
            // The death display owns the ped (it has enabled the ragdoll itself): nothing to put back.
            pe.hitReact.Clear();
            pe.reactLifted = pe.reactRagdolled = pe.reactClip = false;
            pe.reactDamage = 0;
            pe.reactFlags = 0;
            return false;
        }
        const bool seated = pe.seatedVeh >= 0 || ps.move == proto::kMoveInVehicle;
        if (pe.reactDamage)
        {
            const uint32_t dmg = pe.reactDamage;
            const uint8_t flags = (uint8_t)(pe.reactFlags & (sync::HitReact::kFlagHeadshot | sync::HitReact::kFlagMelee |
                                                              sync::HitReact::kFlagExplosion | sync::HitReact::kFlagArmourAbsorbed));
            const uint16_t attacker = pe.reactAttacker;
            pe.reactDamage = 0;
            pe.reactFlags = 0;
            if (g_hitReact.load(std::memory_order_relaxed) != kReactOff)
            {
                sync::HitReactState st;
                st.inVehicle = seated;
                st.climbing = pe.collisionOff || (ps.flags & (proto::kPfClimbing | proto::kPfVaulting)) != 0;
                st.sinceKnockdownMs = pe.hitReact.SinceKnockdownMs(nowMs);
                // The Damage.flags of protocol v5 (an explosion or a melee blow knocks down at a lower percentage).
                const sync::HitReaction r = sync::HitReact::Decide((int)(dmg > 100000u ? 100000u : dmg),
                                                                   ServerMaxHp(g_localPed.load()), flags,
                                                                   pe.hitReact.SinceLastMs(nowMs), st);
                if (r.kind != sync::ReactKind::None)
                {
                    float ax = 0, ay = 0, az = 0;
                    uint32_t attackerWeapon = 0;
                    const bool haveAttacker = PuppetAttackerAt(attacker, ax, ay, az, attackerWeapon);
                    ApplyPuppetHitReact(pe, id, r, nowMs, haveAttacker, ax, ay, az, attackerWeapon);
                }
            }
        }
        if (seated && pe.hitReact.Active())
        {
            pe.hitReact.Clear();                       // it got into a car mid-reaction
            EndPuppetHitReact(pe, false);
            return false;
        }
        const sync::HitReactOut o = pe.hitReact.Tick(nowMs);
        if (o.ended)
            EndPuppetHitReact(pe, true);
        return o.holds;
    }

    // The victim's own reaction (the local ped), at the Damage drain: sync::HitReact::Decide on the damage the
    // others dealt, then a knockdown (SET_PED_TO_RAGDOLL, a small push away from the attacker's shown puppet
    // when known) or a flinch bullet from the attacker's puppet. Never for a killing blow (the death plays).
    // The flinch bullet is skipped when it could take the last health. Script tick.
    void ReactLocalHit(const sync::DamageSlot& ds, double nowMs)
    {
        const int mode = g_hitReact.load(std::memory_order_relaxed);
        if (mode == kReactOff || g_life != Life::Alive || ds.killed || ds.otherEvents == 0 || ds.otherDamage == 0)
            return;
        const int32_t lp = g_world->LocalPlayer();
        if (!lp)
            return;
        sync::HitReactState st;
        st.dead = g_world->IsPedDeadOrDying(lp);
        st.inVehicle = g_world->IsPedInVehicle(lp);
        st.climbing = g_world->IsPedClimbing(lp) || g_world->IsPedVaulting(lp);
        st.sinceKnockdownMs = g_localReact.SinceKnockdownMs(nowMs);
        // Damage.flags of protocol v5, folded over every hit since the last drain (the bone, ds.lastBone, only
        // names where it landed: the flags already carry what the reaction depends on).
        const sync::HitReaction r = sync::HitReact::Decide((int)(ds.otherDamage > 100000u ? 100000u : ds.otherDamage),
                                                           ServerMaxHp(lp), ds.otherFlags, g_localReact.SinceLastMs(nowMs), st);
        bool ragdoll = false, bullet = false, clip = false;
        sync::ReactionToShow(mode, r, ragdoll, bullet, clip, g_staggerClip.load(std::memory_order_relaxed));
        if (!ragdoll && !bullet && !clip)
            return;
        if (clip)
        {
            // 0.4.30: a stumble on our feet, no SetPedCanRagdoll and no push, so we send no ragdoll flag either (no mirrored fall on the attacker's screen)
            if (PlayStaggerClip(lp, "local") != 0)
                return;
            g_localReact.Start(nowMs, r);
            ++g_v4.reactStagger;
            return;
        }
        int32_t attPed = 0;
        float qx = 0, qy = 0, qz = 0;
        {
            const auto it = g_peers.find(ds.lastAttacker);   // the game thread's own map: no lock
            if (it != g_peers.end() && !it->second.gone && it->second.ped &&
                g_world->GetEntityCoords(it->second.ped, qx, qy, qz))
                attPed = it->second.ped;
        }
        sync::HitReaction shown;
        if (ragdoll)
        {
            g_world->SetPedCanRagdoll(lp, true);
            g_world->SetPedToRagdoll(lp, r.ragdollMs, r.ragdollMs);
            float px, py, pz, dx, dy;
            const float push = r.kind == sync::ReactKind::Stagger ? kStaggerPushScale : 1.0f;
            if (attPed && g_world->GetEntityCoords(lp, px, py, pz) && AwayFrom(qx, qy, px, py, dx, dy))
                g_world->SetEntityVelocity(lp, dx * kKnockSpeedMps * push, dy * kKnockSpeedMps * push, kKnockUpMps * push);
            shown = r;
            if (r.kind == sync::ReactKind::Stagger)
                ++g_v4.reactStagger;
            else
                ++g_v4.reactKnock;
        }
        else
        {
            if (!attPed || g_vitals.Health() <= 3)
                return;                                // no puppet to shoot it, or 1 HP could be the last
            if (!FlinchBullet(lp, qx, qy, qz + 0.4f, attPed, FlinchWeapon(ds.lastWeapon)))
                return;
            g_vitals.NotePuppetLoss(nowMs);            // the bullet costs 1 HP: put the server's value back unless a Damage comes
            shown.kind = sync::ReactKind::Flinch;
            ++g_v4.reactFlinch;
        }
        g_localReact.Start(nowMs, shown);
        static int logged = 0;
        if (logged < 8)
        {
            ++logged;
            GLOGI("engine: hit reaction on us: %s for %u damage (flags 0x%x, max health %d, hit_react mode %d; look and feel INFERRED)",
                  ReactName(shown.kind), (unsigned)ds.otherDamage, (unsigned)ds.otherFlags, ServerMaxHp(lp), mode);
        }
    }

    // The emote on this ped: start what the join worker stored, then one arbiter step. True when the
    // gait must not run this frame (the caller has already placed the ped if it drifted).
    bool TickPeerEmote(Peer& pe, int id, const sync::Pose& pose, double nowMs)
    {
        SyncPeerEmoteDict(pe);                         // after a Clear() elsewhere (a death shown): free the dictionary
        if (pe.emoteSeq != pe.emoteSeen)
        {
            pe.emoteSeen = pe.emoteSeq;
            if (pe.emoteKind == 1)
            {
                const proto::Anim& a = pe.emoteIn;
                if (a.durationMs >= 0 && nowMs - pe.emoteAtMs > 2000.0)
                    ;                                  // a one-shot that waited for the ped: stale
                else if (g_world->AnimDictExists(a.dict.c_str()) == 0)
                {
                    ++g_v4.emoteMissingDict;
                    ++g_animDiag.Of(sync::AnimKind::PeerEmote).requested;
                    ++g_animDiag.Of(sync::AnimKind::PeerEmote).dictMissing;
                    PeerLog(true, "engine: remote peer id=%d anim %s/%s: no such dictionary in this game", id,
                            a.dict.c_str(), a.name.c_str());
                }
                else
                {
                    pe.emoteAnim = a;
                    ++g_animDiag.Of(sync::AnimKind::PeerEmote).requested;
                    if (pe.emote.StartAnim(nowMs, a.durationMs, a.flags))
                        ++g_animDiag.Of(sync::AnimKind::PeerEmote).restarted;
                    SyncPeerEmoteDict(pe);             // counted before any ending emote can free the dictionary
                }
            }
            else if (pe.emoteKind == 2)
            {
                pe.emoteScenario = pe.scenarioIn;
                ++g_animDiag.Of(sync::AnimKind::PeerEmote).requested;
                if (pe.emote.StartScenario(nowMs))
                    ++g_animDiag.Of(sync::AnimKind::PeerEmote).restarted;
            }
            else
                pe.emote.Stop();
        }
        if (!pe.emote.Active())
        {
            SyncPeerEmoteDict(pe);                     // a scenario replaced an animation, or a Stop with nothing playing
            return false;
        }

        sync::EmoteIn in;
        in.nowMs = nowMs;
        const bool anim = pe.emote.Kind() == sync::EmoteKind::Anim;
        if (pe.emote.WaitingForDict())
            in.dictLoaded = g_world->HasAnimDictLoaded(pe.emoteAnim.dict.c_str());
        if (pe.emote.Issued())
            in.playing = anim ? g_world->IsEntityPlayingAnim(pe.ped, pe.emoteAnim.dict.c_str(), pe.emoteAnim.name.c_str())
                              : g_world->IsPedUsingAnyScenario(pe.ped);
        in.dead = pe.deadShown;
        in.inVehicle = pe.seatedVeh >= 0 || pose.ped.move == proto::kMoveInVehicle;
        float px, py, pz;
        if (g_world->GetEntityCoords(pe.ped, px, py, pz))
        {
            const float dx = px - pose.k.x, dy = py - pose.k.y, dz = pz - pose.k.z;
            in.driftM = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        const sync::EmoteStats before = pe.emote.Stats();
        const sync::EmoteOut o = pe.emote.Tick(in);
        if (o.requestDict)
            g_world->RequestAnimDict(pe.emoteAnim.dict.c_str());
        sync::AnimKindStats& dk = g_animDiag.Of(sync::AnimKind::PeerEmote);
        if (o.issueAnim)
        {
            const proto::Anim& a = pe.emoteAnim;
            // 0e: the legs are taken (gait cleared) for a full-body anim or when an earlier emote's task may still be on
            // the ped; an upper-body / secondary anim runs over the gait task and leaves it alone.
            if (o.clearFirst)
                ClearPedTasksNow(pe.ped);
            const bool called = g_world->TaskPlayAnim(pe.ped, a.dict.c_str(), a.name.c_str(), a.blendIn, a.blendOut,
                                                      sync::EmoteArbiter::GtaDuration(a.durationMs),
                                                      (int)sync::EmoteArbiter::GtaFlags(a.durationMs, a.flags), EmotePhase(a));   // 0.4.30: the start phase (the wire rate has no setter here)
            if (o.resetLoco)
                LocoReset(pe, kSiteEmoteEnd);          // the gait was cleared under an upper-body anim: the controller re-issues it
            pe.aimTask = false;
            ++g_v4.emotesStarted;
            ++(called ? dk.issued : dk.failed);
            if (called)
            {
                char tg[200];
                std::snprintf(tg, sizeof(tg), "peer %s emote %s/%s, TASK_PLAY_ANIM flags %u, duration %d ms", pe.name.c_str(), a.dict.c_str(), a.name.c_str(),
                              (unsigned)sync::EmoteArbiter::GtaFlags(a.durationMs, a.flags), (int)sync::EmoteArbiter::GtaDuration(a.durationMs));
                BeginTrace(TraceProbe::Anim, pe.ped, tg, a.dict.c_str(), a.name.c_str());
            }
            NoteDictReady(sync::AnimKind::PeerEmote, o.waitedMs);
            if (g_animFirstUse.Note("emote:" + a.dict + "/" + a.name))
                PeerLog(!called, "engine: anim first use: remote peer id=%d emote %s / %s: DOES_ANIM_DICT_EXIST yes, HAS_ANIM_DICT_LOADED after %.0f ms, "
                        "TASK_PLAY_ANIM %s (flags %u, duration %d ms); IS_ENTITY_PLAYING_ANIM is checked from the next frame", id,
                        a.dict.c_str(), a.name.c_str(), o.waitedMs, called ? "called" : "could NOT be called",
                        (unsigned)sync::EmoteArbiter::GtaFlags(a.durationMs, a.flags),
                        (int)sync::EmoteArbiter::GtaDuration(a.durationMs));
        }
        if (o.issueScenario)
        {
            ClearPedTasksNow(pe.ped);
            g_world->TaskStartScenarioInPlace(pe.ped, pe.emoteScenario.c_str());
            pe.aimTask = false;
            ++g_v4.emotesStarted;
            ++dk.issued;
        }
        if (o.clearTasks)
            g_world->ClearPedTasks(pe.ped);
        if (o.confirmed)
        {
            ++dk.shown;
            if (g_animFirstUse.Note("emote-seen:" + pe.emoteAnim.dict + "/" + pe.emoteAnim.name))
                PeerLog(false, "engine: anim first use: remote peer id=%d emote %s / %s is SEEN playing (IS_ENTITY_PLAYING_ANIM true) %.0f ms after "
                        "the task", id, pe.emoteAnim.dict.c_str(), pe.emoteAnim.name.c_str(), o.confirmMs);
        }
        if (o.reissued)
            ++dk.reissued;
        if (o.neverPlayed)
        {
            ++dk.unseen;
            if (g_animFirstUse.Note("emote-unseen:" + pe.emoteAnim.dict + "/" + pe.emoteAnim.name))
                PeerLog(true, "engine: anim first use: remote peer id=%d emote %s / %s was issued but the ped never played it "
                        "(IS_ENTITY_PLAYING_ANIM stayed false): the clip name is probably wrong, or the ped could not play it", id,
                        pe.emoteAnim.dict.c_str(), pe.emoteAnim.name.c_str());
        }
        if (o.gaveUp)
        {
            ++g_v4.emoteDictTimeouts;
            ++dk.dictTimeout;
            PeerLog(true, "engine: remote peer id=%d anim %s/%s: dictionary did not load in 3 s", id,
                    pe.emoteAnim.dict.c_str(), pe.emoteAnim.name.c_str());
        }
        if (pe.emote.Stats().notPlaying != before.notPlaying)
            ++g_v4.emoteNotPlaying;
        if (o.ended)
        {
            ++dk.ended[(size_t)o.endReason];
            if (o.endReason == sync::EmoteEnd::Dead || o.endReason == sync::EmoteEnd::Vehicle)
                ++dk.cancelled;
            SyncPeerEmoteDict(pe);                     // ended, given up (3 s) or cut short: free the dictionary
            LocoReset(pe, kSiteEmoteEnd);              // the controller re-issues its gait (no snap)
            pe.aimTask = false;
            return false;
        }
        if (o.place)
            PlacePeer(pe, pose);
        return o.locoPaused;
    }

    const char* GaitName(sync::Gait g)
    {
        switch (g)
        {
        case sync::Gait::Still:  return "still";
        case sync::Gait::Walk:   return "walk";
        case sync::Gait::Run:    return "run";
        case sync::Gait::Sprint: return "sprint";
        }
        return "?";
    }

    const char* LocoTaskName(sync::LocoTask t)
    {
        switch (t)
        {
        case sync::LocoTask::None:       return "none";
        case sync::LocoTask::StandStill: return "stand-still";
        case sync::LocoTask::GoStraight: return "go-straight";
        case sync::LocoTask::GoToAnchor: return "go-to-anchor";
        }
        return "?";
    }

    const char* ClimbPhaseName(sync::ClimbWindow::Phase p)
    {
        switch (p)
        {
        case sync::ClimbWindow::Phase::Idle:     return "idle";
        case sync::ClimbWindow::Phase::Pending:  return "pending";
        case sync::ClimbWindow::Phase::Active:   return "active";
        case sync::ClimbWindow::Phase::Fallback: return "fallback-placed";
        case sync::ClimbWindow::Phase::Settle:   return "settle";
        }
        return "?";
    }

    const char* LocoLevelName(sync::LocoLevel l)
    {
        switch (l)
        {
        case sync::LocoLevel::Full:     return "full (the configured mode: anchor task + nudge / straight task + velocity / blend velocity)";
        case sync::LocoLevel::Straight: return "straight task";
        case sync::LocoLevel::NoRate:   return "straight task, speed carried by the blend ratio";
        case sync::LocoLevel::Placed:   return "rigid follow of the pose";
        }
        return "?";
    }
    const char* LocoLevelWhyName(sync::LocoLevelWhy w)
    {
        switch (w)
        {
        case sync::LocoLevelWhy::None:            return "-";
        case sync::LocoLevelWhy::AnchorLost:      return "the go-to-entity task ran but the ped showed no gait";
        case sync::LocoLevelWhy::AnchorStuck:     return "the ped's own speed stayed far under the pose's, or it was snapped again and again";
        case sync::LocoLevelWhy::AnchorDragging:  return "the nudges had to carry more than a tenth of the distance: the task does not follow the anchor";
        case sync::LocoLevelWhy::RateIneffective: return "SET_PED_MOVE_RATE_OVERRIDE does not change the ped's speed";
        case sync::LocoLevelWhy::StraightStuck:   return "the straight task stalled again after a re-issue: nothing moves the ped";
        case sync::LocoLevelWhy::Recovered:       return "a fallback timer ran out: the better level is tried again";
        case sync::LocoLevelWhy::Shared:          return "another puppet's controller found the same failure";
        case sync::LocoLevelWhy::BlendFighting:   return "BLEND: the ped did other than the velocity written and the error stayed up (the task fights the write)";
        case sync::LocoLevelWhy::BlendNoTrack:    return "BLEND: the error stayed up with no sign of a fight (blocked, or snaps that did not hold)";
        }
        return "?";
    }
    uint32_t g_locoNudgeCalls = 0;
    uint32_t g_locoLevelLines = 0;      // T2: "engine: loco level" lines this session (rate limit, script tick)

    // "name n, name n" for the sites with a count, "none" without any.
    std::string SiteList(const sync::SiteTally<kLocoSites>& t)
    {
        std::string s;
        for (size_t i = 0; i < kLocoSites; ++i)
            if (t.Count(i))
            {
                if (!s.empty())
                    s += ", ";
                s += kLocoSiteName[i];
                s += " ";
                s += std::to_string(t.Count(i));
            }
        return s.empty() ? std::string("none") : s;
    }

    // W8 items 3 and 8: the 10 s line that says WHY the controller issued tasks (per trigger, with the Reset callers) and what
    // was last applied to the ped, so an alt-tab freeze or a re-issue storm can be read from gage.log. Deltas against pe.locoSeen,
    // which the caller refreshes after the line.
    // Sync A: one line per peer per 10 s with the numbers that say whether a puppet slides (libs/sync LocoDiag, the same code the
    // simulated-ped tests measured). skate = metres our velocity pushes moved the ped / metres it moved, while the pose moves;
    // ~0 means the stride carries it. idle slide = metres pushed while the ped is told to stand. "speed" = from the ped's positions
    // against the pose's. Tasks and the learned animation speeds come from the controller.
    const char* LocoModeName()
    {
        return g_locoBlend.load() ? "blend" : g_locoAnchor.load() ? "anchor" : "straight";
    }
    // R4 T1: the one-line verdict of the last 10 s about the blend, from READ-BACKS: frames the velocity the game reports (GET_ENTITY_VELOCITY) and the
    // ped's own displacement differed from the velocity written the frame before by more than 1 m/s. 'blend ok' | 'blend fighting the task: N frames
    // ...' | 'blend DEGRADED ...'. Not a blend: says which mode runs.
    std::string BlendVerdict(const Peer& pe, const sync::LocoDiagSummary& d)
    {
        char b[420];
        if (!g_locoBlend.load())
        {
            std::snprintf(b, sizeof(b), "mode %s (no blend verdict)", LocoModeName());
            return b;
        }
        const sync::LocoStats& a = pe.loco.Stats();
        const sync::LocoStats& o = pe.locoSeen;
        if (pe.loco.Level() == sync::LocoLevel::Placed)
        {
            std::snprintf(b, sizeof(b), "blend DEGRADED to the rigid follow of the pose (%u falls this session, %u in the last 10 s): the writes were not honoured, "
                          "followed by coordinate steps of at most the pose speed + 3 m/s instead",
                          (unsigned)a.blendDegrades, (unsigned)(a.blendDegrades - o.blendDegrades));
            return b;
        }
        const uint32_t cmp = d.fightCmpFrames;
        const uint32_t worst = d.fightVelFrames > d.fightPosFrames ? d.fightVelFrames : d.fightPosFrames;
        if (cmp >= 30 && worst * 4 >= cmp)
        {
            std::snprintf(b, sizeof(b), "blend fighting the task: %u frames the velocity read back differs from the write by > 1 m/s (of %u compared; by the ped's positions %u)",
                          (unsigned)d.fightVelFrames, (unsigned)cmp, (unsigned)d.fightPosFrames);
            return b;
        }
        std::snprintf(b, sizeof(b), "blend ok (%u of %u frames differ from the write by > 1 m/s by the read-back, %u by positions)", (unsigned)d.fightVelFrames, (unsigned)cmp,
                      (unsigned)d.fightPosFrames);
        return b;
    }
    std::string LocoSlideLine(int id, const Peer& pe)
    {
        const sync::LocoDiagSummary d = pe.slide.Summary();
        const sync::LocoStats& a = pe.loco.Stats();
        const sync::LocoStats& b = pe.locoSeen;
        char buf[900];
        std::snprintf(buf, sizeof(buf),
                      "engine: loco slide id=%d (%s): last 10 s %u frames, %u with the pose moving: skate %.1f%%, idle slide %.2f m, "
                      "speed from positions %.2f vs pose %.2f m/s, push mean %.2f max %.2f m/s on %u frames, pose-ped error mean %.2f "
                      "max %.2f m, ped-anchor distance mean %.2f max %.2f m (%u anchor frames), %u stop-starts (ped under 0.3 m/s for "
                      "200 ms while the pose runs), tasks %u go + %u stand-still, anchor stalls %u, %u settles, %u grace frames, %u "
                      "contaminated velocity reads; learned animation speed x walk %.2f run %.2f sprint %.2f, move rate %.2f",
                      id, pe.name.c_str(), d.frames, d.movingFrames, d.skate * 100.0, d.idleSlideM, d.derivedSpeedMs, d.poseSpeedMs,
                      d.meanPushMs, d.maxPushMs, d.pushFrames, d.meanErrM, d.maxErrM, d.meanAnchorM, d.maxAnchorM, d.anchorFrames,
                      d.stops, a.goStraight - b.goStraight, a.standStill - b.standStill, a.anchorFallbacks - b.anchorFallbacks,
                      a.settles - b.settles, a.graceFrames - b.graceFrames, a.contaminated - b.contaminated,
                      pe.loco.NominalScale(sync::Gait::Walk), pe.loco.NominalScale(sync::Gait::Run),
                      pe.loco.NominalScale(sync::Gait::Sprint), pe.loco.LastMoveRate());
        // T2: appended so the older fields keep their place. level = the degradation ladder (full | straight | blend-carried |
        // rigid); nudge = the coordinate writes that closed the position error (share = metres nudged / metres travelled while a
        // gait was shown); rate = SET_PED_MOVE_RATE_OVERRIDE as applied (min / mean / max, share of frames at a limit); own speed =
        // the ped's displacement net of our writes (the one measurement that needs no model of the game).
        std::string line = buf;
        const double dTravel = a.travelledM - b.travelledM, dNudge = a.nudgedM - b.nudgedM;
        char t2[700];
        std::snprintf(t2, sizeof(t2),
                      " | T2: level %d (%s), nudge %u frames %.2f m (mean %.2f max %.2f m/s, %.1f%% of the distance), move rate applied "
                      "min %.2f mean %.2f max %.2f (%.0f%% of %u frames at a limit), own speed %.2f vs pose %.2f m/s, level changes %u "
                      "(anchor-lost %u, stuck %u, dragging %u, rate-ineffective %u, straight-stuck %u, recovered %u, shared %u), push %s",
                      (int)pe.loco.Level(), LocoLevelName(pe.loco.Level()), d.nudgeFrames, d.nudgedM, d.meanNudgeMs, d.maxNudgeMs,
                      dTravel > 0.5 ? 100.0 * dNudge / dTravel : 0.0, d.rateMin, d.rateMean, d.rateMax, d.rateClampFrac * 100.0, d.rateFrames,
                      d.selfSpeedMs, d.poseSpeedMs, a.levelChanges - b.levelChanges,
                      a.levelWhy[(int)sync::LocoLevelWhy::AnchorLost] - b.levelWhy[(int)sync::LocoLevelWhy::AnchorLost],
                      a.levelWhy[(int)sync::LocoLevelWhy::AnchorStuck] - b.levelWhy[(int)sync::LocoLevelWhy::AnchorStuck],
                      a.levelWhy[(int)sync::LocoLevelWhy::AnchorDragging] - b.levelWhy[(int)sync::LocoLevelWhy::AnchorDragging],
                      a.levelWhy[(int)sync::LocoLevelWhy::RateIneffective] - b.levelWhy[(int)sync::LocoLevelWhy::RateIneffective],
                      a.levelWhy[(int)sync::LocoLevelWhy::StraightStuck] - b.levelWhy[(int)sync::LocoLevelWhy::StraightStuck],
                      a.levelWhy[(int)sync::LocoLevelWhy::Recovered] - b.levelWhy[(int)sync::LocoLevelWhy::Recovered],
                      a.levelWhy[(int)sync::LocoLevelWhy::Shared] - b.levelWhy[(int)sync::LocoLevelWhy::Shared],
                      g_locoPushNudge.load() ? "nudge" : "velocity");
        line += t2;
        {
            // R4 T1: the blend's numbers (also printed in the other modes, where the corrections are the older push / nudge): speed from positions
            // against the pose, the largest per-frame correction on top of the POSE velocity, snaps, the error percentiles, the verdict
            char r4[900];
            std::snprintf(r4, sizeof(r4),
                          " | R4 T1: mode %s, speed from positions %.2f vs pose %.2f m/s (ratio %.2f), largest per-frame correction %.2f m/s above the pose velocity (mean along the "
                          "track %.2f), hard snaps %u (%u snap frames incl. teleports), pose-ped error p50 %.2f p95 %.2f max %.2f m, move blend ratio trim %.2f, gait source %s, "
                          "task issues held by a melee swing %u; verdict: %s",
                          LocoModeName(), d.derivedSpeedMs, d.poseSpeedMs, d.poseSpeedMs > 0.05 ? d.derivedSpeedMs / d.poseSpeedMs : 0.0, d.maxCorrMs, pe.loco.BlendMeanCorr(),
                          (unsigned)(a.hardSnaps - b.hardSnaps), (unsigned)d.snapFrames, d.errP50M, d.errP95M, d.maxErrM, pe.loco.BlendTrim(),
                          pe.loco.NoGaitSource() ? "NONE (the task shows no gait; the position is carried by the velocity)" : "ok", (unsigned)(a.swingHeld - b.swingHeld),
                          BlendVerdict(pe, d).c_str());
            line += r4;
        }
        {
            // R5: the grace window after a task-changing Reset (how often it opened, frames it held the verdict, places it did, chain caps), the falls
            // into the rigid follow split into explained (a window was open in the last blendAttributeMs) and unexplained, the probation after the way
            // back up, and the heading numbers (degrees; frames with the pose over 1 m/s): the puppet's heading against the sender's, the angle
            // between the pose's velocity and the puppet's heading, and between the puppet's OWN velocity and its heading (90 = it slides sideways)
            char r5[1100];
            std::snprintf(r5, sizeof(r5),
                          " | R5: grace windows %u (%u frames held, %u places, %u chain caps), falls to the rigid follow %u explained / %u unexplained, probations %u (%u fell again), "
                          "heading error mean %.1f p95 %.1f deg (%u frames; 0.4.30: no ragdoll / jump / swim / aim frames), pose velocity vs puppet heading mean %.1f p95 %.1f deg, "
                          "puppet's own velocity vs its heading mean %.1f p95 %.1f deg (%u frames), SENDER off-axis (velocity vs its own heading) mean %.1f p95 %.1f deg, "
                          "%u of %u frames over 60 deg (strafe / backpedal)",
                          (unsigned)(a.graceWindows - b.graceWindows), (unsigned)(a.graceHeldFrames - b.graceHeldFrames), (unsigned)(a.gracePlaced - b.gracePlaced),
                          (unsigned)(a.graceChainCapped - b.graceChainCapped), (unsigned)(a.fallsAfterGrace - b.fallsAfterGrace),
                          (unsigned)(a.fallsUnexplained - b.fallsUnexplained), (unsigned)(a.probations - b.probations), (unsigned)(a.probationFalls - b.probationFalls),
                          d.headErrMeanDeg, d.headErrP95Deg, (unsigned)d.headFrames, d.poseVelHeadMeanDeg, d.poseVelHeadP95Deg, d.pedVelHeadMeanDeg, d.pedVelHeadP95Deg,
                          (unsigned)d.pedVelHeadFrames, d.senderOffMeanDeg, d.senderOffP95Deg, (unsigned)d.senderOffOver60, (unsigned)d.senderOffFrames);
            line += r5;
        }
        return line;
    }

    std::string LocoLine(int id, const Peer& pe)
    {
        const sync::LocoStats& a = pe.loco.Stats();
        const sync::LocoStats& b = pe.locoSeen;
        const std::string hard = SiteList(pe.hardSites), soft = SiteList(pe.softSites);
        char buf[1300];
        std::snprintf(buf, sizeof(buf),
                      "engine: loco id=%d (%s): last 10 s %u go-straight (first of a gait %u, task lost %u, forced %u, blend %u, "
                      "turn %u, drift %u, arrive %u, stall %u), %u stand-still; %u Reset [%s]; %u SoftReset [%s] (%u kept the "
                      "task, %u re-issued); W11: anchor %s (mode %s, entity %d, %u go-to-entity issues, %u frames, %u stall "
                      "fallbacks to straight, %u spawn fails), %u gait changes carried by SET_PED_DESIRED_MOVE_BLEND_RATIO "
                      "without a re-issue; last applied: flags 0x%04x move %u weapon 0x%08x, latched task %s (gait %s), "
                      "collisionOff %d, climb %s, water %d",
                      id, pe.name.c_str(), a.goStraight - b.goStraight, a.whyGait - b.whyGait, a.whyReset - b.whyReset,
                      a.whyForced - b.whyForced, a.whyBlend - b.whyBlend, a.whyTurn - b.whyTurn, a.whyDrift - b.whyDrift,
                      a.whyArrive - b.whyArrive, a.whyStall - b.whyStall, a.standStill - b.standStill, a.resets - b.resets,
                      hard.c_str(), a.softResets - b.softResets, soft.c_str(), a.softKept - b.softKept,
                      a.softReissued - b.softReissued, pe.anchor ? "on" : "off", LocoModeName(),
                      pe.anchor, a.anchorIssues - b.anchorIssues, a.anchorFrames - b.anchorFrames,
                      a.anchorFallbacks - b.anchorFallbacks, pe.anchorSpawnFails, a.gaitKept - b.gaitKept,
                      (unsigned)pe.lastPed.flags, (unsigned)pe.lastPed.move,
                      (unsigned)pe.lastPed.weapon, LocoTaskName(pe.loco.LastTask()), GaitName(pe.loco.CurrentGait()),
                      pe.collisionOff ? 1 : 0, ClimbPhaseName(pe.climb.CurrentPhase()), pe.water.InWater() ? 1 : 0);
        return buf;
    }

    // ---- seated drive-by (sync plan W2 F) -----------------------------------------------------
    // TASK_DRIVE_BY's parameters. Its documented meanings are NativeDB's; the values are INFERRED, to be
    // confirmed live: distanceToShoot 0 and accuracy 0 so the task keeps the gun raised but is not meant
    // to fire real bullets (the shots players see are PlayPeerShots' zero-damage bullets, one per
    // shotSeq step); the firing pattern 0 becomes FIRING_PATTERN_FULL_AUTO in World::TaskDriveBy.
    constexpr float kDriveByShootDistM = 0.0f;
    constexpr int   kDriveByAccuracy = 0;

    // A seated remote puppet with a gun and kPfAiming shows the drive-by pose. Without an aim task GTA
    // holds no weapon object for a seated ped, so the gun (and the tracers) were never visible. One
    // TASK_DRIVE_BY, then SET_DRIVEBY_TASK_TARGET every kVehAimReissueMs (sync::DecideVehicleAim);
    // TASK_AIM_GUN_AT_COORD when TASK_DRIVE_BY cannot be called (or veh_driveby=aim). Taking it down
    // blends out: CLEAR_PED_TASKS_IMMEDIATELY on a seated ped pulls it out of the car. Script tick,
    // Script tick, no lock, the ped seated (pe.seatedVeh >= 0).
    void TickSeatedAim(Peer& pe, int id, const sync::Pose& pose, double nowMs)
    {
        const proto::PedState& ps = pose.ped;
        // Sync plan W3: through the shared callable-only cache (IsGunWeapon). The per-peer "checked once" cache that
        // stood here remembered a "not a gun" given while the natives were still uncallable, for good.
        pe.weaponIsGun = IsGunWeapon(ps.weapon);
        const sync::VehAim act = sync::DecideVehicleAim(true, ps.flags, pe.weaponIsGun, pe.aimTask, nowMs - pe.driveByAtMs);
        if (act == sync::VehAim::None)
            return;
        const int32_t ped = pe.ped;
        if (act == sync::VehAim::Clear)
        {
            g_world->ClearDrivebyTaskUnderneathDrivingTask(ped);
            g_world->ClearPedTasks(ped);               // blends out and leaves the ped in its seat
            pe.aimTask = false;
            return;
        }
        // Issue: the aim point is the sender's offset from the shown position, as on foot.
        const float ax = pose.k.x + ps.aimX, ay = pose.k.y + ps.aimY, az = pose.k.z + ps.aimZ;
        const bool first = !pe.aimTask;
        bool ok = false;
        if (!pe.driveByFallback && g_vehDriveByAimOnly.load())
            pe.driveByFallback = true;
        if (!pe.driveByFallback)
        {
            if (!first)
                ok = g_world->SetDrivebyTaskTarget(ped, 0, 0, ax, ay, az);
            if (!ok)                                   // the first issue, or the target setter could not be called
                ok = g_world->TaskDriveBy(ped, 0, 0, ax, ay, az, kDriveByShootDistM, kDriveByAccuracy, false, 0);
            if (!ok)
            {
                pe.driveByFallback = true;
                PeerLog(true, "engine: remote peer id=%d (%s): TASK_DRIVE_BY cannot be called -- the seated aim falls "
                        "back to TASK_AIM_GUN_AT_COORD", id, pe.name.c_str());
            }
        }
        if (pe.driveByFallback)
            ok = g_world->TaskAimGunAtCoord(ped, ax, ay, az, -1);
        if (ok && !pe.driveByLogged)
        {
            pe.driveByLogged = true;
            PeerLog(false, "engine: remote peer id=%d (%s): seated aim started with %s, weapon 0x%08x at %.1f, %.1f, %.1f "
                    "(drive-by pose and firing unverified live)", id, pe.name.c_str(),
                    pe.driveByFallback ? "TASK_AIM_GUN_AT_COORD" : "TASK_DRIVE_BY", (unsigned)ps.weapon, ax, ay, az);
        }
        pe.aimTask = ok;
        pe.driveByAtMs = nowMs;
    }

    // Makes a remote player's ped follow the pose the smoother gives for this frame. Script tick
    // only, no lock, pe.ped != 0. dtMs is the time since this ped's previous DrivePeer (0 on
    // the first one after a spawn: the controller, just ResetForNewPed, snaps then). pose.teleported is true
    // on the first frame after a snap (respawn, warp); the controller snaps on it.
    // peer_drive=tasks: the locomotion controller (libs/sync/locomotion.h) decides tasks, velocity
    // steer, heading and the rare snap; this applies them in the order its header gives.
    void UnseatPeer(Peer& pe, int32_t leaveVeh);   // defined below (its default argument is declared there)

    // R3 T2: what the controller learned about coordinate writes on this puppet, the level cap, and the repairs it asks of the engine.
    // All of them are rare, one-off events; the per-puppet budget keeps a broken session from flooding the log.
    const char* WriteVerdictName(sync::WriteVerdict v)
    {
        return v == sync::WriteVerdict::Harm ? "HARM (every coordinate write cancels the walk)"
             : v == sync::WriteVerdict::Harmless ? "harmless (the writes are not why the task shows nothing)" : "unknown";
    }
    void NoteLocoVerdict(Peer& pe, int id, const sync::LocoOutput& o)
    {
        if (pe.verdictLogged >= 6)
            return;
        ++pe.verdictLogged;
        const sync::LocoStats& s = pe.loco.Stats();
        PeerLog(true, "engine: loco write verdict id=%d (%s): %s (quiet windows %u: %u showed a gait with no coordinate write, %u did not). %s", id,
                pe.name.c_str(), WriteVerdictName(o.writeVerdict), s.quietWindows, s.quietHarm, s.quietClear,
                o.writeVerdict == sync::WriteVerdict::Harm
                    ? "The position error is now closed by single pulses (one write per 1.5 s, beyond 0.9 m) instead of a write on every frame."
                    : "The ladder and the repairs go on from the task, not the writes.");
    }
    // The engine's side of sync::LocoRepair: what to do to a puppet that no level of the ladder moved. ClearTasks keeps the ped; the
    // respawns are done by TickPeers (pe.respawnPed) with the recipe set here. Script tick, ped valid.
    void ApplyLocoRepair(Peer& pe, int id, const sync::LocoOutput& o, const sync::LocoInput& in, double nowMs)
    {
        (void)nowMs;
        const float sp = std::sqrt(in.vel.x * in.vel.x + in.vel.y * in.vel.y);
        ++pe.repairsDone;
        const char* what = "";
        switch (o.repair)
        {
        case sync::LocoRepair::ClearTasks:
            what = "CLEAR_PED_TASKS_IMMEDIATELY, freeze off, collision on, then the gait task again";
            ClearPedTasksNow(pe.ped);
            g_world->FreezeEntityPosition(pe.ped, false);
            g_world->SetEntityCollision(pe.ped, true);
            pe.aimTask = false;
            LocoReset(pe, kSiteRepair);
            break;
        case sync::LocoRepair::RespawnLean:
            what = "the ped is created again with the LEAN recipe (invincible + events blocked only)";
            pe.recipe = 1;
            pe.respawnPed = true;
            break;
        case sync::LocoRepair::RespawnAlt:
            what = "the ped is created again with the ALT recipe (civilian ped type, no script-host flag, lean flags)";
            pe.recipe = 2;
            pe.respawnPed = true;
            break;
        case sync::LocoRepair::None:
            return;
        }
        PeerLog(true, "engine: loco repair id=%d (%s): step %d of 3 because the gait task stalled twice and no coordinate write was to blame (pose speed "
                "%.2f m/s, the ped's own speed %.2f m/s, write verdict %s): %s", id, pe.name.c_str(), pe.repairsDone, sp, pe.loco.SelfSpeed(),
                WriteVerdictName(o.writeVerdict), what);
    }
    // R3 T2: the task types GET_IS_TASK_ACTIVE says are running on the ped (indices 0..kTaskScanMax, no name guessed): the list of a puppet
    // that "runs" a go-to task shows whether the game really has one, and compares with the same list of the local player walking.
    constexpr int kTaskScanMax = 600;
    std::string ActiveTaskTypes(int32_t ped)
    {
        std::string s;
        int n = 0;
        for (int t = 0; t < kTaskScanMax; ++t)
            if (g_world->GetIsTaskActive(ped, t))
            {
                if (n++)
                    s += ",";
                s += std::to_string(t);
                if (n >= 24)
                {
                    s += ",...";
                    break;
                }
            }
        return n ? s : std::string("none");
    }
    // The per-puppet gait line (every 10 s while a moving gait was commanded): what the game says the ped does, next to what the pose
    // asks. One log is enough to tell W (writes cancel the walk), D / F (no task moves it) and B (the gait natives lie) apart.
    std::string GaitLine(int id, const Peer& pe)
    {
        const sync::GaitDiagSummary g = pe.gaitDiag.Summary();
        const sync::LocoStats& s = pe.loco.Stats();
        const float lat = pe.loco.MeanLatencyMs();
        char latb[48];
        if (lat >= 0.0f) std::snprintf(latb, sizeof(latb), "%.0f ms over %u", lat, s.latencyN);
        else std::snprintf(latb, sizeof(latb), "none seen");
        char buf[1100];
        std::snprintf(buf, sizeof(buf),
                      "engine: gait id=%d (%s): last 10 s a moving gait was commanded on %u of %u frames; the game READS still %.0f%% walking %.0f%% running %.0f%% "
                      "sprinting %.0f%% (IS_PED_*), IS_PED_STILL true on %u of %u asks; speed: pose %.2f, ped from positions %.2f (our writes included), "
                      "GET_ENTITY_SPEED %.2f, own displacement net of our writes %.2f m/s; move blend ratio set on %u frames (mean %.2f), coordinate writes on "
                      "%u frames, %u task issues, task-to-first-gait %s issues; ragdoll %u frames; write verdict %s (quiet windows %u: %u gait / %u none, pulses %u); "
                      "ladder level %d, %u changes this session%s; repairs %d, creation recipe %d",
                      id, pe.name.c_str(), g.commandedFrames, g.frames, g.readShare(g.readStill) * 100.0, g.readShare(g.readWalk) * 100.0,
                      g.readShare(g.readRun) * 100.0, g.readShare(g.readSprint) * 100.0, g.stillTrue, g.stillAsked, g.poseMs, g.posMs, g.entityMs, g.ownMs,
                      g.blendFrames, g.blendMean, g.writeFrames, g.issues, latb, g.ragdollFrames, WriteVerdictName(pe.loco.Verdict()), s.quietWindows,
                      s.quietHarm, s.quietClear, s.pulses, (int)pe.loco.Level(), (unsigned)pe.loco.LevelChangesTotal(),
                      pe.loco.LadderFrozen() ? " (FROZEN by the cap)" : "", pe.repairsDone, pe.recipe);
        std::string line = buf;
        line += "; R4 T1 " + BlendVerdict(pe, pe.slide.Summary());
        if (g.commandedFrames > 0)
            line += "; active task types [" + ActiveTaskTypes(pe.ped) + "]";
        return line;
    }

    // 0.5.7: the per-puppet swing-walk line (every 10 s with a moving gait asked or a walk-through issue): metres the puppet moved while
    // IS_PED_STILL read true with a moving gait commanded, by cause (a swing hold the walk-through did not apply to = the 0.5.6 slide; one it
    // applied to; no swing), and what IS_ENTITY_PLAYING_ANIM said 100+ ms after a walk-through gait task went out under the clip.
    std::string SwingWalkLine(int id, const Peer& pe)
    {
        const sync::SwingWalkSummary& s = pe.swingWalkDiag.Summary();
        if (!s.Any())
            return std::string();
        using C = sync::SwingWalkSummary;
        char buf[720];
        std::snprintf(buf, sizeof(buf),
                      "engine: on-foot swing walk (0.5.7) id=%d (%s): loco_swing_walk=%s; moved while IS_PED_STILL read true with a moving gait commanded: "
                      "in a held swing %.2f m (%u of %u asks), in a walk-through swing %.2f m (%u of %u asks), no swing %.2f m (%u of %u asks); "
                      "walk-through task issues %u; the swing clip 100+ ms after such an issue: still playing %u, NOT playing %u, unread %u",
                      id, pe.name.c_str(), g_locoSwingWalk.load() ? "on" : "OFF", s.slideM[C::Held], s.slideFrames[C::Held], s.askedFrames[C::Held],
                      s.slideM[C::Through], s.slideFrames[C::Through], s.askedFrames[C::Through], s.slideM[C::Other], s.slideFrames[C::Other],
                      s.askedFrames[C::Other], s.walkIssued, s.clipSurvived, s.clipCut, s.clipUnread);
        return buf;
    }

    // T2: a puppet's controller moved to another level of the degradation ladder: say so (the numbers that decided it are in the
    // line), count it, and tell the witness (two puppets finding the same failure make it machine-wide).
    void NoteLocoLevel(Peer& pe, int id, const sync::LocoOutput& o, const sync::LocoInput& in, double nowMs)
    {
        const bool shared = g_locoDegrade.Report(o.levelWhy, id, nowMs);
        if (g_locoLevelLines < 60)
        {
            ++g_locoLevelLines;
            const float sp = std::sqrt(in.vel.x * in.vel.x + in.vel.y * in.vel.y);
            PeerLog(true, "engine: loco level id=%d (%s): -> %s because %s (pose speed %.2f m/s, the ped's own speed %.2f m/s, %s, "
                    "anchor %s, learned stride x walk %.2f run %.2f sprint %.2f)%s", id, pe.name.c_str(), LocoLevelName(o.level),
                    LocoLevelWhyName(o.levelWhy), sp, pe.loco.SelfSpeed(), pe.slide.Summary().meanAnchorM > 0.0 ? "anchor distance measured" : "no anchor frames",
                    pe.anchor ? "present" : "absent", pe.loco.NominalScale(sync::Gait::Walk), pe.loco.NominalScale(sync::Gait::Run),
                    pe.loco.NominalScale(sync::Gait::Sprint), shared ? "; a SECOND puppet found it: imposed on every puppet for a while" : "");
        }
    }

    // R3 T3: cover as a VERIFIED window (libs/sync/cover.h CoverPuppet), once per DrivePeer from TickPeerTierB. The 0d version put the puppet on
    // the sender's pose (a SET at 1 m or more: a teleport), issued TASK_PUT_PED_DIRECTLY_INTO_COVER at coordinates where a car is no cover
    // point and froze the puppet for 700 ms; it then stood upright on the pose while the sender crouched, or was snapped onto a cover point
    // the game found a few decimetres away. Now: nothing is forced. The puppet stays under the locomotion pipeline until it is within 0.30 m
    // of the pose and the pose is nearly still; the task goes out ONCE (parameters as in 0d: time -1, no peek / fire, blend 0, doEntry false)
    // and counts only when IS_PED_IN_COVER reads true within the window, the puppet is still near the pose and faces the sender's way. A puppet
    // the game moved (a snap), a task that is not accepted (no cover point, or a puppet that ignores tasks), a sender that slides along the
    // cover or aims all end in ONE ClearPedTasksNow and the pose fallback: the pipeline keeps the puppet on the sender's position and the
    // crouch hint (kPfStealth, set by the sender) crouches it. Returns true while the cover task owns the ped (DrivePeer then runs no gait
    // task, no velocity push and no anchor move). Every step is counted per puppet ("engine: cover puppet (R3 T3) id=..." every 10 s).
    bool TickPeerCover(Peer& pe, int id, const sync::Pose& pose, double nowMs)
    {
        const proto::PedState& ps = pose.ped;
        const sync::CoverSyncMode mode = (sync::CoverSyncMode)g_coverSyncMode.load(std::memory_order_relaxed);
        pe.cover.SetMode(mode);
        sync::CoverIn in;
        in.flag = (ps.flags & proto::kPfCover) != 0 && (ps.flags & proto::kPfRagdoll) == 0 && (ps.flags & proto::kPfDead) == 0 && pe.seatedVeh < 0;
        in.aiming = sync::AimWanted(ps.flags, ps.weapon);
        in.poseSpeed = std::sqrt(pose.k.vx * pose.k.vx + pose.k.vy * pose.k.vy);
        in.nowMs = nowMs;
        float cx = 0, cy = 0, cz = 0;
        if (pe.cover.NeedsPed(in.flag) && g_world->GetEntityCoords(pe.ped, cx, cy, cz))
        {
            in.pedReadable = true;
            const float dx = cx - pose.k.x, dy = cy - pose.k.y, dz = cz - pose.k.z;
            in.distM = std::sqrt(dx * dx + dy * dy + dz * dz);
            in.headingErrDeg = sync::CoverHeadingErrDeg(g_world->GetEntityHeading(pe.ped), Heading360(pose.k.heading));
            const float mx = cx - pe.coverIssuePos[0], my = cy - pe.coverIssuePos[1], mz = cz - pe.coverIssuePos[2];
            in.movedM = std::sqrt(mx * mx + my * my + mz * mz);
        }
        if (pe.cover.Busy())
        {
            in.pedInCover = g_world->IsPedInCover(pe.ped, false);
            in.pedGoingInto = g_world->IsPedGoingIntoCover(pe.ped);
        }
        const sync::CoverStats before = pe.cover.Stats();
        sync::CoverOut cv = pe.cover.Update(in, &g_coverBreaker);
        static int coverLogged = 0;
        if (cv.accepted)
        {
            ++g_v4.coverStarted;
            if (coverLogged < 24)
            {
                ++coverLogged;
                PeerLog(false, "engine: cover id=%d (%s): IS_PED_IN_COVER confirmed %.0f ms after the task (puppet %.2f m from the sender's pose, moved %.2f m "
                        "by the game, heading off %.0f deg)", id, pe.name.c_str(), nowMs - pe.coverIssueMs, in.distM, in.movedM, in.headingErrDeg);
            }
        }
        if (cv.issue)
        {
            ++g_v4.coverIssued;
            pe.aimTask = false;
            if (cv.nudge)
            {
                // At most 0.30 m (the window gates on it): the puppet is put exactly on the pose so the cover point is searched where the sender is.
                g_world->SetEntityCoordsNoOffset(pe.ped, pose.k.x, pose.k.y, pose.k.z);
                if (in.headingErrDeg > sync::kPrePlaceHeadingDeg)
                    g_world->SetEntityHeading(pe.ped, Heading360(pose.k.heading));
                cx = pose.k.x; cy = pose.k.y; cz = pose.k.z;
            }
            pe.coverIssuePos[0] = cx; pe.coverIssuePos[1] = cy; pe.coverIssuePos[2] = cz;
            pe.coverIssueMs = nowMs;
            const bool called = g_world->TaskPutPedDirectlyIntoCover(pe.ped, pose.k.x, pose.k.y, pose.k.z, -1, false, 0.0f, false, false, -1, false);
            if (!called)
            {
                const sync::CoverOut f = pe.cover.NoteIssueFailed(nowMs, &g_coverBreaker);
                cv.why = f.why;
                cv.suspend = false;
            }
            if (coverLogged < 24)
            {
                ++coverLogged;
                PeerLog(!called, "engine: cover id=%d (%s): sender in cover (flags 0x%x, pose speed %.2f m/s), puppet %.2f m from the pose%s: "
                        "TASK_PUT_PED_DIRECTLY_INTO_COVER at %.2f %.2f %.2f %s", id, pe.name.c_str(), (unsigned)ps.flags, (double)in.poseSpeed, in.distM,
                        cv.nudge ? " (put exactly on it first)" : "", pose.k.x, pose.k.y, pose.k.z,
                        called ? "issued once; checked with IS_PED_IN_COVER within 400 ms" : "could not be called");
            }
        }
        if (cv.stay)
        {
            g_world->TaskStayInCover(pe.ped);
            if (coverLogged < 24)
            {
                ++coverLogged;
                PeerLog(true, "engine: cover id=%d (%s): out of cover while the sender is in it for 350 ms: TASK_STAY_IN_COVER once", id, pe.name.c_str());
            }
        }
        if (cv.why != sync::CoverWhy::None)
        {
            ++g_v4.coverFailed;
            if (coverLogged < 24)
            {
                ++coverLogged;
                PeerLog(true, "engine: cover id=%d (%s): FALLBACK to the crouch pose on the sender's position (%s): puppet %.2f m from the pose, moved %.2f m "
                        "by the game, heading off %.0f deg, IS_PED_IN_COVER %d IS_PED_GOING_INTO_COVER %d, %s", id, pe.name.c_str(), sync::CoverWhyName(cv.why),
                        in.distM, in.movedM, in.headingErrDeg, (int)in.pedInCover, (int)in.pedGoingInto, cv.clear ? "task cleared once" : "no task was on the ped");
            }
        }
        if (cv.clear)
        {
            if (cv.ended)
                ++g_v4.coverEnded;
            if (pe.seatedVeh < 0)
                ClearPedTasksNow(pe.ped);              // once: the controller re-issues its gait and keeps the ped on the pose
            pe.aimTask = false;
            LocoReset(pe, cv.why != sync::CoverWhy::None ? kSiteCoverFail : kSiteCoverExit);
        }
        (void)before;
        return cv.suspend;
    }

    // The Tier B receivers of protocol v5's PedState (sync plan W6), run once per DrivePeer before the ped is tasked:
    // reload once per edge, stealth on change (refreshed while on), cover (TickPeerCover, a window), a
    // walk to a networked car on the entering move (TASK_ENTER_VEHICLE to that car, seat any; no jack flags), and
    // the way out on the exiting move. INFERRED live checks: that the native seat -2 takes any free seat, that
    // SET_PED_STEALTH_MOVEMENT's second argument true means "on", and that cover finds a point. Returns true while
    // the enter walk or the cover owns the ped (DrivePeer then issues no gait task over it).
    // T1 seats (wire v6): the replay of one remote player's enter intent (libs/sync/seat_intent.h PuppetSeatReplay), once per DrivePeer from
    // TickPeerTierB. SeatNotice::Reserved started it (HandleSeatNotice); this issues GTA's own TASK_ENTER_VEHICLE for the reserved seat ONCE,
    // watches it (is the car named by GET_VEHICLE_PED_IS_TRYING_TO_ENTER, is the ped in the seat), clears it when the server cancels the
    // intent, and tells the seat loop (TickVehicles, AllowsSeatWarp) when the BOUNDED fallback warp is allowed. Returns true while the
    // task owns the ped (no gait task over it, also when it is seated by the task and the server's list has not named it yet).
    // Natives: GET_VEHICLE_PED_IS_TRYING_TO_ENTER naming a puppet's scripted TASK_ENTER_VEHICLE, GET_VEHICLE_CLASS 8 / 13 for bikes and
    // cycles, TASK_ENTER_VEHICLE's seat / speed arguments on a puppet: all INFERRED, live-unverified (the counters tell: issued / started /
    // finished / neverStarted / warps in the "engine: seats (T1)" line). Script tick, no lock.
    bool TickPeerSeatReplay(Peer& pe, int id, double nowMs)
    {
        sync::PuppetSeatReplay* rp = g_seats.Find((uint16_t)id);
        if (!rp || !rp->NeedsReadings())
            return false;
        sync::SeatReplayIn in;
        in.nowMs = nowMs;
        in.pedReady = pe.ped != 0;
        int32_t h = 0;
        const int veh = rp->Veh();
        if (veh >= 0)
        {
            const auto vit = g_vehs.find((uint16_t)veh);
            if (vit != g_vehs.end() && vit->second.handle && !vit->second.removed)
            {
                h = vit->second.handle;
                in.vehReady = true;
                for (size_t s = 0; s < proto::kVehSeats; ++s)
                    in.listedSomewhere = in.listedSomewhere || vit->second.st.seats[s] == (uint16_t)id;
            }
        }
        if (h && pe.ped)
        {
            in.inSeat = g_world->GetPedInVehicleSeat(h, rp->Seat() - 1) == pe.ped;
            // "running" = the game says this ped is entering that car by either native (TRYING_TO_ENTER, or USING = in or entering; a ped that is
            // in the seat is already caught by inSeat above): one native that does not answer for a scripted task must not read as a task that never ran.
            in.tryingToEnter = g_world->GetVehiclePedIsTryingToEnter(pe.ped) == h || g_world->GetVehiclePedIsUsing(pe.ped) == h;
            const int cls = g_world->GetVehicleClass(h);
            in.mount = cls == 8 || cls == 13;          // motorcycles, cycles (hook.h)
        }
        const sync::SeatReplayOut o = rp->Tick(in);
        switch (o.act)
        {
        case sync::SeatAct::IssueEnter:
        {
            ClearPedTasksNow(pe.ped);                  // the gait task goes; the enter task replaces it
            const bool ok = g_world->TaskEnterVehicle(pe.ped, h, o.timeoutMs, o.seat - 1, o.run ? 2.0f : 1.0f, 1);
            pe.aimTask = false;
            LocoReset(pe, kSiteEnterWalk);
            if (!ok)
                rp->NoteIssueFailed();                 // TASK_ENTER_VEHICLE not callable: behave as a task that never ran (the bounded warp)
            static int logged = 0;
            if (logged < 12)
            {
                ++logged;
                PeerLog(!ok, "engine: seats: replaying player %d's entry: TASK_ENTER_VEHICLE vehicle id=%d seat %d (%s, %s, limit %d ms): %s", id,
                        o.veh, o.seat, o.run ? "run" : "walk", in.mount ? "bike/cycle" : "door", o.timeoutMs, ok ? "issued" : "NOT callable");
            }
            break;
        }
        case sync::SeatAct::ClearTask:
            ClearPedTasksNow(pe.ped);                  // cancelled or over without a seat: the controller re-issues its gait
            pe.aimTask = false;
            LocoReset(pe, kSiteEnterTimeout);
            break;
        case sync::SeatAct::Evict:
        {
            // Seated by the task, and the server's list never named the player: the screens disagree. Out of the car.
            const auto vit = g_vehs.find((uint16_t)veh);
            ++g_t1.evictLog;
            if (g_t1.evictLog <= 8)
                PeerLog(true, "engine: seats: DIVERGENCE player %d sits in vehicle id=%d seat %d here but the server's seat list never named it: put out", id,
                        veh, rp->Seat());
            UnseatPeer(pe, vit != g_vehs.end() && !vit->second.removed ? vit->second.handle : 0);
            break;
        }
        case sync::SeatAct::None:
            break;
        }
        return o.holdsPed;
    }

    bool TickPeerTierB(Peer& pe, int id, const sync::Pose& pose, double nowMs)
    {
        const proto::PedState& ps = pose.ped;
        // R3 T3: the sender's crouch hint in cover (kPfStealth set with kPfCover) is the POSE fallback's crouch; while the cover task path owns the
        // hold (not the fallback) the stealth edge is not applied over a cover task. It is applied the moment the window falls back to the pose.
        uint16_t edgeFlags = ps.flags;
        if ((ps.flags & proto::kPfCover) && (sync::CoverSyncMode)g_coverSyncMode.load(std::memory_order_relaxed) == sync::CoverSyncMode::Task &&
            pe.cover.CurrentPhase() != sync::CoverPhase::Pose)
            edgeFlags = (uint16_t)(edgeFlags & ~proto::kPfStealth);
        const sync::PedEdges e = pe.edges.Update({ edgeFlags, ps.move, ps.vehId });
        const int32_t ped = pe.ped;
        if (e.reloadStart && pe.seatedVeh < 0)
        {
            g_world->TaskReloadWeapon(ped, false);
            ArmOneShot(pe, sync::OneShot::Reload, nowMs);
        }
        if (e.stealthChanged || (e.stealthOn && nowMs - pe.stealthAtMs > 2000.0))
        {
            if (e.stealthChanged)
                ArmOneShot(pe, sync::OneShot::Stealth, nowMs, e.stealthOn);
            g_world->SetPedStealthMovement(ped, e.stealthOn, "DEFAULT_ACTION");
            pe.stealthOn = e.stealthOn;
            pe.stealthAtMs = nowMs;
        }
        const bool coverHolds = TickPeerCover(pe, id, pose, nowMs);
        if (e.exitStart && pe.seatedVeh >= 0)
        {
            // The sender is getting out: climb out with TASK_LEAVE_VEHICLE (flags 0) now instead of waiting for
            // the seat list to lose it (UnseatPeer pauses the controller for the exit animation).
            const auto vit = g_vehs.find((uint16_t)pe.seatedVeh);
            if (vit != g_vehs.end() && vit->second.handle)
                UnseatPeer(pe, vit->second.handle);
        }
        // Sync D: the sender's F / G entry is a run to the door and a warp into the seat (never a door animation), so by default the
        // puppet keeps FOLLOWING the sender's pose with the ordinary locomotion and is warped into the seat when the seat list names
        // the player (a few tens of cm); its own TASK_ENTER_VEHICLE walk (blend 1.0, its own path, a door animation) fell behind a
        // running sender and was warped from half the door distance away. puppet_enter=task restores it (sync::EnterWalkMode).
        // T1 seats: with the server's intent (default) the walk to the door is the real TASK_ENTER_VEHICLE, started by SeatNotice::Reserved and
        // replayed by TickPeerSeatReplay; the two older modes below only run with HKCU seat_intent=0.
        const bool seatIntent = g_seatIntentOn.load();
        const bool enterTask = !seatIntent && sync::PuppetUsesEnterTask(g_peerEnterTask.load() ? sync::EnterWalkMode::Task : sync::EnterWalkMode::Follow);
        if (e.enterStart && !enterTask && !seatIntent)
            ++g_syncD.enterFollowed;
        if (e.enterStart && enterTask && pe.seatedVeh < 0 && !g_world->IsPedInVehicle(ped))
        {
            const auto vit = g_vehs.find((uint16_t)e.enterVeh);
            if (vit != g_vehs.end() && vit->second.handle && !vit->second.removed)
            {
                // Seat from the car's seat list when the sender already holds one (it does not yet, it is entering),
                // else -2: any free passenger seat. The sender's seat arrives with the seat list; the seat warp then
                // moves a wrong choice (TickVehicles, EnterWarpDue).
                int seat = -2;
                for (size_t s = 0; s < proto::kVehSeats; ++s)
                    if (vit->second.st.seats[s] == (uint16_t)id)
                        seat = (int)s - 1;
                ClearPedTasksNow(ped);
                if (g_world->TaskEnterVehicle(ped, vit->second.handle, 5000, seat, 1.0f, 1))
                {
                    ++g_syncD.enterTasked;
                    pe.enterVeh = e.enterVeh;
                    pe.enterTaskMs = nowMs > 0.0 ? nowMs : 1.0;
                    pe.aimTask = false;
                    LocoReset(pe, kSiteEnterWalk);
                }
            }
        }
        // The walk to the car. While the sender still says entering, nothing else tasks the ped (a walk that has not
        // finished in 6 s is dropped). Once it says in-vehicle (move 4) the ped keeps its animation until the seat
        // list names it and TickVehicles' seat warp moves it to the right seat (sync::EnterWarpDue: at once when it
        // is already inside, else after kEnterTaskMaxMs). A sender that stopped entering without sitting down gave up.
        if (pe.enterTaskMs > 0.0)
        {
            const bool inCar = g_world->IsPedInVehicle(ped);
            if (ps.move == proto::kMoveEnteringVehicle)
            {
                if (nowMs - pe.enterTaskMs <= 6000.0)
                    return true;
                pe.enterTaskMs = 0.0;                  // the walk timed out
                pe.enterVeh = -1;
                ClearPedTasksNow(ped);
                LocoReset(pe, kSiteEnterTimeout);
            }
            else if (ps.move == proto::kMoveInVehicle)
            {
                // W9 item 1: the sender is no longer entering (F warps the real passenger into the seat in 0.3 s), so the
                // door walk is over the moment the server's seat list names this player (the seat warp, TickVehicles,
                // follows). Until the list does, the walk plays on at most kEnterTaskMaxMs (the list is a tick or two
                // behind the sender's move): EnterWarpDue is asked "is the sender still entering" = "not listed yet".
                bool listed = false;
                const auto evit = g_vehs.find((uint16_t)pe.enterVeh);
                if (evit != g_vehs.end())
                    for (size_t s2 = 0; s2 < proto::kVehSeats; ++s2)
                        listed = listed || evit->second.st.seats[s2] == (uint16_t)id;
                if (!sync::EnterWarpDue(true, false, inCar ? 1e9 : nowMs - pe.enterTaskMs, !listed))
                    return true;                       // still walking in; the seat warp comes when it is due
            }
            else if (!inCar)
            {
                pe.enterTaskMs = 0.0;                  // the sender gave up: stop walking to the car
                pe.enterVeh = -1;
                ClearPedTasksNow(ped);
                LocoReset(pe, kSiteEnterGaveUp);
            }
        }
        const bool seatHolds = seatIntent && TickPeerSeatReplay(pe, id, nowMs);
        return coverHolds || seatHolds;
    }

    void DrivePeer(Peer& pe, int id, const sync::Pose& pose, double dtMs, double nowMs)
    {
        const int mode = g_peerDrive.load();
        if (mode != pe.driveMode)
        {
            if (pe.driveMode == kDriveTasks && pe.seatedVeh < 0)
                ClearPedTasksNow(pe.ped);              // to placement: stop the last task walking it off
            pe.aimTask = false;
            LocoReset(pe, kSiteDriveMode);             // to tasks: re-issue everything (a real divergence still snaps)
            pe.driveMode = mode;
        }
        const proto::PedState& ps = pose.ped;          // discrete: the snapshot at or before the render time
        pe.lastPed = ps;
        // W8 item 4: the sender entering or leaving water re-evaluates the gait task at once (kept when it still serves).
        // W8 item 5: climbing / vaulting is ONE TASK_CLIMB per rising edge (sync::ClimbWindow, below); only the parachute
        // and the climb's placement fallback (the task did not start within 300 ms) put the ped on the pose with its
        // collision off. Collision comes back the moment neither applies, whichever early return follows (a cleared
        // kPfClimbing / kPfVaulting / kPfParachute included: the alt-tab fallback state clears them).
        const bool notRagdoll = (ps.flags & proto::kPfRagdoll) == 0;
        const sync::WaterEdge::Out waterEdge = pe.water.Update((ps.flags & proto::kPfSwimming) != 0);
        if (waterEdge.entered || waterEdge.left)
        {
            LocoSoft(pe, kSiteWater);
            ArmOneShot(pe, sync::OneShot::Swim, nowMs, waterEdge.entered);   // T2: IS_PED_SWIMMING must follow the sender's water edge
        }
        const bool climbFlag = notRagdoll && (ps.flags & (proto::kPfClimbing | proto::kPfVaulting)) != 0;
        const bool parachute = notRagdoll && (ps.flags & proto::kPfParachute) != 0;
        const bool placeInAir = parachute || (climbFlag && pe.climb.CurrentPhase() == sync::ClimbWindow::Phase::Fallback);
        if (pe.collisionOff && !placeInAir)
        {
            g_world->SetEntityCollision(pe.ped, true);
            pe.collisionOff = false;
            LocoReset(pe, kSiteCollisionBack);         // back on foot: re-issue the gait task, no snap
        }
        if (ps.flags & proto::kPfShooting)
            ++pe.shootingFrames;
        ShowPeerWeapon(pe, ps.weapon);
        PlayPeerShots(pe, id, pose, nowMs);            // v4: zero-damage bullets per shotSeq step (W3: debt, flash, swing)
        const bool reactHolds = TickPeerHitReact(pe, id, pose, nowMs);   // W3: a cosmetic hit reaction owns the ped
        if (pe.deadShown)
        {
            pe.climb.Reset();
            pe.cover.Reset();
            pe.audit.CancelAll();
            return;                                    // playing dead (UpdatePeerDeath): no tasks, no placing
        }
        ObservePeerOneShots(pe, nowMs);                // T2: the game's readback of the one-shots asked for (only while one is armed)
        ObservePeerEdges(pe, ps, nowMs);               // R5 telemetry: the sender's swim / ragdoll flag against the puppet's read-back (counts only)
        const bool tierBHolds = !reactHolds && TickPeerTierB(pe, id, pose, nowMs);   // W6: reload / stealth / cover / enter / exit
        if (pe.seatedVeh >= 0)
        {
            pe.climb.Reset();                          // a seated ped climbs nothing
            pe.lastMove = ps.move;                     // seated in a synced vehicle: the vehicle carries it
            TickSeatedAim(pe, id, pose, nowMs);        // a raised gun is the only thing it adds
            return;
        }
        if (pe.carryFromMs > 0.0)
        {
            // W9 item 5: a bail-out leaves a moving vehicle with its momentum: the ped is given the vehicle's velocity for
            // kCarryVelocityMs from the first frame it is outside (fading), so it does not drop dead at the door.
            if (nowMs - pe.carryFromMs > 2000.0)
                pe.carryFromMs = pe.carryOutMs = 0;                // never got out: the pop below has it
            else if (!g_world->IsPedInVehicle(pe.ped))
            {
                if (pe.carryOutMs <= 0.0)
                    pe.carryOutMs = nowMs;
                const float k = sync::CarryVelocityScale(nowMs - pe.carryOutMs);
                if (k <= 0.0f)
                    pe.carryFromMs = pe.carryOutMs = 0;
                else
                    g_world->SetEntityVelocity(pe.ped, pe.carryVel[0] * k, pe.carryVel[1] * k, pe.carryVel[2] * k);
            }
        }
        if (pe.leaveUntilMs > 0.0 && nowMs >= pe.leaveUntilMs)
        {
            // The exit animation of UnseatPeer had its time. A normal exit from a car that is still moving
            // waits for it to stop: if the ped is still inside, take it out at once (the old pop).
            pe.leaveUntilMs = 0;
            if (g_world->IsPedInVehicle(pe.ped))
            {
                ClearPedTasksNow(pe.ped);
                pe.aimTask = false;
                LocoReset(pe, kSiteLeaveVeh);
            }
        }
        const bool emoteHolds = TickPeerEmote(pe, id, pose, nowMs);
        if (reactHolds)
        {
            pe.lastMove = ps.move;
            return;                                    // a hit reaction (flinch / ragdoll) owns the ped: no gait, aim or placement over it
        }
        if (tierBHolds)
        {
            // The walk to a car or the cover task owns the ped. The pose still wins over a big drift (the sender was
            // placed elsewhere, a teleport): placed onto it, and the hold ends.
            float cx, cy, cz;
            if (g_world->GetEntityCoords(pe.ped, cx, cy, cz))
            {
                const float ddx = cx - pose.k.x, ddy = cy - pose.k.y;
                if (ddx * ddx + ddy * ddy > 4.0f * 4.0f)
                {
                    PlacePeer(pe, pose);
                    pe.enterTaskMs = 0.0;
                    LocoReset(pe, kSiteTierBDrift);
                }
            }
            pe.lastMove = ps.move;
            return;
        }
        if (mode == kDrivePlace)
        {
            if (!emoteHolds)
                PlacePeer(pe, pose);
            return;
        }

        // In a vehicle (move 4..6) with no synced vehicle seat for it here (its VehicleWorld entry
        // not received yet, or out of our vehicle range): the ped stays visible, placed on the pose
        // (the seat position the sender reports), with no tasks. Back on foot: the controller
        // re-issues its gait task (Reset: the ped was just placed on the pose, nothing to snap).
        if (ps.move == proto::kMoveInVehicle)
        {
            if (pe.lastMove != proto::kMoveInVehicle)
            {
                ClearPedTasksNow(pe.ped);
                pe.aimTask = false;
            }
            pe.lastMove = ps.move;
            PlacePeer(pe, pose);
            return;
        }
        if (pe.lastMove == proto::kMoveInVehicle)
            LocoReset(pe, kSiteBackFromVeh);
        pe.lastMove = ps.move;
        if (emoteHolds)
            return;                                    // an emote owns the ped (placed by TickPeerEmote when it drifts)
        // Climb / vault (W8 item 5): TASK_CLIMB once on the rising edge, then no gait task, no velocity and no position
        // correction until the flag has fallen and settled (the correction fighting the climb was the live-test "teleports
        // up the wall"); the settle ends with a Reset so the gait task is issued again. The game is asked whether the
        // puppet really climbs / vaults only while a window is open.
        const sync::ClimbWindow::Phase climbBefore = pe.climb.CurrentPhase();
        bool pedClimbing = false;
        if (climbFlag || pe.climb.Busy())
            pedClimbing = g_world->IsPedClimbing(pe.ped) || g_world->IsPedVaulting(pe.ped);
        const sync::ClimbWindow::Out cw = pe.climb.Update(climbFlag, pedClimbing, nowMs);
        if (cw.issueClimb)
        {
            pe.aimTask = false;
            // 0d: the puppet first goes onto the sender's pose of the edge (position + heading: it is at the obstacle, facing it;
            // the puppet is ~100 ms behind and the vault is short), the task goes out the same frame. The loco / anchor / push
            // are suspended by the window from this frame on, so nothing drives the ped off the pose again.
            PrePlacePeer(pe, pose);
            const bool issued = g_world->TaskClimb(pe.ped, false);
            if (issued)
                ++g_v4.climbTasks;
            else
                pe.climb.NoteIssueFailed();
            static int climbLogged = 0;
            if (climbLogged < 8)
            {
                ++climbLogged;
                PeerLog(!issued, "engine: remote peer id=%d (%s) %s (flags 0x%x): TASK_CLIMB %s", id, pe.name.c_str(),
                        (ps.flags & proto::kPfVaulting) ? "vaults" : "climbs", (unsigned)ps.flags,
                        issued ? "issued once; correction and velocity are suspended until the flag falls"
                               : "could not be called: the ped is placed on the pose instead");
            }
        }
        if (climbBefore == sync::ClimbWindow::Phase::Pending && pe.climb.CurrentPhase() == sync::ClimbWindow::Phase::Active)
            ++g_v4.climbStarted;                       // 0d: the puppet really climbs / vaults
        if (climbBefore != sync::ClimbWindow::Phase::Fallback && pe.climb.CurrentPhase() == sync::ClimbWindow::Phase::Fallback)
        {
            ++g_v4.climbFallbacks;
            static int fallbackLogged = 0;
            if (fallbackLogged < 8)
            {
                ++fallbackLogged;
                PeerLog(true, "engine: remote peer id=%d (%s): the puppet is not climbing 300 ms after TASK_CLIMB: placed on the "
                        "pose with its collision off until the sender's flag falls", id, pe.name.c_str());
            }
        }
        if (cw.reset)
            LocoReset(pe, kSiteClimbEnd);              // the climb task replaced the gait task: issue it again (then drive on)
        if (parachute || cw.fallbackPlace)
        {
            if (!pe.collisionOff)
            {
                ClearPedTasksNow(pe.ped);              // no gait task: the game's own fall / stand behaviour takes over
                g_world->SetEntityCollision(pe.ped, false);
                pe.collisionOff = true;
                pe.aimTask = false;
            }
            PlacePeer(pe, pose);
            return;
        }
        if (cw.suspend)
            return;                                    // TASK_CLIMB owns the ped: no gait, no velocity, no correction

        // Aiming (kPfAiming, not while ragdolling): an aim task replaces the gait task. Its end hands
        // the ped back to the controller with a Reset (the gait task re-issued at once; no snap, the
        // ped is where the aim left it).
        // W8 item 8: no aim task without a weapon (the alt-tab state used to send weapon 0 with the flags kept or dropped):
        // a stale aim task is cleared the frame the aim is no longer wanted, and the gait task replaces it.
        const bool aiming = sync::AimWanted(ps.flags, ps.weapon);
        if (!aiming && pe.aimTask)
        {
            pe.aimTask = false;
            LocoReset(pe, kSiteAimEnd);
        }

        sync::LocoInput in;
        in.pos = { pose.k.x, pose.k.y, pose.k.z };
        in.vel = { pose.k.vx, pose.k.vy, pose.k.vz };
        in.heading = pose.k.heading;
        in.teleported = pose.teleported;
        if (!g_world->GetEntityCoords(pe.ped, in.pedPos.x, in.pedPos.y, in.pedPos.z))
        {
            PlacePeer(pe, pose);                       // cannot read the ped: keep it on the pose
            pe.loco.ResetHard();                       // and where it is is not known: snap on the next read
            return;
        }
        in.pedHeading = g_world->GetEntityHeading(pe.ped);
        const int pedGait = g_world->PedGait(pe.ped);
        in.pedGait = (sync::Gait)(pedGait >= 0 && pedGait <= 3 ? pedGait : 0);
        if (aiming)
            in.pedGait = pe.loco.CurrentGait();        // the aim tasks own the animation: its own gait is no stall signal
        in.dtMs = dtMs;
        in.moveState = (int8_t)ps.move;                // 0..3 here: the sender's gait, a hint the speed may override
        in.ragdoll = (ps.flags & proto::kPfRagdoll) != 0;
        // A fall counts as a jump: no task over it (it would cancel the fall), z from physics.
        in.jumping = (ps.flags & (proto::kPfJumping | proto::kPfFalling)) != 0;
        in.jumpSeq = ps.jumpSeq;                       // a change starts one jump, even if kPfJumping was skipped
        in.moveRateAvailable = !g_moveRateFailed;
        in.swimming = (ps.flags & proto::kPfSwimming) != 0;   // W6: a swimmer shows no land gait: not a stall
        {
            // W11 item 3: the ped's own velocity (what its animation moves it by): the controller pushes only the residual on it.
            float pvx = 0, pvy = 0, pvz = 0;
            in.havePedVel = g_world->GetEntityVelocity(pe.ped, pvx, pvy, pvz);
            in.pedVel = { pvx, pvy, pvz };
            // W11 item 2: anchor mode needs the entity; an aim task owns the walk (its target is a coordinate) and a broken
            // TASK_GO_TO_ENTITY ends it: the controller then acts as straight for the frame.
            const bool anchorWanted = g_locoAnchor.load() && !g_anchorBroken && !aiming;
            in.noAnchor = !(anchorWanted && EnsureAnchor(pe, pose, nowMs));
        }
        in.repairsDone = pe.repairsDone;                   // R3 T2
        in.swingActive = pe.swingProbe.Active();           // R4 T1: no movement task / repair while the swing's clip is being read back (task 3's interface)
        // 0.5.7 loco_swing_walk: the newest swing's clip runs on the secondary slot (upper body; not a full-body / plain clip, not the melee
        // task) AND the read-back has seen it playing (until then the hold stays, so SwingRetryDue's "a task replaced it" logic is untouched).
        // First() keeps its sighting until the next swing's Begin, so this also covers the controller's 700 ms hold after the probe.
        in.swingSecondary = pe.swingProbe.First().taken && pe.swingVariant != sync::SwingVariant::MeleeTask &&
                            !sync::SwingVariantReplacesGait(pe.swingVariant);
        // T2: a failure two puppets' controllers found is imposed on this one too (the game is one game), until its timer runs out.
        if (g_locoDegradeReset.exchange(false))
            g_locoDegrade.Reset();
        if (const double ms = g_locoDegrade.ActiveMs(sync::LocoLevel::Straight, nowMs); ms > 0.0)
            pe.loco.Degrade(sync::LocoLevel::Straight, sync::LocoLevelWhy::Shared, ms);
        if (const double ms = g_locoDegrade.ActiveMs(sync::LocoLevel::NoRate, nowMs); ms > 0.0)
            pe.loco.Degrade(sync::LocoLevel::NoRate, sync::LocoLevelWhy::Shared, ms);
        if (g_locoBlend.load())
            if (const double ms = g_locoDegrade.ActiveMs(sync::LocoLevel::Placed, nowMs); ms > 0.0)
                pe.loco.Degrade(sync::LocoLevel::Placed, sync::LocoLevelWhy::Shared, ms);   // R4 T1: two puppets found the writes ignored
        const sync::LocoOutput o = pe.loco.Update(in);
        if (o.paused)
            return;                                    // leaving a vehicle: the exit animation owns the ped
        if (o.levelChanged)
            NoteLocoLevel(pe, id, o, in, nowMs);
        if (o.verdictChanged)
            NoteLocoVerdict(pe, id, o);                    // R3 T2
        if (o.capHit && !pe.capLogged)
        {
            pe.capLogged = true;
            PeerLog(true, "engine: loco level id=%d (%s): the cap of level changes per session was reached (%u): the ladder is FROZEN at level %d (%s) for the "
                    "rest of the session (loco_r3=0 lifts the cap)", id, pe.name.c_str(), (unsigned)pe.loco.LevelChangesTotal(), (int)pe.loco.Level(),
                    LocoLevelName(pe.loco.Level()));
        }
        {
            // R3 T2: the gait diagnostic frame: what the game says the ped does next to what the pose asks
            sync::GaitDiagFrame gf;
            gf.dtMs = dtMs;
            gf.nowMs = nowMs;
            gf.commanded = aiming ? sync::Gait::Still : o.gait;      // (an aim task owns the animation: its gait read is not a signal)
            gf.read = in.pedGait;
            gf.poseSpeed = std::sqrt(in.vel.x * in.vel.x + in.vel.y * in.vel.y);
            gf.ownSpeed = pe.loco.SelfSpeed();
            gf.pedPos = in.pedPos;
            gf.wrote = o.snap || o.nudge;
            gf.issued = o.task == sync::LocoTask::GoStraight || o.task == sync::LocoTask::GoToAnchor;
            gf.blendSet = gf.issued || o.setBlend;
            gf.moveBlend = o.moveBlendRatio;
            gf.ragdoll = in.ragdoll;
            if (gf.commanded != sync::Gait::Still)
            {
                // GET_ENTITY_SPEED is the length of GET_ENTITY_VELOCITY, already read above this frame: one native less per puppet
                gf.entitySpeed = in.havePedVel ? std::sqrt(in.pedVel.x * in.pedVel.x + in.pedVel.y * in.pedVel.y + in.pedVel.z * in.pedVel.z)
                                               : g_world->GetEntitySpeed(pe.ped);
                gf.pedStill = g_world->IsPedStill(pe.ped) ? 1 : 0;
            }
            pe.gaitDiag.Observe(gf);
            // 0.5.7: the same IS_PED_STILL read and position, split by the swing cause of the frame
            sync::SwingWalkFrame sw;
            sw.dtMs = dtMs;
            sw.commandedMoving = gf.commanded != sync::Gait::Still;
            sw.pedStill = gf.pedStill;
            sw.pedPos = in.pedPos;
            sw.snapped = o.snap || in.teleported;
            sw.swingWindow = o.swingWindow;
            sw.swingThrough = o.swingThrough;
            sw.walkIssued = o.swingWalk;
            pe.swingWalkDiag.Observe(sw);
            if (o.swingWalk && pe.swingProbe.Active() && pe.swingWalkClipAtMs < 0.0)
                pe.swingWalkClipAtMs = nowMs;               // the swing tick reads the clip >= 100 ms later: did our gait task cut it?
        }
        if (o.repair != sync::LocoRepair::None)
            ApplyLocoRepair(pe, id, o, in, nowMs);         // R3 T2 (after the diagnostic: the frame is judged on the ped it was read from)
        {
            // Sync A: the slide metric (libs/sync LocoDiag), from what this frame read and what the controller decided
            sync::LocoDiagFrame df;
            df.dtMs = dtMs;
            df.nowMs = nowMs;
            df.pose = in.pos;
            df.pedPos = in.pedPos;
            df.poseSpeed = std::sqrt(in.vel.x * in.vel.x + in.vel.y * in.vel.y);
            df.gait = o.gait;
            df.pushed = o.setVelocity;
            df.push = o.velocity;
            df.pedVel = in.pedVel;
            df.havePedVel = in.havePedVel;
            df.anchorMode = o.anchorMode && o.moveAnchor;
            df.anchor = o.anchorPos;
            df.nudged = o.nudge && !o.snap;                // T2
            df.nudge = o.nudgeDelta;
            df.rateSet = o.setMoveRate;
            df.rate = o.moveRate;
            df.rateClamped = o.rateClamped;
            df.selfSpeed = pe.loco.SelfSpeed();
            df.poseVel = in.vel;                           // R4 T1
            df.snapped = o.snap;
            df.blend = o.blend;
            df.blendPlaced = o.level == sync::LocoLevel::Placed;
            df.pedHeading = in.pedHeading;                 // R5 telemetry: GET_ENTITY_HEADING of the puppet against the sender's heading
            df.poseHeadingDeg = in.heading;
            df.haveHeading = sync::HeadingTelemetryFrame(in.ragdoll, in.jumping, in.swimming, aiming);   // 0.4.30: was always true
            pe.slide.Observe(df);
        }

        const int32_t ped = pe.ped;
        if (o.task == sync::LocoTask::GoStraight)
        {
            pe.lastTarget = o.target;
            pe.haveLastTarget = true;
            pe.lastMbr = o.moveBlendRatio;
        }
        if (o.moveAnchor && pe.anchor)
            g_world->SetEntityCoordsNoOffset(pe.anchor, o.anchorPos.x, o.anchorPos.y, o.anchorPos.z);   // W11: every frame
        if (o.nudge && !o.snap)
        {
            // T2: the position error closed by a coordinate write of a few millimetres (exact in every model of what the game does
            // with SET_ENTITY_VELOCITY on a ped under a task). z stays the ped's own unless the controller asked for a vertical step.
            g_world->SetEntityCoordsNoOffset(ped, in.pedPos.x + o.nudgeDelta.x, in.pedPos.y + o.nudgeDelta.y,
                                             in.pedPos.z + o.nudgeDelta.z);
            ++g_locoNudgeCalls;
        }
        if (o.snap)
        {
            g_world->SetEntityCoordsNoOffset(ped, o.snapPos.x, o.snapPos.y, o.snapPos.z);
            g_world->SetEntityHeading(ped, o.setHeading ? o.heading : Heading360(pose.k.heading));
        }
        if (o.ragdollStart)
        {
            g_world->SetPedCanRagdoll(ped, true);      // SpawnRemotePed spawns it with ragdoll off
            g_world->SetPedToRagdoll(ped, 50000, 60000); // held until ragdollEnd clears it
            ArmOneShot(pe, sync::OneShot::Ragdoll, nowMs);
        }
        if (o.ragdollEnd)
        {
            ClearPedTasksNow(ped);
            g_world->SetPedCanRagdoll(ped, false);
            pe.aimTask = false;                        // cleared with the ragdoll
        }
        if (o.jumpStart)
        {
            g_world->TaskJump(ped);
            ArmOneShot(pe, sync::OneShot::Jump, nowMs);
        }
        if (aiming && !in.ragdoll)
        {
            // The aim point: the sender's offset (PedState.aim*) from the shown position. Every issue
            // restarts the aim blend, so sync::AimGate allows one only when no aim task runs, the point
            // moved (> 1 m, more for a far point), the shown gait changed, or the controller re-issued
            // its go-straight (the walk-aim task carries that target); never over a jump or a fall
            // (one starting now, kPfJumping, kPfFalling).
            const float ax = pose.k.x + ps.aimX, ay = pose.k.y + ps.aimY, az = pose.k.z + ps.aimZ;
            if (pe.aimGate.ShouldIssue({ ax, ay, az }, in.pedPos, o.gait, pe.aimTask,
                                       o.task == sync::LocoTask::GoStraight, o.jumpStart || in.jumping))
            {
                if (o.gait != sync::Gait::Still && pe.haveLastTarget)
                    g_world->TaskGoToCoordWhileAimingAtCoord(ped, pe.lastTarget.x, pe.lastTarget.y, pe.lastTarget.z,
                                                             ax, ay, az, pe.lastMbr, false);
                else
                    g_world->TaskAimGunAtCoord(ped, ax, ay, az, -1);
                pe.aimTask = true;
            }
        }
        else
        {
            switch (o.task)
            {
            case sync::LocoTask::None:
                break;
            case sync::LocoTask::StandStill:
                g_world->TaskStandStill(ped, o.standStillMs);
                break;
            case sync::LocoTask::GoStraight:
                // targetHeading 0.0, slide 0.0 as every reference passes them: GTA Network
                // (Client/Sync/Entity/OnFoot.cs WalkAnimation, platform SyncPed.cs), RAGECOOP-V
                // (SyncedPed.cs WalkTo) and ScriptHookVDotNet Tasks.GoTo(ignorePaths). The native
                // docs (alloc8or) name the param but say nothing of a "no final heading" value; no
                // source we have uses 40000. The target sits 1 s ahead and is re-issued before the
                // ped reaches it, so the arrival heading is never played out anyway.
                g_world->TaskGoStraightToCoord(ped, o.target.x, o.target.y, o.target.z, o.moveBlendRatio, -1, 0.0f,
                                               0.0f);
                g_world->SetPedDesiredMoveBlendRatio(ped, o.moveBlendRatio);
                break;
            case sync::LocoTask::GoToAnchor:
                // W11: ONE task for as long as the ped keeps walking; the anchor (placed above) moves every frame.
                // TASK_GO_TO_ENTITY(ped, anchor, duration -1, distance 0.1, blend, slowDownDistance 0, flags 0).
                if (pe.anchor && g_world->TaskGoToEntity(ped, pe.anchor, -1, 0.1f, o.moveBlendRatio, 0.0f, 0))
                    g_world->SetPedDesiredMoveBlendRatio(ped, o.moveBlendRatio);
                else
                {
                    g_anchorBroken = true;
                    PeerLog(true, "engine: TASK_GO_TO_ENTITY could not be called (anchor %d): remote walking falls back to "
                            "TASK_GO_STRAIGHT_TO_COORD (loco_mode=straight) for this run", pe.anchor);
                    LocoReset(pe, kSiteAnchorLost);
                }
                break;
            }
            if (o.setBlend)                            // W11: a Walk / Run / Sprint change rides on the running task
                g_world->SetPedDesiredMoveBlendRatio(ped, o.moveBlendRatio);
        }
        // Heading. Standing still nothing else turns the ped, so it is set outright. While a gait task
        // runs, GTA's motion task steers the ped toward its target every frame and blends the turn
        // into the walk/run clips; a SET_ENTITY_HEADING each frame would rotate the capsule against
        // it (the two fight: twitching, sliding sideways, "moonwalk"). SET_PED_DESIRED_HEADING feeds
        // the same turn system instead (the controller's author flagged this risk). Not on a snap
        // frame: the heading was just set with it. Not while an aim task runs: it faces the aim point.
        if (o.setHeading && !o.snap && !pe.aimTask)
        {
            if (o.gait == sync::Gait::Still)
                g_world->SetEntityHeading(ped, o.heading);
            else if (g_locoBlend.load() && g_locoFace.load())
            {
                // 0.4.29 (loco_face): the Blend carries the position by the velocity, so the facing is ours as well (FiveM's ped blender)
                g_world->SetEntityHeading(ped, o.heading);
                g_world->SetPedDesiredHeading(ped, Heading360(pose.k.heading));
            }
            else
                g_world->SetPedDesiredHeading(ped, o.heading);
        }
        if (o.setMoveRate)
        {
            // Animation speed = pose speed / the gait's nominal speed. The native holds only while it is
            // called, so every frame (not just when a task is issued). If it cannot be called at all, the
            // controller moves to its quantised move blend ratio.
            if (!g_world->SetPedMoveRateOverride(ped, o.moveRate))
            {
                g_moveRateFailed = true;
                PeerLog(true, "engine: SET_PED_MOVE_RATE_OVERRIDE could not be called: remote walk / run animation "
                        "speed falls back to the move blend ratio");
            }
        }
        if (o.setVelocity)
        {
            float vz = o.velocity.z;
            bool ok = true;
            if (o.velocityZFromPhysics)
            {
                float cx = 0, cy = 0, cz = 0;
                ok = g_world->GetEntityVelocity(ped, cx, cy, cz); // a jump's own z: never overwrite it
                vz = cz;
            }
            if (ok)
                g_world->SetEntityVelocity(ped, o.velocity.x, o.velocity.y, vz);
        }
    }

    const char* PoseModeName(sync::PoseMode m)
    {
        switch (m)
        {
        case sync::PoseMode::Buffering:     return "buffering";
        case sync::PoseMode::Interpolating: return "interpolating";
        case sync::PoseMode::Extrapolating: return "extrapolating";
        case sync::PoseMode::Holding:       return "holding";
        }
        return "?";
    }

    // ---- the hand-over between the join worker's maps and the game thread's (sync plan N6a) ------------------

    // Script tick, once per frame, at the head of TickPeers: under g_peersMtx, takes over what the join worker wrote
    // (names, looks, deaths, emotes, despawns, presence and states of vehicles) into the game thread's own entries,
    // prunes the silent peers out of the smoother and samples it for every peer at the tick's time, then lets go of
    // the lock BEFORE the first native call. Copies, counter compares and smoother arithmetic only. `nowMs` is the
    // tick's clock, read after the lock was won (the wait must not age a sample); `reportNow` says this tick writes
    // the 10 s sync lines (their smoother numbers are copied here too, the smoother belongs to the lock).
    void SyncNet(double& nowMs, bool& reportNow, double& lastReportMs, double reportEveryMs, std::vector<uint16_t>& forgotten)
    {
        PeersLock lk(g_peersMtx, true);
        nowMs = NowMs();
        reportNow = nowMs - lastReportMs >= reportEveryMs;
        if (reportNow)
            lastReportMs = nowMs;
        s_syncLive = g_syncLive;
        s_frameMs = nowMs;
        s_pipeClock.have = false;
        if (reportNow && g_syncLive && g_sync.Clock().Ready())
        {
            sync::NetClock& clk = g_sync.Clock();
            s_pipeClock.have = true;
            s_pipeClock.frameGaps = clk.TakeFrameGaps();
            s_pipeClock.baselineDriftMs = clk.TakeBaselineDriftMs();
            s_pipeClock.stepMaxMs = clk.TakeBaselineStepMaxMs();
            s_pipeClock.riseMaxMs = clk.TakeDelayRiseMaxMs();
            s_pipeClock.delayMs = clk.DelayMs();
            s_pipeClock.targetMs = clk.TargetDelayMs();
            s_pipeClock.excessP95Ms = clk.ExcessP95Ms();
            s_pipeClock.ageP95Ms = clk.AgeP95Ms();
            s_pipeClock.stats = clk.Stats();
        }
        forgotten.clear();
        if (g_syncLive)
            g_sync.Prune(nowMs, &forgotten);

        for (auto it = g_peerNet.begin(); it != g_peerNet.end();)
        {
            const int id = it->first;
            PeerNet& n = it->second;
            auto pit = g_peers.find(id);
            if (pit == g_peers.end())
            {
                if (n.gone)
                {
                    it = g_peerNet.erase(it);          // never had an entry here: nothing for the tick to clean up
                    continue;
                }
                pit = g_peers.emplace(id, Peer()).first;
                pit->second.netEpoch = n.epoch;
            }
            Peer& pe = pit->second;
            if (pe.name != n.name)
                pe.name = n.name;
            if (pe.netEpoch != n.epoch)
            {
                // A new player took the id before this tick removed the old one (OnJoined): none of the old
                // player's look or death carries over (its emote was ended by the bumped emoteSeq).
                pe.netEpoch = n.epoch;
                pe.hasLook = pe.lookDirty = pe.lookModelFailed = false;
                pe.hasFace = pe.faceDirty = false;     // 0.5.9
                pe.killedAtMs = 0;
                // ... and its ped and model are the old player's: OnJoined's id-reuse branch asked for a despawn, but
                // OnWorld clears that request when the new player shows up in relevance, so ask again here (W4 review).
                pe.despawn = true;
                pe.respawnPed = true;
            }
            if (pe.netLookSeq != n.lookSeq)
            {
                pe.netLookSeq = n.lookSeq;
                pe.look = n.look;
                pe.hasLook = n.hasLook;
                pe.lookDirty = true;
                pe.lookFailLogged = false;
                pe.lookModelFailed = false;
            }
            if (pe.netFaceSeq != n.faceSeq)
            {
                pe.netFaceSeq = n.faceSeq;             // 0.5.9: a new character face
                pe.face = n.face;
                pe.hasFace = n.hasFace;
                pe.faceDirty = n.hasFace;
            }
            if (n.killedAtMs != 0)
            {
                pe.killedAtMs = n.killedAtMs;
                n.killedAtMs = 0;
            }
            if (pe.netEmoteSeq != n.emoteSeq)
            {
                pe.netEmoteSeq = n.emoteSeq;
                pe.emoteSeq = n.emoteSeq;
                pe.emoteKind = n.emoteKind;
                pe.emoteAtMs = n.emoteAtMs;
                pe.emoteIn = n.emoteIn;
                pe.scenarioIn = n.scenarioIn;
            }
            if (n.reactDamage)
            {
                pe.reactDamage += n.reactDamage;
                pe.reactFlags = (uint8_t)(pe.reactFlags | n.reactFlags);
                pe.reactBone = n.reactBone;
                pe.reactAttacker = n.reactAttacker;
                n.reactDamage = 0;
                n.reactFlags = 0;
            }
            if (n.despawn)
            {
                pe.despawn = true;
                n.despawn = false;
            }
            pe.gone = n.gone;
            pe.poseOk = false;
            pe.haveSyncStats = false;
            if (!n.gone && g_syncLive && g_sync.Sample((uint16_t)id, nowMs, pe.pose))
            {
                pe.poseOk = true;
                if (reportNow)
                    if (const sync::PlayerSmoother* ps = g_sync.Find((uint16_t)id))
                    {
                        pe.haveSyncStats = true;
                        pe.syncDelayMs = ps->DelayMs();
                        pe.syncJitterMs = ps->JitterMs();
                        pe.syncStats = ps->Stats();
                        pe.pipeExtraMs = ps->ExtraMs();
                        pe.pipeClass = ps->ClassHeld();
                        g_sync.TakeDiagnostics((uint16_t)id, pe.pipeArrivals, pe.pipeLongestExtrapMs);
                    }
            }
            ++it;
        }

        for (auto& kv : g_vehNet)
        {
            auto vit = g_vehs.find(kv.first);
            if (vit == g_vehs.end())
                vit = g_vehs.emplace(kv.first, NetVeh()).first;
            NetVeh& v = vit->second;
            const VehNet& n = kv.second;
            v.present = n.present;
            v.removed = n.removed;
            v.haveState = n.haveState;
            v.ctrlSeq = n.ctrlSeq;
            v.driveResetSeq = n.driveResetSeq;
            if (v.stVersion != n.stVersion)
            {
                v.st = n.st;
                v.stVersion = n.stVersion;
            }
        }
    }

    // Script tick, after it removed entries from g_peers / g_vehs: drops the net-side entries too, unless the join
    // worker has brought them back to life since SyncNet (a new player on the id, a vehicle presented again), in
    // which case the next SyncNet makes a fresh entry from them.
    void CommitNet(const std::vector<int>& peersDone, const std::vector<uint16_t>& vehsDone)
    {
        if (peersDone.empty() && vehsDone.empty())
            return;
        PeersLock lk(g_peersMtx, true);
        for (int id : peersDone)
        {
            auto it = g_peerNet.find(id);
            if (it != g_peerNet.end() && it->second.gone)
                g_peerNet.erase(it);
        }
        for (uint16_t id : vehsDone)
        {
            auto it = g_vehNet.find(id);
            if (it != g_vehNet.end() && (it->second.removed || !it->second.present || !g_syncLive))
                g_vehNet.erase(it);
        }
    }

    // A vehicle's interpolator belongs to the join worker's side (it pushes snapshots into it): the tick samples and
    // clears it under the lock, a few lines each.
    // The car is drawn at the connection's render clock (the one the peds read: sync plan W7) minus its class extra,
    // (whole ticks its snapshots are spaced beyond one, slewed): `nowMs` is the tick's time, so a ped and a car of
    // one frame show the same instant. False: no snapshot yet, or the clock has heard no frame this session.
    //
    // W9 item 6 (present-time prediction): with a `lead` (the car's sync::PresentLead) the car is drawn `lead` ms later
    // on that timeline, up to the present: the delay and the class extra it is otherwise behind by, weighted 1 within
    // 30 m of the local player and 0 beyond 45 m (`distM`), the interpolator extrapolating past its newest snapshot
    // by the velocity (arc, capped at 500 ms). `leadMs` returns what was added. dtMs is the frame gap for the slew.
    //
    // R3 T5 (docs/sync-specs/vehicle-position.md): with veh_near_clock (default) a near copy is drawn at NetClock::NearT + the link lag instead
    // (sync::NearLeadWantMs): the lead takes the difference to the delayed clock, so neither the shared delay (it rises and falls) nor the class
    // extra moves it, and the stamp's missing half round trips (the owner's uplink, our downlink) are added. `ctrl` is the controller's player id
    // (its round trip comes from the server's LinkInfo; ours from the session). `ideal` (optional) is the owner's report sampled at the same
    // instant plus the lag: the diagnostic's reference; `lagOut` the link lag, `unledOut` how far the interpolation time is behind it.
    bool SampleVehInterp(uint16_t id, double nowMs, sync::VehicleSample& out, sync::PresentLead* lead = nullptr,
                         double distM = 1e9, double dtMs = 0.0, double* leadMs = nullptr, double* dispOut = nullptr,
                         uint16_t ctrl = proto::kNoPlayer, sync::VehicleSample* ideal = nullptr, double* lagOut = nullptr, double* unledOut = nullptr)
    {
        PeersLock lk(g_peersMtx, true);
        if (!g_syncLive || !g_sync.Clock().Ready())
            return false;
        const auto it = g_vehNet.find(id);
        if (it == g_vehNet.end())
            return false;
        VehNet& v = it->second;
        sync::NetClock& clock = g_sync.Clock();
        const double extra = v.extra.UpdateGap(v.interp.TypicalGapMs(), clock.TickMs(), nowMs, clock.Config().maxSlew);
        const double renderT = clock.RenderT(nowMs);
        const double nearT = clock.NearT(nowMs);
        const bool nearOn = g_nearClock.load(std::memory_order_relaxed);
        double lagMs = 0.0;
        {
            // the lag the stamp does not carry: half our round trip (the fastest downlink) + half the controller's (the fastest uplink SampleClock absorbs)
            double ownerRtt = 0.0;
            {
                std::lock_guard<std::mutex> llk(g_linkMtx);
                const auto rt = g_linkRtt.find(ctrl);
                if (rt != g_linkRtt.end()) ownerRtt = (double)rt->second;
            }
            if (ownerRtt <= 0.0 && ctrl != proto::kNoPlayer && lagOut) ++g_t5.ownerRttUnknown;
            lagMs = sync::LinkLagMs((double)join::RttMs(), ownerRtt);
        }
        double add = 0.0;
        if (lead)
        {
            lead->SetConfig(nearOn ? sync::NearLeadConfig() : sync::PresentLeadConfig());
            const double want = nearOn ? sync::NearLeadWantMs(nearT, lagMs, renderT, extra) : clock.DelayMs() + extra;
            add = lead->Update(distM, want, dtMs);
        }
        if (leadMs)
            *leadMs = add;
        v.interp.SetExtrapolateMaxMs(sync::VehicleInterpConfig().extrapolateMaxMs + (nearOn ? add : 0.0));   // R3 T5: the cap grows with the lead
        v.interp.SetAccelExtrapolation(g_accelExtrap.load(std::memory_order_relaxed));                         // R4 T2
        v.interp.SetOmegaGuard(g_omegaGuard.load(std::memory_order_relaxed));                                 // 0.4.30
        const double disp = renderT - extra + add;
        if (dispOut)
            *dispOut = disp;
        if (lagOut)
            *lagOut = lagMs;
        if (unledOut)
            *unledOut = nearT + lagMs - disp;
        if (ideal)
            v.interp.Sample(nearT + lagMs, *ideal);
        return v.interp.Sample(disp, out);
    }
    // T4: the copy's own response to a hit becomes its trajectory from `snap.tMs` on (VehicleInterpolator::ReplaceFrom); the stream's pushes are
    // refused until they show the impact (preVel = the stream's velocity before it) or guardMs have passed.
    void AdoptVehInterp(uint16_t id, const sync::VehicleSnapshot& snap, double guardMs, const sync::DriveVec& preVel)
    {
        PeersLock lk(g_peersMtx, true);
        const auto it = g_vehNet.find(id);
        if (it != g_vehNet.end())
            it->second.interp.ReplaceFrom(snap, guardMs, &preVel);
    }
    uint32_t VehInterpGuarded(uint16_t id)
    {
        PeersLock lk(g_peersMtx, true);
        const auto it = g_vehNet.find(id);
        return it != g_vehNet.end() ? it->second.interp.Stats().guarded : 0u;
    }
    // R3 T5: the vehicles' interpolator totals over every copy (session totals; the position line prints the 10 s deltas): snapshots stored, identical or
    // earlier-placed resends dropped, stale ones, teleports.
    void VehInterpTotals(uint32_t& accepted, uint32_t& repeats, uint32_t& stale, uint32_t& teleports, uint32_t& omegaStill, uint32_t& omegaCross)
    {
        accepted = repeats = stale = teleports = omegaStill = omegaCross = 0;
        PeersLock lk(g_peersMtx, true);
        for (const auto& kv : g_vehNet)
        {
            const sync::VehicleInterpStats& s = kv.second.interp.Stats();
            accepted += s.accepted;
            repeats += s.repeats;
            stale += s.stale;
            teleports += s.teleports;
            omegaStill += s.omegaStill;
            omegaCross += s.omegaCross;
        }
    }
    void ClearVehInterp(uint16_t id)
    {
        PeersLock lk(g_peersMtx, true);
        const auto it = g_vehNet.find(id);
        if (it != g_vehNet.end())
            it->second.interp.Clear();
    }

    // Remote players, once per script tick (never on the render-thread fallback): forget the
    // silent ones, delete the peds of the gone / out-of-relevance ones, spawn a ped at a peer's
    // first smoothed sample and drive it every frame after. Every 10 s one compact sync line per
    // peer (at most 8) goes to gage.log. No lock is held here: SyncNet took what the join worker wrote and
    // sampled the smoother, and everything below works on the game thread's own entries.
    void TickPeers(int32_t lp, int32_t lpVeh)
    {
        static std::vector<uint16_t> forgotten;       // script tick only; reused, no per-frame allocation
        static std::vector<std::string> report;
        static std::vector<int> removedPeers;
        static double lastReportMs = 0;
        constexpr double kReportEveryMs = 10000.0;
        constexpr size_t kReportMaxPeers = 8;
        report.clear();
        removedPeers.clear();
        const float lpVehSpeed = lpVeh ? g_world->GetEntitySpeed(lpVeh) : 0.0f;   // W9 item 6: once per frame, for the impact check
        {
            double nowMs = 0;
            bool reportNow = false;
            SyncNet(nowMs, reportNow, lastReportMs, kReportEveryMs, forgotten);
            for (uint16_t id : forgotten)
            {
                auto it = g_peers.find(id);
                if (it != g_peers.end() && it->second.ped)
                {
                    it->second.despawn = true;
                    PeerLog(false, "engine: remote peer id=%d (%s) silent for 5 s -- ped despawned", (int)id,
                            it->second.name.c_str());
                }
            }
            size_t reported = 0, withData = 0;
            if (reportNow && s_pipeClock.have && !g_peers.empty())
            {
                const PipeClockReport& pc = s_pipeClock;
                char line[600];
                std::snprintf(line, sizeof(line),
                              "engine: pipe clock: %u world/vehicle frames in the last 10 s (arrival gap ms mean %.0f p95 %.0f max %.0f), clock offset moved %+.1f ms, "
                              "%u steps over 5 ms (largest %.0f ms), %u delay rises (%u over 5 ms, mean %.1f ms, largest %.1f ms), %u reordered frames, %u resyncs; shared delay %.0f ms (target %.0f), lateness p95 "
                              "excess %.0f + age %.0f ms",
                              pc.frameGaps.n + 1, pc.frameGaps.MeanMs(), pc.frameGaps.PercentileMs(0.95), pc.frameGaps.maxMs, pc.baselineDriftMs,
                              pc.stats.baselineSteps - pc.seen.baselineSteps, pc.stepMaxMs, pc.stats.delayRises - pc.seen.delayRises,
                              pc.stats.delayRisesBig - pc.seen.delayRisesBig,
                              pc.stats.delayRises > pc.seen.delayRises ? (pc.stats.delayRiseSumMs - pc.seen.delayRiseSumMs) / (pc.stats.delayRises - pc.seen.delayRises) : 0.0,
                              pc.riseMaxMs,
                              pc.stats.reorderedFrames - pc.seen.reorderedFrames, pc.stats.resyncs - pc.seen.resyncs, pc.delayMs, pc.targetMs,
                              pc.excessP95Ms, pc.ageP95Ms);
                report.emplace_back(line);
                s_pipeClock.seen = pc.stats;
            }
            for (auto it = g_peers.begin(); it != g_peers.end();)
            {
                Peer& pe = it->second;
                if (pe.gone)
                {
                    DeleteAnchor(pe);
                    if (pe.ped)
                        g_world->DeleteEntity(pe.ped);
                    SyncEmoteDict(pe.emoteDict, false, std::string());   // its ped is gone: free the dictionary
                    ReleaseRequest(pe);
                    removedPeers.push_back(it->first);
                    g_seats.DropPlayer(it->first);          // T1: whatever it was walking to is moot
                    it = g_peers.erase(it);
                    continue;
                }
                if (pe.despawn || pe.respawnPed)
                {
                    // respawnPed (v4): a death ended, or the ped died on this machine only. It is its
                    // own flag: OnWorld clears `despawn` with every WorldState that lists the peer.
                    pe.despawn = false;
                    pe.respawnPed = false;
                    DeleteAnchor(pe);
                    if (pe.ped)
                        g_world->DeleteEntity(pe.ped);
                    pe.ped = 0;                        // DeleteEntity zeroes it; also when the call failed
                    pe.lastDriveMs = 0;
                    pe.spawnSinceMs = 0;
                    SyncEmoteDict(pe.emoteDict, false, std::string());   // a looping emote re-acquires on the new ped
                    ReleaseRequest(pe);
                }
                // The stand-in for a look whose model did not load: every 30 s ask for the real model
                // again in the background (the stand-in stays visible meanwhile). Loaded -> the
                // model-change path below swaps the ped; still not loaded after 5 s -> released,
                // next try in 30 s.
                if (pe.ped && pe.hasLook && pe.lookModelFailed && nowMs - pe.lookFailedAtMs >= 30000.0)
                {
                    if (pe.requestedModel != pe.look.model)
                    {
                        ReleaseRequest(pe);
                        g_world->RequestModel(pe.look.model);
                        pe.requestedModel = pe.look.model;
                    }
                    if (g_world->HasModelLoaded(pe.look.model))
                    {
                        pe.lookModelFailed = false;
                        pe.lookDirty = true;
                        PeerLog(false, "engine: remote peer id=%d (%s): model 0x%08x loaded on retry", it->first,
                                pe.name.c_str(), (unsigned)pe.look.model);
                    }
                    else if (nowMs - pe.lookFailedAtMs >= 35000.0)
                    {
                        ReleaseRequest(pe);
                        pe.lookFailedAtMs = nowMs;
                    }
                }
                // A new look: same model -> put it on; another model -> the ped is respawned with it
                // below (a ped cannot change model; ApplyPedLook refuses a mismatch).
                if (pe.ped && pe.lookDirty && pe.hasLook)
                {
                    if (pe.look.model == pe.pedModel)
                        ApplyPeerLook(pe, it->first);
                    else if (!pe.lookModelFailed)
                    {
                        PeerLog(false, "engine: remote peer id=%d (%s) changed model 0x%08x -> 0x%08x: respawning its ped",
                                it->first, pe.name.c_str(), (unsigned)pe.pedModel, (unsigned)pe.look.model);
                        DeleteAnchor(pe);
                        g_world->DeleteEntity(pe.ped);
                        pe.ped = 0;
                        pe.lastDriveMs = 0;
                        pe.spawnSinceMs = 0;
                        SyncEmoteDict(pe.emoteDict, false, std::string());
                    }
                    else
                        pe.lookDirty = false;          // its model did not load: the stand-in stays for now
                }
                // 0.5.9: only the face changed (a character switch or edit with the same look), or the ped has no look to wait for.
                if (pe.ped && pe.faceDirty && !pe.lookDirty)
                    ApplyPeerFace(pe, it->first);
                if (!pe.poseOk)
                {
                    // No data (yet), out of relevance or silent. A spawn in progress stops here: its
                    // model request is handed back and its 5 s clock restarts when the peer returns
                    // (a peer that left mid-load must not come back already "failed").
                    if (!pe.ped && pe.spawnSinceMs)
                    {
                        pe.spawnSinceMs = 0;
                        ReleaseRequest(pe);
                    }
                    ++it;
                    continue;
                }
                const sync::Pose pose = pe.pose;       // SyncNet's sample for this tick (a copy: DrivePeer works on `pe`)
                if (!pe.ped)
                {
                    if (!pe.loggedSeen)
                    {
                        pe.loggedSeen = true;
                        PeerLog(false, "engine: remote peer id=%d (%s) first sample %.1f, %.1f, %.1f heading %.0f (%s) "
                                "-- spawning", it->first, pe.name.c_str(), pose.k.x, pose.k.y, pose.k.z,
                                Heading360(pose.k.heading), PoseModeName(pose.mode));
                    }
                    // The peer's own model once its look is known (else the freemode stand-in). A
                    // model that has not streamed in after 5 s (bad hash, not in this build) falls
                    // back to the stand-in; the retry above tries it again every 30 s.
                    if (!pe.spawnSinceMs)
                        pe.spawnSinceMs = nowMs;
                    if (pe.hasLook && !pe.lookModelFailed && pe.look.model != (uint32_t)kCompanionModel &&
                        nowMs - pe.spawnSinceMs > 5000.0)
                    {
                        pe.lookModelFailed = true;
                        pe.lookFailedAtMs = nowMs;
                        PeerLog(true, "engine: remote peer id=%d (%s): model 0x%08x did not load in 5 s; showing the "
                                "stand-in, retrying every 30 s", it->first, pe.name.c_str(), (unsigned)pe.look.model);
                    }
                    const uint32_t model = pe.hasLook && !pe.lookModelFailed ? pe.look.model : (uint32_t)kCompanionModel;
                    if (pe.requestedModel != model)
                        ReleaseRequest(pe);            // the model we were waiting for is no longer the one
                    pe.requestedModel = model;         // SpawnRemotePed requests it every call
                    // Returns 0 until the model has streamed in: tried again next frame at the new pose.
                    pe.ped = g_world->SpawnRemotePed((int)model, pose.k.x, pose.k.y, pose.k.z,
                                                     Heading360(pose.k.heading), pe.recipe);
                    if (pe.ped)
                    {
                        PeerLog(pe.recipe != 0, "engine: spawned remote peer id=%d (%s) as ped %d (model 0x%08x%s, creation recipe %d: %s)", it->first,
                                pe.name.c_str(), pe.ped, (unsigned)model, pe.hasLook ? ", own look" : ", no look yet", pe.recipe,
                                pe.recipe == 0 ? "full flags" : pe.recipe == 1 ? "LEAN: invincible + events blocked only" : "ALT: civilian ped type, no script-host flag, lean");
                        pe.requestedModel = 0;          // SpawnRemotePed released it after CREATE_PED
                        pe.pedModel = model;
                        pe.spawnSinceMs = 0;
                        pe.loco.ResetForNewPed();       // a new ped: snap, re-issue, fresh jump / ragdoll edges
                        pe.swingWalkDiag.Break();       // 0.5.7: no position delta across a respawn
                        pe.weaponShown = 0;             // a fresh ped holds nothing yet
                        pe.aimTask = false;
                        pe.haveLastTarget = false;
                        pe.lastMove = proto::kMoveIdle;
                        SetupPeerPed(pe, it->first);    // v4: hit pool, shot / death / seat / emote edges
                        pe.faceDirty = pe.hasFace;      // 0.5.9: a fresh ped has the default head (ApplyPeerLook or the face path puts it on)
                        if (pe.hasLook && model == pe.look.model)
                            ApplyPeerLook(pe, it->first);
                        else
                            pe.lookDirty = pe.hasLook && !pe.lookModelFailed;
                    }
                    pe.lastDriveMs = 0;
                    ++it;
                    continue;                          // spawned at this pose: driven from the next frame
                }
                if (!g_world->EntityExists(pe.ped))
                {
                    // Deleted under us (a game cleanup): forget the handle, a new ped next frame.
                    PeerLog(true, "engine: remote peer id=%d (%s): ped %d no longer exists -- respawning", it->first,
                            pe.name.c_str(), pe.ped);
                    DeleteAnchor(pe);
                    pe.ped = 0;
                    pe.lastDriveMs = 0;
                    pe.spawnSinceMs = 0;
                    ++it;
                    continue;
                }
                UpdatePeerDeath(pe, it->first, pose, nowMs);
                DrivePeer(pe, it->first, pose, pe.lastDriveMs > 0 ? nowMs - pe.lastDriveMs : 0.0, nowMs);
                pe.poseX = pose.k.x; pe.poseY = pose.k.y; pe.poseZ = pose.k.z; pe.poseKnown = true;   // R4 T3: where the owner really is, for the swing line
                ProbePeerHits(pe, it->first, lp, lpVeh, lpVehSpeed, nowMs);
                pe.lastDriveMs = nowMs;
                ++pe.samples;
                if (pose.mode == sync::PoseMode::Extrapolating) ++pe.extrapolated;
                if (pose.mode == sync::PoseMode::Holding) ++pe.held;
                if (pose.teleported) ++pe.snaps;
                if (reportNow)
                {
                    ++withData;
                    if (pe.haveSyncStats && reported < kReportMaxPeers)
                    {
                        ++reported;
                        const sync::SmootherStats& st = pe.syncStats;
                        const sync::LocoStats& ls = pe.loco.Stats();
                        char line[480];
                        std::snprintf(line, sizeof(line),
                                      "engine: sync id=%d (%s): delay %.0f ms, jitter %.0f ms, now %s; last 10 s "
                                      "%u frames (%u extrapolating, %u holding, %u snaps); total %u stored, %u "
                                      "repeats, %u stale, %u teleports, %u bridge resets, %u resyncs; loco %s: gait "
                                      "%s, last 10 s %u go-straight / %u stand-still tasks, %u snaps; total %u snaps "
                                      "(%u persistent-error), %u jumps; ped move %u flags 0x%x weapon 0x%08x%s, %u "
                                      "shooting frames",
                                      it->first, pe.name.c_str(), pe.syncDelayMs, pe.syncJitterMs,
                                      PoseModeName(pose.mode), pe.samples, pe.extrapolated, pe.held, pe.snaps,
                                      st.accepted, st.repeats, st.stale, st.teleports, st.bridgeResets, st.resyncs,
                                      pe.driveMode == kDrivePlace ? "off (place)" : "tasks",
                                      GaitName(pe.loco.CurrentGait()), ls.goStraight - pe.locoGoSeen,
                                      ls.standStill - pe.locoStandSeen, ls.snaps - pe.locoSnapsSeen, ls.snaps,
                                      ls.persistentSnaps, ls.jumps, (unsigned)pe.lastPed.move,
                                      (unsigned)pe.lastPed.flags, (unsigned)pe.lastPed.weapon,
                                      pe.aimTask ? " (aim task)" : "", pe.shootingFrames);
                        report.emplace_back(line);
                        {
                            // Sync pipeline review: the rhythm of this player's stored snapshots and what the smoother did with the rest.
                            const sync::SmootherStats& ps = pe.syncStats;
                            const sync::SmootherStats& pv = pe.pipeSeen;
                            // (a new smoother restarts its counters below the last ones seen: 0.5.5 logged "repeats 4294965501"; count from the restart)
                            const auto since = [](uint32_t now, uint32_t seen) { return now >= seen ? now - seen : now; };
                            char pl[440];
                            std::snprintf(pl, sizeof(pl),
                                          "engine: pipe id=%d (%s): %u snapshots stored in 10 s (arrival gap ms mean %.0f p95 %.0f max %.0f), repeats %u identical + %u earlier-placed, "
                                          "%u stale, %u denoised; delay %.0f ms (class %d, extra %.0f ms, %u class changes); %u extrapolating frames (longest %.0f ms), "
                                          "%u holding, %u snaps, %u bridge resets, %u resyncs",
                                          it->first, pe.name.c_str(), pe.pipeArrivals.n + 1, pe.pipeArrivals.MeanMs(), pe.pipeArrivals.PercentileMs(0.95),
                                          pe.pipeArrivals.maxMs, since(ps.repeatsIdentical, pv.repeatsIdentical), since(ps.repeatsEarlier, pv.repeatsEarlier),
                                          since(ps.stale, pv.stale), since(ps.denoised, pv.denoised), pe.syncDelayMs, pe.pipeClass, pe.pipeExtraMs,
                                          since(ps.extraChanges, pv.extraChanges), pe.extrapolated, pe.pipeLongestExtrapMs, pe.held, pe.snaps,
                                          since(ps.bridgeResets, pv.bridgeResets), since(ps.resyncs, pv.resyncs));
                            report.emplace_back(pl);
                            pe.pipeSeen = ps;
                        }
                        report.emplace_back(LocoLine(it->first, pe));   // W8: why the controller issued tasks, what was last applied
                        report.emplace_back(LocoSlideLine(it->first, pe));   // Sync A: the slide metric
                        report.emplace_back(GaitLine(it->first, pe));        // R3 T2: what the game says the ped does next to the pose
                        const std::string swl = SwingWalkLine(it->first, pe);   // 0.5.7: the still slide by cause, the clip after a walk-through
                        if (!swl.empty())
                            report.emplace_back(swl);
                    }
                    {
                        // R3 T3: one cover line per puppet with cover activity in the last 10 s (the window is taken either way)
                        const sync::CoverStats cw = pe.cover.TakeWindow();
                        const std::string cl = sync::CoverPuppetLine(it->first, pe.name, pe.cover.Mode(), pe.cover.CurrentPhase(), pe.cover.LastWhy(), cw,
                                                                     pe.cover.Stats());
                        if (!cl.empty())
                            report.emplace_back(cl);
                    }
                    {
                        const std::string el = EdgeLine(it->first, pe);   // R5: swim / ragdoll edges (empty when quiet; the window is taken either way)
                        if (!el.empty())
                            report.emplace_back(el);
                    }
                    pe.slide.Reset();
                    pe.gaitDiag.Reset();
                    pe.swingWalkDiag.ResetCounters();
                    pe.shootingFrames = 0;
                    const sync::LocoStats& ls = pe.loco.Stats();
                    pe.locoGoSeen = ls.goStraight;
                    pe.locoStandSeen = ls.standStill;
                    pe.locoSnapsSeen = ls.snaps;
                    pe.locoSeen = ls;
                    pe.anchorSpawnFails = 0;
                    pe.hardSites = sync::SiteTally<kLocoSites>();
                    pe.softSites = sync::SiteTally<kLocoSites>();
                    pe.samples = pe.extrapolated = pe.held = pe.snaps = 0;
                }
                ++it;
            }
            if (reportNow && lp && withData > 0)
            {
                // R3 T2: the same readings on the LOCAL player while it walks, to set against the puppets' "engine: gait" lines (a puppet
                // whose active task types never include what the walking player has is not running a locomotion task at all)
                const int lg = g_world->PedGait(lp);
                if (lg > 0)
                {
                    char lb[120];
                    std::snprintf(lb, sizeof(lb), "engine: gait local player (for comparison): the game reads %s, GET_ENTITY_SPEED %.2f m/s, active task types [",
                                  lg == 3 ? "sprinting" : lg == 2 ? "running" : "walking", g_world->GetEntitySpeed(lp));
                    report.emplace_back(std::string(lb) + ActiveTaskTypes(lp) + "]");
                }
            }
            if (withData > reported)
                report.emplace_back("engine: sync: " + std::to_string(withData - reported) + " more peer(s) not listed");
        }
        CommitNet(removedPeers, std::vector<uint16_t>());
        FlushPeerLog();
        for (const std::string& line : report)
            GLOGI("%s", line.c_str());
    }

    // ---- protocol v4: server vehicles (script tick) -----------------------------------------------

    const char* VehOpName(uint8_t op)
    {
        switch (op)
        {
        case 1: return "Spawn";     case 2: return "Despawn";   case 3: return "Enter";
        case 4: return "Exit";      case 5: return "SetLocked"; case 6: return "SetLights";
        case 7: return "SetColours"; case 8: return "Register";   case 9: return "Release";
        case 10: return "EnterIntent"; case 11: return "CancelIntent";
        }
        return "?";
    }

    const char* VehResultName(uint8_t code)
    {
        switch (code)
        {
        case 0: return "Ok";          case 1: return "NoVehicle";   case 2: return "SeatTaken";
        case 3: return "TooFar";      case 4: return "Locked";      case 5: return "NotAllowed";
        case 6: return "Limit";       case 7: return "RateLimited"; case 8: return "BadArg";
        case 9: return "Dead";        case 10: return "NotInVehicle"; case 11: return "NoState";
        case 12: return "NotOwner";   // protocol v5: ejects like SeatTaken (LocalVehicleLink::Ejects), RefusalText says so in chat
        }
        return "?";
    }

    // A remote ped seated in a server vehicle back on foot (the vehicle is going away, or its seat list
    // no longer has them). With `leaveVeh` (the vehicle it sits in, still alive: the player really got
    // out) sync::DecideUnseat picks the exit (W9 item 5): a parked car is left with TASK_LEAVE_VEHICLE flags 0 and
    // the locomotion paused for the exit animation (kUnseatPauseMs); a car moving faster than 3 m/s, or a sender
    // whose first on-foot sample is falling / ragdolling / jumping, is bailed out of (flag 4160, 16 = warp out when
    // that call fails), the vehicle's velocity is carried for the exit window (DrivePeer) and, when the sender is
    // already ragdolling, the locomotion is not paused at all (its ragdoll edge takes the ped and holds it, with no
    // position correction over it, until the sender's ragdoll ends). Without a vehicle, or when the ped is not in a
    // vehicle on this machine, CLEAR_PED_TASKS_IMMEDIATELY takes it out at once. Either way the next DrivePeer
    // steers it onto its pose (Reset: a snap only if it really diverged). Script tick, no lock.
    void UnseatPeer(Peer& pe, int32_t leaveVeh = 0)
    {
        if (pe.ped)
        {
            sync::UnseatIn ui;
            ui.haveVehicle = leaveVeh && g_world->EntityExists(leaveVeh) && g_world->IsPedInVehicle(pe.ped);
            if (ui.haveVehicle)
            {
                ui.vehSpeedMps = g_world->GetEntitySpeed(leaveVeh);
                ui.senderFlags = pe.lastPed.flags;
            }
            const sync::UnseatPlan plan = sync::DecideUnseat(ui);
            bool left = false;
            if (plan.mode != sync::UnseatMode::Clear)
            {
                left = g_world->TaskLeaveVehicle(pe.ped, leaveVeh, plan.leaveFlags);
                if (!left && plan.fallbackFlags >= 0)
                {
                    left = g_world->TaskLeaveVehicle(pe.ped, leaveVeh, plan.fallbackFlags);
                    if (left)
                        ++g_syncD.bailFallback;
                }
            }
            if (left)
            {
                const double now = NowMs();
                {
                    char tg[200];
                    std::snprintf(tg, sizeof(tg), "peer %s exit by %s (TASK_LEAVE_VEHICLE flags %d), vehicle %.1f m/s, locomotion paused %.0f ms",
                                  pe.name.c_str(), plan.mode == sync::UnseatMode::Bail ? "bail-out" : "climb-out", plan.leaveFlags,
                                  (double)ui.vehSpeedMps, plan.pauseMs);
                    BeginTrace(TraceProbe::Exit, pe.ped, tg);
                }
                // Sync D: which exit was used (counted per 10 s: normal climb-out / bail-out, and a bail-out that had to fall back to the warp)
                if (plan.mode == sync::UnseatMode::Bail)
                    ++g_syncD.unseatBail;
                else
                    ++g_syncD.unseatLeave;
                if (plan.pauseMs > 0.0)
                    pe.loco.Pause(plan.pauseMs);
                pe.leaveUntilMs = now + (plan.pauseMs > 0.0 ? plan.pauseMs : sync::kBailPauseMs);
                pe.carryFromMs = pe.carryOutMs = 0;
                if (plan.carryVelocity)
                {
                    float vx = 0, vy = 0, vz = 0;
                    if (g_world->GetEntityVelocity(leaveVeh, vx, vy, vz))
                    {
                        pe.carryVel[0] = vx; pe.carryVel[1] = vy; pe.carryVel[2] = vz;
                        pe.carryFromMs = now;
                    }
                    ++g_v4.bailOuts;
                    static int bailLogged = 0;
                    if (bailLogged < 8)
                    {
                        ++bailLogged;
                        PeerLog(false, "engine: vehicle: peer left a vehicle moving at %.1f m/s (sender flags 0x%x): bail-out (flags %d), "
                                "velocity carried %s, locomotion %s", ui.vehSpeedMps, (unsigned)ui.senderFlags, plan.leaveFlags,
                                pe.carryFromMs > 0.0 ? "yes" : "no (velocity unreadable)",
                                plan.pauseMs > 0.0 ? "paused for the exit" : "not paused (the sender is ragdolling)");
                    }
                }
            }
            else
            {
                ++g_syncD.unseatClear;
                ClearPedTasksNow(pe.ped);
                pe.leaveUntilMs = 0;
            }
        }
        pe.seatedVeh = -1;
        pe.seatedSeat = -1;
        pe.aimTask = false;
        LocoReset(pe, kSiteUnseat);
        ++g_v4.unseats;
    }

    void UnseatPeersIn(int id)
    {
        for (auto& kv : g_peers)
            if (kv.second.seatedVeh == id)
                UnseatPeer(kv.second);
    }

    void ResetVehicleDamage(NetVeh& v);    // R3 T4, below with the rest of the damage code
    void NoteUnreliableVisual(uint16_t id, const sync::VisualMask& f);

    void ForgetVehicleHandle(NetVeh& v)
    {
        v.handle = 0;
        v.adopted = false;
        v.spawnedModel = 0;
        v.appliedVersion = 0;
        v.appliedC1 = v.appliedC2 = v.appliedLock = v.appliedLights = v.appliedEngine = v.appliedSiren = -1;
        v.appliedBody = v.appliedEngineHp = 0xFFFF;
        v.lastDriveMs = 0;
        v.drive.Reset();
        v.vis.Reset();                 // T3: a new entity shows nothing of the server's visual state yet
        v.visReadMs = -1e9;
        v.steerPrevRotMs = -1e9;
        ResetVehicleDamage(v);         // R3 T4: ... nor of the damage state, and every baseline starts over
        v.horn.Reset();                // 0.4.19: ... nor its horn or lights
        v.lightsKeep.Reset();
        v.lockKeep.Reset();            // 0.4.22: ... nor its door lock
        v.vehClass = -2;               // a recycled id can be another model: read the class again (bike / mass / ride freedom)
        v.lastContactFlag = false;     // the new entity's first contact is a rising edge again
        v.divCheckAtMs = 0;            // a divergence check scheduled for the old entity would measure the new one
    }

    // ---- T3: vehicle visual state (Desync Overhaul round 2, protocol v6) -------------------------------------------------------
    // Burst tyres, smashed windows, brake lights / handbrake and a standing car's steering for the OTHER clients. The logic is
    // libs/sync/vehicle_visual.h (tested offline, tests/t3_vehvisual_test.cpp); this is the glue. Every native here is one of
    // the hook.h ones whose hash is still 0 (UNRESOLVED: lookup needed): a World:: wrapper then returns false, the sync module
    // treats that as "no information / not done", and nothing below changes behaviour until GI adds the hashes.
    // Control ids (INFERRED, GET_CONTROL_NORMAL(0, id)): INPUT_VEH_MOVE_LR 59 (-1 left .. +1 right), INPUT_VEH_ACCELERATE 71,
    // INPUT_VEH_BRAKE 72, INPUT_VEH_HANDBRAKE 76. The steering-axis agreement counters in the 10 s line show a wrong id or sign.
    constexpr int kCtlVehMoveLR = 59, kCtlVehAccelerate = 71, kCtlVehBrake = 72, kCtlVehHandbrake = 76, kCtlVehHeadlight = 74, kCtlVehHorn = 86;
    constexpr double kVisReadMs = 250.0;          // the controller reads its car's tyres / windows this often
    constexpr double kVisNearM = 150.0;           // a copy within this distance of us is re-checked and scanned after our shots
    struct VisStats
    {
        // tyres / windows
        uint32_t reads = 0, tyreReadOk = 0, tyreReadFail = 0, winReadOk = 0, winReadFail = 0;
        uint32_t sentUpdates = 0, sentWithTyres = 0, sentWithWindows = 0;   // controller reports (VehicleUpdates) and those showing a burst tyre / smashed window
        uint32_t partHits = 0, partHitsRefused = 0;                         // pops on copies reported as vehicle Hits, and the ones the session refused locally
        // brake lights / handbrake / steering
        uint32_t brakeSet = 0, brakeFail = 0, handSet = 0, handFail = 0, brakeBitsSent = 0, handBitsSent = 0, pedalReadFail = 0;
        uint32_t steerApplied = 0, steerApplyFail = 0;
        // 0.4.30 veh_steer_field: frames written through the field / refused by it (not a plausible angle: the bias took over), read-backs of
        // the last write (held = the game kept it a frame), our own car's field against the steering axis (sign proof), and on moving copies
        // (> 3 m/s) the brake float / handbrake byte next to the field (is the script driver holding the wheels?)
        uint32_t steerField = 0, steerFieldRefused = 0, steerFieldHeld = 0, steerFieldOverwritten = 0, signAgree = 0, signDisagree = 0;
        uint32_t copyMoving = 0, copyBrake = 0, copyHandbrake = 0;
        float steerAppliedMin = 0.0f, steerAppliedMax = 0.0f;
        double steerAppliedSum = 0.0;
        sync::SteerStats steerSent;
        double yawRatioSum = 0.0;
        uint32_t yawRatioN = 0;                                              // sender: avz (GET_ENTITY_ROTATION_VELOCITY) / the rotation-derived yaw rate
    };
    VisStats g_vis;
    // One entity as vehicle_visual.h's templates see it.
    struct VehVisualGame
    {
        int32_t h;
        bool TyreBurst(int slot, bool completely, bool& burst)
        {
            const bool ok = g_world->IsVehicleTyreBurst(h, slot, completely, burst);
            ++(ok ? g_vis.tyreReadOk : g_vis.tyreReadFail);
            return ok;
        }
        bool WindowIntact(int slot, bool& intact)
        {
            const bool ok = g_world->IsVehicleWindowIntact(h, slot, intact);
            ++(ok ? g_vis.winReadOk : g_vis.winReadFail);
            return ok;
        }
        bool BurstTyre(int slot, bool onRim) { return g_world->SetVehicleTyreBurst(h, slot, onRim, 1000.0f); }
        bool FixTyre(int slot) { return g_world->SetVehicleTyreFixed(h, slot); }
        bool SmashWindow(int slot) { return g_world->SmashVehicleWindow(h, slot); }
        bool FixWindow(int slot) { return g_world->FixVehicleWindow(h, slot); }
    };

    // One spawned server vehicle, every script frame: the visual state (tyres, windows). Cheap when nothing is due (Due()).
    // iControl: we control it - the server's bits are burst on our car once (a Hit by somebody else) and the controller's report
    // goes out in SendControlledVehicles. A copy: the server's mask is applied level-wise, and what OUR game popped on it (we shot
    // a tyre of a car we do not control) is reported to the server as a vehicle Hit (a part code in Hit.bone), kept pending for
    // 2.5 s so the round trip is not drawn as pop, fix, pop. Script tick.
    void TickVehicleVisual(NetVeh& v, uint16_t id, bool iControl, double nowMs, bool nearUs, int32_t lp)
    {
        const proto::VehicleState& st = v.st;
        const sync::VisualMask server{ st.tyres, st.tyresRim, st.windows };
        const bool shotRecent = nowMs - g_lastShotMs < 1500.0;
        VehVisualGame game{ v.handle };
        bool ran = false;
        sync::VisualOut o;
        if (g_vehVisualGuard.load(std::memory_order_relaxed) && !iControl)
        {
            // R3 T4: a copy compares and reports its OWN readback, so that readback goes through the per-slot baselines first (a window slot that
            // reads smashed on every car would otherwise be fixed, smashed, or reported as a vehicle Hit). The local player's recent shot
            // corroborates a pop at once; without it a new bit must hold on a second read.
            sync::VisualMask flagged;
            o = sync::StepVisualGuarded(v.vis, v.visGuard, game, server, false, nearUs, shotRecent, nowMs, shotRecent, &ran, &flagged);
            if (flagged.Any())
                NoteUnreliableVisual(id, flagged);
        }
        else
            o = sync::StepVisual(v.vis, game, server, iControl, nearUs, shotRecent, nowMs, &ran);
        if (!ran)
            return;
        ++g_vis.reads;
        static int appliedLogged = 0;
        if (o.cmd.Any() && appliedLogged < 12)
        {
            ++appliedLogged;
            PeerLog(false, "engine: vehicle id=%u visual (%s): server mask tyres %02x rim %02x windows %02x -> burst %02x, off rim %02x, fixed %02x, "
                    "windows smashed %02x fixed %02x", id, iControl ? "ours" : "copy", st.tyres, st.tyresRim, st.windows, o.cmd.burst, o.cmd.rim,
                    o.cmd.fixTyre, o.cmd.smash, o.cmd.fixWindow);
        }
        if (!o.report.Any())
            return;
        int16_t codes[24];
        const int n = sync::ReportToParts(o.report, codes, 24);
        for (int i = 0; i < n; ++i)
        {
            proto::Hit h;
            h.kind = proto::HitKind::Vehicle;
            h.targetId = id;
            h.weapon = lp ? g_world->GetSelectedPedWeapon(lp) : 0;
            h.damage = 1;                              // the server needs a non-zero damage; the part code is what it is sent for
            h.bone = codes[i];
            h.shotSeq = (uint8_t)(g_shotSeq & 15);
            if (join::SendHit(h))
                ++g_vis.partHits;
            else
                ++g_vis.partHitsRefused;
        }
        static int reportLogged = 0;
        if (reportLogged < 12)
        {
            ++reportLogged;
            PeerLog(false, "engine: vehicle id=%u visual: our game popped tyres %02x (off rim %02x) windows %02x on this copy: reported to the server as %d vehicle Hit(s), "
                    "weapon 0x%08x", id, o.report.tyres, o.report.rim, o.report.windows, n, (unsigned)(lp ? g_world->GetSelectedPedWeapon(lp) : 0));
        }
    }

    // ---- R3 T4: vehicle damage state (Desync Overhaul round 3, protocol v7) -----------------------------------------------------
    // Petrol tank health, broken doors / headlights and a coarse dent map for the OTHER clients, and the sanity layer that keeps a wrong
    // read off the wire (libs/sync/vehicle_damage.h has the rules, tests/r3_t4_tests.h the proofs). Switches (HKCU\Software\GAGE-MP):
    // veh_damage_sync=0, veh_visual_guard=0. The 10 s line is "engine: vehicle damage (R3 T4)" (ReportVehDamage).
    struct DmgStats
    {
        uint32_t ctlReads = 0, blocksSent = 0, adopted = 0, guardsSet = 0, copyRuns = 0, copyReplays = 0;
        uint32_t flaggedTyres = 0, flaggedWindows = 0, flaggedParts = 0, flaggedDents = 0, flaggedPetrol = 0;
        uint32_t handLogged = 0;
    };
    DmgStats g_dmg;

    void ResetVehicleDamage(NetVeh& v)
    {
        v.visGuard.Reset();
        v.pedalGuard.Reset();
        v.dmgRep.Reset();
        v.dmgApply.Reset();
        v.handLimit.Reset();
        v.dmgReport = proto::VehicleDamage();
        v.dmgSend.Reset();
        v.dmgRaw = sync::DamageRaw();
        v.dmgReadMs = -1e9;
        v.lastBodyHp = -1.0f;
        v.bodyFellMs = -1e9;
        v.appliedDmgGuard = -1;
        v.handEff = -1;
        v.dmgHullKnown = false;
    }

    // The probe frame of a car: half width, nose and tail from the wheel bones (cached per model), else a mid-size car's. Script tick.
    sync::DentHull VehicleHullOf(int32_t veh)
    {
        static std::map<uint32_t, sync::DentHull> cache;
        const uint32_t model = g_world->GetEntityModel(veh);
        if (!model)
            return sync::DentHull();
        const auto it = cache.find(model);
        if (it != cache.end())
            return it->second;
        sync::DentHull h;
        bool measured = false;
        float px = 0, py = 0, pz = 0, qx = 0, qy = 0, qz = 0, qw = 1;
        if (g_world->GetEntityCoords(veh, px, py, pz) && g_world->GetEntityQuaternion(veh, qx, qy, qz, qw))
        {
            const sync::DriveQuat inv = sync::vq::Conj(sync::vq::Normalize({ qx, qy, qz, qw }));
            auto local = [&](const char* bone, sync::DriveVec& out) {
                const int bi = g_world->GetEntityBoneIndexByName(veh, bone);
                float wx = 0, wy = 0, wz = 0;
                if (bi < 0 || !g_world->GetWorldPositionOfEntityBone(veh, bi, wx, wy, wz))
                    return false;
                out = sync::vq::Rotate(inv, { wx - px, wy - py, wz - pz });
                return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
            };
            sync::DriveVec lf, lr;
            if (local("wheel_lf", lf) && local("wheel_lr", lr) && lf.y - lr.y >= 0.8f && lf.y - lr.y <= 12.0f && std::fabs(lf.x) >= 0.2f &&
                std::fabs(lf.x) <= 2.5f)
            {
                h.halfWidth = std::max(0.6f, std::min(1.8f, std::fabs(lf.x) + 0.25f));
                h.front = std::max(1.2f, std::min(5.0f, lf.y + 0.9f));
                h.rear = std::min(-1.2f, std::max(-6.0f, lr.y - 0.9f));
                measured = true;
            }
        }
        cache[model] = h;
        GLOGI("engine: vehicle: model 0x%08x dent probe frame half width %.2f nose %.2f tail %.2f (%s)", (unsigned)model, (double)h.halfWidth, (double)h.front,
              (double)h.rear, measured ? "from the wheel bones" : "fallback: the wheel bones did not read");
        return h;
    }

    // One entity as vehicle_damage.h's templates see it.
    struct VehDamageGame
    {
        int32_t h;
        sync::DentHull hull;
        bool BodyHealth(float& f) { return g_world->GetVehicleBodyHealth(h, f); }
        bool PetrolHealth(float& f) { return g_world->GetVehiclePetrolTankHealth(h, f); }
        bool DoorValid(int d, bool& v) { return g_world->GetIsDoorValid(h, d, v); }
        bool DoorBroken(int d, bool& b) { return g_world->IsVehicleDoorDamaged(h, d, b); }
        bool LampBroken(int l, bool& b) { return g_world->IsVehicleHeadlightDamaged(h, l == 0, b); }
        bool Dent(int probe, float& metres)
        {
            const sync::DentPoint pt = sync::DentProbePoint(probe, hull);
            float dx = 0, dy = 0, dz = 0;
            if (!g_world->GetVehicleDeformationAtPos(h, pt.x, pt.y, pt.z, dx, dy, dz))
                return false;
            const float m = std::sqrt(dx * dx + dy * dy + dz * dz);
            metres = (std::isfinite(m) && m < 2.0f) ? m : 0.0f;          // a non-finite or absurd vector is "no dent", never a huge one
            return true;
        }
        bool BreakDoor(int d) { return g_world->SetVehicleDoorBroken(h, d, true); }
        bool SetPetrol(float f) { return g_world->SetVehiclePetrolTankHealth(h, f); }
        bool ReplayLamp(int l, float damage)
        {
            const sync::DentPoint pt = sync::LampPoint(l, hull);
            return g_world->SetVehicleDamage(h, pt.x, pt.y, pt.z, damage, 0.35f, true);
        }
        bool ReplayDent(int probe, float damage)
        {
            const sync::DentPoint pt = sync::DentProbePoint(probe, hull);
            return g_world->SetVehicleDamage(h, pt.x, pt.y, pt.z, damage, 1.0f, true);
        }
        void BeginReplay() { g_world->SetVehicleCanBeVisiblyDamaged(h, true); }    // a copy cannot be damaged by the game, but the replay must
        void EndReplay() { g_world->SetVehicleCanBeVisiblyDamaged(h, false); }
    };

    void EnsureVehicleHull(NetVeh& v)
    {
        if (v.dmgHullKnown)
            return;
        v.dmgHull = VehicleHullOf(v.handle);
        v.dmgHullKnown = true;
    }

    // The controller: its body health fell since the last look (corroborates a tyre / window / dent that appears now).
    void NoteVehicleBody(NetVeh& v, double nowMs)
    {
        float hp = 0.0f;
        if (!g_world->GetVehicleBodyHealth(v.handle, hp) || !std::isfinite(hp))
            return;
        if (v.lastBodyHp >= 0.0f && hp < v.lastBodyHp - 0.5f)
            v.bodyFellMs = nowMs;
        v.lastBodyHp = hp;
    }

    // One line per car and slot (capped): a read that was damaged at the first sample while the server shows nothing. Script tick.
    void NoteUnreliableVisual(uint16_t id, const sync::VisualMask& f)
    {
        g_dmg.flaggedTyres += (uint32_t)sync::PopCount8(f.tyres | f.rim);
        g_dmg.flaggedWindows += (uint32_t)sync::PopCount8(f.windows);
        static int logged = 0;
        if (logged < 40)
        {
            ++logged;
            PeerLog(true, "engine: vehicle id=%u sanity: tyre slot(s) %02x (off rim %02x) and window slot(s) %02x read as DAMAGED at the first sample while the server shows "
                    "none: unreliable slot(s), not sent and not compared (a slot the model does not have, or a read that never clears)", id, f.tyres, f.rim, f.windows);
        }
    }

    // Per script frame and car: the self-damage guards, the copy's handbrake limit and the copy's damage apply. Script tick.
    void TickVehicleDamage(NetVeh& v, uint16_t id, bool iControl, double nowMs, bool nearUs)
    {
        const proto::VehicleState& st = v.st;
        const int32_t h = v.handle;
        if (!iControl && g_vehVisualGuard.load(std::memory_order_relaxed))
        {
            // The handbrake bit of the controller's state, limited: held on a standing car, released after 4 s of rolling with it.
            const float sp = std::sqrt(st.k.vx * st.k.vx + st.k.vy * st.k.vy + st.k.vz * st.k.vz);
            const int eff = v.handLimit.Effective((st.flags2 & proto::kVeh2Handbrake) != 0, sp, nowMs) ? 1 : 0;
            if (eff != v.handEff)
            {
                ++(g_world->SetVehicleHandbrake(h, eff != 0) ? g_vis.handSet : g_vis.handFail);
                v.handEff = eff;
            }
        }
        if (!g_vehDamageSync.load(std::memory_order_relaxed))
            return;
        // A copy is kinematic and only follows its controller: the game must not damage it on its own (a local collision with a puppet, an
        // engine that degrades, a tank that leaks), because that damage would exist on this machine only. A car we control has the defaults.
        const int want = iControl ? 0 : 1;
        if (v.appliedDmgGuard != want)
        {
            const bool a = g_world->SetVehicleCanBeVisiblyDamaged(h, iControl);
            const bool b = g_world->SetVehicleEngineCanDegrade(h, iControl);
            const bool c = g_world->SetDisableVehiclePetrolTankDamage(h, !iControl);
            v.appliedDmgGuard = want;
            ++g_dmg.guardsSet;
            static int logged = 0;
            if (logged < 6)
            {
                ++logged;
                PeerLog(false, "engine: vehicle id=%u damage guards for a %s: visibly damaged %s (%s), engine degrades %s (%s), tank damage %s (%s)", id,
                        iControl ? "car we control" : "copy", iControl ? "on" : "OFF", a ? "ok" : "FAILED", iControl ? "on" : "OFF", b ? "ok" : "FAILED",
                        iControl ? "on" : "OFF", c ? "ok" : "FAILED");
            }
        }
        if (iControl || !st.dmg.Any())
            return;                                    // a clean server block costs nothing: not even the probe frame is measured
        EnsureVehicleHull(v);
        VehDamageGame game{ h, v.dmgHull };
        bool ran = false;
        const sync::DamageCmd cmd = sync::StepDamageCopy(v.dmgApply, game, st.dmg, nearUs, nowMs, &ran, &v.dmgRaw);
        if (!ran)
            return;
        ++g_dmg.copyRuns;
        if (!cmd.Any())
            return;
        ++g_dmg.copyReplays;
        if (cmd.Replays())
        {
            // SET_VEHICLE_DAMAGE lowers the health too: put the server's values back.
            g_world->SetVehicleBodyHealth(h, (float)st.bodyHealth);
            g_world->SetVehicleEngineHealth(h, (float)st.engineHealth);
        }
        static int logged = 0;
        if (logged < 24)
        {
            ++logged;
            PeerLog(false, "engine: vehicle id=%u damage (copy): server petrol %u parts %02x dents [%u %u %u %u %u %u %u %u]; local read petrol %.0f doors %02x lamps %x "
                    "dents [%u %u %u %u %u %u %u %u]; commands: doors %02x lamps %x dents %02x petrol %d (gain %.0f damage per metre)", id, (unsigned)st.dmg.petrol,
                    (unsigned)st.dmg.parts, st.dmg.dent[0], st.dmg.dent[1], st.dmg.dent[2], st.dmg.dent[3], st.dmg.dent[4], st.dmg.dent[5], st.dmg.dent[6], st.dmg.dent[7],
                    (double)v.dmgRaw.petrol, v.dmgRaw.doors, v.dmgRaw.lamps, v.dmgRaw.dent[0], v.dmgRaw.dent[1], v.dmgRaw.dent[2], v.dmgRaw.dent[3], v.dmgRaw.dent[4],
                    v.dmgRaw.dent[5], v.dmgRaw.dent[6], v.dmgRaw.dent[7], cmd.breakDoors, cmd.lamps, cmd.dents, cmd.petrol ? 1 : 0, (double)v.dmgApply.Gain());
        }
    }

    // The controller adopts the server's LOWER health: a vehicle Hit lowers the server's body / engine health, but our own car took no bullet
    // (the shooter's game hit its copy), so without this it would show no smoke while every copy does. Script tick.
    void AdoptServerHealth(NetVeh& v, uint16_t id)
    {
        if (!g_vehDamageSync.load(std::memory_order_relaxed))
            return;
        float body = 0.0f, engine = 0.0f;
        const proto::VehicleState& st = v.st;
        bool did = false;
        if (g_world->GetVehicleBodyHealth(v.handle, body) && sync::ShouldAdoptServerHealth((float)st.bodyHealth, body))
            did = g_world->SetVehicleBodyHealth(v.handle, (float)st.bodyHealth) || did;
        if (g_world->GetVehicleEngineHealth(v.handle, engine) && sync::ShouldAdoptServerHealth((float)st.engineHealth, engine))
            did = g_world->SetVehicleEngineHealth(v.handle, (float)st.engineHealth) || did;
        if (!did)
            return;
        ++g_dmg.adopted;
        static int logged = 0;
        if (logged < 8)
        {
            ++logged;
            PeerLog(false, "engine: vehicle id=%u (ours): adopted the server's lower health (a vehicle Hit): body %.0f -> %u, engine %.0f -> %u", id, (double)body,
                    (unsigned)st.bodyHealth, (double)engine, (unsigned)st.engineHealth);
        }
    }

    // The server's own fields of a vehicle state onto its entity, when a new full state came in.
    // Driver-owned fields (engine, siren, horn) only when someone else drives it.
    // The controller's high beam, interior light, indicators, doors and roof on our copy, only the bits that differ
    // from what the copy shows (sync::DiffBody; the first sight applies every bit, instantly). The headlights
    // bit is the lights native's (ApplyVehicleFields). Door ids 0..5 = FL, FR, RL, RR, hood, trunk (SET_VEHICLE_
    // DOOR_*), the wire's kDoor* bit order. Each native is a call that may be unresolved: the shown state is
    // recorded either way, so a missing native costs one attempt, not one per frame.
    void ApplyVehicleBody(NetVeh& v)
    {
        const int32_t h = v.handle;
        const proto::VehicleState& st = v.st;
        sync::BodyDelta d = sync::DiffBody(v.bodyShown, st.flags2, st.doors);
        d.flags2 = sync::CopyFlags2Delta(d.flags2, g_vcIndSync.load(std::memory_order_relaxed));   // 0.4.22: veh_indicator_sync=0 leaves the indicators alone
        if (!d.Any())
            return;
        const bool instantly = !v.bodyShown.valid;
        if (d.flags2 & proto::kVeh2HighBeam)
            g_world->SetVehicleFullbeam(h, (st.flags2 & proto::kVeh2HighBeam) != 0);
        if (d.flags2 & proto::kVeh2Interior)
            g_world->SetVehicleInteriorLight(h, (st.flags2 & proto::kVeh2Interior) != 0);
        if (d.flags2 & sync::kVcIndBoth)
        {
            int ok = 0, failed = 0;
            if (d.flags2 & proto::kVeh2IndicatorL)
                ++(g_world->SetVehicleIndicatorLights(h, 1, (st.flags2 & proto::kVeh2IndicatorL) != 0) ? ok : failed);
            if (d.flags2 & proto::kVeh2IndicatorR)
                ++(g_world->SetVehicleIndicatorLights(h, 0, (st.flags2 & proto::kVeh2IndicatorR) != 0) ? ok : failed);
            g_vcx.copyIndApplied += (uint32_t)ok;
            g_vcx.copyIndFailed += (uint32_t)failed;
            if (st.flags2 & sync::kVcIndBoth)                        // 0.4.27: the per-model line, at the first call that switches one on
                IndNoteCalls(h, "the copy (another player's car)", (uint8_t)(st.flags2 & sync::kVcIndBoth), ok, failed);
        }
        if (d.flags2 & proto::kVeh2Brake)
            ++(g_world->SetVehicleBrakeLights(h, (st.flags2 & proto::kVeh2Brake) != 0) ? g_vis.brakeSet : g_vis.brakeFail);
        // The first sight applies "released" too: a copy must not keep a handbrake of its own. With the sanity layer on (R3 T4) the handbrake is
        // applied per frame by TickVehicleDamage (HandbrakeLimiter) instead, so a claim that outlives a moving car is released.
        if ((d.flags2 & proto::kVeh2Handbrake) && !g_vehVisualGuard.load(std::memory_order_relaxed))
            ++(g_world->SetVehicleHandbrake(h, (st.flags2 & proto::kVeh2Handbrake) != 0) ? g_vis.handSet : g_vis.handFail);
        if (d.flags2 & proto::kVeh2RoofDown)
        {
            if (st.flags2 & proto::kVeh2RoofDown)
                g_world->LowerConvertibleRoof(h, instantly);
            else
                g_world->RaiseConvertibleRoof(h, instantly);
        }
        for (int door = 0; door < sync::kBodyDoors; ++door)
        {
            if (!(d.doors & (1u << door)))
                continue;
            // R3 T4: a door that is broken off is neither open nor shut (its angle ratio reads as open for ever): do not fight the damage replay.
            if (g_vehDamageSync.load(std::memory_order_relaxed) && (st.dmg.parts & (1u << door)))
                continue;
            if (st.doors & (1u << door))
                g_world->SetVehicleDoorOpen(h, door, false, instantly);
            else
                g_world->SetVehicleDoorShut(h, door, instantly);
        }
        v.bodyShown = sync::BodyShown{ st.flags2, st.doors, true };
    }

    // The server's fields on our copy of a vehicle. `iControl`: the server says WE control it (ctrl == me): its
    // engine, siren, horn, health, lights and body are ours, read from the car and sent, so only the statics
    // (colours, locks, the server's lights override) are applied. For a copy somebody else controls the lights
    // shown are the server's override OR the controller's actual headlights, and the rest of its body (high beam,
    // interior light, indicators, doors, roof) follows the controller's flags2 / doors, applied only on change.
    void ApplyVehicleFields(NetVeh& v, bool iControl)
    {
        const int32_t h = v.handle;
        const proto::VehicleState& st = v.st;
        if (st.colour1 != v.appliedC1 || st.colour2 != v.appliedC2)
        {
            g_world->SetVehicleColours(h, st.colour1, st.colour2);
            v.appliedC1 = st.colour1;
            v.appliedC2 = st.colour2;
        }
        const int lock = (st.flags & proto::kVehLocked) ? 2 : 1;
        if (lock != v.appliedLock)
        {
            const bool ok = g_world->SetVehicleDoorsLocked(h, lock);
            // 0.4.22: a lock that is not the unlocked default (or a change of one) is logged; the keeper reads it back at once and again every second
            if (lock == sync::kLockLocked || v.appliedLock >= 0)
                GLOGI("engine: doors: vehicle id=%u is now %s (the server's flag): SET_VEHICLE_DOORS_LOCKED(%d) %s", (unsigned)st.id,
                      lock == sync::kLockLocked ? "LOCKED" : "UNLOCKED", lock, ok ? "called" : "NOT callable");
            v.appliedLock = lock;
            v.lockKeep.Reset();
        }
        // SET_VEHICLE_LIGHTS: 0 normal, 1 forced off, 2 forced on. Our own car: forced on by the server's override
        // only. A copy: on when the override or the controller's actual headlights say so, else forced off (a
        // normal-mode copy would light itself at night with the puppet driver, which the controller may not have).
        const bool lightsOverride = (st.flags & proto::kVehLights) != 0;
        const int lights = iControl ? (lightsOverride ? 2 : 0)
                                    : (sync::HeadlightsShown(lightsOverride, (st.flags2 & proto::kVeh2Headlights) != 0) ? 2 : 1);
        if (lights != v.appliedLights)
        {
            g_world->SetVehicleLights(h, lights);
            v.appliedLights = lights;
            ++g_sig.lightsChanged;
        }
        if (iControl)
        {
            AdoptServerHealth(v, st.id);       // R3 T4
            return;
        }
        ApplyVehicleBody(v);
        if (st.flags2 & proto::kVeh2Brake)
            g_world->SetVehicleBrakeLights(h, true);       // renewed by every full state while they are on (INFERRED: the native may not hold)
        const int engine = (st.flags & proto::kVehEngine) ? 1 : 0;
        if (engine != v.appliedEngine)
        {
            g_world->SetVehicleEngineOn(h, engine != 0);
            v.appliedEngine = engine;
            ++g_vcx.copyEngineApplied;                 // 0.4.22: the controller's Y (the engine bit) reaches this copy
        }
        const int siren = (st.flags & proto::kVehSiren) ? 1 : 0;
        if (siren != v.appliedSiren)
        {
            g_world->SetVehicleSiren(h, siren != 0);
            v.appliedSiren = siren;
        }
        // The horn: 0.4.19 TickVehicleSignals (HornSync) follows the state every frame; veh_horn_sync=0 keeps 0.4.17's call per received state.
        if (st.flags & proto::kVehHorn)
        {
            ++g_sig.hornStates;
            if (!g_hornSync.load(std::memory_order_relaxed))
            {
                g_world->StartVehicleHorn(h, 400);     // renewed by every state while it sounds (0.4.17)
                ++g_sig.oldHornCalls;
            }
        }
        if (st.bodyHealth != v.appliedBody)
        {
            g_world->SetVehicleBodyHealth(h, (float)st.bodyHealth);
            v.appliedBody = st.bodyHealth;
        }
        if (st.engineHealth != v.appliedEngineHp)
        {
            g_world->SetVehicleEngineHealth(h, (float)st.engineHealth);
            v.appliedEngineHp = st.engineHealth;
        }
    }

    // 0.4.19 vehicle signals: the controller's horn and headlights / high beam on a copy somebody else controls, every script tick
    // (libs/sync/vehicle_signals.h, tested offline in tests/vehsignals_tests.h). The horn follows the newest state's bit (a rise at once, a
    // hold renewed while it sounds, a release call, a car without fresh states let go) with IS_HORN_ACTIVE as the read-back; the lights
    // ApplyVehicleFields set on a change are read back on near copies and re-issued when the game dropped them. Switches: veh_horn_sync,
    // veh_horn_held, veh_horn_stop, veh_lights_keep (see g_hornSync). The 10 s line is "engine: vehicle signals (0.4.19)".
    struct VehSignalGame
    {
        int32_t h;
        uint32_t hornMode;
        bool StartHorn(int durationMs) { return g_world->StartVehicleHorn(h, durationMs, hornMode); }
        bool HornActive(bool& active) { return g_world->IsHornActive(h, active); }
        bool ReadLights(bool& on, bool& high) { return g_world->GetVehicleLightsState(h, on, high); }
        bool SetLights(int lightsMode)
        {
            const bool ok = g_world->SetVehicleLights(h, lightsMode);
            ++(ok ? g_sig.lightsReissueCalls : g_sig.lightsReissueFail);
            return ok;
        }
        bool SetFullbeam(bool high)
        {
            const bool ok = g_world->SetVehicleFullbeam(h, high);
            ++(ok ? g_sig.lightsReissueCalls : g_sig.lightsReissueFail);
            return ok;
        }
    };
    void TickVehicleSignals(NetVeh& v, bool iControl, double nowMs, bool nearUs)
    {
        if (iControl || !v.handle || !v.haveState)
            return;
        const proto::VehicleState& st = v.st;
        VehSignalGame g{ v.handle, g_hornHeld.load(std::memory_order_relaxed) ? kHornModeHeldDown : 0u };
        if (g_hornSync.load(std::memory_order_relaxed))
        {
            v.horn.Configure(HornConfigNow());
            sync::StepHorn(v.horn, g, (st.flags & proto::kVehHorn) != 0, v.sigStateMs, nowMs);
            g_sig.horn += v.horn.TakeStats();
        }
        if (g_lightsKeep.load(std::memory_order_relaxed))
        {
            const sync::LightsWant want{ sync::HeadlightsShown((st.flags & proto::kVehLights) != 0, (st.flags2 & proto::kVeh2Headlights) != 0),
                                         (st.flags2 & proto::kVeh2HighBeam) != 0 };
            v.lightsKeep.Configure(LightsConfigNow());
            const sync::LightsCmd c = sync::StepLights(v.lightsKeep, g, want, nearUs, nowMs);
            if (c.setMode)
                v.appliedLights = c.mode;
            g_sig.lights += v.lightsKeep.TakeStats();
        }
    }

    // One server vehicle we have not spawned yet. False while it is still streaming (or failed).
    bool SpawnNetVehicle(NetVeh& v, uint16_t id, double nowMs)
    {
        if (nowMs < v.spawnRetryAtMs)
            return false;
        const uint32_t model = v.st.model;
        if (!g_world->IsVehicleModel(model))
        {
            if (!v.badModelLogged)
            {
                v.badModelLogged = true;
                PeerLog(true, "engine: vehicle id=%u: model 0x%08x is not a vehicle in this game -- not shown", id,
                        (unsigned)model);
            }
            v.spawnRetryAtMs = nowMs + 30000.0;
            ++g_v4.vehSpawnFail;
            return false;
        }
        if (v.requestedModel != model)
        {
            if (v.requestedModel)
                g_world->ModelNoLongerNeeded(v.requestedModel);
            g_world->RequestModel(model);
            v.requestedModel = model;
            v.spawnSinceMs = nowMs;
        }
        if (!g_world->HasModelLoaded(model))
        {
            if (nowMs - v.spawnSinceMs > 10000.0)
            {
                PeerLog(true, "engine: vehicle id=%u: model 0x%08x did not load in 10 s; retrying in 10 s", id,
                        (unsigned)model);
                g_world->ModelNoLongerNeeded(model);
                v.requestedModel = 0;
                v.spawnRetryAtMs = nowMs + 10000.0;
                ++g_v4.vehSpawnFail;
            }
            return false;
        }
        const sync::DriveQuat q = sync::vq::Normalize({ v.st.rot.x, v.st.rot.y, v.st.rot.z, v.st.rot.w });
        const int32_t h = g_world->CreateVehicle(model, v.st.k.x, v.st.k.y, v.st.k.z,
                                                 Heading360((float)sync::vq::HeadingDeg(q)));
        g_world->ModelNoLongerNeeded(model);
        v.requestedModel = 0;
        if (!h)
        {
            v.spawnRetryAtMs = nowMs + 1000.0;
            ++g_v4.vehSpawnFail;
            return false;
        }
        g_world->MarkVehicleOwned(h);                  // ours: never cleaned up, no ambient driver made for it
        RadioOffNewVehicle(h);                         // 0.4.19: a remote driver's car plays no radio either
        g_world->SetEntityQuaternion(h, q.x, q.y, q.z, q.w);
        ForgetVehicleHandle(v);
        v.handle = h;
        v.spawnedModel = model;
        v.badModelLogged = false;
        ++g_v4.vehSpawned;
        PeerLog(false, "engine: vehicle id=%u spawned as %d (model 0x%08x) at %.1f, %.1f, %.1f", id, h,
                (unsigned)model, v.st.k.x, v.st.k.y, v.st.k.z);
        return true;
    }

    // Vehicles: delete the gone ones, spawn the new ones, apply the server's fields, drive the ones
    // someone else drives (interpolator + drive controller), seat the remote peds per occupancy.
    // ---- T4: vehicle collisions (libs/sync/vehicle_impact.h, docs/sync-specs/vehicle-collisions.md) ----
    constexpr double kAdoptGuardMs = 300.0;      // the longest the stale stream is held back from an adopted copy
    constexpr double kAdoptCooldownMs = 150.0;   // after an adoption no new contact is looked for on that copy this long
    constexpr double kImpactNearM = 7.0;         // a copy this close (origins) to our car around a server impact counts as "isNear"
    constexpr double kImpactWatchM = 30.0;       // a controlled car with another vehicle within this is watched for velocity jumps

    // A car we control (the local player's, or an empty one the server handed us): feed its velocity to the jump tracker once per frame,
    // but only while another vehicle is isNear (two natives per frame).
    void FeedImpactTracker(uint16_t id, NetVeh& v, double nowMs)
    {
        bool isNear = false;
        for (const auto& kv : g_vehs)
        {
            if (kv.first == id || !kv.second.handle || !kv.second.haveState)
                continue;
            const float dx = kv.second.st.k.x - v.st.k.x, dy = kv.second.st.k.y - v.st.k.y;
            if (dx * dx + dy * dy < kImpactWatchM * kImpactWatchM)
            {
                isNear = true;
                break;
            }
        }
        if (!isNear)
        {
            v.impactTracker.Reset();
            return;
        }
        float vx = 0, vy = 0, vz = 0, ax = 0, ay = 0, az = 0;
        if (!g_world->GetEntityVelocity(v.handle, vx, vy, vz) || !g_world->GetEntityRotationVelocity(v.handle, ax, ay, az))
            return;
        if (v.impactTracker.Feed(nowMs, { vx, vy }, az))
            ++g_t4.jumpEpisodes;
    }

    // The server's VehicleImpact messages for cars we control: the authoritative change of the car's velocity and yaw rate for the contact, less
    // what the game's own physics already did (the tracker's jumps): the difference is written onto the car.
    void DrainImpacts(double nowMs, uint16_t me)
    {
        static std::vector<proto::VehicleImpact> got;   // script tick only
        got.clear();
        {
            std::lock_guard<std::mutex> lk(g_impactMtx);
            got.swap(g_impactQueue);
        }
        for (const proto::VehicleImpact& m : got)
        {
            ++g_t4.impactsIn;
            if (!g_impactSync.load(std::memory_order_relaxed))
                continue;
            const auto it = g_vehs.find(m.vehId);
            if (it == g_vehs.end() || !it->second.handle || it->second.st.ctrl != me)
            {
                ++g_t4.noVehicle;
                continue;
            }
            NetVeh& v = it->second;
            if (!proto::SeqNewer(m.seq, v.lastImpactSeq) && v.lastImpactSeq != 0)
            {
                ++g_t4.dup;
                continue;
            }
            ++g_r4.impactsAuth;
            g_r4.authDvMax = std::max(g_r4.authDvMax, std::sqrt((double)m.dvx * m.dvx + (double)m.dvy * m.dvy));
            g_r4.closingMax = std::max(g_r4.closingMax, (double)m.closing);
            g_r4.depthMax = std::max(g_r4.depthMax, (double)m.depth);
            v.lastImpactSeq = m.seq;
            float cx = 0, cy = 0, cz = 0;
            bool isNear = false;
            if (g_world->GetEntityCoords(v.handle, cx, cy, cz))
            {
                const auto ot = g_vehs.find(m.otherId);
                float ox = 0, oy = 0, oz = 0;
                if (ot != g_vehs.end() && ot->second.handle && g_world->GetEntityCoords(ot->second.handle, ox, oy, oz))
                {
                    const double dx = ox - cx, dy = oy - cy;
                    isNear = dx * dx + dy * dy < kImpactNearM * kImpactNearM;
                }
            }
            const double tc = sync::impact::UnwrapStampMs(m.sampleMs, nowMs);
            const sync::impact::ApplyDecision d = sync::impact::DecideImpactApply(v.impactTracker, { m.dvx, m.dvy }, m.dyaw, tc, nowMs, isNear, m.depth);
            static int logged = 0;
            if (d.why != sync::impact::ApplyWhy::Applied)
            {
                if (d.why == sync::impact::ApplyWhy::NoLocalContact) ++g_t4.noLocal;
                else if (d.why == sync::impact::ApplyWhy::Tiny) ++g_t4.tiny;
                else ++g_t4.stale;
                if (logged < 12)
                {
                    ++logged;
                    PeerLog(false, "engine: vehicle id=%u: server impact #%u with vehicle %u (change %.1f,%.1f m/s, closing %.1f, depth %.2f m, contact %.0f ms ago) "
                            "not applied (%s)", (unsigned)m.vehId, m.seq, (unsigned)m.otherId, (double)m.dvx, (double)m.dvy, (double)m.closing, (double)m.depth,
                            nowMs - tc, d.why == sync::impact::ApplyWhy::NoLocalContact ? "our physics felt no contact and the server's boxes barely met" :
                            d.why == sync::impact::ApplyWhy::Tiny ? "too small" : "too old");
                }
                continue;
            }
            float vx = 0, vy = 0, vz = 0, ax = 0, ay = 0, az = 0;
            if (!g_world->GetEntityVelocity(v.handle, vx, vy, vz))
                continue;
            const float nvx = vx + (float)d.dv.x, nvy = vy + (float)d.dv.y;
            g_world->SetEntityVelocity(v.handle, nvx, nvy, vz);
            const bool haveRot = g_world->GetEntityRotationVelocity(v.handle, ax, ay, az);
            if (haveRot && std::fabs(d.dw) > 1e-3)
            {
                // 0.4.22: (ax, ay, az) is the WORLD angular velocity and the setter takes a BODY vector: written back unchanged the x / y part
                // (pitch / roll rates) came out turned by the heading. Hand it the body-frame version when the orientation can be read.
                sync::DriveVec wv = { ax, ay, az + (float)d.dw };
                float oqx = 0, oqy = 0, oqz = 0, oqw = 1;
                if (g_shakeFix.load() && g_angJudge.UseBody() && g_world->GetEntityQuaternion(v.handle, oqx, oqy, oqz, oqw))
                    wv = sync::shake::WorldToBody({ oqx, oqy, oqz, oqw }, wv);
                g_world->SetEntityAngularVelocity(v.handle, wv.x, wv.y, wv.z);
                ++g_shk.impactAngWrites;
            }
            v.impactTracker.NoteWritten({ nvx, nvy }, haveRot ? az + d.dw : 0.0, d.dv, d.dw);
            ++g_t4.applied;
            {
                // R4 T2: the OTHER car's copy is the one to compare with its owner after this impact (+0.5 / +1 / +2 s)
                const auto ot = g_vehs.find(m.otherId);
                if (ot != g_vehs.end() && ot->second.handle && ot->second.st.ctrl != me)
                    g_r4.watch.Open(m.otherId, sync::ContactWatch::kServerImpact, nowMs);
            }
            const double dvLen = sync::impact::Len(d.dv);
            g_t4.appliedDvSum += dvLen;
            g_t4.appliedDvMax = std::max(g_t4.appliedDvMax, dvLen);
            if (logged < 12)
            {
                ++logged;
                PeerLog(false, "engine: vehicle id=%u: server impact #%u with vehicle %u (closing %.1f m/s, depth %.2f m, contact %.0f ms ago): authoritative "
                        "change %.1f,%.1f m/s / yaw %.2f, our physics had done %.1f,%.1f m/s%s; applied the difference %.1f,%.1f m/s / yaw %.2f",
                        (unsigned)m.vehId, m.seq, (unsigned)m.otherId, (double)m.closing, (double)m.depth, nowMs - tc, (double)m.dvx, (double)m.dvy,
                        (double)m.dyaw, d.dvLocal.x, d.dvLocal.y, d.localContact ? "" : " (it felt no contact)", d.dv.x, d.dv.y, d.dw);
            }
        }
    }

    void TickVehicles(int32_t lp, int32_t lpVeh)
    {
        static std::vector<uint16_t> removedVehs;     // script tick only
        removedVehs.clear();
        {
            // No lock: g_vehs / g_peers are the game thread's own, SyncNet (TickPeers, this frame) brought them up to date.
            const double nowMs = NowMs();
            const uint16_t me = join::MyId();
            DrainImpacts(nowMs, me);   // T4: the server's impacts for cars we control
            float lpx = 0, lpy = 0, lpz = 0;
            const bool haveLp = lp && g_world->GetEntityCoords(lp, lpx, lpy, lpz);
            // Sync B: where the local player's own vehicle is (the penetration estimate of the collision line: a remote copy whose
            // origin comes within ~2 m of it overlaps it).
            float lcx = 0, lcy = 0, lcz = 0;
            const bool haveLc = lpVeh && g_world->GetEntityCoords(lpVeh, lcx, lcy, lcz);
            for (auto& kv : g_vehs)
                kv.second.contactPosValid = false;
            // R3 T5 (proximity contact): OUR vehicle as the oriented box the server's arbiter uses, read once per frame and only when a copy is close enough to ask.
            sync::impact::Pose t5OwnPose;
            sync::impact::MassClass t5OwnMc = sync::impact::MassClass::Car;
            bool t5OwnTried = false, t5OwnOk = false;
            // R4 T2: where the vehicles we control are this frame (the contact gate and the touch classification): the local player's vehicle and any empty car handed to us
            struct OwnCar { float x, y; };
            static std::vector<OwnCar> r4Own;       // script tick only; reused
            r4Own.clear();
            // The local player's vehicle counts only when we control it: a server car we ride in as a passenger is the driver's
            // (0.4.17, veh_ride_gate); a car the server does not track is ours.
            bool lpVehOurs = true;
            if (g_rideGate.load())
                for (const auto& okv : g_vehs)
                    if (okv.second.handle && okv.second.handle == lpVeh && okv.second.haveState)
                    {
                        lpVehOurs = okv.second.st.ctrl == me || g_localSeat == 0;
                        break;
                    }
            if (haveLc && lpVehOurs)
                r4Own.push_back({ lcx, lcy });
            for (const auto& okv : g_vehs)
            {
                const NetVeh& ov = okv.second;
                if (!ov.handle || ov.handle == lpVeh || !ov.haveState || ov.st.ctrl != me)
                    continue;
                float ox = 0, oy = 0, oz = 0;
                if (g_world->GetEntityCoords(ov.handle, ox, oy, oz))
                    r4Own.push_back({ ox, oy });
            }
            size_t spawned = 0;
            for (const auto& kv : g_vehs)
                if (kv.second.handle)
                    ++spawned;
            for (auto it = g_vehs.begin(); it != g_vehs.end();)
            {
                const uint16_t id = it->first;
                NetVeh& v = it->second;
                if (v.handle && g_world->EntityExists(v.handle) == 0)
                {
                    PeerLog(true, "engine: vehicle id=%u: entity %d no longer exists (game cleanup?)", id, v.handle);
                    UnseatPeersIn(id);
                    ForgetVehicleHandle(v);
                    --spawned;
                }
                const bool mine = v.handle && v.handle == lpVeh;
                if (v.removed || !v.present || !s_syncLive)
                {
                    if (mine && v.removed && s_syncLive)
                    {
                        // Destroyed on the server with us inside: out first (warp, flag 16), delete after.
                        if (!v.leaveAtMs || nowMs - v.leaveAtMs > 2000.0)
                        {
                            // W9 item 5: out of a MOVING car by bail-out (4160), not a warp; the retry (and a slow car) warps (16).
                            const float speed = g_world->GetEntitySpeed(v.handle);
                            const int flags = sync::LocalLeaveFlags(speed, v.leaveAtMs > 0.0);
                            g_world->TaskLeaveVehicle(lp, v.handle, flags);
                            v.leaveAtMs = nowMs;
                            if (flags == sync::kLeaveBailOut)
                                ++g_v4.localBailOuts;
                            PeerLog(true, "engine: vehicle id=%u was removed by the server while we sit in it (%.1f m/s): "
                                    "getting out (flags %d)", id, speed, flags);
                        }
                        ++it;
                        continue;
                    }
                    if (v.handle)
                    {
                        UnseatPeersIn(id);
                        if (mine)
                            PeerLog(true, "engine: vehicle id=%u left our range while we sit in it: kept as a local car", id);
                        else
                        {
                            g_world->DeleteEntity(v.handle);
                            ++g_v4.vehDeleted;
                        }
                    }
                    if (v.requestedModel)
                        g_world->ModelNoLongerNeeded(v.requestedModel);
                    removedVehs.push_back(id);
                    g_seats.OnVehicleGone((int)id);         // T1: nobody is walking to a car that is gone
                    it = g_vehs.erase(it);
                    continue;
                }
                if (!v.haveState)
                {
                    ++it;
                    continue;
                }
                const proto::VehicleState& st = v.st;
                bool meIn = false;
                for (size_t s = 0; s < proto::kVehSeats; ++s)
                    meIn = meIn || st.seats[s] == me;
                // The server reused this id for another model (destroyed, then created again, inside one
                // VehicleWorld): the old entity is the wrong car. Not our own adopted car, nor one we sit
                // in; those keep their entity.
                if (v.handle && sync::NetVehicleModelStale(st.model, v.spawnedModel, v.adopted, mine || meIn))
                {
                    PeerLog(true, "engine: vehicle id=%u: the server now says model 0x%08x, the entity %d is 0x%08x "
                            "(id reused): replacing it", id, (unsigned)st.model, v.handle, (unsigned)v.spawnedModel);
                    UnseatPeersIn(id);
                    g_world->DeleteEntity(v.handle);
                    ++g_v4.vehDeleted;
                    ForgetVehicleHandle(v);
                    ClearVehInterp(id);                // the old car's snapshots are not this car's
                    --spawned;
                }
                if (!v.handle)
                {
                    // Never spawn the one the server has us in: that is our own car (bound by the link).
                    if (meIn || spawned >= kMaxLocalVehicles || !SpawnNetVehicle(v, id, nowMs))
                    {
                        ++it;
                        continue;
                    }
                    ++spawned;
                }
                const int32_t h = v.handle;
                // The server says who controls the motion (ctrl, sync plan W6): not the seat. `mine && seat 0` stays as a
                // guard for the few ticks between our Enter being granted and the first state that names us.
                const bool iControl = st.ctrl == me || (mine && g_localSeat == 0);
                if (v.ctrlSeq != v.appliedCtrlSeq)
                {
                    // A hand-over (or a new session): the interpolator was emptied by the join worker. A drive
                    // controller snaps onto its first sample (a new source of motion); if we are the new controller
                    // nobody snaps anything: the car stays where it is and our updates start from that pose.
                    v.appliedCtrlSeq = v.ctrlSeq;
                    if (v.driveResetSeq != v.appliedDriveResetSeq)
                    {
                        v.appliedDriveResetSeq = v.driveResetSeq;
                        v.drive.Reset();
                        v.lastDriveMs = 0;
                    }
                    v.bodyShown = sync::BodyShown();   // the new controller's lights and doors are shown afresh
                    v.appliedEngine = v.appliedSiren = v.appliedLights = -1;   // ... and so are the server's engine / siren / health
                    v.appliedBody = v.appliedEngineHp = 0xFFFF;
                    v.sendGate.Reset();
                    v.sentOnce = false;
                    v.moving = false;
                    v.lastReadMs = v.bodyReadMs = -1e9;
                    v.steerPrev = 0.0f;
                    v.sirenHornHold = sync::SirenHornHold();
                    v.steerFieldWritten = NAN;         // 0.4.30: no read-back of a write made under the previous role
                    v.vis.Reset();                    // T3: the new controller's tyres / windows are read and applied afresh
                    v.visReadMs = -1e9;
                    v.sentVis = sync::VisualMask();
                    v.steerPrevRotMs = -1e9;
                    ResetVehicleDamage(v);             // R3 T4: new role, new baselines
                    v.horn.Reset();                    // 0.4.19: the new controller's horn and lights start afresh
                    v.lightsKeep.Reset();
                    // 0.4.22: a car we take over keeps blinking what it showed (hazards on a parked car): our updates start from the server's bits.
                    v.ctlInd = g_vcIndSync.load(std::memory_order_relaxed) ? (uint8_t)(st.flags2 & sync::kVcIndBoth) : (uint8_t)0;
                }
                if (v.appliedVersion != v.stVersion)
                {
                    ApplyVehicleFields(v, iControl);
                    v.appliedVersion = v.stVersion;
                    v.sigStateMs = nowMs;              // 0.4.19: a horn is held only while states keep coming
                }
                {
                    // T3: tyres / windows. "Near" = the server position within kVisNearM of us (a far copy is only brought to the
                    // server's state when it changes, never re-read or scanned).
                    const float vdx = st.k.x - lpx, vdy = st.k.y - lpy, vdz = st.k.z - lpz;
                    const bool visNear = mine || (haveLp && vdx * vdx + vdy * vdy + vdz * vdz <= kVisNearM * kVisNearM);
                    TickVehicleVisual(v, id, iControl, nowMs, visNear, lp);
                    TickVehicleDamage(v, id, iControl, nowMs, visNear);    // R3 T4
                    TickVehicleSignals(v, iControl, nowMs, visNear);       // 0.4.19: horn, headlights / high beam
                }
                // W9 item 4 (passenger engine sound): the engine flag above is applied only when the server's flag CHANGES, and the
                // server clears it at 0 fuel, so a car the local ped rides in as a passenger could sit with its engine stopped on
                // this machine (silent). While the server says it runs, SET_VEHICLE_ENGINE_ON is asserted again every ~1 s
                // (250 ms while the car reads not running). A throttle / RPM native for the puppet driver's audio is not in 3889
                // (SET_VEHICLE_CURRENT_RPM is not available): only the engine flag is driven, and the line below says so once.
                {
                    const bool passengerHere = mine && !iControl && g_localSeat > 0;
                    const bool serverOn = (st.flags & proto::kVehEngine) != 0;
                    const bool running = passengerHere ? g_world->IsVehicleEngineRunning(h) : true;
                    if (passengerHere)
                    {
                        g_pax.atMs = nowMs;
                        g_pax.id = id;
                        g_pax.serverOn = serverOn;
                        g_pax.running = running;
                        if (!g_pax.rpmLogged)
                        {
                            g_pax.rpmLogged = true;
                            PeerLog(false, "engine: vehicle id=%u: we ride as a passenger; the engine is re-asserted every 1 s while the server "
                                    "says it runs. Tried for the puppet driver's engine audio: SET_VEHICLE_ENGINE_ON only (no throttle / RPM "
                                    "native in 3889: SET_VEHICLE_CURRENT_RPM is unavailable)", id);
                        }
                    }
                    if (v.engineAssert.Due(nowMs, passengerHere, serverOn, running))
                    {
                        g_world->SetVehicleEngineOn(h, true);
                        v.appliedEngine = 1;
                        ++g_v4.engineAsserts;
                        if (!running)
                        {
                            static double notRunningLoggedMs = -1e9;
                            if (nowMs - notRunningLoggedMs >= 5000.0)
                            {
                                notRunningLoggedMs = nowMs;
                                PeerLog(true, "engine: vehicle id=%u: the server says the engine runs but our copy reads it stopped "
                                        "(we ride in it): SET_VEHICLE_ENGINE_ON again", id);
                            }
                        }
                    }
                }
                if (!iControl)
                {
                    sync::VehicleSample s;
                    // W9 item 6: how far the local player is from this car (by the server position: cheap, and the bands are wide)
                    const double leadDist = haveLp ? std::sqrt((double)(st.k.x - lpx) * (st.k.x - lpx) + (double)(st.k.y - lpy) * (st.k.y - lpy) +
                                                               (double)(st.k.z - lpz) * (st.k.z - lpz))
                                                   : 1e9;
                    double dispT = 0.0;   // T4: the interpolation time of this frame (an adopted response is stored at it)
                    sync::VehicleSample t5Ideal;   // R3 T5: the owner's report sampled at (now + link lag): the diagnostic's reference
                    double t5Lag = 0.0, t5Unled = 0.0;
                    // 0.4.17 (veh_ride_lead): the car we ride in plays back the driver's real path, no lead (it eases to 0). The lead exists to
                    // line copies up with OUR car for collisions; inside the copy it only shows every turn-in and brake as a correction.
                    const bool rideNoLead = mine && g_rideLead.load();
                    const double clockDist = rideNoLead ? 1e9 : leadDist;
                    const bool t5Diag = !rideNoLead && leadDist < 120.0;
                    if (SampleVehInterp(id, s_frameMs, s, &v.lead, clockDist, v.lastDriveMs > 0 ? nowMs - v.lastDriveMs : 0.0, &v.leadMs, &dispT, st.ctrl,
                                        t5Diag ? &t5Ideal : nullptr, &t5Lag, &t5Unled))
                    {
                        if (v.leadMs > 1.0)
                            ++g_v4.leadFrames;
                        sync::VehicleDriveInput in;
                        in.target = s;
                        float qx, qy, qz, qw;
                        if (g_world->GetEntityCoords(h, in.localPos.x, in.localPos.y, in.localPos.z) &&
                            g_world->GetEntityVelocity(h, in.localVel.x, in.localVel.y, in.localVel.z) &&
                            g_world->GetEntityQuaternion(h, qx, qy, qz, qw))
                        {
                            in.localRot = { qx, qy, qz, qw };
                            in.dtMs = v.lastDriveMs > 0 ? nowMs - v.lastDriveMs : 0.0;
                            // W9 item 6 (contact grace): HAS_ENTITY_COLLIDED_WITH_ANYTHING of the copy, a rising edge only, and only
                            // for a car within 60 m of us (the native is not free); the drive controller also opens a grace by itself
                            // on a speed loss it did not command. Whether the flag stays set on the ground is INFERRED: a flag that
                            // never falls opens one grace, not a stream of them (rising edge, and the controller's cooldown).
                            bool flagRead = false, flagNow = false;
                            if (leadDist < 60.0)
                            {
                                const bool flag = g_world->HasEntityCollidedWithAnything(h);
                                in.contact = flag && !v.lastContactFlag;
                                v.lastContactFlag = flag;
                                flagRead = true;
                                flagNow = flag;
                            }
                            else
                                v.lastContactFlag = false;
                            // R3 T5 (proximity contact): this copy's drawn pose against OUR vehicle, both as the arbiter's boxes, 120 ms ahead.
                            in.proximity = false;
                            if (g_proxContact.load(std::memory_order_relaxed) && haveLc && !mine && leadDist < 40.0)
                            {
                                if (!t5OwnTried)
                                {
                                    t5OwnTried = true;
                                    float ovx = 0, ovy = 0, ovz = 0, oqx, oqy, oqz, oqw, owx = 0, owy = 0, owz = 0;
                                    if (g_world->GetEntityVelocity(lpVeh, ovx, ovy, ovz) && g_world->GetEntityQuaternion(lpVeh, oqx, oqy, oqz, oqw))
                                    {
                                        if (!g_world->GetEntityRotationVelocity(lpVeh, owx, owy, owz)) owz = 0;
                                        t5OwnPose.p = { (double)lcx, (double)lcy };
                                        t5OwnPose.v = { (double)ovx, (double)ovy };
                                        t5OwnPose.h = sync::vq::HeadingDeg({ oqx, oqy, oqz, oqw }) * (3.14159265358979323846 / 180.0);
                                        t5OwnPose.w = owz;
                                        static int32_t clsHandle = 0;
                                        static int cls = -1;
                                        if (clsHandle != lpVeh) { clsHandle = lpVeh; cls = g_world->GetVehicleClass(lpVeh); }
                                        t5OwnMc = cls >= 0 ? sync::impact::MassClassOfVehicleClass(cls) : sync::impact::MassClass::Car;
                                        t5OwnOk = true;
                                    }
                                }
                                if (t5OwnOk)
                                {
                                    const sync::impact::Pose pc{ { (double)in.localPos.x, (double)in.localPos.y }, { (double)s.vel.x, (double)s.vel.y },
                                                                 sync::vq::HeadingDeg(in.localRot) * (3.14159265358979323846 / 180.0), (double)s.angVel.z };
                                    const sync::impact::MassClass cmc = v.vehClass >= 0 ? sync::impact::MassClassOfVehicleClass(v.vehClass) : sync::impact::MassClass::Car;
                                    if (sync::impact::PredictOverlap(t5OwnPose, t5OwnMc, pc, cmc, sync::impact::ProximityCfg()).hit)
                                    {
                                        in.proximity = true;
                                        ++g_t5.proxFrames;
                                    }
                                }
                            }
                            // R4 T2 (contact gate): a vehicle we control within kContactNearM? Only then do the flag / speed loss / kick mean a car-to-car contact.
                            double r4OwnDist = -1.0;
                            for (const OwnCar& oc : r4Own)
                            {
                                const double ddx = (double)oc.x - in.localPos.x, ddy = (double)oc.y - in.localPos.y;
                                const double dd = std::sqrt(ddx * ddx + ddy * ddy);
                                if (r4OwnDist < 0.0 || dd < r4OwnDist)
                                    r4OwnDist = dd;
                            }
                            in.ownNear = !g_contactGate.load(std::memory_order_relaxed) || (r4OwnDist >= 0.0 && r4OwnDist <= kContactNearM);
                            if (r4OwnDist >= 0.0)
                            {
                                g_r4.ownMinM = std::min(g_r4.ownMinM, r4OwnDist);
                                if (r4OwnDist < 3.0)
                                    ++g_r4.ownUnder3m;
                            }
                            if (g_rideFree.load(std::memory_order_relaxed) && sync::RideFreeClass(v.vehClass))   // wheeled ground classes only
                                v.drive.SetRideFreedom(sync::kRideTolM, sync::kTiltTolDeg);
                            else
                                v.drive.SetRideFreedom(0.0f, 0.0f);
                            sync::VehicleDriveOutput o = v.drive.Update(in);
                            {
                                // 0.4.22 shake meter: the error vectors the controller worked from (target - local, and the rotation that takes the local
                                // pose to the target), by the target's speed; a snapped frame restarts the history
                                const sync::DriveVec pe = { s.pos.x - in.localPos.x, s.pos.y - in.localPos.y, s.pos.z - in.localPos.z };
                                const sync::DriveVec re = sync::vq::Log(sync::vq::Mul(sync::vq::Normalize(s.rot), sync::vq::Conj(sync::vq::Normalize(in.localRot))));
                                v.shake.Add(in.dtMs, pe, re,
                                            std::sqrt((double)s.vel.x * s.vel.x + (double)s.vel.y * s.vel.y + (double)s.vel.z * s.vel.z), o.snap);
                            }
                            {
                                // R4 T2: what did the flag touch (the evidence we can read), what the gate decided, and the watch that compares this copy with its owner after the contact
                                if (in.contact)
                                {
                                    sync::TouchEvidence ev;
                                    ev.ownDistM = r4OwnDist;
                                    for (const auto& okv : g_vehs)
                                    {
                                        if (okv.first == id || !okv.second.handle || !okv.second.haveState)
                                            continue;
                                        const double ddx = (double)okv.second.st.k.x - st.k.x, ddy = (double)okv.second.st.k.y - st.k.y;
                                        const double dd = std::sqrt(ddx * ddx + ddy * ddy);
                                        if (ev.copyDistM < 0.0 || dd < ev.copyDistM)
                                            ev.copyDistM = dd;
                                    }
                                    ev.heightM = g_world->GetEntityHeightAboveGround(h);
                                    ev.speedLossMs = o.devMs;
                                    const sync::TouchKind kind = sync::ClassifyTouch(ev);
                                    g_r4.tally.Add(kind, !in.ownNear, ev.heightM, ev.speedLossMs);
                                    static int touchLogged = 0;
                                    if (touchLogged < 12)
                                    {
                                        ++touchLogged;
                                        PeerLog(false, "engine: vehicle id=%u: collision flag rose on the remote copy: touched %s (by the evidence: nearest car we control %.1f m, nearest other copy %.1f m, "
                                                "height above ground %.2f m, speed lost against the last command %.1f m/s, own speed %.1f m/s)%s", id, sync::TouchName(kind),
                                                ev.ownDistM, ev.copyDistM, (double)ev.heightM, ev.speedLossMs,
                                                std::sqrt((double)in.localVel.x * in.localVel.x + (double)in.localVel.y * in.localVel.y + (double)in.localVel.z * in.localVel.z),
                                                in.ownNear ? "" : "; no vehicle we control within the gate range: NO grace, the copy keeps following its owner");
                                    }
                                }
                                if (o.contactIgnored)
                                {
                                    if (o.contactIgnored == 1) ++g_r4.gatedEdges;
                                    else if (o.contactIgnored == 2) ++g_r4.gatedLoss;
                                    else ++g_r4.gatedKick;
                                    g_r4.watch.Open(id, sync::ContactWatch::kWorld, nowMs);
                                }
                                if (o.contactOpened)
                                    g_r4.watch.Open(id, o.contactOpened == 1 ? sync::ContactWatch::kFlag : (o.contactOpened == 2 ? sync::ContactWatch::kSpeedLoss :
                                                    (o.contactOpened == 4 ? sync::ContactWatch::kProximity : sync::ContactWatch::kKick)), nowMs);
                            }
                            if (t5Diag)
                            {
                                // R3 T5: drawn (the entity's position before this frame's commands) vs the owner's report at (now + link lag), the aim and the tracking.
                                auto d3 = [](const sync::DriveVec& a, const sync::DriveVec& b) {
                                    return std::sqrt((double)(a.x - b.x) * (a.x - b.x) + (double)(a.y - b.y) * (a.y - b.y) + (double)(a.z - b.z) * (a.z - b.z));
                                };
                                sync::PositionDiag::Frame pf;
                                pf.speedMps = std::sqrt((double)s.vel.x * s.vel.x + (double)s.vel.y * s.vel.y + (double)s.vel.z * s.vel.z);
                                pf.distM = leadDist;
                                pf.totalM = d3(in.localPos, t5Ideal.pos);
                                pf.targetM = d3(s.pos, t5Ideal.pos);
                                pf.trackM = o.posErrorM;
                                pf.unledMs = t5Unled;
                                pf.leadMs = v.leadMs;
                                pf.snapped = o.snap;
                                g_t5.pos.Add(pf);
                                g_r4.watch.Sample(id, nowMs, pf.totalM, o.posErrorM);   // R4 T2: the divergence check after EVERY contact (+0.5 / +1 / +2 s)
                                g_t5.lagSum += t5Lag;
                                ++g_t5.lagN;
                                g_t5.lagMax = std::max(g_t5.lagMax, t5Lag);
                            }
                            {
                                const uint32_t pu = v.drive.Stats().proximityUpgrades;
                                g_t5.proxUpgraded += pu - v.proxUpgradesSeen;
                                v.proxUpgradesSeen = pu;
                            }
                            // T4 (vehicle collisions): when this copy took an impulse inside the last frame (the contact the controller just
                            // detected), that is the OTHER car's response to the hit from the game's own physics. The controller's next command
                            // would pull it back to the stream, which still carries the pre-impact motion for the whole network latency: the copy
                            // ploughs back through the car it hit (ours) and hits it again. So the response becomes the copy's trajectory
                            // (VehicleInterpolator::ReplaceFrom: the stale stream is refused until it shows the impact), the grace the contact
                            // opened is ended, and this frame's commands are not written. impact_sync=0 leaves the grace as the only handling.
                            if (o.contactOpened && !o.snap && g_impactSync.load(std::memory_order_relaxed))
                            {
                                float wx = 0, wy = 0, wz = 0;
                                const sync::impact::AdoptOut ad =
                                    g_world->GetEntityRotationVelocity(h, wx, wy, wz)
                                        ? sync::impact::DecideAdopt(s.vel.x, s.vel.y, s.vel.z, in.localVel.x, in.localVel.y, wz,
                                                                  sync::vq::HeadingDeg(in.localRot) * (3.14159265358979323846 / 180.0), sync::impact::AdoptCfg())
                                        : sync::impact::AdoptOut();
                                if (ad.adopt)
                                {
                                    sync::VehicleSnapshot sn;
                                    sn.tMs = dispT;
                                    sn.pos = in.localPos;
                                    sn.rot = in.localRot;
                                    sn.vel = { (float)ad.vx, (float)ad.vy, (float)ad.vz };
                                    sn.hasAngVel = true;
                                    sn.angVel = { 0.0f, 0.0f, (float)ad.yawRate };   // the yaw only: the frame of the native's pitch / roll is unverified
                                    AdoptVehInterp(id, sn, kAdoptGuardMs, s.vel);
                                    v.drive.EndGrace(kAdoptCooldownMs);
                                    o.setVelocity = o.setAngularVelocity = o.setQuaternion = false;
                                    ++g_t4.adopted;
                                    if (!g_r4.watch.Active(id))
                                        g_r4.watch.Open(id, sync::ContactWatch::kAdoption, nowMs);
                                    v.divCheckAtMs = nowMs + 500.0;
                                    static int adoptLogged = 0;
                                    if (adoptLogged < 12)
                                    {
                                        ++adoptLogged;
                                        PeerLog(false, "engine: vehicle id=%u: contact on the remote copy -- its own response (%.1f m/s change, yaw rate %.2f) "
                                                "adopted as its trajectory; the stream is held back until it shows the impact (max %.0f ms)", id, ad.dv,
                                                ad.yawRate, kAdoptGuardMs);
                                    }
                                }
                                else
                                    ++g_t4.adoptRefused;
                            }
                            if (v.divCheckAtMs > 0.0 && nowMs >= v.divCheckAtMs)
                            {
                                // the divergence estimate: this copy against its controller's stream 500 ms after the contact
                                v.divCheckAtMs = 0.0;
                                ++g_t4.divN;
                                g_t4.divSum += o.posErrorM;
                                g_t4.divMax = std::max(g_t4.divMax, (double)o.posErrorM);
                            }
                            v.errWin.Add(o.posErrorM, o.rotErrorDeg, sync::LateralErrorM(s.pos, s.rot, in.localPos), s.mode, o.snap);   // Sync D
                            {
                                // 0.5.7: the copy at rest against the commands of the previous frame (read-backs: in.localRot / localPos / localVel)
                                sync::VehRestDiag::Frame rf;
                                rf.dtMs = v.restLastMs > 0.0 ? nowMs - v.restLastMs : 0.0;
                                rf.targetSpeed = std::sqrt((double)s.vel.x * s.vel.x + (double)s.vel.y * s.vel.y + (double)s.vel.z * s.vel.z);
                                double dyaw = sync::vq::HeadingDeg(s.rot) - sync::vq::HeadingDeg(in.localRot);
                                while (dyaw > 180.0) dyaw -= 360.0;
                                while (dyaw <= -180.0) dyaw += 360.0;
                                rf.yawErrDeg = std::fabs(dyaw);
                                const sync::DriveVec ut = sync::vq::Rotate(s.rot, sync::DriveVec{ 0.0f, 0.0f, 1.0f });
                                const sync::DriveVec ul = sync::vq::Rotate(in.localRot, sync::DriveVec{ 0.0f, 0.0f, 1.0f });
                                const double cosT = std::max(-1.0, std::min(1.0, (double)ut.x * ul.x + (double)ut.y * ul.y + (double)ut.z * ul.z));
                                rf.tiltErrDeg = std::acos(cosT) * (180.0 / 3.14159265358979323846);
                                const double ex = (double)s.pos.x - in.localPos.x, ey = (double)s.pos.y - in.localPos.y;
                                rf.horizErrM = std::sqrt(ex * ex + ey * ey);
                                rf.vertErrM = std::fabs((double)s.pos.z - in.localPos.z);
                                rf.headingDeg = sync::vq::HeadingDeg(in.localRot);
                                rf.haveCmdYaw = v.restCmdYaw;
                                rf.cmdYawRate = v.restYawRate;
                                rf.haveCmdVel = v.restCmdVel;
                                if (v.restCmdVel)
                                {
                                    const double dx = (double)in.localVel.x - v.restVel.x, dy = (double)in.localVel.y - v.restVel.y, dz = (double)in.localVel.z - v.restVel.z;
                                    rf.velDevMs = std::sqrt(dx * dx + dy * dy + dz * dz);
                                }
                                rf.snapped = o.snap;
                                v.restDiag.Add(rf);
                                v.restLastMs = nowMs;
                                // what goes out this frame (after the adoption above may have cancelled it): the next frame's reference
                                v.restCmdYaw = o.setAngularVelocity && !o.setQuaternion && !o.snap;
                                v.restYawRate = o.angularVelocity.z;
                                v.restCmdVel = o.setVelocity && !o.snap;
                                v.restVel = o.velocity;
                            }
                            if (o.contactOpened)
                            {
                                if (o.contactOpened == 1)
                                    ++g_v4.contactsFlag;
                                else if (o.contactOpened == 2)
                                    ++g_v4.contactsDecel;
                                else if (o.contactOpened == 4)
                                    ++g_t5.proxOpened;                  // R3 T5: the proximity trigger (the copy was about to overlap our car)
                                else
                                    ++g_v4.contactsKick;
                                static int graceLogged = 0;
                                if (graceLogged < 12)
                                {
                                    ++graceLogged;
                                    PeerLog(false, "engine: vehicle id=%u: contact on the remote copy (%s): soft correction for %.0f ms "
                                            "(speed %.1f m/s, error %.2f m, deviation from the last command %.1f m/s)", id,
                                            o.contactOpened == 1 ? "collision flag" : (o.contactOpened == 2 ? "speed loss" : (o.contactOpened == 4 ? "proximity: about to overlap our car" : "velocity kick")),
                                            sync::VehicleDriveConfig().contactGraceMs, std::sqrt((double)in.localVel.x * in.localVel.x + (double)in.localVel.y * in.localVel.y +
                                                                                        (double)in.localVel.z * in.localVel.z), (double)o.posErrorM,
                                            (double)o.devMs);
                                }
                            }
                            {
                                // Sync B: the contact diagnostics (printed every 10 s by ReportV4)
                                sync::ContactFrame cf;
                                cf.flagRead = flagRead;
                                cf.flag = flagNow;
                                cf.edge = in.contact;
                                cf.devMs = o.devMs;
                                cf.graceActive = o.graceActive;
                                cf.opened = o.contactOpened;
                                cf.overwrote = o.setVelocity;
                                if (haveLc && !mine)
                                {
                                    const double dx = (double)in.localPos.x - lcx, dy = (double)in.localPos.y - lcy, dz = (double)in.localPos.z - lcz;
                                    cf.localCarDistM = (float)std::sqrt(dx * dx + dy * dy + dz * dz);
                                }
                                v.contact.Add(cf);
                                v.contactLastPosX = in.localPos.x;
                                v.contactLastPosY = in.localPos.y;
                                v.contactLastPosZ = in.localPos.z;
                                v.contactPosValid = true;
                            }
                            // W9 item 7: bikes carry their lean in the quaternion (no bike-specific handling exists; none is invented): the
                            // roll error against the sender's goes into a per-bike 10 s line so the next live test shows what is wrong.
                            if (v.vehClass == -2)
                            {
                                v.vehClass = g_world->GetVehicleClass(h);
                                if (sync::IsBikeClass(v.vehClass))
                                    PeerLog(false, "engine: vehicle id=%u is a bike (class %d): a roll line is logged every 10 s", id, v.vehClass);
                            }
                            if (sync::IsBikeClass(v.vehClass))
                                v.roll.Add(sync::vq::RollDeg(in.localRot) - sync::vq::RollDeg(s.rot),
                                           std::sqrt((double)in.localVel.x * in.localVel.x + (double)in.localVel.y * in.localVel.y +
                                                     (double)in.localVel.z * in.localVel.z));
                            if (o.snap)
                            {
                                g_world->SetEntityCoordsNoOffset(h, o.snapPos.x, o.snapPos.y, o.snapPos.z);
                                g_world->SetEntityQuaternion(h, o.snapRot.x, o.snapRot.y, o.snapRot.z, o.snapRot.w);
                                ++g_v4.vehSnaps;
                            }
                            else if (o.setQuaternion)
                                g_world->SetEntityQuaternion(h, o.rot.x, o.rot.y, o.rot.z, o.rot.w);
                            if (o.setVelocity)
                                g_world->SetEntityVelocity(h, o.velocity.x, o.velocity.y, o.velocity.z);
                            if (o.setAngularVelocity)
                            {
                                // 0.4.22: the setter takes the BODY frame (libs/sync/vehicle_shake.h); the controller's world vector went in unchanged
                                // before. With the fix on the frame follows the judge; veh_shake_fix=0 keeps veh_angframe's choice (default world).
                                const bool bodyFrame = g_shakeFix.load(std::memory_order_relaxed) ? g_angJudge.UseBody()
                                                                                                 : g_vehAngBody.load(std::memory_order_relaxed);
                                const sync::DriveVec& w = bodyFrame ? o.angularVelocityBody : o.angularVelocity;
                                g_world->SetEntityAngularVelocity(h, w.x, w.y, w.z);
                                ++g_shk.copyAngWrites;
                                ++(bodyFrame ? g_shk.copyAngBody : g_shk.copyAngWorld);
                                if (++v.angWrites % kShakeAngCheckEvery == 0)
                                {
                                    // the read-back of the evidence: the world vector the game now reports against what the frames predict
                                    float rx = 0, ry = 0, rz = 0;
                                    if (g_world->GetEntityRotationVelocity(h, rx, ry, rz))
                                        g_angJudge.Add(sync::shake::ClassifyAngWrite(o.setQuaternion ? o.rot : (o.snap ? o.snapRot : in.localRot), w, { rx, ry, rz }));
                                }
                            }
                            // Steering (sync plan W2 D): the sender fills VehicleUpdate.steer from its yaw rate
                            // (sync::EstimateSteer). The wire is -1 left .. +1 right and SET_VEHICLE_STEER_BIAS
                            // is -1 right .. +1 left, so the sign flips. Every frame, the native holds only while
                            // called. Whether it turns a puppet's visible wheels is INFERRED (verify live).
                            if (o.setSteering)
                            {
                                // 0.4.30 veh_steer_field: the CVehicle steering angle when it resolved (the bias does nothing on a
                                // script-ped-driven copy); first the read-back of last frame's write: held, or overwritten by the game
                                const bool fieldOn = g_world->VehicleFieldsReady();
                                if (fieldOn && std::isfinite(v.steerFieldWritten))
                                {
                                    float before = 0.0f;
                                    if (g_world->GetVehicleSteerAngle(h, before))
                                        ++(std::fabs(before - v.steerFieldWritten) < 0.01f ? g_vis.steerFieldHeld : g_vis.steerFieldOverwritten);
                                }
                                const sync::CopySteer how = sync::ApplyCopySteer(*g_world, h, o.steering, fieldOn);
                                v.steerFieldWritten = how == sync::CopySteer::Field ? sync::SteerAngleFromWire(o.steering) : NAN;
                                if (how == sync::CopySteer::Field) ++g_vis.steerField;
                                else if (fieldOn && how == sync::CopySteer::Bias) ++g_vis.steerFieldRefused;
                                if (fieldOn)
                                {
                                    float brake = 0.0f;
                                    bool hand = false;
                                    const float sp = std::sqrt(in.localVel.x * in.localVel.x + in.localVel.y * in.localVel.y);
                                    if (sp > 3.0f && g_world->GetVehicleCopyControls(h, brake, hand))
                                    {
                                        ++g_vis.copyMoving;
                                        if (brake > 0.5f) ++g_vis.copyBrake;
                                        if (hand) ++g_vis.copyHandbrake;
                                    }
                                }
                                const bool steerOk = how != sync::CopySteer::Failed;
                                if (steerOk)
                                {
                                    const float w = -o.steering;
                                    if (g_vis.steerApplied == 0 || w < g_vis.steerAppliedMin) g_vis.steerAppliedMin = w;
                                    if (g_vis.steerApplied == 0 || w > g_vis.steerAppliedMax) g_vis.steerAppliedMax = w;
                                    g_vis.steerAppliedSum += w;
                                    ++g_vis.steerApplied;
                                }
                                else
                                    ++g_vis.steerApplyFail;
                            }
                            else
                                v.steerFieldWritten = NAN;     // no write this frame: the next read-back has nothing to compare
                            v.lastDriveMs = nowMs;
                        }
                    }
                }
                else if (g_impactSync.load(std::memory_order_relaxed) && v.handle)
                    FeedImpactTracker(id, v, nowMs);
                // Remote occupants: warp each into its seat (wire seat s = GTA seat s - 1). The retry gap is
                // 100 ms for the car we sit in and the cars within kSeatNearM of us (a driver seat that stays
                // empty for half a second reads as "nobody is driving" to whoever stands there), 500 ms for the
                // rest. A seat whose occupant has no ped here yet is logged (rate limited).
                const float seatDx = st.k.x - lpx, seatDy = st.k.y - lpy, seatDz = st.k.z - lpz;
                const bool nearUs = mine || (haveLp && seatDx * seatDx + seatDy * seatDy + seatDz * seatDz <=
                                                           kSeatNearM * kSeatNearM);
                const double seatRetryMs = nearUs ? kSeatRetryNearMs : kSeatRetryFarMs;
                for (size_t s = 0; s < proto::kVehSeats; ++s)
                {
                    const uint16_t pid = st.seats[s];
                    if (pid == proto::kNoPlayer || pid == me)
                        continue;
                    auto pit = g_peers.find(pid);
                    if (pit == g_peers.end() || !pit->second.ped)
                    {
                        double& lastLog = g_seatNoPedLogMs[pid];
                        if (nowMs - lastLog >= 5000.0)
                        {
                            lastLog = nowMs;
                            PeerLog(true, "engine: vehicle id=%u seat %u: seat target has no ped (player %u %s)", id,
                                    (unsigned)s, (unsigned)pid, pit == g_peers.end() ? "is not known here" : "has no ped yet");
                        }
                        continue;
                    }
                    if (pit->second.deadShown || pit->second.gone)
                        continue;
                    Peer& pe = pit->second;
                    if (nowMs - pe.seatTryMs < seatRetryMs)
                        continue;
                    const int gtaSeat = (int)s - 1;
                    if (pe.seatedVeh == (int)id && pe.seatedSeat == (int)s &&
                        g_world->GetPedInVehicleSeat(h, gtaSeat) == pe.ped)
                        continue;                      // already there
                    if (pe.seatedVeh == (int)id && pe.seatedSeat == (int)s)
                    {
                        // Sync D diagnosis: we warped this ped into this seat and it is no longer in it. Where is it? (the W9 re-warp
                        // counters say how often, not why: GTA's shuffle to another seat, another car, or out of every car)
                        const int32_t nowIn = g_world->GetVehiclePedIsIn(pe.ped);
                        const sync::SeatDisplacement dsp = sync::ClassifyDisplacement(true, true, nowIn == h && nowIn != 0,
                                                                                    g_world->IsPedInVehicle(pe.ped));
                        switch (dsp)
                        {
                        case sync::SeatDisplacement::OtherSeat: ++g_syncD.displacedOtherSeat; break;
                        case sync::SeatDisplacement::OtherVehicle: ++g_syncD.displacedOtherVeh; break;
                        case sync::SeatDisplacement::Outside: ++g_syncD.displacedOutside; break;
                        default: ++g_syncD.displacedUnknown; break;
                        }
                        static int dispLogged = 0;
                        if (dispLogged < 10)
                        {
                            ++dispLogged;
                            int seatNow = -9;
                            if (dsp == sync::SeatDisplacement::OtherSeat)
                                for (int q = -1; q < (int)proto::kVehSeats - 1 && seatNow == -9; ++q)
                                    if (g_world->GetPedInVehicleSeat(h, q) == pe.ped)
                                        seatNow = q + 1;
                            PeerLog(true, "engine: vehicle id=%u: player %u was warped into seat %u and is %s (%s)", id, (unsigned)pid, (unsigned)s,
                                    dsp == sync::SeatDisplacement::OtherSeat ? "now in another seat of the same car" :
                                    dsp == sync::SeatDisplacement::OtherVehicle ? "now in another vehicle" : "no longer in any vehicle",
                                    dsp == sync::SeatDisplacement::OtherSeat ? (std::string("wire seat ") + std::to_string(seatNow)).c_str() : "a re-warp follows");
                        }
                    }
                    // T1 seats: the seat warp is the BOUNDED FALLBACK of the server's enter intent. A ped the real TASK_ENTER_VEHICLE already
                    // put into this seat is only recorded; a ped whose task is still running is left to it until sync::PuppetSeatReplay
                    // allows the warp (confirmGraceMs after the server's Seated, or the task ended / timed out); right after an Exited notice a
                    // stale list may not seat it again. With HKCU seat_intent=0 the W6 / W9 rule below stands.
                    sync::WarpWhy warpWhy = sync::WarpWhy::None;
                    const bool seatIntent = g_seatIntentOn.load();
                    if (seatIntent)
                    {
                        if (g_world->IsPedInVehicle(pe.ped) && g_world->GetPedInVehicleSeat(h, gtaSeat) == pe.ped)
                        {
                            pe.seatedVeh = (int)id;    // seated by the game's own task: nothing to warp
                            pe.seatedSeat = (int)s;
                            pe.aimTask = false;
                            pe.emote.Clear();
                            SyncPeerEmoteDict(pe);
                            g_world->SetPedConfigFlag(pe.ped, 184, true);
                            continue;
                        }
                        const sync::WarpDecision wd = g_seats.Puppet(pid).AllowsSeatWarp((int)id, (int)s, nowMs);
                        if (!wd.allow)
                            continue;
                        warpWhy = wd.why;
                    }
                    // W6: a ped walking to this car with TASK_ENTER_VEHICLE (TickPeerTierB) is left to the walk; the
                    // old seat warp takes over once it has had sync::kEnterTaskMaxMs (or the ped is already inside).
                    // W9 item 1: ... and no longer once the sender's move says it is not entering (it was put into the seat).
                    if (!seatIntent && pe.enterTaskMs > 0.0 && pe.seatedVeh < 0 && pe.enterVeh == (int)id &&
                        !g_world->IsPedInVehicle(pe.ped) &&
                        !sync::EnterWarpDue(true, false, nowMs - pe.enterTaskMs, pe.lastPed.move == proto::kMoveEnteringVehicle))
                        continue;
                    pe.seatTryMs = nowMs;
                    const int32_t occupant = g_world->GetPedInVehicleSeat(h, gtaSeat);
                    if (occupant && occupant == lp)
                        continue;                      // our own ped holds it here: the server will sort it out
                    // W11 (W9 evidence): a warp of a ped that is already seated somewhere (here or in a car) is a RE-warp, the
                    // visible "passenger teleports"; counted per 10 s so the next live test shows how often it still happens.
                    const bool wasSeated = pe.seatedVeh >= 0 || g_world->IsPedInVehicle(pe.ped);
                    // W9 item 2: GTA moving the puppet out of the seat the server gave it (the auto-shuffle to the driver's
                    // seat; PCF 184 below should stop it) made this loop re-warp it every 100 ms. A re-warp to the same seat
                    // now waits on a gap that doubles (400 ms, 800, ... 4 s) and starts over after 5 s of calm; a different
                    // seat or vehicle (the server moved the player) goes at once.
                    if (!pe.seatLimiter.Allow(nowMs, (int)id, (int)s, wasSeated))
                    {
                        ++g_v4.seatRewarpsHeld;
                        continue;
                    }
                    if (pe.seatedVeh < 0 && !g_world->IsPedInVehicle(pe.ped))
                        ClearPedTasksNow(pe.ped);      // drop the gait task before the warp
                    pe.emote.Clear();
                    SyncPeerEmoteDict(pe);
                    {
                        // W9 item 2: PCF 184 (PreventAutoShuffleToDriversSeat) on the PUPPET, as on the local ped
                        // (TickLocalVehicle): a passenger puppet beside an empty driver's seat is otherwise shuffled over by GTA.
                        // Flag id 184 is INFERRED; set before every warp (a warp resets nothing, but it is one native call).
                        const bool pcfOk = g_world->SetPedConfigFlag(pe.ped, 184, true);
                        static bool pcfLogged = false;
                        if (!pcfLogged)
                        {
                            pcfLogged = true;
                            PeerLog(false, "engine: vehicle: PCF 184 (PreventAutoShuffleToDriversSeat) set on remote puppets at seat time: %s",
                                    pcfOk ? "called" : "NOT callable");
                        }
                    }
                    g_world->SetPedIntoVehicle(pe.ped, h, gtaSeat);
                    if (seatIntent)
                        g_seats.Puppet(pid).NoteWarp(warpWhy);   // T1: counted by reason (the 10 s "seats (T1)" line)
                    pe.enterTaskMs = 0.0;              // the walk (if any) is over: the ped is seated
                    pe.enterVeh = -1;
                    pe.seatedVeh = id;
                    pe.seatedSeat = (int)s;
                    pe.aimTask = false;
                    ++g_v4.seatWarps;
                    if (wasSeated)
                        ++g_v4.seatRewarps;
                    if (wasSeated)
                    {
                        static double rewarpLoggedMs = -1e9;
                        if (nowMs - rewarpLoggedMs >= 5000.0)
                        {
                            rewarpLoggedMs = nowMs;
                            PeerLog(true, "engine: vehicle id=%u seat %u: re-warp of player %u (already seated; %u held back so far by the limiter)",
                                    id, (unsigned)s, (unsigned)pid, (unsigned)pe.seatLimiter.Suppressed());
                        }
                    }
                }
                ++it;
            }
            {
                // Sync B: remote copies whose origins are close to each other this frame (two copies the senders drove apart on their
                // own screens that overlap here). At most 64 entries; pairs under 3 m only are noted. No native.
                static std::vector<NetVeh*> driven;
                driven.clear();
                for (auto& kv : g_vehs)
                    if (kv.second.contactPosValid)
                        driven.push_back(&kv.second);
                for (size_t a = 0; a < driven.size(); ++a)
                    for (size_t b = a + 1; b < driven.size(); ++b)
                    {
                        const double dx = (double)driven[a]->contactLastPosX - driven[b]->contactLastPosX;
                        const double dy = (double)driven[a]->contactLastPosY - driven[b]->contactLastPosY;
                        const double dz = (double)driven[a]->contactLastPosZ - driven[b]->contactLastPosZ;
                        const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
                        if (d < 3.0)
                        {
                            driven[a]->contact.AddPuppetNear((float)d);
                            driven[b]->contact.AddPuppetNear((float)d);
                        }
                    }
            }
            // Peds seated in a vehicle whose seat list no longer has them (or that is gone): on foot.
            for (auto& kv : g_peers)
            {
                Peer& pe = kv.second;
                if (pe.seatedVeh < 0)
                    continue;
                auto vit = g_vehs.find((uint16_t)pe.seatedVeh);
                bool still = false;
                if (vit != g_vehs.end() && vit->second.handle && !vit->second.removed)
                    for (size_t s = 0; s < proto::kVehSeats; ++s)
                        still = still || vit->second.st.seats[s] == kv.first;
                if (!still)
                    UnseatPeer(pe, vit != g_vehs.end() && !vit->second.removed ? vit->second.handle : 0);
            }
        }
        CommitNet(std::vector<int>(), removedVehs);
        FlushPeerLog();
    }

    // ---- F and G vehicle entry (sync plan W2 A4-A6) -------------------------------------------------
    // GTA's own entry (TASK_ENTER_VEHICLE) jacks a seat somebody holds and walks to the driver's door by
    // itself, so for NETWORKED cars the F key is taken over: DISABLE_CONTROL_ACTION on INPUT_ENTER while a
    // networked car is within kEntryReachM, the press read back from the disabled control, and the seat
    // chosen by sync::PickEntry (F: the driver's seat when free and not someone else's, else the nearest free
    // passenger seat; G: the nearest free passenger seat; never a seat that is taken). The ped walks to the
    // seat's door (TASK_GO_STRAIGHT_TO_COORD, re-picked every frame) and is put into the seat with
    // SET_PED_INTO_VEHICLE once within kEntryWarpM: no door animation, no jack, and never TASK_ENTER_VEHICLE.
    // The link then sends Enter and the server has the last word. Cars that are not networked (ambient,
    // local) keep GTA's default entry: with no networked car in reach nothing here touches the controls.
    // Control ids 23 (INPUT_ENTER, F) and 47 (INPUT_DETONATE, G) are INFERRED: confirm live.
    constexpr int kControlEnter = 23, kControlDetonate = 47;
    constexpr float kEntryReachM = 8.0f;        // a networked car this close (to its origin) takes the keys over
    constexpr float kEntryWalkMaxM = 5.0f;      // the farthest door the ped is walked to
    constexpr float kEntryWarpM = 1.6f;         // this close to the door (seat bone): put into the seat
    constexpr float kEntryRunM = 3.0f;          // T1: farther than this from the door the entry task is issued at a run (the wire's run flag)
    constexpr double kEntryWalkMaxMs = 4000.0;  // give up a walk that has not arrived by then
    constexpr double kEntryReissueMs = 1500.0;  // the go-to task is re-issued at least this often (it has a 3 s timeout)
    // Seat bones by wire seat (driver, front passenger, rear left, rear right, then the third and fourth
    // rows). INFERRED names; a model without them falls back to fixed offsets from the vehicle's origin.
    const char* const kSeatBones[sync::kEntrySeats] = {
        "seat_dside_f", "seat_pside_f", "seat_dside_r", "seat_pside_r",
        "seat_dside_r1", "seat_pside_r1", "seat_dside_r2", "seat_pside_r2"
    };

    struct EntryCar
    {
        sync::EntryCandidate c;
        int32_t handle = 0;
        uint32_t model = 0;
        float x = 0, y = 0, z = 0;   // the vehicle's origin now
    };

    // The seat bones and seat count of a model, read once (script tick).
    const SeatInfo& SeatInfoFor(int32_t veh, uint32_t model)
    {
        SeatInfo& si = g_seatInfo[model];
        if (!si.read)
        {
            si.read = true;
            int found = 0;
            for (int s = 0; s < sync::kEntrySeats; ++s)
            {
                si.bone[s] = g_world->GetEntityBoneIndexByName(veh, kSeatBones[s]);
                if (si.bone[s] >= 0)
                    ++found;
            }
            // 0.4.22: both seat-count natives (the vehicle's own count excludes the driver, the model's includes him); the larger wins.
            const int passengers = g_world->GetVehicleMaxPassengers(veh);
            const int modelSeats = g_world->GetVehicleModelNumberOfSeats(model);
            const sync::SeatCount sc = sync::ResolveSeatCount(passengers, modelSeats);
            si.maxSeats = sc.seats;
            GLOGI("engine: vehicle: model 0x%08x has %d seats (GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS %d%s, GET_VEHICLE_MODEL_NUMBER_OF_SEATS %d%s)%s%s; "
                  "%d of 8 seat bones found%s", (unsigned)model, si.maxSeats, passengers, passengers >= 0 ? "" : " (not callable)", modelSeats,
                  modelSeats >= 1 ? "" : " (not callable)", sc.disagree ? ", THE TWO DISAGREE: the larger is used" : "",
                  !sc.fromVehicle && !sc.fromModel ? ", neither callable: assumed 2" : "", found,
                  found == 0 ? " -- door positions use fixed offsets" : "");
        }
        return si;
    }

    // Where one stands to use each seat of `car` (world), from the seat bones; a seat without a bone gets a
    // fixed offset (x +-0.45 m by side, y 0.4 m forward minus 0.9 m per row) rotated by the vehicle's quaternion.
    void FillEntryDoors(EntryCar& car)
    {
        const SeatInfo& si = SeatInfoFor(car.handle, car.model);
        const int offered = sync::SeatsOffered(si.maxSeats, g_vehRearSeats.load(std::memory_order_relaxed));   // 0.4.22: veh_rear_seats=0 stops at the front passenger
        car.c.maxSeats = offered;
        sync::DriveQuat q;
        bool haveQ = false, triedQ = false;
        for (int s = 0; s < offered && s < sync::kEntrySeats; ++s)
        {
            float x = car.x, y = car.y, z = car.z;
            if (si.bone[s] >= 0 && g_world->GetWorldPositionOfEntityBone(car.handle, si.bone[s], x, y, z))
            {
                car.c.door[s] = { x, y, z };
                continue;
            }
            if (!triedQ)
            {
                triedQ = true;
                float qx, qy, qz, qw;
                if (g_world->GetEntityQuaternion(car.handle, qx, qy, qz, qw))
                {
                    q = sync::vq::Normalize({ qx, qy, qz, qw });
                    haveQ = true;
                }
            }
            sync::DriveVec off{ (s & 1) ? 0.45f : -0.45f, 0.4f - 0.9f * (float)(s / 2), 0.0f };
            if (haveQ)
                off = sync::vq::Rotate(q, off);
            car.c.door[s] = { car.x + off.x, car.y + off.y, car.z + off.z };
        }
    }

    // The networked cars whose origin is within kEntryReachM of `lpPos`, with who sits where (the server's
    // seat list) and whether they are locked. Doors are filled by FillEntryDoors only when a key needs them.
    void GatherEntryCars(const float lpPos[3], std::vector<EntryCar>& out)
    {
        for (const auto& kv : g_vehs)                  // the game thread's own map: no lock
        {
            const NetVeh& v = kv.second;
            if (!v.handle || v.removed || !v.present || !v.haveState || !v.spawnedModel)
                continue;
            if (std::fabs(v.st.k.x - lpPos[0]) > 40.0f || std::fabs(v.st.k.y - lpPos[1]) > 40.0f)
                continue;                              // cheap reject by the server position before the native
            EntryCar car;
            car.handle = v.handle;
            car.model = v.spawnedModel;
            if (!g_world->GetEntityCoords(v.handle, car.x, car.y, car.z))
                continue;
            const float dx = car.x - lpPos[0], dy = car.y - lpPos[1], dz = car.z - lpPos[2];
            if (dx * dx + dy * dy + dz * dz > kEntryReachM * kEntryReachM)
                continue;
            car.c.netId = (int)kv.first;
            const double bookNow = NowMs();
            for (size_t s = 0; s < proto::kVehSeats; ++s)
            {
                car.c.seats[s] = v.st.seats[s];
                // T1 seats: a seat somebody else is walking to counts as taken (the server's reservation), so F never picks a seat that is
                // about to be refused; our own reservation stays ours.
                if (car.c.seats[s] == sync::kEntryNobody && g_seatIntentOn.load(std::memory_order_relaxed))
                {
                    const uint16_t holder = g_seats.Book().Holder((int)kv.first, (int)s, bookNow);
                    if (holder != sync::kSeatNobody && holder != join::MyId())
                        car.c.seats[s] = holder;
                }
            }
            car.c.locked = (v.st.flags & proto::kVehLocked) != 0;
            // VehicleState.ownerId (v5): the pick avoids the driver's seat of a car somebody else owns (PickEntry),
            // and the server's NotOwner (12) is the authority if a stale view picked it anyway.
            car.c.ownerId = v.st.ownerId;
            car.c.keyHolder = g_dsState.keys.Has((int)kv.first);   // 0.4.22: a car we drove last may be entered locked (the server decides: spawner / last driver)
            out.push_back(car);
        }
    }

    void ApplyLocalEnterOut(const sync::LocalEnterOut& o, int32_t lp, double nowMs);   // below, with EntryNotice

    // T1 seats: our own entry is given up (the player moved off, ragdolled, died, pressed F again, the car went away): the flow clears the
    // task and tells the server (CancelIntent) when it had got that far. No effect when no entry is under way.
    void AbortLocalEnter(int32_t lp)
    {
        if (!g_localEnter.Active())
            return;
        sync::LocalEnterIn in;
        in.nowMs = NowMs();
        in.abort = true;
        ApplyLocalEnterOut(g_localEnter.Tick(in), lp, in.nowMs);
    }

    // Ends a walk in progress: the go-to task, if one was issued, is blended out; an entry by the server's intent is aborted.
    void CancelEntryWalk(int32_t lp)
    {
        AbortLocalEnter(lp);
        if (g_entryWalk.active && g_entryWalk.tasked && lp)
            g_world->ClearPedTasks(lp);
        g_entryWalk = EntryWalk();
        g_entryGuard.OnCancel();
    }

    // The chat line for an F / G that found nothing (one per second at most).
    void EntryNotice(const char* text, double nowMs)
    {
        if (!text || !*text || nowMs - g_entryChatMs < 1000.0)
            return;
        g_entryChatMs = nowMs;
        game::Overlay::PushSystemLine(text);
        ++g_v4.entryRefused;
    }

    // T1 seats: applies what LocalEnterFlow::Tick returned (the natives and the wire; the decisions are the flow's). Script tick.
    //   SendIntent / SendCancel  VehicleCmd EnterIntent / CancelIntent (answered by HandleIntentResult / the Cancelled notice)
    //   IssueEnter               GTA's own TASK_ENTER_VEHICLE for the reserved seat (flags 1: no jack flag, never takes a held seat)
    //   FallbackWarp             the task never started / stopped without a seat / ran out of time: the BOUNDED teleport-in
    void ApplyLocalEnterOut(const sync::LocalEnterOut& o, int32_t lp, double nowMs)
    {
        const int netId = g_localEnter.NetId(), seat = g_localEnter.Seat();
        int32_t h = 0;
        if (netId >= 0)
        {
            const auto it = g_vehs.find((uint16_t)netId);
            if (it != g_vehs.end() && !it->second.removed)
                h = it->second.handle;
        }
        bool fellBack = false;
        for (const sync::LocalAct a : { o.act, o.also })
        {
            switch (a)
            {
            case sync::LocalAct::None:
                break;
            case sync::LocalAct::SendIntent:
            case sync::LocalAct::SendCancel:
            {
                proto::VehicleCmd c;
                c.op = a == sync::LocalAct::SendIntent ? proto::VehicleOp::EnterIntent : proto::VehicleOp::CancelIntent;
                c.vehId = (uint16_t)netId;
                c.arg = a == sync::LocalAct::SendIntent ? sync::PackIntentArg(seat, g_localEnter.Run()) : 0u;
                const bool sent = join::SendVehicleCmd(c);
                ++g_v4.vehCmds;
                if (a == sync::LocalAct::SendIntent) ++g_t1.intentsSent; else ++g_t1.cancelsSent;
                GLOGI("engine: seats: %s vehicle id=%d seat %d (%s) %s", a == sync::LocalAct::SendIntent ? "EnterIntent" : "CancelIntent", netId, seat,
                      g_localEnter.Run() ? "run" : "walk", sent ? "sent" : "NOT sent (no session)");
                break;
            }
            case sync::LocalAct::IssueEnter:
            {
                const bool ok = lp && h && g_world->TaskEnterVehicle(lp, h, o.timeoutMs, seat - 1, g_localEnter.Run() ? 2.0f : 1.0f, 1);
                if (!ok) ++g_t1.localIssueFailed;
                GLOGI("engine: seats: the server reserved seat %d of vehicle id=%d: TASK_ENTER_VEHICLE %s (%s, limit %d ms)", seat, netId,
                      ok ? "issued" : "NOT issued (native not callable or the car is not here): the fallback warp follows", g_localEnter.Run() ? "run" : "walk",
                      o.timeoutMs);
                break;
            }
            case sync::LocalAct::ClearTask:
                if (lp) g_world->ClearPedTasks(lp);
                break;
            case sync::LocalAct::FallbackWarp:
            {
                fellBack = true;
                if (!lp || !h)
                    break;
                g_world->ClearPedTasks(lp);
                const int32_t holder = g_world->GetPedInVehicleSeat(h, seat - 1);
                const bool ok = (!holder || holder == lp) && g_world->SetPedIntoVehicle(lp, h, seat - 1);
                g_localSeatAtMs = 0;                   // read the real seat next tick; the link then sends Enter
                ++g_v4.entryPicks;
                g_entryGuard.OnSeated(nowMs);
                GLOGW("engine: seats: FALLBACK WARP into seat %d of vehicle id=%d (%s): the enter task %s: %s", seat, netId,
                      o.why == sync::LocalWhy::NeverStarted ? "never started" : o.why == sync::LocalWhy::EndedNoSeat ? "stopped without a seat" : "ran out of time",
                      o.why == sync::LocalWhy::TimedOut ? "(limit reached)" : "", ok ? "SET_PED_INTO_VEHICLE called" : "SET_PED_INTO_VEHICLE FAILED or the seat is held");
                break;
            }
            }
        }
        if (o.why == sync::LocalWhy::Refused)
        {
            const char* text = sync::LocalVehicleLink::RefusalText(o.refusal);
            EntryNotice(*text ? text : "The server refused that vehicle seat.", nowMs);
            GLOGI("engine: seats: entry refused by the server (%s)", VehResultName(o.refusal));
        }
        else if (o.why == sync::LocalWhy::NoAnswer)
            GLOGW("engine: seats: no answer to our EnterIntent within %.0f ms: entry dropped", sync::LocalEnterCfg().answerWaitMs);
        else if (o.why == sync::LocalWhy::ServerCancelled)
            GLOGI("engine: seats: the server cancelled our reservation (expired, replaced or the car went): entry dropped");
        if (!g_localEnter.Active())
        {
            g_entryWalk = EntryWalk();                 // the entry is over, one way or the other
            if (!fellBack)
                g_entryGuard.OnCancel();
        }
    }

    // Runs every script tick on the connected, entered player, BEFORE TickLocalVehicle.
    void TickVehicleEntry(int32_t lp, int32_t lpVeh, bool dead, double nowMs)
    {
        if (lp && lpVeh && !dead && g_localEnter.Active())
        {
            // T1 seats: the game's own TASK_ENTER_VEHICLE seated us (GET_VEHICLE_PED_IS_IN names the car): done, no warp. The link's Enter follows.
            sync::LocalEnterIn in;
            in.nowMs = nowMs;
            in.seatedNow = true;
            ApplyLocalEnterOut(g_localEnter.Tick(in), lp, nowMs);
            g_entryGuard.OnSeated(nowMs);
            g_entryWalk = EntryWalk();
            GLOGI("engine: seats: seated by the game's own enter task (no warp)");
        }
        if (!lp || lpVeh || dead)
        {
            // W9 item 3: just seated by our press: GTA's own INPUT_ENTER must not consume the same press (or the next
            // one a moment later) as another entry / exit. Whether 23 is the control that did it is INFERRED (counted).
            if (lp && lpVeh && !dead && g_entryGuard.DisableEnter(nowMs))
            {
                uint64_t args[] = { 0, (uint64_t)kControlEnter, 1 };     // DISABLE_CONTROL_ACTION(0, 23, true)
                g_world->CallNative(game::native::DISABLE_CONTROL_ACTION, args, 3, nullptr);
            }
            CancelEntryWalk(lp);                       // in a vehicle (ours, or GTA's) or dead: no walk to finish
            return;
        }
        float lpPos[3];
        if (!g_world->GetEntityCoords(lp, lpPos[0], lpPos[1], lpPos[2]))
        {
            CancelEntryWalk(lp);
            return;
        }
        static std::vector<EntryCar> cars;             // script tick only: keeps its capacity (no allocation per on-foot frame)
        cars.clear();
        GatherEntryCars(lpPos, cars);
        if (cars.empty())
        {
            CancelEntryWalk(lp);                       // nothing networked in reach: GTA's entry keeps every other car
            return;
        }
        // Doors once per frame (bone indexes are cached per model). The keys are taken only while a networked
        // car's door is within walking range, so F beside an ambient car in the 5..8 m band stays GTA's.
        float nearestDoorM = 1e9f;
        for (EntryCar& car : cars)
        {
            FillEntryDoors(car);
            for (int s = 0; s < car.c.maxSeats && s < sync::kEntrySeats; ++s)
            {
                const float dx = car.c.door[s].x - lpPos[0], dy = car.c.door[s].y - lpPos[1],
                            dz = car.c.door[s].z - lpPos[2];
                nearestDoorM = std::min(nearestDoorM, std::sqrt(dx * dx + dy * dy + dz * dz));
            }
        }
        const bool takeKeys = g_entryWalk.active || nearestDoorM <= kEntryWalkMaxM;
        // The keys. Disabled first: IS_DISABLED_CONTROL_JUST_PRESSED reads the control in its disabled state
        // (the pause menu's pattern). Presses while the chat or a panel has the keyboard are not ours.
        if (takeKeys)
            for (const int control : { kControlEnter, kControlDetonate, kControlThrowGrenade })   // 0.4.22: G is also the grenade throw on foot
            {
                uint64_t args[] = { 0, (uint64_t)control, 1 };     // DISABLE_CONTROL_ACTION(0, control, true)
                g_world->CallNative(game::native::DISABLE_CONTROL_ACTION, args, 3, nullptr);
            }
        const bool typing = game::Overlay::WantsInput();
        const bool pressF = takeKeys && !typing && g_world->IsDisabledControlJustPressed(0, kControlEnter);
        const bool pressG = takeKeys && !typing && !pressF && g_world->IsDisabledControlJustPressed(0, kControlDetonate);
        // Safety net: GTA started entering a networked car anyway (a path the disabled keys do not cover).
        // Take the ped out of that task at once and treat it as F for that car.
        bool net = false;
        if (!pressF && !pressG && !g_localEnter.Active())      // T1: the enter task running now is OURS, not GTA's own
        {
            const int32_t trying = g_world->GetVehiclePedIsTryingToEnter(lp);
            for (size_t i = 0; trying && i < cars.size(); ++i)
                if (cars[i].handle == trying)
                {
                    const EntryCar target = cars[i];
                    cars.assign(1, target);
                    net = true;
                    ClearPedTasksNow(lp);
                    g_entryWalk = EntryWalk();         // the old task is gone with the rest
                    GLOGW("engine: vehicle: GTA began entering networked vehicle id=%d on its own: stopped, "
                          "treated as F", target.c.netId);
                    break;
                }
        }
        const bool begin = pressF || pressG || net;
        if (!begin && !g_entryWalk.active)
            return;
        if (begin && (g_world->IsPedRagdoll(lp) || g_world->IsPedFalling(lp) || g_world->IsPedJumping(lp) ||
                      g_world->IsPedClimbing(lp)))
            return;                                    // not now; the press is spent
        bool wantDriver;
        if (begin)
        {
            if (g_entryWalk.active)
                CancelEntryWalk(lp);                   // a new press replaces the walk (F after G, or the other way)
            wantDriver = !pressG;
            g_entryWalk = EntryWalk();
            g_entryWalk.active = true;
            g_entryWalk.wantDriver = wantDriver;
            g_entryWalk.startMs = nowMs;
            g_entryGuard.OnPress(nowMs);               // W9 item 3: the press is ours, control 23 stays disabled until seated + 500 ms
            ++g_v4.entryPresses;
            ++(pressG ? g_ds.pressG : g_ds.pressF);    // 0.4.22
        }
        else
        {
            wantDriver = g_entryWalk.wantDriver;
            // Committed to the car chosen at the press: keep only it (the seat is re-picked every frame).
            std::vector<EntryCar> only;
            for (const EntryCar& car : cars)
                if (car.c.netId == g_entryWalk.netId)
                    only.push_back(car);
            cars.swap(only);
            const bool inFlow = g_localEnter.Active();         // T1: the flow has its own limits (the answer wait, the task limit)
            if (cars.empty() || (!inFlow && nowMs - g_entryWalk.startMs > kEntryWalkMaxMs) || g_world->IsPedRagdoll(lp))
            {
                CancelEntryWalk(lp);
                return;
            }
            if (inFlow)
            {
                sync::LocalEnterIn in;
                in.nowMs = nowMs;
                // either native counts (see TickPeerSeatReplay): the fallback warp must not fire on a task that runs but is not named by one of them
                in.tryingToEnter = g_world->GetVehiclePedIsTryingToEnter(lp) == cars[0].handle || g_world->GetVehiclePedIsUsing(lp) == cars[0].handle;
                const int cls = g_world->GetVehicleClass(cars[0].handle);
                in.mount = cls == 8 || cls == 13;
                in.abort = g_world->IsPedFalling(lp) || g_world->IsPedJumping(lp);
                ApplyLocalEnterOut(g_localEnter.Tick(in), lp, nowMs);
                return;
            }
        }
        std::vector<sync::EntryCandidate> cands;
        cands.reserve(cars.size());
        for (const EntryCar& car : cars)
            cands.push_back(car.c);                    // doors filled above
        const sync::EntryPick pick = sync::PickEntry({ lpPos[0], lpPos[1], lpPos[2] }, cands.data(), (int)cands.size(),
                                                     join::MyId(), wantDriver);
        if (pick.reason != sync::EntryReason::Ok)
        {
            if (begin)
            {
                EntryNotice(sync::EntryRefusalText(pick.reason), nowMs);
                GLOGI("engine: vehicle: %s refused: %s", wantDriver ? "F" : "G", sync::EntryRefusalText(pick.reason));
                ++(pick.reason == sync::EntryReason::Locked ? g_ds.refusedLocked : pick.reason == sync::EntryReason::Owned ? g_ds.refusedOwned : g_ds.refusedFull);
            }
            CancelEntryWalk(lp);                       // mid-walk: the seat went away (taken, locked): stop quietly
            return;
        }
        const EntryCar* car = nullptr;
        for (const EntryCar& c : cars)
            if (c.c.netId == pick.netId)
                car = &c;
        if (!car)
        {
            CancelEntryWalk(lp);
            return;
        }
        g_entryWalk.netId = pick.netId;
        g_entryWalk.seat = pick.seat;
        if (pick.doorDistM > kEntryWalkMaxM)
        {
            if (begin)
            {
                EntryNotice(sync::LocalVehicleLink::RefusalText(3 /* TooFar */), nowMs);
                ++g_ds.refusedFar;
            }
            CancelEntryWalk(lp);
            return;
        }
        // 0.4.22: arm the read-back of this pick (TickDoorsSeats compares it with the seat GTA really puts the ped in), and for the press that
        // began the entry count the seat class and read IS_VEHICLE_SEAT_FREE (entering counts) next to the server's list, which called it free.
        g_dsState.expect.active = true;
        g_dsState.expect.netId = pick.netId;
        g_dsState.expect.seat = pick.seat;
        g_dsState.expect.atMs = nowMs;
        if (begin)
        {
            ++g_ds.pickSeat[(int)sync::ClassifySeat(pick.seat)];
            const int seatFree = g_world->VehicleSeatFreeState(car->handle, pick.seat - 1, true);
            ++(seatFree < 0 ? g_ds.freeUnknown : seatFree ? g_ds.freeYes : g_ds.freeNo);
            GLOGI("engine: doors: %s picked wire seat %d (GTA seat %d, %s) of vehicle id=%d, door %.2f m away; the model offers %d seats; "
                  "IS_VEHICLE_SEAT_FREE (a ped entering counts) says %s while the server's list says free", wantDriver ? "F" : "G", pick.seat,
                  pick.seat - 1, sync::ClassifySeat(pick.seat) == sync::SeatClass::Driver ? "driver"
                  : sync::ClassifySeat(pick.seat) == sync::SeatClass::FrontPassenger ? "front passenger"
                  : sync::ClassifySeat(pick.seat) == sync::SeatClass::Rear ? "REAR" : "further back", pick.netId, (double)pick.doorDistM,
                  car->c.maxSeats, seatFree < 0 ? "unknown (not callable)" : seatFree ? "free" : "TAKEN");
        }
        if (begin && g_seatIntentOn.load() && !car->c.locked)
        {
            // T1 seats (wire v6): the real entry. The seat is RESERVED on the server first (EnterIntent); only its Ok starts GTA's own
            // TASK_ENTER_VEHICLE for exactly that seat (no jack flag), on every screen. A locked car (its owner's entry) and HKCU seat_intent=0 keep
            // the walk + teleport-in below.
            const int32_t holder = g_world->GetPedInVehicleSeat(car->handle, pick.seat - 1);
            if (holder && holder != lp)
            {
                CancelEntryWalk(lp);                   // the server's list can run ahead of a remote occupant's ped: not now
                return;
            }
            g_entryWalk.tasked = false;
            g_localEnter.Begin(pick.netId, pick.seat, pick.doorDistM > kEntryRunM, nowMs);
            ++g_v4.entryPicks;
            GLOGI("engine: seats: %s -> seat %d of vehicle id=%d (door %.2f m): asking the server", wantDriver ? "F" : "G", pick.seat, pick.netId,
                  pick.doorDistM);
            sync::LocalEnterIn in;
            in.nowMs = nowMs;
            ApplyLocalEnterOut(g_localEnter.Tick(in), lp, nowMs);   // SendIntent
            return;
        }
        if (pick.doorDistM <= kEntryWarpM)
        {
            // The local seat must be empty too: the server's list can run ahead of a remote occupant's ped.
            // (GET_PED_IN_VEHICLE_SEAT reads 0 for an empty seat or when it cannot be called.)
            const int32_t holder = g_world->GetPedInVehicleSeat(car->handle, pick.seat - 1);
            if (holder && holder != lp)
                return;                                // retried next frame until the walk times out
            if (g_entryWalk.tasked)
                g_world->ClearPedTasks(lp);
            const bool ok = g_world->SetPedIntoVehicle(lp, car->handle, pick.seat - 1);
            g_localSeatAtMs = 0;                       // read the real seat next tick
            ++g_v4.entryPicks;
            g_entryGuard.OnSeated(nowMs);
            GLOGI("engine: vehicle: %s -> seat %d of vehicle id=%d (door %.2f m): %s", wantDriver ? "F" : "G", pick.seat,
                  pick.netId, pick.doorDistM, ok ? "SET_PED_INTO_VEHICLE called" : "SET_PED_INTO_VEHICLE FAILED");
            g_entryWalk = EntryWalk();
            return;
        }
        // Walk to the door. The task is re-issued when the door moved or when its timeout is near.
        const float mx = pick.door.x - g_entryWalk.target.x, my = pick.door.y - g_entryWalk.target.y,
                    mz = pick.door.z - g_entryWalk.target.z;
        if (!g_entryWalk.tasked || mx * mx + my * my + mz * mz > 0.25f || nowMs - g_entryWalk.taskMs >= kEntryReissueMs)
        {
            g_world->TaskGoStraightToCoord(lp, pick.door.x, pick.door.y, pick.door.z, 2.0f, 3000, 0.0f, 0.0f);
            if (!g_entryWalk.tasked)
                GLOGI("engine: vehicle: %s -> walking to seat %d of vehicle id=%d (door %.2f m away)",
                      wantDriver ? "F" : "G", pick.seat, pick.netId, pick.doorDistM);
            g_entryWalk.tasked = true;
            g_entryWalk.taskMs = nowMs;
            g_entryWalk.target = pick.door;
        }
    }

    // ---- 0.4.22 noclip: the keys, the natives and the log lines around sync::noclip::Gate (libs/sync/noclip.h) ----------------------------
    // The keys are read raw (GetAsyncKeyState), not through the game's controls: noclip's keys do not depend on what the player
    // rebound, on the on-foot / vehicle context or on a control held disabled. Only while the game's window is the foreground
    // window and neither the chat nor a Tab menu panel has the keyboard (Overlay::WantsInput).
    bool NoclipOurWindowForeground()
    {
        const HWND fg = GetForegroundWindow();
        if (!fg)
            return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        return pid == GetCurrentProcessId();
    }

    bool NoclipKeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

    sync::noclip::Keys ReadNoclipKeys()
    {
        sync::noclip::Keys k;
        if (game::Overlay::WantsInput() || !NoclipOurWindowForeground())
            return k;
        k.fwd = NoclipKeyDown('W');
        k.back = NoclipKeyDown('S');
        k.left = NoclipKeyDown('A');
        k.right = NoclipKeyDown('D');
        k.up = NoclipKeyDown(VK_SPACE);
        k.down = NoclipKeyDown(VK_CONTROL);
        k.fast = NoclipKeyDown(VK_SHIFT);
        k.slow = NoclipKeyDown(VK_MENU);
        return k;
    }

    sync::noclip::GroundRead NoclipGround(float x, float y, float z)
    {
        sync::noclip::GroundRead g;
        float gz = 0;
        if (g_world->GetGroundZ(x, y, z, gz))
        {
            g.found = true;
            g.z = gz;
        }
        return g;
    }

    const char* NoclipSubjectName(sync::noclip::Subject s)
    {
        switch (s)
        {
        case sync::noclip::Subject::Ped: return "ped";
        case sync::noclip::Subject::Vehicle: return "vehicle";
        case sync::noclip::Subject::None: break;
        }
        return "none";
    }

    const char* NoclipEndName(sync::noclip::EndWhy w)
    {
        switch (w)
        {
        case sync::noclip::EndWhy::Toggled: return "toggled off";
        case sync::noclip::EndWhy::SwitchedOff: return "noclip=0";
        case sync::noclip::EndWhy::NotReady: return "player not alive / not entered";
        case sync::noclip::EndWhy::SubjectGone: return "the ped / vehicle changed or vanished";
        case sync::noclip::EndWhy::None: break;
        }
        return "?";
    }

    const char* NoclipPhaseName(sync::noclip::Phase p)
    {
        switch (p)
        {
        case sync::noclip::Phase::On: return "on";
        case sync::noclip::Phase::Landing: return "landing";
        case sync::noclip::Phase::Off: break;
        }
        return "off";
    }

    float NoclipLen(float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); }

    // The 10 s line (quiet windows are skipped; the first one is always written, so the log shows the switch state).
    void NoclipReport(double nowMs, bool enabled)
    {
        if (nowMs - g_noclipReportMs < 10000.0)
            return;
        g_noclipReportMs = nowMs;
        const NoclipStats& s = g_noclipStats;
        const uint32_t activity = s.onTicks + s.holdTicks + s.starts + s.endToggle + s.endSwitch + s.endNotReady + s.endGone +
                                  s.refusedDisabled + s.refusedNotReady + s.refusedPassenger;
        if (activity || g_noclipFirstReport)
        {
            g_noclipFirstReport = false;
            GLOGI("engine: noclip (0.4.22): last 10 s: noclip=%s, now %s; started %u, ended %u (toggle %u, noclip=0 %u, player not alive %u, "
                  "ped / vehicle gone %u), refused %u (noclip=0 %u, not ready %u, passenger %u); flying %u ticks (keys held %u, shift %u, alt %u, "
                  "resynced %u), %d controls held disabled on %u ticks, landing-hold ticks %u, landed: placed %u, fell back to the last safe spot %u, "
                  "left where it was %u; issued vs read back (%u samples every 0.5 s): collision off %u, collision back on %u (re-asserted), "
                  "invincible %u, invincible lost %u (re-asserted); ground probes %u (valid %u); commanded speed max %.1f m/s, game speed max %.1f m/s, "
                  "position drift max %.2f m (game vs what was written), height %.1f m, camera pitch %.1f yaw %.1f; uncallable %u",
                  enabled ? "on" : "OFF", NoclipPhaseName(g_noclip.GetPhase()), s.starts,
                  s.endToggle + s.endSwitch + s.endNotReady + s.endGone, s.endToggle, s.endSwitch, s.endNotReady, s.endGone,
                  s.refusedDisabled + s.refusedNotReady + s.refusedPassenger, s.refusedDisabled, s.refusedNotReady, s.refusedPassenger,
                  s.onTicks, s.keysHeld, s.fastTicks, s.slowTicks, s.resyncs, sync::noclip::kDisabledControlCount, s.controlHolds, s.holdTicks,
                  s.placed, s.fellBack, s.skipped, s.samples, s.collisionOff, s.collisionBackOn, s.invincibleOn, s.invincibleLost,
                  s.groundProbes, s.groundValid, s.cmdSpeedMax, s.gameSpeedMax, s.driftMax, s.heightLast, s.camPitchLast, s.camYawLast,
                  s.uncallable);
        }
        g_noclipStats = NoclipStats();
    }

    // Every script tick, on the entered player (the call sits after RunCommands, before TickVehicleEntry). lp = the local ped,
    // lpVeh = the vehicle it is in (0 on foot).
    void TickNoclip(int32_t lp, int32_t lpVeh, double nowMs)
    {
        namespace nc = sync::noclip;
        NoclipStats& st = g_noclipStats;
        const bool toggle = g_noclipToggle;
        g_noclipToggle = false;
        const bool enabled = g_noclipSwitch.load();
        if (g_noclip.GetPhase() == nc::Phase::Off && !toggle)
        {
            game::Overlay::SetNoclip(enabled, false);
            NoclipReport(nowMs, enabled);
            return;
        }

        const bool wasOn = g_noclip.GetPhase() == nc::Phase::On;
        nc::In in;
        in.nowMs = nowMs;
        in.enabled = enabled;
        in.toggle = toggle;
        in.ready = lp && g_life == Life::Alive && g_entry.load() == Entry::Done && !g_world->IsPedDeadOrDying(lp);
        if (in.ready)
        {
            if (lpVeh && g_world->GetPedInVehicleSeat(lpVeh, -1) == lp)
            {
                in.entity = lpVeh;
                in.subject = nc::Subject::Vehicle;
            }
            else
            {
                in.entity = lp;
                in.subject = nc::Subject::Ped;
                in.passenger = lpVeh != 0;
            }
            if (!g_world->GetEntityCoords(in.entity, in.pos.x, in.pos.y, in.pos.z))
            {
                in.entity = 0;                           // cannot be read: gone (Off: not ready; flying: the subject changed)
                in.subject = nc::Subject::None;
            }
        }
        const bool flying = g_noclip.Active();
        if (!flying && toggle && enabled && in.ready && in.entity && !in.passenger)
        {
            // The flags noclip changes, read before anything is set (they are given back exactly as read).
            bool disabled = false;
            if (g_world->GetEntityCollisionDisabled(in.entity, disabled))
                in.read.collision = !disabled;
            else
                ++st.uncallable;
            bool inv = false;
            if (g_world->GetPlayerInvincible(inv))
                in.read.invincible = inv;
            else
                ++st.uncallable;
        }
        static float s_pitch = 0, s_yaw = 0;
        if (flying)
        {
            in.keys = ReadNoclipKeys();
            float pitch = 0, roll = 0, yaw = 0;
            if (g_world->GetGameplayCamRot(pitch, roll, yaw))
            {
                s_pitch = pitch;
                s_yaw = yaw;
            }
            in.pitchDeg = s_pitch;
            in.yawDeg = s_yaw;
            st.camPitchLast = s_pitch;
            st.camYawLast = s_yaw;
            if (in.entity)
            {
                g_world->RequestCollisionAt(in.pos.x, in.pos.y, in.pos.z);   // the world around has to have collision when we land
                const bool ending = wasOn && (toggle || !enabled);
                if (ending || g_noclip.GetPhase() == nc::Phase::Landing)
                {
                    in.probed = true;
                    in.below = NoclipGround(in.pos.x, in.pos.y, in.pos.z);
                    in.above = NoclipGround(in.pos.x, in.pos.y, in.pos.z + nc::kHighProbeM);
                    ++st.groundProbes;
                    if (nc::GroundValid(in.below) || nc::GroundValid(in.above))
                        ++st.groundValid;
                }
                else if (nowMs - g_noclipProbeMs >= nc::kProbeEveryMs)
                {
                    g_noclipProbeMs = nowMs;
                    in.probed = true;
                    in.below = NoclipGround(in.pos.x, in.pos.y, in.pos.z);
                    ++st.groundProbes;
                    if (nc::GroundValid(in.below))
                        ++st.groundValid;
                }
            }
        }

        const nc::Out out = g_noclip.Tick(in);

        if (out.refused != nc::Refusal::None)
        {
            const char* say = "";
            switch (out.refused)
            {
            case nc::Refusal::Disabled:
                ++st.refusedDisabled;
                say = "Noclip is turned off on this PC (noclip=0 in the launcher settings).";
                break;
            case nc::Refusal::NotReady:
                ++st.refusedNotReady;
                say = "Noclip needs a living character in the world.";
                break;
            case nc::Refusal::Passenger:
                ++st.refusedPassenger;
                say = "Noclip: you are a passenger. Take the driver's seat, or leave the vehicle first.";
                break;
            case nc::Refusal::None:
                break;
            }
            game::Overlay::PushSystemLine(say);
            GLOGI("engine: noclip (0.4.22): refused: %s", say);
        }

        // The game's own controls stay out of the way while noclip owns the keys (a per-frame effect, hence every tick).
        if (g_noclip.Active() || out.end)
        {
            for (const int control : nc::kDisabledControls)
            {
                uint64_t args[] = { 0, (uint64_t)control, 1 };  // DISABLE_CONTROL_ACTION(0, control, true)
                g_world->CallNative(game::native::DISABLE_CONTROL_ACTION, args, 3, nullptr);
            }
            ++st.controlHolds;
        }

        if (out.begin)
        {
            CancelEntryWalk(lp);                         // a F / G walk to a car must not run under a flying ped
            g_noclipHaveWritten = false;
            g_noclipProbeMs = nowMs;
            const bool a = g_world->SetEntityCollision(out.entity, false);
            const bool b = g_world->SetEntityHasGravity(out.entity, false);
            const bool c = g_world->SetPlayerInvincible(true);
            const bool d = out.subject != nc::Subject::Vehicle || g_world->SetEntityInvincible(out.entity, true);
            if (!a || !b || !c || !d)
                ++st.uncallable;
            ++st.starts;
            bool disabledNow = false;
            const bool rb = g_world->GetEntityCollisionDisabled(out.entity, disabledNow);
            GLOGI("engine: noclip (0.4.22): ON, %s %d at %.1f %.1f %.1f; flags read first: collision %s, gravity %s (no getter), invincible %s; "
                  "set: collision off %s, gravity off %s, invincible on %s%s; collision-disabled read-back %s",
                  NoclipSubjectName(out.subject), out.entity, in.pos.x, in.pos.y, in.pos.z, g_noclip.Saved().collision ? "on" : "off",
                  g_noclip.Saved().gravity ? "on" : "off", g_noclip.Saved().invincible ? "on" : "off", a ? "ok" : "FAILED", b ? "ok" : "FAILED",
                  c ? "ok" : "FAILED", out.subject == nc::Subject::Vehicle ? (d ? ", vehicle invincible ok" : ", vehicle invincible FAILED") : "",
                  rb ? (disabledNow ? "yes" : "NO (the game ignored it)") : "uncallable");
            game::Overlay::PushSystemLine("Noclip on: W A S D fly along the view, Space up, Ctrl down, Shift fast, Alt slow.");
        }

        if (out.move)
        {
            ++st.onTicks;
            ++st.moves;
            if (in.keys.Any())
            {
                ++st.keysHeld;
                if (in.keys.fast) ++st.fastTicks;
                if (in.keys.slow) ++st.slowTicks;
            }
            if (out.resynced)
                ++st.resyncs;
            if (g_noclipHaveWritten)
            {
                const float drift = NoclipLen(in.pos.x - g_noclipLastWritten.x, in.pos.y - g_noclipLastWritten.y, in.pos.z - g_noclipLastWritten.z);
                if (drift > st.driftMax)
                    st.driftMax = drift;
            }
            g_world->SetEntityCoordsNoOffset(out.entity, out.pos.x, out.pos.y, out.pos.z);
            g_world->SetEntityVelocity(out.entity, out.vel.x, out.vel.y, out.vel.z);
            g_world->SetEntityHeading(out.entity, out.heading);
            g_noclipLastWritten = out.pos;
            g_noclipHaveWritten = true;
            const float cmd = NoclipLen(out.vel.x, out.vel.y, out.vel.z);
            if (cmd > st.cmdSpeedMax)
                st.cmdSpeedMax = cmd;
            st.heightLast = out.pos.z;
            if (in.probed)
            {
                // Issued vs read back, every 0.5 s: collision still off, invincibility still on, the speed the game holds.
                bool disabled = false;
                if (g_world->GetEntityCollisionDisabled(out.entity, disabled))
                {
                    ++st.samples;
                    if (disabled)
                        ++st.collisionOff;
                    else
                    {
                        ++st.collisionBackOn;
                        g_world->SetEntityCollision(out.entity, false);
                    }
                }
                bool inv = false;
                if (g_world->GetPlayerInvincible(inv))
                {
                    if (inv)
                        ++st.invincibleOn;
                    else
                    {
                        ++st.invincibleLost;
                        g_world->SetPlayerInvincible(true);
                    }
                }
                float vx = 0, vy = 0, vz = 0;
                if (g_world->GetEntityVelocity(out.entity, vx, vy, vz))
                {
                    const float gs = NoclipLen(vx, vy, vz);
                    if (gs > st.gameSpeedMax)
                        st.gameSpeedMax = gs;
                }
            }
        }

        if (out.hold)
        {
            ++st.holdTicks;
            g_world->SetEntityCoordsNoOffset(out.entity, out.pos.x, out.pos.y, out.pos.z);   // still, until the ground under it exists
            g_world->SetEntityVelocity(out.entity, 0.0f, 0.0f, 0.0f);
        }

        if (out.end)
        {
            const bool askedFor = out.why == nc::EndWhy::Toggled || out.why == nc::EndWhy::SwitchedOff;
            bool placedOk = true;
            if (out.land.kind == nc::LandKind::Place || out.land.kind == nc::LandKind::Fallback)
            {
                placedOk = g_world->SetEntityCoordsNoOffset(out.entity, out.land.x, out.land.y, out.land.z);
                g_world->SetEntityVelocity(out.entity, 0.0f, 0.0f, 0.0f);
                if (out.land.kind == nc::LandKind::Place) ++st.placed; else ++st.fellBack;
            }
            else
                ++st.skipped;
            // The flags go back last, exactly as read when it began (the subject stands on the ground by now).
            const bool a = g_world->SetEntityCollision(out.entity, out.restore.collision);
            const bool b = g_world->SetEntityHasGravity(out.entity, out.restore.gravity);
            const bool c = g_world->SetPlayerInvincible(out.restore.invincible);
            const bool d = out.subject != nc::Subject::Vehicle || g_world->SetEntityInvincible(out.entity, false);
            if (askedFor && (!a || !b || !c || !d || !placedOk))
                ++st.uncallable;
            bool disabledNow = false;
            const bool rb = g_world->GetEntityCollisionDisabled(out.entity, disabledNow);
            switch (out.why)
            {
            case nc::EndWhy::Toggled: ++st.endToggle; break;
            case nc::EndWhy::SwitchedOff: ++st.endSwitch; break;
            case nc::EndWhy::NotReady: ++st.endNotReady; break;
            case nc::EndWhy::SubjectGone: ++st.endGone; break;
            case nc::EndWhy::None: break;
            }
            const char* landed = out.land.kind == nc::LandKind::Place ? "placed on the ground"
                               : out.land.kind == nc::LandKind::Fallback ? "no ground read within 1.5 s: back to the last safe spot"
                               : "not moved";
            GLOGI("engine: noclip (0.4.22): OFF (%s), %s %d: %s (%.1f %.1f %.1f); flags given back: collision %s, gravity %s, invincible %s "
                  "(calls %s); collision-disabled read-back %s",
                  NoclipEndName(out.why), NoclipSubjectName(out.subject), out.entity, landed,
                  out.land.kind == nc::LandKind::Skip ? in.pos.x : out.land.x, out.land.kind == nc::LandKind::Skip ? in.pos.y : out.land.y,
                  out.land.kind == nc::LandKind::Skip ? in.pos.z : out.land.z, out.restore.collision ? "on" : "off",
                  out.restore.gravity ? "on" : "off", out.restore.invincible ? "on" : "off", (a && b && c && d) ? "ok" : "some failed (entity gone?)",
                  rb ? (disabledNow ? "STILL DISABLED" : "collision on again") : "uncallable");
            if (askedFor)
                game::Overlay::PushSystemLine("Noclip off.");
            g_noclipHaveWritten = false;
        }

        game::Overlay::SetNoclip(enabled, g_noclip.GetPhase() == nc::Phase::On);
        NoclipReport(nowMs, enabled);
    }

    // ---- 0.4.22 doors and seats: the L key, the server's answers, the read-backs (rules: libs/sync/vehicle_lock.h) ----------------
    // The lock is the server's: L only ASKS (VehicleOp::SetLocked, arg 1 locks / 0 unlocks, toggling what the replicated flag says), the answer
    // is shown (a chat line and a frontend sound) and the flag comes back in the VehicleWorld, which ApplyVehicleFields turns into
    // SET_VEHICLE_DOORS_LOCKED(2 / 1) on every copy, late joiners included (they get the flag in their first full entry). TickLockKeeper then
    // reads GET_VEHICLE_DOOR_LOCK_STATUS back about once a second and puts a lock the game dropped back.
    constexpr float kLockReachM = 8.0f;          // L sees a networked car this close (to its origin); the server's own limit for a player outside it is vehicleLockDistance
    constexpr double kSeatGraceMs = 2000.0;      // a pick with no entry under way is judged "never sat in it" this long after it was last picked
    constexpr int kLockCandMax = 16;

    void LogDoorsSeatsSwitches()
    {
        GLOGI("engine: doors and seats (0.4.22): veh_rear_seats=%s (F / G %s), veh_locks=%s (the L key and the lock read-back on the copies %s); "
              "controls (INFERRED ids, confirm live): F = %d, G = %d (+ %d, the grenade throw, disabled with it), L = %d (disabled while a networked car is in reach)",
              g_vehRearSeats.load() ? "on" : "off", g_vehRearSeats.load() ? "may pick the rear seats of a car that has them" : "see only the driver's seat and the front passenger's",
              g_vehLocks.load() ? "on" : "off", g_vehLocks.load() ? "work" : "are off", kControlEnter, kControlDetonate, kControlThrowGrenade, kControlLockKey);
    }

    // The answer to our SetLocked (DrainEvents routes it here, not to the link).
    void HandleLockResult(const proto::VehicleResult& r, double nowMs)
    {
        DoorsSeatsState::Pending& pd = g_dsState.lockPending;
        const bool matched = pd.active && pd.netId == (int)r.vehId;
        bool want = false;
        if (matched)
            want = pd.wantLocked;
        else
        {
            const auto it = g_vehs.find(r.vehId);
            want = it != g_vehs.end() && (it->second.st.flags & proto::kVehLocked) != 0;   // late or unmatched: what the flag says now
        }
        if (matched)
            pd.active = false;
        const uint8_t code = (uint8_t)r.code;
        const char* text = sync::LockResultText(code, want);
        game::Overlay::PushSystemLine(text);
        const sync::LockSound snd = sync::LockSoundFor(code, want);
        ++(g_world->PlaySoundFrontend(snd.name, snd.set) ? g_ds.soundOk : g_ds.soundFail);
        if (code == 0)
        {
            g_lockFlashUntilMs = nowMs + sync::kLockFlashMs;           // 0.4.27: the state row shows the new lock (on foot too)
            g_lockFlashLocked = want;
            ++g_ds.lockOk;
            ++(want ? g_ds.lockOkLocked : g_ds.lockOkUnlocked);
            g_dsState.keys.Note((int)r.vehId);                 // an Ok means the server counts us as the key's holder
            GLOGI("engine: doors: the server answered SetLocked id=%u: Ok, the vehicle is now %s (%.0f ms after the press%s)", (unsigned)r.vehId,
                  want ? "LOCKED" : "UNLOCKED", matched ? nowMs - pd.sentMs : -1.0, matched ? "" : "; no press of ours was waiting for it");
        }
        else
        {
            ++g_ds.lockRefused;
            ++g_ds.lockRefusedBy[code & 15];
            if (code == 5 /* NotAllowed */ || code == 1 /* NoVehicle */)
                g_dsState.keys.Forget((int)r.vehId);
            GLOGW("engine: doors: the server REFUSED SetLocked id=%u: %s (%s)", (unsigned)r.vehId, VehResultName(code), text);
        }
    }

    // The seat read-back of an F / G pick (armed by TickVehicleEntry) and the keys of the cars we drive.
    void TickSeatReadback(int32_t lp, int32_t lpVeh, int inNetId, double nowMs)
    {
        DoorsSeatsState::Expect& ex = g_dsState.expect;
        if (!ex.active)
            return;
        if (!lp)
        {
            ex.active = false;
            return;
        }
        const bool busy = g_localEnter.Active() || g_entryWalk.active;
        if (lpVeh && inNetId >= 0)
        {
            int actual = -1;                                   // the wire seat the game says our ped sits in
            for (int s = -1; s < (int)proto::kVehSeats - 1; ++s)
                if (g_world->GetPedInVehicleSeat(lpVeh, s) == lp)
                {
                    actual = s + 1;
                    break;
                }
            if (actual < 0 && nowMs - ex.atMs < kSeatGraceMs)
                return;                                        // GET_VEHICLE_PED_IS_IN names the car a moment before the seat reads: try again
            const bool theCar = inNetId == ex.netId;
            const sync::SeatReadback rb = theCar ? sync::JudgeSeatReadback(ex.seat, actual) : sync::SeatReadback::NotSeated;
            ++(rb == sync::SeatReadback::Match ? g_ds.seatMatch : rb == sync::SeatReadback::Differs ? g_ds.seatDiffers : g_ds.seatNotSeated);
            if (rb == sync::SeatReadback::Match)
                GLOGI("engine: doors: seat read-back OK: picked wire seat %d of vehicle id=%d, GET_PED_IN_VEHICLE_SEAT puts us in wire seat %d (GTA seat %d)", ex.seat,
                      ex.netId, actual, actual - 1);
            else if (rb == sync::SeatReadback::Differs)
                GLOGW("engine: doors: seat read-back DIFFERS: picked wire seat %d of vehicle id=%d, GET_PED_IN_VEHICLE_SEAT puts us in wire seat %d (GTA seat %d)",
                      ex.seat, ex.netId, actual, actual - 1);
            else
                GLOGW("engine: doors: seat read-back NOT SEATED: picked wire seat %d of vehicle id=%d, but %s", ex.seat, ex.netId,
                      !theCar ? "we are in another networked vehicle" : "the game has no seat of that car holding us");
            ex.active = false;
            return;
        }
        if (!busy && nowMs - ex.atMs >= kSeatGraceMs)
        {
            ++g_ds.seatNotSeated;
            GLOGI("engine: doors: seat read-back: picked wire seat %d of vehicle id=%d but we never sat in it (refused, cancelled or out of reach)", ex.seat, ex.netId);
            ex.active = false;
        }
    }

    // L: lock / unlock the vehicle we are in, or the networked one in reach we hold the key of (else the nearest: the server answers NotAllowed to a
    // stranger). Script tick, on foot or seated; the control is disabled only while a networked car is in reach.
    void TickLockKey(int32_t lp, int32_t lpVeh, bool dead, double nowMs)
    {
        if (!lp || dead)
            return;
        float lpPos[3];
        if (!g_world->GetEntityCoords(lp, lpPos[0], lpPos[1], lpPos[2]))
            return;
        const uint16_t me = join::MyId();
        sync::LockCand cands[kLockCandMax];
        int n = 0;
        for (const auto& kv : g_vehs)
        {
            const NetVeh& v = kv.second;
            if (!v.handle || v.removed || !v.present || !v.haveState)
                continue;
            const bool inside = lpVeh && v.handle == lpVeh;
            if (!inside && (std::fabs(v.st.k.x - lpPos[0]) > 40.0f || std::fabs(v.st.k.y - lpPos[1]) > 40.0f))
                continue;                                      // cheap reject by the server position before the native
            float x = 0, y = 0, z = 0;
            if (!g_world->GetEntityCoords(v.handle, x, y, z))
                continue;
            const float dx = x - lpPos[0], dy = y - lpPos[1], dz = z - lpPos[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (!inside && d > kLockReachM)
                continue;
            if (n >= kLockCandMax)
                break;
            sync::LockCand& c = cands[n++];
            c.netId = (int)kv.first;
            c.distM = inside ? 0.0f : d;
            c.locked = (v.st.flags & proto::kVehLocked) != 0;
            c.keyed = g_dsState.keys.Has((int)kv.first) || (me != proto::kNoPlayer && v.st.ownerId == me);
            c.inside = inside;
        }
        if (n == 0)
            return;                                            // no networked car in reach: the L key stays the game's
        uint64_t args[] = { 0, (uint64_t)kControlLockKey, 1 };     // DISABLE_CONTROL_ACTION(0, 182, true), so IS_DISABLED_CONTROL_JUST_PRESSED reads it
        g_world->CallNative(game::native::DISABLE_CONTROL_ACTION, args, 3, nullptr);
        if (game::Overlay::WantsInput() || !g_world->IsDisabledControlJustPressed(0, kControlLockKey))
            return;
        ++g_ds.lockPress;
        DoorsSeatsState::Pending& pd = g_dsState.lockPending;
        if (pd.active && nowMs - pd.sentMs < kLockPendingMs)
        {
            ++g_ds.lockHeld;                                   // the last press has not been answered yet
            return;
        }
        const sync::LockPick pick = sync::PickLockTarget(cands, n);
        if (pick.why != sync::LockWhy::Ok)
            return;
        proto::VehicleCmd c;
        c.op = proto::VehicleOp::SetLocked;
        c.vehId = (uint16_t)pick.netId;
        c.arg = pick.wantLocked ? 1u : 0u;
        const bool sent = join::SendVehicleCmd(c);
        ++g_v4.vehCmds;
        ++(sent ? g_ds.lockSent : g_ds.lockNotSent);
        if (sent)
            pd = DoorsSeatsState::Pending{ true, pick.netId, pick.wantLocked, nowMs };
        GLOGI("engine: doors: L -> asking the server to %s vehicle id=%d (%s%s) %s", pick.wantLocked ? "LOCK" : "UNLOCK", pick.netId,
              pick.inside ? "we sit in it" : "in reach", pick.keyed ? ", we believe we hold its key" : "", sent ? "sent" : "NOT sent (no session)");
        if (!sent)
            game::Overlay::PushSystemLine("Not connected.");
    }

    // The copies' lock, read back about once a second and put back when the game dropped it. Script tick.
    void TickLockKeeper(double nowMs)
    {
        if (!g_vehLocks.load(std::memory_order_relaxed))
            return;
        for (auto& kv : g_vehs)
        {
            NetVeh& v = kv.second;
            if (!v.handle || v.removed || !v.present || !v.haveState || v.appliedLock < 0)
                continue;
            const int want = sync::LockStateFor((v.st.flags & proto::kVehLocked) != 0);
            if (v.appliedLock != want || !v.lockKeep.Due(nowMs))
                continue;                                      // the newest flag is not applied yet (ApplyVehicleFields does that), or not due
            const sync::LockKeepStats before = v.lockKeep.Stats();
            const int status = g_world->GetVehicleDoorLockStatus(v.handle);
            const sync::LockKeeper::Act act = v.lockKeep.Observe(want, status, nowMs);
            const sync::LockKeepStats& after = v.lockKeep.Stats();
            g_ds.keep.reads += after.reads - before.reads;
            g_ds.keep.readFails += after.readFails - before.readFails;
            g_ds.keep.match += after.match - before.match;
            g_ds.keep.mismatch += after.mismatch - before.mismatch;
            g_ds.keep.reissued += after.reissued - before.reissued;
            g_ds.keep.stuck += after.stuck - before.stuck;
            g_ds.keep.restored += after.restored - before.restored;
            if (after.stuck != before.stuck && g_dsState.reissueLogged < 40)
            {
                ++g_dsState.reissueLogged;
                GLOGW("engine: doors: vehicle id=%u: GET_VEHICLE_DOOR_LOCK_STATUS keeps reading %d, not the %d the server's flag asks for, after %d re-issues: "
                      "left alone for %.0f s (a script or the game owns its doors)", (unsigned)kv.first, status, want, sync::LockKeeper::kMaxReissues,
                      sync::LockKeeper::kBackoffMs / 1000.0);
            }
            if (act == sync::LockKeeper::Act::Reissue)
            {
                const bool ok = g_world->SetVehicleDoorsLocked(v.handle, want);
                if (g_dsState.reissueLogged < 40)
                {
                    ++g_dsState.reissueLogged;
                    GLOGW("engine: doors: vehicle id=%u: the game's lock status reads %d, the server's flag wants %d: SET_VEHICLE_DOORS_LOCKED re-issued (%s)",
                          (unsigned)kv.first, status, want, ok ? "called" : "NOT callable");
                }
            }
        }
    }

    // Runs every script tick on the connected, entered player, AFTER TickLocalVehicle.
    void TickDoorsSeats(int32_t lp, int32_t lpVeh, bool dead, double nowMs)
    {
        int inNetId = -1;                                      // the networked car we sit in
        if (lp && lpVeh)
            for (const auto& kv : g_vehs)
                if (kv.second.handle == lpVeh && !kv.second.removed)
                {
                    inNetId = (int)kv.first;
                    break;
                }
        if (inNetId >= 0 && g_localSeat == 0)
            g_dsState.keys.Note(inNetId);                      // the driver of the last ride holds its key (the server's lastDriver)
        TickSeatReadback(lp, lpVeh, inNetId, nowMs);
        if (g_vehLocks.load(std::memory_order_relaxed))
            TickLockKey(lp, lpVeh, dead, nowMs);
        TickLockKeeper(nowMs);
    }

    // Our own vehicle: which one, which seat, and the link's commands and answers.
    void TickLocalVehicle(int32_t lp, int32_t lpVeh, bool dead, double nowMs)
    {
        // Sync plan W2 A1: no auto-shuffle. GTA moves a passenger over to an empty driver's seat by itself
        // (PCF 184 PreventAutoShuffleToDriversSeat stops it); the link would read that as a seat change
        // the server may refuse. Set on entering and re-asserted every 2 s while in a vehicle. Flag id 184
        // is INFERRED (live check: sit 10 s as a passenger beside an empty driver's seat).
        if (lp && lpVeh && (lpVeh != g_noShuffleVeh || nowMs - g_noShuffleAtMs >= 2000.0))
        {
            const bool first = lpVeh != g_noShuffleVeh;
            const bool ok = g_world->SetPedConfigFlag(lp, 184, true);
            g_noShuffleVeh = lpVeh;
            g_noShuffleAtMs = nowMs;
            if (first)
                GLOGI("engine: vehicle: PCF 184 (PreventAutoShuffleToDriversSeat) set on vehicle %d: %s", lpVeh,
                      ok ? "called" : "NOT callable");
        }
        else if (!lpVeh)
            g_noShuffleVeh = 0;
        if (lpVeh != g_link.Vehicle() || lpVeh != g_lastMarkedVeh || nowMs - g_localSeatAtMs >= 500.0)
        {
            g_localSeat = -1;
            if (lpVeh)
            {
                if (lpVeh != g_lastMarkedVeh)
                {
                    // Before any seat read: on an ambient car GET_PED_IN_VEHICLE_SEAT may create its
                    // missing driver; owned by the player it does not (and the game keeps the car).
                    g_world->MarkVehicleOwned(lpVeh);
                    g_lastMarkedVeh = lpVeh;
                }
                for (int s = -1; s < (int)proto::kVehSeats - 1; ++s)
                    if (g_world->GetPedInVehicleSeat(lpVeh, s) == lp)
                    {
                        g_localSeat = s + 1;
                        break;
                    }
            }
            g_localSeatAtMs = nowMs;
        }
        int netId = -1;
        if (lpVeh)
        {
            for (const auto& kv : g_vehs)              // the game thread's own map: no lock
                if (kv.second.handle == lpVeh && !kv.second.removed)
                {
                    netId = kv.first;
                    break;
                }
        }
        if (lp && lpVeh)
        {
            if (g_exitTaskAtMs < 0.0 && g_world->GetIsTaskActive(lp, kTaskExitVehicle))
            {
                g_exitTaskAtMs = nowMs;
                GLOGI("engine: vehicle: our exit task (CTaskExitVehicle) started in vehicle %d (GET_VEHICLE_PED_IS_IN still names it)", lpVeh);
            }
        }
        else if (g_exitTaskAtMs >= 0.0 && nowMs - g_exitTaskAtMs > 5000.0)
            g_exitTaskAtMs = -1.0;                     // out of a car the link sent no Exit for (not networked): forget it
        sync::VehLinkIn in;
        in.nowMs = nowMs;
        in.veh = lpVeh;
        in.seat = lpVeh ? g_localSeat : -1;
        in.netId = netId;
        in.dead = dead;
        const sync::VehLinkOut o = g_link.Tick(in);

        if (o.bindVeh && o.bindNetId >= 0 && o.bindNetId < (int)proto::kMaxVehicles)
        {
            {
                // The join worker's side of this vehicle must exist too, as it did when both lived in one entry
                // (a VehicleWorld that does not list it then marks it absent, and the tick drops it).
                PeersLock lk(g_peersMtx, true);
                g_vehNet.emplace((uint16_t)o.bindNetId, VehNet());
            }
            NetVeh& v = g_vehs[(uint16_t)o.bindNetId];
            if (v.handle && v.handle != o.bindVeh)
            {
                UnseatPeersIn(o.bindNetId);
                g_world->DeleteEntity(v.handle);       // a copy spawned before the answer: ours is the one
                ++g_v4.vehDeleted;
            }
            ForgetVehicleHandle(v);
            v.handle = o.bindVeh;
            v.adopted = true;
            v.spawnedModel = g_world->GetEntityModel(o.bindVeh);
        }
        if (o.resultOp)
        {
            if (o.bindVeh)
                GLOGI("engine: vehicle: our car %d is now server vehicle id=%d (seat %d)", o.bindVeh, o.bindNetId,
                      g_link.Seat());
            else if (o.resultCode == 0)
                GLOGI("engine: vehicle: %s ok -> link %s (id=%d seat %d)", VehOpName(o.resultOp), g_link.StateName(),
                      g_link.NetId(), g_link.Seat());
        }
        if (o.revertSeat >= 0 && lp && lpVeh)
        {
            // Sync plan W2 A2: the server refused a SEAT CHANGE inside this car (it still has us in
            // revertSeat). Put the ped back, no leave animation (the other players would see a passenger
            // thrown out). SET_PED_INTO_VEHICLE takes the GTA seat (wire seat - 1); the seat cache is
            // zeroed so the next tick reads the real seat. The auto-shuffle flag above should stop GTA from
            // moving us again; the link ignores a differing reading for a moment after the revert.
            const bool ok = g_world->SetPedIntoVehicle(lp, lpVeh, o.revertSeat - 1);
            g_localSeatAtMs = 0;
            g_localSeat = o.revertSeat;
            ++g_v4.reverts;
            game::Overlay::PushSystemLine(sync::LocalVehicleLink::RefusalText(o.resultCode));
            GLOGW("engine: vehicle: %s refused (%s) for a seat change: %s back into seat %d of vehicle %d",
                  VehOpName(o.resultOp), VehResultName(o.resultCode),
                  ok ? "warped" : "COULD NOT warp (SET_PED_INTO_VEHICLE failed)", o.revertSeat, lpVeh);
        }
        if (o.eject && lp && lpVeh)
        {
            // First entry refused: out at once (flag 16 teleports out, no animation), with the reason.
            g_world->TaskLeaveVehicle(lp, lpVeh, 16);
            ++g_v4.ejects;
            const char* why = sync::LocalVehicleLink::RefusalText(o.resultCode);
            game::Overlay::PushSystemLine(*why ? why : "The server refused that vehicle seat.");
            GLOGW("engine: vehicle: %s refused (%s): leaving vehicle %d", VehOpName(o.resultOp),
                  VehResultName(o.resultCode), lpVeh);
        }
        if (o.cmd == sync::VehLinkCmd::None)
            return;
        proto::VehicleCmd c;
        const char* what = "?";
        switch (o.cmd)
        {
        case sync::VehLinkCmd::Enter:
            c.op = proto::VehicleOp::Enter;
            c.vehId = (uint16_t)o.netId;
            c.arg = (uint32_t)o.seat;
            what = "Enter";
            break;
        case sync::VehLinkCmd::Exit:
            c.op = proto::VehicleOp::Exit;
            c.vehId = (uint16_t)o.netId;
            what = "Exit";
            g_exitNetId = o.netId;                     // PedState.move 6 (exiting) names this car for a moment
            g_exitAtMs = nowMs;
            if (g_exitTaskAtMs >= 0.0)
                GLOGI("engine: vehicle: Exit for id=%d goes out %.0f ms after our exit task started (the others' climb-out starts that much late)",
                      o.netId, nowMs - g_exitTaskAtMs);
            else
                GLOGI("engine: vehicle: Exit for id=%d: no CTaskExitVehicle was seen while seated (a warp, a death, or the read is not callable)", o.netId);
            g_exitTaskAtMs = -1.0;
            if (g_entryGuard.NoteExit(nowMs))
            {
                ++g_v4.quickExits;
                GLOGI("engine: vehicle: exit within %.0f s of our F / G entry (the double-F / enter-exit loop: if this repeats, control 23 "
                      "was not the one that consumed the press)", sync::kQuickExitMs / 1000.0);
            }
            break;
        case sync::VehLinkCmd::Register:
        {
            c.op = proto::VehicleOp::Register;
            c.arg = g_world->GetEntityModel(o.veh);
            // A failed read leaves zeros, and 0,0,0 with a zero quaternion would register the car at the
            // world's origin with no rotation: the command is skipped and the link retries in 1.5 s.
            if (!g_world->GetEntityCoords(o.veh, c.x, c.y, c.z)
                || !g_world->GetEntityQuaternion(o.veh, c.rot.x, c.rot.y, c.rot.z, c.rot.w))
            {
                g_link.OnNotSent(nowMs);
                static double warnedMs = -1e9;
                if (nowMs - warnedMs >= 5000.0)
                {
                    warnedMs = nowMs;
                    GLOGW("engine: vehicle: Register of local car %d skipped: its position or rotation did not read; "
                          "retrying", o.veh);
                }
                return;
            }
            int c1 = 0, c2 = 0;
            if (g_world->GetVehicleColours(o.veh, c1, c2))
            {
                c.colour1 = (uint8_t)(c1 >= 0 && c1 <= 255 ? c1 : 0);
                c.colour2 = (uint8_t)(c2 >= 0 && c2 <= 255 ? c2 : 0);
            }
            c.seat = (uint8_t)(o.seat >= 0 && o.seat < (int)proto::kVehSeats ? o.seat : 0);
            what = "Register";
            break;
        }
        case sync::VehLinkCmd::None:
            break;
        }
        const bool sent = join::SendVehicleCmd(c);
        if (!sent)
            g_link.OnNotSent(nowMs);                   // nothing went out: no answer is coming
        ++g_v4.vehCmds;
        GLOGI("engine: vehicle: %s id=%d seat %d (local car %d, model 0x%08x) %s", what, o.netId, o.seat, o.veh,
              (unsigned)(o.cmd == sync::VehLinkCmd::Register ? c.arg : 0), sent ? "sent" : "NOT sent (no session)");
    }

    // ---- 0.4.22 vehicle controls ------------------------------------------------------------------------------------------------------------------
    // GI: "indicators for turn signals, headlights, belt and engine on/off, mechanical keys, small indicators on the car UI", and a fuel gauge.
    // Left / Right Arrow = the left / right indicator, Down Arrow = hazards, Y = engine off / on, B = seatbelt, H = GTA's own INPUT_VEH_HEADLIGHT (74),
    // untouched: the strip reads GET_VEHICLE_LIGHTS_STATE. The key -> state rules, the auto-cancel after a completed turn, the engine guard, the belt
    // and fuel_use are libs/sync/vehicle_controls.h (tested in tests/vehcontrols_pure_tests.h, the wire path in tests/vehcontrols_wire_tests.h).
    // This function feeds the machine (the overlay's key edges, the native reads), calls the natives it names, pushes the strip and the fuel gauge
    // to the overlay and writes the log. Held disabled every tick in a vehicle: 172-175 (the arrow keys' cellphone controls), 29 (B), 246 (Y).
    // Script tick only; lp != 0. veh_keys=0: nothing is read, issued or drawn (the fuel gauge is independent of it).
    const char* VcIndName(uint8_t ind)
    {
        return ind == sync::kVcIndBoth ? "hazards" : (ind == proto::kVeh2IndicatorL ? "left" : (ind == proto::kVeh2IndicatorR ? "right" : "off"));
    }
    bool VcApplyIndicators(int32_t veh, uint8_t ind)
    {
        const bool l = g_world->SetVehicleIndicatorLights(veh, 1, (ind & proto::kVeh2IndicatorL) != 0);
        const bool r = g_world->SetVehicleIndicatorLights(veh, 0, (ind & proto::kVeh2IndicatorR) != 0);
        g_vcx.indCalls += 2;
        g_vcx.indFailed += (l ? 0u : 1u) + (r ? 0u : 1u);
        if (ind & sync::kVcIndBoth)                                   // 0.4.27: the per-model line, at the first call that switches one on
            IndNoteCalls(veh, "our car", (uint8_t)(ind & sync::kVcIndBoth), (l ? 1 : 0) + (r ? 1 : 0), (l ? 0 : 1) + (r ? 0 : 1));
        return l && r;
    }
    NetVeh* VcNetVehOf(int32_t handle)
    {
        if (!handle)
            return nullptr;
        for (auto& kv : g_vehs)
            if (kv.second.handle == handle && !kv.second.removed)
                return &kv.second;
        return nullptr;
    }
    void VcLogEvent(const sync::VcEvent& e, int32_t veh)
    {
        static const char* const kKeyName[sync::kVcKeyCount] = { "Left", "Right", "Down (hazards)", "Y (engine)", "B (belt)" };
        static const char* const kWhyName[] = { "not in a vehicle", "chat / Tab menu / pause menu has the keyboard", "not the driver", "the tank is empty (fuel_use)", "veh_keys=0" };
        if (g_vcx.keyLines >= 300 || (e.kind == sync::VcEv::Rejected && e.b == (uint8_t)sync::VcWhy::Off))
            return;                                              // the 10 s line carries the counts (veh_keys=0 presses are not worth a line)
        ++g_vcx.keyLines;
        switch (e.kind)
        {
        case sync::VcEv::Entered:
            GLOGI("engine: vehicle controls: sat in vehicle %d; the car shows indicators %s; the ped's flag 32 (WillFlyThroughWindscreen) read %s; "
                  "set to true (unbuckled)", veh, VcIndName(e.a), e.b ? "true" : "false");
            break;
        case sync::VcEv::Left:
            GLOGI("engine: vehicle controls: left the car; indicators %s (hazards stay on a parked car, a single side goes out), belt was %s; flag 32 put back",
                  VcIndName(e.a), e.b ? "buckled" : "off");
            break;
        case sync::VcEv::Indicator:
            GLOGI("engine: vehicle controls: indicators -> %s (SET_VEHICLE_INDICATOR_LIGHTS on vehicle %d)", VcIndName(e.a), veh);
            break;
        case sync::VcEv::AutoCancel:
            GLOGI("engine: vehicle controls: the indicator went out by itself after a completed turn (net heading change %d degrees, wheel centred)", (int)e.deg);
            break;
        case sync::VcEv::EngineKey:
            GLOGI("engine: vehicle controls: Y: engine -> %s (SET_VEHICLE_ENGINE_ON %s, instantly %d, disableAutoStart %d)", e.a ? "ON" : "OFF", e.a ? "true" : "false",
                  e.a ? 0 : 1, e.a ? 0 : 1);
            break;
        case sync::VcEv::EngineGuard:
            GLOGI("engine: vehicle controls: the game started the engine of a car we switched off: off again (guard)");
            break;
        case sync::VcEv::FuelCut:
            GLOGI("engine: vehicle controls: fuel_use: the tank is empty, engine cut (and it will not start)");
            break;
        case sync::VcEv::Belt:
            GLOGI("engine: vehicle controls: B: seatbelt %s (PED config flag 32 set to %s)", e.a ? "BUCKLED" : "off", e.a ? "false" : "true");
            break;
        case sync::VcEv::BeltAuto:
            GLOGI("engine: vehicle controls: the exit task started (F): belt unbuckled (flag 32 true)");
            break;
        case sync::VcEv::BeltReassert:
            GLOGI("engine: vehicle controls: the game moved PED config flag 32 under a %s belt: set again", e.a ? "buckled" : "unbuckled");
            break;
        case sync::VcEv::Rejected:
            GLOGI("engine: vehicle controls: %s ignored: %s", e.a < sync::kVcKeyCount ? kKeyName[e.a] : "?", e.b < 5 ? kWhyName[e.b] : "?");
            break;
        case sync::VcEv::None:
            break;
        }
    }
    void TickVehicleControls(int32_t lp, int32_t lpVeh, bool dead, double nowMs)
    {
        if (g_vcReset.exchange(false))
            g_vc.Reset();
        g_vc.Configure(VehControlsConfigNow());
        const bool keysOn = g_vcKeys.load(std::memory_order_relaxed);
        const bool indSync = g_vcIndSync.load(std::memory_order_relaxed);
        const int32_t veh = (lp && lpVeh && !dead) ? lpVeh : 0;
        NetVeh* nv = VcNetVehOf(veh);

        // The seat: the link's own (refreshed every 500 ms by TickLocalVehicle) in a server session, the driver's seat read otherwise.
        int seat = -1;
        if (veh)
        {
            if (ServerMode())
                seat = g_localSeat;
            else if (g_world->GetPedInVehicleSeat(veh, -1) == lp)
                seat = 0;
            else
                seat = 1;
        }

        sync::VehControlsIn in;
        in.nowMs = nowMs;
        in.presses = game::Overlay::TakeVehicleKeys();           // always taken: a press never waits for the next car
        in.vehicle = keysOn ? (uint32_t)veh : 0u;
        in.seat = seat;
        in.blocked = game::Overlay::Interactive() || game::Overlay::QuitPromptOpen() || g_pauseMenuActive;
        if (in.vehicle)
        {
            in.exiting = g_world->GetIsTaskActive(lp, kTaskExitVehicle);
            in.shownInd = (indSync && nv && nv->haveState) ? (uint8_t)(nv->st.flags2 & sync::kVcIndBoth) : (uint8_t)0;
            in.headingDeg = g_world->GetEntityHeading(veh);
            in.speedMps = g_world->GetEntitySpeed(veh);
            float axis = 0.0f;
            in.steerOk = g_world->GetControlNormal(0, kCtlVehMoveLR, axis);
            in.steer = axis;
            bool running = false;
            in.engineOk = g_world->GetVehicleEngineRunning(veh, running);
            in.engineRunning = running;
            if (g_vc.Vehicle() != (uint32_t)veh || nowMs - g_vcBeltReadMs >= 1000.0)
            {
                bool flag = true;
                in.beltFlagOk = g_world->GetPedConfigFlag(lp, sync::kVcPcfWillFlyThroughWindscreen, flag);
                in.beltFlag = flag;
                g_vcBeltReadMs = nowMs;
                ++g_vcx.beltReads;
                if (!in.beltFlagOk)
                    ++g_vcx.beltReadFail;
                else if (flag)
                    ++g_vcx.beltFlagTrue;
            }
            if (nv && nv->haveState)
            {
                in.fuelKnown = true;
                in.fuelPermille = nv->st.fuelPermille;
            }
            for (const int control : sync::kVcDisabledControls)
            {
                uint64_t args[] = { 0, (uint64_t)control, 1 };       // DISABLE_CONTROL_ACTION(0, control, true): a per-frame effect
                g_world->CallNative(game::native::DISABLE_CONTROL_ACTION, args, 3, nullptr);
            }
            ++g_vcx.ticksInVeh;
            if (in.blocked)
                ++g_vcx.ticksBlocked;
            if (!in.engineOk || !in.engineRunning)
                ++g_vcx.engineStopped;
        }

        const sync::VehControlsOut out = g_vc.Tick(in);

        // The natives the machine names. A car we just left first (hazards stay, a single side goes out), then the car we sit in.
        if (out.leftVehicle)
        {
            if (out.leftIndChanged)
                VcApplyIndicators((int32_t)out.leftVehicle, out.leftInd);
            if (NetVeh* lv = VcNetVehOf((int32_t)out.leftVehicle))
                lv->ctlInd = indSync ? out.leftInd : (uint8_t)0;     // what a controller keeps sending for the parked car
        }
        if (out.setIndicators && in.vehicle)
            VcApplyIndicators(veh, out.ind);
        if (out.setEngine && in.vehicle)
        {
            const sync::VcEngineCall call = sync::EngineCallFor(out.engineOn);
            ++g_vcx.engineCalls;
            if (!g_world->SetVehicleEngineOn(veh, call.on, call.instantly, call.disableAutoStart))
                ++g_vcx.engineFailed;
        }
        if (out.setBelt && lp)
        {
            ++g_vcx.beltCalls;
            if (!g_world->SetPedConfigFlag(lp, sync::kVcPcfWillFlyThroughWindscreen, out.beltFlag))
                ++g_vcx.beltFailed;
        }
        for (int i = 0; i < out.nev; ++i)
            VcLogEvent(out.ev[i], out.ev[i].kind == sync::VcEv::Left ? (int32_t)out.leftVehicle : veh);
        // What a controller sends for the car we drive; a passenger's machine state is not a controller's.
        if (nv && in.vehicle && g_vc.Vehicle() == (uint32_t)veh && g_vc.Seat() == 0)
            nv->ctlInd = indSync ? g_vc.Indicators() : (uint8_t)0;

        // The strip and the fuel gauge.
        game::Overlay::VehicleHud hud;
        static bool lastLightsOk = false;
        static int lastLights = 0;
        if (in.vehicle)
        {
            const bool driver = seat == 0;
            const uint8_t ind = sync::HudIndicators(driver, g_vc.Indicators(), nv && nv->haveState ? nv->st.flags2 : (uint8_t)0, indSync);
            bool lightsOn = false, highBeam = false;
            lastLightsOk = g_world->GetVehicleLightsState(veh, lightsOn, highBeam);
            lastLights = sync::HudLights(lastLightsOk, lightsOn, highBeam, g_lightsBeamLit.load(std::memory_order_relaxed));
            if (lastLightsOk)
                ++g_vcx.lightsRaw[(lightsOn ? 1 : 0) | (highBeam ? 2 : 0)];
            if (g_world->IsDisabledControlJustPressed(0, kCtlVehHeadlight))
                ++g_vcx.headlightPresses;
            if (lastLights == 1) ++g_vcx.lightsLow;
            else if (lastLights == 2) ++g_vcx.lightsHigh;
            hud.show = true;
            hud.indLeft = (ind & proto::kVeh2IndicatorL) != 0;
            hud.indRight = (ind & proto::kVeh2IndicatorR) != 0;
            hud.lights = lastLights;
            hud.beltOn = g_vc.Belt();
            hud.beltWarn = sync::HudBeltWarn(g_vc.Belt(), in.speedMps, g_vc.Config().beltWarnMps);
            hud.engineOff = g_vc.EngineOffHud(nowMs);
            hud.lock = (nv && nv->haveState) ? ((nv->st.flags & proto::kVehLocked) ? 1 : 0) : -1;   // 0.4.27 state row: the server's flag
        }
        else if (keysOn && !veh && nowMs < g_lockFlashUntilMs)
        {
            hud.lockFlash = true;                                    // 0.4.27: on foot, the lock the server just confirmed
            hud.lock = g_lockFlashLocked ? 1 : 0;
        }
        game::Overlay::SetVehicleHud(hud);
        g_fuelHud = sync::FuelHudFor(veh != 0, nv != nullptr, nv && nv->haveState, nv ? (unsigned)nv->st.fuelPermille : 1000u);

        // The 10 s line, with the read-backs: the indicator state has no getter (the machine's own state is what is sent and shown), the engine,
        // the lights and the belt flag are read back every tick / about once a second.
        static bool lastEngineOk = false, lastEngineRunning = false, lastBeltOk = false, lastBeltFlag = true;
        if (in.vehicle)
        {
            lastEngineOk = in.engineOk;
            lastEngineRunning = in.engineRunning;
            if (in.beltFlagOk)
            {
                lastBeltOk = true;
                lastBeltFlag = in.beltFlag;
            }
        }
        if (nowMs - g_vcReportMs < 10000.0)
            return;
        g_vcReportMs = nowMs;
        const sync::VehControlsStats s = g_vc.TakeStats();
        const VcExtraStats& x = g_vcx;
        const bool quiet = !x.ticksInVeh && !x.copyIndApplied && !x.copyEngineApplied && !s.rejNotInVehicle;
        if (!quiet || g_vcFirstReport)
        {
            g_vcFirstReport = false;
            GLOGI("engine: vehicle controls (0.4.22), last 10 s: veh_keys=%s indicator_autocancel=%s indicator_sync=%s fuel_use=%s; %u ticks in a vehicle "
                  "(%u with a panel / pause menu holding the keyboard); presses taken: Left %u Right %u hazards %u Y %u B %u; ignored: not in a vehicle %u, "
                  "keyboard held %u, not the driver %u, empty tank %u, veh_keys=0 %u; indicators: %u changes (%u by themselves after a turn), state now %s "
                  "(NO getter exists in 3889, so there is no read-back: the machine's own state is what is sent and shown), SET_VEHICLE_INDICATOR_LIGHTS calls "
                  "%u (%u failed); engine: Y off %u / on %u, guard re-issues %u, fuel cuts %u, SET_VEHICLE_ENGINE_ON calls %u (%u failed), READ-BACK "
                  "GET_IS_VEHICLE_ENGINE_RUNNING %s, stopped on %u of %u ticks",
                  g_vcKeys.load() ? "on" : "OFF", g_vcAutoCancel.load() ? "on" : "OFF", indSync ? "on" : "OFF", g_vcFuelUse.load() ? "ON" : "off",
                  x.ticksInVeh, x.ticksBlocked, s.taken[0], s.taken[1], s.taken[2], s.taken[3], s.taken[4], s.rejNotInVehicle, s.rejBlocked, s.rejNotDriver,
                  s.rejNoFuel, s.rejOff, s.indicatorChanges, s.autoCancels, VcIndName(g_vc.Indicators()), x.indCalls, x.indFailed, s.engineOff, s.engineOn,
                  s.engineGuards, s.fuelCuts, x.engineCalls, x.engineFailed, lastEngineOk ? (lastEngineRunning ? "running" : "stopped") : "not readable",
                  x.engineStopped, x.ticksInVeh);
            GLOGI("engine: vehicle controls (0.4.22), last 10 s, continued: READ-BACK GET_VEHICLE_LIGHTS_STATE %s, low beam %u / high beam %u ticks "
                  "(raw lightsOn/highbeamsOn 0/0 %u, 1/0 %u, 0/1 %u, 1/1 %u; H pressed %u); "
                  "belt: on %u / off %u, unbuckled by F %u, flag re-asserted %u, SET_PED_CONFIG_FLAG(32) calls %u (%u failed), READ-BACK flag 32 %s "
                  "(%u reads, %u failed, true on %u) while the belt is %s; COPIES: SET_VEHICLE_INDICATOR_LIGHTS %u (%u failed) and SET_VEHICLE_ENGINE_ON %u "
                  "applied from the controllers' bits; fuel gauge %s (%u permille)",
                  lastLightsOk ? (lastLights == 0 ? "lights off" : (lastLights == 1 ? "low beam" : "high beam")) : "not readable",
                  x.lightsLow, x.lightsHigh, x.lightsRaw[0], x.lightsRaw[1], x.lightsRaw[2], x.lightsRaw[3], x.headlightPresses, s.beltOn, s.beltOff, s.beltAuto, s.beltReasserts, x.beltCalls, x.beltFailed,
                  lastBeltOk ? (lastBeltFlag ? "true (flies through the windscreen)" : "false (held)") : "not read", x.beltReads, x.beltReadFail, x.beltFlagTrue,
                  g_vc.Belt() ? "buckled" : "off", x.copyIndApplied, x.copyIndFailed, x.copyEngineApplied,
                  g_fuelHud.show ? "shown" : "hidden", (unsigned)(g_fuelHud.frac * 1000.0f + 0.5f));
        }
        const uint32_t keyLines = g_vcx.keyLines;
        g_vcx = VcExtraStats();
        g_vcx.keyLines = keyLines;
    }

    // 0.4.27 indicator fallback: every frame, our own blinking amber light at each lit corner of a car whose indicators the game does not show
    // (veh_ind_fallback: pack cars by default). The car we sit in takes the state the HUD shows (the machine's for the driver, the controller's bits
    // for a passenger); every other networked car the controller's flags2 bits, so remote viewers see it too. Script tick, after TickVehicleControls.
    void TickIndicatorFallback(int32_t lp, int32_t lpVeh, double nowMs)
    {
        const sync::IndFallbackMode mode = (sync::IndFallbackMode)g_indFallbackMode.load(std::memory_order_relaxed);
        const bool indSync = g_vcIndSync.load(std::memory_order_relaxed);
        float px = 0, py = 0, pz = 0;
        const bool havePos = lp && g_world->GetEntityCoords(lp, px, py, pz);
        if (mode != sync::IndFallbackMode::Off && havePos)
        {
            ++g_indx.frames;
            auto drawCar = [&](int32_t h, uint8_t ind) {
                ind = (uint8_t)(ind & sync::kVcIndBoth);
                if (!h || !ind)
                    return;
                const uint32_t model = g_world->GetEntityModel(h);
                if (!model)
                    return;
                IndModelInfo& m = IndInfoFor(h, model);
                if (!sync::UseIndFallback(mode, !m.pack.empty()))
                {
                    ++g_indx.skippedStock;
                    return;
                }
                float cx = 0, cy = 0, cz = 0;
                if (!g_world->GetEntityCoords(h, cx, cy, cz))
                    return;
                const float dx = cx - px, dy = cy - py, dz = cz - pz;
                if (dx * dx + dy * dy + dz * dz > sync::kIndDrawRangeM * sync::kIndDrawRangeM)
                {
                    ++g_indx.skippedFar;
                    return;
                }
                const double since = g_indPhase.Touch(h, ind, nowMs);
                if (since < 0.0)
                    return;                                          // the table is full (counted)
                ++g_indx.carFrames;
                if (!sync::IndBlinkLit(nowMs, since))
                    return;
                ++g_indx.litFrames;
                const unsigned mask = sync::IndCornerMask(ind);
                const sync::V3 centre{cx, cy, cz};
                for (int i = 0; i < sync::kIndCorners; ++i)
                {
                    if (!(mask & (1u << i)))
                        continue;
                    sync::V3 p;
                    bool have = false;
                    if (m.bone[i] >= 0 && g_world->GetWorldPositionOfEntityBone(h, m.bone[i], p.x, p.y, p.z) && (p.x != 0.0f || p.y != 0.0f))
                    {
                        p = sync::IndPushOut(p, centre);
                        have = true;
                        ++g_indx.fromBone;
                    }
                    else if (m.dimsOk)
                    {
                        const sync::V3 o = sync::IndCornerFromDims(i, m.mn, m.mx);
                        have = g_world->GetOffsetInWorld(h, o.x, o.y, o.z, p.x, p.y, p.z);
                        ++(have ? g_indx.fromDims : g_indx.posFailed);
                    }
                    else
                        ++g_indx.noCorner;
                    if (!have)
                        continue;
                    ++(g_world->DrawLightWithRange(p.x, p.y, p.z, sync::kIndR, sync::kIndG, sync::kIndB, sync::kIndRangeM, sync::kIndIntensity)
                           ? g_indx.lights : g_indx.lightFailed);
                }
            };
            // The car we sit in.
            NetVeh* own = VcNetVehOf(lpVeh);
            if (lpVeh)
            {
                const bool driver = g_vc.Vehicle() == (uint32_t)lpVeh && g_vc.Seat() == 0;
                drawCar(lpVeh, sync::HudIndicators(driver, g_vc.Indicators(), own && own->haveState ? own->st.flags2 : (uint8_t)0, indSync));
            }
            // Every other networked car: the controller's bits (a parked car we left with the hazards on is one of these).
            if (indSync)
                for (auto& kv : g_vehs)
                {
                    const NetVeh& v = kv.second;
                    if (!v.handle || v.removed || !v.present || !v.haveState || v.handle == lpVeh)
                        continue;
                    drawCar(v.handle, v.st.flags2);
                }
        }
        g_indPhase.Sweep();

        if (nowMs - g_indReportMs < 10000.0)
            return;
        g_indReportMs = nowMs;
        const unsigned full = g_indPhase.TakeFull();
        if (g_indx.carFrames || g_indx.lightFailed || full)
            GLOGI("engine: indicator fallback (0.4.27), last 10 s: veh_ind_fallback=%s; %u frames, %u car-frames with an indicator on (%u in the lit half), "
                  "DRAW_LIGHT_WITH_RANGE %u (%u failed; range %.1f m, intensity %.1f, rgb %d %d %d, blink %.0f ms), corners from bones %u / from the model box %u "
                  "(%u box offsets failed, %u corners with neither); skipped: stock cars %u, beyond %.0f m %u, phase table full %u; models probed %zu",
                  sync::IndFallbackModeName(mode), g_indx.frames, g_indx.carFrames, g_indx.litFrames, g_indx.lights, g_indx.lightFailed,
                  (double)sync::kIndRangeM, (double)sync::kIndIntensity, sync::kIndR, sync::kIndG, sync::kIndB, sync::kIndPeriodMs,
                  g_indx.fromBone, g_indx.fromDims, g_indx.posFailed, g_indx.noCorner, g_indx.skippedStock, (double)sync::kIndDrawRangeM,
                  g_indx.skippedFar, full, g_indModels.size());
        g_indx = IndFallbackStats();
    }

    // Front axle to rear axle of the vehicle's model, from the wheel bones wheel_lf and wheel_lr (names
    // INFERRED), measured once per model and cached. kFallbackWheelbaseM when the bones are missing (bikes,
    // boats, planes) or cannot be read; the steering estimate for those is not meaningful anyway. Script tick.
    constexpr float kFallbackWheelbaseM = 2.7f;
    float VehicleWheelbaseM(int32_t veh)
    {
        const uint32_t model = g_world->GetEntityModel(veh);
        if (!model)
            return kFallbackWheelbaseM;            // not cached: the entity did not read this time
        const auto it = g_wheelbase.find(model);
        if (it != g_wheelbase.end())
            return it->second;
        float wb = kFallbackWheelbaseM;
        bool measured = false;
        const int front = g_world->GetEntityBoneIndexByName(veh, "wheel_lf");
        const int rear = g_world->GetEntityBoneIndexByName(veh, "wheel_lr");
        float fx = 0, fy = 0, fz = 0, rx = 0, ry = 0, rz = 0;
        if (front >= 0 && rear >= 0 && g_world->GetWorldPositionOfEntityBone(veh, front, fx, fy, fz) &&
            g_world->GetWorldPositionOfEntityBone(veh, rear, rx, ry, rz))
        {
            const float d = std::sqrt((fx - rx) * (fx - rx) + (fy - ry) * (fy - ry) + (fz - rz) * (fz - rz));
            if (std::isfinite(d) && d >= 0.8f && d <= 12.0f)   // a plausible axle distance
            {
                wb = d;
                measured = true;
            }
        }
        g_wheelbase[model] = wb;
        GLOGI("engine: vehicle: model 0x%08x wheelbase %.2f m (%s)", (unsigned)model, wb,
              measured ? "from the wheel_lf / wheel_lr bones" : "fallback: the wheel bones did not read");
        return wb;
    }

    // The controller's reads of a vehicle's body (sync plan W6, protocol v5 flags2 / doors): the lights and high
    // beam (GET_VEHICLE_LIGHTS_STATE), the six doors (GET_VEHICLE_DOOR_ANGLE_RATIO above 0.1 = open) and the
    // convertible roof. There is no interior-light getter in the script API (skipped: bit 2 is always 0) and none
    // for the indicators (bits 3 and 4 are 0 here; since 0.4.22 SendControlledVehicles ORs in the controls' own indicator state, NetVeh::ctlInd).
    // A native that cannot be called leaves its bit 0. Script tick.
    void ReadVehicleBody(int32_t veh, uint8_t& flags2, uint8_t& doors)
    {
        bool lightsOn = false, highBeam = false;
        g_world->GetVehicleLightsState(veh, lightsOn, highBeam);
        float ratios[sync::kBodyDoors] = {};
        for (int d = 0; d < sync::kBodyDoors; ++d)
        {
            float r = 0.0f;
            if (g_world->GetVehicleDoorAngleRatio(veh, d, r))
                ratios[d] = r;
        }
        int roof = 0;
        const bool haveRoof = g_world->GetConvertibleRoofState(veh, roof);
        flags2 = sync::Flags2FromState(sync::LightsLit(lightsOn, highBeam, g_lightsBeamLit.load(std::memory_order_relaxed)), highBeam, false, false, false, haveRoof && sync::RoofDownFromState(roof));
        doors = sync::DoorsFromRatios(ratios);
    }

    // The VehicleUpdate of every spawned server vehicle the server says WE control (ctrl == me, sync plan W6): the
    // car we drive, and the empty or abandoned cars the server handed us because we are the nearest player (it
    // cannot simulate them, so their motion must come from a client or they stand still). Called in the same send
    // tick as SendState with that frame's sample time, so a moving car goes out at the full rate; a still one at
    // 5 Hz, at once on a change (lights, doors, flags) and at once when it stops (sync::VehicleSendGate). A still
    // car is read (velocity) every kVehReadStillMs and its body every kBodyReadMs, not every call; at most
    // kMaxVehicleSends cars go out per call, the rest next call (the cursor rotates), so a lot full of parked cars
    // cannot flood the tick. Script tick.
    constexpr double kVehReadStillMs = 100.0, kBodyReadMs = 100.0;
    constexpr int kMaxVehicleSends = 12;
    uint16_t g_vehSendCursor = 0;                      // the id after the last car served
    void SendControlledVehicles(double nowMs, int32_t sampleMs, int32_t lpVeh)
    {
        const uint16_t me = join::MyId();
        if (g_vehs.empty())
            return;
        static uint32_t seq = 0;
        int sent = 0;
        auto visit = [&](uint16_t id, NetVeh& v) {
            if (!v.handle || !v.haveState || v.removed || v.st.ctrl != me)
                return;
            // A still car is not re-read on every call: its motion is checked every kVehReadStillMs.
            if (!v.moving && v.sentOnce && nowMs - v.lastReadMs < kVehReadStillMs)
                return;
            proto::VehicleUpdate u;
            u.vehId = id;
            if (!g_world->GetEntityCoords(v.handle, u.k.x, u.k.y, u.k.z))
                return;
            u.k.heading = 0;
            if (!g_world->GetEntityQuaternion(v.handle, u.rot.x, u.rot.y, u.rot.z, u.rot.w))
                return;
            // Velocity beyond the protocol's cap is a teleport (a respawn, a warp), not movement: send 0.
            if (!g_world->GetEntityVelocity(v.handle, u.k.vx, u.k.vy, u.k.vz) ||
                !(std::fabs(u.k.vx) <= proto::kMaxSpeed && std::fabs(u.k.vy) <= proto::kMaxSpeed && std::fabs(u.k.vz) <= proto::kMaxSpeed))
                u.k.vx = u.k.vy = u.k.vz = 0;
            float ax = 0, ay = 0, az = 0;
            if (g_world->GetEntityRotationVelocity(v.handle, ax, ay, az))
            {
                auto clampAv = [](float a) { return std::isfinite(a) ? std::max(-proto::kMaxAngVel, std::min(proto::kMaxAngVel, a)) : 0.0f; };
                u.avx = clampAv(ax);
                u.avy = clampAv(ay);
                u.avz = clampAv(az);
            }
            v.lastReadMs = nowMs;
            const float speed = std::sqrt(u.k.vx * u.k.vx + u.k.vy * u.k.vy + u.k.vz * u.k.vz);
            const float spin = std::sqrt(u.avx * u.avx + u.avy * u.avy + u.avz * u.avz);
            v.moving = sync::VehicleIsMoving(speed, spin);
            // Steering (sync plan W2 D): GTA has no steering-angle getter, so the wheel angle is inferred from
            // the yaw rate and the speed along the body's forward axis (a kinematic bicycle). The rotation
            // velocity's unit (rad/s) and frame (world vs body; both read the same for a level car's yaw, which
            // is all this uses) are INFERRED, like the sender's avx..avz. Held below 1 m/s, per vehicle.
            // T3: the yaw rate comes from successive rotations (a unit-safe source: the unit and frame of GET_ENTITY_ROTATION_VELOCITY are
            // unverified) when two reads 5..300 ms apart exist, else from that native as before; the ratio between the two is logged so the
            // first live run shows whether avz was in rad/s. When the local player DRIVES this car and the control native answers, its
            // steering axis is the value below 1 m/s, where the estimate is noise: a standing car shows its wheels turned. Pedals: brake
            // lights and handbrake (flags2 bits 6 and 7).
            const bool drivenByMe = v.handle == lpVeh && g_localSeat == 0;
            float vFwd = 0.0f;
            {
                const sync::DriveQuat bq = sync::vq::Normalize({ u.rot.x, u.rot.y, u.rot.z, u.rot.w });
                const sync::DriveVec fwd = sync::vq::Rotate(bq, { 0.0f, 1.0f, 0.0f });
                vFwd = u.k.vx * fwd.x + u.k.vy * fwd.y + u.k.vz * fwd.z;
                float yaw = u.avz;
                float yawQ = 0.0f;
                if (v.steerPrevRotMs > -1e8 && sync::YawRateFromRotations(v.steerPrevRot, bq, (nowMs - v.steerPrevRotMs) / 1000.0, yawQ))
                {
                    if (std::fabs(yawQ) > 0.15f && std::isfinite(u.avz))
                    {
                        g_vis.yawRatioSum += (double)u.avz / (double)yawQ;
                        ++g_vis.yawRatioN;
                    }
                    yaw = yawQ;
                }
                v.steerPrevRot = bq;
                v.steerPrevRotMs = nowMs;
                bool axisOk = false;
                float axis = 0.0f;
                if (drivenByMe)
                    axisOk = g_world->GetControlNormal(0, kCtlVehMoveLR, axis);
                const float wheelbase = VehicleWheelbaseM(v.handle);   // one model read (a native) per update, not two
                const sync::SteerChoice sc = sync::ChooseSteer(vFwd, yaw, wheelbase, v.steerPrev, axisOk, axis);
                u.steer = sc.steer;
                v.steerPrev = u.steer;
                g_vis.steerSent.Add(sc, axisOk, axis, sync::EstimateSteer(vFwd, yaw, wheelbase, 0.0f), vFwd);
                // 0.4.30 sign proof of the steering field: our own car's angle against the axis held over half (axis + = right, angle + = left)
                float ownAngle = 0.0f;
                if (axisOk && std::fabs(axis) > 0.5f && g_world->VehicleFieldsReady() && g_world->GetVehicleSteerAngle(v.handle, ownAngle) &&
                    std::fabs(ownAngle) > 0.05f)
                    ++((ownAngle > 0.0f) == (axis < 0.0f) ? g_vis.signAgree : g_vis.signDisagree);
            }
            if (g_world->IsVehicleEngineRunning(v.handle)) u.flags |= proto::kVehEngine;
            if (g_world->IsVehicleSirenOn(v.handle))      u.flags |= proto::kVehSiren;
            {
                // 0.4.30: with the siren on, a held E is the siren's second tone and IS_HORN_ACTIVE stays false: send the held key
                const bool hornActive = g_world->IsHornActive(v.handle);
                const bool sirenOn = (u.flags & proto::kVehSiren) != 0;
                float hornAxis = 0.0f;
                const bool hornAxisOk = !hornActive && sirenOn && drivenByMe && g_world->GetControlNormal(0, kCtlVehHorn, hornAxis);
                if (v.sirenHornHold.Step(hornActive, sync::HornBitToSend(hornActive, sirenOn, drivenByMe, hornAxisOk, hornAxis,
                                                                         g_sirenHorn.load(std::memory_order_relaxed)), nowMs))
                {
                    u.flags |= proto::kVehHorn;
                    if (!hornActive) ++g_sig.sentSirenHorn;
                }
                if (sirenOn) ++g_sig.sentSiren;
            }
            // The body (lights, doors, roof): at most every kBodyReadMs, the last read in between.
            if (nowMs - v.bodyReadMs >= kBodyReadMs)
            {
                ReadVehicleBody(v.handle, v.bodyFlags2, v.bodyDoors);
                v.bodyReadMs = nowMs;
            }
            // T3: the pedals every call (a brake light is seen the moment it is pressed), the tyres / windows every kVisReadMs.
            bool brakeBit = false, handBit = false;
            if (drivenByMe)
            {
                float accel = 0.0f, brake = 0.0f, hand = 0.0f;
                const bool pedalsOk = g_world->GetControlNormal(0, kCtlVehAccelerate, accel) && g_world->GetControlNormal(0, kCtlVehBrake, brake) &&
                                      g_world->GetControlNormal(0, kCtlVehHandbrake, hand);
                if (!pedalsOk)
                    ++g_vis.pedalReadFail;
                // R3 T4: a handbrake read that is held at the first sample of a rolling car, or that stays held while the car does not slow down, is
                // a stuck read and is not sent (the brake lights then come from the foot brake alone). veh_visual_guard=0: the v6 classification.
                const bool guardOn = g_vehVisualGuard.load(std::memory_order_relaxed);
                const uint32_t stuckBefore = v.pedalGuard.Stats().stuckRuns;
                const sync::BrakeState bs = guardOn ? v.pedalGuard.Classify(pedalsOk, accel, brake, hand, vFwd, nowMs)
                                                    : sync::ClassifyBrake(pedalsOk, accel, brake, hand, vFwd);
                if (guardOn && v.pedalGuard.Stats().stuckRuns != stuckBefore && g_dmg.handLogged < 12)
                {
                    ++g_dmg.handLogged;
                    PeerLog(true, "engine: vehicle id=%u sanity: the handbrake control (id %d) reads %.2f at %.1f m/s and the car does not slow down: a stuck read, NOT sent as "
                            "a handbrake until it reads released (pedals: accelerate %.2f, brake %.2f)", id, kCtlVehHandbrake, (double)hand, (double)vFwd, (double)accel, (double)brake);
                }
                brakeBit = bs.brakeLights;
                handBit = bs.handbrake;
            }
            if (nowMs - v.visReadMs >= kVisReadMs)
            {
                VehVisualGame game{ v.handle };
                if (g_vehVisualGuard.load(std::memory_order_relaxed))
                {
                    // R3 T4: what goes on the wire passes the per-slot baselines. The body health falling is the corroborating signal.
                    NoteVehicleBody(v, nowMs);
                    sync::VisualMask flagged;
                    v.visReport = sync::ReadReportGuarded(v.vis, v.visGuard, game, sync::VisualMask{ v.st.tyres, v.st.tyresRim, v.st.windows },
                                                          nowMs - v.bodyFellMs <= 3000.0, &flagged);
                    if (flagged.Any())
                        NoteUnreliableVisual(id, flagged);
                }
                else
                    v.visReport = sync::ReadReport(v.vis, game);
                v.visReadMs = nowMs;
            }
            // R3 T4: the damage block. Read every 500 ms (250 ms while the body health is falling), through the baselines; it rides the next
            // few updates after a change (the state channel may lose one) and is repeated every 3 s while the server does not show it.
            const bool dmgOn = g_vehDamageSync.load(std::memory_order_relaxed);
            if (dmgOn && nowMs - v.dmgReadMs >= (nowMs - v.bodyFellMs <= 3000.0 ? 250.0 : 500.0))
            {
                EnsureVehicleHull(v);
                VehDamageGame dg{ v.handle, v.dmgHull };
                sync::DamageReporter::Flagged fl;
                v.dmgReport = sync::StepDamageController(v.dmgRep, dg, v.st.dmg, nowMs, &fl, true, &v.dmgRaw);
                v.dmgReadMs = nowMs;
                ++g_dmg.ctlReads;
                if (v.dmgRaw.bodyOk && std::isfinite(v.dmgRaw.body))
                {
                    if (v.lastBodyHp >= 0.0f && v.dmgRaw.body < v.lastBodyHp - 0.5f)
                        v.bodyFellMs = nowMs;
                    v.lastBodyHp = v.dmgRaw.body;
                }
                v.dmgSend.Observe(v.dmgReport, v.st.dmg);
                if (fl.parts || fl.dents || fl.petrol)
                {
                    g_dmg.flaggedParts += (uint32_t)sync::PopCount8(fl.parts);
                    g_dmg.flaggedDents += (uint32_t)sync::PopCount8(fl.dents);
                    g_dmg.flaggedPetrol += fl.petrol ? 1u : 0u;
                    static int flagLogged = 0;
                    if (flagLogged < 30)
                    {
                        ++flagLogged;
                        PeerLog(true, "engine: vehicle id=%u sanity: door/lamp bits %02x, dent probe(s) %02x%s read as DAMAGED at the first sample while the server shows none: "
                                "unreliable, not sent", id, fl.parts, fl.dents, fl.petrol ? " and the petrol tank (low)" : "");
                    }
                }
            }
            const bool sendDmg = dmgOn && v.dmgSend.Due(v.dmgReport, v.st.dmg, nowMs);
            u.flags2 = (uint8_t)((v.bodyFlags2 & ~(proto::kVeh2Brake | proto::kVeh2Handbrake)) | (brakeBit ? proto::kVeh2Brake : 0) |
                                 (handBit ? proto::kVeh2Handbrake : 0));
            // 0.4.22: the indicators have no getter, so the controls' own state (the key machine of the car we drive, or what a car we left / took
            // over keeps blinking) is what goes out; veh_indicator_sync=0 sends them clear as before.
            u.flags2 = sync::ControllerFlags2(u.flags2, v.ctlInd, g_vcIndSync.load(std::memory_order_relaxed));
            // A door that is broken off reads as open for ever: it is not an open door (the copy would fight the damage replay).
            u.doors = (uint8_t)(v.bodyDoors & ~(dmgOn ? (v.dmgReport.parts & proto::kVehDmgDoorsMask) : 0));
            u.tyres = v.visReport.tyres;
            u.tyresRim = v.visReport.rim;
            u.windows = v.visReport.windows;
            if (sendDmg)
            {
                u.hasDmg = true;
                u.dmg = v.dmgReport;
            }
            const bool changed = !v.sentOnce || u.flags != v.sentFlags || u.flags2 != v.sentFlags2 || u.doors != v.sentDoors ||
                                 v.visReport != v.sentVis || std::fabs(u.steer - v.sentSteer) > 0.04f || (sendDmg && v.dmgSend.Fresh());
            if (!v.sendGate.Due(nowMs, v.moving, changed))
                return;
            auto clampHp = [](float hp) -> uint16_t {
                if (!std::isfinite(hp) || hp <= 0.0f) return 0;
                return hp >= (float)proto::kMaxVehicleHealth ? proto::kMaxVehicleHealth : (uint16_t)hp;
            };
            float hp = 0;
            if (g_world->GetVehicleBodyHealth(v.handle, hp))   u.bodyHealth = clampHp(hp);
            if (g_world->GetVehicleEngineHealth(v.handle, hp)) u.engineHealth = clampHp(hp);
            u.seq = ++seq;
            if (join::SendVehicleState(u, sampleMs))
            {
                ++g_v4.vehUpdates;
                {
                    // R3 T5, the controller's side of the position line: how fast the cars we send were going (the other machine's per-band numbers are its
                    // view of exactly these frames).
                    const double sp = std::sqrt((double)u.k.vx * u.k.vx + (double)u.k.vy * u.k.vy + (double)u.k.vz * u.k.vz);
                    ++g_t5.sentFrames[sync::PositionDiag::SpeedBand(sp)];
                    g_t5.sentSpeedSum += sp;
                    ++g_t5.sentN;
                }
                v.sentOnce = true;
                v.sentFlags = u.flags;
                v.sentFlags2 = u.flags2;
                v.sentDoors = u.doors;
                v.sentVis = v.visReport;
                v.sentSteer = u.steer;
                if (u.hasDmg)
                {
                    v.dmgSend.Sent(nowMs);
                    ++g_dmg.blocksSent;
                }
                ++g_vis.sentUpdates;
                ++g_sig.sentUpdates;                                   // 0.4.18: what our own reads put on the wire (IS_HORN_ACTIVE, GET_VEHICLE_LIGHTS_STATE)
                if (u.flags & proto::kVehHorn) ++g_sig.sentHorn;
                if (u.flags2 & proto::kVeh2Headlights) ++g_sig.sentHeadlights;
                if (u.flags2 & proto::kVeh2HighBeam) ++g_sig.sentHighBeam;
                if (u.tyres) ++g_vis.sentWithTyres;
                if (u.windows) ++g_vis.sentWithWindows;
                if (u.flags2 & proto::kVeh2Brake) ++g_vis.brakeBitsSent;
                if (u.flags2 & proto::kVeh2Handbrake) ++g_vis.handBitsSent;
            }
            ++sent;
        };
        // Ascending ids from the cursor, wrapping once; the cursor then rests after the last car served.
        uint16_t last = g_vehSendCursor;
        bool any = false;
        for (auto it = g_vehs.lower_bound(g_vehSendCursor); it != g_vehs.end() && sent < kMaxVehicleSends; ++it)
        {
            visit(it->first, it->second);
            last = (uint16_t)(it->first + 1);
            any = true;
        }
        if (sent < kMaxVehicleSends)
            for (auto it = g_vehs.begin(); it != g_vehs.end() && it->first < g_vehSendCursor && sent < kMaxVehicleSends; ++it)
            {
                visit(it->first, it->second);
                last = (uint16_t)(it->first + 1);
                any = true;
            }
        g_vehSendCursor = (any && sent >= kMaxVehicleSends) ? last : 0;
    }

    // ---- protocol v4: health, hits, respawn (script tick) ------------------------------------------

    // T1 seats: one SeatNotice, in the order the server sent it (DrainEvents). Script tick.
    //   Reserved / Seated / Cancelled go to the book and to the replay of the player concerned (TickPeerSeatReplay acts on them);
    //   a Cancelled about US also voids our own walk (LocalEnterFlow); Exited about someone else runs the normal exit animation at once.
    void HandleSeatNotice(const proto::SeatNotice& n, double nowMs)
    {
        ++g_t1.noticesIn;
        if (!g_seatIntentOn.load())
            return;
        const uint16_t me = join::MyId();
        const sync::NoticeEffect fx = g_seats.OnNotice(n, me, nowMs);
        static int logged = 0;
        if (logged < 40)
        {
            ++logged;
            GLOGI("engine: seats: notice %s player %u vehicle id=%u seat %u detail %u%s", proto::ToString(n.kind), (unsigned)n.player, (unsigned)n.vehId,
                  (unsigned)n.seat, (unsigned)n.detail, fx.aboutMe ? " (about us)" : "");
        }
        if (fx.aboutMe)
        {
            ++g_t1.noticesAboutUs;
            if (n.kind == proto::SeatNoticeKind::Cancelled)
                g_localEnter.OnServerCancel(n.vehId, n.seat);
            return;
        }
        if (fx.unseatPuppet)
        {
            const auto pit = g_peers.find(fx.player);
            if (pit == g_peers.end() || !pit->second.ped || pit->second.deadShown)
            {
                ++g_t1.exitsNoPed;
                return;
            }
            Peer& pe = pit->second;
            const auto vit = g_vehs.find((uint16_t)fx.veh);
            if (pe.seatedVeh == fx.veh || g_world->IsPedInVehicle(pe.ped))
            {
                UnseatPeer(pe, vit != g_vehs.end() && !vit->second.removed ? vit->second.handle : 0);
                ++g_t1.exitsReplayed;
            }
        }
    }

    // T1 seats: the server's answer to our EnterIntent / CancelIntent (not the link's: it must never see these ops). Script tick.
    void HandleIntentResult(const proto::VehicleResult& r)
    {
        if (r.op == proto::VehicleOp::CancelIntent)
            return;                                    // Ok whether or not there was a reservation: nothing to learn
        const uint8_t code = (uint8_t)r.code;
        if (r.code == proto::VehicleResultCode::Ok)
            ++g_t1.intentsOk;
        else
        {
            ++g_t1.intentsRefused;
            ++g_t1.refusedBy[code < 16 ? code : 15];
            GLOGW("engine: seats: the server REFUSED our entry intent on vehicle id=%u: %s", (unsigned)r.vehId, VehResultName(code));
        }
        g_localEnter.OnResult((int)r.vehId, code);
    }

    // Damage about us / RespawnOk / Killed / VehicleResult from the join worker. Runs before TickLife,
    // so a RespawnOk is seen the frame it is taken. The first two never pass through the bounded queue
    // (g_inbox and g_respawnSlot), so a flood of kill-feed lines cannot lose them.
    void DrainEvents(double nowMs)
    {
        static std::vector<NetEvent> evs;              // script tick only
        evs.clear();
        sync::DamageSlot ds;
        bool haveDamage = false, haveRespawn = false;
        proto::RespawnOk respawn;
        {
            std::lock_guard<std::mutex> lk(g_evMtx);
            haveDamage = g_inbox.Take(ds);
            if (g_respawnSlotFull)
            {
                respawn = g_respawnSlot;
                haveRespawn = true;
                g_respawnSlotFull = false;
            }
            g_evQueue.Drain(evs);
        }
        if (!haveDamage && !haveRespawn && evs.empty())
            return;
        const uint16_t me = join::MyId();
        auto nameOf = [](uint16_t id) -> std::string {
            if (id == proto::kServerId) return "the server";
            PeersLock lk(g_peersMtx, true);            // the join worker's names are the freshest
            auto it = g_peerNet.find(id);
            return it != g_peerNet.end() ? it->second.name : "player " + std::to_string(id);
        };
        if (haveRespawn)
        {
            g_respawnOk = respawn;
            g_haveRespawnOk = true;
            GLOGI("engine: combat: RespawnOk at %.1f, %.1f, %.1f heading %.0f, health %u armour %u", respawn.x, respawn.y,
                  respawn.z, respawn.heading, (unsigned)respawn.health, (unsigned)respawn.armor);
        }
        if (haveDamage)
        {
            if (ds.events > 0)
            {
                g_vitals.OnDamageMerged(ds.health, ds.armour, ds.damage, ds.selfAckDamage, ds.events, ds.killed);
                g_v4.damageIn += ds.events;
                g_v4.damageInTotal += ds.damage;
                if (ds.otherEvents > 0)
                {
                    char many[32] = "";
                    if (ds.otherEvents > 1)
                        std::snprintf(many, sizeof(many), " [%u hits]", (unsigned)ds.otherEvents);
                    GLOGI("engine: combat: hit by %s for %u (weapon 0x%08x) -> health %u armour %u%s%s",
                          nameOf(ds.lastAttacker).c_str(), (unsigned)ds.otherDamage, (unsigned)ds.lastWeapon,
                          (unsigned)ds.health, (unsigned)ds.armour, many, ds.killed ? " (killed)" : "");
                }
                ReactLocalHit(ds, nowMs);              // W3: flinch / knockdown from the damage others dealt (cosmetic)
            }
            else if (ds.killed)
                g_vitals.OnKilled();                   // a Killed about us with no Damage before it
        }
        for (const NetEvent& e : evs)
        {
            switch (e.kind)
            {
            case NetEvent::Killed:
            {
                const proto::Killed& k = e.killed;
                ++g_v4.kills;
                const std::string victim = k.victimId == me ? std::string("You") : nameOf(k.victimId);
                // "A killed B with Carbine Rifle." (the weapon's name when it is a known one), "... (headshot)." and
                // "A beat B to death with Knife." from Killed's flags (protocol v5; sync::KillFeedLine).
                const bool noKiller = k.killerId == k.victimId || k.killerId == proto::kNoPlayer;
                const std::string line = sync::KillFeedLine(noKiller ? std::string() : (k.killerId == me ? std::string("You") : nameOf(k.killerId)),
                                                            victim, sync::WeaponLabel(k.weapon), k.flags, noKiller);
                game::Overlay::PushSystemLine(line.c_str());
                GLOGI("engine: combat: kill feed: %s (victim %u, killer %u, weapon 0x%08x, flags 0x%02x)", line.c_str(),
                      (unsigned)k.victimId, (unsigned)k.killerId, (unsigned)k.weapon, (unsigned)k.flags);
                break;
            }
            case NetEvent::SeatNote:
                HandleSeatNotice(e.seat, nowMs);
                break;
            case NetEvent::VehResult:
            {
                const proto::VehicleResult& r = e.veh;
                if (r.op == proto::VehicleOp::EnterIntent || r.op == proto::VehicleOp::CancelIntent)
                {
                    HandleIntentResult(r);             // T1: ours, not the link's
                    break;
                }
                if (r.op == proto::VehicleOp::SetLocked)
                {
                    HandleLockResult(r, nowMs);        // 0.4.22: the L key's answer, not the link's
                    break;
                }
                if (r.op == proto::VehicleOp::Enter && r.code == proto::VehicleResultCode::Locked)
                    g_dsState.keys.Forget((int)r.vehId);   // 0.4.22: the key we believed in is gone (somebody else drove it since)
                g_link.OnResult((uint8_t)r.op, (uint8_t)r.code, r.vehId);
                if (r.code == proto::VehicleResultCode::Ok)
                {
                    ++g_v4.vehOk;
                    GLOGI("engine: vehicle: server answered %s id=%u: Ok", VehOpName((uint8_t)r.op), (unsigned)r.vehId);
                }
                else
                {
                    ++g_v4.vehRefused;
                    GLOGW("engine: vehicle: server REFUSED %s id=%u: %s", VehOpName((uint8_t)r.op), (unsigned)r.vehId,
                          VehResultName((uint8_t)r.code));
                    // An Enter refused for the seat itself gets its own line from TickLocalVehicle (eject or
                    // revert); only the others are reported here.
                    const bool seatRefusal = r.op == proto::VehicleOp::Enter && sync::LocalVehicleLink::Ejects((uint8_t)r.code);
                    if (!sync::LocalVehicleLink::Retryable((uint8_t)r.code) && !seatRefusal)
                    {
                        char line[96];
                        std::snprintf(line, sizeof(line), "Vehicle %s refused: %s.", VehOpName((uint8_t)r.op),
                                      VehResultName((uint8_t)r.code));
                        game::Overlay::PushSystemLine(line);
                    }
                }
                break;
            }
            }
        }
    }

    // W8 item 6 (script tick): while a session is on, every AI ped's weapon and melee damage modifier is 0.0
    // (SET_AI_WEAPON_DAMAGE_MODIFIER / SET_AI_MELEE_WEAPON_DAMAGE_MODIFIER), so the remote players' puppets, which are AI
    // peds firing real shots, hurt nobody on this machine: damage is the server's. The two natives are global, are
    // re-asserted every 5 s (a revive or a game state change may restore them) and RESET when the session ends. Up to 5 tries
    // when a native cannot be called, then given up for the session (the clear-line check of PlayPeerShots then stays).
    // Hyper tune (GI 2026-11-02; the Dubsta 2026-11-04): the cars in sync::kHyperProfiles (libs/sync/hypertune.h has every number and the
    // pure assist math) get giga acceleration and a 500 km/h top speed while the LOCAL player drives them. A profile with `tough` also
    // gets the toughness natives (strong body and axles, tyres that do not burst, wheels that do not break, health kept at 1000), and one
    // with assists gets the per-frame handling assists (downforce, lateral grip, brake assist, a yaw-rate cap, anti-roll damping) as ONE
    // velocity write and at most one angular velocity write per frame. The owner's client is the authority for its car, so the other
    // players see all of it through the normal vehicle sync. Script tick only.
    // HKCU\Software\GAGE-MP, read when a hyper car is first driven: hyper_tune=0 switches the whole tune off (the Jesko too);
    // hyper_assist=<bit mask: 1 downforce, 2 lateral grip, 4 brake assist, 8 yaw cap, 16 anti-roll> (0 = none; default all 31) switches
    // single assists off. Nothing here is a guess the game cannot correct: every velocity / angular write is read back in the same tick
    // and the assists drop out (one WARN line) when it does not hold.
    struct HyperState                                  // the vehicle being driven and what the tune has seen of it
    {
        int32_t veh = 0;
        uint32_t mask = 0;                             // the assists in force: the registry's mask minus what a read-back dropped
        double lastMs = 0, flagsMs = 0, healthMs = 0, logMs = 0;
        bool velDead = false, angDead = false;
        uint32_t velWrites = 0, angWrites = 0;         // for the read-back schedule (the first 12 writes, then one in 120)
        uint32_t velOk = 0, velBad = 0, angOk = 0, angBad = 0;
        // the 10 s window
        uint32_t frames = 0, grounded = 0, brakeDown = 0, downN = 0, gripN = 0, brakeN = 0, yawN = 0, velN = 0, angN = 0;
        uint32_t readFail = 0, velFail = 0, angFail = 0, healthFixed = 0;
        double downSum = 0, gripSum = 0, brakeSum = 0, yawCutSum = 0, yawCutMax = 0;
        float peakMs = 0;
    };
    struct HyperTough { bool strong = false, axles = false, tyres = false, wheels = false, degrade = false, tank = false; };
    constexpr float kHyperGroundedHeightM = 1.6f;      // the fallback when IS_ENTITY_IN_AIR cannot be called: the origin this close to the ground

    HyperTough HyperApplyTough(int32_t veh)
    {
        HyperTough t;
        t.strong = g_world->SetVehicleStrong(veh, true);
        t.axles = g_world->SetVehicleHasStrongAxles(veh, true);
        t.tyres = g_world->SetVehicleTyresCanBurst(veh, false);
        t.wheels = g_world->SetVehicleWheelsCanBreak(veh, false);
        t.degrade = g_world->SetVehicleEngineCanDegrade(veh, false);
        t.tank = g_world->SetDisableVehiclePetrolTankDamage(veh, true);
        return t;
    }

    // A write read back in the same tick (nothing has stepped the physics in between): the first 12 writes of a drive, then one in 120.
    // A write the game did not take, or took in another unit or frame, drops the assists that depend on it (4 bad reads, more bad than good).
    void HyperVerifyVelocity(HyperState& st, int32_t veh, const sync::HyperVec& wrote)
    {
        const uint32_t n = st.velWrites++;
        if (!(n < 12 || n % 120 == 0))
            return;
        float x = 0, y = 0, z = 0;
        const auto close = [](float a, float b) { return std::fabs(a - b) <= 0.02f + 0.002f * std::fabs(b); };
        if (g_world->GetEntityVelocity(veh, x, y, z) && close(x, wrote.x) && close(y, wrote.y) && close(z, wrote.z))
        {
            ++st.velOk;
            return;
        }
        ++st.velBad;
        static int logged = 0;
        if (logged < 4)
        {
            ++logged;
            GLOGW("engine: hyper assist: SET_ENTITY_VELOCITY wrote %.2f %.2f %.2f and the same-tick read says %.2f %.2f %.2f (bad %u, ok %u)",
                  (double)wrote.x, (double)wrote.y, (double)wrote.z, (double)x, (double)y, (double)z, st.velBad, st.velOk);
        }
        if (st.velBad >= 4 && st.velBad > st.velOk)
        {
            st.velDead = true;
            st.mask = 0;
            GLOGW("engine: hyper assist: the velocity write does not read back (bad %u, ok %u): ALL handling assists are OFF for this session "
                  "(the toughness natives and the power / top speed stay)", st.velBad, st.velOk);
        }
    }

    // `intended` is the WORLD angular velocity the assist wants, `wrote` the vector actually handed to the setter (the body-frame version
    // of it with the 0.4.22 fix, the same world vector without), `q` the car's orientation at the write.
    //
    // Before 0.4.22 (veh_shake_fix=0) the check was "does the read-back equal what was written, x / y within 3 % of the vector": on the
    // live Jesko (0.4.14, 2026-11-05) the yaw read back to 3 decimals while roll / pitch were 0.01-0.02 off, which was put down to the car's
    // body tilt. It was the FRAME: the setter takes a body vector, so the x / y part comes back turned by the heading (gage.log 08:06:52:
    // wrote -0.019 0.032 -0.002, read 0.037 -0.001 -0.001 at heading ~238 = R(q) * wrote). The old check passed only at headings near 0 and
    // killed the assist otherwise. With the fix the test is "the read-back (world) equals the intended world vector", and every read-back
    // also goes to the frame judge, which flips the writers' frame if the evidence says the setter takes the other one.
    void HyperVerifyAngular(HyperState& st, int32_t veh, const sync::DriveQuat& q, const sync::HyperVec& intended, const sync::DriveVec& wrote)
    {
        const uint32_t n = st.angWrites++;
        if (!(n < 12 || n % 120 == 0))
            return;
        float x = 0, y = 0, z = 0;
        bool good = false;
        if (g_world->GetEntityRotationVelocity(veh, x, y, z))
        {
            g_angJudge.Add(sync::shake::ClassifyAngWrite(q, wrote, { x, y, z }));
            if (g_shakeFix.load())
            {
                const float mag = std::sqrt(intended.x * intended.x + intended.y * intended.y + intended.z * intended.z);
                good = sync::shake::Dist3({ x, y, z }, { intended.x, intended.y, intended.z }) <= 0.01f + 0.01f * mag;
            }
            else
            {
                const float mag = std::sqrt(wrote.x * wrote.x + wrote.y * wrote.y + wrote.z * wrote.z);
                const auto close = [](float a, float b) { return std::fabs(a - b) <= 0.01f + 0.01f * std::fabs(b); };
                const auto closeTilt = [mag](float a, float b) { return std::fabs(a - b) <= 0.01f + 0.03f * mag; };
                good = closeTilt(x, wrote.x) && closeTilt(y, wrote.y) && close(z, wrote.z);
            }
        }
        if (good)
        {
            ++st.angOk;
            return;
        }
        ++st.angBad;
        static int logged = 0;
        if (logged < 4)
        {
            ++logged;
            GLOGW("engine: hyper assist: SET_ENTITY_ANGULAR_VELOCITY wrote %.3f %.3f %.3f (intended world %.3f %.3f %.3f) and the same-tick "
                  "GET_ENTITY_ROTATION_VELOCITY says %.3f %.3f %.3f (bad %u, ok %u; veh_shake_fix %s)", (double)wrote.x, (double)wrote.y, (double)wrote.z,
                  (double)intended.x, (double)intended.y, (double)intended.z, (double)x, (double)y, (double)z, st.angBad, st.angOk,
                  g_shakeFix.load() ? "on" : "OFF");
        }
        if (st.angBad >= 4 && st.angBad > st.angOk)
        {
            st.angDead = true;
            st.mask &= ~sync::kHyperAssistAngular;
            GLOGW("engine: hyper assist: the angular velocity write does not read back (bad %u, ok %u): the yaw cap and the anti-roll damping "
                  "are OFF for this session", st.angBad, st.angOk);
        }
    }

    // One frame of the assists on the car the local player drives: read the car, ask the pure step, write what changed.
    void HyperAssistFrame(HyperState& st, const sync::HyperProfile& p, int32_t veh, double now)
    {
        const double dtMs = st.lastMs > 0.0 ? now - st.lastMs : 0.0;
        st.lastMs = now;
        g_shk.assistMask = st.mask;
        float vx = 0, vy = 0, vz = 0, qx = 0, qy = 0, qz = 0, qw = 1;
        if (!g_world->GetEntityVelocity(veh, vx, vy, vz))
        {
            ++st.readFail;
            return;
        }
        {
            const float sp = std::sqrt(vx * vx + vy * vy + vz * vz);   // the peak speed is logged with the assists switched off too
            ++st.frames;
            if (sp > st.peakMs) st.peakMs = sp;
        }
        if (st.mask == 0 || !(dtMs >= 1.0 && dtMs <= 100.0))
            return;                                    // assists off, the first frame of a drive, a pause or a hitch: no write
        if (!g_world->GetEntityQuaternion(veh, qx, qy, qz, qw))
        {
            ++st.readFail;
            return;
        }
        const sync::DriveQuat q = sync::vq::Normalize({ qx, qy, qz, qw });
        const sync::DriveVec f = sync::vq::Rotate(q, { 0.0f, 1.0f, 0.0f });
        const sync::DriveVec r = sync::vq::Rotate(q, { 1.0f, 0.0f, 0.0f });
        const sync::DriveVec u = sync::vq::Rotate(q, { 0.0f, 0.0f, 1.0f });
        sync::HyperAssistIn in;
        in.vel = { vx, vy, vz };
        in.fwd = { f.x, f.y, f.z };
        in.right = { r.x, r.y, r.z };
        in.up = { u.x, u.y, u.z };
        float wx = 0, wy = 0, wz = 0;
        in.angVel = g_world->GetEntityRotationVelocity(veh, wx, wy, wz) ? sync::HyperVec{ wx, wy, wz }
                                                                        : sync::HyperVec{ NAN, NAN, NAN };   // no read: the angular part stays out
        float brake = 0, accel = 0;                    // left at 0 when the control native cannot be called: no brake assist then
        g_world->GetControlNormal(0, kCtlVehBrake, brake);
        g_world->GetControlNormal(0, kCtlVehAccelerate, accel);
        bool inAir = false;
        if (g_world->IsEntityInAir(veh, inAir))
            in.grounded = !inAir;
        else
        {
            const float h = g_world->GetEntityHeightAboveGround(veh);   // 0 when it cannot be called: unknown, not "on the ground"
            in.grounded = h > 0.05f && h < kHyperGroundedHeightM;
        }
        in.brake = brake;
        in.accel = accel;
        in.dt = (float)(dtMs / 1000.0);
        in.mask = st.mask;
        sync::HyperAssist a = p.assist;
        if (!g_hyperYaw0430.load(std::memory_order_relaxed))
            sync::RestoreYawCap0429(a);
        const sync::HyperAssistOut out = sync::HyperAssistStep(a, in);
        if (in.grounded) ++st.grounded;
        if (brake > 0.05f) ++st.brakeDown;
        if (out.downMs2 > 0) { ++st.downN; st.downSum += out.downMs2; }
        if (out.gripMs2 > 0) { ++st.gripN; st.gripSum += out.gripMs2; }
        if (out.brakeMs2 > 0) { ++st.brakeN; st.brakeSum += out.brakeMs2; }
        if (out.yawClamped)
        {
            ++st.yawN;
            st.yawCutSum += out.yawCut;
            st.yawCutMax = std::max(st.yawCutMax, (double)out.yawCut);
        }
        if (out.velChanged)
        {
            if (g_world->SetEntityVelocity(veh, out.vel.x, out.vel.y, out.vel.z))
            {
                ++st.velN;
                ++g_shk.assistVelWrites;                // 0.4.22: how hard the assist pushes the car per write (the shake line)
                const double dv = std::sqrt((double)(out.vel.x - in.vel.x) * (out.vel.x - in.vel.x) + (double)(out.vel.y - in.vel.y) * (out.vel.y - in.vel.y) +
                                            (double)(out.vel.z - in.vel.z) * (out.vel.z - in.vel.z));
                g_shk.dvSum += dv;
                g_shk.dvMax = std::max(g_shk.dvMax, dv);
                HyperVerifyVelocity(st, veh, out.vel);
            }
            else if (++st.velFail >= 30 && st.velN == 0)
            {
                st.velDead = true;
                st.mask = 0;
                GLOGW("engine: hyper assist: SET_ENTITY_VELOCITY cannot be called: the handling assists are OFF for this session");
            }
        }
        if (out.angChanged && !st.angDead && st.mask != 0)
        {
            // 0.4.22: the setter takes the BODY frame, the assist computed a WORLD vector (see libs/sync/vehicle_shake.h): hand it the
            // body-frame version unless veh_shake_fix=0 or the frame judge has found the setter takes the world frame after all.
            const bool body = g_shakeFix.load() && g_angJudge.UseBody();
            const sync::DriveVec wvec = body ? sync::shake::WorldToBody(q, { out.angVel.x, out.angVel.y, out.angVel.z })
                                             : sync::DriveVec{ out.angVel.x, out.angVel.y, out.angVel.z };
            if (g_world->SetEntityAngularVelocity(veh, wvec.x, wvec.y, wvec.z))
            {
                ++st.angN;
                ++g_shk.assistAngWrites;
                ++(body ? g_shk.assistAngBody : g_shk.assistAngWorld);
                const double dw = std::sqrt((double)(out.angVel.x - in.angVel.x) * (out.angVel.x - in.angVel.x) + (double)(out.angVel.y - in.angVel.y) * (out.angVel.y - in.angVel.y) +
                                            (double)(out.angVel.z - in.angVel.z) * (out.angVel.z - in.angVel.z));
                g_shk.dwSum += dw;
                g_shk.dwMax = std::max(g_shk.dwMax, dw);
                HyperVerifyAngular(st, veh, q, out.angVel, wvec);
            }
            else if (++st.angFail >= 30 && st.angN == 0)
            {
                st.angDead = true;
                st.mask &= ~sync::kHyperAssistAngular;
                GLOGW("engine: hyper assist: SET_ENTITY_ANGULAR_VELOCITY cannot be called: the yaw cap and the anti-roll damping are OFF");
            }
        }
    }

    void HyperLogWindow(HyperState& st, const sync::HyperProfile& p, double now)
    {
        if (st.logMs <= 0.0)
        {
            st.logMs = now;
            return;
        }
        if (now - st.logMs < 10000.0)
            return;
        if (st.frames > 0)
            GLOGI("engine: hyper assist (%s, vehicle %d), last %.0f s: %u frames (%u on the ground, brake pedal down in %u), peak speed %.0f km/h of %.0f; "
                  "applied: downforce %u frames (mean %.1f m/s^2), lateral grip %u (mean %.1f), brake assist %u (mean %.1f), yaw cap %u (cut mean %.2f / max %.2f rad/s, %s); "
                  "writes: velocity %u, angular %u; failures: reads %u, velocity writes %u, angular writes %u; read-back: velocity ok %u / bad %u, "
                  "angular ok %u / bad %u; health top-ups %u; mask 0x%x%s%s", sync::HyperProfileName(p), st.veh, (now - st.logMs) / 1000.0, st.frames, st.grounded,
                  st.brakeDown, (double)st.peakMs * 3.6, (double)p.targetKmh, st.downN, st.downN ? st.downSum / st.downN : 0.0, st.gripN,
                  st.gripN ? st.gripSum / st.gripN : 0.0, st.brakeN, st.brakeN ? st.brakeSum / st.brakeN : 0.0, st.yawN,
                  st.yawN ? st.yawCutSum / st.yawN : 0.0, st.yawCutMax, g_hyperYaw0430.load() ? "0.4.30 spin guard" : "0.4.29 cap, hyper_yaw=0", st.velN, st.angN,
                  st.readFail, st.velFail, st.angFail, st.velOk, st.velBad, st.angOk, st.angBad, st.healthFixed, st.mask,
                  st.velDead ? " (velocity write dropped)" : "", st.angDead ? " (angular write dropped)" : "");
        st.logMs = now;
        st.frames = st.grounded = st.brakeDown = st.downN = st.gripN = st.brakeN = st.yawN = st.velN = st.angN = 0;
        st.readFail = st.velFail = st.angFail = st.healthFixed = 0;
        st.downSum = st.gripSum = st.brakeSum = st.yawCutSum = st.yawCutMax = 0;
        st.peakMs = 0;
    }

    // The Tab menu's car browser (overlay "Cars" page): every car of every mounted pack, pushed only when the pack list
    // or the loading state changed (a 1000-car copy is not a per-frame cost). Script tick.
    void PushCarList(game::content::Status packs)
    {
        static uint32_t s_gen = ~0u;
        static int s_status = -1;
        const uint32_t gen = game::content::VehiclesGeneration();
        if (gen == s_gen && (int)packs == s_status)
            return;
        s_gen = gen;
        s_status = (int)packs;
        const std::vector<game::content::PackVehicle> list = game::content::Vehicles();
        std::vector<game::Overlay::CarEntry> out(std::min<std::size_t>(list.size(), (std::size_t)game::Overlay::kMaxCars));
        auto put = [](char* dst, std::size_t cap, const std::string& s) {
            const std::size_t n = std::min(s.size(), cap - 1);
            std::memcpy(dst, s.data(), n);
            dst[n] = 0;
        };
        for (std::size_t i = 0; i < out.size(); ++i) {
            const game::content::PackVehicle& v = list[i];
            put(out[i].model, sizeof(out[i].model), v.model);
            put(out[i].name, sizeof(out[i].name), v.name.empty() ? v.model : v.name);   // BuildCarList's name (pack_meta.h)
            put(out[i].vclass, sizeof(out[i].vclass), game::content::CarClassName(v.vclass));
            put(out[i].pack, sizeof(out[i].pack), v.pack);
            out[i].ready = v.ready;
        }
        game::Overlay::SetCars(out.data(), (int)out.size(), packs == game::content::Status::Pending);
    }

    void TickHyperTune(int32_t lp)
    {
        static uint32_t hashes[sync::kHyperProfileCount] = {};
        static bool hashed = false;
        static bool cfgRead = false, tuneOn = true;
        static uint32_t assistMask = sync::kHyperAssistAll;
        static int32_t tuned[8] = {};                  // the vehicles whose top speed is already raised (a ring: swapping between hyper cars must not re-apply it)
        static size_t tunedNext = 0;
        static HyperState st;
        if (!hashed)
        {
            for (size_t i = 0; i < sync::kHyperProfileCount; ++i)
                hashes[i] = sync::kHyperProfiles[i].model ? g_world->HashKey(sync::kHyperProfiles[i].model) : 0u;   // 0 = a pack-wide row
            hashed = true;
        }
        const int32_t veh = lp ? g_world->GetVehiclePedIsIn(lp) : 0;
        if (!veh || g_world->GetPedInVehicleSeat(veh, -1) != lp)
        {
            st.lastMs = 0;                             // the next drive starts without a frame length
            return;                                    // only the driver tunes (passengers see the owner's result)
        }
        const uint32_t model = g_world->GetEntityModel(veh);
        const sync::HyperProfile* prof = nullptr;
        for (size_t i = 0; i < sync::kHyperProfileCount; ++i)
            if (hashes[i] && hashes[i] == model) prof = &sync::kHyperProfiles[i];
        if (!prof)                                     // a model from a content pack with a pack-wide row (the NG packs' names come from the game)
            prof = sync::HyperProfileForPack(game::content::PackOfModel(model));
        if (!prof)
        {
            st.lastMs = 0;
            return;
        }
        if (!cfgRead)
        {
            cfgRead = true;
            tuneOn = reg::Get(L"hyper_tune") != L"0";
            const std::wstring maskReg = reg::Get(L"hyper_assist");
            std::string narrow;
            for (wchar_t ch : maskReg)
                narrow.push_back(ch < 128 ? (char)ch : '?');
            assistMask = sync::ParseHyperAssistMask(narrow.c_str());
            g_hyperYaw0430.store(reg::Get(L"hyper_yaw") != L"0");
            GLOGI("engine: hyper tune switches: hyper_tune=%s, hyper_assist mask 0x%x (hyper_tune=0 turns the tune off; hyper_assist=<bits: 1 downforce, "
                  "2 lateral grip, 4 brake assist, 8 yaw cap, 16 anti-roll; 0 = none> turns the handling assists off), yaw cap %s", tuneOn ? "on" : "OFF",
                  assistMask, g_hyperYaw0430.load() ? "0.4.30 spin guard (hyper: 50 m/s^2 from 45 m/s, street: 32 from 35; hyper_yaw=0 = 0.4.29)"
                                                    : "0.4.29 (30 / 18.4 m/s^2 from 20 m/s, hyper_yaw=0)");
        }
        if (!tuneOn)
            return;
        // 0.4.31 the acceleration curve (sync::AccelPowerAt): the multiplier falls with the car's speed; accel_curve=0 = the flat 0.4.30 power
        static int accelCurve = -1;
        if (accelCurve < 0)
        {
            accelCurve = reg::Get(L"accel_curve") == L"0" ? 0 : 1;
            const float tMs = sync::HyperTargetMs(*prof);
            GLOGI("engine: acceleration curve (0.4.31) %s: %s power x%.2f flat -> x%.2f to 60 km/h, x%.2f at 100, x%.2f at 200, x%.2f at 300, "
                  "x%.2f at %.0f km/h (accel_curve=0 = flat)", accelCurve ? "ON" : "off", sync::HyperProfileName(*prof), prof->power,
                  sync::AccelPowerAt(prof->power, tMs, 0.0f), sync::AccelPowerAt(prof->power, tMs, 100.0f / 3.6f),
                  sync::AccelPowerAt(prof->power, tMs, 200.0f / 3.6f), sync::AccelPowerAt(prof->power, tMs, 300.0f / 3.6f),
                  sync::AccelPowerAt(prof->power, tMs, tMs), prof->targetKmh);
        }
        float power = prof->power;
        if (accelCurve)
        {
            float vx = 0, vy = 0, vz = 0;
            if (g_world->GetEntityVelocity(veh, vx, vy, vz))   // a failed read keeps the flat power (never a launch boost on no information)
                power = sync::AccelPowerAt(prof->power, sync::HyperTargetMs(*prof), std::sqrt(vx * vx + vy * vy + vz * vz));
        }
        g_world->SetVehicleCheatPowerIncrease(veh, power);         // the game clears it every frame
        g_shk.power = power;                                       // 0.4.22: the shake line names what is written to the car every tick
        if (st.veh != veh)
        {
            st = HyperState();                         // a new car: its own window, read-back record and mask
            st.veh = veh;
            st.mask = prof->assist.Any() ? assistMask : 0;
        }
        bool isTuned = false;
        for (int32_t t : tuned)
            if (t == veh) isTuned = true;
        if (!isTuned)
        {
            const float est = g_world->GetVehicleEstimatedMaxSpeed(veh);
            const float pct = sync::HyperTopSpeedPercent(est, sync::HyperTargetMs(*prof));
            const bool topOk = g_world->ModifyVehicleTopSpeed(veh, pct);
            const bool capOk = g_world->SetVehicleMaxSpeed(veh, sync::HyperTargetMs(*prof));
            tuned[tunedNext] = veh;
            tunedNext = (tunedNext + 1) % (sizeof tuned / sizeof tuned[0]);
            GLOGI("engine: hyper tune on vehicle %d (model 0x%08x): stock estimate %.1f m/s, top speed +%.0f%% (%s), "
                  "cap %.0f km/h (%s), power x%.1f every frame", veh, model, est, pct, topOk ? "set" : "NOT callable",
                  prof->targetKmh, capOk ? "set" : "NOT callable", prof->power);
            if (prof->tough || prof->assist.Any())
            {
                const HyperTough t = prof->tough ? HyperApplyTough(veh) : HyperTough();
                st.flagsMs = NowMs();                  // applied just now: the 2 s re-assert starts from here
                const sync::HyperAssist& a = prof->assist;
                GLOGI("engine: hyper tune (%s) vehicle %d handling: toughness %s [strong body %s, strong axles %s, tyres cannot burst %s, wheels cannot "
                      "break %s, engine cannot degrade %s, tank damage off %s; health back to 1000 when it reads lower]; assists mask 0x%x of "
                      "downforce %.1f m/s^2 at %.0f km/h (cap %.1f), lateral grip rate %.1f /s up to %.1f m/s^2, brake assist +%.1f m/s^2, yaw cap "
                      "%.1f m/s^2 / speed from %.0f m/s, anti-roll %.1f /s (grip, brake and anti-roll fade in over %.0f..%.0f m/s)", sync::HyperProfileName(*prof), veh,
                      prof->tough ? "ON" : "off", t.strong ? "set" : "NOT callable", t.axles ? "set" : "NOT callable", t.tyres ? "set" : "NOT callable",
                      t.wheels ? "set" : "NOT callable", t.degrade ? "set" : "NOT callable", t.tank ? "set" : "NOT callable", st.mask,
                      (double)a.downAtRefMs2, (double)a.downRefMs * 3.6, (double)a.downCapMs2, (double)a.gripRate, (double)a.gripCapMs2,
                      (double)a.brakeMs2, (double)a.yawLatMs2, (double)a.yawFromMs, (double)a.tiltRate, (double)a.rampFromMs, (double)a.rampToMs);
            }
        }
        if (!prof->tough && !prof->assist.Any())
            return;                                    // the Jesko: power and top speed only
        const double now = NowMs();
        if (prof->tough)
        {
            if (now - st.flagsMs >= 2000.0)            // the game's own damage-guard code resets some of these when a car changes hands
            {
                st.flagsMs = now;
                HyperApplyTough(veh);
            }
            if (now - st.healthMs >= 250.0)
            {
                st.healthMs = now;
                float h = 0;
                if (g_world->GetVehicleBodyHealth(veh, h) && sync::HyperHealthLow(h))
                    st.healthFixed += g_world->SetVehicleBodyHealth(veh, sync::kHyperHealthFull) ? 1u : 0u;
                if (g_world->GetVehicleEngineHealth(veh, h) && sync::HyperHealthLow(h))
                    st.healthFixed += g_world->SetVehicleEngineHealth(veh, sync::kHyperHealthFull) ? 1u : 0u;
                if (g_world->GetVehiclePetrolTankHealth(veh, h) && sync::HyperHealthLow(h))
                    st.healthFixed += g_world->SetVehiclePetrolTankHealth(veh, sync::kHyperHealthFull) ? 1u : 0u;
            }
        }
        if (prof->assist.Any())
            HyperAssistFrame(st, *prof, veh, now);
        HyperLogWindow(st, *prof, now);
    }

    // 0.4.22 (libs/sync/vehicle_shake.h): the shake meter of the car the local player DRIVES, any car (tuned or not): its pitch, roll and
    // vertical speed at the script tick, as second differences (a buzz at tick rate reads at full gain, a 1-2 Hz bounce at none). Reads only:
    // GET_ENTITY_VELOCITY and GET_ENTITY_QUATERNION. The 10 s line is "engine: vehicle shake (0.4.22) ... OUR car". Script tick.
    void TickShakeMeter(int32_t lp, double nowMs)
    {
        const int32_t veh = lp ? g_world->GetVehiclePedIsIn(lp) : 0;
        float vx = 0, vy = 0, vz = 0, qx = 0, qy = 0, qz = 0, qw = 1;
        if (!veh || g_world->GetPedInVehicleSeat(veh, -1) != lp || !g_world->GetEntityVelocity(veh, vx, vy, vz) ||
            !g_world->GetEntityQuaternion(veh, qx, qy, qz, qw))
        {
            g_localShake.Restart();
            if (!veh)
                g_shk.localVeh = 0;
            return;
        }
        if (g_shk.localVeh != veh)
        {
            g_shk.localVeh = veh;
            g_shk.localModel = g_world->GetEntityModel(veh);
            g_localShake.Restart();
        }
        g_localShake.Add(nowMs, { vx, vy, vz }, { qx, qy, qz, qw });
    }

    // W10 (docs/sync-specs/live-0.4.2-fixes.md, libs/sync/nametag.h has every rule): the name above each remote ped's head.
    // Script tick, after TickPeers; natives outside g_peersMtx (this works on the game thread's own g_peers). Per peer: the pose
    // distance picks the nearest candidates (no native), then SKEL_Head + 0.35 m, GET_SCREEN_COORD_FROM_WORLD_COORD, and, for a
    // point on screen, HAS_ENTITY_CLEAR_LOS_TO_ENTITY (local ped -> puppet) at most 5 Hz per peer (the answer is cached, see
    // LosCache). The list goes to Overlay::SetNameTags (copied under the overlay's data lock); the overlay only draws it.
    // Hidden as a whole while the server session is not up, the local ped is not alive, or the pause menu is open.
    void TickNameTags(int32_t lp, double nowMs)
    {
        static const sync::NameTagConfig cfg;
        static sync::NameTagStats st;
        static std::vector<sync::TagCandidate> cand;
        static std::vector<sync::TagItem> items;
        static game::Overlay::NameTag pushOut[game::Overlay::kMaxNameTags];
        static double lastMs = 0, lastLogMs = 0;
        static bool pushed = false, loggedPath = false;
        constexpr int kSkelHead = 31086;          // SKEL_Head
        constexpr float kPoseSlackM = 4.0f;       // the pose is the body origin, the head anchor is up to ~2 m from it

        const double dtMs = lastMs > 0 ? nowMs - lastMs : 16.0;
        lastMs = nowMs;
        const bool hide = !g_nameTags.load() || !lp || !join::IsConnected() || g_life != Life::Alive || g_pauseMenuActive ||
                          g_peers.empty();
        float cx = 0, cy = 0, cz = 0;
        if (hide || !g_world->GetFinalRenderedCamCoord(cx, cy, cz))
        {
            if (!hide)
                ++st.nativeFailed;
            for (auto& kv : g_peers)
                kv.second.nameTag.Reset();
            if (pushed)
            {
                game::Overlay::SetNameTags(nullptr, 0);
                pushed = false;
            }
            return;
        }
        if (!loggedPath)
        {
            loggedPath = true;
            GLOGI("engine: name tags run: head bone %d + %.2f m, GET_SCREEN_COORD_FROM_WORLD_COORD, HAS_ENTITY_CLEAR_LOS_TO_ENTITY "
                  "(trace %d) every %.0f ms per peer, fade %.0f-%.0f m, at most %zu tags; hidden while the pause menu is open or the "
                  "local player is not alive",
                  kSkelHead, cfg.headLiftM, g_nameTagTrace.load(), cfg.losIntervalMs, cfg.fadeStartM, cfg.maxRangeM, cfg.maxTags);
        }

        // 1. Candidates by pose distance: no native.
        cand.clear();
        for (const auto& kv : g_peers)
        {
            const Peer& pe = kv.second;
            if (!(pe.ped && pe.poseOk && pe.lastDriveMs > 0 && !pe.gone && !pe.despawn))
                continue;
            const float dx = pe.pose.k.x - cx, dy = pe.pose.k.y - cy, dz = pe.pose.k.z - cz;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (sync::NameTagTrack::Worth(true, pe.deadShown, d - kPoseSlackM, cfg))
                cand.push_back({ kv.first, d });
        }
        sync::KeepNearestCandidates(cand, cfg.maxCandidates);
        st.candidates += (uint32_t)cand.size();

        // 2. Every peer steps once: the candidates with what the natives say, the rest with nothing (their tag fades out).
        items.clear();
        for (auto& kv : g_peers)
        {
            Peer& pe = kv.second;
            sync::TagFrame f;
            f.nowMs = nowMs;
            f.dtMs = dtMs;
            f.placed = pe.ped != 0 && !pe.gone && !pe.despawn;
            f.dead = pe.deadShown;
            const int id = kv.first;
            const bool isCand = std::any_of(cand.begin(), cand.end(), [id](const sync::TagCandidate& c) { return c.id == id; });
            if (isCand)
            {
                float hx = 0, hy = 0, hz = 0;
                if (!g_world->GetPedBoneCoords(pe.ped, kSkelHead, 0.0f, 0.0f, 0.0f, hx, hy, hz))
                    ++st.nativeFailed;
                else
                {
                    hz += cfg.headLiftM;
                    const float dx = hx - cx, dy = hy - cy, dz = hz - cz;
                    f.distM = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (f.distM <= cfg.maxRangeM)
                    {
                        f.projOk = g_world->GetScreenCoordFromWorldCoord(hx, hy, hz, f.sx, f.sy);
                        if (!f.projOk)
                            ++st.projFailed;
                        else if (sync::ProjectionUsable(true, f.sx, f.sy, cfg))
                        {
                            if (pe.nameTag.los.Due(nowMs, cfg))
                            {
                                const int r = g_world->HasEntityClearLosToEntity(lp, pe.ped, g_nameTagTrace.load());
                                ++st.losChecks;
                                if (r < 0)
                                    ++st.losFailed;           // fail closed: recorded as blocked
                                else if (r == 0)
                                    ++st.losBlocked;
                                pe.nameTag.los.Record(nowMs, r == 1);
                            }
                            else
                                ++st.losHits;
                        }
                    }
                }
            }
            const sync::TagOut o = pe.nameTag.Step(f, cfg);
            if (o.draw)
            {
                sync::TagItem it;
                it.id = id;
                it.distM = o.distM;
                it.out = o;
                it.text = sync::FormatTag(id, pe.name.c_str(), cfg);
                items.push_back(std::move(it));
            }
        }
        sync::KeepNearest(items, cfg.maxTags);

        // 3. Hand the list over.
        int n = 0;
        for (const sync::TagItem& it : items)
        {
            if (n >= game::Overlay::kMaxNameTags)
                break;
            game::Overlay::NameTag& t = pushOut[n++];
            t = game::Overlay::NameTag();
            t.x = it.out.x;
            t.y = it.out.y;
            t.alpha = it.out.alpha;
            t.scale = it.out.scale;
            std::snprintf(t.idText, sizeof(t.idText), "%s", it.text.id.c_str());
            std::snprintf(t.name, sizeof(t.name), "%s", it.text.name.c_str());
        }
        if (n > 0 || pushed)
        {
            game::Overlay::SetNameTags(pushOut, n);
            pushed = n > 0;
        }
        ++st.frames;
        st.drawnSum += (uint32_t)n;
        st.drawnMax = std::max(st.drawnMax, (uint32_t)n);

        if (nowMs - lastLogMs >= 10000.0)
        {
            if (lastLogMs > 0 && (st.candidates || st.drawnMax))
                GLOGI("engine: name tags (10 s): %u frames, %.1f drawn avg (max %u), %u candidates, LOS %u asked / %u reused / %u blocked / "
                      "%u failed, %u off screen, %u native failures",
                      st.frames, st.frames ? (double)st.drawnSum / st.frames : 0.0, st.drawnMax, st.candidates, st.losChecks, st.losHits,
                      st.losBlocked, st.losFailed, st.projFailed, st.nativeFailed);
            lastLogMs = nowMs;
            st.Reset();
        }
    }

    void TickAiDamage(double nowMs)
    {
        static double atMs = -1e9;
        static int failed = 0;
        if (g_aiDamageWanted.load(std::memory_order_relaxed))
        {
            if (failed >= 5 || nowMs - atMs < (g_aiDamageApplied ? 5000.0 : 1000.0))
                return;
            atMs = nowMs;
            // 0c: the MELEE modifier only with ai_melee_mod=1. A puppet plays a swing animation and starts no melee task, so
            // nothing of its own can hurt the local player, and the 0.0 modifier zeroed our own fists on the puppet.
            const bool meleeWanted = g_aiMeleeMod.load(std::memory_order_relaxed);
            const bool weaponOk = g_world->SetAiWeaponDamageModifier(0.0f);
            const bool meleeOk = !meleeWanted || g_world->SetAiMeleeWeaponDamageModifier(0.0f);
            if (weaponOk && meleeOk)
            {
                if (!g_aiDamageApplied)
                {
                    g_aiDamageApplied = true;
                    failed = 0;
                    ++g_v4.aiModSets;
                    GLOGI("engine: AI weapon damage modifier set to 0.0: a puppet's real shot hurts nobody here (damage is the "
                          "server's); the clear-line check of real peer shots is lifted. AI melee damage modifier: %s",
                          meleeWanted ? "also 0.0 (ai_melee_mod=1)" : "NOT set (puppets only play a swing animation; our fists must hurt the puppet's pool)");
                }
                g_aiMeleeApplied = meleeWanted;
            }
            else if (++failed >= 5)
                GLOGW("engine: SET_AI_WEAPON_DAMAGE_MODIFIER%s could not be called (weapon %s, melee %s): real peer shots keep "
                      "the clear-line check", meleeWanted ? " / SET_AI_MELEE_WEAPON_DAMAGE_MODIFIER" : "", weaponOk ? "ok" : "no",
                      meleeOk ? "ok" : "no");
            return;
        }
        failed = 0;
        if (g_aiDamageApplied)
        {
            const bool weaponOk = g_world->ResetAiWeaponDamageModifier();
            const bool meleeOk = !g_aiMeleeApplied || g_world->ResetAiMeleeWeaponDamageModifier();   // only if we set it
            g_aiDamageApplied = false;
            GLOGI("engine: session ended: AI weapon damage modifier reset (%s), melee modifier %s", weaponOk ? "ok" : "NOT CALLABLE",
                  !g_aiMeleeApplied ? "was never set" : meleeOk ? "reset" : "NOT CALLABLE");
            g_aiMeleeApplied = false;
        }
    }

    // A session began or ended: every v4 unit starts over (script tick).
    void ResetV4Units()
    {
        {
            std::lock_guard<std::mutex> lk(g_realPedMtx);
            g_haveRealPed = false;                    // the alt-tab fallback has no state of the old session to resend
        }
        g_link.Reset();
        g_angJudge.Configure(g_shakeFix.load() && !g_shakeWorldPinned.load());   // 0.4.22: a new session starts on the body frame again
        g_localShake.Restart();
        g_vitals.Reset(g_playerHealth.load(), 0);     // the server playerHealth, no armour (Welcome)
        g_health.Reset();
        if (!g_v4ResetEnded.load())
            ApplyServerMaxHealth(g_world->LocalPlayer(), "session start", true);
        else
            g_vitals.SetScale(sync::HealthScale());   // R3 T1: a session ended: no stale SCALED mode (and no health written to the offline ped)
        g_respawnGate.OnAlive();
        g_hits.Clear();
        g_localEmote.Clear();
        SyncLocalEmoteDict();
        g_localReact.Clear();
        g_meleeGate.Reset();
        g_swingLadder.SetMode((sync::SwingMode)g_meleeAnimWanted.load(), g_meleeLoopWanted.load());   // R3 T6: a new session starts the ladder from the registry mode again (R5: and its loop switch)
        {
            // R4 T3: ... and the survey from its first attempt (only where animations are tried at all: auto / play)
            const sync::SwingMode sm = g_swingLadder.Mode();
            g_swingSurvey.Start(sm == sync::SwingMode::Auto || sm == sync::SwingMode::Play ? (uint32_t)g_meleeSurveyWanted.load() : 0u, g_meleeLoopWanted.load());
            g_swingDurationMs = SwingDurationDefault();
            g_swingNo = 0;
            g_ownSwingNo = 0;
        }
        ReleaseSwingDicts();
        g_world->RequestAnimDict(kStaggerDict);        // 0.4.30: the stagger clip is ready for the first punch
        g_haveRespawnOk = false;
        g_localSeat = -1;
        g_lastMarkedVeh = 0;
        g_noShuffleVeh = 0;
        g_entryWalk = EntryWalk();
        g_localEnter.Reset();
        g_seats.Reset();
        g_entryGuard.Reset();
        g_vitalsPrevTotal = -1;
        g_serverDeadNoticed = false;
    }

    // Our health against the server's (VitalsTracker), self-damage out as self-hits, the hit batcher
    // flushed. Before TickLife: a death seen here is reported in the same frame.
    void TickVitals(int32_t lp, int32_t lpVeh, double nowMs)
    {
        // Regeneration would be undone by the tracker every few frames: off while the server owns
        // health (re-asserted every 5 s; a revive may restore it), back to 1 offline.
        static bool rechargeOff = false;
        static double rechargeAtMs = -1e9;
        const bool server = ServerMode();
        if (server != rechargeOff || (server && nowMs - rechargeAtMs >= 5000.0))
        {
            const bool rechargeOk = g_world->SetLocalHealthRecharge(server ? 0.0f : 1.0f);
            static bool rechargeLogged = false;
            if (!rechargeOk || !rechargeLogged)    // T6: the first call and any failure (a failure lets GTA regenerate to 50 %)
            {
                rechargeLogged = true;
                if (rechargeOk)
                    GLOGI("engine: health: SET_PLAYER_HEALTH_RECHARGE_MULTIPLIER(%.1f) -> ok", server ? 0.0 : 1.0);
                else
                    GLOGW("engine: health: SET_PLAYER_HEALTH_RECHARGE_MULTIPLIER(%.1f) -> FAILED (GTA may regenerate to 50 %%)",
                          server ? 0.0 : 1.0);
            }
            rechargeOff = server;
            rechargeAtMs = nowMs;
        }
        // R3 T1: the probe's one-frame and one-second read-backs; the tracker waits the one tick the ped still holds the probe's value.
        ++g_healthTick;
        bool healthHold = false;
        if (lp)
        {
            HealthPort hport{lp};
            healthHold = g_health.Pre(hport, lp, g_vitals, g_healthTick, nowMs);
            DrainHealthLog();
        }
        // Alive, and Dying until the server has us dead: a lethal self-hit the server never answered is
        // re-sent after the tracker's ack timeout only if the tracker keeps ticking while we lie dead.
        if (lp && !healthHold && sync::VitalsTickDue(server, g_life == Life::Alive, g_life == Life::Dying, g_vitals.ServerDead()))
        {
            sync::VitalsObs ob;
            ob.nowMs = nowMs;
            ob.health = g_world->GetEntityHealth(lp);
            ob.maxHealth = g_world->GetEntityMaxHealth(lp);
            ob.ped = lp;                               // T6: a changed handle is a new ped, not a loss
            ob.armour = g_world->GetPedArmour(lp);
            ob.dead = g_world->IsPedDeadOrDying(lp);
            // A loss a remote puppet caused (a ram, a thrown car, its car's wheels) is reported by the
            // puppet's owner as a Hit; counting it here as environmental damage too would take it twice.
            // Scanned only when the ped's total dropped (or it is dead); the "last damage" marker is
            // cleared every tick so a stale touch cannot blame a later fall on a puppet.
            const bool readable = (ob.dead || ob.health > 0) && ob.armour >= 0;
            const int total = ob.dead ? 0 : (ob.health > 100 ? ob.health - 100 : 0) + (ob.armour > 0 ? ob.armour : 0);
            if (readable && (ob.dead || (g_vitalsPrevTotal >= 0 && total < g_vitalsPrevTotal)))
            {
                static std::vector<int32_t> handles;   // script tick only
                handles.clear();
                {
                    // The game thread's own maps: no lock.
                    for (const auto& kv : g_peers)
                        if (!kv.second.gone && kv.second.ped)
                            handles.push_back(kv.second.ped);
                    for (const auto& kv : g_vehs)
                        if (kv.second.handle && !kv.second.removed)
                            handles.push_back(kv.second.handle);
                }
                const int32_t who = sync::FindPuppetDamager(handles.data(), handles.size(), lpVeh,
                                                            [&](int32_t h) { return g_world->DamagedBy(lp, h); });
                if (who)
                {
                    ob.puppetDamage = true;
                    for (const auto& kv : g_peers)     // W8 item 6: a puppet PED (its gun or fists), not a car
                        if (kv.second.ped == who)
                        {
                            ++g_v4.puppetFireLoss;
                            break;
                        }
                    static double puppetLoggedMs = -1e9;
                    if (nowMs - puppetLoggedMs >= 1000.0)
                    {
                        puppetLoggedMs = nowMs;
                        GLOGI("engine: combat: loss %d -> %d (health+armour above the death line) was caused by remote "
                              "entity %d: its owner's Hit reports it, not counted as a self-hit",
                              g_vitalsPrevTotal, total, who);
                    }
                }
            }
            if (readable)
                g_vitalsPrevTotal = total;
            g_world->ClearLastDamage(lp);
            const sync::VitalsOut v = g_vitals.Tick(ob);
            if (v.probe)
            {
                // R3 T1: measure this ped now (a handle nobody probed, or a mismatch GTA kept); the tracker said nothing else this tick.
                g_vitalsPrevTotal = -1;
                ApplyServerMaxHealth(lp, v.probeWhy);
            }
            else if (v.selfHitCount)
            {
                static uint32_t fallHash = 0;
                if (!fallHash)
                    fallHash = g_world->HashKey("WEAPON_FALL");
                const bool fell = g_world->IsPedFalling(lp) || g_world->IsPedRagdoll(lp);
                for (int i = 0; i < v.selfHitCount; ++i)
                    if (v.selfHits[i] >= 50 && !fell && !ob.puppetDamage)
                    {
                        // T6: a big loss with no fall and no puppet behind it is what a mismatched ped looked like (a swapped
                        // ped's default 200 under a max of 300 was a self-hit of 100): say so, with the numbers that explain it.
                        static double warnedMs = -1e9;
                        if (nowMs - warnedMs >= 2000.0)
                        {
                            warnedMs = nowMs;
                            GLOGW("engine: health: self-hit of %d with no fall and no puppet cause: ped health %d / max %d "
                                  "(wanted max %d), server health %u, playerHealth %u, tracker pending %d",
                                  v.selfHits[i], ob.health, ob.maxHealth, sync::PedMaxHealth(g_playerHealth.load()),
                                  (unsigned)g_vitals.Health(), (unsigned)g_playerHealth.load(), g_vitals.Pending());
                        }
                    }
                for (int i = 0; i < v.selfHitCount; ++i)
                {
                    sync::BatchedHit h;
                    h.target = join::MyId();
                    h.kind = (uint8_t)proto::HitKind::Player;
                    h.weapon = fell ? fallHash : 0;
                    h.damage = (uint16_t)v.selfHits[i];
                    h.shotSeq = (uint8_t)(g_shotSeq & 15);
                    g_hits.Add(h, nowMs);
                }
            }
            if (v.apply && !ob.dead)
            {
                g_world->SetEntityHealth(lp, v.setHealth);
                g_world->SetPedArmour(lp, v.setArmour);
                g_vitalsPrevTotal = -1;                // our own write is no loss: the next tick re-seeds
            }
            if (v.kill)
            {
                g_world->SetEntityHealth(lp, 0);
                GLOGI("engine: combat: the server has us dead and our ped lived: killed it");
            }
        }
        else
            g_vitalsPrevTotal = -1;
        sync::BatchedHit out[16];
        const size_t n = server ? g_hits.Flush(nowMs, out, 16) : 0;
        if (!server)
            g_hits.Clear();
        for (size_t i = 0; i < n; ++i)
        {
            proto::Hit h;
            h.kind = (proto::HitKind)out[i].kind;
            h.targetId = out[i].target;
            h.weapon = out[i].weapon;
            h.damage = out[i].damage;
            h.bone = out[i].bone;
            h.shotSeq = out[i].shotSeq;
            if (join::SendHit(h))
                ++g_v4.hitsSent;
            else
                ++g_v4.hitsRefusedLocal;
        }
    }

    // Our own emote: stream, play, send; ended by the player moving off it, /stopanim, a death or a
    // vehicle.
    void TickLocalEmote(int32_t lp, bool inVehicle, bool dead, double nowMs)
    {
        SyncLocalEmoteDict();                          // also after a Clear() elsewhere (a death, a new session)
        if (!g_localEmote.Active() || !lp)
            return;
        const bool wasIssued = g_localEmote.Issued();
        const bool wasLooping = g_localEmote.Looping();
        const bool anim = g_localEmote.Kind() == sync::EmoteKind::Anim;
        const sync::EmoteStats before = g_localEmote.Stats();
        sync::EmoteIn in;
        in.nowMs = nowMs;
        if (g_localEmote.WaitingForDict())
            in.dictLoaded = g_world->HasAnimDictLoaded(g_localAnim.dict.c_str());
        if (wasIssued)
            in.playing = anim ? g_world->IsEntityPlayingAnim(lp, g_localAnim.dict.c_str(), g_localAnim.name.c_str())
                              : g_world->IsPedUsingAnyScenario(lp);
        in.dead = dead;
        in.inVehicle = inVehicle;
        const sync::EmoteOut o = g_localEmote.Tick(in);
        sync::AnimKindStats& dk = g_animDiag.Of(sync::AnimKind::LocalEmote);
        if (o.requestDict)
            g_world->RequestAnimDict(g_localAnim.dict.c_str());
        if (o.issueAnim)
        {
            const proto::Anim& a = g_localAnim;
            if (o.replacedTask)
                g_world->ClearPedTasks(lp);            // 0e: an earlier emote of ours is still on the ped (a restart): blended out first
            const bool called = g_world->TaskPlayAnim(lp, a.dict.c_str(), a.name.c_str(), a.blendIn, a.blendOut,
                                                      sync::EmoteArbiter::GtaDuration(a.durationMs),
                                                      (int)sync::EmoteArbiter::GtaFlags(a.durationMs, a.flags), EmotePhase(a));   // 0.4.30: the start phase (the wire rate has no setter here)
            const bool sent = join::SendAnim(a);
            if (called)
            {
                char tg[200];
                std::snprintf(tg, sizeof(tg), "local emote %s/%s, TASK_PLAY_ANIM flags %u, duration %d ms", a.dict.c_str(), a.name.c_str(),
                              (unsigned)sync::EmoteArbiter::GtaFlags(a.durationMs, a.flags), (int)sync::EmoteArbiter::GtaDuration(a.durationMs));
                BeginTrace(TraceProbe::Anim, lp, tg, a.dict.c_str(), a.name.c_str());
            }
            ++(called ? dk.issued : dk.failed);
            NoteDictReady(sync::AnimKind::LocalEmote, o.waitedMs);
            GLOGI("engine: anims: playing %s/%s%s; DOES_ANIM_DICT_EXIST yes, HAS_ANIM_DICT_LOADED after %.0f ms, TASK_PLAY_ANIM %s "
                  "(flags %u, duration %d ms); IS_ENTITY_PLAYING_ANIM is checked from the next frame", a.dict.c_str(), a.name.c_str(),
                  sent ? ", sent" : " (not sent: offline)", o.waitedMs, called ? "called" : "could NOT be called",
                  (unsigned)sync::EmoteArbiter::GtaFlags(a.durationMs, a.flags), (int)sync::EmoteArbiter::GtaDuration(a.durationMs));
        }
        if (o.issueScenario)
        {
            g_world->TaskStartScenarioInPlace(lp, g_localScenario.c_str());
            const bool sent = join::SendScenario(g_localScenario);
            ++dk.issued;
            GLOGI("engine: anims: scenario %s%s", g_localScenario.c_str(), sent ? ", sent" : " (not sent: offline)");
        }
        if (o.clearTasks)
            g_world->ClearPedTasks(lp);
        if (o.confirmed)
        {
            ++dk.shown;
            GLOGI("engine: anims: our emote %s is SEEN playing (IS_ENTITY_PLAYING_ANIM true) %.0f ms after the task",
                  anim ? (g_localAnim.dict + "/" + g_localAnim.name).c_str() : g_localScenario.c_str(), o.confirmMs);
        }
        if (o.reissued)
            ++dk.reissued;
        if (o.neverPlayed)
        {
            // 0e: the clip never played on our own ped: a wrong clip name (TASK_PLAY_ANIM reports nothing) or a task the game dropped.
            ++dk.unseen;
            game::Overlay::PushSystemLine(anim ? "That animation did not play: check the clip name (/e <dict> <clip>)."
                                               : "That scenario did not start.");
            GLOGW("engine: anims: %s was issued but IS_ENTITY_PLAYING_ANIM never saw it play: the %s is probably wrong",
                  anim ? (g_localAnim.dict + "/" + g_localAnim.name).c_str() : g_localScenario.c_str(),
                  anim ? "clip name" : "scenario name");
        }
        if (o.gaveUp)
        {
            ++g_v4.emoteDictTimeouts;
            ++dk.dictTimeout;
            game::Overlay::PushSystemLine("That animation did not load.");
            GLOGW("engine: anims: %s did not load in 3 s", g_localAnim.dict.c_str());
        }
        if (o.ended)
        {
            ++dk.ended[(size_t)o.endReason];
            if (o.endReason == sync::EmoteEnd::Dead || o.endReason == sync::EmoteEnd::Vehicle)
                ++dk.cancelled;
        }
        if (o.ended && wasIssued)
        {
            // The others stop theirs too: always after a /stopanim or a cut-short one-shot, and for a
            // loop (the server remembers loops for newcomers).
            const bool stopped = g_localEmote.Stats().stopped != before.stopped;
            const bool finished = g_localEmote.Stats().finished != before.finished;
            if (stopped || wasLooping || !finished)
                join::SendStopAnim();
            if (g_localEmote.Stats().notPlaying != before.notPlaying)
                ++g_v4.emoteNotPlaying;
            GLOGI("engine: anims: our emote ended (%s)", stopped ? "/stopanim" : finished ? "finished"
                  : (dead ? "death" : inVehicle ? "vehicle" : "no longer playing"));
        }
        else if (o.ended && g_localEmote.Stats().stopped != before.stopped)
            join::SendStopAnim();                      // stopped before it ever played: clear the server's copy
        if (o.ended)
            SyncLocalEmoteDict();                      // ended, given up or cut short: free the dictionary
    }

    // Sync D, every 10 s with the vehicles lines: what the remote cars' drive did (position / heading / sideways error against the
    // sender's sample AT THE INSTANT IT CLAIMS, so latency is not in it; snaps; which interpolator mode the frames were in) and the
    // occupant counters the W9 line cannot give (door walks followed vs tasked, a warped puppet found out of its seat and where,
    // exits by flag, controller hand-overs kept vs snapped). Script tick. `last` is the previous window's counters.
    void ReportSyncD(const SyncDStats& last, const uint32_t lastHand[2])
    {
        const SyncDStats& c = g_syncD;
        sync::DriveErrWindow::Window agg;
        double posSum = 0, rotSum = 0, latSum = 0;
        uint16_t worstId = 0xFFFF;
        float worstPos = -1.0f;
        uint32_t cars = 0, snapsTotal = 0, persistent = 0, underFloor = 0, blends = 0, graces = 0;
        for (auto& kv : g_vehs)
        {
            NetVeh& v = kv.second;
            const sync::VehicleDriveStats& ds = v.drive.Stats();
            const sync::DriveErrWindow::Window w = v.errWin.Take();
            {
                // 0.5.7: a copy whose owner is at rest: does it obey the yaw we command? (read-backs only; one line per car with >= 30 rest frames)
                const sync::VehRestDiag::Window r = v.restDiag.Take();
                if (r.restFrames >= 30)
                    GLOGI("engine: vehicle rest (0.5.7) id=%u, last 10 s: %u of %u drive frames with the owner at rest (< %.1f m/s); error yaw mean %.1f / max %.1f deg, "
                          "tilt mean %.1f / max %.1f deg, horizontal mean %.2f m, vertical mean %.2f m; yaw rate commanded on %u frames (mean |%.2f| rad/s) -> the "
                          "read-back heading turned at mean |%.2f| rad/s, honoured %.2f (1 obeyed, 0 ignored, < 0 the other way); read-back velocity vs the one "
                          "written the frame before: mean %.2f m/s, over 1 m/s on %u of %u; steady yaw error over %.0f deg for > %.0f ms: %u episodes, longest %.0f ms",
                          (unsigned)kv.first, r.restFrames, r.frames, sync::VehRestDiag::kRestSpeed, r.yawMeanDeg, r.yawMaxDeg, r.tiltMeanDeg, r.tiltMaxDeg,
                          r.horizMeanM, r.vertMeanM, r.cmdFrames, r.cmdMeanRad, r.achievedMeanRad, r.honoured, r.velDevMeanMs, r.velDevOver1, r.velDevN,
                          sync::VehRestDiag::kSteadyDeg, sync::VehRestDiag::kSteadyMs, r.episodes, r.longestSteadyMs);
            }
            persistent += ds.persistentSnaps - v.syncDPersistentSeen;
            underFloor += ds.underFloorSnaps - v.syncDUnderFloorSeen;
            blends += ds.blends - v.syncDBlendsSeen;
            graces += ds.contacts - v.syncDGracesSeen;
            v.syncDPersistentSeen = ds.persistentSnaps;
            v.syncDUnderFloorSeen = ds.underFloorSnaps;
            v.syncDBlendsSeen = ds.blends;
            v.syncDGracesSeen = ds.contacts;
            if (!w.frames)
                continue;
            ++cars;
            agg.frames += w.frames;
            agg.buffering += w.buffering;
            agg.interpolating += w.interpolating;
            agg.extrapolating += w.extrapolating;
            agg.holding += w.holding;
            agg.snaps += w.snaps;
            posSum += (double)w.posMeanM * w.frames;
            rotSum += (double)w.rotMeanDeg * w.frames;
            latSum += (double)w.latMeanM * w.frames;
            agg.posMaxM = std::max(agg.posMaxM, w.posMaxM);
            agg.rotMaxDeg = std::max(agg.rotMaxDeg, w.rotMaxDeg);
            agg.latMaxM = std::max(agg.latMaxM, w.latMaxM);
            snapsTotal += w.snaps;
            if (w.posMaxM > worstPos)
            {
                worstPos = w.posMaxM;
                worstId = kv.first;
            }
        }
        if (agg.frames)
            GLOGI("engine: vehicle drive (D), last 10 s: %u remote cars, %u drive frames (%u interpolating, %u extrapolating, %u holding, %u "
                  "buffering); error against the sender's sample at the instant it claims: position mean %.3f / max %.2f m, heading mean %.2f / "
                  "max %.1f deg, sideways mean %.3f / max %.2f m (worst car id=%u); %u snaps (%u persistent-error, %u under-floor in the window), "
                  "%u quaternion blends, %u contact graces",
                  cars, agg.frames, agg.interpolating, agg.extrapolating, agg.holding, agg.buffering, posSum / agg.frames, (double)agg.posMaxM,
                  rotSum / agg.frames, (double)agg.rotMaxDeg, latSum / agg.frames, (double)agg.latMaxM, (unsigned)worstId, snapsTotal, persistent,
                  underFloor, blends, graces);
        const uint32_t hand0 = g_syncDHandovers[0].load(), hand1 = g_syncDHandovers[1].load();
        GLOGI("engine: occupants (D), last 10 s: door walks followed on the pose %u / TASK_ENTER_VEHICLE %u (puppet_enter=%s); a warped puppet "
              "found out of its seat (one count per retry): in another seat %u, in another car %u, in no car %u, unknown %u; exits: %u climb-out "
              "(flag 0), %u bail-out (4160, %u of them by the flag-16 fallback), %u pops; controller hand-overs kept %u / snapped %u",
              c.enterFollowed - last.enterFollowed, c.enterTasked - last.enterTasked, g_peerEnterTask.load() ? "task" : "follow",
              c.displacedOtherSeat - last.displacedOtherSeat, c.displacedOtherVeh - last.displacedOtherVeh,
              c.displacedOutside - last.displacedOutside, c.displacedUnknown - last.displacedUnknown, c.unseatLeave - last.unseatLeave,
              c.unseatBail - last.unseatBail, c.bailFallback - last.bailFallback, c.unseatClear - last.unseatClear, hand0 - lastHand[0],
              hand1 - lastHand[1]);
    }

    // T1 seats (wire v6), every 10 s with the vehicle lines: what the server's enter / exit intents did on THIS machine. One line, only when
    // something happened in the window. Grep "engine: seats (T1)". Script tick.
    //   our entries: begun, EnterIntent sent / Ok / refused (by code), the game's task issued / seen running / finished with us seated /
    //                never started / stopped without a seat / timed out, the BOUNDED fallback warps, entries aborted / cancelled by the server
    //   other players' entries (replays): Reserved notices, TASK_ENTER_VEHICLE issued (must equal the Reserved count minus the cancelled
    //                before the car was here) / started / finished by the game / never started / stopped / timed out / not callable, cancelled
    //                by the server, exit animations replayed, stale lists refused after an exit, the seat WARPS by reason (the fallback), puppets put
    //                out because the server never named them
    //   divergences: puppets put out + warped puppets found out of their seat (SyncD displaced counters, last window)
    void ReportSeats()
    {
        static T1Stats last;
        static uint32_t lastDisplaced = 0;
        const T1Stats& c = g_t1;
        const sync::SeatReplayStats p = g_seats.TakeStats();
        const sync::LocalEnterStats l = g_localEnter.TakeStats();
        const uint32_t displaced = g_syncD.displacedOtherSeat + g_syncD.displacedOtherVeh + g_syncD.displacedOutside + g_syncD.displacedUnknown;
        const uint32_t sent = c.intentsSent - last.intentsSent, ok = c.intentsOk - last.intentsOk, refused = c.intentsRefused - last.intentsRefused,
                       notices = c.noticesIn - last.noticesIn, exitsRep = c.exitsReplayed - last.exitsReplayed;
        const bool active = sent || ok || refused || notices || p.issued || p.warpNoIntent || p.Warps() || p.exits || l.begun || exitsRep || p.evicted;
        if (active)
        {
            std::string by;
            for (int i = 1; i < 16; ++i)
                if (c.refusedBy[i] != last.refusedBy[i])
                {
                    char b[40];
                    std::snprintf(b, sizeof(b), "%s%s=%u", by.empty() ? "" : " ", VehResultName((uint8_t)i), c.refusedBy[i] - last.refusedBy[i]);
                    by += b;
                }
            GLOGI("engine: seats (T1), last 10 s: OUR entries: %u begun, intents sent %u / Ok %u / refused %u [%s]; the game's task issued %u (not callable %u) / "
                  "running %u / finished with us seated %u / never started %u / stopped without a seat %u / timed out %u; FALLBACK WARPS %u; aborted %u, "
                  "cancelled by the server %u, no answer %u, cancels sent %u. OTHERS: %u notices in (%u about us), replays: Reserved %u -> task issued %u "
                  "(bike/cycle %u, not callable %u) / running %u / finished by the game %u / never started %u / stopped %u / timed out %u / cancelled %u / "
                  "dropped (car never came) %u / gave up unconfirmed %u; exit animations replayed %u (no ped %u), stale-list denials after an exit %u (per check); "
                  "seat WARPS %u (no intent %u, confirmed before the task %u, grace over %u, task ended %u, task timed out %u); DIVERGENCES: puppets put out "
                  "(seated here, never in the server's list) %u, warped puppets found out of their seat %u",
                  l.begun, l.sent, l.accepted, l.refused, by.empty() ? "none" : by.c_str(), l.issued, c.localIssueFailed - last.localIssueFailed, l.started,
                  l.finished, l.neverStarted, l.endedNoSeat, l.timedOut, l.fallbackWarps, l.aborted, l.serverCancelled, l.noAnswer, c.cancelsSent - last.cancelsSent,
                  notices, c.noticesAboutUs - last.noticesAboutUs, p.intents, p.issued, p.mounts, p.issueFailed, p.started, p.finished, p.neverStarted,
                  p.endedNoSeat, p.timeouts, p.cancelled, p.droppedNotReady, p.gaveUp, exitsRep, c.exitsNoPed - last.exitsNoPed, p.latchDenied, p.Warps(),
                  p.warpNoIntent, p.warpConfirmed, p.warpGraceOver, p.warpTaskEnded, p.warpTaskTimeout, p.evicted, displaced - lastDisplaced);
        }
        last = c;
        lastDisplaced = displaced;
    }

    // 0.4.22, every 10 s with the seats line: the F / G keys by seat class, what IS_VEHICLE_SEAT_FREE said at a pick, the READ-BACK of the seat
    // GET_PED_IN_VEHICLE_SEAT really put us in, the L key and the server's answers, and the copies' lock read back with
    // GET_VEHICLE_DOOR_LOCK_STATUS. Printed when the window had any of it or a copy is locked. Counters are of the last window. Script tick.
    void ReportDoorsSeats()
    {
        const DoorsSeatsStats& d = g_ds;
        uint32_t copies = 0, lockedCopies = 0;
        for (const auto& kv : g_vehs)
            if (kv.second.handle && !kv.second.removed && kv.second.haveState)
            {
                ++copies;
                if (kv.second.st.flags & proto::kVehLocked)
                    ++lockedCopies;
            }
        const uint32_t picks = d.pickSeat[0] + d.pickSeat[1] + d.pickSeat[2] + d.pickSeat[3];
        const bool active = d.pressF || d.pressG || picks || d.refusedLocked || d.refusedFull || d.refusedOwned || d.refusedFar || d.seatMatch ||
                            d.seatDiffers || d.seatNotSeated || d.lockPress || d.lockOk || d.lockRefused || d.keep.reissued || d.keep.stuck || lockedCopies;
        if (active)
        {
            std::string by;
            for (int i = 1; i < 16; ++i)
                if (d.lockRefusedBy[i])
                {
                    char b[40];
                    std::snprintf(b, sizeof(b), "%s%s=%u", by.empty() ? "" : " ", VehResultName((uint8_t)i), d.lockRefusedBy[i]);
                    by += b;
                }
            GLOGI("engine: doors and seats (0.4.22), last 10 s: switches rear_seats=%s locks=%s; F %u / G %u presses; picks that began an entry: driver %u, front "
                  "passenger %u, REAR %u, further back %u; no seat found: locked %u, full %u, owned %u, too far %u; IS_VEHICLE_SEAT_FREE at a pick: free %u / taken %u / "
                  "unknown %u; SEAT READ-BACK (GET_PED_IN_VEHICLE_SEAT): match %u / differs %u / never seated %u; L: %u presses (%u held back by an unanswered one), "
                  "%u sent (%u not sent), server answers Ok %u (locked %u / unlocked %u), refused %u [%s], frontend sound ok %u / not callable %u; COPIES: %u of %u locked, "
                  "LOCK READ-BACK (GET_VEHICLE_DOOR_LOCK_STATUS) %u reads (%u not callable): match %u / mismatch %u, re-issued %u, restored %u, left alone (stuck) %u",
                  g_vehRearSeats.load() ? "on" : "off", g_vehLocks.load() ? "on" : "off", d.pressF, d.pressG, d.pickSeat[0], d.pickSeat[1], d.pickSeat[2], d.pickSeat[3],
                  d.refusedLocked, d.refusedFull, d.refusedOwned, d.refusedFar, d.freeYes, d.freeNo, d.freeUnknown, d.seatMatch, d.seatDiffers, d.seatNotSeated,
                  d.lockPress, d.lockHeld, d.lockSent, d.lockNotSent, d.lockOk, d.lockOkLocked, d.lockOkUnlocked, d.lockRefused, by.empty() ? "none" : by.c_str(),
                  d.soundOk, d.soundFail, lockedCopies, copies, d.keep.reads, d.keep.readFails, d.keep.match, d.keep.mismatch, d.keep.reissued, d.keep.restored,
                  d.keep.stuck);
        }
        g_ds = DoorsSeatsStats();
    }

    // T3, every 10 s with the vehicles lines: the vehicle visual state (tyres, windows, brake lights, handbrake, steering). Counters of the
    // last window (the VisStats ones) and totals since the first sight of each car (the per-car sync::VisualSync stats): see
    // docs/api/protocol.md "T3". Script tick.
    void ReportVehVisual()
    {
        sync::VisualStats tot;
        uint32_t carsTyres = 0, tyreBits = 0, carsWindows = 0;
        for (auto& kv : g_vehs)
        {
            const sync::VisualStats& vs = kv.second.vis.Stats();
            tot.reads += vs.reads;
            tot.divergent += vs.divergent;
            tot.stuck += vs.stuck;
            tot.burstsApplied += vs.burstsApplied;
            tot.rimsApplied += vs.rimsApplied;
            tot.tyresFixed += vs.tyresFixed;
            tot.windowsSmashed += vs.windowsSmashed;
            tot.windowsFixed += vs.windowsFixed;
            tot.reported += vs.reported;
            tot.confirmed += vs.confirmed;
            tot.expired += vs.expired;
            tot.cmdFailures += vs.cmdFailures;
            tot.suspends += vs.suspends;
            if (kv.second.handle && kv.second.haveState)
            {
                if (kv.second.st.tyres)
                {
                    ++carsTyres;
                    tyreBits += (uint32_t)sync::PopCount8(kv.second.st.tyres);
                }
                if (kv.second.st.windows)
                    ++carsWindows;
            }
        }
        const VisStats& c = g_vis;
        const bool active = c.reads || c.sentUpdates || c.steerSent.n || c.steerApplied || c.steerApplyFail || c.brakeSet || c.brakeFail ||
                            c.handSet || c.handFail || c.partHits || carsTyres || carsWindows || tot.reads;
        if (join::IsConnected() && active)
            GLOGI("engine: vehicle visual (T3), last 10 s: natives: tyre reads ok %u / failed %u, window reads ok %u / failed %u, brake lights set %u (failed %u), "
                  "handbrake set %u (failed %u), pedal reads failed %u; as controller: %u updates sent (%u with a burst tyre, %u with a smashed window, %u with "
                  "brake lights, %u with the handbrake); copies: %u cars show tyre damage (%u tyres), %u show window damage; our shots: %u pops reported as vehicle "
                  "Hits (%u refused locally); since each car's first sight: %u reads, %u tyres burst / %u off the rim / %u fixed, %u windows smashed / %u fixed, "
                  "%u pops reported of which %u confirmed and %u expired, %u divergent reads of which %u stuck, %u failed command rounds (%u back-offs); steering "
                  "sent: n=%u min %.2f mean %.2f max %.2f (axis %u, held %u, estimate %u; axis agrees %u / disagrees %u with the estimate above 3 m/s); steering "
                  "applied: n=%u (failed %u) min %.2f mean %.2f max %.2f; sender yaw rate, GET_ENTITY_ROTATION_VELOCITY / rotation-derived: mean %.2f over %u samples; "
                  "0.4.30 steering field %s: written %u, refused or unreachable %u (bias instead), read-back held %u / overwritten %u, own-car sign agrees %u / disagrees %u; "
                  "moving copies %u frames: brake > 0.5 on %u, handbrake on %u",
                  c.tyreReadOk, c.tyreReadFail, c.winReadOk, c.winReadFail, c.brakeSet, c.brakeFail, c.handSet, c.handFail, c.pedalReadFail, c.sentUpdates,
                  c.sentWithTyres, c.sentWithWindows, c.brakeBitsSent, c.handBitsSent, carsTyres, tyreBits, carsWindows, c.partHits, c.partHitsRefused, tot.reads,
                  tot.burstsApplied, tot.rimsApplied, tot.tyresFixed, tot.windowsSmashed, tot.windowsFixed, tot.reported, tot.confirmed, tot.expired, tot.divergent,
                  tot.stuck, tot.cmdFailures, tot.suspends, c.steerSent.n, (double)c.steerSent.min, c.steerSent.Mean(), (double)c.steerSent.max, c.steerSent.axis,
                  c.steerSent.held, c.steerSent.estimate, c.steerSent.agree, c.steerSent.disagree, c.steerApplied, c.steerApplyFail, (double)g_vis.steerAppliedMin,
                  c.steerApplied ? g_vis.steerAppliedSum / c.steerApplied : 0.0, (double)g_vis.steerAppliedMax, c.yawRatioN ? c.yawRatioSum / c.yawRatioN : 0.0,
                  c.yawRatioN, g_world->VehicleFieldsReady() ? "ready" : "NOT resolved (bias only; see hook: [vehfields])", c.steerField, c.steerFieldRefused,
                  c.steerFieldHeld, c.steerFieldOverwritten, c.signAgree, c.signDisagree, c.copyMoving, c.copyBrake, c.copyHandbrake);
        // The window counters start over; the sync::VisualSync totals above run on.
        g_vis.reads = g_vis.tyreReadOk = g_vis.tyreReadFail = g_vis.winReadOk = g_vis.winReadFail = 0;
        g_vis.sentUpdates = g_vis.sentWithTyres = g_vis.sentWithWindows = g_vis.partHits = g_vis.partHitsRefused = 0;
        g_vis.brakeSet = g_vis.brakeFail = g_vis.handSet = g_vis.handFail = g_vis.brakeBitsSent = g_vis.handBitsSent = g_vis.pedalReadFail = 0;
        g_vis.steerApplied = g_vis.steerApplyFail = 0;
        g_vis.steerField = g_vis.steerFieldRefused = g_vis.steerFieldHeld = g_vis.steerFieldOverwritten = g_vis.signAgree = g_vis.signDisagree = 0;
        g_vis.copyMoving = g_vis.copyBrake = g_vis.copyHandbrake = 0;
        g_vis.steerAppliedMin = g_vis.steerAppliedMax = 0.0f;
        g_vis.steerAppliedSum = 0.0;
        g_vis.steerSent.Reset();
        g_vis.yawRatioSum = 0.0;
        g_vis.yawRatioN = 0;
    }

    // 0.4.19, every 10 s with the vehicles lines: the horn and the lights of the copies (HornSync / LightsKeeper counters of this window, the
    // IS_HORN_ACTIVE and GET_VEHICLE_LIGHTS_STATE read-backs) and what our own car put on the wire. "issued" counts calls the game accepted,
    // "heard" / "match" are read-backs. Script tick.
    void ReportVehSignals()
    {
        const SigStats& s = g_sig;
        const sync::HornStats& h = s.horn;
        const sync::LightsStats& l = s.lights;
        const bool active = s.hornStates || s.sentUpdates || s.lightsChanged || s.lightsReissueCalls || s.lightsReissueFail || h.rises ||
                            h.probes || h.startsIssued || h.callFails || l.reads || l.readFails;
        if (join::IsConnected() && active)
            GLOGI("engine: vehicle signals (0.4.19), last 10 s: horn on copies: %u states with the horn bit; %s: %u rises, %u releases (%u let go without "
                  "states), START_VEHICLE_HORN issued %u (+%u release calls, %u could not be called, %u back-offs); IS_HORN_ACTIVE read-back (%u asked, "
                  "%u unreadable): after a rise heard %u / silent %u, while held sounding %u / silent %u, after a release quiet %u / still sounding %u; "
                  "0.4.17 per-state calls %u; as controller: %u updates sent, %u with the horn (%u of them E held with the siren on, 0.4.30), %u with the siren, %u with the headlights, "
                  "%u with the high beam; lights on "
                  "copies: SET_VEHICLE_LIGHTS on a change %u; %s: read-backs %u (unreadable %u), match %u / differ %u, re-issued %u rounds (%u calls, %u "
                  "could not be called), restored %u, stuck %u",
                  s.hornStates, g_hornSync.load() ? "veh_horn_sync on" : "veh_horn_sync OFF", h.rises, h.releases, h.staleReleases, h.startsIssued,
                  h.stopsIssued, h.callFails, h.suspends, h.probes, h.probeFails, h.riseHeard, h.riseSilent, h.heldHeard, h.heldSilent, h.releaseQuiet,
                  h.releaseSounding, s.oldHornCalls, s.sentUpdates, s.sentHorn, s.sentSirenHorn, s.sentSiren, s.sentHeadlights, s.sentHighBeam, s.lightsChanged,
                  g_lightsKeep.load() ? "veh_lights_keep on" : "veh_lights_keep OFF", l.reads, l.readFails, l.match, l.mismatch, l.reissued,
                  s.lightsReissueCalls, s.lightsReissueFail, l.restored, l.stuck);
        g_sig = SigStats();
    }

    // 0.4.22 cars shake, every 10 s with the vehicles lines (libs/sync/vehicle_shake.h has the definitions). Grep "engine: vehicle shake (0.4.22)".
    //   OUR car: the car the local player drives, ticks by speed band (0-10 / 10-30 / over 30 m/s horizontal), the high-frequency part of its
    //     pitch and roll (rms of the second difference at the script tick, degrees: ~0.01 = smooth, a 50 Hz buzz of amplitude a reads 4a) and the
    //     vertical-speed sign flips per second (a buzz reads ~100, a bumpy road a few); what we write to it every tick (the hyper assist's velocity /
    //     angular changes per write, the power multiplier, the assists in force).
    //   remote copies: the same kind of numbers for the pose error the drive controller works from (position in mm, rotation in millidegrees), by
    //     the target's speed (parked / slow / moving), and the sign flips of that error (a copy overshooting its target every frame).
    //   angular writes: how many went out in which frame and what the read-backs said (body / world / both / neither), the frame in force.
    void ReportVehShake()
    {
        const sync::shake::LocalMeter::Window lw = g_localShake.Take();
        sync::shake::CopyMeter::Window cw;
        for (auto& kv : g_vehs)
            cw.Merge(kv.second.shake.Take());
        const sync::shake::AngFrameJudge::Counts jc = g_angJudge.TakeWindow();
        const ShakeStats s = g_shk;
        uint32_t copyFrames = 0;
        for (int i = 0; i < sync::shake::CopyMeter::kKinds; ++i)
            copyFrames += cw.bucket[i].frames;
        if (lw.ticks)
        {
            const char* pack = s.localModel ? game::content::PackOfModel(s.localModel) : nullptr;
            const auto& b = lw.band;
            GLOGI("engine: vehicle shake (0.4.22), last 10 s, OUR car (vehicle %d, model 0x%08x, pack %s): %u ticks (bands 0-10 / 10-30 / over 30 m/s: %u / %u / %u, "
                  "mean speed %.1f / %.1f / %.1f); pitch high-frequency rms %.3f / %.3f / %.3f deg (max %.2f), roll %.3f / %.3f / %.3f deg (max %.2f), "
                  "vertical-speed sign flips per s %.1f / %.1f / %.1f; history restarts %u; WE WRITE: velocity %u times (mean change %.3f, max %.3f m/s), "
                  "angular %u times (%u body frame / %u world frame; mean change %.4f, max %.3f rad/s), assists mask 0x%x, power x%.1f every tick; "
                  "veh_shake_fix=%s",
                  s.localVeh, (unsigned)s.localModel, pack ? pack : "-", lw.ticks, b[0].ticks, b[1].ticks, b[2].ticks, b[0].MeanSpeed(), b[1].MeanSpeed(),
                  b[2].MeanSpeed(), b[0].PitchRms(), b[1].PitchRms(), b[2].PitchRms(), std::max(b[0].pitchMax, std::max(b[1].pitchMax, b[2].pitchMax)),
                  b[0].RollRms(), b[1].RollRms(), b[2].RollRms(), std::max(b[0].rollMax, std::max(b[1].rollMax, b[2].rollMax)), b[0].FlipsPerS(),
                  b[1].FlipsPerS(), b[2].FlipsPerS(), lw.restarts, s.assistVelWrites, s.assistVelWrites ? s.dvSum / s.assistVelWrites : 0.0, s.dvMax,
                  s.assistAngWrites, s.assistAngBody, s.assistAngWorld, s.assistAngWrites ? s.dwSum / s.assistAngWrites : 0.0, s.dwMax, s.assistMask,
                  (double)s.power, g_shakeFix.load() ? "on" : "OFF");
        }
        if (copyFrames || s.copyAngWrites || s.impactAngWrites || jc.body || jc.world || jc.both || jc.neither)
        {
            using sync::shake::CopyMeter;
            const CopyMeter::Bucket& pk = cw.bucket[CopyMeter::Parked];
            const CopyMeter::Bucket& sl = cw.bucket[CopyMeter::Slow];
            const CopyMeter::Bucket& mv = cw.bucket[CopyMeter::Moving];
            GLOGI("engine: vehicle shake (0.4.22), last 10 s, remote copies: PARKED %u frames: position high-frequency rms %.2f mm (max %.1f), rotation %.1f mdeg "
                  "(max %.1f), error sign flips %u pos / %u rot; SLOW (0.5-3 m/s) %u frames: %.2f mm (max %.1f), %.1f mdeg, flips %u / %u; MOVING (over 3 m/s) %u frames: "
                  "%.2f mm (max %.1f), %.1f mdeg (max %.1f), flips %u / %u; history restarts %u; ANGULAR WRITES: copies %u (%u body frame / %u world frame), "
                  "server impacts %u; read-backs of ALL angular writes (copies, assist, impacts) %u body / %u world / %u both / %u neither; frame in force: %s "
                  "(judge flips %u); veh_shake_fix=%s",
                  pk.frames, pk.PosHfRmsMm(), pk.posMax * 1000.0, pk.RotHfRmsMdeg(), pk.rotMax * 1000.0, pk.posFlips, pk.rotFlips, sl.frames, sl.PosHfRmsMm(),
                  sl.posMax * 1000.0, sl.RotHfRmsMdeg(), sl.posFlips, sl.rotFlips, mv.frames, mv.PosHfRmsMm(), mv.posMax * 1000.0, mv.RotHfRmsMdeg(),
                  mv.rotMax * 1000.0, mv.posFlips, mv.rotFlips, cw.restarts, s.copyAngWrites, s.copyAngBody, s.copyAngWorld, s.impactAngWrites, jc.body, jc.world,
                  jc.both, jc.neither, g_angJudge.UseBody() ? "body" : "world", g_angJudge.Flips(), g_shakeFix.load() ? "on" : "OFF");
        }
        const int32_t keepVeh = g_shk.localVeh;
        const uint32_t keepModel = g_shk.localModel;
        g_shk = ShakeStats();
        g_shk.localVeh = keepVeh;                      // the identity of the car being followed survives the window
        g_shk.localModel = keepModel;
    }

    // R3 T4, every 10 s with the vehicles lines: the vehicle damage state, the controller's reads AND the copy's applied state per damaged car, so
    // ONE log shows a divergence (the "ctl" line is what our game reads off a car we control and what we report; the "copy" line is the server's
    // block next to what our game shows on a car somebody else controls), plus the sanity layer's counters and the pedal histogram that answers
    // whether the handbrake / brake-light reads are plausible. Script tick.
    void ReportVehDamage()
    {
        sync::PedalStats ped;
        sync::DamageStats rep;
        sync::ApplierStats app;
        uint32_t handReleases = 0, tyreUnrel = 0, winUnrel = 0, partsUnrel = 0, dentUnrel = 0;
        float gain = 0.0f;
        for (auto& kv : g_vehs)
        {
            const NetVeh& n = kv.second;
            const sync::PedalStats& ps = n.pedalGuard.Stats();
            ped.samples += ps.samples; ped.moving += ps.moving; ped.accelHigh += ps.accelHigh; ped.brakeHigh += ps.brakeHigh; ped.handHigh += ps.handHigh;
            ped.brakeRolling += ps.brakeRolling; ped.handDropped += ps.handDropped; ped.stuckRuns += ps.stuckRuns;
            ped.maxAccel = std::max(ped.maxAccel, ps.maxAccel); ped.maxBrake = std::max(ped.maxBrake, ps.maxBrake); ped.maxHand = std::max(ped.maxHand, ps.maxHand);
            handReleases += n.handLimit.Releases();
            const sync::DamageStats& rs = n.dmgRep.Stats();
            rep.reads += rs.reads; rep.sentRises += rs.sentRises;
            const sync::ApplierStats& as = n.dmgApply.Stats();
            app.reads += as.reads; app.divergent += as.divergent; app.doorsBroken += as.doorsBroken; app.lampsReplayed += as.lampsReplayed;
            app.dentsReplayed += as.dentsReplayed; app.petrolSet += as.petrolSet; app.dentConverged += as.dentConverged; app.dentNoEffect += as.dentNoEffect;
            app.dentGaveUp += as.dentGaveUp; app.cmdFailures += as.cmdFailures; app.suspends += as.suspends;
            if (as.gainUpdates) { gain = n.dmgApply.Gain(); app.gainUpdates += as.gainUpdates; }
            const sync::VisualMask u = n.visGuard.Unreliable();
            tyreUnrel += (uint32_t)sync::PopCount8(u.tyres | u.rim);
            winUnrel += (uint32_t)sync::PopCount8(u.windows);
            partsUnrel += (uint32_t)sync::PopCount8(n.dmgRep.UnreliableParts());
            dentUnrel += (uint32_t)sync::PopCount8(n.dmgRep.UnreliableDents());
        }
        const DmgStats& c = g_dmg;
        const bool active = c.ctlReads || c.blocksSent || c.copyRuns || c.guardsSet || c.adopted || ped.samples || tyreUnrel || winUnrel;
        if (join::IsConnected() && active)
        {
            GLOGI("engine: vehicle damage (R3 T4), last 10 s: veh_damage_sync %s, veh_visual_guard %s; as controller: %u damage reads, %u updates carried the block, "
                  "%u adopted a lower server health (a vehicle Hit); SANITY: unreliable slots now: tyres %u, windows %u, doors/lamps %u, dents %u (flagged in the window: "
                  "tyres %u windows %u doors/lamps %u dents %u petrol %u); PEDALS since the first sight of each car: %u samples (%u rolling), accelerator>0.1 %u, brake>0.1 %u, "
                  "handbrake>0.5 %u, foot brake while rolling forward %u, max accel %.2f brake %.2f hand %.2f, handbrake withheld as stuck %u samples (%u runs); as copy: %u "
                  "damage reads, doors broken %u, lamps replayed %u, dents replayed %u (converged %u, no effect %u, gave up %u, gain %.0f damage per metre after %u "
                  "read-backs), petrol set %u, failed rounds %u (%u back-offs), handbrake releases after a long roll %u, self-damage guards set %u",
                  g_vehDamageSync.load() ? "on" : "OFF", g_vehVisualGuard.load() ? "on" : "OFF", c.ctlReads, c.blocksSent, c.adopted, tyreUnrel, winUnrel, partsUnrel,
                  dentUnrel, c.flaggedTyres, c.flaggedWindows, c.flaggedParts, c.flaggedDents, c.flaggedPetrol, ped.samples, ped.moving, ped.accelHigh, ped.brakeHigh,
                  ped.handHigh, ped.brakeRolling, (double)ped.maxAccel, (double)ped.maxBrake, (double)ped.maxHand, ped.handDropped, ped.stuckRuns, c.copyRuns,
                  app.doorsBroken, app.lampsReplayed, app.dentsReplayed, app.dentConverged, app.dentNoEffect, app.dentGaveUp, (double)gain, app.gainUpdates, app.petrolSet,
                  app.cmdFailures, app.suspends, handReleases, c.guardsSet);
            int lines = 0;
            for (const auto& kv : g_vehs)
            {
                const NetVeh& n = kv.second;
                if (!n.handle || !n.haveState || lines >= 4)
                    continue;
                const bool ours = n.st.ctrl == join::MyId();
                const proto::VehicleDamage& d = ours ? n.dmgReport : n.st.dmg;
                if (!n.st.dmg.Any() && !d.Any() && n.st.bodyHealth >= 990)
                    continue;
                ++lines;
                GLOGI("engine: vehicle id=%u damage (%s): server body %u engine %u petrol %u parts %02x dents [%u %u %u %u %u %u %u %u]; %s read body %.0f petrol %.0f "
                      "doors %02x lamps %x dents [%u %u %u %u %u %u %u %u]%s", (unsigned)kv.first, ours ? "ctl" : "copy", (unsigned)n.st.bodyHealth, (unsigned)n.st.engineHealth,
                      (unsigned)n.st.dmg.petrol, (unsigned)n.st.dmg.parts, n.st.dmg.dent[0], n.st.dmg.dent[1], n.st.dmg.dent[2], n.st.dmg.dent[3], n.st.dmg.dent[4],
                      n.st.dmg.dent[5], n.st.dmg.dent[6], n.st.dmg.dent[7], ours ? "controller's" : "copy's own", (double)n.dmgRaw.body, (double)n.dmgRaw.petrol,
                      n.dmgRaw.doors, n.dmgRaw.lamps, n.dmgRaw.dent[0], n.dmgRaw.dent[1], n.dmgRaw.dent[2], n.dmgRaw.dent[3], n.dmgRaw.dent[4], n.dmgRaw.dent[5],
                      n.dmgRaw.dent[6], n.dmgRaw.dent[7],
                      ours ? "" : (n.dmgRaw.Any() ? "" : " (not read yet)"));
            }
        }
        g_dmg.ctlReads = g_dmg.blocksSent = g_dmg.adopted = g_dmg.copyRuns = g_dmg.copyReplays = 0;
        g_dmg.flaggedTyres = g_dmg.flaggedWindows = g_dmg.flaggedParts = g_dmg.flaggedDents = g_dmg.flaggedPetrol = 0;
    }

    // Every 10 s: one line each for vehicles, combat and anims (only while connected).
    void ReportV4(double nowMs)
    {
        static double lastMs = 0;
        static V4Stats last;
        static uint32_t lastWorlds = 0, lastEmotes = 0, lastFallbackStates = 0;
        if (nowMs - lastMs < 10000.0)
            return;
        lastMs = nowMs;
        ReportVehVisual();                             // T3
        ReportVehDamage();                             // R3 T4
        ReportVehSignals();                            // 0.4.18
        ReportVehShake();                              // 0.4.22
        g_v4.hitDamage = g_hitDamageConfirmed.load(std::memory_order_relaxed);   // the join worker counts it
        g_v4.hitsConfirmed = g_hitConfirmCount.load(std::memory_order_relaxed);
        const V4Stats& c = g_v4;
        const uint32_t worlds = g_vehWorldsIn.load(), emotes = g_emotesIn.load();
        if (join::IsConnected())
        {
            size_t tracked = 0, spawned = 0, seated = 0;
            {
                tracked = g_vehs.size();                // the game thread's own maps: no lock
                for (const auto& kv : g_vehs)
                    if (kv.second.handle)
                        ++spawned;
                for (const auto& kv : g_peers)
                    if (kv.second.seatedVeh >= 0)
                        ++seated;
            }
            const sync::VehLinkStats& ls = g_link.Stats();
            GLOGI("engine: vehicles: %zu tracked, %zu spawned, %zu peers seated; link %s id=%d seat %d; last 10 s: "
                  "%u VehicleWorld in, %u spawned, %u deleted, %u spawn fails, %u updates sent, %u cmds (%u ok, %u "
                  "refused), %u seat warps (%u re-warps of an already seated ped), %u unseats, %u ejects, %u seat reverts, %u F/G entries (%u refused), "
                  "%u snaps; link total %u enters %u registers %u exits %u retries %u timeouts",
                  tracked, spawned, seated, g_link.StateName(), g_link.NetId(), g_link.Seat(), worlds - lastWorlds,
                  c.vehSpawned - last.vehSpawned, c.vehDeleted - last.vehDeleted, c.vehSpawnFail - last.vehSpawnFail,
                  c.vehUpdates - last.vehUpdates, c.vehCmds - last.vehCmds, c.vehOk - last.vehOk,
                  c.vehRefused - last.vehRefused, c.seatWarps - last.seatWarps, c.seatRewarps - last.seatRewarps, c.unseats - last.unseats,
                  c.ejects - last.ejects, c.reverts - last.reverts, c.entryPicks - last.entryPicks,
                  c.entryRefused - last.entryRefused, c.vehSnaps - last.vehSnaps, ls.enters, ls.registers, ls.exits,
                  ls.retries, ls.timeouts);
            {
                // W9: the passenger / seat / collision / bike numbers of the same 10 s (docs/sync-specs/live-0.4.2-fixes.md W9).
                char pax[120];
                if (nowMs - g_pax.atMs < 2000.0)
                    std::snprintf(pax, sizeof(pax), "riding in vehicle id=%u: the server says the engine %s, our copy reads it %s", (unsigned)g_pax.id,
                                  g_pax.serverOn ? "runs" : "is off", g_pax.running ? "running" : "stopped");
                else
                    std::snprintf(pax, sizeof(pax), "not riding a car somebody else controls");
                GLOGI("engine: vehicles (W9), last 10 s: %u seat re-warps done, %u held back by the limiter; %u F/G presses taken (%u quick exits "
                      "after an entry); passenger engine: %u re-asserts, %s; %u peer bail-outs of a moving vehicle, %u forced exits of ours "
                      "by bail-out; %u vehicle impacts shown on puppets; contact graces opened %u by the collision flag / %u by a speed "
                      "loss / %u by a velocity kick; %u car frames drawn in the present (lead); shots: at seated puppets %u, at a puppet's car %u, at puppets on foot "
                      "%u; hits reported on seated puppets %u, pool drops on seated puppets %u",
                      c.seatRewarps - last.seatRewarps, c.seatRewarpsHeld - last.seatRewarpsHeld, c.entryPresses - last.entryPresses,
                      c.quickExits - last.quickExits, c.engineAsserts - last.engineAsserts, pax, c.bailOuts - last.bailOuts,
                      c.localBailOuts - last.localBailOuts, c.vehImpacts - last.vehImpacts, c.contactsFlag - last.contactsFlag,
                      c.contactsDecel - last.contactsDecel, c.contactsKick - last.contactsKick, c.leadFrames - last.leadFrames, c.shotsAtSeated - last.shotsAtSeated,
                      c.shotsAtPuppetCar - last.shotsAtPuppetCar, c.shotsAtFoot - last.shotsAtFoot, c.hitsOnSeated - last.hitsOnSeated,
                      c.seatedHpDrops - last.seatedHpDrops);
                for (auto& kv : g_vehs)
                {
                    NetVeh& bv = kv.second;
                    if (!sync::IsBikeClass(bv.vehClass))
                        continue;
                    const sync::RollTracker::Window w = bv.roll.Take();
                    const sync::VehicleDriveStats& ds = bv.drive.Stats();
                    if (w.frames)
                        GLOGI("engine: bike id=%u (class %d), last 10 s: %u frames, roll error against the sender's max %.1f / mean %.1f deg, "
                              "top speed %.1f m/s; drive: %u snaps, %u quaternion blends, %u contact graces (no bike-specific handling: the lean "
                              "rides in the quaternion)", (unsigned)kv.first, bv.vehClass, w.frames, w.maxErrDeg, w.meanErrDeg, w.maxSpeedMs,
                              ds.snaps - bv.rollSnapsSeen, ds.blends - bv.rollBlendsSeen, ds.contacts - bv.rollGracesSeen);
                    bv.rollSnapsSeen = ds.snaps;
                    bv.rollBlendsSeen = ds.blends;
                    bv.rollGracesSeen = ds.contacts;
                }
                {
                    // Sync B (docs/sync-specs/vehicles.md, collision audit): what the remote copies' collision path really does. One total line
                    // per 10 s while any copy is driven, one line per copy something touched or came near (at most 6). See
                    // libs/sync/vehicle_contact.h for how to read the numbers.
                    sync::ContactWindow::Window tot;
                    size_t driven = 0, inRange = 0, printed = 0, interesting = 0;
                    uint32_t mutes = 0;
                    float worstDev = 0.0f;
                    for (auto& kv : g_vehs)
                    {
                        NetVeh& cv = kv.second;
                        const sync::ContactWindow::Window w = cv.contact.Take();
                        const uint32_t kmutes = cv.drive.Stats().kickMutes - cv.contactKickMutesSeen;
                        cv.contactKickMutesSeen = cv.drive.Stats().kickMutes;
                        if (!w.frames)
                            continue;
                        ++driven;
                        if (w.flagReads)
                            ++inRange;
                        tot.frames += w.frames;
                        tot.flagReads += w.flagReads;
                        tot.flagTrue += w.flagTrue;
                        tot.edges += w.edges;
                        tot.flagTrueHard += w.flagTrueHard;
                        for (int i = 1; i < 4; ++i)
                            tot.opened[i] += w.opened[i];
                        tot.graceFrames += w.graceFrames;
                        tot.hardFrames += w.hardFrames;
                        tot.devOver1 += w.devOver1;
                        tot.devOver3 += w.devOver3;
                        tot.localUnder3m += w.localUnder3m;
                        tot.localUnder1_5m += w.localUnder1_5m;
                        tot.puppetUnder3m += w.puppetUnder3m;
                        worstDev = std::max(worstDev, w.devMax);
                        mutes += kmutes;
                        if (w.Interesting() || kmutes)
                        {
                            ++interesting;
                            if (printed < 6)
                            {
                                ++printed;
                                GLOGI("engine: vehicle collision (Sync B) id=%u, last 10 s: %u frames; collision flag read %u times, true %u (%.0f%%), %u "
                                      "rising edges, %u flag-true frames overwritten at full weight (no grace); graces opened %u flag / %u speed loss / "
                                      "%u kick (%u kick mutes), %u grace frames, %u full-weight frames; deviation from our last velocity command: max "
                                      "%.1f m/s, %u frames over 1, %u over 3; at an edge max %.1f (the frame before it %.1f); nearest approach to our "
                                      "own vehicle %.1f m (%u frames under 3 m, %u under 1.5 m), to another remote car %.1f m (%u frames under 3 m)",
                                      (unsigned)kv.first, w.frames, w.flagReads, w.flagTrue, w.flagReads ? 100.0 * w.flagTrue / w.flagReads : 0.0, w.edges,
                                      w.flagTrueHard, w.opened[1], w.opened[2], w.opened[3], kmutes, w.graceFrames, w.hardFrames, (double)w.devMax,
                                      w.devOver1, w.devOver3, (double)w.edgeDevMax, (double)w.preEdgeDevMax, (double)w.minLocalM, w.localUnder3m,
                                      w.localUnder1_5m, (double)w.minPuppetM, w.puppetUnder3m);
                            }
                        }
                    }
                    if (driven)
                        GLOGI("engine: vehicle collision (Sync B), last 10 s, all remote copies: %zu driven (%zu within 60 m, where the flag is read), %u "
                              "frames; collision flag true in %u of %u reads (%.0f%%), %u rising edges, %u flag-true frames overwritten at full weight; "
                              "graces opened %u flag / %u speed loss / %u kick (%u kick mutes: a body that does not honour SET_ENTITY_VELOCITY); "
                              "%u grace frames; deviation from our last velocity command over 1 m/s in %u frames, over 3 m/s in %u (worst %.1f m/s); "
                              "frames within 3 m of our own vehicle %u (1.5 m: %u), within 3 m of another remote copy %u; %zu copies printed of %zu "
                              "that were touched or close",
                              driven, inRange, tot.frames, tot.flagTrue, tot.flagReads, tot.flagReads ? 100.0 * tot.flagTrue / tot.flagReads : 0.0,
                              tot.edges, tot.flagTrueHard, tot.opened[1], tot.opened[2], tot.opened[3], mutes, tot.graceFrames, tot.devOver1,
                              tot.devOver3, (double)worstDev, tot.localUnder3m, tot.localUnder1_5m, tot.puppetUnder3m, printed, interesting);
                }
            }
            {
                // R3 T5 (vehicle position): where a remote copy is DRAWN against where its owner's report puts it now (+ the link lag), per speed and distance band,
                // with the terms behind it, and the controller's side (how fast the cars we send were going), so ONE log has both views. Read with
                // docs/sync-specs/vehicle-position.md: a mean of metres at 15-30 m/s with "aim N ms behind" ~ 0 = the lag is the network (check the link lag line);
                // "aim" large = the lead is not closing the render delay; "tracking" large = the drive controller / physics.
                static uint32_t lastLinkIn = 0, lastAcc = 0, lastRep = 0, lastStale = 0, lastTele = 0, lastOmStill = 0, lastOmCross = 0;
                uint32_t acc = 0, rep = 0, stl = 0, tel = 0, omStill = 0, omCross = 0;
                VehInterpTotals(acc, rep, stl, tel, omStill, omCross);
                const sync::PositionDiag::Window pw = g_t5.pos.Take();
                const uint32_t linkIn = g_linkInfoIn.load(std::memory_order_relaxed);
                if (pw.frames > 0 || g_t5.sentN > 0 || linkIn != lastLinkIn)
                {
                    std::string owners;
                    {
                        std::lock_guard<std::mutex> llk(g_linkMtx);
                        for (const auto& rt : g_linkRtt)
                            owners += (owners.empty() ? "" : ", ") + std::to_string(rt.first) + ":" + std::to_string(rt.second) + " ms";
                    }
                    const sync::NetClock& clk5 = g_sync.Clock();
                    GLOGI("engine: vehicle position (R3 T5), last 10 s: near clock %s, proximity contact %s; our round trip %u ms, controllers' (server's view) [%s], %u LinkInfo in, "
                          "owner's round trip unknown on %u frames; link lag used mean %.0f max %.0f ms; shared delay %.0f ms; copies: %s; proximity: %u frames predicted an overlap, "
                          "%u graces opened, %u upgraded to a real contact; snapshots in 10 s: %u stored, %u resends dropped, %u stale, %u teleports; "
                          "omega guard (0.4.30, veh_omega_guard %s): omega calls held still %u, refused across sources %u; as controller: %u updates sent (mean speed %.1f m/s; speed bands 0-5/5-15/15-30/over 30: %u/%u/%u/%u)",
                          g_nearClock.load() ? "on" : "OFF", g_proxContact.load() ? "on" : "OFF", (unsigned)join::RttMs(), owners.c_str(), linkIn - lastLinkIn,
                          g_t5.ownerRttUnknown, g_t5.lagN ? g_t5.lagSum / g_t5.lagN : 0.0, g_t5.lagMax, clk5.DelayMs(), sync::PositionDiag::Format(pw).c_str(),
                          g_t5.proxFrames, g_t5.proxOpened, g_t5.proxUpgraded, (acc >= lastAcc ? acc - lastAcc : 0u), (rep >= lastRep ? rep - lastRep : 0u), (stl >= lastStale ? stl - lastStale : 0u), (tel >= lastTele ? tel - lastTele : 0u),
                          g_omegaGuard.load() ? "on" : "OFF", (omStill >= lastOmStill ? omStill - lastOmStill : 0u), (omCross >= lastOmCross ? omCross - lastOmCross : 0u),
                          g_t5.sentN, g_t5.sentN ? g_t5.sentSpeedSum / g_t5.sentN : 0.0,
                          g_t5.sentFrames[0], g_t5.sentFrames[1], g_t5.sentFrames[2], g_t5.sentFrames[3]);
                }
                lastLinkIn = linkIn;
                lastAcc = acc; lastRep = rep; lastStale = stl; lastTele = tel; lastOmStill = omStill; lastOmCross = omCross;
                g_t5.proxFrames = g_t5.proxOpened = g_t5.proxUpgraded = 0;
                g_t5.lagSum = g_t5.lagMax = 0.0;
                g_t5.lagN = g_t5.ownerRttUnknown = 0;
                g_t5.sentN = 0;
                g_t5.sentSpeedSum = 0.0;
                for (uint32_t& sf : g_t5.sentFrames) sf = 0;
            }
            {
                // T4 (vehicle collisions): the contacts our cars felt, the copies whose response was adopted, the server's impacts and what became of
                // them, and the divergence estimate (a copy against its controller's stream 500 ms after the contact). Read with
                // docs/sync-specs/vehicle-collisions.md: "adopted" > 0 with "refused" 0 = the copy path ran; "applied" > 0 = the server's resolution
                // reached a car we control and differed from the game's; "no local contact" many = the server's boxes met where our physics felt nothing.
                static T4Stats lastT4;
                static uint32_t lastGuard = 0;
                uint32_t guardNow = 0;
                for (auto& kv : g_vehs)
                    guardNow += VehInterpGuarded(kv.first);
                const T4Stats& t = g_t4;
                if (t.adopted != lastT4.adopted || t.adoptRefused != lastT4.adoptRefused || t.jumpEpisodes != lastT4.jumpEpisodes ||
                    t.impactsIn != lastT4.impactsIn || t.divN != lastT4.divN)
                    GLOGI("engine: vehicle impacts (T4), last 10 s: impact_sync %s; copies whose response was adopted %u (contacts left to the grace "
                          "%u, stream pushes held back %u); velocity jumps our cars felt %u; server impacts in %u: applied %u (change written: %.1f m/s "
                          "total, largest %.1f), no local contact %u, too small %u, too old %u, duplicate %u, car not ours %u, queue overflow %u; "
                          "copy vs controller's stream 500 ms after the adoption: %u compared, mean error %.2f m, worst %.2f m",
                          g_impactSync.load() ? "on" : "OFF", t.adopted - lastT4.adopted, t.adoptRefused - lastT4.adoptRefused, guardNow - lastGuard,
                          t.jumpEpisodes - lastT4.jumpEpisodes, t.impactsIn - lastT4.impactsIn, t.applied - lastT4.applied, t.appliedDvSum - lastT4.appliedDvSum,
                          t.appliedDvMax, t.noLocal - lastT4.noLocal, t.tiny - lastT4.tiny, t.stale - lastT4.stale, t.dup - lastT4.dup,
                          t.noVehicle - lastT4.noVehicle, t.dropped - lastT4.dropped, t.divN - lastT4.divN,
                          t.divN > lastT4.divN ? (t.divSum - lastT4.divSum) / (t.divN - lastT4.divN) : 0.0, t.divMax);
                lastT4 = g_t4;
                lastT4.divMax = 0.0;
                g_t4.divMax = 0.0;
                lastGuard = guardNow;
            }
            {
                // R4 T2 (docs/sync-specs/vehicle-collision-authority.md): what the copies' collision flags touched and what the contact gate did with it, how close a copy came to a
                // vehicle we control, the arbiter's numbers for our cars (authoritative change, closing speed, depth), and the divergence check after EVERY contact.
                const sync::TouchTally::Window tw = g_r4.tally.Take();
                const sync::ContactWatch::Window cw = g_r4.watch.Take();
                uint32_t edges = 0;
                for (uint32_t c : tw.byKind) edges += c;
                bool anyWatch = cw.merged || cw.dropped;
                for (int c = 1; c < sync::ContactWatch::kCauses; ++c) anyWatch = anyWatch || cw.opened[c];
                if (edges || anyWatch || g_r4.impactsAuth || g_r4.gatedKick || g_r4.gatedLoss)
                {
                    GLOGI("engine: vehicle contacts (R4 T2), last 10 s: contact gate %s (a grace needs a vehicle we control within %.0f m), acceleration-aware extrapolation %s; "
                          "collision flag rose %u times on copies, touching: a car we control %u, another copy %u, in the air / on a prop %u, something solid (speed lost) %u, the ground / a kerb %u "
                          "(highest %.2f m above ground, largest speed loss %.1f m/s); evidence ignored by the gate: %u flag edges, %u speed losses, %u kicks; closest copy to a vehicle we "
                          "control %.1f m (%u copy-frames under 3 m); server impacts for our cars %u: largest authoritative change %.1f m/s, closing speed %.1f m/s, depth %.2f m; "
                          "copy vs the owner after a contact: %s",
                          g_contactGate.load() ? "on" : "OFF", kContactNearM, g_accelExtrap.load() ? "on" : "OFF", edges,
                          tw.byKind[(int)sync::TouchKind::OwnCar], tw.byKind[(int)sync::TouchKind::OtherCopy], tw.byKind[(int)sync::TouchKind::Airborne],
                          tw.byKind[(int)sync::TouchKind::Solid], tw.byKind[(int)sync::TouchKind::Ground], tw.heightMax, tw.lossMax,
                          g_r4.gatedEdges, g_r4.gatedLoss, g_r4.gatedKick, g_r4.ownMinM < 1e8 ? g_r4.ownMinM : -1.0, g_r4.ownUnder3m, g_r4.impactsAuth, g_r4.authDvMax,
                          g_r4.closingMax, g_r4.depthMax, sync::ContactWatch::Format(cw).c_str());
                }
                g_r4.gatedEdges = g_r4.gatedKick = g_r4.gatedLoss = g_r4.impactsAuth = g_r4.ownUnder3m = 0;
                g_r4.authDvMax = g_r4.closingMax = g_r4.depthMax = 0.0;
                g_r4.ownMinM = 1e9;
            }
            {
                static SyncDStats lastD;
                static uint32_t lastHand[2] = { 0, 0 };
                ReportSyncD(lastD, lastHand);
                lastD = g_syncD;
                lastHand[0] = g_syncDHandovers[0].load();
                lastHand[1] = g_syncDHandovers[1].load();
                ReportSeats();                             // T1 seats
                ReportDoorsSeats();                        // 0.4.22 doors and seats
            }
            const sync::VitalsStats& vs = g_vitals.Stats();
            const sync::HitBatchStats& hs = g_hits.Stats();
            GLOGI("engine: combat: server health %u armour %u%s, pending self-damage %d; last 10 s: %u shots, %u peer "
                  "shots shown, %u hits sent, %u confirmed damage dealt, %u refused locally, %u damage events in (%u), "
                  "%u kills in feed, %u respawn requests, peer deaths %u / recoveries %u / local-only deaths %u; "
                  "totals: self-hits %u (%u), acks %u, resyncs %u, gains undone %u, forced kills %u, batch dropped %u "
                  "rate-held %u",
                  (unsigned)g_vitals.Health(), (unsigned)g_vitals.Armour(), g_vitals.ServerDead() ? " (DEAD)" : "",
                  g_vitals.Pending(), c.shots - last.shots, c.peerShots - last.peerShots, c.hitsSent - last.hitsSent,
                  c.hitDamage - last.hitDamage, c.hitsRefusedLocal - last.hitsRefusedLocal, c.damageIn - last.damageIn,
                  c.damageInTotal - last.damageInTotal, c.kills - last.kills, c.respawnReqs - last.respawnReqs,
                  c.peerDeaths - last.peerDeaths, c.peerRecoveries - last.peerRecoveries,
                  c.peerLocalDeaths - last.peerLocalDeaths, vs.selfHits, vs.selfDamage, vs.acks, vs.resyncs,
                  vs.clampsDown, vs.kills, hs.dropped, hs.rateHeld);
            // T6 health line: what the ped really holds against what the server says, and the counters that explain a
            // mismatch (see the T6 report for how to read it).
            // R3 T1: ONE line that tells which GTA behaviour is true (mode, measured ceiling, ped against server, bar, read-backs, reconciler).
            GLOGI("%s", g_health.StatusLine(g_vitals, (int)g_playerHealth.load(), ServerMode()).c_str());
            uint32_t evEvicted = 0, evRefused = 0;
            {
                std::lock_guard<std::mutex> lk(g_evMtx);
                evEvicted = g_evQueue.Evicted();
                evRefused = g_evQueue.Refused();
            }
            GLOGI("engine: combat: guards (totals): %u losses a remote puppet caused (not self-reported, %u put back "
                  "after the grace), %u ticks skipped for an unreadable health/armour, event queue evicted %u refused %u",
                  vs.puppetLosses, vs.puppetRestores, vs.skippedReads, evEvicted, evRefused);
            GLOGI("engine: combat client (W3), last 10 s: %u melee swings counted, %u peer swing tasks ISSUED (not proof anything was seen: see the "
                  "'engine: melee swings (R3 T6)' line for the read-back), %u real peer shots, "
                  "%u muzzle flashes; reactions on us %u flinch / %u knockdown / %u melee stagger, on puppets %u flinch / %u "
                  "knockdown / %u melee stagger; stagger clips NOT shown (0.4.30): no dictionary %u, streaming %u, call failed %u",
                  c.meleeSwings - last.meleeSwings, c.peerSwings - last.peerSwings, c.peerShotsReal - last.peerShotsReal,
                  c.muzzleFlashes - last.muzzleFlashes, c.reactFlinch - last.reactFlinch, c.reactKnock - last.reactKnock,
                  c.reactStagger - last.reactStagger, c.puppetFlinch - last.puppetFlinch, c.puppetKnock - last.puppetKnock,
                  c.puppetStagger - last.puppetStagger, c.staggerNoDict - last.staggerNoDict, c.staggerStreaming - last.staggerStreaming,
                  c.staggerFailed - last.staggerFailed);
            GLOGI("engine: melee (0c), last 10 s: hits sent (all weapons) %u; melee hits detected by the damage EVENT %u / by the pool drop %u (event read "
                  "%u times in swing windows); swings counted %u; on puppets %u swing steps seen, %u swing tasks issued (NOT proof they played), %u not issued (ped owned "
                  "%u, dictionary missing/streaming %u, anim task failed %u); AI melee damage modifier %s; audit (0.4.5): swing windows "
                  "opened on a puppet %u (puppet within 3 m %u), stale damage events cleared %u, claimed %u by event / %u by pool, "
                  "Damage confirmed by the server on our Hits %u (%u damage; R3 T6: the server now judges our swings itself, see the 'melee:' "
                  "server line and 'engine: melee server (R3 T6)')",
                  c.hitsSent - last.hitsSent, c.meleeHitsEvent - last.meleeHitsEvent, c.meleeHitsPool - last.meleeHitsPool,
                  c.meleeEventsRead - last.meleeEventsRead, c.meleeSwings - last.meleeSwings,
                  c.peerSwingSteps - last.peerSwingSteps, c.peerSwings - last.peerSwings,
                  (c.peerSwingOwned - last.peerSwingOwned) + (c.peerSwingNoDict - last.peerSwingNoDict) +
                      (c.peerSwingFailed - last.peerSwingFailed),
                  c.peerSwingOwned - last.peerSwingOwned, c.peerSwingNoDict - last.peerSwingNoDict,
                  c.peerSwingFailed - last.peerSwingFailed, g_aiMeleeApplied ? "0.0 (ai_melee_mod=1)" : "not set",
                  c.meleeWindows - last.meleeWindows, c.meleeNearSwings - last.meleeNearSwings,
                  c.meleeStaleEvents - last.meleeStaleEvents, c.meleeEventDamage - last.meleeEventDamage,
                  c.meleePoolDamage - last.meleePoolDamage, c.hitsConfirmed - last.hitsConfirmed, c.hitDamage - last.hitDamage);
            {
                // R3 T6: the EFFECT counters. Swings of OUR fists that the server counted as hits (its own judgement or our Hit, the Damage
                // carries kDmgMelee either way), and the other player's swings as the read-back saw them on our screen.
                const uint32_t mc = g_meleeConfirmCount.load(std::memory_order_relaxed), md = g_meleeConfirmDamage.load(std::memory_order_relaxed),
                               mk = g_meleeConfirmKills.load(std::memory_order_relaxed);
                static uint32_t lastMc = 0, lastMd = 0, lastMk = 0;
                GLOGI("engine: melee server (R3 T6), last 10 s: our swings counted %u, melee blows the SERVER confirmed on others %u (%u damage, %u kills) "
                      "[totals this session: %u / %u damage / %u kills]; Hits we sent %u; melee hits we detected locally (event %u, pool %u) are no longer "
                      "needed for damage",
                      c.meleeSwings - last.meleeSwings, mc - lastMc, md - lastMd, mk - lastMk, mc, md, mk, c.hitsSent - last.hitsSent,
                      c.meleeHitsEvent - last.meleeHitsEvent, c.meleeHitsPool - last.meleeHitsPool);
                lastMc = mc; lastMd = md; lastMk = mk;
                GLOGI("engine: melee swings (R3 T6), last 10 s: %s", sync::SwingDiagLine(g_swingDiag, g_swingDiagLast, g_swingLadder).c_str());
                g_swingDiagLast = g_swingDiag;
            }
            GLOGI("engine: combat client (W8), last 10 s: local health changed by a puppet ped's gun or fists %u times (our "
                  "own cosmetic flinch bullets included); AI damage modifiers %s; dead-puppet melee reports capped %u, puppets "
                  "killed by a takedown %u; TASK_CLIMB issued %u, placement fallbacks %u; alt-tab fallback states sent %u",
                  c.puppetFireLoss - last.puppetFireLoss, g_aiDamageApplied ? "0.0 (applied)" : "not applied",
                  c.deadCapped - last.deadCapped, c.takedownKills - last.takedownKills, c.climbTasks - last.climbTasks,
                  c.climbFallbacks - last.climbFallbacks,
                  g_fallbackStates.load(std::memory_order_relaxed) - lastFallbackStates);
            lastFallbackStates = g_fallbackStates.load(std::memory_order_relaxed);
            // R3 T3: the sender's own cover reads of the last 10 s (always in the 0d line, in full when any read was true)
            const sync::CoverSenderStats sndCover = g_coverSense.Take();
            const std::string sndLine = sync::CoverSenderLine((int)join::MyId(), sndCover);
            GLOGI("engine: vault / cover (0d), last 10 s: TASK_CLIMB issued %u, started %u (the puppet really climbed / vaulted), "
                  "placement fallbacks %u; puppet put on the sender's pose before a task: nudged %u, set outright %u; cover: "
                  "task issued %u, started %u (IS_PED_IN_COVER), fell back to the crouch pose on the sender's position %u, ended %u (R3 T3: cover_sync=%s; the "
                  "SENDER side, ours: IS_PED_IN_COVER true %u, IS_PED_GOING_INTO_COVER true %u, IS_PED_AIMING_FROM_COVER true %u of %u reads, kPfCover sent on %u; "
                  "the breaker opened %u times)",
                  c.climbTasks - last.climbTasks, c.climbStarted - last.climbStarted, c.climbFallbacks - last.climbFallbacks,
                  c.prePlaceNudged - last.prePlaceNudged, c.prePlaceSet - last.prePlaceSet, c.coverIssued - last.coverIssued,
                  c.coverStarted - last.coverStarted, c.coverFailed - last.coverFailed, c.coverEnded - last.coverEnded,
                  sync::CoverSyncName((sync::CoverSyncMode)g_coverSyncMode.load()), sndCover.inReads, sndCover.goingReads, sndCover.aimingReads, sndCover.samples,
                  sndCover.sentCover, g_coverBreaker.Opens());
            if (!sndLine.empty())
                GLOGI("%s", sndLine.c_str());
            GLOGI("engine: anims: last 10 s: %u emote events in, %u started, %u dictionary timeouts, %u ended "
                  "not-playing, %u missing dictionaries; ours %s",
                  emotes - lastEmotes, c.emotesStarted - last.emotesStarted, c.emoteDictTimeouts - last.emoteDictTimeouts,
                  c.emoteNotPlaying - last.emoteNotPlaying, c.emoteMissingDict - last.emoteMissingDict,
                  !g_localEmote.Active() ? "none" : g_localEmote.Kind() == sync::EmoteKind::Anim ? g_localAnim.dict.c_str()
                                                                                                  : g_localScenario.c_str());
        }
        if (join::IsConnected())
        {
            // 0e: per animation kind, what the last 10 s asked for, loaded, issued and actually SAW playing; "unseen" is a clip
            // IS_ENTITY_PLAYING_ANIM never saw (wrong name), "ended(...)" says what ended the emotes.
            GLOGI("engine: anims (0e), last 10 s: %s", sync::AnimDiagLine(g_animDiag, g_animDiagLast).c_str());
            // T2: the on-foot one-shots asked of the puppets against the game's own readback ("SHOWN" = IS_PED_JUMPING etc. said yes within
            // the kind's window after the task). A kind that is asked but never SHOWN is an animation the others do not see.
            GLOGI("engine: anims onfoot (T2), last 10 s: %s", OneShotLine().c_str());
        }
        for (size_t i = 0; i < (size_t)sync::OneShot::kCount; ++i)
            g_oneShotLast[i] = g_oneShot[i];
        g_animDiagLast = g_animDiag;
        last = c;
        lastWorlds = worlds;
        lastEmotes = emotes;
    }

    // Sync plan W3 (combat 4, sender): IS_PED_SHOOTING never fires for a punch, so shotSeq did not move and the
    // others saw no swing. On foot and alive with a melee-class weapon, a press of INPUT_MELEE_ATTACK_LIGHT /
    // HEAVY / ALTERNATE (140 / 141 / 142: INFERRED ids) or the rising edge of IS_PED_PERFORMING_MELEE_ACTION counts
    // one swing into g_shotSeq; sync::MeleeSwingGate makes the two triggers one swing within 300 ms. Not while the
    // chat or a menu has the keyboard. The server's melee rules are unchanged (W5). Script tick, every frame.
    void TickLocalMelee(int32_t lp, bool inVehicle, double nowMs)
    {
        bool control = false, action = false;
        // 0c: the melee state of our ped, for ProbePeerHits' event window (a menu or the chat does not change what is in hand)
        g_meleeHeld = !inVehicle && g_life == Life::Alive && IsMeleeClassWeapon(g_world->GetSelectedPedWeapon(lp));
        g_meleeActionNow = false;
        if (g_meleeHeld && !game::Overlay::Interactive())
        {
            control = g_world->IsDisabledControlJustPressed(0, 140) || g_world->IsDisabledControlJustPressed(0, 141) ||
                      g_world->IsDisabledControlJustPressed(0, 142);
            action = g_world->IsPedPerformingMeleeAction(lp);
            g_meleeActionNow = action;
        }
        if (!g_meleeGate.Tick(nowMs, control, action))
            return;
        const double sinceLastSwing = nowMs - g_lastSwingMs;
        g_lastSwingMs = nowMs;
        ++g_shotSeq;
        ++g_v4.meleeSwings;
        {
            // R4 T3: ONE line per counted swing of ours (the first 30, then one per 10 s) that says whether there was a puppet to hit, how far
            // it is from us and how far it is from where its owner's pose puts it (0.4.9: 11 swings, "windows opened on a puppet 0", no line said why).
            static double lastLineMs = -1e9;
            ++g_ownSwingNo;
            if (g_ownSwingNo <= 30 || nowMs - lastLineMs >= 10000.0)
            {
                lastLineMs = nowMs;
                sync::OwnSwingIn oi;
                oi.index = g_ownSwingNo;
                oi.sinceLastMs = sinceLastSwing;
                float lx = 0, ly = 0, lz = 0;
                const bool haveLocal = g_world->GetEntityCoords(lp, lx, ly, lz);
                float bestD2 = 1e30f;
                for (const auto& kv : g_peers)
                {
                    const Peer& pe = kv.second;
                    ++oi.peers;
                    if (!pe.ped)
                        continue;
                    ++oi.spawned;
                    if (pe.hpPool && !pe.deadShown)
                        ++oi.hittable;
                    float px, py, pz;
                    if (!haveLocal || !g_world->GetEntityCoords(pe.ped, px, py, pz))
                        continue;
                    const float d2 = (px - lx) * (px - lx) + (py - ly) * (py - ly) + (pz - lz) * (pz - lz);
                    if (d2 < bestD2)
                    {
                        bestD2 = d2;
                        oi.nearestId = kv.first;
                        oi.nearestM = std::sqrt(d2);
                        oi.poseErrM = -1.0f;
                        oi.poseDistM = -1.0f;
                        if (pe.poseKnown)
                        {
                            oi.poseErrM = std::sqrt((px - pe.poseX) * (px - pe.poseX) + (py - pe.poseY) * (py - pe.poseY) + (pz - pe.poseZ) * (pz - pe.poseZ));
                            oi.poseDistM = std::sqrt((pe.poseX - lx) * (pe.poseX - lx) + (pe.poseY - ly) * (pe.poseY - ly) + (pe.poseZ - lz) * (pe.poseZ - lz));
                        }
                    }
                }
                GLOGI("%s", sync::OwnSwingLine(oi).c_str());
            }
        }
        static int logged = 0;
        if (logged < 6)
        {
            ++logged;
            GLOGI("engine: melee: swing counted (shotSeq %u; control press %d, action edge %d; control ids 140/141/142 "
                  "are INFERRED)", (unsigned)(g_shotSeq & 15), control, action);
        }
    }

    // Our on-foot state for the wire (protocol v3 PedState). Script tick only, once per send (the
    // send rate, not every frame). A flag that lasts less than a send interval can be missed: the
    // jump is carried by jumpSeq, counted on IsPedJumping's rising edge; IS_PED_SHOOTING ("fired
    // this frame") is sampled, so a single shot between two sends is not seen (log-only remotely).
    // Not sent, for want of a native in the 3889 crossmap: entering/exiting a vehicle (move 5/6),
    // reloading, cover, ladder, stealth. (v4: kPfDriver from the seat the link reads, shotSeq per frame.)
    uint8_t g_jumpSeq = 0;      // script tick only
    bool    g_wasJumping = false;
    // The server id of a networked vehicle by its local handle; -1 when it is not one (an ambient car, 0).
    // The game thread's own map (g_vehs): at most kMaxLocalVehicles entries.
    int NetIdOfVehicle(int32_t handle)
    {
        if (!handle)
            return -1;
        for (const auto& kv : g_vehs)
            if (kv.second.handle == handle)
                return (int)kv.first;
        return -1;
    }

    proto::PedState ReadLocalPedState(int32_t lp, float px, float py, float pz, float heading, bool inVehicle,
                                      double nowMs)
    {
        proto::PedState s;
        s.shotSeq = (uint8_t)(g_shotSeq & 15);         // v4: counted every frame (GameWork)
        if (inVehicle)
        {
            s.move = proto::kMoveInVehicle;
            if (g_localSeat == 0)
                s.flags |= proto::kPfDriver;
        }
        else
        {
            const int gait = g_world->PedGait(lp);
            s.move = (uint8_t)(gait >= 0 && gait <= 3 ? gait : 0);
        }
        const bool jumping = !inVehicle && g_world->IsPedJumping(lp);
        if (jumping && !g_wasJumping)
            ++g_jumpSeq;                               // only the low 2 bits travel: a plain ++ wraps
        g_wasJumping = jumping;
        s.jumpSeq = g_jumpSeq;
        if (jumping)                                  s.flags |= proto::kPfJumping;
        if (g_world->IsPedRagdoll(lp))                s.flags |= proto::kPfRagdoll;
        if (g_world->IsPedShooting(lp))               s.flags |= proto::kPfShooting;
        if (g_world->IsPedFalling(lp))                s.flags |= proto::kPfFalling;
        if (g_world->IsPedInParachuteFreeFall(lp))    s.flags |= proto::kPfParachute;
        if (g_world->IsPedClimbing(lp))               s.flags |= proto::kPfClimbing;
        if (g_world->IsPedVaulting(lp))               s.flags |= proto::kPfVaulting;   // 0.4.3: the vault / slide is its own bit
        if (g_world->IsPedDeadOrDying(lp))            s.flags |= proto::kPfDead;
        s.weapon = g_world->GetSelectedPedWeapon(lp);  // 0 when it cannot be called = "not reported"
        // Tier B on-foot state (protocol v5): swimming, a melee action, reloading, stealth, cover. A native that
        // cannot be called reads false. In a vehicle none of them applies.
        if (!inVehicle)
        {
            if (g_world->IsPedSwimming(lp))                    s.flags |= proto::kPfSwimming;
            if (g_world->IsPedPerformingMeleeAction(lp))       s.flags |= proto::kPfMelee;
            if (g_world->IsPedReloading(lp))                   s.flags |= proto::kPfReloading;
            if (g_world->GetPedStealthMovement(lp))            s.flags |= proto::kPfStealth;
            // R3 T3: cover, classified (libs/sync/cover.h CoverSense): IS_PED_IN_COVER / IS_PED_GOING_INTO_COVER / IS_PED_AIMING_FROM_COVER.
            // kPfCover for in-cover and aiming-from-cover (and a 150 ms hold over a read gap); going-into sends nothing (the ped is still running
            // to the cover: the others keep following its pose). A low head in cover is the crouch hint, sent as kPfStealth ("crouched").
            sync::CoverReads cr;
            cr.inCover = g_world->IsPedInCover(lp, false);
            cr.goingInto = g_world->IsPedGoingIntoCover(lp);
            cr.aimingFrom = g_world->IsPedAimingFromCover(lp);
            cr.x = px; cr.y = py; cr.z = pz; cr.heading = heading;
            if (cr.inCover || cr.aimingFrom)
            {
                float hx = 0, hy = 0, hz = 0;
                if (g_world->GetPedBoneCoords(lp, 31086, 0.0f, 0.0f, 0.0f, hx, hy, hz) && std::isfinite(hz))   // SKEL_Head
                    cr.headAboveM = hz - pz;
            }
            const sync::CoverSenderOut co = g_coverSense.Update(cr, nowMs);
            if (co.sendCover)                                  s.flags |= proto::kPfCover;
            if (co.sendCrouch)                                 s.flags |= proto::kPfStealth;
        }
        else if (g_coverSense.Up())
            g_coverSense.Update(sync::CoverReads(), nowMs);    // got into a vehicle: the hold ends
        // Entering (move 5) / exiting (move 6) a NETWORKED car, with its server id, so the others can play the
        // door walk instead of seeing the ped appear in the seat. Not while seated (move 4 stands).
        {
            sync::VehicleMoveIn mv;
            mv.seated = inVehicle;
            if (!inVehicle)
            {
                mv.tryingNetId = NetIdOfVehicle(g_world->GetVehiclePedIsTryingToEnter(lp));
                mv.walkNetId = g_entryWalk.active ? g_entryWalk.netId : -1;
                mv.exitNetId = g_exitNetId;
                mv.sinceExitMs = nowMs - g_exitAtMs;
            }
            const sync::VehicleMoveOut mo = sync::DecideVehicleMove(mv);
            if (mo.use)
            {
                s.move = mo.move;
                s.vehId = mo.vehId;
            }
        }
        // The aim point while free-aiming, and for 500 ms after a shot (so remote bullets fly where
        // ours did, hip fire included): the camera ray's hit (or its 200 m end) from
        // RaycastFromCamera. Fallback when the ray cannot be cast: 10 m ahead along the heading at
        // chest height (no pitch). GTA heading 0 = +Y (north), counter-clockwise: forward = (-sin h, cos h).
        // From a vehicle too (a drive-by): without the point the other machines fire the shot along the
        // ped's heading. The ray then ignores the vehicle, whose body the camera sits behind.
        if (sync::AimPointDue(g_world->IsLocalPlayerFreeAiming(), nowMs, g_lastShotMs))
        {
            s.flags |= proto::kPfAiming;
            bool hit = false;
            float ax = 0, ay = 0, az = 0;
            int32_t ent = 0;
            int32_t ignore = lp;
            if (inVehicle)
                if (const int32_t veh = g_world->GetVehiclePedIsIn(lp))
                    ignore = veh;
            if (g_world->RaycastFromCamera(200.0f, ignore, hit, ax, ay, az, ent) && std::isfinite(ax) &&
                std::isfinite(ay) && std::isfinite(az))
            {
                s.aimX = ax - px;
                s.aimY = ay - py;
                s.aimZ = az - pz;
            }
            else
            {
                const float r = heading * 3.14159265f / 180.0f;
                s.aimX = -std::sin(r) * 10.0f;
                s.aimY = std::cos(r) * 10.0f;
                s.aimZ = 0.5f;
            }
        }
        return s;
    }

    // The game work: read and send the local player, keep the remote players'
    // peds in step. inScript = running inside GTA's script tick (the context that
    // creates and moves entities safely); false = the render-thread fallback. probe (script tick
    // only, else null) receives the slowest timed part for the frame-time probe.
    void GameWork(bool inScript, WorkProbe* probe)
    {
        // Before anything else, including the "no player yet" return: the menu and character-switch
        // controls stay off through the whole load. (spawn=0 is the patch-nothing diagnostic mode.)
        if (inScript && g_world->spawnMode != 0)
        {
            WorkSection s(probe, kSecMenuControls);
            HoldMenuControls();
            TickPauseMenu(NowMs());
        }

        int32_t lp = g_world->LocalPlayer();
        TickAmbient(inScript, lp, probe);              // T5: no ambient NPCs / traffic, every tick, before the connection / entry gates
        if (inScript && g_world->spawnMode != 0)
            TickInvPed(lp);                            // 0.5.8 preview / 0.5.4 camera: before every early return below, so a closed page always restores them
        if (!lp) {
            // Player not spawned yet (the overlay's own loader is up). Heartbeat so a stuck load is diagnosable from gage.log: PLAYER_PED_ID stays 0 while GTA
            // is still loading its world, or if our script-tick context never goes live (then GameWork
            // only runs on the render thread, where the native has no script-thread TLS and returns 0).
            static std::atomic<ULONGLONG> firstNoPed{0}, lastNoPedLog{0};
            const ULONGLONG now = GetTickCount64();
            if (!firstNoPed.load()) firstNoPed.store(now);
            if (now - lastNoPedLog.load() >= 3000) {
                lastNoPedLog.store(now);
                GLOGI("engine: waiting for local player ped (PLAYER_PED_ID=0 via %s; script tick %s; %llu s)",
                      inScript ? "script tick" : "render thread",
                      g_world->ScriptTickActive() ? "live" : "not running",
                      (unsigned long long)((now - firstNoPed.load()) / 1000));
            }
            return;
        }

        // First frame with a valid player ped: go live.
        {
            static bool loggedSpawn = false;
            if (!loggedSpawn) {
                loggedSpawn = true;
                GLOGI("engine: player ped spawned — engine going live (%s)",
                      inScript ? "script tick" : "render thread");
            }
        }

        // The player exists: start the loader's give-up clock (first sight per loading episode) and
        // serve the in-game UI. This runs before the entry gating below so that typing in the chat
        // always gets an answer, even while the entry is still waiting.
        if (!g_loaderPedAt.load())
            g_loaderPedAt.store(GetTickCount64());
        if (inScript)
        {
            WorkSection s(probe, kSecUiTick);
            UiTick(lp);
        }

        // Changing game state: inside the script tick unless spawn=0; on the render
        // thread only with spawn=1.
        const int mode = g_world->spawnMode;
        const bool mayChange = inScript ? mode != 0 : mode == 1;
        if (inScript && mayChange)
            TickAiDamage(NowMs());                     // W8 item 6: the AI damage modifiers follow the session (also after it ends)
        if (inScript)
            TickClipTraces(NowMs());                   // 0.4.13: follows the issued clips / ragdolls / exits frame by frame (reads only)
        if (inScript && mayChange)
            TickHyperTune(lp);                         // the hyper cars (libs/sync/hypertune.h): power every frame, top speed once
        if (inScript && mayChange)
            TickShakeMeter(lp, NowMs());               // 0.4.22: reads the car we drive (pitch / roll / vertical-speed jitter) for the 10 s shake line
        if (inScript && mayChange)
            TickRadioOff(lp, NowMs());                 // 0.4.19: no vehicle radio, no radio wheel / its slow-mo (libs/sync/radio_off.h)
        if (inScript && mayChange)
            TickMinimap(NowMs());                      // 0.4.22: GTA's radar after the entry + its bars hidden + the vitals dock (libs/sync/minimap.h)
        if (inScript && mayChange)
            game::worldclock::Tick(*g_world);          // 0.5.7: the server's time of day and weather held (or released) (world_clock.h)
        if (inScript && mayChange && lp) {
            // Auto-mount the add-on car packs through the game's own DLC loader, if armed (pack_mount=1; default off),
            // and hand the Tab menu's car browser the list whenever it (or the loading state) changes.
            const game::content::Status packs = game::content::Tick(*g_world);
            g_packStatus.store((int)packs);            // 0.4.19: the spawn reveal waits for the packs to settle
            PushCarList(packs);
        }
        // The session ended (OnEnded marked every peer gone and every server vehicle removed): the entry gate
        // below returns before TickPeers / TickVehicles run, so the remote peds and server vehicles would stay
        // frozen in the world until a later session finished its entry (W4 review, fix 1). Run the cleanup pass
        // here: SyncNet, delete what is gone / removed, CommitNet. With no live smoother (g_syncLive is false
        // until the next Welcome) neither function spawns or drives anything, it only tears down.
        if (inScript && !join::IsConnected() && (!g_peers.empty() || !g_vehs.empty()))
        {
            WorkSection s(probe, kSecPeers);
            const int32_t cleanVeh = g_world->GetVehiclePedIsIn(lp);
            TickPeers(lp, cleanVeh);
            TickVehicles(lp, cleanVeh);
        }
        if (mayChange && inScript)
        {
            WorkSection s(probe, kSecEntry);
            if (!EnterServer())
                return;                                // entering: model streaming
        }

        float px = 0, py = 0, pz = 0, heading = 0;
        {
            WorkSection s(probe, kSecReadSend);
            lp = g_world->LocalPlayer();               // the entry swaps the ped
            if (!lp)
                return;
            if (!g_world->GetEntityCoords(lp, px, py, pz))
                return;                                // native invocation failed
            heading = g_world->GetEntityHeading(lp);
        }
        g_localPed.store(lp);
        g_localX.store(px);
        g_localY.store(py);
        g_localZ.store(pz);
        g_localHeading.store(heading);
        {
            static bool loggedPos = false;
            if (!loggedPos) {
                loggedPos = true;
                GLOGI("engine: local player ped %d at %.1f, %.1f, %.1f heading %.0f; sending state",
                      lp, px, py, pz, heading);
            }
        }

        // -- send local state at 1.5x the server tick rate (33 ms at 20 Hz, 22 ms = 45 Hz at the default 30 Hz) ------
        // Why faster than the tick: the server forwards the newest sample it holds at each tick, so a
        // sender paced at exactly the tick spacing beats against it (frame quantisation + network
        // jitter put two samples into one tick and none into the next: a repeat the receivers'
        // smoother has to bridge, and more lateness to cover with delay). 1.5x guarantees a fresh
        // sample per tick, and each carries its own placement (WorldEntry::ageMs), so the uneven
        // spacing costs nothing. Scheduled on the QPC clock (not "now - last >= N" on GetTickCount's
        // 15.6 ms steps, which gave ~47 ms gaps): the next send is due one interval after the last
        // due time, so the average rate holds even when frames quantise each gap.
        ULONG now = GetTickCount();
        const double nowMs = NowMs();
        // v4, every script-tick frame: our vehicle and our shots (IS_PED_SHOOTING is "fired this
        // frame", so it is counted here, not at the send rate; only the low 4 bits travel).
        int32_t lpVeh = 0;
        if (inScript)
        {
            if (g_world->IsPedInVehicle(lp))
                lpVeh = g_world->GetVehiclePedIsIn(lp);
            if (g_world->IsPedShooting(lp))
            {
                ++g_shotSeq;
                g_lastShotMs = nowMs;
                ++g_v4.shots;
                NoteShotTarget();                      // W9 item 8: at a seated puppet, a puppet's car, or one on foot
            }
            TickLocalMelee(lp, lpVeh != 0, nowMs);     // W3: a punch or a swing steps shotSeq too
        }
        if (nowMs >= g_nextSendMs.load())
        {
            WorkSection s(probe, kSecReadSend);
            const uint16_t hz = g_tickHz.load();
            double intervalMs = (hz ? 1000.0 / hz : 50.0) / 1.5;
            if (intervalMs < 16.0)
                intervalMs = 16.0;                     // a server above 40 Hz: at most ~60 states/s
            const double due = g_nextSendMs.load() + intervalMs;
            g_nextSendMs.store(due > nowMs ? due : nowMs + intervalMs); // behind (a hitch): restart the beat
            proto::Kinematics k;
            k.x = px; k.y = py; k.z = pz;
            k.heading = heading;
            // The game's own velocity (the vehicle's while seated: a seated ped's own may read 0).
            // Fallback when the native cannot be called: the position change since the last send.
            // Either way a speed past the protocol's cap is a teleport (respawn, warp), not
            // movement: send 0.
            const bool inVehicle = g_world->IsPedInVehicle(lp);
            int32_t velEntity = lp;
            if (inVehicle)
                if (const int32_t veh = g_world->GetVehiclePedIsIn(lp))
                    velEntity = veh;
            bool haveVel = g_world->GetEntityVelocity(velEntity, k.vx, k.vy, k.vz);
            if (!haveVel)
            {
                const double dtMs = nowMs - g_lastPosMs;
                if (g_haveLastPos && dtMs > 0.5 && dtMs < 1000.0)
                {
                    const float dt = (float)(dtMs / 1000.0);
                    k.vx = (px - g_lastPos[0]) / dt;
                    k.vy = (py - g_lastPos[1]) / dt;
                    k.vz = (pz - g_lastPos[2]) / dt;
                    haveVel = true;
                }
            }
            if (!haveVel || !(std::fabs(k.vx) <= proto::kMaxSpeed && std::fabs(k.vy) <= proto::kMaxSpeed &&
                              std::fabs(k.vz) <= proto::kMaxSpeed))
                k.vx = k.vy = k.vz = 0;
            g_lastPos[0] = px; g_lastPos[1] = py; g_lastPos[2] = pz;
            g_lastPosMs = nowMs;
            g_haveLastPos = true;
            // The on-foot state (v3) is read only on the script tick, where these natives are valid. W8 item 8: when GTA's
            // script tick is not running (the window lost focus) the render-thread fallback sends the LAST REAL state with
            // what cannot still be true cleared (sync::FallbackPedState: idle, no transient flags, the weapon, the vehicle
            // seat, the death / stealth / cover / swimming / ragdoll / parachute state kept) and a zero velocity, not the
            // default "standing, no weapon, no vehicle" that made the others see the player drop the gun and leave the car.
            proto::PedState ped;
            if (inScript)
            {
                ped = ReadLocalPedState(lp, px, py, pz, heading, inVehicle, nowMs);
                std::lock_guard<std::mutex> lk(g_realPedMtx);
                g_realPed = ped;
                g_haveRealPed = true;
            }
            else
            {
                {
                    std::lock_guard<std::mutex> lk(g_realPedMtx);
                    ped = sync::FallbackPedState(g_realPed, g_haveRealPed);
                }
                k.vx = k.vy = k.vz = 0;
                g_fallbackStates.fetch_add(1, std::memory_order_relaxed);
            }
            // The sample time (protocol v5): this frame's time on the engine's monotonic clock, the instant the state
            // above was read (not the moment the join worker gets to send it). Whole ms, the low 16 bits travel.
            const int32_t sampleMs = (int32_t)((uint64_t)nowMs & 0xFFFFu);
            join::SendState(k, ped, sampleMs);
            if (inScript)
                SendControlledVehicles(nowMs, sampleMs, lpVeh);   // v5: every car the server says we control, not only ours
        }

        if (!mayChange)
            return;

        // -- hold the character wheel, HUD and autosave suppressed every tick ---
        // (only from inside the script tick, where we own the player as main_persistent).
        if (inScript)
        {
            // With stock scripts off (no_sp_scripts=1, GtaThread::Tick a no-op) no SP script executes:
            // nothing can relaunch the Prologue or swap the player, so the per-tick terminate loops
            // below have nothing to do and are skipped. They stay on the fallback path (hook absent).
            const bool scriptsOff = g_world->StockScriptsOff();
            {
                WorkSection s(probe, kSecSpFrame);
                g_world->SuppressSPFrame();
            }
            // (the ambient world is held off from TickAmbient at the top of GameWork, every tick, whatever the connection: T5)
            // For the first 30 s after entry, keep terminating the SP mission-flow scripts
            // (flow_controller + prologue1) every tick: flow_controller relaunches the Prologue a
            // few seconds in (gage.log: prologue1 reappeared as id 68, then teleported the player to
            // North Yankton), so a one-shot kill at entry is not enough. After that the periodic
            // SuppressSPSystems keeps them down. Stock scripts off: once per entry, for its
            // FORCE_GAME_STATE_PLAYING (a code-side state change, not a script kill).
            if (g_entryDoneAt && now - g_entryDoneAt < 30000 && (!scriptsOff || !g_entryBootOnce))
            {
                WorkSection s(probe, kSecEntryBoot);
                g_entryBootOnce = true;
                g_world->SuppressEntryBoot();
            }
            // main relaunches player_controller a few seconds after the entry and it swaps the
            // player to the story character; keep the story-character scripts down from then on.
            if (g_entryDoneAt && g_world->killSpHijack && !scriptsOff)
            {
                WorkSection s(probe, kSecHijackers);
                g_world->SuppressPedHijackers();
            }
            {
                WorkSection s(probe, kSecLife);
                if (g_v4Reset.exchange(false))
                    ResetV4Units();             // a session began or ended
                DrainEvents(nowMs);             // Damage / Killed / RespawnOk / VehicleResult
                TickVitals(lp, lpVeh, nowMs);   // server-owned health; a death is reported this frame
                TickLife(lp, now);              // self-driven respawn on death
            }
            if (g_life == Life::Alive)
            {
                {
                    WorkSection s(probe, kSecPedGuard);
                    if (TickEntryGuard(lp, now))  // GTA swapped the ped back: freemode re-applied
                        lp = g_world->LocalPlayer();
                }
                if (lp)
                {
                    WorkSection s(probe, kSecCommands);
                    RunCommands(lp, now);   // /car /gun /heal /kill /tp /fix /noclip
                }
            }
            {
                WorkSection s(probe, kSecCommands);
                TickNoclip(lp, lpVeh, nowMs);   // 0.4.22: flies, lands and gives the flags back; also runs while dead / not Alive (ends at once)
            }
            if (lp)
            {
                WorkSection s(probe, kSecCommands);
                const bool dead = g_life != Life::Alive || g_world->IsPedDeadOrDying(lp);
                TickLocalEmote(lp, lpVeh != 0, dead, nowMs);
                if (ServerMode())
                {
                    if (!g_noclip.Active())     // F / G do not walk a flying ped to a car
                        TickVehicleEntry(lp, lpVeh, dead, nowMs);   // F / G first; the link sees the new seat next tick
                    TickLocalVehicle(lp, lpVeh, dead, nowMs);
                    TickDoorsSeats(lp, lpVeh, dead, nowMs);     // 0.4.22: the L key, the lock read-back on the copies, the seat read-back of a pick
                }
                TickVehicleControls(lp, lpVeh, dead, nowMs);    // 0.4.22: indicators, engine, belt keys + the strip and the fuel gauge (server or not)
                TickInventoryWorld(lp, dead, nowMs);            // 0.5.4: ped weapons <-> inventory (only once the server offered it)
                TickIndicatorFallback(lp, lpVeh, nowMs);        // 0.4.27: our own blinking light on cars whose indicators the game does not show
            }
            if (lp)
            {
                WorkSection s(probe, kSecHud);
                DrawHud(lp, now);           // health + satiety to the overlay (hidden unless alive)
            }
            // Our look, once the entry has put the final model and outfit on (every 2 s; the session
            // sends only a change).
            if (lp && g_life == Life::Alive && g_entry.load() == Entry::Done)
            {
                WorkSection s(probe, kSecReadSend);
                game::characters::TickLocal(*g_world, lp, g_world->GetEntityModel(lp));   // 0.5.9: our active character's face (on a change only)
                TickOutfitItems(lp, now);   // 0.5.4 outfit_items: the worn items' clothes first, so the look sent below already has them
                PublishLook(lp, now);
            }
        }

        // -- suppress singleplayer ambient systems, throttled to ~1/5s -----
        {
            static ULONG lastSuppress = 0;
            if (now - lastSuppress >= 5000) {
                lastSuppress = now;
                {
                    WorkSection s(probe, kSecSpSystems);
                    g_world->SuppressSPSystems();
                    if (inScript)
                        g_world->DisableWanted();
                }
                // Default shop/property/location blips; ours come later. ~1000 natives a call. Those
                // blips are script-made: with stock scripts off none is ever created (gage.log: "found
                // 0 blip(s)" at entry), so only the entry's one-shot runs. 0.4.20: only with map_clear_blips=0;
                // otherwise TickMap's sweep (below) hides them whatever the stock-script state.
                if (inScript && !g_mapClearBlips.load() && !g_world->StockScriptsOff())
                {
                    WorkSection s(probe, kSecBlips);
                    g_world->ClearSpBlips();
                }
            }
        }

        // 0.4.20 map clean-up: the blip sweep + fog reveal every 3 s after the entry, its 10 s line (sync/mapclear.h).
        if (inScript && g_entry.load() == Entry::Done)
        {
            WorkSection s(probe, kSecBlips);
            TickMap(now);
        }

        // One-shot ~8 s after entry: log the live scripts again so we can see which singleplayer
        // story/ambient scripts survived suppression (diagnostic for the campaign voice/text bleed).
        if (inScript && g_entryDoneAt)
        {
            static bool dumpedPost = false;
            if (!dumpedPost && now - g_entryDoneAt >= 8000)
            {
                WorkSection s(probe, kSecDumpScripts);
                dumpedPost = true;
                g_world->DumpLiveScripts("after-suppress");
            }
        }

        // -- companion demo ped (render-thread spawn=1 test only: on a server the
        //    remote players are the proof) ------------------------------------
        if (!inScript)
        {
            if (!g_companion && now - g_lastSpawnTried.load() >= 1000)
            {
                g_lastSpawnTried.store(now);
                g_companion = g_world->SpawnPed(kCompanionModel, px - 2.0f, py, pz + 0.1f);
                if (g_companion)
                    GLOGI("engine: companion ped spawned (model 0x%08x)", (unsigned)kCompanionModel);
            }
            if (g_companion)
            {
                g_world->SetEntityCoords(g_companion, px - 2.0f, py, pz + 0.1f);
                g_world->SetEntityHeading(g_companion, heading);
            }
        }

        // -- update / spawn / remove remote peers (script tick only: the peds are created, moved
        //    and later tasked there; the render-thread fallback leaves them alone) -------------
        if (inScript)
        {
            WorkSection peersSection(probe, kSecPeers);    // the two short lock windows (SyncNet, CommitNet) and the natives
            TickPeers(lp, lpVeh);
            TickVehicles(lp, lpVeh);                   // after the peds: seating needs them spawned
            TickNameTags(lp, nowMs);                   // W10: the names above the heads, from the poses this frame placed
            ReportV4(nowMs);
        }
    }

    // The per-frame driver (the Present detour body, render thread).
    void Frame()
    {
        // Also feeds the frame-time probe: the Present interval on entry, our own time on exit.
        struct FrameMark
        {
            int64_t t0 = QpcNow();
            int64_t ui = 0;                            // QPC ticks spent in UiFrame this Present
            FrameMark() { g_frameEnteredAt.store(GetTickCount64()); ++g_frames; NotePresent(t0); }
            ~FrameMark() { g_frameEnteredAt.store(0); NoteFrameDone(t0, ui); }
        } mark;

        if (!g_active || !g_world)
            return;

        EnsureConnected();

        // The in-game UI is drawn from every present, whichever thread runs the game work.
        {
            const int64_t u0 = QpcNow();
            UiFrame();
            mark.ui = QpcNow() - u0;
        }

        // GTA's script tick drives the game work once it runs.
        if (g_world->ScriptTickActive())
            return;

        // Present fires tens of thousands of times a second while the game loads:
        // run the native work at most ~60 times a second.
        static ULONGLONG lastWork = 0;
        const ULONGLONG workNow = GetTickCount64();
        if (workNow - lastWork < 16)
            return;
        lastWork = workNow;
        GameWork(false, nullptr);
    }

    // Once per game frame inside GTA's script update, as main_persistent.
    void ScriptTick()
    {
        if (!g_active || !g_world)
            return;
        // Only this tick touches the game thread's peer and vehicle maps (the render-thread fallback leaves them
        // alone); Stop() clears them from another thread, so it waits for a tick in flight and vice versa.
        std::lock_guard<std::mutex> sk(g_scriptMtx);
        if (!g_active)
            return;                                    // Stop() won the race
        const int64_t t0 = QpcNow();
        WorkProbe probe;
        GameWork(true, &probe);
        NoteScriptTick(t0, QpcNow(), probe);
    }

    // Logs every 10 s how many frames the game ran (none = Present is not being
    // called, or passive mode) with the frame-time probe's summary, once per episode
    // when the game thread has been inside Frame() for 2 s or more, and the worst
    // hitch over 100 ms at most every kHitchLogEveryMs.
    void StartWatchdog()
    {
        if (g_watchdogStarted.exchange(true))
            return;
        std::thread([] {
            uint64_t lastFrames = 0;
            ULONGLONG lastBeat = GetTickCount64();
            int64_t lastBeatQpc = QpcNow();
            bool stuckLogged = false;
            for (;;)
            {
                Sleep(500);
                const ULONGLONG now = GetTickCount64();
                LogHitch(now);
                const ULONGLONG entered = g_frameEnteredAt.load();
                if (entered && now - entered >= 2000)
                {
                    if (!stuckLogged)
                        GLOGW("engine: game thread stuck inside Frame() for %llu ms",
                              (unsigned long long)(now - entered));
                    stuckLogged = true;
                }
                else
                    stuckLogged = false;
                if (now - lastBeat >= 10000)
                {
                    const int64_t beatQpc = QpcNow();
                    char stats[640];
                    TakeFrameStats(stats, sizeof(stats), QpcUs(beatQpc - lastBeatQpc) / 1e6);
                    lastBeatQpc = beatQpc;
                    const uint64_t f = g_frames.load();
                    if (f == lastFrames)
                        GLOGW("engine: no frame in the last 10 s (%llu in total): Present is not being called%s%s",
                              (unsigned long long)f, g_world && g_world->passive ? " (passive mode)" : "", stats);
                    else if (const int32_t ped = g_localPed.load())
                        GLOGI("engine: game thread alive, %llu frames in the last 10 s; local ped %d at "
                              "%.1f, %.1f, %.1f heading %.0f%s", (unsigned long long)(f - lastFrames), ped,
                              g_localX.load(), g_localY.load(), g_localZ.load(), g_localHeading.load(), stats);
                    else
                        GLOGI("engine: game thread alive, %llu frames in the last 10 s%s",
                              (unsigned long long)(f - lastFrames), stats);
                    lastFrames = f;
                    lastBeat = now;
                }
            }
        }).detach();
    }

    // Alt+F4 / window close (overlay WndProc, the window's thread): leave the server and end the
    // process at once, as FiveM does, instead of GTA's pausing "quit game?" screen. The leave is
    // join::Disconnect (an ENet disconnect, sent within a poll of ~10 ms; it then waits for the
    // server's ack up to SessionOptions::closeTimeoutMs = 1500 ms), run on a helper thread and
    // waited for at most kQuitLeaveMs: the server gets our disconnect either way, so no ghost
    // player stays behind. Once only: a second Alt+F4 while this runs is swallowed.
    constexpr DWORD kQuitLeaveMs = 1000;
    game::quit::OnceGate g_quitGate;

    void OnQuitRequest()
    {
        if (!g_quitGate.First())
            return;
        GLOGI("quit: Alt+F4/close -> leaving server and exiting");
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::thread([done] {
            join::Disconnect();
            if (done)
                SetEvent(done);
        }).detach();
        const DWORD w = done ? WaitForSingleObject(done, kQuitLeaveMs) : WAIT_TIMEOUT;
        GLOGI("quit: %s; terminating the game", w == WAIT_OBJECT_0
              ? "left the server" : "leave not confirmed within 1 s (our disconnect was sent)");
        TerminateProcess(GetCurrentProcess(), 0);
    }

}

namespace gage::engine {

    void Start(game::World* world, const join::Config& cfg)
    {
        if (!world)
            return;
        g_world = world;
        g_cfg = cfg;
        g_giveUp.store(false);
        // v8: with no server every command runs locally (single player); on a server the AccountInfo decides.
        game::Overlay::SetIsAdmin(cfg.host.empty());
        join::PeerSinks sinks;
        sinks.onWelcome = OnWelcome;
        sinks.onJoined = OnJoined;
        sinks.onLeft = OnLeft;
        sinks.onWorld = OnWorld;
        // onCoarse stays unset: far players' coarse positions had a store nothing read (no map blips are drawn); 0.4.32 removed it
        sinks.onLook = OnLook;
        sinks.onChat = OnChat;
        sinks.onEnded = OnEnded;
        sinks.onVehicles = OnVehicles;                 // protocol v4
        sinks.onVehicleResult = OnVehicleResult;
        sinks.onSeatNotice = OnSeatNotice;
        sinks.onVehicleImpact = OnVehicleImpact;   // T4
        sinks.onLinkInfo = OnLinkInfo;             // R3 T5
        sinks.onAccount = OnAccount;               // v8
        sinks.onAdminResult = OnAdminResult;       // v8
        sinks.onInvState = OnInvState;             // 0.6 inventory
        sinks.onInvUsed = OnInvUsed;
        sinks.onInvSnap = OnInvSnap;               // 0.5.4 inventory world
        sinks.onInvNearby = OnInvNearby;
        sinks.onWorldClock = game::worldclock::OnClock;   // 0.5.7 world clock
        sinks.onCharRoster = game::characters::OnRoster;  // 0.5.9 characters
        sinks.onCharResult = game::characters::OnResult;
        sinks.onPeerFace = OnPeerFace;
        sinks.onDamage = OnDamage;
        sinks.onKilled = OnKilled;
        sinks.onRespawnOk = OnRespawnOk;
        sinks.onPeerAnim = OnPeerAnim;
        sinks.onPeerScenario = OnPeerScenario;
        sinks.onPeerAnimStop = OnPeerAnimStop;
        sinks.beatNote = PeersLockNote;                // the 10 s join line also says how long the peer lock made anyone wait
        join::SetPeerSinks(std::move(sinks));
        game::Overlay::SetQuitHandler(OnQuitRequest);  // Alt+F4 / close: leave the server, exit at once
        // The script tick takes over the game work once the native poller installs it.
        world->HookScriptTick(ScriptTick);
        if (world->HookTick(Frame))
            GLOGI("engine: tick hook installed; per-frame driver active");
        else
            GLOGW("engine: tick hook NOT installed (main loop unresolved); engine idle until resolved");
        g_active = true;
        StartWatchdog();
        EnsureConnected();
        GLOGI("engine: target server %s:%d player=%s",
            Narrow(cfg.host).c_str(), cfg.port, Narrow(cfg.player).c_str());
    }

    void Stop()
    {
        game::Overlay::SetQuitHandler(nullptr);        // Alt+F4 / close back to the game's own handling
        g_active = false;
        if (g_world)
            g_world->Unhook();
        game::Overlay::Shutdown(); // after the unhook: no Present detour runs a frame any more
        game::brand::Shutdown();   // same reason: puts the two D3D11 vtable slots back, drops the branded textures
        join::Disconnect(); // the session's worker is gone after this: no more peer events
        if (g_companion && g_world)
            g_world->DeleteEntity(g_companion);
        g_companion = 0;
        {
            // The game thread's maps are cleared with the game thread's work held off (a tick in flight finishes
            // first); the join worker is gone by now (Disconnect above), the lock is for the net maps.
            std::lock_guard<std::mutex> sk(g_scriptMtx);
            for (auto& kv : g_peers)
            {
                DeleteAnchor(kv.second);
                if (kv.second.ped && g_world)
                    g_world->DeleteEntity(kv.second.ped);
            }
            g_peers.clear();
            for (auto& kv : g_vehs)
            {
                // Not the one we sit in (it is our car either way; deleting it under us is unsafe).
                if (kv.second.handle && g_world && kv.second.handle != g_link.Vehicle())
                    g_world->DeleteEntity(kv.second.handle);
                if (kv.second.requestedModel && g_world)
                    g_world->ModelNoLongerNeeded(kv.second.requestedModel);
            }
            g_vehs.clear();
            PeersLock lk(g_peersMtx, true);
            g_peerNet.clear();
            g_vehNet.clear();
            g_sync.Clear();
            g_syncLive = false;
        }
    }

    void SendChat(const char* text)
    {
        if (!text)
            return;
        // Client-side commands (/car /gun /heal /kill /tp /fix) run locally on the
        // script tick and are not sent to the server; anything else is chat.
        if (text[0] == '/' && QueueCommand(text))
            return;
        const bool ok = join::SendChat(text);
        GLOGI("engine: chat %s: %s", ok ? "sent" : "dropped (not connected, or empty, too long or invalid)", text);
    }

}
