// The PLAY cue, synthesised with WebAudio when it plays: no audio file ships, so its
// provenance is this code (authored for GAGE-MP, ASSETS.md). A rush of filtered air that rises
// while the emblem charges and the footage pushes in, cut by a low thump as the picture drops
// to black and the launch log takes over.

let ctx: AudioContext | null = null;

function noise(ac: AudioContext, seconds: number): AudioBuffer {
  const buf = ac.createBuffer(1, Math.ceil(ac.sampleRate * seconds), ac.sampleRate);
  const ch = buf.getChannelData(0);
  let seed = 0x2d6eb9;
  for (let i = 0; i < ch.length; i++) {
    // xorshift: deterministic, so every launch sounds the same
    seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5;
    ch[i] = ((seed >>> 0) / 0xffffffff) * 2 - 1;
  }
  return buf;
}

/**
 * Play the cue. `riseMs` is the rush before the thump (match the charge). Silent when audio
 * is unavailable (no output device, autoplay policy); the launch never waits on it.
 */
export function playLaunch(riseMs = 460, volume = 0.45): void {
  try {
    ctx ??= new AudioContext();
    const ac = ctx;
    if (ac.state === 'suspended') void ac.resume();
    const t0 = ac.currentTime + 0.01;
    const rise = riseMs / 1000;
    const master = ac.createGain();
    master.gain.value = volume;
    const comp = ac.createDynamicsCompressor();
    comp.threshold.value = -12;
    comp.ratio.value = 4;
    master.connect(comp).connect(ac.destination);

    // the rush: band-passed noise sweeping up, swelling, then cut
    const air = ac.createBufferSource();
    air.buffer = noise(ac, rise + 0.2);
    const bp = ac.createBiquadFilter();
    bp.type = 'bandpass';
    bp.Q.value = 0.9;
    bp.frequency.setValueAtTime(380, t0);
    bp.frequency.exponentialRampToValueAtTime(2600, t0 + rise);
    const ag = ac.createGain();
    ag.gain.setValueAtTime(0.0001, t0);
    ag.gain.exponentialRampToValueAtTime(0.32, t0 + rise * 0.92);
    ag.gain.exponentialRampToValueAtTime(0.0001, t0 + rise + 0.06);
    air.connect(bp).connect(ag).connect(master);
    air.start(t0);
    air.stop(t0 + rise + 0.2);

    // the thump: a falling sine with a short click on its front
    const tc = t0 + rise;
    const body = ac.createOscillator();
    body.type = 'sine';
    body.frequency.setValueAtTime(120, tc);
    body.frequency.exponentialRampToValueAtTime(42, tc + 0.22);
    const bg = ac.createGain();
    bg.gain.setValueAtTime(0.0001, tc);
    bg.gain.exponentialRampToValueAtTime(0.75, tc + 0.008);
    bg.gain.exponentialRampToValueAtTime(0.0001, tc + 0.32);
    body.connect(bg).connect(master);
    body.start(tc);
    body.stop(tc + 0.34);
  } catch (err) {
    console.warn('[launch cue] audio unavailable:', err);
  }
}
