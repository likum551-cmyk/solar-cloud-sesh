// The PLAY shutter, synthesised with WebAudio at the moment it plays: no audio file ships, so
// its provenance is this code (authored for GAGE-MP, ASSETS.md). A flash capacitor whine that
// is cut off by two mechanical clicks (mirror up, curtain) and a short body thump.

let ctx: AudioContext | null = null;

function noiseBuffer(ac: AudioContext, seconds: number): AudioBuffer {
  const buf = ac.createBuffer(1, Math.ceil(ac.sampleRate * seconds), ac.sampleRate);
  const ch = buf.getChannelData(0);
  let seed = 0x9e3779b9;
  for (let i = 0; i < ch.length; i++) {
    // xorshift: deterministic, so every shutter sounds the same
    seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5;
    ch[i] = ((seed >>> 0) / 0xffffffff) * 2 - 1;
  }
  return buf;
}

function click(ac: AudioContext, out: AudioNode, at: number, freq: number, gain: number, decay: number) {
  const src = ac.createBufferSource();
  src.buffer = noiseBuffer(ac, decay + 0.02);
  const bp = ac.createBiquadFilter();
  bp.type = 'bandpass';
  bp.frequency.value = freq;
  bp.Q.value = 1.4;
  const g = ac.createGain();
  g.gain.setValueAtTime(0, at);
  g.gain.linearRampToValueAtTime(gain, at + 0.0015);
  g.gain.exponentialRampToValueAtTime(0.0008, at + decay);
  src.connect(bp).connect(g).connect(out);
  src.start(at);
  src.stop(at + decay + 0.02);
}

/**
 * Play the shutter. `whineMs` is how long the flash charges before the clicks.
 * Resolves silently when audio is unavailable (no device, autoplay policy).
 */
export function playShutter(whineMs = 260, volume = 0.5): void {
  try {
    ctx ??= new AudioContext();
    const ac = ctx;
    if (ac.state === 'suspended') void ac.resume();
    const t0 = ac.currentTime + 0.01;
    const master = ac.createGain();
    master.gain.value = volume;
    const comp = ac.createDynamicsCompressor();
    comp.threshold.value = -10;
    comp.ratio.value = 6;
    master.connect(comp).connect(ac.destination);

    // flash charge whine: a thin sine sweeping up, quiet
    const w = whineMs / 1000;
    const osc = ac.createOscillator();
    osc.type = 'sine';
    osc.frequency.setValueAtTime(2400, t0);
    osc.frequency.exponentialRampToValueAtTime(7200, t0 + w);
    const wg = ac.createGain();
    wg.gain.setValueAtTime(0, t0);
    wg.gain.linearRampToValueAtTime(0.035, t0 + w * 0.6);
    wg.gain.linearRampToValueAtTime(0, t0 + w);
    osc.connect(wg).connect(master);
    osc.start(t0);
    osc.stop(t0 + w + 0.02);

    // mirror slap, curtain, thump
    const tc = t0 + w;
    click(ac, master, tc, 3200, 0.9, 0.035);
    click(ac, master, tc + 0.004, 900, 0.55, 0.06);
    click(ac, master, tc + 0.072, 2600, 0.7, 0.03);
    const thump = ac.createOscillator();
    thump.type = 'sine';
    thump.frequency.setValueAtTime(140, tc);
    thump.frequency.exponentialRampToValueAtTime(55, tc + 0.08);
    const tg = ac.createGain();
    tg.gain.setValueAtTime(0.4, tc);
    tg.gain.exponentialRampToValueAtTime(0.001, tc + 0.1);
    thump.connect(tg).connect(master);
    thump.start(tc);
    thump.stop(tc + 0.12);
  } catch (err) {
    console.warn('[shutter] audio unavailable:', err);
  }
}
