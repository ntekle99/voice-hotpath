// A log-linear latency histogram, HdrHistogram's bucketing scheme.
//
// Why not "collect a vector and sort it": recording must be O(1), allocation-free
// and bounded in memory, or the measurement changes what it measures. This gives
// constant-time record, fixed footprint (~270 KB at 3 significant digits over a
// 60 s range) and a guaranteed relative error.
//
// Coordinated omission
// --------------------
// If the thing you are measuring stalls, the naive loop stops issuing requests
// during the stall and so never records the latency that the stall caused. The
// result systematically under-reports the tail -- often by an order of magnitude.
// record_with_expected_interval() backfills the samples the stall swallowed. Use
// it wherever there is an intended rate (a 20 ms audio pacer, a request loop);
// use plain record() only for genuinely event-driven measurements.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace vhp {

class Histogram {
 public:
  explicit Histogram(std::uint64_t highest_trackable = 60ULL * 1'000'000'000ULL,
                     int significant_digits = 3, std::uint64_t lowest_discernible = 1) {
    unit_magnitude_ = static_cast<int>(std::floor(std::log2(static_cast<double>(
        lowest_discernible == 0 ? 1 : lowest_discernible))));
    const double largest_single_unit = 2.0 * std::pow(10.0, significant_digits);
    sub_bucket_magnitude_ = static_cast<int>(std::ceil(std::log2(largest_single_unit)));
    sub_bucket_count_ = 1ULL << sub_bucket_magnitude_;
    sub_bucket_half_count_ = sub_bucket_count_ / 2;
    sub_bucket_half_magnitude_ = sub_bucket_magnitude_ - 1;
    sub_bucket_mask_ = (sub_bucket_count_ - 1) << unit_magnitude_;
    leading_zero_base_ = 64 - unit_magnitude_ - sub_bucket_magnitude_;

    int buckets = 1;
    std::uint64_t smallest_untrackable = sub_bucket_count_ << unit_magnitude_;
    while (smallest_untrackable <= highest_trackable) {
      if (smallest_untrackable > (UINT64_MAX / 2)) { ++buckets; break; }
      smallest_untrackable <<= 1;
      ++buckets;
    }
    counts_.assign(static_cast<std::size_t>(buckets + 1) * sub_bucket_half_count_, 0);
  }

  void record(std::uint64_t value) noexcept {
    const std::size_t index = counts_index(value);
    if (index >= counts_.size()) {
      ++overflow_;
      return;
    }
    ++counts_[index];
    ++total_;
    if (value > max_ ) max_ = value;
    if (total_ == 1 || value < min_) min_ = value;
  }

  // Record `value`, then synthesise the samples that a stall of this length
  // would have prevented us from taking, at `expected_interval` spacing.
  void record_with_expected_interval(std::uint64_t value,
                                     std::uint64_t expected_interval) noexcept {
    record(value);
    if (expected_interval == 0 || value <= expected_interval) return;
    for (std::uint64_t missing = value - expected_interval; missing >= expected_interval;
         missing -= expected_interval) {
      record(missing);
    }
  }

  std::uint64_t count() const noexcept { return total_; }
  std::uint64_t min() const noexcept { return total_ ? min_ : 0; }
  std::uint64_t max() const noexcept { return max_; }
  std::uint64_t overflow_count() const noexcept { return overflow_; }

