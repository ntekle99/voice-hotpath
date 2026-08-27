// Ring correctness. Run this binary under ThreadSanitizer as well as plain:
// x86-64 has a strong memory model and will happily hide an incorrect ordering
// that would break on ARM. TSan models the C++ abstract machine, not the CPU,
// so it catches the bug the hardware forgives. See `ctest -R tsan`.
#include <atomic>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "test_util.hpp"
#include "vhp/spsc_ring.hpp"

namespace {

void test_basic_fifo() {
  vhp::SpscRing<int, 4> ring;
  int out = 0;
  CHECK(!ring.try_pop(out));
  CHECK(ring.try_push(1) == vhp::PushResult::kOk);
  CHECK(ring.try_push(2) == vhp::PushResult::kOk);
  CHECK_EQ(ring.size(), 2u);
  CHECK(ring.try_pop(out)); CHECK_EQ(out, 1);
  CHECK(ring.try_pop(out)); CHECK_EQ(out, 2);
  CHECK(!ring.try_pop(out));
  CHECK(ring.empty());
}

void test_full_is_capacity_not_capacity_minus_one() {
  // A ring that silently wastes a slot is a common off-by-one. Capacity 4 must
  // hold 4.
  vhp::SpscRing<int, 4> ring;
  for (int i = 0; i < 4; ++i) CHECK(ring.try_push(i) == vhp::PushResult::kOk);
  CHECK(ring.try_push(99) == vhp::PushResult::kFull);
  CHECK_EQ(ring.size(), 4u);
  int out = 0;
  CHECK(ring.try_pop(out)); CHECK_EQ(out, 0);
  CHECK(ring.try_push(99) == vhp::PushResult::kOk);
}

void test_wraparound_many_times() {
  vhp::SpscRing<int, 8> ring;
  int out = 0;
  for (int i = 0; i < 10'000; ++i) {
    CHECK(ring.try_push(i) == vhp::PushResult::kOk);
    CHECK(ring.try_pop(out));
    if (out != i) { CHECK_EQ(out, i); return; }
  }
}

// Indices are uint64 and never reset, so wrap of the *index* is unreachable in
// practice (2^64 frames at 50/s is ~10^10 years). Wrap of the slot mask is
// exercised above. This test pins the invariant that the two are independent.
void test_index_arithmetic_is_unsigned_safe() {
  vhp::SpscRing<int, 2> ring;
  int out = 0;
  for (int i = 0; i < 1000; ++i) {
    CHECK(ring.try_push(i) == vhp::PushResult::kOk);
    CHECK(ring.try_push(i) == vhp::PushResult::kOk);
    CHECK(ring.try_push(i) == vhp::PushResult::kFull);
    CHECK(ring.try_pop(out));
    CHECK(ring.try_pop(out));
    CHECK(!ring.try_pop(out));
  }
}

void test_concurrent_no_loss_no_reorder() {
  constexpr int kItems = 200'000;
  auto ring = std::make_unique<vhp::SpscRing<std::uint64_t, 64>>();
  std::atomic<bool> go{false};
  std::atomic<int> mismatches{0};

  std::thread producer([&] {
    while (!go.load(std::memory_order_acquire)) {}
    for (std::uint64_t i = 0; i < kItems; ++i) {
      while (ring->try_push(i) == vhp::PushResult::kFull) std::this_thread::yield();
    }
  });
  std::thread consumer([&] {
    while (!go.load(std::memory_order_acquire)) {}
    std::uint64_t expected = 0, value = 0;
    while (expected < kItems) {
      if (!ring->try_pop(value)) { std::this_thread::yield(); continue; }
      if (value != expected) mismatches.fetch_add(1, std::memory_order_relaxed);
      ++expected;
    }
  });
  go.store(true, std::memory_order_release);
  producer.join();
  consumer.join();
  CHECK_EQ(mismatches.load(), 0);
  CHECK(ring->empty());
}

// The payload must be visible, not just the index. A ring that publishes the
// index before the data passes a sequence-number-only test and corrupts audio
// in production; filling every byte and checking it is what catches that.
void test_payload_visibility() {
  struct Block { std::uint64_t seq; std::uint8_t bytes[120]; };
  constexpr int kItems = 100'000;
  auto ring = std::make_unique<vhp::SpscRing<Block, 32>>();
  std::atomic<bool> go{false};
  std::atomic<int> corrupt{0};

  std::thread producer([&] {
    while (!go.load(std::memory_order_acquire)) {}
    for (std::uint64_t i = 0; i < kItems; ++i) {
      Block block{};
      block.seq = i;
      std::memset(block.bytes, static_cast<int>(i & 0xff), sizeof(block.bytes));
      while (ring->try_push(block) == vhp::PushResult::kFull) std::this_thread::yield();
    }
  });
  std::thread consumer([&] {
    while (!go.load(std::memory_order_acquire)) {}
    Block block{};
    for (int received = 0; received < kItems;) {
      if (!ring->try_pop(block)) { std::this_thread::yield(); continue; }
      const auto expected = static_cast<std::uint8_t>(block.seq & 0xff);
      for (unsigned char byte : block.bytes) {
        if (byte != expected) { corrupt.fetch_add(1, std::memory_order_relaxed); break; }
      }
      ++received;
    }
  });
  go.store(true, std::memory_order_release);
  producer.join();
  consumer.join();
  CHECK_EQ(corrupt.load(), 0);
}

}  // namespace

int main() {
  test_basic_fifo();
  test_full_is_capacity_not_capacity_minus_one();
  test_wraparound_many_times();
  test_index_arithmetic_is_unsigned_safe();
  test_concurrent_no_loss_no_reorder();
  test_payload_visibility();
  return vhptest::summarize("test_ring");
}
