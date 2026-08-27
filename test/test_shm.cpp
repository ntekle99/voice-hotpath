// Cross-process transport. Forks a real producer so the mapping, the header
// validation and the atomics are exercised across address spaces -- a
// same-process create/attach would pass even if the layout were process-local.
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <string>

#include "test_util.hpp"
#include "vhp/clock.hpp"
#include "vhp/pipeline.hpp"

namespace {

const std::string kName = "/vhp_test_" + std::to_string(::getpid());

void test_header_validation_rejects_mismatch() {
  auto ring = vhp::ShmRing<vhp::AudioFrame, 1024>::create(kName + "_hdr");
  bool threw = false;
  try {
    // Same name, different capacity: the layout check must catch it rather than
    // hand back a ring whose indices mean something else.
    auto bad = vhp::ShmRing<vhp::AudioFrame, 512>::attach(kName + "_hdr");
    (void)bad;
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK_MSG(threw, "attaching with a mismatched layout must throw");
}

void test_attach_missing_segment_throws() {
  bool threw = false;
  try {
    auto missing = vhp::FrameRing::attach("/vhp_definitely_not_here_12345");
    (void)missing;
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
}

void test_cross_process_roundtrip() {
  constexpr std::uint64_t kFrames = 20'000;
  auto ring = vhp::FrameRing::create(kName);

  const pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    // Child: attach by name and stream frames in.
    auto producer = vhp::FrameRing::attach(kName);
    vhp::AudioFrame frame{};
    frame.hdr.byte_len = 960;
    frame.hdr.sample_rate = 24'000;
    for (std::uint64_t seq = 0; seq < kFrames; ++seq) {
      frame.hdr.seq = seq;
      frame.hdr.t_produce_ns = vhp::now_ns();
      std::memset(frame.payload, static_cast<int>(seq & 0xff), frame.hdr.byte_len);
      while (producer.ring().try_push(frame) == vhp::PushResult::kFull) {
        __builtin_ia32_pause();
      }
    }
    ::_exit(0);
  }

  vhp::AudioFrame frame{};
  std::uint64_t expected = 0;
  int order_errors = 0, payload_errors = 0;
  const std::uint64_t deadline = vhp::now_ns() + 30'000'000'000ULL;
  while (expected < kFrames && vhp::now_ns() < deadline) {
    if (!ring.ring().try_pop(frame)) { __builtin_ia32_pause(); continue; }
    if (frame.hdr.seq != expected) ++order_errors;
    const auto want = static_cast<std::uint8_t>(frame.hdr.seq & 0xff);
    for (std::uint32_t i = 0; i < frame.hdr.byte_len; ++i) {
      if (frame.payload[i] != want) { ++payload_errors; break; }
    }
    ++expected;
  }
  int status = 0;
  ::waitpid(pid, &status, 0);

  CHECK_EQ(expected, kFrames);
  CHECK_EQ(order_errors, 0);
  CHECK_EQ(payload_errors, 0);
  CHECK(ring.ring().empty());
}

}  // namespace

int main() {
  test_header_validation_rejects_mismatch();
  test_attach_missing_segment_throws();
  test_cross_process_roundtrip();
  return vhptest::summarize("test_shm");
}
