#!/usr/bin/env node
// The Node side of the bridge, and the baseline it exists to beat.
//
// Two jobs:
//   1. Show how the existing TypeScript gateway feeds the C++ hot path today --
//      a unix socket and a 40-byte header, no native addon, no rebuild of the
//      Node runtime.
//   2. Be the honest control. Run it with --gc-pressure and the send-side
//      lateness distribution grows a tail that has nothing to do with audio and
//      everything to do with V8. That tail is what moving the path out of the
//      runtime removes, and quoting it next to the C++ producer's numbers is the
//      whole argument.
//
// On what actually induces the tail: churning Buffers does almost nothing --
// they are off-heap and the collector barely tracks them. Retained *object
// graphs* are what cost, and the cost scales with the live set, because that is
// what a major GC has to trace. --gc-pressure-mb sizes that live set; measured
// on a 4-core GCP VM, p50 lateness stayed at 0.00 ms across the whole range
// while p99.9 went 6.7 ms at 32 MB -> 12.6 ms at 128 MB -> 17.9 ms at 384 MB.
// A mean-based dashboard sees none of this.
//
// Compare like with like: report p99/p99.9 of *lateness against the intended
// grid*, not the mean interval. The mean interval is 20 ms in every
// configuration, healthy or not.
//
//   node tools/node_producer.mjs --uds /tmp/vhp.sock --seconds 30 --gc-pressure
//   node tools/node_producer.mjs --uds /tmp/vhp.sock --sessions 16
import net from "node:net";
import { Buffer } from "node:buffer";

const args = new Map();
for (let i = 2; i < process.argv.length; i += 1) {
  const arg = process.argv[i];
  if (!arg.startsWith("--")) continue;
  const key = arg.slice(2);
  const next = process.argv[i + 1];
  if (next && !next.startsWith("--")) { args.set(key, next); i += 1; } else { args.set(key, "true"); }
}

const config = {
  socket: args.get("uds") ?? "/tmp/vhp.sock",
  seconds: Number(args.get("seconds") ?? 30),
  frameMs: Number(args.get("frame-ms") ?? 20),
  sampleRate: Number(args.get("rate") ?? 24_000),
  speechMs: Number(args.get("speech-ms") ?? 1500),
  gapMs: Number(args.get("gap-ms") ?? 900),
  amplitude: Number(args.get("amplitude") ?? 0.25),
  noise: Number(args.get("noise") ?? 0.004),
  sessions: Math.max(1, Number(args.get("sessions") ?? 1)),
  gcPressure: args.has("gc-pressure") || args.has("gc-pressure-mb"),
  gcPressureMb: Number(args.get("gc-pressure-mb") ?? 128),
};

const SAMPLES = Math.round((config.sampleRate * config.frameMs) / 1000);
const PAYLOAD_BYTES = SAMPLES * 2;
const HEADER_BYTES = 40;
const FORMAT_PCM16 = 0;

// Must match FrameHeader in include/vhp/audio_frame.hpp. The static_asserts in
// include/vhp/pipeline.hpp pin the C++ side; this comment and those asserts are
// the contract. Change one without the other and the consumer reads garbage.
function writeHeader(buf, { seq, tProduceNs, tCaptureNs, byteLen, sampleRate, flags, sessionId }) {
  buf.writeBigUInt64LE(BigInt(seq), 0);
  buf.writeBigUInt64LE(tProduceNs, 8);
  buf.writeBigUInt64LE(tCaptureNs, 16);
  buf.writeUInt32LE(byteLen, 24);
  buf.writeUInt32LE(sampleRate, 28);
  buf.writeUInt16LE(FORMAT_PCM16, 32);
  buf.writeUInt16LE(flags, 34);
  buf.writeUInt32LE(sessionId, 36);  // demultiplexed by the C++ session table
}

function percentile(sorted, p) {
  if (sorted.length === 0) return 0;
  const index = Math.min(sorted.length - 1, Math.ceil((p / 100) * sorted.length) - 1);
  return sorted[Math.max(0, index)];
}

const socket = net.createConnection(config.socket);
socket.on("error", (error) => {
  console.error(`[node-producer] ${error.message}`);
  process.exit(1);
});

