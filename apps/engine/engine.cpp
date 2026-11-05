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
#include "loading_screen.h"
#include "overlay.h"
#include "brand_apparel.h"
#include "pack_stream.h"
#include "pack_meta.h"
#include "quit_gate.h"
#include "ambient.h"
#include "common/common.h"
#include "common/logger.h"
#include "sync/smoother.h"
#include "sync/locomotion.h"
#include "sync/oneshot.h"
#include "sync/vehicle_drive.h"
#include "sync/vehicle_link.h"
#include "sync/vehicle_body.h"
#include "sync/vehicle_visual.h"
#include "sync/vehicle_damage.h"
#include "sync/pedstate.h"
#include "sync/cover.h"
#include "sync/vehicle_entry.h"
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
#include "sync/pausemenu.h"
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
    sync::LevelWitness g_locoDegrade;     // script tick only
    std::atomic<bool> g_locoDegradeReset{false};   // OnWelcome (join worker) asks, the next DrivePeer clears
    sync::LocoConfig LocoCfg()
    {
        sync::LocoConfig c = g_locoBlend.load() ? sync::ShippedLocoConfig(sync::LocoMode::Blend) : sync::ShippedLocoConfig(g_locoAnchor.load());
        if (!g_locoBlend.load() && !g_locoPushNudge.load())
            c.push = sync::PushMode::Velocity;
        c.selfCheck = g_locoSelfCheck.load();
        if (!g_locoR3.load())
            c = sync::Round2Behaviour(c);
        if (!g_locoRepair.load())
            c.maxRepairs = 0;
        c.blendGaitClip = g_locoGaitClip.load();
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
        sync::SiteTally<kLocoSites> hardSites, softSites;
        sync::WaterEdge water;
        sync::ClimbWindow climb;
        sync::OneShotAudit audit;   // T2: did the game SHOW the jump / reload / stealth / swim / ragdoll we asked for (script tick)
        uint32_t levelLogged = 0;   // T2: "engine: loco level" lines written for this puppet (rate limit)
        // R3 T2: the creation recipe of the puppet's ped (0 full, 1 lean, 2 alt: World::SpawnRemotePed), the repairs done on it
        // (LocoInput::repairsDone), the gait diagnostic of the last 10 s ("engine: gait") and its log-line budgets. Script tick only.
        int recipe = 0;
        int repairsDone = 0;
        sync::GaitDiag gaitDiag;
        uint32_t verdictLogged = 0;
        bool capLogged = false;

        // Stage 3 (protocol v3). Appearance: the newest PeerLook (PeerNet::look, copied by SyncNet, applied
        // by the script tick); lookDirty until it is on the ped. pedModel is the model the ped was spawned with.
        proto::Look look;
        bool hasLook = false, lookDirty = false, lookFailLogged = false;
        bool lookModelFailed = false; // look.model did not stream in: the kCompanionModel stand-in, retried
        double lookFailedAtMs = 0;    // when it failed / the last retry gave up (NowMs)
        uint32_t pedModel = 0;
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
        sync::MeleeEventGate meleeEvents; // 0.4.5: the damage event of THIS puppet is cleared on the first frame of a swing window (stale ones)
        bool infiniteAmmo = false;       // real peer fire: SET_PED_INFINITE_AMMO_CLIP is on this ped
        sync::HitReactArbiter hitReact;  // a cosmetic reaction owns the ped for its window (DrivePeer holds the gait)
        bool reactLifted = false;        // PCF 188 (DisableHurt) and the ragdoll block are lifted for the reaction
        bool reactRagdolled = false;     // ... and the reaction is a knockdown (the end clears the ped's tasks)
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
    struct CoarsePos { float x = 0, y = 0, z = 0; double atMs = 0; };

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
    };

    // g_peersMtx guards g_peerNet, g_vehNet, g_sync (with the connection's one render clock, g_sync.Clock(), which the
    // peds AND the vehicles read: sync plan W7), g_syncLive and g_coarse. Writers: the join
    // worker thread (OnWelcome / OnJoined / OnLeft / OnWorld / OnCoarse / OnEnded and the v4 sinks) and, for a
    // few lines at a time, the script tick (SyncNet, CommitNet, the vehicle interpolators). RemoteSync is
    // single-threaded: every call to it is made under this lock. No native is called while it is held.
    std::mutex g_peersMtx;
    std::map<int, PeerNet> g_peerNet;
    sync::RemoteSync g_sync;
    bool g_syncLive = false;                   // a Welcome configured g_sync for the current session
    std::map<uint16_t, CoarsePos> g_coarse;
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
    };
    std::map<uint16_t, NetVeh> g_vehs;            // script tick only (no lock)
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
    // 0.4.17 passenger ride (GI: "desync is crazy when sitting on the passenger seat"). veh_ride_gate=0: the car we ride in as a passenger counts as a
    // vehicle we control for the R4 T2 contact gate (0.4.15: its distance to itself is 0 m, so every kerb / brake jolt of the copy we sit in opened the
    // 400 ms soft-correction grace and the own-physics adoption: rubber-banding every ~1.2 s). veh_ride_lead=0: the car we ride in gets the near-car
    // lead (0.4.15: up to 450 ms of extrapolation ahead of the driver's last report, with the camera inside it). Default on = both fixes. Re-read at Welcome.
    std::atomic<bool> g_rideGate{true}, g_rideLead{true};
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
    std::atomic<int> g_meleeSurveyWanted{1};
    std::atomic<bool> g_meleeFollowWanted{true};
    sync::SwingSurvey g_swingSurvey;
    uint32_t g_swingNo = 0, g_ownSwingNo = 0;
    int g_swingDurationMs = 400;                       // the duration of a non-survey swing (a survey winner may set it to the clip's length)
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
        enum Kind { Heal, Kill, Tp, Car, Gun, Fix, Emote, Scenario, StopAnim, Swing } kind = Heal;
        float x = 0, y = 0, z = 0;    // /tp
        std::string arg;              // /car, /gun model/weapon name; /e dictionary; /scenario name
        std::string arg2;             // /e clip name
        bool loop = false, upper = false; // /e options
    };
    std::mutex g_cmdMtx;
    std::vector<Command> g_cmdQueue;

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
                if (join::Connect(g_cfg, 4000, sess))   // was 6000; a failed attempt is retried after 1 s
                    GLOGI("engine: joined %s:%d (server '%s', id %u)",
                        Narrow(g_cfg.host).c_str(), g_cfg.port,
                        sess.serverName.c_str(), (unsigned)sess.myId);
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
        g_peerHits.store(reg::Get(L"peer_hits") != L"0");
        g_vehDriveByAimOnly.store(reg::Get(L"veh_driveby") == L"aim");
        g_vehDamageSync.store(reg::Get(L"veh_damage_sync") != L"0");     // R3 T4: the vehicle damage state (default on; 0 = the v6 behaviour)
        g_vehVisualGuard.store(reg::Get(L"veh_visual_guard") != L"0");   // R3 T4: the sanity layer of the tyre / window / handbrake reads (default on)
        g_impactSync.store(reg::Get(L"impact_sync") != L"0");   // T4: the vehicle impact handling (default on; 0 = the W9 contact grace only)
        g_nearClock.store(reg::Get(L"veh_near_clock") != L"0");     // R3 T5: the near-car clock + link lag (0 = round 2's lead)
        g_proxContact.store(reg::Get(L"veh_prox_contact") != L"0"); // R3 T5: the proximity contact path (0 = the flag / speed loss / kick triggers only)
        g_contactGate.store(reg::Get(L"veh_contact_gate") != L"0"); // R4 T2: a copy's contact counts only with a vehicle we control near (0 = any contact, round 3)
        g_accelExtrap.store(reg::Get(L"veh_accel_extrap") != L"0"); // R4 T2: acceleration-aware extrapolation of copies (0 = constant velocity, round 3)
        g_rideGate.store(reg::Get(L"veh_ride_gate") != L"0");       // 0.4.17: a car we only ride in is not "a vehicle we control" (0 = 0.4.15)
        g_rideLead.store(reg::Get(L"veh_ride_lead") != L"0");       // 0.4.17: no near-car lead on the car we ride in (0 = 0.4.15)
        GLOGI("engine: passenger ride (0.4.17): veh_ride_gate=%s veh_ride_lead=%s", g_rideGate.load() ? "on" : "OFF (0.4.15)",
              g_rideLead.load() ? "on" : "OFF (0.4.15)");
        g_peerEnterTask.store(reg::Get(L"puppet_enter") == L"task");   // Sync D: the W6 door walk back (default: follow the pose)
        g_seatIntentOn.store(reg::Get(L"seat_intent") != L"0");        // T1 seats: 0 = the pre-v6 entry (walk + teleport-in; follow + seat warp)
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
                int cycles = 1;
                if (!sv.empty() && sv[0] >= L'0' && sv[0] <= L'9')
                    cycles = sv[0] - L'0';
                g_meleeSurveyWanted.store(cycles > 3 ? 3 : cycles);
                g_meleeFollowWanted.store(reg::Get(L"melee_follow") != L"0");
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
            g_coarse.clear();
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
        g_coarse.erase(id);
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
        g_coarse.clear();
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

    // Far players' coarse positions: kept for map blips only.
    void OnCoarse(const proto::CoarseState& cs)
    {
        const double nowMs = NowMs();
        const uint16_t me = join::MyId();
        PeersLock lk(g_peersMtx, false);
        if (!g_syncLive)
            return;
        for (const proto::CoarseEntry& e : cs.players)
        {
            if (e.id == me || e.id >= proto::kMaxPlayers || !IsJoinedPeer(e.id))
                continue;
            CoarsePos& c = g_coarse[e.id];
            c.x = e.x; c.y = e.y; c.z = e.z;
            c.atMs = nowMs;
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
        }
        for (const auto& kv : g_vehs)
            g_ambExempt.Add(kv.second.handle);         // server vehicles (their occupants are the puppets above)
        g_ambExempt.Add(g_companion);                  // the render-thread test ped
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
        g_world->ClearSpBlips();
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
        const std::string remove = hold.Sync(g_animDicts, wanted ? dict : std::string());
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
            if (g_world->HasCollisionLoadedAround(ped) || el >= kLoadMaxMs)
            {
                g_world->ShutdownLoadingScreen();   // dismiss GTA's loading screen (a no-op on respawn)
                g_world->FreezeEntityPosition(ped, false);
                g_world->SetPlayerControl(true);    // grant control (a harmless re-assert on respawn)
                g_world->DoScreenFadeIn(kFadeInMs);
                g_life = Life::Alive;
                GLOGI("engine: world reveal complete (%s)",
                      el >= kLoadMaxMs ? "collision timeout" : "collision loaded");
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
        else return false;  // unknown: let it go to the server as chat

        std::lock_guard<std::mutex> lk(g_cmdMtx);
        if (g_cmdQueue.size() < 32)   // bound the queue
            g_cmdQueue.push_back(c);
        return true;
    }

    void RunSwingTest(int32_t lp);   // R4 T3, defined with the swing display (PlayPeerSwing)

    // Run any queued commands and advance a pending vehicle spawn. Script tick only.
    // lp is the local ped, now is GetTickCount().
    void RunCommands(int32_t lp, ULONG now) {
        std::vector<Command> cmds;
        {
            std::lock_guard<std::mutex> lk(g_cmdMtx);
            cmds.swap(g_cmdQueue);
        }
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

        // Fuel is server-authoritative and comes in a later phase: no client-side fuel here.
        game::Overlay::SetVitals(hpFrac, satFrac, 0.0f, g_life == Life::Alive, false);
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
    }

    // Script tick only. lp is the local ped.
    // The no-pause pause menu (TickPauseMenu, below). Script tick only.
    sync::PauseMenuGate g_pauseGate;
    bool g_pauseMenuActive = false;       // IS_PAUSE_MENU_ACTIVE as of the last tick (read while connected or open)
    int g_pauseTimerBase = -1;            // GET_GAME_TIMER when the menu was first seen open (-1 unreadable)
    double g_pauseOpenedAtMs = 0;         // NowMs() then

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
        // frame only, so it is repeated every tick). The natives that would read a key press or
        // disable everything in one call are not in the 3889 crossmap, hence the loop.
        // While the pause menu (opened by TickPauseMenu, not pausing) is up, 199 / 200 / 202 stay
        // enabled: Esc, P and Backspace are how the player closes it.
        if (game::Overlay::Interactive())
        {
            for (uint64_t control = 0; control < 227; ++control)
            {
                if (g_pauseMenuActive && (control == (uint64_t)sync::kControlFrontendPause ||
                                          control == (uint64_t)sync::kControlFrontendPauseAlt ||
                                          control == (uint64_t)sync::kControlFrontendCancel))
                    continue;
                uint64_t args[] = { 0, control, 1 };  // DISABLE_CONTROL_ACTION(0, control, true)
                g_world->CallNative(game::native::DISABLE_CONTROL_ACTION, args, 3, nullptr);
            }
        }
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
    void TickPauseMenu(double nowMs)
    {
        const bool connected = join::IsConnected();
        const bool menuActive = (connected || g_pauseMenuActive) && g_world->IsPauseMenuActive();
        sync::PauseMenuIn in;
        in.nowMs = nowMs;
        in.connected = connected;
        in.canOpen = ServerMode();
        in.menuActive = menuActive;
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

    // Puts the peer's stored look on its ped (its model must be look.model). Script tick.
    void ApplyPeerLook(Peer& pe, int id)
    {
        pe.lookDirty = false;
        if (!g_world->ApplyPedLook(pe.ped, FromProtoLook(pe.look)) && !pe.lookFailLogged)
        {
            pe.lookFailLogged = true;
            PeerLog(true, "engine: remote peer id=%d (%s): look not applied (model 0x%08x)", id, pe.name.c_str(),
                    (unsigned)pe.look.model);
        }
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
    bool IssuePeerMeleeTask(Peer& pe, double nowMs)
    {
        if (!pe.anchor || !g_world->EntityExists(pe.anchor))
        {
            ++g_swingDiag.meleeTaskNoTarget;           // straight locomotion mode, or the anchor is not there yet: nothing safe to aim at
            return false;
        }
        const int32_t lp = g_localPed.load();
        pe.swingLocalHp = lp ? g_world->GetEntityHealth(lp) + g_world->GetPedArmour(lp) : 0;
        if (!g_aiMeleeApplied)
        {
            g_meleeTaskModSet = g_world->SetAiMeleeWeaponDamageModifier(0.0f);
            g_meleeTaskModUntilMs = nowMs + 1200.0;
        }
        if (!g_world->TaskPutPedDirectlyIntoMelee(pe.ped, pe.anchor, 0.2f, 0.6f, 0.0f, 0))
            return false;
        pe.swingMeleeUntilMs = nowMs + kSwingMs + 150.0;
        return true;
    }

    // R4 T3. Whether the survey may issue its melee-task entry on this puppet: auto mode, the breaker closed, the puppet's own anchor ped exists
    // (not in loco_mode=straight, the 0.4.9 default: the entry is then skipped and counted).
    bool SurveyMeleeTaskOk(const Peer& pe)
    {
        return g_swingLadder.Mode() == sync::SwingMode::Auto && !g_swingLadder.MeleeTripped() && pe.anchor && g_world->EntityExists(pe.anchor);
    }

    // R4 T3. Once the survey plan is done and every attempt has reported: pin the ladder to the attempt whose swings PROGRESSED best and log the table.
    void SurveyFinishCheck()
    {
        const int w = g_swingSurvey.TakeWinner();
        if (w == -2)
            return;
        std::string tab;
        for (size_t i = 0; i < sync::kSurveyPlan; ++i)
        {
            char t[200];
            std::snprintf(t, sizeof(t), "%s[%s: issued %u, progressed %u, blended out / flicker / frozen %u, not seen %u]", tab.empty() ? "" : " ",
                          sync::SurveyAttemptName(sync::SurveyPlanAt(i)).c_str(), g_swingSurvey.Issued(i), g_swingSurvey.Progressed(i),
                          g_swingSurvey.Failed(i), g_swingSurvey.Unseen(i));
            tab += t;
        }
        if (w >= 0)
        {
            const sync::SwingAttempt win = sync::SurveyPlanAt((size_t)w);
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
            if (pe.swingProbe.Active())
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
        const sync::SwingVariant var = att.variant;
        const bool ok = var == sync::SwingVariant::MeleeTask
                            ? IssuePeerMeleeTask(pe, nowMs)
                            : g_world->TaskPlayAnim(pe.ped, c.dict, c.clip, 8.0f, -8.0f, att.durationMs, sync::SwingVariantFlags(var), 1.0f);
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
                                var == sync::SwingVariant::MeleeTask ? kSwingMs + 200.0 : -1.0);
            pe.swingResetPending = var == sync::SwingVariant::FullBody;   // a whole-body clip replaced the gait task: the controller re-issues it when the swing is over
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
                g_swingLadder.SetMode(want);
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
        if (!swingActive)
            return;
        SwingSet& set = pe.swingProbeArmed ? g_swingSetArmed : g_swingSetUnarmed;
        const size_t ci = pe.swingProbe.Candidate();
        SwingClip& c = set.list[ci];
        const bool viaMelee = pe.swingVariant == sync::SwingVariant::MeleeTask;
        const int playing = viaMelee ? g_world->IsPedInMeleeCombat(pe.ped) : g_world->IsEntityPlayingAnim(pe.ped, c.dict, c.clip);
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
            g_swingSurvey.Start(1);                    // a finished survey: a new pass
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
    void ApplyPuppetHitReact(Peer& pe, int peerId, const sync::HitReaction& r, double nowMs, bool haveAttacker, float ax,
                             float ay, float az, uint32_t attackerWeapon)
    {
        bool ragdoll = false, bullet = false;
        sync::ReactionToShow(g_hitReact.load(std::memory_order_relaxed), r, ragdoll, bullet);
        if (bullet && !haveAttacker)
            bullet = false;
        if (!pe.ped || (!ragdoll && !bullet))
            return;
        const int32_t ped = pe.ped;
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
        pe.reactLifted = false;
        pe.reactRagdolled = false;
        pe.aimTask = false;
        if (cleared)
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
            pe.reactLifted = pe.reactRagdolled = false;
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
        bool ragdoll = false, bullet = false;
        sync::ReactionToShow(mode, r, ragdoll, bullet);
        if (!ragdoll && !bullet)
            return;
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
                                                      (int)sync::EmoteArbiter::GtaFlags(a.durationMs, a.flags), a.playbackRate / 100.0f);
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

    // T2: a puppet's controller moved to another level of the degradation ladder: say so (the numbers that decided it are in the
    // line), count it, and tell the witness (two puppets finding the same failure make it machine-wide).
    void NoteLocoLevel(Peer& pe, int id, const sync::LocoOutput& o, const sync::LocoInput& in, double nowMs)
    {
        ++pe.levelLogged;
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
                gf.entitySpeed = g_world->GetEntitySpeed(pe.ped);
                gf.pedStill = g_world->IsPedStill(pe.ped) ? 1 : 0;
            }
            pe.gaitDiag.Observe(gf);
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
    void VehInterpTotals(uint32_t& accepted, uint32_t& repeats, uint32_t& stale, uint32_t& teleports)
    {
        accepted = repeats = stale = teleports = 0;
        PeersLock lk(g_peersMtx, true);
        for (const auto& kv : g_vehNet)
        {
            const sync::VehicleInterpStats& s = kv.second.interp.Stats();
            accepted += s.accepted;
            repeats += s.repeats;
            stale += s.stale;
            teleports += s.teleports;
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
                        pe.weaponShown = 0;             // a fresh ped holds nothing yet
                        pe.aimTask = false;
                        pe.haveLastTarget = false;
                        pe.lastMove = proto::kMoveIdle;
                        SetupPeerPed(pe, it->first);    // v4: hit pool, shot / death / seat / emote edges
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
                            char pl[440];
                            std::snprintf(pl, sizeof(pl),
                                          "engine: pipe id=%d (%s): %u snapshots stored in 10 s (arrival gap ms mean %.0f p95 %.0f max %.0f), repeats %u identical + %u earlier-placed, "
                                          "%u stale, %u denoised; delay %.0f ms (class %d, extra %.0f ms, %u class changes); %u extrapolating frames (longest %.0f ms), "
                                          "%u holding, %u snaps, %u bridge resets, %u resyncs",
                                          it->first, pe.name.c_str(), pe.pipeArrivals.n + 1, pe.pipeArrivals.MeanMs(), pe.pipeArrivals.PercentileMs(0.95),
                                          pe.pipeArrivals.maxMs, ps.repeatsIdentical - pv.repeatsIdentical, ps.repeatsEarlier - pv.repeatsEarlier, ps.stale - pv.stale,
                                          ps.denoised - pv.denoised, pe.syncDelayMs, pe.pipeClass, pe.pipeExtraMs, ps.extraChanges - pv.extraChanges, pe.extrapolated,
                                          pe.pipeLongestExtrapMs, pe.held, pe.snaps, ps.bridgeResets - pv.bridgeResets, ps.resyncs - pv.resyncs);
                            report.emplace_back(pl);
                            pe.pipeSeen = ps;
                        }
                        report.emplace_back(LocoLine(it->first, pe));   // W8: why the controller issued tasks, what was last applied
                        report.emplace_back(LocoSlideLine(it->first, pe));   // Sync A: the slide metric
                        report.emplace_back(GaitLine(it->first, pe));        // R3 T2: what the game says the ped does next to the pose
                    }
                    {
                        // R3 T3: one cover line per puppet with cover activity in the last 10 s (the window is taken either way)
                        const sync::CoverStats cw = pe.cover.TakeWindow();
                        const std::string cl = sync::CoverPuppetLine(it->first, pe.name, pe.cover.Mode(), pe.cover.CurrentPhase(), pe.cover.LastWhy(), cw,
                                                                     pe.cover.Stats());
                        if (!cl.empty())
                            report.emplace_back(cl);
                    }
                    pe.slide.Reset();
                    pe.gaitDiag.Reset();
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
    }

    // ---- T3: vehicle visual state (Desync Overhaul round 2, protocol v6) -------------------------------------------------------
    // Burst tyres, smashed windows, brake lights / handbrake and a standing car's steering for the OTHER clients. The logic is
    // libs/sync/vehicle_visual.h (tested offline, tests/t3_vehvisual_test.cpp); this is the glue. Every native here is one of
    // the hook.h ones whose hash is still 0 (UNRESOLVED: lookup needed): a World:: wrapper then returns false, the sync module
    // treats that as "no information / not done", and nothing below changes behaviour until GI adds the hashes.
    // Control ids (INFERRED, GET_CONTROL_NORMAL(0, id)): INPUT_VEH_MOVE_LR 59 (-1 left .. +1 right), INPUT_VEH_ACCELERATE 71,
    // INPUT_VEH_BRAKE 72, INPUT_VEH_HANDBRAKE 76. The steering-axis agreement counters in the 10 s line show a wrong id or sign.
    constexpr int kCtlVehMoveLR = 59, kCtlVehAccelerate = 71, kCtlVehBrake = 72, kCtlVehHandbrake = 76;
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
        const sync::BodyDelta d = sync::DiffBody(v.bodyShown, st.flags2, st.doors);
        if (!d.Any())
            return;
        const bool instantly = !v.bodyShown.valid;
        if (d.flags2 & proto::kVeh2HighBeam)
            g_world->SetVehicleFullbeam(h, (st.flags2 & proto::kVeh2HighBeam) != 0);
        if (d.flags2 & proto::kVeh2Interior)
            g_world->SetVehicleInteriorLight(h, (st.flags2 & proto::kVeh2Interior) != 0);
        if (d.flags2 & proto::kVeh2IndicatorL)
            g_world->SetVehicleIndicatorLights(h, 1, (st.flags2 & proto::kVeh2IndicatorL) != 0);
        if (d.flags2 & proto::kVeh2IndicatorR)
            g_world->SetVehicleIndicatorLights(h, 0, (st.flags2 & proto::kVeh2IndicatorR) != 0);
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
            g_world->SetVehicleDoorsLocked(h, lock);
            v.appliedLock = lock;
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
        }
        const int siren = (st.flags & proto::kVehSiren) ? 1 : 0;
        if (siren != v.appliedSiren)
        {
            g_world->SetVehicleSiren(h, siren != 0);
            v.appliedSiren = siren;
        }
        if (st.flags & proto::kVehHorn)
            g_world->StartVehicleHorn(h, 400);         // renewed by every full state while it sounds
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
                g_world->SetEntityAngularVelocity(v.handle, ax, ay, az + (float)d.dw);
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
                    v.vis.Reset();                     // T3: the new controller's tyres / windows are read and applied afresh
                    v.visReadMs = -1e9;
                    v.sentVis = sync::VisualMask();
                    v.steerPrevRotMs = -1e9;
                    ResetVehicleDamage(v);             // R3 T4: new role, new baselines
                }
                if (v.appliedVersion != v.stVersion)
                {
                    ApplyVehicleFields(v, iControl);
                    v.appliedVersion = v.stVersion;
                }
                {
                    // T3: tyres / windows. "Near" = the server position within kVisNearM of us (a far copy is only brought to the
                    // server's state when it changes, never re-read or scanned).
                    const float vdx = st.k.x - lpx, vdy = st.k.y - lpy, vdz = st.k.z - lpz;
                    const bool visNear = mine || (haveLp && vdx * vdx + vdy * vdy + vdz * vdz <= kVisNearM * kVisNearM);
                    TickVehicleVisual(v, id, iControl, nowMs, visNear, lp);
                    TickVehicleDamage(v, id, iControl, nowMs, visNear);    // R3 T4
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
                            sync::VehicleDriveOutput o = v.drive.Update(in);
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
                                const sync::DriveVec& w = g_vehAngBody.load(std::memory_order_relaxed)
                                                              ? o.angularVelocityBody : o.angularVelocity;
                                g_world->SetEntityAngularVelocity(h, w.x, w.y, w.z);
                            }
                            // Steering (sync plan W2 D): the sender fills VehicleUpdate.steer from its yaw rate
                            // (sync::EstimateSteer). The wire is -1 left .. +1 right and SET_VEHICLE_STEER_BIAS
                            // is -1 right .. +1 left, so the sign flips. Every frame, the native holds only while
                            // called. Whether it turns a puppet's visible wheels is INFERRED (verify live).
                            if (o.setSteering)
                            {
                                const bool steerOk = g_world->SetVehicleSteerBias(h, -o.steering);
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
            const int passengers = g_world->GetVehicleMaxPassengers(veh);
            si.maxSeats = passengers >= 0 ? std::min(passengers + 1, sync::kEntrySeats) : 2;
            GLOGI("engine: vehicle: model 0x%08x has %d seats%s; %d of 8 seat bones found%s", (unsigned)model,
                  si.maxSeats, passengers >= 0 ? "" : " (GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS not callable: assumed)",
                  found, found == 0 ? " -- door positions use fixed offsets" : "");
        }
        return si;
    }

    // Where one stands to use each seat of `car` (world), from the seat bones; a seat without a bone gets a
    // fixed offset (x +-0.45 m by side, y 0.4 m forward minus 0.9 m per row) rotated by the vehicle's quaternion.
    void FillEntryDoors(EntryCar& car)
    {
        const SeatInfo& si = SeatInfoFor(car.handle, car.model);
        car.c.maxSeats = si.maxSeats;
        sync::DriveQuat q;
        bool haveQ = false, triedQ = false;
        for (int s = 0; s < si.maxSeats && s < sync::kEntrySeats; ++s)
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
        std::vector<EntryCar> cars;
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
            for (const int control : { kControlEnter, kControlDetonate })
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
                EntryNotice(sync::LocalVehicleLink::RefusalText(3 /* TooFar */), nowMs);
            CancelEntryWalk(lp);
            return;
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
    // for the indicators (INFERRED: a script-set indicator would need its own command; bits 3 and 4 are 0).
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
        flags2 = sync::Flags2FromState(lightsOn, highBeam, false, false, false, haveRoof && sync::RoofDownFromState(roof));
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
                const sync::SteerChoice sc = sync::ChooseSteer(vFwd, yaw, VehicleWheelbaseM(v.handle), v.steerPrev, axisOk, axis);
                u.steer = sc.steer;
                v.steerPrev = u.steer;
                g_vis.steerSent.Add(sc, axisOk, axis, sync::EstimateSteer(vFwd, yaw, VehicleWheelbaseM(v.handle), 0.0f), vFwd);
            }
            if (g_world->IsVehicleEngineRunning(v.handle)) u.flags |= proto::kVehEngine;
            if (g_world->IsVehicleSirenOn(v.handle))      u.flags |= proto::kVehSiren;
            if (g_world->IsHornActive(v.handle))          u.flags |= proto::kVehHorn;
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
        double downSum = 0, gripSum = 0, brakeSum = 0;
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

    void HyperVerifyAngular(HyperState& st, int32_t veh, const sync::HyperVec& wrote)
    {
        const uint32_t n = st.angWrites++;
        if (!(n < 12 || n % 120 == 0))
            return;
        float x = 0, y = 0, z = 0;
        // The setter and the getter do not use exactly the same frame: on the live Jesko (0.4.14, 2026-11-05) the
        // yaw read back to 3 decimals every time while roll / pitch were 0.01-0.02 off, which is a ~1.16 rad/s
        // yaw vector tilted by the car's ~0.6 degree body tilt, not a write that failed. So x / y get a tolerance
        // relative to the whole vector (3 % = a 1.7 degree frame difference); z keeps the tight one.
        const float mag = std::sqrt(wrote.x * wrote.x + wrote.y * wrote.y + wrote.z * wrote.z);
        const auto close = [](float a, float b) { return std::fabs(a - b) <= 0.01f + 0.01f * std::fabs(b); };
        const auto closeTilt = [mag](float a, float b) { return std::fabs(a - b) <= 0.01f + 0.03f * mag; };
        if (g_world->GetEntityRotationVelocity(veh, x, y, z) && closeTilt(x, wrote.x) && closeTilt(y, wrote.y) && close(z, wrote.z))
        {
            ++st.angOk;
            return;
        }
        ++st.angBad;
        static int logged = 0;
        if (logged < 4)
        {
            ++logged;
            GLOGW("engine: hyper assist: SET_ENTITY_ANGULAR_VELOCITY wrote %.3f %.3f %.3f and the same-tick GET_ENTITY_ROTATION_VELOCITY says "
                  "%.3f %.3f %.3f (bad %u, ok %u)", (double)wrote.x, (double)wrote.y, (double)wrote.z, (double)x, (double)y, (double)z,
                  st.angBad, st.angOk);
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
        const sync::HyperAssistOut out = sync::HyperAssistStep(p.assist, in);
        if (in.grounded) ++st.grounded;
        if (brake > 0.05f) ++st.brakeDown;
        if (out.downMs2 > 0) { ++st.downN; st.downSum += out.downMs2; }
        if (out.gripMs2 > 0) { ++st.gripN; st.gripSum += out.gripMs2; }
        if (out.brakeMs2 > 0) { ++st.brakeN; st.brakeSum += out.brakeMs2; }
        if (out.yawClamped) ++st.yawN;
        if (out.velChanged)
        {
            if (g_world->SetEntityVelocity(veh, out.vel.x, out.vel.y, out.vel.z))
            {
                ++st.velN;
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
            if (g_world->SetEntityAngularVelocity(veh, out.angVel.x, out.angVel.y, out.angVel.z))
            {
                ++st.angN;
                HyperVerifyAngular(st, veh, out.angVel);
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
                  "applied: downforce %u frames (mean %.1f m/s^2), lateral grip %u (mean %.1f), brake assist %u (mean %.1f), yaw cap %u; "
                  "writes: velocity %u, angular %u; failures: reads %u, velocity writes %u, angular writes %u; read-back: velocity ok %u / bad %u, "
                  "angular ok %u / bad %u; health top-ups %u; mask 0x%x%s%s", sync::HyperProfileName(p), st.veh, (now - st.logMs) / 1000.0, st.frames, st.grounded,
                  st.brakeDown, (double)st.peakMs * 3.6, (double)p.targetKmh, st.downN, st.downN ? st.downSum / st.downN : 0.0, st.gripN,
                  st.gripN ? st.gripSum / st.gripN : 0.0, st.brakeN, st.brakeN ? st.brakeSum / st.brakeN : 0.0, st.yawN, st.velN, st.angN,
                  st.readFail, st.velFail, st.angFail, st.velOk, st.velBad, st.angOk, st.angBad, st.healthFixed, st.mask,
                  st.velDead ? " (velocity write dropped)" : "", st.angDead ? " (angular write dropped)" : "");
        st.logMs = now;
        st.frames = st.grounded = st.brakeDown = st.downN = st.gripN = st.brakeN = st.yawN = st.velN = st.angN = 0;
        st.readFail = st.velFail = st.angFail = st.healthFixed = 0;
        st.downSum = st.gripSum = st.brakeSum = 0;
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
            put(out[i].name, sizeof(out[i].name), game::content::CarDisplayName(v.make, v.label, v.model));
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
            GLOGI("engine: hyper tune switches: hyper_tune=%s, hyper_assist mask 0x%x (hyper_tune=0 turns the tune off; hyper_assist=<bits: 1 downforce, "
                  "2 lateral grip, 4 brake assist, 8 yaw cap, 16 anti-roll; 0 = none> turns the handling assists off)", tuneOn ? "on" : "OFF",
                  assistMask);
        }
        if (!tuneOn)
            return;
        g_world->SetVehicleCheatPowerIncrease(veh, prof->power);   // the game clears it every frame
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
        g_swingLadder.SetMode((sync::SwingMode)g_meleeAnimWanted.load());   // R3 T6: a new session starts the ladder from the registry mode again
        {
            // R4 T3: ... and the survey from its first attempt (only where animations are tried at all: auto / play)
            const sync::SwingMode sm = g_swingLadder.Mode();
            g_swingSurvey.Start(sm == sync::SwingMode::Auto || sm == sync::SwingMode::Play ? (uint32_t)g_meleeSurveyWanted.load() : 0u);
            g_swingDurationMs = 400;
            g_swingNo = 0;
            g_ownSwingNo = 0;
        }
        ReleaseSwingDicts();
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
                                                      (int)sync::EmoteArbiter::GtaFlags(a.durationMs, a.flags), a.playbackRate / 100.0f);
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
                  "applied: n=%u (failed %u) min %.2f mean %.2f max %.2f; sender yaw rate, GET_ENTITY_ROTATION_VELOCITY / rotation-derived: mean %.2f over %u samples",
                  c.tyreReadOk, c.tyreReadFail, c.winReadOk, c.winReadFail, c.brakeSet, c.brakeFail, c.handSet, c.handFail, c.pedalReadFail, c.sentUpdates,
                  c.sentWithTyres, c.sentWithWindows, c.brakeBitsSent, c.handBitsSent, carsTyres, tyreBits, carsWindows, c.partHits, c.partHitsRefused, tot.reads,
                  tot.burstsApplied, tot.rimsApplied, tot.tyresFixed, tot.windowsSmashed, tot.windowsFixed, tot.reported, tot.confirmed, tot.expired, tot.divergent,
                  tot.stuck, tot.cmdFailures, tot.suspends, c.steerSent.n, (double)c.steerSent.min, c.steerSent.Mean(), (double)c.steerSent.max, c.steerSent.axis,
                  c.steerSent.held, c.steerSent.estimate, c.steerSent.agree, c.steerSent.disagree, c.steerApplied, c.steerApplyFail, (double)g_vis.steerAppliedMin,
                  c.steerApplied ? g_vis.steerAppliedSum / c.steerApplied : 0.0, (double)g_vis.steerAppliedMax, c.yawRatioN ? c.yawRatioSum / c.yawRatioN : 0.0,
                  c.yawRatioN);
        // The window counters start over; the sync::VisualSync totals above run on.
        g_vis.reads = g_vis.tyreReadOk = g_vis.tyreReadFail = g_vis.winReadOk = g_vis.winReadFail = 0;
        g_vis.sentUpdates = g_vis.sentWithTyres = g_vis.sentWithWindows = g_vis.partHits = g_vis.partHitsRefused = 0;
        g_vis.brakeSet = g_vis.brakeFail = g_vis.handSet = g_vis.handFail = g_vis.brakeBitsSent = g_vis.handBitsSent = g_vis.pedalReadFail = 0;
        g_vis.steerApplied = g_vis.steerApplyFail = 0;
        g_vis.steerAppliedMin = g_vis.steerAppliedMax = 0.0f;
        g_vis.steerAppliedSum = 0.0;
        g_vis.steerSent.Reset();
        g_vis.yawRatioSum = 0.0;
        g_vis.yawRatioN = 0;
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
                static uint32_t lastLinkIn = 0, lastAcc = 0, lastRep = 0, lastStale = 0, lastTele = 0;
                uint32_t acc = 0, rep = 0, stl = 0, tel = 0;
                VehInterpTotals(acc, rep, stl, tel);
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
                          "as controller: %u updates sent (mean speed %.1f m/s; speed bands 0-5/5-15/15-30/over 30: %u/%u/%u/%u)",
                          g_nearClock.load() ? "on" : "OFF", g_proxContact.load() ? "on" : "OFF", (unsigned)join::RttMs(), owners.c_str(), linkIn - lastLinkIn,
                          g_t5.ownerRttUnknown, g_t5.lagN ? g_t5.lagSum / g_t5.lagN : 0.0, g_t5.lagMax, clk5.DelayMs(), sync::PositionDiag::Format(pw).c_str(),
                          g_t5.proxFrames, g_t5.proxOpened, g_t5.proxUpgraded, (acc >= lastAcc ? acc - lastAcc : 0u), (rep >= lastRep ? rep - lastRep : 0u), (stl >= lastStale ? stl - lastStale : 0u), (tel >= lastTele ? tel - lastTele : 0u),
                          g_t5.sentN, g_t5.sentN ? g_t5.sentSpeedSum / g_t5.sentN : 0.0,
                          g_t5.sentFrames[0], g_t5.sentFrames[1], g_t5.sentFrames[2], g_t5.sentFrames[3]);
                }
                lastLinkIn = linkIn;
                lastAcc = acc; lastRep = rep; lastStale = stl; lastTele = tel;
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
                  "knockdown / %u melee stagger",
                  c.meleeSwings - last.meleeSwings, c.peerSwings - last.peerSwings, c.peerShotsReal - last.peerShotsReal,
                  c.muzzleFlashes - last.muzzleFlashes, c.reactFlinch - last.reactFlinch, c.reactKnock - last.reactKnock,
                  c.reactStagger - last.reactStagger, c.puppetFlinch - last.puppetFlinch, c.puppetKnock - last.puppetKnock,
                  c.puppetStagger - last.puppetStagger);
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
        if (!lp) {
            // Player not spawned yet — render the GAGE loading overlay.
            void* sc = game::GetSwapChain();
            // if (sc) game::RenderLoadingOverlay(sc, false);
            // Heartbeat so a stuck load is diagnosable from gage.log: PLAYER_PED_ID stays 0 while GTA
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

        // First frame with a valid player ped — dismiss the overlay and go live.
        {
            static bool loggedSpawn = false;
            if (!loggedSpawn) {
                loggedSpawn = true;
                void* sc = game::GetSwapChain();
                // if (sc) game::RenderLoadingOverlay(sc, true);
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
        if (inScript && mayChange && lp) {
            // Auto-mount the add-on car packs through the game's own DLC loader, if armed (pack_mount=1; default off),
            // and hand the Tab menu's car browser the list whenever it (or the loading state) changes.
            const game::content::Status packs = game::content::Tick(*g_world);
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
                    RunCommands(lp, now);   // /car /gun /heal /kill /tp /fix
                }
            }
            if (lp)
            {
                WorkSection s(probe, kSecCommands);
                const bool dead = g_life != Life::Alive || g_world->IsPedDeadOrDying(lp);
                TickLocalEmote(lp, lpVeh != 0, dead, nowMs);
                if (ServerMode())
                {
                    TickVehicleEntry(lp, lpVeh, dead, nowMs);   // F / G first; the link sees the new seat next tick
                    TickLocalVehicle(lp, lpVeh, dead, nowMs);
                }
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
                // 0 blip(s)" at entry), so only the entry's one-shot runs.
                if (inScript && !g_world->StockScriptsOff())
                {
                    WorkSection s(probe, kSecBlips);
                    g_world->ClearSpBlips();
                }
            }
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

        // If natives weren't found at Init time (too early), retry here.
        game::retryNativeTableOnce();

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
        join::PeerSinks sinks;
        sinks.onWelcome = OnWelcome;
        sinks.onJoined = OnJoined;
        sinks.onLeft = OnLeft;
        sinks.onWorld = OnWorld;
        sinks.onCoarse = OnCoarse;
        sinks.onLook = OnLook;
        sinks.onChat = OnChat;
        sinks.onEnded = OnEnded;
        sinks.onVehicles = OnVehicles;                 // protocol v4
        sinks.onVehicleResult = OnVehicleResult;
        sinks.onSeatNotice = OnSeatNotice;
        sinks.onVehicleImpact = OnVehicleImpact;   // T4
        sinks.onLinkInfo = OnLinkInfo;             // R3 T5
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
            g_coarse.clear();
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
