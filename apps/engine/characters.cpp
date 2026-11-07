// characters: 0.5.9, the server's characters on this machine. The design is in characters.h and libs/sync/character.h.
#include "characters.h"

#include "hook.h"
#include "common/logger.h"
#include "common/registry.h"

#include <atomic>
#include <mutex>
#include <string>

namespace gage::game::characters {

    namespace chr = gage::sync::chr;

    namespace {

        std::atomic<bool> g_on{true};          // char_face (read at every Welcome)

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

    void ReadSwitch()
    {
        const bool on = reg::Get(L"char_face") != L"0";
        g_on.store(on);
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_roster = chr::Roster();
            g_have = g_guest = false;
        }
        g_lines.store(0);
        GLOGI("engine: characters (0.5.9): char_face=%s", on ? "on (our active character's face, hair and eyes on our ped; the other players' "
                                                               "faces on their puppets; female characters not shown yet; LIVE-UNVERIFIED)"
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
        std::lock_guard<std::mutex> lk(g_mtx);
        g_roster = r;
        g_have = true;
        g_guest = (m.flags & proto::kCharRosterGuest) != 0;
        ++g_seq;
    }

    void OnResult(const proto::CharResult& m)
    {
        const char* op = m.op >= 1 && m.op <= chr::kOpLast ? chr::ToString((chr::Op)m.op) : "?";
        const char* code = m.code <= chr::kCodeLast ? chr::ToString((chr::Code)m.code) : "?";
        if (m.code == (uint8_t)chr::Code::Ok)
            GLOGI("engine: characters (0.5.9): %s slot %u (seq %u): ok %s", op, (unsigned)m.slot, (unsigned)m.seq, m.text.c_str());
        else
            GLOGW("engine: characters (0.5.9): %s slot %u (seq %u) refused: %s %s", op, (unsigned)m.slot, (unsigned)m.seq, code, m.text.c_str());
    }

    void OnEnded()
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_have = false;
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

    void TickLocal(World& w, int32_t ped, uint32_t model)
    {
        if (!g_on.load() || !ped) return;
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
                      a->look.sex == chr::Sex::Female ? "female characters need the creator round: our ped is the male freemode model"
                                                     : "waiting for the freemode model");
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