socket.on("connect", () => {
  const totalFrames = Math.floor((config.seconds * 1000) / config.frameMs);
  const frameNs = BigInt(Math.round(config.frameMs * 1e6));
  const startNs = process.hrtime.bigint();
  const cycleMs = config.speechMs + config.gapMs;

  // Preallocated: reallocating per frame would add allocation pressure of our
  // own and confound the --gc-pressure comparison.
  const wire = Buffer.allocUnsafe(4 + HEADER_BYTES + PAYLOAD_BYTES);
  wire.writeUInt32LE(PAYLOAD_BYTES, 0);
  const payload = wire.subarray(4 + HEADER_BYTES);
  const header = wire.subarray(4, 4 + HEADER_BYTES);

  const lateness = [];
  let seq = 0;
  let retained = [];
  let retainedBytes = 0;

  const tick = () => {
    if (seq >= totalFrames) { finish(); return; }
    const targetNs = startNs + BigInt(seq) * frameNs;
    const nowNs = process.hrtime.bigint();
    lateness.push(Number(nowNs > targetNs ? nowNs - targetNs : 0n) / 1e6);

    // Deliberate churn of retained object graphs, to make V8 do what it does in
    // a real gateway holding session state, transcripts and tool-call ledgers.
    // ~200 B/object is a rough but stable estimate; the knob is the shape of the
    // curve, not a byte-accurate heap target.
    if (config.gcPressure) {
      const batch = [];
      for (let i = 0; i < 2000; i += 1) {
        batch.push({ a: i, b: "x".repeat(64), c: [i, i + 1, i + 2] });
      }
      retained.push(batch);
      retainedBytes += 2000 * 200;
      if (retainedBytes > config.gcPressureMb * 1024 * 1024) {
        retained = [];
        retainedBytes = 0;
      }
    }

    for (let sessionId = 0; sessionId < config.sessions; sessionId += 1) {
      // Stagger each call across the utterance cycle. Synchronised callers all
      // go quiet at the same instant, so the endpoint work never overlaps and
      // the measured capacity comes out flattering and wrong.
      const offsetMs = (cycleMs * sessionId) / config.sessions;
      const phaseMs = (seq * config.frameMs + offsetMs) % cycleMs;
      const amplitude = phaseMs < config.speechMs ? config.amplitude : config.noise;
      for (let i = 0; i < SAMPLES; i += 1) {
        payload.writeInt16LE(Math.trunc((Math.random() * 2 - 1) * amplitude * 32767), i * 2);
      }
      writeHeader(header, {
        seq,
        tProduceNs: process.hrtime.bigint(),
        tCaptureNs: targetNs,
        byteLen: PAYLOAD_BYTES,
        sampleRate: config.sampleRate,
        flags: seq === 0 ? 1 : 0,
        sessionId,
      });
      socket.write(wire);
    }
    seq += 1;

    // Self-correcting: schedule against the absolute grid rather than sleeping
    // for frameMs, so timer slop does not accumulate into drift. Node's timer
    // resolution is ~1 ms, which is itself part of what this baseline measures.
    const nextTargetNs = startNs + BigInt(seq) * frameNs;
    const delayMs = Number(nextTargetNs - process.hrtime.bigint()) / 1e6;
    setTimeout(tick, Math.max(0, delayMs));
  };

  const finish = () => {
    socket.end();
    const sorted = [...lateness].sort((a, b) => a - b);
    const mean = lateness.reduce((a, b) => a + b, 0) / (lateness.length || 1);
    console.error("\n--- node producer (baseline) ---------------------------------------");
    console.error(`send lateness vs ${config.frameMs} ms grid, n=${sorted.length}` +
                  (config.gcPressure ? `  [gc-pressure ${config.gcPressureMb} MB live set]` : "  [gc-pressure off]"));
    console.error(
      `  p50=${percentile(sorted, 50).toFixed(3)}  p90=${percentile(sorted, 90).toFixed(3)}` +
      `  p99=${percentile(sorted, 99).toFixed(3)}  p99.9=${percentile(sorted, 99.9).toFixed(3)}` +
      `  max=${(sorted.at(-1) ?? 0).toFixed(3)}  mean=${mean.toFixed(3)} ms`);
    if (sorted.length < 1000) {
      console.error(`  note: n=${sorted.length} -- p99.9 is ~${(sorted.length / 1000).toFixed(1)} ` +
                    "samples deep, treat as indicative");
    }
    console.error("--------------------------------------------------------------------");
    process.exit(0);
  };

  tick();
});
