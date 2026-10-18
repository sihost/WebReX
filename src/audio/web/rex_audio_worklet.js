// ReXGlue web audio worklet processor (Workstream E).
//
// Embedded into web_audio_output.cpp with #embed and loaded through a Blob URL, so it
// needs no extra file next to halo3.js. Reads stereo frames from the ring the SDK audio
// driver writes in the wasm SharedArrayBuffer; layout in include/rex/audio/web/web_audio_output.h.
// Each ring slot is one guest frame (256 interleaved stereo samples = two render quanta).
'use strict';

const CTL_WRITE = 0, CTL_READ = 1, CTL_QUANTA = 5, CTL_UNDERRUN = 6, CTL_PLAYED = 7,
  CTL_PEAK = 10, CTL_MUTED = 12, CTL_STOP = 16, CTL_PRIME = 18, CTL_COUNT = 32;
// After an underrun, wait for this many queued frames (10.7 ms) before playing again, so
// a producer that is just keeping up does not alternate frame / silence quantum.
const PRIME_FRAMES = 2;
// A bursty producer (a hidden tab's guest threads get less CPU) underruns again and again:
// each underrun that follows the previous one within PRIME_RAISE_QUANTA raises the prime
// level by a frame, up to PRIME_FRAMES_MAX (32 ms, below the guest's default 8 frames in
// flight), so gaps become fewer and longer instead of a crackle; it drops back by a frame
// after PRIME_DECAY_QUANTA without an underrun.
const PRIME_FRAMES_MAX = 6;
const PRIME_RAISE_QUANTA = 375;   // 1 s
const PRIME_DECAY_QUANTA = 3750;  // 10 s

class RexAudioProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const o = options.processorOptions;
    this.ctl = new Int32Array(o.buffer, o.ctl, CTL_COUNT);
    this.ctlF = new Float32Array(o.buffer, o.ctl, CTL_COUNT);
    this.slots = o.slots;
    this.slotSamples = o.slotSamples;
    this.channels = o.channels;
    this.ring = new Float32Array(o.buffer, o.ring, o.slots * o.slotSamples * o.channels);
    this.cur = -1;  // frame counter of the slot being played, -1 = none
    this.pos = 0;   // sample position inside that slot
    this.primed = false;
    this.peak = 0;
    this.prime = PRIME_FRAMES;
    this.quantum = 0;
    this.lastUnderrun = -1e9;
    Atomics.store(this.ctl, CTL_PRIME, this.prime);
  }

  process(inputs, outputs) {
    const ctl = this.ctl;
    if (Atomics.load(ctl, CTL_STOP)) return false;
    Atomics.add(ctl, CTL_QUANTA, 1);
    const out = outputs[0];
    const left = out[0], right = out[1] || out[0];
    const n = left.length;
    const muted = Atomics.load(ctl, CTL_MUTED) !== 0;
    const ss = this.slotSamples;
    const q = ++this.quantum;
    if (this.prime > PRIME_FRAMES && q - this.lastUnderrun > PRIME_DECAY_QUANTA) {
      --this.prime;
      this.lastUnderrun = q;
      Atomics.store(this.ctl, CTL_PRIME, this.prime);
    }
    let i = 0;
    while (i < n) {
      if (this.cur < 0) {
        const r = Atomics.load(ctl, CTL_READ);
        const avail = (Atomics.load(ctl, CTL_WRITE) - r) | 0;
        if (avail <= 0 || (!this.primed && avail < this.prime)) {
          // Underrun: the rest of this quantum stays silent (outputs start zeroed).
          if (avail <= 0 && this.primed) {
            this.primed = false;
            if (q - this.lastUnderrun < PRIME_RAISE_QUANTA && this.prime < PRIME_FRAMES_MAX) {
              ++this.prime;
              Atomics.store(ctl, CTL_PRIME, this.prime);
            }
            this.lastUnderrun = q;
          }
          Atomics.add(ctl, CTL_UNDERRUN, 1);
          break;
        }
        this.primed = true;
        this.cur = r;
        this.pos = 0;
        this.peak = 0;
      }
      const base = ((this.cur >>> 0) % this.slots) * ss * 2;
      const take = Math.min(n - i, ss - this.pos);
      const ring = this.ring;
      let peak = this.peak;
      for (let k = 0; k < take; ++k) {
        const s = base + (this.pos + k) * 2;
        const l = ring[s], rr = ring[s + 1];
        const al = l < 0 ? -l : l, ar = rr < 0 ? -rr : rr;
        if (al > peak) peak = al;
        if (ar > peak) peak = ar;
        if (!muted) {
          left[i + k] = l;
          if (right !== left) right[i + k] = rr;
        }
      }
      this.peak = peak;
      i += take;
      this.pos += take;
      if (this.pos >= ss) {
        // Frame done: hand it back. The CAS fails only if the SDK's free-run pacing took
        // this frame while this worklet was not being scheduled; then it is not counted.
        const c = this.cur;
        if (Atomics.compareExchange(ctl, CTL_READ, c, (c + 1) | 0) === c) {
          Atomics.add(ctl, CTL_PLAYED, 1);
        }
        Atomics.notify(ctl, CTL_READ);
        this.ctlF[CTL_PEAK] = this.peak;
        this.cur = -1;
      }
    }
    return true;
  }
}

registerProcessor('rex-audio', RexAudioProcessor);
