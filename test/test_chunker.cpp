// Chunk assembly: the state machine that turns gate transitions into ASR work.
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include "test_util.hpp"
#include "vhp/vad.hpp"

namespace {

constexpr std::uint32_t kSampleRate = 24'000;
constexpr std::size_t kSamplesPerFrame = 480;  // 20 ms
constexpr std::size_t kFrameBytes = kSamplesPerFrame * 2;
constexpr std::uint64_t kFrameNs = 20'000'000;

vhp::AudioFrame make_frame(std::uint64_t seq, bool loud) {
  vhp::AudioFrame frame{};
  frame.hdr.seq = seq;
  frame.hdr.sample_rate = kSampleRate;
  frame.hdr.format = static_cast<std::uint16_t>(vhp::AudioFormat::kPcm16Le);
  frame.hdr.byte_len = kFrameBytes;
  frame.hdr.t_produce_ns = seq * kFrameNs;
  // Constant-magnitude square wave: rms == peak, so the gate decision is exact
  // and the test does not depend on where a threshold sits relative to noise.
  const auto amplitude = static_cast<std::int16_t>((loud ? 0.30 : 0.001) * 32767.0);
  for (std::size_t i = 0; i < kSamplesPerFrame; ++i) {
    const std::int16_t sample = (i % 2 == 0) ? amplitude : static_cast<std::int16_t>(-amplitude);
    std::memcpy(frame.payload + i * 2, &sample, 2);
  }
  return frame;
}

vhp::ChunkerConfig base_config() {
  vhp::ChunkerConfig cfg;
  cfg.gate.rms_threshold = 0.02;
  cfg.gate.peak_threshold = 0.10;
  cfg.gate.speech_frames = 2;
  cfg.gate.silence_frames = 3;
  cfg.gate.cooldown_ms = 0.0;
  cfg.pre_roll_frames = 2;
  cfg.max_chunk_ms = 15'000.0;
  return cfg;
}

void test_silence_produces_nothing() {
  vhp::ChunkAssembler assembler(base_config());
  for (std::uint64_t seq = 0; seq < 200; ++seq) {
    CHECK(!assembler.on_frame(make_frame(seq, false), seq * kFrameNs).has_value());
  }
  CHECK(!assembler.active());
  CHECK(!assembler.flush(200 * kFrameNs).has_value());
}

void test_single_utterance_closes_on_hangover() {
  vhp::ChunkAssembler assembler(base_config());
  std::optional<vhp::ChunkEvent> event;
  for (std::uint64_t seq = 0; seq < 20; ++seq) {
    const bool loud = seq < 10;
    auto result = assembler.on_frame(make_frame(seq, loud), seq * kFrameNs);
    if (result) { CHECK(!event.has_value()); event = result; }
  }
  CHECK(event.has_value());
  if (!event) return;

  // Timeline: onset is declared on frame 1 (speech_frames=2). One pre-roll frame
  // (frame 0) is available and is prepended. Frames 1..9 are speech. Frames
  // 10,11,12 are the hangover and are kept -- ASR wants the trailing silence.
  // The chunk closes on frame 12.
  CHECK_EQ(event->first_seq, 0u);
  CHECK_EQ(event->last_seq, 12u);
  CHECK_EQ(event->frame_count, 13u);
  CHECK_EQ(event->byte_count, static_cast<std::uint32_t>(13 * kFrameBytes));
  CHECK_EQ(event->reason, static_cast<std::uint16_t>(vhp::CloseReason::kHangover));
  CHECK_EQ(event->dropped_frames, 0u);
  CHECK_EQ(assembler.chunk_size(), 13 * kFrameBytes);
  CHECK(!assembler.active());
}

void test_pre_roll_is_bounded_and_recent() {
  auto cfg = base_config();
  cfg.pre_roll_frames = 3;
  vhp::ChunkAssembler assembler(cfg);
  std::optional<vhp::ChunkEvent> event;
  // 50 frames of silence first: the pre-roll must keep the last 3, not all 50.
  for (std::uint64_t seq = 0; seq < 50; ++seq) {
    assembler.on_frame(make_frame(seq, false), seq * kFrameNs);
  }
  for (std::uint64_t seq = 50; seq < 70; ++seq) {
    auto result = assembler.on_frame(make_frame(seq, seq < 60), seq * kFrameNs);
    if (result) event = result;
  }
  CHECK(event.has_value());
  if (!event) return;
  // Onset on frame 51; pre-roll holds frames 48, 49, 50.
  CHECK_EQ(event->first_seq, 48u);
}

void test_no_pre_roll_when_disabled() {
  auto cfg = base_config();
  cfg.pre_roll_frames = 0;
  vhp::ChunkAssembler assembler(cfg);
  std::optional<vhp::ChunkEvent> event;
  for (std::uint64_t seq = 0; seq < 20; ++seq) {
    auto result = assembler.on_frame(make_frame(seq, seq < 10), seq * kFrameNs);
    if (result) event = result;
  }
  CHECK(event.has_value());
  if (event) CHECK_EQ(event->first_seq, 1u);  // the onset frame itself
}

void test_max_length_force_flush_is_gapless() {
  auto cfg = base_config();
  cfg.max_chunk_ms = 200.0;  // 10 frames
  vhp::ChunkAssembler assembler(cfg);
  std::vector<vhp::ChunkEvent> events;
  for (std::uint64_t seq = 0; seq < 100; ++seq) {
    if (auto result = assembler.on_frame(make_frame(seq, true), seq * kFrameNs)) {
      events.push_back(*result);
    }
  }
  CHECK(events.size() >= 8u);
  for (const auto& event : events) {
    CHECK_EQ(event.reason, static_cast<std::uint16_t>(vhp::CloseReason::kMaxLength));
  }
  // Contiguity: each chunk must resume exactly where the previous one ended.
  // A gap here means audio was dropped mid-utterance and ASR would see a splice.
  for (std::size_t i = 1; i < events.size(); ++i) {
    CHECK_EQ(events[i].first_seq, events[i - 1].last_seq + 1);
  }
}

void test_upstream_gaps_are_reported() {
  vhp::ChunkAssembler assembler(base_config());
  std::optional<vhp::ChunkEvent> event;
  std::uint64_t seq = 0;
  for (int i = 0; i < 6; ++i) {
    assembler.on_frame(make_frame(seq, true), seq * kFrameNs);
    ++seq;
  }
  seq += 4;  // four frames lost upstream
  for (int i = 0; i < 10; ++i) {
    if (auto result = assembler.on_frame(make_frame(seq, i < 4), seq * kFrameNs)) event = result;
    ++seq;
  }
  CHECK(event.has_value());
  if (event) CHECK_EQ(event->dropped_frames, 4u);
}

void test_flush_closes_an_open_chunk() {
  vhp::ChunkAssembler assembler(base_config());
  for (std::uint64_t seq = 0; seq < 10; ++seq) {
    assembler.on_frame(make_frame(seq, true), seq * kFrameNs);
  }
  CHECK(assembler.active());
  auto event = assembler.flush(10 * kFrameNs);
  CHECK(event.has_value());
  if (event) CHECK_EQ(event->reason, static_cast<std::uint16_t>(vhp::CloseReason::kStreamEnd));
  CHECK(!assembler.active());
  CHECK(!assembler.flush(11 * kFrameNs).has_value());
}

void test_hangover_length_sets_endpoint_latency() {
  // The knob-to-latency relationship, asserted rather than assumed: the endpoint
  // fires exactly silence_frames after the last loud frame. This is the curve
  // the sweep in tools/sweep_hangover.sh traces out.
  for (std::uint32_t silence_frames : {1u, 3u, 6u, 12u, 25u}) {
    auto cfg = base_config();
    cfg.gate.silence_frames = silence_frames;
    vhp::ChunkAssembler assembler(cfg);
    std::optional<vhp::ChunkEvent> event;
    const std::uint64_t last_loud_seq = 9;
    for (std::uint64_t seq = 0; seq < 100 && !event; ++seq) {
      event = assembler.on_frame(make_frame(seq, seq <= last_loud_seq), seq * kFrameNs);
      if (event) CHECK_EQ(event->last_seq, last_loud_seq + silence_frames);
    }
    CHECK_MSG(event.has_value(), "no endpoint at silence_frames=" + std::to_string(silence_frames));
  }
}

}  // namespace

int main() {
  test_silence_produces_nothing();
  test_single_utterance_closes_on_hangover();
  test_pre_roll_is_bounded_and_recent();
  test_no_pre_roll_when_disabled();
  test_max_length_force_flush_is_gapless();
  test_upstream_gaps_are_reported();
  test_flush_closes_an_open_chunk();
  test_hangover_length_sets_endpoint_latency();
  return vhptest::summarize("test_chunker");
}
