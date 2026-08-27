#!/usr/bin/env -S npx tsx
// Generates the golden vector that pins the C++ VAD port to the TypeScript one.
//
// This imports the REAL implementation from src/talk/audio-energy.ts -- not a
// reimplementation -- synthesises a deterministic frame sequence, and writes the
// per-frame energy and gate decisions to JSON. test_vad_parity.cpp replays the
// identical sequence through the C++ gate and asserts every decision matches.
//
// Both sides use the same xorshift64* and the same float pipeline, so the audio
// is bit-identical and any divergence is a real porting bug rather than a
// difference in the input.
//
//   node_modules/.bin/tsx tools/gen_vad_reference.mjs > test/vad_reference.json
import {
  readPcm16AudioStats,
  createSpeechThresholdGate,
} from "../../openclaw/src/talk/audio-energy.ts";

const MASK64 = (1n << 64n) - 1n;

class Rng {
  constructor(seed) {
    this.state = seed === 0n ? 0x9e3779b97f4a7c15n : seed;
  }
  next() {
    let s = this.state;
    s ^= s >> 12n;
    s &= MASK64;
    s ^= (s << 25n) & MASK64;
    s ^= s >> 27n;
    this.state = s;
    return (s * 0x2545f4914f6cdd1dn) & MASK64;
  }
  // Mirrors Rng::uniform() in src/vhp_producer.cpp exactly.
  uniform() {
    const bits = this.next() >> 11n; // 53 bits
    return Number(bits) / 2 ** 52 - 1.0;
  }
}

const SAMPLE_RATE = 24_000;
const FRAME_MS = 20;
const SAMPLES = (SAMPLE_RATE * FRAME_MS) / 1000;
const PCM16_MAX = 32_768;

const config = {
  seed: 0xc0ffeen,
  frames: 600,
  speechMs: 1500,
  gapMs: 900,
  amplitude: 0.25,
  noise: 0.004,
  gate: { rmsThreshold: 0.02, peakThreshold: 0.1, speechFrames: 2, silenceFrames: 12 },
};

const rng = new Rng(config.seed);
const gate = createSpeechThresholdGate(config.gate);
const cycleMs = config.speechMs + config.gapMs;
const frames = [];

for (let seq = 0; seq < config.frames; seq += 1) {
  const phaseMs = (seq * FRAME_MS) % cycleMs;
  const amplitude = phaseMs < config.speechMs ? config.amplitude : config.noise;
  const pcm = Buffer.alloc(SAMPLES * 2);
  for (let i = 0; i < SAMPLES; i += 1) {
    // Math.trunc matches the C++ double -> int16_t narrowing conversion.
    pcm.writeInt16LE(Math.trunc(rng.uniform() * amplitude * 32767.0), i * 2);
  }
  const stats = readPcm16AudioStats(pcm);
  const normalized = { rms: stats.rms / PCM16_MAX, peak: stats.peak / PCM16_MAX };
  const accepted = gate.accept(normalized, { nowMs: seq * FRAME_MS });
  frames.push({
    seq,
    rms: normalized.rms,
    peak: normalized.peak,
    accepted,
  });
}

process.stdout.write(JSON.stringify({ config: { ...config, seed: "0xc0ffee" }, frames }, null, 1));
