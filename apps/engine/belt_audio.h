// 0.5.9 seatbelt sound: the player. The sounds and the decision are libs/sync/belt_sound.h (pure); this is the thread that plays them.
//
// Why a worker thread: the engine asks for a sound from the script tick (TickVehicleControls), and nothing that can stall may run there or in
// the Present hook. winmm's PlaySound opens the audio device on its first call (tens of ms, longer on a slow endpoint), so the tick only stores
// the request in a one-slot mailbox (a mutex held for a few instructions, never across the sink) and wakes the worker; the worker owns the WAV
// images and is the only caller of the sink. PlaySound(SND_MEMORY | SND_ASYNC | SND_NODEFAULT) then returns at once and winmm mixes the
// sound on its own thread through the shared-mode endpoint, beside GTA's own XAudio2 output (same process, same audio session: the Windows
// volume mixer's slider for the game moves it too; GTA's in-game volume settings do not). SND_ASYNC with SND_MEMORY reads the image while it
// plays, so an image is rebuilt (a belt_sound_vol change) only after a synchronous stop (the sink called with nullptr). A new sound cuts the
// one playing (PlaySound has one voice): a buckle and an unbuckle never overlap.
//
// Never twice per press: the mailbox holds one sound; a request while one is still waiting replaces it (counted as superseded) and the worker
// clears the slot when it takes it.
//
// Portable C++ (std::thread): the sink is a function pointer, so tests drive the player with a fake sink and nothing is ever audible there.
// The real sink (WinmmSink) is compiled out with GAGE_BELT_AUDIO_NO_WINMM (tests/belt_sound_tests.h).
#pragma once

#include "sync/belt_sound.h"

#include <cstddef>
#include <cstdint>

namespace gage::beltaudio {

    // Plays one WAV image asynchronously; wav == nullptr = stop whatever plays, synchronously. True = accepted.
    using Sink = bool (*)(const uint8_t* wav, size_t bytes);

    struct Counters
    {
        uint32_t requested = 0;    // sounds asked for while on
        uint32_t played = 0;       // the sink accepted
        uint32_t failed = 0;       // the sink refused (no device, ...)
        uint32_t superseded = 0;   // a request replaced one still waiting in the mailbox
        uint32_t muted = 0;        // asked for while belt_sound=0 or belt_sound_vol=0
        uint32_t builds = 0;       // the WAV images were (re)built (start, a volume change)
        uint32_t buckles = 0, unbuckles = 0;   // what was requested
    };

    struct Built
    {
        int vol = -1;                                   // the volume the images hold (-1 = not built yet)
        size_t buckleBytes = 0, unbuckleBytes = 0;
        int bucklePeak = 0, unbucklePeak = 0;           // the largest |sample| of each
    };

    struct Shared;

    class Player
    {
    public:
        explicit Player(Sink sink);
        ~Player();                                      // Stop()
        Player(const Player&) = delete;
        Player& operator=(const Player&) = delete;

        void Configure(bool on, int volPct);            // any thread; starts the worker on the first call
        bool Request(sync::beltsound::Sound s);         // any thread, never waits for the sink; false = None / muted / stopped
        Counters Take();                                // the counters since the last Take
        Built BuiltInfo() const;
        bool On() const;
        int  Vol() const;
        void Stop();                                    // the worker ends (joined) after stopping the playback; idempotent

    private:
        Shared* s_;
    };

    // The engine's player (WinmmSink), created on the first use, never freed (Stop() ends its worker in engine::Stop).
    Player& Engine();

#ifndef GAGE_BELT_AUDIO_NO_WINMM
    bool WinmmSink(const uint8_t* wav, size_t bytes);
#endif

}