  double mean() const noexcept {
    if (total_ == 0) return 0.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < counts_.size(); ++i) {
      if (counts_[i]) sum += static_cast<double>(counts_[i]) * static_cast<double>(median_equivalent(value_from_index(i)));
    }
    return sum / static_cast<double>(total_);
  }

  double stddev() const noexcept {
    if (total_ == 0) return 0.0;
    const double mu = mean();
    double acc = 0.0;
    for (std::size_t i = 0; i < counts_.size(); ++i) {
      if (counts_[i]) {
        const double d = static_cast<double>(median_equivalent(value_from_index(i))) - mu;
        acc += d * d * static_cast<double>(counts_[i]);
      }
    }
    return std::sqrt(acc / static_cast<double>(total_));
  }

  std::uint64_t value_at_percentile(double percentile) const noexcept {
    if (total_ == 0) return 0;
    const double p = std::clamp(percentile, 0.0, 100.0);
    const std::uint64_t target = static_cast<std::uint64_t>(
        std::ceil((p / 100.0) * static_cast<double>(total_)));
    std::uint64_t running = 0;
    for (std::size_t i = 0; i < counts_.size(); ++i) {
      running += counts_[i];
      if (running >= target && counts_[i] != 0) {
        // The bucket's upper bound, which is the conservative reading -- but
        // clamped to the largest value actually recorded. Unclamped, a summary
        // can print p99.9 > max, which reads as a bug even though it is only
        // the bucket width showing through.
        return std::min(highest_equivalent(value_from_index(i)), max_);
      }
    }
    return max_;
  }

  void merge(const Histogram& other) noexcept {
    if (other.counts_.size() != counts_.size()) return;  // incompatible layouts
    for (std::size_t i = 0; i < counts_.size(); ++i) counts_[i] += other.counts_[i];
    total_ += other.total_;
    overflow_ += other.overflow_;
    if (other.total_) {
      max_ = std::max(max_, other.max_);
      min_ = total_ == other.total_ ? other.min_ : std::min(min_, other.min_);
    }
  }

  void reset() noexcept {
    std::fill(counts_.begin(), counts_.end(), 0);
    total_ = 0; overflow_ = 0; min_ = 0; max_ = 0;
  }

  // `scale` divides recorded units into display units (1e6 for ns -> ms).
  std::string summary(const std::string& label, double scale = 1e6,
                      const std::string& unit = "ms") const {
    char buf[512];
    std::string out;
    std::snprintf(buf, sizeof(buf), "%-26s n=%-8llu  min=%8.3f  p50=%8.3f  p90=%8.3f\n",
                  label.c_str(), static_cast<unsigned long long>(total_),
                  static_cast<double>(min()) / scale,
                  static_cast<double>(value_at_percentile(50)) / scale,
                  static_cast<double>(value_at_percentile(90)) / scale);
    out += buf;
    std::snprintf(buf, sizeof(buf),
                  "%-26s p99=%8.3f  p99.9=%8.3f  max=%8.3f  mean=%8.3f  sd=%7.3f %s\n", "",
                  static_cast<double>(value_at_percentile(99)) / scale,
                  static_cast<double>(value_at_percentile(99.9)) / scale,
                  static_cast<double>(max()) / scale, mean() / scale, stddev() / scale,
                  unit.c_str());
    out += buf;
    if (total_ > 0 && total_ < 1000) {
      std::snprintf(buf, sizeof(buf),
                    "%-26s note: n=%llu -- p99.9 is ~%.1f samples deep, treat as indicative\n",
                    "", static_cast<unsigned long long>(total_),
                    static_cast<double>(total_) * 0.001);
      out += buf;
    }
    if (overflow_) {
      std::snprintf(buf, sizeof(buf), "%-26s WARNING: %llu samples exceeded the tracked range\n",
                    "", static_cast<unsigned long long>(overflow_));
      out += buf;
    }
    return out;
  }

  // CSV of (value, count) for non-empty buckets -- feed to the replay analysis.
  std::string to_csv() const {
    std::string out = "value,count\n";
    char buf[64];
    for (std::size_t i = 0; i < counts_.size(); ++i) {
      if (!counts_[i]) continue;
      std::snprintf(buf, sizeof(buf), "%llu,%llu\n",
                    static_cast<unsigned long long>(value_from_index(i)),
                    static_cast<unsigned long long>(counts_[i]));
      out += buf;
    }
    return out;
  }

 private:
  std::size_t counts_index(std::uint64_t value) const noexcept {
    const int bucket_index =
        leading_zero_base_ - __builtin_clzll(value | sub_bucket_mask_);
    const std::uint64_t sub_bucket_index = value >> (bucket_index + unit_magnitude_);
    return static_cast<std::size_t>(
        ((static_cast<std::uint64_t>(bucket_index) + 1) << sub_bucket_half_magnitude_) +
        (sub_bucket_index - sub_bucket_half_count_));
  }

  std::uint64_t value_from_index(std::size_t index) const noexcept {
    std::int64_t bucket_index =
        static_cast<std::int64_t>(index >> sub_bucket_half_magnitude_) - 1;
    std::uint64_t sub_bucket_index =
        (index & (sub_bucket_half_count_ - 1)) + sub_bucket_half_count_;
    if (bucket_index < 0) {
      sub_bucket_index -= sub_bucket_half_count_;
      bucket_index = 0;
    }
    return sub_bucket_index << (bucket_index + unit_magnitude_);
  }

  std::uint64_t equivalent_range(std::uint64_t value) const noexcept {
    const int bucket_index =
        leading_zero_base_ - __builtin_clzll(value | sub_bucket_mask_);
    return 1ULL << (unit_magnitude_ + bucket_index);
  }

  std::uint64_t highest_equivalent(std::uint64_t value) const noexcept {
    return value + equivalent_range(value) - 1;
  }

  std::uint64_t median_equivalent(std::uint64_t value) const noexcept {
    return value + (equivalent_range(value) >> 1);
  }

  std::vector<std::uint64_t> counts_;
  std::uint64_t total_ = 0;
  std::uint64_t overflow_ = 0;
  std::uint64_t min_ = 0;
  std::uint64_t max_ = 0;
  std::uint64_t sub_bucket_count_ = 0;
  std::uint64_t sub_bucket_half_count_ = 0;
  std::uint64_t sub_bucket_mask_ = 0;
  int sub_bucket_magnitude_ = 0;
  int sub_bucket_half_magnitude_ = 0;
  int unit_magnitude_ = 0;
  int leading_zero_base_ = 0;
};

}  // namespace vhp
