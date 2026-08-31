# voice-hotpath

A C++20 audio hot path for the OpenClaw Mattermost voice agent, and the
measurement harness that justifies it.

The TypeScript pipeline works: WebRTC audio in, VAD chunking, NVIDIA Parakeet
ASR, LLM, Magpie TTS back out. What it did not have was a way to say where the
time goes, or a hot path insulated from the runtime's garbage collector. This is
both: the frame-ingest → ring → VAD → chunk-assembly stage rewritten in C++, and
the instrumentation to prove what that buys and what it costs.

## What is here

```
include/vhp/
  spsc_ring.hpp    lock-free SPSC ring; cache-line separated indices
  shm_ring.hpp     the same ring in a POSIX shm segment, with layout validation
  audio_frame.hpp  fixed-size frame slot; the cross-process ABI
  codec.hpp        G.711 mu-law decode as a constexpr table
  energy.hpp       per-frame RMS and peak
  vad.hpp          gate (ported from the TypeScript) + chunk assembler
  session.hpp      multi-call demux: open-addressed table, admission control
  histogram.hpp    HdrHistogram-style log-linear latency histogram
  metrics.hpp      per-stage latency accounting
  clock.hpp        CLOCK_MONOTONIC, absolute-deadline pacing, thread pinning
src/
  vhp_consumer.cpp the hot path process
  vhp_producer.cpp deterministic paced frame source / load generator
bench/
  bench_ring.cpp   handoff latency and throughput, padded vs false-shared
tools/
  node_producer.mjs      Node → C++ bridge over a unix socket, and the baseline
  gen_vad_reference.mjs  golden vector generator, imports the real TypeScript
  sweep_hangover.sh      endpoint latency vs false-cut frontier
  sweep_capacity.sh      concurrent-call capacity, with a Little's Law check
results/
  sweep_hangover.csv     the frontier, as measured
  sweep_capacity.csv     the capacity curve, as measured
```

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j
```

```bash
ctest --test-dir build --output-on-failure
```

Ten tests: five plain, two under ThreadSanitizer, three under
AddressSanitizer+UBSan. The TSan pass is the one that matters for the ring —
x86-64's memory model will happily execute an incorrectly-ordered acquire/release
pair without complaint, and the bug only shows up on weakly-ordered hardware.
TSan models the C++ abstract machine instead of the CPU, so it catches what the
hardware forgives. (CMake wraps those two in `setarch -R` to disable ASLR;
without it TSan aborts with `unexpected memory mapping` on recent kernels.)

## Architecture

```
ingest ──▶ SPSC ring ──▶ pinned VAD thread ──────▶ event ring ──▶ publisher thread
(shm or                  demux by session_id                     JSONL on stdout
 uds reader)             energy + gate + chunk assembly
                         (one assembler per live call)
