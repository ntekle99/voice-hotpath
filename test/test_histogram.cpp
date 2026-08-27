#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "test_util.hpp"
#include "vhp/histogram.hpp"

namespace {

void test_relative_error_bound() {
  // The contract of the bucketing scheme: any value it reports back is within
  // 10^-sigdigits of the value recorded. Checked one value at a time against a
  // fresh histogram -- computing a rank over a shared histogram introduces
  // floating-point ties that test the arithmetic of the test, not the bound.
  for (std::uint64_t v = 1; v < 100'000'000ULL; v = v * 7 / 5 + 1) {
    vhp::Histogram h{1'000'000'000'000ULL, 3};
    h.record(v);
    for (double p : {0.0, 50.0, 100.0}) {
      const std::uint64_t reported = h.value_at_percentile(p);
      const double relative = std::fabs(static_cast<double>(reported) - static_cast<double>(v)) /
                              static_cast<double>(v);
      CHECK_MSG(relative <= 1e-3,
                "value " + std::to_string(v) + " reported as " + std::to_string(reported) +
                    " at p" + std::to_string(p));
    }
    CHECK_EQ(h.max(), v);
    CHECK_EQ(h.min(), v);
  }
}

void test_percentiles_are_monotonic() {
  vhp::Histogram h;
  for (std::uint64_t i = 1; i <= 100'000; ++i) h.record(i * 1000);
  std::uint64_t previous = 0;
  for (double p = 0.0; p <= 100.0; p += 0.5) {
    const std::uint64_t v = h.value_at_percentile(p);
    CHECK(v >= previous);
    previous = v;
  }
  CHECK_EQ(h.max(), 100'000'000ULL);
}

void test_constant_distribution() {
  vhp::Histogram h;
  for (int i = 0; i < 10'000; ++i) h.record(1'000'000);
  CHECK_NEAR(static_cast<double>(h.value_at_percentile(50)), 1'000'000.0, 1000.0);
  CHECK_NEAR(static_cast<double>(h.value_at_percentile(99.9)), 1'000'000.0, 1000.0);
  CHECK_NEAR(h.stddev(), 0.0, 1.0);
}

void test_coordinated_omission_backfill() {
  // 1000 samples at the expected 20 ms interval, then one 200 ms stall. A naive
  // histogram reports p99 = 20 ms and calls it healthy. The corrected one has to
  // show the nine synthesised samples the stall hid.
  constexpr std::uint64_t kInterval = 20'000'000;
  vhp::Histogram naive, corrected;
  for (int i = 0; i < 1000; ++i) {
    naive.record(kInterval);
    corrected.record_with_expected_interval(kInterval, kInterval);
  }
  naive.record(200'000'000);
  corrected.record_with_expected_interval(200'000'000, kInterval);

  CHECK_EQ(naive.count(), 1001u);
  CHECK_EQ(corrected.count(), 1010u);  // 1001 + 9 backfilled

  // One stall in a thousand does not move p99 in either histogram, and saying
  // otherwise would be the same overclaim the correction exists to prevent.
  // It moves p99.9, and that is where the two diverge: the naive view reports a
  // flat 20 ms and looks healthy, the corrected view reports ~180 ms.
  CHECK(naive.value_at_percentile(99) < 21'000'000);
  CHECK(corrected.value_at_percentile(99) < 21'000'000);
  CHECK(naive.value_at_percentile(99.9) < 21'000'000);
  CHECK(corrected.value_at_percentile(99.9) > 100'000'000);
  CHECK_EQ(naive.max(), corrected.max());
}

void test_merge() {
  vhp::Histogram a, b;
  for (int i = 1; i <= 1000; ++i) a.record(static_cast<std::uint64_t>(i) * 1000);
  for (int i = 1001; i <= 2000; ++i) b.record(static_cast<std::uint64_t>(i) * 1000);
  a.merge(b);
  CHECK_EQ(a.count(), 2000u);
  CHECK_EQ(a.max(), 2'000'000ULL);
  CHECK_NEAR(static_cast<double>(a.value_at_percentile(50)), 1'000'000.0, 2000.0);
}

void test_empty_is_not_a_crash() {
  vhp::Histogram h;
  CHECK_EQ(h.count(), 0u);
  CHECK_EQ(h.value_at_percentile(99.9), 0u);
  CHECK_NEAR(h.mean(), 0.0, 0.0);
  CHECK_NEAR(h.stddev(), 0.0, 0.0);
  CHECK(!h.summary("empty").empty());
}

void test_out_of_range_is_counted_not_silently_clamped() {
  vhp::Histogram h{1'000'000ULL, 3};
  h.record(500'000);
  h.record(1'000'000'000'000ULL);
  CHECK_EQ(h.count(), 1u);
  CHECK_EQ(h.overflow_count(), 1u);
  CHECK(h.summary("overflow").find("WARNING") != std::string::npos);
}

}  // namespace

int main() {
  test_relative_error_bound();
  test_percentiles_are_monotonic();
  test_constant_distribution();
  test_coordinated_omission_backfill();
  test_merge();
  test_empty_is_not_a_crash();
  test_out_of_range_is_counted_not_silently_clamped();
  return vhptest::summarize("test_histogram");
}
