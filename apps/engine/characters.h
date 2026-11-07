// characters: 0.5.9, the server's characters on this machine (GI 2026-11-07: "1 character min insta creation if not created on server join.
// can buy additional 2 char slots"). The wire is docs/api/protocol.md "0.5.9", the rules libs/sync/character.h, the server
// apps/server/character_store.* + Server::LoadCharacters / OnCharOp.
//
// The join worker hands the roster (CharRoster) and the answers (CharResult) here; the script tick puts the ACTIVE character's face on our own
// ped (TickLocal: after the entry, after every change of the roster, of the ped handle, and after DressPlayer put the fixed 0.5.x look back),
// and engine.cpp's peer path puts every other player's face (PeerFace) on its puppet right after its look (ApplyPeer). The face is
// sync::chr::PlanFace: the head blend, 20 micro morphs, 13 head overlays with tints, the hair and its tint, the eye colour (World::
// ApplyCharacterFace), each followed by a read-back of the eye colour, the eyebrows and the hair drawable.
//
// Not in this round: a FEMALE character needs the female freemode model on our ped (the entry, the ped guard and the outfit are male-only),
// so its face is not applied and one line says so; the instant character is always male, and only the creator (a later round) makes women.
// Switch: char_face in HKCU\Software\GAGE-MP (default on; 0 = 0.5.8: the fixed face on our ped, the others' faces ignored).
// Log: `engine: characters (0.5.9)` (the switch per session, every roster and answer received) and `engine: character face (0.5.9)` (each
// apply with its read-backs; at most 40 lines a session).
#pragma once

#include "protocol/protocol.h"
#include "sync/character.h"

#include <cstdint>
#include <string>

namespace gage::game {
    struct World;
}

namespace gage::game::characters {

    // Join worker, at every Welcome: reads the switch and logs it; forgets the last session's roster.
    void ReadSwitch();
    bool On();
    // Join worker: our roster / the answer to one of our CharOps / the session ended.
    void OnRoster(const proto::CharRoster& m);
    void OnResult(const proto::CharResult& m);
    void OnEnded();
    // Any thread: a copy of our roster (false when none arrived this session). `guest` = the server will not keep it.
    bool Roster(sync::chr::Roster& out, bool* guest = nullptr);

    // Script tick: DressPlayer put the fixed look (face, hair) back on our ped; the next TickLocal applies the character again.
    void NoteRedressed();
    // Script tick, once the entry is done and we are alive: the active character's face on our ped when anything it depends on changed.
    void TickLocal(World& w, int32_t ped, uint32_t model);
    // Script tick: another player's face on its puppet (whose model is `model`). False (nothing called) when the switch is off or the model is
    // not the freemode model of the face's sex. `who` names it in the log.
    bool ApplyPeer(World& w, int32_t ped, uint32_t model, const sync::chr::Appearance& a, int id, const std::string& who);

}  // namespace gage::game::characters
