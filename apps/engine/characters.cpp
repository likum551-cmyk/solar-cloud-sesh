// characters: 0.5.9, the server's characters on this machine. The design is in characters.h and libs/sync/character.h.
#include "characters.h"

#include "hook.h"
#include "overlay.h"
#include "common/logger.h"
#include "common/registry.h"

#include <windows.h>   // GetTickCount64

#include <atomic>
#include <mutex>
#include <string>

namespace gage::game::characters {

    namespace chr = gage::sync::chr;

    namespace {

        std::atomic<bool> g_on{true};          // char_face (read at every Welcome)
        std::atomic<bool> g_female{true};      // char_female (stage 4)

        // Join worker -> script tick (and the overlay).
        std::mutex g_mtx;
        chr::Roster g_roster;
        bool g_have = false, g_guest = false;
        uint32_t g_seq = 0;                    // bumped by every roster received

        // Script tick only.
        std::atomic<bool> g_redressed{false};
        uint32_t g_appliedSeq = 0;
        int32_t g_appliedPed = 0;
        std::atomic<int> g_lines{0};           // face lines this session (at most kMaxLines; reset by the join worker)
        uint32_t g_skipSeq = 0;                // the roster seq whose "not applied" reason was logged
        constexpr int kMaxLines = 40;

        const char* SexName(chr::Sex s) { return s == chr::Sex::Female ? "female" : "male"; }

        // One apply with its log line. `who` names the ped ("our ped", "peer id=3 (Pia)").
        bool ApplyLogged(World& w, int32_t ped, const chr::Appearance& a, const std::string& who, const std::string& name)
        {
            World::FaceResult r;
            const chr::FacePlan plan = chr::PlanFace(a);
            const bool ok = w.ApplyCharacterFace(ped, plan, &r);
            const bool match = r.eyeRead == plan.eyeColor && r.browsRead == plan.overlay[2].index && r.hairRead == plan.hairDrawable;
            if (g_lines.fetch_add(1) < kMaxLines)
            {
                GLOGI("engine: character face (0.5.9): %s%s%s (handle %d, %s): %d natives, %d could not be called; hair %s; read back eye %d "
                      "(want %d), eyebrows %d (want %d), hair %d (want %d): %s",
                      who.c_str(), name.empty() ? "" : " ", name.c_str(), (int)ped, SexName(a.sex), r.calls, r.failed,
                      r.hairSet ? "set" : "NOT valid on this ped", r.eyeRead, plan.eyeColor, r.browsRead, (int)plan.overlay[2].index, r.hairRead,
                      plan.hairDrawable, match ? "all as asked" : "MISMATCH (the game shows something else)");
            }
            return ok;
        }

    }

    bool On() { return g_on.load(); }
    bool FemaleOn() { return g_female.load(); }

