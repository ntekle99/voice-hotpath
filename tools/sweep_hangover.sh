#!/usr/bin/env bash
# Trace the endpoint hangover frontier.
#
# The hangover (gate silence_frames) is the single biggest latency knob in the
# pipeline, and it is not a free one. Short hangover: the endpoint fires quickly,
# but any inter-word pause longer than the hangover splits one utterance into
# several, and ASR sees fragments. Long hangover: never cuts, but every single
# utterance pays the full hangover before the ASR request is even issued.
#
# This sweeps it against a deterministic corpus with known utterance boundaries
# and emits the frontier as CSV, so the operating point is chosen from a curve
# instead of picked. Pick the knee, then state the cost ratio you are assuming:
# "a false cut costs us more than 200 ms of latency" is a defensible position;
# "silenceFrames = 12" with no stated reason is not.
#
#   tools/sweep_hangover.sh > sweep.csv
set -euo pipefail

BUILD="${BUILD:-build}"
SECONDS_PER_RUN="${SECONDS_PER_RUN:-12}"
SPEECH_MS="${SPEECH_MS:-1500}"
GAP_MS="${GAP_MS:-900}"
WORD_GAP_MS="${WORD_GAP_MS:-80}"
WORD_EVERY_MS="${WORD_EVERY_MS:-320}"
FRAME_MS=20
PRODUCER_CORE="${PRODUCER_CORE:-0}"
CONSUMER_CORE="${CONSUMER_CORE:-2}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Ground truth: the producer's utterance schedule is deterministic, so the
# expected chunk count is arithmetic, not a guess. Utterance k begins at
# k*CYCLE_MS and its endpoint fires at k*CYCLE_MS + SPEECH_MS + hangover, so the
# count of hangover-closed chunks is the number of k for which that lands inside
# the run. It depends on the hangover, which is why it is computed per row.
CYCLE_MS=$(( SPEECH_MS + GAP_MS ))
RUN_MS=$(( SECONDS_PER_RUN * 1000 ))
expected_for_hangover() {
  local hangover_ms="$1"
  local last=$(( RUN_MS - SPEECH_MS - hangover_ms ))
  if (( last < 0 )); then echo 0; else echo $(( last / CYCLE_MS + 1 )); fi
}

echo "silence_frames,hangover_ms,chunks,expected_utterances,chunks_per_utterance,endpoint_p50_ms"

for sf in 1 2 3 4 5 6 8 10 12 16 20 25; do
  "$BUILD/vhp_consumer" --core "$CONSUMER_CORE" --seconds "$(( SECONDS_PER_RUN + 3 ))" \
      --silence-frames "$sf" >"$WORK/chunks.jsonl" 2>"$WORK/report.txt" &
  consumer_pid=$!
  sleep 0.7
  "$BUILD/vhp_producer" --core "$PRODUCER_CORE" --seconds "$SECONDS_PER_RUN" \
      --speech-ms "$SPEECH_MS" --gap-ms "$GAP_MS" \
      --word-gap-ms "$WORD_GAP_MS" --word-every-ms "$WORD_EVERY_MS" >/dev/null 2>&1
  wait "$consumer_pid"

  # Only hangover-closed chunks count as endpoints. A stream_end chunk is an
  # artefact of the run ending, not a decision the gate made.
  chunks=$(grep -c '"reason":"hangover"' "$WORK/chunks.jsonl" || true)
  endpoint_p50=$(grep -A0 'endpoint decision lag' "$WORK/report.txt" \
                 | sed -n 's/.*p50=[[:space:]]*\([0-9.]*\).*/\1/p' | head -1)
  endpoint_p50="${endpoint_p50:-0}"
  hangover_ms=$(( sf * FRAME_MS ))
  EXPECTED=$(expected_for_hangover "$hangover_ms")
  ratio=$(awk -v c="$chunks" -v e="$EXPECTED" 'BEGIN{ printf "%.2f", (e>0? c/e : 0) }')
  echo "$sf,$hangover_ms,$chunks,$EXPECTED,$ratio,$endpoint_p50"
done

cat >&2 <<'NOTE'

Reading this: chunks_per_utterance == 1.00 means no false cuts. Above 1.00, the
hangover is shorter than the inter-word pause and utterances are being split.
endpoint_p50_ms is what every utterance pays. The knee is the shortest hangover
that still holds the ratio at 1.00 -- and note it should land near WORD_GAP_MS,
which is a useful sanity check on the whole measurement.
NOTE
