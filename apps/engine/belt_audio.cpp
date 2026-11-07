// 0.5.9 seatbelt sound: the player (see belt_audio.h).
#include "belt_audio.h"

#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#ifndef GAGE_BELT_AUDIO_NO_WINMM
#include <windows.h>
#include <mmsystem.h>
#endif

namespace gage::beltaudio {

    namespace bs = gage::sync::beltsound;

    struct Shared
    {
        std::mutex mu;
        std::condition_variable cv;
        std::thread* worker = nullptr;
        bool stop = false;
        bool on = true;
        int volWanted = bs::kVolDefault;
        bs::Sound pending = bs::Sound::None;
        bool playedAny = false;            // the sink was given a sound since the worker started (a stop is owed before a rebuild / at the end)
        Counters c;
        Built built;
        std::vector<uint8_t> buckle, unbuckle;   // the worker's alone (read only by the worker, outside the lock)
        Sink sink = nullptr;
    };

    namespace {

        int PeakOf(const std::vector<uint8_t>& wav)
        {
            int peak = 0;
            for (size_t i = 44; i + 1 < wav.size(); i += 2)
            {
                const int v = std::abs((int)(int16_t)(uint16_t)(wav[i] | (wav[i + 1] << 8)));
                if (v > peak)
                    peak = v;
            }
            return peak;
        }

        void Run(Shared* s)
        {
            std::unique_lock<std::mutex> lk(s->mu);
            for (;;)
            {
                s->cv.wait(lk, [s] { return s->stop || s->volWanted != s->built.vol || s->pending != bs::Sound::None; });
                if (s->stop)
                    break;
                if (s->volWanted != s->built.vol)
                {
                    // A new volume: stop what may still be reading the old images, then build both (well under a millisecond each).
                    const int vol = s->volWanted;
                    const bool stopFirst = s->playedAny;
                    lk.unlock();
                    if (stopFirst)
                        s->sink(nullptr, 0);
                    std::vector<uint8_t> b = bs::BuildWav(bs::Sound::Buckle, vol);
                    std::vector<uint8_t> u = bs::BuildWav(bs::Sound::Unbuckle, vol);
                    const int pb = PeakOf(b), pu = PeakOf(u);
                    lk.lock();
                    s->buckle.swap(b);
                    s->unbuckle.swap(u);
                    s->playedAny = false;
                    s->built.vol = vol;
                    s->built.buckleBytes = s->buckle.size();
                    s->built.unbuckleBytes = s->unbuckle.size();
                    s->built.bucklePeak = pb;
                    s->built.unbucklePeak = pu;
                    ++s->c.builds;
                    continue;
                }
                const bs::Sound snd = s->pending;
                s->pending = bs::Sound::None;               // taken: one press, one play
                const std::vector<uint8_t>& img = snd == bs::Sound::Unbuckle ? s->unbuckle : s->buckle;
                lk.unlock();
                const bool ok = s->sink(img.data(), img.size());
                lk.lock();
                s->playedAny = true;
                ++(ok ? s->c.played : s->c.failed);
            }
            const bool stopLast = s->playedAny;
            s->playedAny = false;
            lk.unlock();
            if (stopLast)
                s->sink(nullptr, 0);                        // nothing reads the images after the worker is gone
        }

    }

    Player::Player(Sink sink) : s_(new Shared)
    {
        s_->sink = sink;
    }

    Player::~Player()
    {
        Stop();
        delete s_;
    }

    void Player::Configure(bool on, int volPct)
    {
        if (volPct < 0) volPct = 0;
        if (volPct > bs::kVolMax) volPct = bs::kVolMax;
        std::lock_guard<std::mutex> lk(s_->mu);
        s_->on = on;
        s_->volWanted = volPct;
        if (!s_->worker && s_->sink)
        {
            s_->stop = false;
            s_->worker = new std::thread(Run, s_);
        }
        s_->cv.notify_one();
    }

    bool Player::Request(bs::Sound snd)
    {
        if (snd == bs::Sound::None)
            return false;
        std::lock_guard<std::mutex> lk(s_->mu);
        if (!s_->worker || s_->stop || !s_->on || s_->volWanted == 0)
        {
            ++s_->c.muted;
            return false;
        }
        ++s_->c.requested;
        ++(snd == bs::Sound::Buckle ? s_->c.buckles : s_->c.unbuckles);
        if (s_->pending != bs::Sound::None)
            ++s_->c.superseded;
        s_->pending = snd;
        s_->cv.notify_one();
        return true;
    }

    Counters Player::Take()
    {
        std::lock_guard<std::mutex> lk(s_->mu);
        const Counters c = s_->c;
        s_->c = Counters();
        return c;
    }

    Built Player::BuiltInfo() const
    {
        std::lock_guard<std::mutex> lk(s_->mu);
        return s_->built;
    }

    bool Player::On() const
    {
        std::lock_guard<std::mutex> lk(s_->mu);
        return s_->on;
    }

    int Player::Vol() const
    {
        std::lock_guard<std::mutex> lk(s_->mu);
        return s_->volWanted;
    }

    void Player::Stop()
    {
        std::thread* t = nullptr;
        {
            std::lock_guard<std::mutex> lk(s_->mu);
            t = s_->worker;
            s_->worker = nullptr;
            s_->stop = true;
            s_->pending = bs::Sound::None;
            s_->cv.notify_one();
        }
        if (!t)
            return;
        if (t->joinable())
            t->join();
        delete t;
        std::lock_guard<std::mutex> lk(s_->mu);
        s_->built = Built();                                // a restarted worker rebuilds the images
    }

#ifndef GAGE_BELT_AUDIO_NO_WINMM
    bool WinmmSink(const uint8_t* wav, size_t bytes)
    {
        if (!wav || !bytes)
            return PlaySoundW(nullptr, nullptr, 0) != FALSE;
        return PlaySoundW(reinterpret_cast<LPCWSTR>(wav), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT) != FALSE;
    }

    Player& Engine()
    {
        // Never freed: the DLL can be unloaded at process exit with the worker parked; engine::Stop joins it first.
        static Player* p = new Player(WinmmSink);
        return *p;
    }
#endif

}
