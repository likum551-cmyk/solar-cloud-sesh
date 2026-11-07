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
// Stage 4: a FEMALE character is played on mp_f_freemode_01 (WantedModel: the entry, the ped guard and DressPlayer's female table follow
// the active character; a switch to a character of the other sex is a model swap by the ped guard within a second). The instant character
// is male; the creator makes women.
// Switches: char_face in HKCU\Software\GAGE-MP (default on; 0 = 0.5.8: the fixed face on our ped, the others' faces ignored, every
// character on the male model) and char_female (default on; 0 = every character on the male model).
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
    // Stage 4. Any thread. The freemode model our ped must be: the female one for a female active character while char_face and char_female
    // are on (char_female in HKCU\Software\GAGE-MP, default on; 0 = every character is played on the male model and a female face is not
    // applied), else the male one (also with no roster). The entry and the ped guard ask this instead of a fixed model.
    uint32_t WantedModel();
    bool FemaleOn();
    // Our name as the others see it (sync::chr::PublicName of the active character); empty with no roster (the caller keeps the player name).
    std::string ShownName();
    // The active character's "First Last" for the inventory's CHARACTER chip; empty with no roster.
    std::string ActiveDisplay();
    // The roster's change counter (bumped by every CharRoster received): the overlay's select screen re-reads on a change.
    uint32_t RosterSeq();

    // Script tick: DressPlayer put the fixed look (face, hair) back on our ped; the next TickLocal applies the character again.
    void NoteRedressed();
    // Stage 4, script tick (before TickLocal): while the creator is on screen (Overlay::CharacterDraft) its draft face goes on our ped when the
    // draft's sex matches our model (re-applied on every change, at most ~7 a second; TickLocal holds off meanwhile), and the other sex's
    // model is requested for the preview ped. When the creator closes, the active character's face goes back on. True = a draft is open
    // (`draftOut` gets it).
    bool TickDraft(World& w, int32_t ped, uint32_t model, sync::chr::Appearance* draftOut = nullptr);
    // Script tick, once the entry is done and we are alive: the active character's face on our ped when anything it depends on changed.
    void TickLocal(World& w, int32_t ped, uint32_t model);
    // Script tick: another player's face on its puppet (whose model is `model`). False (nothing called) when the switch is off or the model is
    // not the freemode model of the face's sex. `who` names it in the log.
    bool ApplyPeer(World& w, int32_t ped, uint32_t model, const sync::chr::Appearance& a, int id, const std::string& who);

}  // namespace gage::game::characters