    uint32_t WantedModel()
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        const chr::Character* a = g_have ? g_roster.Active() : nullptr;
        return a && g_on.load() && g_female.load() && a->look.sex == chr::Sex::Female ? chr::kModelFemale : chr::kModelMale;
    }

    std::string ShownName()
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        const chr::Character* a = g_have ? g_roster.Active() : nullptr;
        return a ? chr::PublicName(*a) : std::string();
    }

    std::string ActiveDisplay()
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        const chr::Character* a = g_have ? g_roster.Active() : nullptr;
        return a ? a->Display() : std::string();
    }

    uint32_t RosterSeq()
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        return g_seq;
    }

    void ReadSwitch()
    {
        const bool on = reg::Get(L"char_face") != L"0";
        g_on.store(on);
        const bool fem = reg::Get(L"char_female") != L"0";
        g_female.store(fem);
        const bool scr = reg::Get(L"char_screens") != L"0";
        game::Overlay::SetCharacterScreens(scr);
        GLOGI("engine: characters (0.5.9): char_screens=%s", scr ? "on (the Tab menu's CHARACTERS page: the 3 slots, play / edit / delete with the typed "
                                                                   "name / create, and the creator with its preview; LIVE-UNVERIFIED)"
                                                                 : "OFF (no CHARACTERS page: the roster is still kept and the face applied)");
        GLOGI("engine: characters (0.5.9): char_female=%s", fem ? "on (a female character is played on mp_f_freemode_01: the entry, the ped guard "
                                                                  "and the outfit follow the active character; LIVE-UNVERIFIED)"
                                                                : "OFF (every character on the male model, a female face is not applied)");
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_roster = chr::Roster();
            g_have = g_guest = false;
        }
        g_lines.store(0);
        GLOGI("engine: characters (0.5.9): char_face=%s", on ? "on (our active character's face, hair and eyes on our ped; the other players' "
                                                               "faces on their puppets; LIVE-UNVERIFIED)"
                                                             : "OFF (0.5.8: the fixed face on our ped, the others' faces ignored)");
    }

    void OnRoster(const proto::CharRoster& m)
    {
        chr::Roster r;
        if (!chr::DecodeRoster(m.blob, r))
        {
            GLOGW("engine: characters (0.5.9): a roster of %u bytes does not decode; kept the previous one", (unsigned)m.blob.size());
            return;
        }
        const chr::Character* a = r.Active();
        GLOGI("engine: characters (0.5.9): roster: %d character(s) in %d slot(s)%s, playing slot %d \"%s\" (%s, %s)", r.Count(), (int)r.slots,
              (m.flags & proto::kCharRosterGuest) ? " (guest: not kept by the server)" : "", (int)r.active,
              a ? a->Display().c_str() : "?", a ? SexName(a->look.sex) : "?",
              a ? ((a->flags & chr::kCharCustomised) ? "customised" : "instant, not customised yet") : "?");
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_roster = r;
            g_have = true;
            g_guest = (m.flags & proto::kCharRosterGuest) != 0;
            ++g_seq;
        }
        game::Overlay::SetCharacterRoster(m.blob, (m.flags & proto::kCharRosterGuest) != 0);   // stage 4: the CHARACTERS page
    }

    void OnResult(const proto::CharResult& m)
    {
        const char* op = m.op >= 1 && m.op <= chr::kOpLast ? chr::ToString((chr::Op)m.op) : "?";
        const char* code = m.code <= chr::kCodeLast ? chr::ToString((chr::Code)m.code) : "?";
        if (m.code == (uint8_t)chr::Code::Ok)
            GLOGI("engine: characters (0.5.9): %s slot %u (seq %u): ok %s", op, (unsigned)m.slot, (unsigned)m.seq, m.text.c_str());
        else
            GLOGW("engine: characters (0.5.9): %s slot %u (seq %u) refused: %s %s", op, (unsigned)m.slot, (unsigned)m.seq, code, m.text.c_str());
        game::Overlay::SetCharacterResult(m.seq, m.op, m.slot, m.code, m.text);   // stage 4: the page's status line
    }

    void OnEnded()
    {
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_have = false;
        }
        game::Overlay::ClearCharacters();
    }

    bool Roster(chr::Roster& out, bool* guest)
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!g_have) return false;
        out = g_roster;
        if (guest) *guest = g_guest;
        return true;
    }

    void NoteRedressed() { g_redressed.store(true); }

    namespace {
        // Script tick only: the creator's draft on our ped (stage 4).
        bool g_draftShown = false;            // our ped wears a draft face now
        uint32_t g_draftRev = ~0u;
        int32_t g_draftPed = 0;
        uint64_t g_draftAt = 0;
        int g_draftLines = 0;
        bool g_draftOtherLogged = false;
    }

    bool TickDraft(World& w, int32_t ped, uint32_t model, chr::Appearance* draftOut)
    {
        chr::Appearance a;
        uint32_t rev = 0;
        const bool open = g_on.load() && ped && game::Overlay::CharacterDraft(&a, &rev);
        if (!open) {
            if (g_draftShown) {
                g_draftShown = false;
                g_draftRev = ~0u;
                g_redressed.store(true);   // the creator closed: TickLocal puts the active character's face back
                if (g_draftLines++ < 40)
                    GLOGI("engine: character creator (0.5.9): closed; the active character's face goes back on our ped");
            }
            g_draftOtherLogged = false;
            return false;
        }
        if (draftOut) *draftOut = a;
        if (model != chr::ModelOf(a.sex)) {
            // a draft of the other sex: the preview spawns a ped of that model (engine.cpp PreviewSourcePed); our own ped keeps its face
            w.RequestModel(chr::ModelOf(a.sex));
            if (!g_draftOtherLogged && g_draftLines++ < 40)
                GLOGI("engine: character creator (0.5.9): a %s draft while our ped is model 0x%08x: shown on a preview ped of model 0x%08x only",
                      SexName(a.sex), (unsigned)model, (unsigned)chr::ModelOf(a.sex));
            g_draftOtherLogged = true;
            if (g_draftShown) {
                g_draftShown = false;
                g_redressed.store(true);
            }
            return true;
        }
        const uint64_t now = GetTickCount64();
        if (rev == g_draftRev && ped == g_draftPed) return true;
        if (now - g_draftAt < 150) return true;   // a held arrow or a fast wheel: at most ~7 applies a second
        g_draftAt = now;
        g_draftRev = rev;
        g_draftPed = ped;
        g_draftShown = true;
        World::FaceResult r;
        const bool ok = w.ApplyCharacterFace(ped, chr::PlanFace(a), &r);
        if (g_draftLines < 40 && (rev == 0 || (rev % 25) == 0)) {
            ++g_draftLines;
            GLOGI("engine: character creator (0.5.9): draft rev %u on our ped (handle %d): %s, %d natives, %d could not be called", rev, (int)ped,
                  ok ? "applied" : "NOT applied", r.calls, r.failed);
        }
        return true;
    }

    void TickLocal(World& w, int32_t ped, uint32_t model)
    {
        if (!g_on.load() || !ped) return;
        if (g_draftShown) return;   // stage 4: the creator's draft is on our ped until the creator closes
        chr::Roster r;
        uint32_t seq = 0;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (!g_have) return;
            r = g_roster;
            seq = g_seq;
        }
        const bool redressed = g_redressed.exchange(false);
        if (!redressed && seq == g_appliedSeq && ped == g_appliedPed) return;
        const chr::Character* a = r.Active();
        if (!a) return;
        if (model != chr::ModelOf(a->look.sex))
        {
            if (g_skipSeq != seq)
            {
                g_skipSeq = seq;
                GLOGW("engine: character face (0.5.9): \"%s\" is %s but our ped is model 0x%08x: face not applied (%s)", a->Display().c_str(),
                      SexName(a->look.sex), (unsigned)model,
                      a->look.sex == chr::Sex::Female && !g_female.load() ? "char_female=0: every character is played on the male model"
                                                                          : "waiting for the ped guard to put the character's model on");
            }
            g_appliedSeq = seq;
            g_appliedPed = ped;
            return;
        }
        ApplyLogged(w, ped, a->look, "our ped", "\"" + a->Display() + "\" slot " + std::to_string(r.active) + (redressed ? " (re-dressed)" : ""));
        g_appliedSeq = seq;
        g_appliedPed = ped;
    }

    bool ApplyPeer(World& w, int32_t ped, uint32_t model, const chr::Appearance& a, int id, const std::string& who)
    {
        if (!g_on.load() || !ped || model != chr::ModelOf(a.sex)) return false;
        return ApplyLogged(w, ped, a, "peer id=" + std::to_string(id), "(" + who + ")");
    }

}  // namespace gage::game::characters
