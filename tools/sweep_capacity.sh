#!/usr/bin/env bash
# How many concurrent calls does one pinned core serve, and where does it break?
#
# Each session contributes 50 frames/second. The producer emits one frame per
# session per 20 ms tick, so all N frames for a tick land back-to-back -- a
# worst-case synchronised arrival burst. Real calls arrive independently and
# spread across the interval, so this understates capacity on purpose; a number
# that only holds when traffic is smooth is not a capacity number.
#
# The Little's Law column is the cross-check: in steady state, mean queue depth
# equals arrival rate x mean time in system. It must be compared against the
# TIME-averaged depth (depth_time_avg_L), not against depth_at_pop_mean. Depth
# sampled at each pop is a departure-average -- the queue is deepest exactly when
# frames are leaving, so departures preferentially sample busy periods and the
# number comes out several times high under bursty arrivals. Both columns are
# reported because they answer different questions: depth_p99 is "how deep does
# it get", L is "how much work is resident on average".
#
#   tools/sweep_capacity.sh > results/sweep_capacity.csv
set -euo pipefail

BUILD="${BUILD:-build}"
RUN_SECONDS="${RUN_SECONDS:-8}"
MAX_SESSIONS="${MAX_SESSIONS:-4096}"
PRODUCER_CORE="${PRODUCER_CORE:-0}"
CONSUMER_CORE="${CONSUMER_CORE:-2}"
FRAME_HZ=50
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

field() { sed -n "s/.*$2=[[:space:]]*\([0-9.]*\).*/\1/p" <<<"$1" | head -1; }

echo "sessions,frames_offered,frames_lost,loss_pct,service_p50_us,service_p99_us,depth_at_pop_mean,depth_p99,depth_time_avg_L,transport_mean_ms,littles_law_predicted_L,event_drops"

for n in 1 8 32 128 512 1024 2048 4096; do
  "$BUILD/vhp_consumer" --core "$CONSUMER_CORE" --seconds "$(( RUN_SECONDS + 3 ))" \
      --max-sessions "$MAX_SESSIONS" --max-chunk-ms 3000 >/dev/null 2>"$WORK/report.txt" &
  consumer_pid=$!
  sleep 1.0
  producer_out=$("$BUILD/vhp_producer" --core "$PRODUCER_CORE" --seconds "$RUN_SECONDS" \
      --sessions "$n" --word-gap-ms 80 2>&1 || true)
  wait "$consumer_pid"

  report=$(cat "$WORK/report.txt")
  offered=$(sed -n 's/.*sessions=[0-9]* frames=\([0-9]*\).*/\1/p' <<<"$producer_out" | head -1)
  lost=$(sed -n 's/.*dropped_by_backpressure=\([0-9]*\).*/\1/p' <<<"$producer_out" | head -1)
  offered="${offered:-0}"; lost="${lost:-0}"

  # `|| true` on every extraction: a label that stops matching should show up as
  # an empty column, not abort the sweep under `set -e` three rows in.
  service_line=$(grep -A1 'vad service time' <<<"$report" | tr '\n' ' ' || true)
  depth_line=$(grep -A1 'ring depth' <<<"$report" | tr '\n' ' ' || true)
  transport_line=$(grep -A1 'transport+queue' <<<"$report" | tr '\n' ' ' || true)

  service_p50=$(field "$service_line" p50)
  service_p99=$(field "$service_line" p99)
  depth_mean=$(field "$depth_line" mean)
  depth_L=$(sed -n "s/.*Little's Law: L=\([0-9.]*\).*/\1/p" <<<"$report" | head -1)
  littles=$(sed -n 's/.*lambda\*W=\([0-9.]*\).*/\1/p' <<<"$report" | head -1)
  depth_p99=$(field "$depth_line" p99)
  transport_mean=$(field "$transport_line" mean)
  event_drops=$(sed -n 's/.*event_drops=\([0-9]*\).*/\1/p' <<<"$report" | head -1)

  loss_pct=$(awk -v l="$lost" -v o="$offered" 'BEGIN{printf "%.4f", (o>0? 100*l/o : 0)}')
  littles="${littles:-}"

  echo "$n,$offered,$lost,$loss_pct,${service_p50:-},${service_p99:-},${depth_mean:-},${depth_p99:-},${depth_L:-},${transport_mean:-},$littles,${event_drops:-0}"
done

cat >&2 <<'NOTE'

Reading this: capacity is the largest N with loss_pct 0.0000 and event_drops 0.
Compare depth_time_avg_L against littles_law_predicted_L -- those should track.
Above it the bounded ingest ring sheds under the synchronised burst -- visibly
and counted, which is the point of bounding it. Compare depth_mean against
littles_law_depth: they should track. Divergence means the run had not reached
steady state, not that the queue is misbehaving.
NOTE
