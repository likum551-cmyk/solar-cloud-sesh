// GAGE-MP in-game engine: singleplayer script tables (shared source of truth).
//
// GTA V singleplayer runs a tree of GTA-script threads. When GAGE boots the game into Story
// Mode and swaps the player to a freemode character, the campaign's story/activity scripts keep
// running underneath -- they drive background voice and on-screen mission text. These tables name:
//   * the scripts the engine terminates to silence that bleed (StoryScriptsToTerminate),
//   * the session-root scripts it must NEVER terminate (SessionRootNames -- main_persistent is our
//     own script host; main / startup own the singleplayer session itself),
//   * a wider set of known names used only to make the DumpLiveScripts diagnostic log readable.
//
// The lists live here, not inline in hook.cpp, so a unit test (script_suppress_test) can assert
// the real data: no hash collisions in the diagnostic table, and the terminate list never
// overlaps the session roots. joaat() is GTA V's script-name hash (Jenkins one-at-a-time,
// lowercased); it is proven against the live game -- the script tick only goes live once
// joaat("main_persistent") matches a running thread's m_script_hash.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace gage::engine::spscripts {

inline uint32_t Joaat(const char* s) {
    uint32_t h = 0;
    for (; *s; ++s) {
        h += (uint8_t)((*s >= 'A' && *s <= 'Z') ? *s + 32 : *s);
        h += h << 10;
        h ^= h >> 6;
    }
    h += h << 3;
    h ^= h >> 11;
    h += h << 15;
    return h;
}

// Session-root scripts that must never be terminated: main_persistent is our own script host, and
// main / startup (and its variants) own the singleplayer session we run inside.
inline const char* const* SessionRootNames(size_t& count) {
    static const char* const kRoots[] = {
        "main_persistent", "main", "startup", "startup_install", "startup_positioning",
        "standard_global_init",
    };
    count = sizeof(kRoots) / sizeof(kRoots[0]);
    return kRoots;
}

// The singleplayer story / activity scripts SuppressSPSystems terminates to silence campaign
// voice and on-screen mission text. Leaf scripts the game itself terminates on mission end;
// terminating one is a no-op when it is not running, and they do not respawn in free-roam.
inline const char* const* StoryScriptsToTerminate(size_t& count) {
    static const char* const kStory[] = {
        "flow_controller",    // the SP mission-flow processor: it (re)launches prologue1, so killing
                              //   prologue1 alone is not enough -- the flow relaunches it (gage.log:
                              //   prologue1 reappeared as id 68 and teleported us to North Yankton)
        "prologue1",          // the prologue mission a fresh Story Mode boot runs (Ludendorff)
        "prologuescreen",
        "selector",           // the singleplayer character-switch wheel (no switching in MP)
        "charactersheet",     // the singleplayer stats sheet
        "replay_controller",  // Rockstar Editor capture
    };
    count = sizeof(kStory) / sizeof(kStory[0]);
    return kStory;
}

// Story-character scripts that take the player ped back AFTER the freemode entry. main relaunches
// them a few seconds in (gage.log 2026-11-01 09:23: not running at entry, player_controller id 52
// live 8 s later, then the ped swapped to Michael 0x0d7114c9 every second against the ped guard).
// The entry no longer needs them -- the engine does its own reveal, collision and control -- so
// SuppressPedHijackers terminates them every tick once the entry is done (kill_sp_hijack=1).
inline const char* const* PostEntryHijackers(size_t& count) {
    static const char* const kHijack[] = {
        "player_controller",       // forces the story character (Michael/Franklin/Trevor) onto the player
        "player_timetable_scene",  // places that character in his timetable scene (teleports the player)
    };
    count = sizeof(kHijack) / sizeof(kHijack[0]);
    return kHijack;
}

// Known singleplayer script names, resolved by DumpLiveScripts for a readable log. Unknown hashes
// are logged as hex and reversed offline. Includes the session roots and terminate targets above
// so they, too, show by name.
inline const char* const* KnownNames(size_t& count) {
    static const char* const kNames[] = {
        // boot / session root -- shown but never terminated (main_persistent is our script host)
        "main", "main_persistent", "startup", "startup_install", "startup_positioning",
        "standard_global_init", "initial_scenes_controller",
        // the prologue (what a fresh Story Mode boot runs) and its screen
        "prologue1", "prologuescreen",
        // singleplayer controllers
        "player_controller", "selector", "charactersheet", "blip_controller", "flow_controller",
        "replay_controller", "respawn_controller", "director_mode", "savegame_bed",
        "cellphone_controller", "autosave_controller", "shop_controller", "wardrobe",
        "stripclub", "scripted_cutscene", "cutscene", "maintransition", "sctv",
        "pausemenu", "pausemenu_multiplayer", "loadingscreen", "net_cloud_loading",
        "ingamehud", "hud", "scaleform_minigame",
        // ambient / character scenes that drive campaign voice
        "ambientblimp", "friends_controller", "player_timetable_scene",
        "player_scene_t_bustout", "celebration_setup",
        // common online roots (should NOT be running in singleplayer)
        "freemode", "fm_mission_controller",
    };
    count = sizeof(kNames) / sizeof(kNames[0]);
    return kNames;
}

// Resolve a live m_script_hash to a known name, or null. (The table is small; a linear scan on a
// once-per-diagnostic call is fine.)
inline const char* LookupName(uint32_t h) {
    size_t n = 0;
    const char* const* names = KnownNames(n);
    for (size_t i = 0; i < n; ++i)
        if (Joaat(names[i]) == h) return names[i];
    return nullptr;
}

}  // namespace gage::engine::spscripts