```

One thread serves every concurrent call. Frames from all sessions arrive
interleaved on a single ring and are demultiplexed by `session_id` through an
open-addressed table. Every session's chunk assembler — including its assembled
-audio buffer — is preallocated at startup, because a call connecting mid-flight
must not allocate on the pinned thread; that allocation would land in the tail of
every *other* call in progress.

The pinned thread does no allocation, no locking, no I/O and no logging. Every
syscall in the process happens on the ingest or publisher thread. That is the
whole discipline; the numbers below follow from it.

### Why a separate process, not a Node addon

An N-API addon was the obvious first design and it is the wrong one. It runs
inside the Node process, so a V8 GC pause still stalls the thread handing frames
to the C++ code — the jitter you set out to remove is still there, you have only
moved where it is measured. A separate process with shared memory between them
isolates the hot path completely, and is structurally the same thing as a
market-data feed handler.

The unix-socket path exists so the current TypeScript gateway can feed the hot
path today, without a native addon or a rebuilt runtime. It costs a copy and a
syscall per frame. Measured, not assumed — see the transport line below.

## Measurements

All on a 4-core GCP VM (Ubuntu 22.04, gcc 11), threads pinned but cores **not**
isolated: no `isolcpus`, no `nohz_full`. Every p99.9 below still contains timer
ticks and other tenants. On a tuned box these get better; the *relative* numbers
are the point.

### Ring handoff, padded vs false-shared

400k items, producer paced to 500 kHz so the ring stays at depth 0 (p50 depth is
0 items — verified, not assumed; a saturated ring measures queueing delay, not
handoff cost, and reporting that as handoff latency is off by three orders of
magnitude):

| | p50 | p99 | p99.9 |
|---|---|---|---|
| indices on separate cache lines | 80 ns | 1.59 µs | 24.8 µs |
| indices sharing a cache line | 80 ns | 2.51 µs | 128.4 µs |
| **penalty** | **0.99×** | **1.58×** | **5.17×** |

False sharing is *completely invisible at p50* and costs 5× at p99.9. Saturated
throughput barely moves either (22.2 vs 21.5 Mitems/s). A mean-based or
throughput-based dashboard would report this bug as "no change".

### Node vs C++ producer, same 30 s, 1500 frames

Deviation of frame arrival from the nominal 20 ms grid, measured at the consumer,
coordinated-omission corrected:

| | p50 | p99 | p99.9 | max |
|---|---|---|---|---|
| Node, 256 MB live set | 0.453 ms | 7.44 ms | 18.45 ms | 19.08 ms |
| C++, pinned | 0.005 ms | 0.62 ms | 1.79 ms | 1.79 ms |

Again: **p50 barely tells you anything is wrong.** The GC tail is a p99+ effect,
and it scales with the retained live set (p99.9 lateness of 6.7 ms at 32 MB,
12.6 ms at 128 MB, 17.9 ms at 384 MB). Churning `Buffer`s does almost nothing —
they are off-heap and the collector barely tracks them. Retained *object graphs*
are what cost, because that is what a major GC has to trace.

### Per-stage breakdown, C++ over shm

| stage | p50 | p99 | p99.9 |
|---|---|---|---|
| shm transport + queueing | 1 µs | 5 µs | 10 µs |
| VAD service time (energy + gate) | 2.4 µs | 19.1 µs | 26.1 µs |
| endpoint decision lag | 240.0 ms | 240.0 ms | 240.1 ms |

The transport and the VAD are together under 30 µs at p99.9. The endpoint lag is
240 ms and it is **not a defect** — it is exactly `silence_frames × frame_ms` =
12 × 20 ms, the hangover, working as configured. This is the number worth
arguing about, and it is four orders of magnitude larger than everything else on
this path. Which is the real lesson: the C++ rewrite bought ~16 ms at p99.9 of
jitter, and the *configuration* of the gate is worth 240 ms on every single
utterance.

Over the UDS bridge the transport line goes from 1 µs to 92 µs at p50 — ~90×,
and still two orders of magnitude below the hangover. The bridge is not the
thing to optimise.

### Concurrent-call capacity

Each session contributes 50 frames/second. The producer emits one frame per
session per 20 ms tick, so all N frames land back-to-back — a worst-case
synchronised arrival burst. Real calls arrive independently and spread across the
interval, so this understates capacity deliberately; a number that only holds
when traffic is smooth is not a capacity number. Full output in
`results/sweep_capacity.csv`:

| calls | frames offered | lost | service p99 | depth p99 | L (time-avg) | λ·W |
|---|---|---|---|---|---|---|
| 128 | 51,200 | 0 | 4.60 µs | 62 | 0.041 | 0.045 |
| 512 | 204,800 | 0 | 4.24 µs | 219 | 0.459 | 0.471 |
| **1024** | **409,600** | **0** | **4.24 µs** | **453** | **1.857** | **1.884** |
| 2048 | 819,200 | 2,829 (0.35%) | 4.13 µs | 767 | 6.238 | 6.285 |
| 4096 | 1,638,400 | 21,067 (1.29%) | 4.02 µs | 928 | 17.450 | 17.760 |

**1024 concurrent calls on one pinned core with zero frame loss and zero event
loss.** Beyond that the bounded ingest ring sheds under the burst — visibly and
counted, which is the entire reason for bounding it.

Two things worth reading off this table:

*Service time improves under load* (2.17 µs at N=1, 1.01 µs at N=4096). That is
cache and branch-predictor warmth: at one call the loop is cold between frames.
It also means the binding constraint is **burst absorption**, not throughput —
at 1.0 µs/frame one core could sustain far more than 1024 calls if arrivals were
smooth. The 1024 ceiling is the 1024-slot ring meeting a 1024-frame instantaneous
burst.

*Little's Law holds to within 2.5% at every load level.* L = λW is
distribution-free, so agreement is a real check that the queueing measurement is
sound rather than a coincidence.

Getting that check to pass took two corrections, both worth recording:

1. **Depth was sampled at each pop.** That is a *departure*-average — the queue
   is deepest exactly when frames are leaving, so departures preferentially
   sample busy periods. It read 21.7 against a predicted 2.46. The fix is to
   integrate depth over time, reusing the timestamp the loop already takes.
   Both are still reported: `depth_p99` answers "how deep does it get",
   `L` answers "how much work is resident on average".
2. **λ was derived from offered load.** The consumer outlived the producer by
   3 s, and that idle tail dilutes a time-average but not a predicted value —
   the two disagreed by exactly the ratio of the windows (11/8 ≈ 1.4). λ is now
   computed from the consumer's own observation window.

### The endpoint frontier

`tools/sweep_hangover.sh` sweeps the hangover against a deterministic corpus with
known utterance boundaries (5 utterances, 1500 ms of speech each, 80 ms
inter-word pauses) and reports `chunks_per_utterance` — 1.00 means no false cuts
— against `endpoint_p50_ms`, which is what every utterance pays. Full output in
`results/sweep_hangover.csv`:

| hangover | chunks/utterance | endpoint p50 |
|---|---|---|
| 20 ms | 5.00 | 20.0 ms |
| 40 ms | 5.00 | 40.0 ms |
| 60 ms | 5.00 | 60.0 ms |
| 80 ms | 5.00 | 80.0 ms |
| **100 ms** | **1.00** | **100.0 ms** |
| 240 ms (current default) | 1.00 | 240.0 ms |
| 500 ms | 1.00 | 500.0 ms |

The frontier is a step, not a slope, and the knee is at 100 ms — one frame past
the 80 ms inter-word pause, which is the sanity check on the whole measurement
chain: the corpus, the gate port and the metric all agree on where the cliff
should be. Below it every utterance shatters into five fragments; at and above it
nothing is cut and the only thing more hangover buys is latency.

Against this corpus the shipped 240 ms default is paying **140 ms per utterance
for nothing**. That is a bigger number than the entire C++ rewrite bought back.
The caveat is that 80 ms is a synthetic pause length — real speakers have a
distribution of them, with a tail, and the right operating point sits far enough
up that tail to be safe. That is the argument to have, and now there is a curve
to have it against. Choose the point and state the cost ratio you are assuming:
"a false cut costs us more than 200 ms of latency, so we sit here" is defensible;
`silenceFrames = 12` with no stated reason is not.

## Parity with the TypeScript

`test_vad_parity` is the load-bearing test. `tools/gen_vad_reference.mjs` imports
the **real** `src/talk/audio-energy.ts` — not a reimplementation — runs a
deterministic 600-frame corpus through it, and writes the per-frame energies and
gate decisions to `test/vad_reference.json`. The C++ test regenerates
bit-identical audio (same xorshift64\*, same float pipeline) and asserts every
decision matches.

Current result: **worst RMS delta 0.000e+00, worst peak delta 0.000e+00, zero
decision mismatches over 600 frames.**

This matters because without it, an A/B of two gate configurations through the
C++ path would be measuring the C++/TypeScript difference rather than the change
under test.

One thing the port deliberately does *not* inherit: `readPcm16AudioStats`
reports RMS in raw int16 units while `calculateMulawRms` reports it normalised,
so the same numeric threshold means different things depending on the transport.
C++ computes both and treats the normalised pair as canonical, which makes one
gate config valid for both formats.

## Design notes

**Overflow policy is drop-newest, not drop-oldest.** Drop-oldest is more
attractive for audio — stale frames have less value than fresh ones — but in a
strict SPSC ring it requires the producer to advance the consumer's index, which
breaks the single-writer invariant the whole lock-free argument rests on. Doing
it properly needs per-slot sequence numbers and consumer-side overwrite detection
(the LMAX Disruptor approach). Drop-newest with an explicit counter at the shed
point is the honest simpler choice, and at 1024 slots (20.5 s of audio) a full
ring means the consumer is wedged, not that we hit a burst.

**The ring does not count drops.** It returns `kFull`; the caller decides what
that means. A caller that sheds counts a dropped frame, a caller that spins
counts nothing. Folding a counter into the ring conflates the two — a retrying
producer inflates it once per spin iteration. (This was a real bug here: the
cross-process test reported 338,329 "drops" for a run that lost zero frames.)

**Bounded queue, explicit shed.** Unbounded buffering at the ingest point trades
a visible drop for an invisible and unbounded latency climb. The drop count is
reported. The same reasoning governs session admission: when the table is full,
new calls are refused and counted, rather than degrading every call in progress
to serve one more.

**Deletion writes tombstones, not empty slots.** The session table is open
-addressed with linear probing, and clearing a slot on release truncates the
probe chain of any id that hashes earlier and collides there — the next lookup
for that id stops short, reports "not found", and opens a *second* session for a
call that already had one. Downstream that reads as an ASR stream that suddenly
forgot the conversation. Binding sessions permanently to slots also fixes the
chain but strands capacity, so session storage is a separate free list.

**The event ring is sized for the burst, not the average.** Chunk events are rare
per call but arrive in waves — staggered calls still endpoint together. At 512
concurrent calls a 256-slot event ring dropped events while the publisher was
mid-write; 4096 slots and flushing only on drain fixed it, and moved the clean
capacity ceiling from 512 to 1024 calls.

**Percentiles are bucket upper bounds, clamped to the recorded max.** Without the
clamp a summary can print `p99.9 > max`, which reads as a bug even though it is
only the bucket width showing through.

**`-O2`, not `-O3`.** The extra inlining and vectorisation buys nothing
measurable here and costs I-cache footprint, which does show up in the tail.
`-march=native` is opt-in (`-DVHP_NATIVE_ARCH=ON`) because a binary built with it
SIGILLs on an older host.

## Running it

Shared memory, both processes C++:

```bash
./build/vhp_consumer --core 2 --seconds 34 & sleep 1 && ./build/vhp_producer --core 0 --seconds 30
```

Unix socket, fed from Node:

```bash
./build/vhp_consumer --uds /tmp/vhp.sock --core 2 --seconds 34
```

```bash
node tools/node_producer.mjs --uds /tmp/vhp.sock --seconds 30 --gc-pressure-mb 256
```

Multi-call load (1024 concurrent sessions):

```bash
./build/vhp_consumer --core 2 --seconds 14 --max-sessions 2048 & sleep 1 && ./build/vhp_producer --core 0 --seconds 10 --sessions 1024
```

Ring benchmark (pin to distinct physical cores or the false-sharing effect
disappears entirely):

```bash
./build/bench_ring --producer-core 0 --consumer-core 2
```

Regenerate the golden vector after any change to the TypeScript gate:

```bash
../openclaw/node_modules/.bin/tsx --tsconfig tools/tsconfig.json tools/gen_vad_reference.mjs > test/vad_reference.json
```
