// Single-producer / single-consumer bounded ring buffer.
//
// Design notes that matter for the tail:
//   * Capacity is a power of two, so the slot index is `seq & (Capacity-1)`
//     -- never a division.
//   * Producer and consumer indices live on separate cache lines. Sharing a
//     line costs a coherence round-trip on every hand-off and is visible at
//     p99.9; `bench_ring --false-sharing` measures the difference.
//   * Each side caches the other's index and only re-reads it when its own
//     view says the ring is full/empty. The common path touches one line.
//   * No allocation, no exceptions, no virtual calls on the hot path.
//   * The atomics are placed by the *caller* (in a heap object, or in a shared
//     memory mapping), so the same type serves in-process and cross-process.
//
// Correctness argument, in full: the producer publishes a slot with a release
// store to `head`; the consumer reads `head` with an acquire load. That pair
// makes the slot write happen-before the slot read. Symmetrically for `tail`.
// Nothing else synchronises, and nothing else needs to.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace vhp {

inline constexpr std::size_t kCacheLine = 64;

// The ring reports rejection; it does not decide what rejection means. A caller
// that sheds counts a dropped frame, a caller that spins counts nothing. Folding
// a "dropped" counter in here would conflate the two -- a retrying producer
// would inflate it once per spin iteration.
// See README for why the shed policy is drop-newest and not drop-oldest.
enum class PushResult : std::uint8_t { kOk, kFull };

template <typename T, std::size_t Capacity, bool CacheAligned = true>
class SpscRing {
  static_assert(Capacity >= 2, "capacity must be at least 2");
  static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
  static_assert(std::is_trivially_copyable_v<T>, "slot type must be trivially copyable");

 public:
  static constexpr std::size_t kCapacity = Capacity;
  static constexpr std::size_t kMask = Capacity - 1;

  SpscRing() = default;
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // Producer thread only.
  PushResult try_push(const T& value) noexcept {
    const std::uint64_t head = prod_.head.load(std::memory_order_relaxed);
    if (head - prod_.tail_cache >= Capacity) {
      prod_.tail_cache = cons_.tail.load(std::memory_order_acquire);
      if (head - prod_.tail_cache >= Capacity) {
        return PushResult::kFull;
      }
    }
    slots_[head & kMask] = value;
    prod_.head.store(head + 1, std::memory_order_release);
    return PushResult::kOk;
  }

  // Consumer thread only.
  bool try_pop(T& out) noexcept {
    const std::uint64_t tail = cons_.tail.load(std::memory_order_relaxed);
    if (tail == cons_.head_cache) {
      cons_.head_cache = prod_.head.load(std::memory_order_acquire);
      if (tail == cons_.head_cache) {
        return false;
      }
    }
    out = slots_[tail & kMask];
    cons_.tail.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Observers. Safe from either side; inherently a snapshot.
  std::uint64_t produced() const noexcept { return prod_.head.load(std::memory_order_acquire); }
  std::uint64_t consumed() const noexcept { return cons_.tail.load(std::memory_order_acquire); }
  std::size_t size() const noexcept { return static_cast<std::size_t>(produced() - consumed()); }
  bool empty() const noexcept { return size() == 0; }

 private:
  static constexpr std::size_t kAlign = CacheAligned ? kCacheLine : alignof(std::uint64_t);

  // Written only by the producer.
  struct alignas(kAlign) ProducerSide {
    std::atomic<std::uint64_t> head{0};
    std::uint64_t tail_cache{0};
  };

  // Written only by the consumer.
  struct alignas(kAlign) ConsumerSide {
    std::atomic<std::uint64_t> tail{0};
    std::uint64_t head_cache{0};
  };

  static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                "uint64 atomics must be lock-free (required for shared memory)");

  ProducerSide prod_{};
  ConsumerSide cons_{};
  alignas(kCacheLine) T slots_[Capacity]{};
};

}  // namespace vhp
