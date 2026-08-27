// bench_ring -- what the ring costs, measured two different ways on purpose.
//
// EXPERIMENT 1: handoff latency, paced.
//   The producer emits on a fixed grid slow enough that the consumer always
//   keeps up, so the ring sits at depth ~0 and the measured send->receive time
//   is the real cost of publishing one item: release store, coherence traffic,
//   acquire load. This is also the only regime where false sharing is visible --
//   both sides are continuously re-reading each other's index.
//
// EXPERIMENT 2: saturated throughput.
//   The producer runs flat out. Here latency is meaningless: the ring is full,
//   so every item waits behind ~4096 others and "latency" is just
//   depth / drain-rate. Reporting a p99 from this regime and calling it handoff
//   cost is a classic way to publish a number that is off by three orders of
//   magnitude. Only items/second is reported.
//
// Each experiment runs padded and unpadded. The unpadded build puts the producer
// and consumer indices on one cache line, which is the bug the header exists to
// avoid; quantifying it beats asserting it.
#include <getopt.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>

#include "vhp/clock.hpp"
#include "vhp/histogram.hpp"
#include "vhp/spsc_ring.hpp"

namespace {

struct Stamped {
  std::uint64_t seq;
  std::uint64_t t_send_ns;
  std::uint8_t filler[48];  // 64 B slot, like a real payload
};

constexpr std::size_t kCapacity = 4096;

struct Config {
  std::uint64_t paced_items = 400'000;
  std::uint64_t pace_ns = 2'000;  // 500 kHz: ~40x slower than the ring can drain
  std::uint64_t saturated_items = 5'000'000;
  int producer_core = -1;
  int consumer_core = -1;
};

struct Result {
  vhp::Histogram latency{100'000'000ULL};
  vhp::Histogram depth{static_cast<std::uint64_t>(kCapacity) * 2};
  double seconds = 0.0;
};

template <bool Padded>
Result run(const Config& cfg, std::uint64_t items, std::uint64_t pace_ns, bool measure_latency) {
  auto ring = std::make_unique<vhp::SpscRing<Stamped, kCapacity, Padded>>();
  Result result;
  std::atomic<bool> go{false};

  std::thread producer([&] {
    if (cfg.producer_core >= 0) vhp::pin_to_core(cfg.producer_core);
    while (!go.load(std::memory_order_acquire)) {}
    const std::uint64_t start = vhp::now_ns();
    for (std::uint64_t i = 0; i < items; ++i) {
      if (pace_ns) vhp::spin_until_ns(start + i * pace_ns);
      Stamped item{};
      item.seq = i;
      item.t_send_ns = vhp::now_ns();
      // Spin rather than shed: a drop here would be an artefact of the harness
      // rather than a property of the ring.
      while (ring->try_push(item) == vhp::PushResult::kFull) __builtin_ia32_pause();
    }
  });

  std::thread consumer([&] {
    if (cfg.consumer_core >= 0) vhp::pin_to_core(cfg.consumer_core);
    while (!go.load(std::memory_order_acquire)) {}
    const std::uint64_t start = vhp::now_ns();
    Stamped item{};
    for (std::uint64_t received = 0; received < items;) {
      if (!ring->try_pop(item)) { __builtin_ia32_pause(); continue; }
      if (measure_latency) {
        result.latency.record(vhp::now_ns() - item.t_send_ns);
        result.depth.record(ring->size());
      }
      ++received;
    }
    result.seconds = static_cast<double>(vhp::now_ns() - start) / 1e9;
  });

  go.store(true, std::memory_order_release);
  producer.join();
  consumer.join();
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  static option longopts[] = {{"paced-items", required_argument, nullptr, 'i'},
                              {"pace-ns", required_argument, nullptr, 'p'},
                              {"saturated-items", required_argument, nullptr, 's'},
                              {"producer-core", required_argument, nullptr, 'P'},
                              {"consumer-core", required_argument, nullptr, 'C'},
                              {nullptr, 0, nullptr, 0}};
  int c;
  while ((c = getopt_long(argc, argv, "", longopts, nullptr)) != -1) {
    switch (c) {
      case 'i': cfg.paced_items = std::strtoull(optarg, nullptr, 10); break;
      case 'p': cfg.pace_ns = std::strtoull(optarg, nullptr, 10); break;
      case 's': cfg.saturated_items = std::strtoull(optarg, nullptr, 10); break;
      case 'P': cfg.producer_core = std::atoi(optarg); break;
      case 'C': cfg.consumer_core = std::atoi(optarg); break;
      default: return 2;
    }
  }

  std::printf("bench_ring  capacity=%zu slot=%zuB  cores: producer=%d consumer=%d\n", kCapacity,
              sizeof(Stamped), cfg.producer_core, cfg.consumer_core);
  if (cfg.producer_core < 0 || cfg.consumer_core < 0) {
    std::printf(
        "WARNING: threads unpinned. They may share a physical core, which hides\n"
        "         the false-sharing effect completely. Pass --producer-core and\n"
        "         --consumer-core on distinct physical cores.\n");
  }

  std::printf("\n=== 1. handoff latency, paced at %llu ns (ring stays near-empty) ===\n",
              static_cast<unsigned long long>(cfg.pace_ns));
  auto padded = run<true>(cfg, cfg.paced_items, cfg.pace_ns, true);
  auto shared = run<false>(cfg, cfg.paced_items, cfg.pace_ns, true);
  std::fputs(padded.latency.summary("padded (separate lines)", 1.0, "ns").c_str(), stdout);
  std::fputs(padded.depth.summary("  ring depth at pop", 1.0, "items").c_str(), stdout);
  std::fputs(shared.latency.summary("unpadded (false sharing)", 1.0, "ns").c_str(), stdout);
  std::fputs(shared.depth.summary("  ring depth at pop", 1.0, "items").c_str(), stdout);

  const auto ratio = [](const vhp::Histogram& a, const vhp::Histogram& b, double p) {
    return static_cast<double>(a.value_at_percentile(p)) /
           static_cast<double>(b.value_at_percentile(p) + 1);
  };
  std::printf("false-sharing penalty: %.2fx at p50, %.2fx at p99, %.2fx at p99.9\n",
              ratio(shared.latency, padded.latency, 50), ratio(shared.latency, padded.latency, 99),
              ratio(shared.latency, padded.latency, 99.9));

  std::printf("\n=== 2. saturated throughput (latency omitted: it is queueing, not handoff) ===\n");
  auto padded_sat = run<true>(cfg, cfg.saturated_items, 0, false);
  auto shared_sat = run<false>(cfg, cfg.saturated_items, 0, false);
  const double padded_rate = static_cast<double>(cfg.saturated_items) / padded_sat.seconds / 1e6;
  const double shared_rate = static_cast<double>(cfg.saturated_items) / shared_sat.seconds / 1e6;
  std::printf("padded    %.2f Mitems/s  (%.1f ns/item)\n", padded_rate, 1000.0 / padded_rate);
  std::printf("unpadded  %.2f Mitems/s  (%.1f ns/item)\n", shared_rate, 1000.0 / shared_rate);
  std::printf("\nfor reference: 20 ms audio frames arrive at 50 Hz. One call needs\n"
              "0.00005 Mitems/s, so the ring is ~%.0fx oversized for the workload --\n"
              "which is the point: it must never be the bottleneck under burst.\n",
              padded_rate * 1e6 / 50.0);
  return 0;
}
