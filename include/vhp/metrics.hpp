// Per-stage latency accounting for the hot path.
//
// The split matters more than any single number. "p99 is 40 ms" is not
// actionable; "p99 is 40 ms and 38 ms of it is queueing delay in the ingest
// ring" tells you exactly what to fix. Service time and queueing delay are
// tracked separately at every stage for that reason.
#pragma once

#include <cstdint>
#include <string>

#include "vhp/histogram.hpp"

namespace vhp {

struct HotPathMetrics {
  // Producer enqueue -> consumer dequeue. Transport + queueing.
  Histogram transport{1'000'000'000ULL};
  // Dequeue -> done processing this frame. Pure service time: energy + gate.
  Histogram service{1'000'000'000ULL};
  // Deviation of observed frame spacing from the nominal cadence. This is the
  // jitter number; the mean is uninformative because the mean is the cadence.
  Histogram jitter{1'000'000'000ULL};
  // Ring occupancy sampled at dequeue. This is the DEPARTURE-average: it only
  // has a sample when a frame is popped, so it oversamples busy periods and
  // reads high under bursty arrivals. Useful for "how deep does it get", wrong
  // for Little's Law.
  Histogram depth{static_cast<std::uint64_t>(kFrameDepthMax)};
  // Endpoint decision lag: last loud frame -> chunk closed. Dominated by the
  // hangover, which is the intended tradeoff, not a defect.
  Histogram endpoint_lag{60'000'000'000ULL};

  std::uint64_t frames = 0;
  std::uint64_t chunks = 0;
  std::uint64_t seq_gaps = 0;       // frames lost upstream of us
  std::uint64_t ring_full_drops = 0;  // frames we refused: backpressure
  std::uint64_t event_drops = 0;
  // TIME-average occupancy, accumulated as the integral of depth over elapsed
  // time. This is the L in Little's Law (L = lambda x W). Computing it from the
  // departure-sampled histogram above gives a number several times too large
  // under bursty arrivals -- the queue is deep exactly when frames are leaving,
  // so departures preferentially sample the busy periods. No extra clock reads:
  // it reuses the timestamp the loop already takes.
  std::uint64_t depth_area_ns = 0;   // sum of depth * dt
  std::uint64_t depth_time_ns = 0;   // sum of dt

  double mean_depth_time_averaged() const {
    return depth_time_ns == 0 ? 0.0
                              : static_cast<double>(depth_area_ns) /
                                    static_cast<double>(depth_time_ns);
  }
  std::uint64_t frames_refused = 0;   // frames for calls admission control turned away
  std::uint32_t sessions_peak = 0;
  std::uint32_t sessions_capacity = 0;
  std::uint64_t sessions_opened = 0;
  std::uint64_t sessions_rejected = 0;

  static constexpr std::uint64_t kFrameDepthMax = 1u << 20;

  std::string report(double nominal_frame_ms, const char* transport_label = "shm") const {
    std::string out;
    char buf[256];
    out += "\n--- vhp hot path ---------------------------------------------------\n";
    out += transport.summary(std::string(transport_label) + " transport+queue");
    out += service.summary("vad service time", 1e3, "us");
    out += jitter.summary("interarrival |dev| @" + std::to_string(static_cast<int>(nominal_frame_ms)) + "ms");
    out += depth.summary("ring depth (at pop)", 1.0, "frames");
    // Little's Law, computed here rather than by the caller so lambda comes from
    // this run's own observation window. Deriving lambda from the offered load
    // instead builds in a bias whenever the consumer outlives the producer: the
    // idle tail dilutes the time-average but not the predicted value, and the
    // two disagree by exactly the ratio of the windows.
    const double window_s = static_cast<double>(depth_time_ns) / 1e9;
    const double lambda = window_s > 0.0 ? static_cast<double>(frames) / window_s : 0.0;
    const double W_s = transport.mean() / 1e9;
    std::snprintf(buf, sizeof(buf),
                  "%-26s Little's Law: L=%.3f  vs  lambda*W=%.3f  "
                  "(lambda=%.0f/s, W=%.3f ms, window=%.1f s)\n",
                  "", mean_depth_time_averaged(), lambda * W_s, lambda, W_s * 1000.0, window_s);
    out += buf;
    out += endpoint_lag.summary("endpoint decision lag");
    std::snprintf(buf, sizeof(buf),
                  "frames=%llu chunks=%llu upstream_gaps=%llu backpressure_drops=%llu "
                  "event_drops=%llu\n",
                  static_cast<unsigned long long>(frames),
                  static_cast<unsigned long long>(chunks),
                  static_cast<unsigned long long>(seq_gaps),
                  static_cast<unsigned long long>(ring_full_drops),
                  static_cast<unsigned long long>(event_drops));
    out += buf;
    std::snprintf(buf, sizeof(buf),
                  "sessions: peak=%u/%u opened=%llu rejected=%llu refused_frames=%llu\n",
                  sessions_peak, sessions_capacity,
                  static_cast<unsigned long long>(sessions_opened),
                  static_cast<unsigned long long>(sessions_rejected),
                  static_cast<unsigned long long>(frames_refused));
    out += buf;
    out +=
        "caveat: thread pinned but core not isolated (no isolcpus/nohz_full);\n"
        "        p99.9 still includes timer ticks and other tenants.\n";
    out += "--------------------------------------------------------------------\n";
    return out;
  }
};

}  // namespace vhp
