// Voice-activity gating and chunk assembly.
//
// SpeechThresholdGate is a faithful port of createSpeechThresholdGate() in
// src/talk/audio-energy.ts -- same OR-threshold, same sustained-onset counter,
// same silence hold, same cooldown, same return semantics (true only on the
// rising edge). Keeping it bit-compatible is what makes the replay harness
// meaningful: a C++/TypeScript disagreement would show up as a pipeline
// difference and contaminate every A/B.
//
// ChunkAssembler is the part the TypeScript never had as a unit: it turns the
// gate's speaking/not-speaking state into closed chunks, which is where the
// latency/false-cut tradeoff actually lives. `silence_frames` is the knob to
// sweep -- longer hold means fewer mid-sentence cuts and strictly more latency
// on every single utterance.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "vhp/audio_frame.hpp"
#include "vhp/energy.hpp"

namespace vhp {

struct GateConfig {
  // Thresholds are in normalised units (0..1). Leave unset to disable that arm;
  // the two are OR-ed, exactly as in the TypeScript.
  std::optional<double> rms_threshold = 0.02;
  std::optional<double> peak_threshold = 0.10;
  // Consecutive loud frames required before onset is declared. Suppresses
  // single-frame transients (key clicks, door slams).
  std::uint32_t speech_frames = 2;
  // Consecutive quiet frames required before speech is declared over. This is
  // the endpoint hangover, and the dominant latency knob in the whole pipeline.
  std::uint32_t silence_frames = 12;
  // Minimum gap between onsets. 0 disables.
  double cooldown_ms = 0.0;
};

class SpeechThresholdGate {
 public:
  explicit SpeechThresholdGate(GateConfig config) : cfg_(config) {
    if (cfg_.speech_frames < 1) cfg_.speech_frames = 1;
  }

  // Returns true only on the frame where speech onset is declared.
  bool accept(const AudioEnergy& energy, double now_ms) noexcept {
    const bool loud =
        (cfg_.rms_threshold && energy.rms_norm >= *cfg_.rms_threshold) ||
        (cfg_.peak_threshold && energy.peak_norm >= *cfg_.peak_threshold);

    if (!loud) {
      loud_frames_ = 0;
      if (speaking_ && ++quiet_frames_ >= cfg_.silence_frames) {
        speaking_ = false;
      }
      return false;
    }

    quiet_frames_ = 0;
    ++loud_frames_;
    if (speaking_ || loud_frames_ < cfg_.speech_frames) {
      return false;
    }
    if (cfg_.cooldown_ms > 0.0 && has_triggered_ && (now_ms - last_trigger_ms_) < cfg_.cooldown_ms) {
      return false;
    }
    last_trigger_ms_ = now_ms;
    has_triggered_ = true;
    speaking_ = cfg_.silence_frames > 0;
    if (!speaking_) loud_frames_ = 0;
    return true;
  }

  bool speaking() const noexcept { return speaking_; }
  std::uint32_t quiet_frames() const noexcept { return quiet_frames_; }
  const GateConfig& config() const noexcept { return cfg_; }

  void reset() noexcept {
    loud_frames_ = 0;
    quiet_frames_ = 0;
    speaking_ = false;
    has_triggered_ = false;
    last_trigger_ms_ = 0.0;
  }

 private:
  GateConfig cfg_;
  std::uint32_t loud_frames_ = 0;
  std::uint32_t quiet_frames_ = 0;
  bool speaking_ = false;
  bool has_triggered_ = false;
  double last_trigger_ms_ = 0.0;
};

struct ChunkerConfig {
  GateConfig gate{};
  // Frames of audio retained from *before* onset. Without this the gate's
  // sustained-onset requirement eats the first consonant of every utterance,
  // which ASR reports as a word error rather than as a latency problem.
  std::uint32_t pre_roll_frames = 5;
  // Force-flush cap. Bounds worst-case latency when someone talks continuously.
  double max_chunk_ms = 15'000.0;
  // Preallocation ceiling for the assembled buffer.
  std::size_t max_chunk_bytes = 1u << 20;
};

class ChunkAssembler {
 public:
  explicit ChunkAssembler(ChunkerConfig config)
      : cfg_(config), gate_(config.gate), pre_roll_(config.pre_roll_frames) {
    buffer_.reserve(cfg_.max_chunk_bytes);  // the only allocation, at construction
  }

  // Feed one frame. Returns a ChunkEvent on the frame that closes a chunk.
  // The assembled audio is valid until the next call that opens a chunk.
  std::optional<ChunkEvent> on_frame(const AudioFrame& frame, std::uint64_t now_ns) noexcept {
    const AudioEnergy energy = frame_energy(frame);
    const double now_ms = static_cast<double>(now_ns) / 1e6;

    if (last_seq_seen_ && frame.hdr.seq > *last_seq_seen_ + 1) {
      gap_frames_ += static_cast<std::uint32_t>(frame.hdr.seq - *last_seq_seen_ - 1);
    }
    last_seq_seen_ = frame.hdr.seq;

    const bool onset = gate_.accept(energy, now_ms);

    if (onset && !active_) {
      open_chunk(now_ns);
      flush_pre_roll();
    }

    if (active_) {
      append(frame);
    } else {
      pre_roll_.push(frame);
    }

    if (!active_) return std::nullopt;

    if (!gate_.speaking()) {
      return close_chunk(CloseReason::kHangover, now_ns);
    }
    if (elapsed_ms(now_ns) >= cfg_.max_chunk_ms) {
      auto event = close_chunk(CloseReason::kMaxLength, now_ns);
      // Speech is still running; start the next chunk on the same frame so the
      // stream stays gapless. No pre-roll: the audio is already contiguous.
      open_chunk(now_ns);
      return event;
    }
    return std::nullopt;
  }

  // Close whatever is open, e.g. when the transport goes away.
  std::optional<ChunkEvent> flush(std::uint64_t now_ns) noexcept {
    if (!active_) return std::nullopt;
    return close_chunk(CloseReason::kStreamEnd, now_ns);
  }

  const std::uint8_t* chunk_data() const noexcept { return buffer_.data(); }
  std::size_t chunk_size() const noexcept { return buffer_.size(); }
  bool active() const noexcept { return active_; }
  const SpeechThresholdGate& gate() const noexcept { return gate_; }

 private:
  // Fixed-capacity frame ring for pre-roll. Overwrites oldest by construction,
  // which is the correct policy here: stale pre-roll has no value.
  class PreRoll {
   public:
    explicit PreRoll(std::uint32_t capacity)
        : cap_(capacity == 0 ? 0 : capacity), frames_(cap_) {}
    void push(const AudioFrame& f) noexcept {
      if (cap_ == 0) return;
      frames_[head_ % cap_] = f;
      ++head_;
    }
    std::size_t size() const noexcept {
      return head_ < cap_ ? static_cast<std::size_t>(head_) : cap_;
    }
    const AudioFrame& at(std::size_t i) const noexcept {
      const std::uint64_t start = head_ < cap_ ? 0 : head_ - cap_;
      return frames_[(start + i) % cap_];
    }
    void clear() noexcept { head_ = 0; }

   private:
    std::size_t cap_;
    std::vector<AudioFrame> frames_;
    std::uint64_t head_ = 0;
  };

  void open_chunk(std::uint64_t now_ns) noexcept {
    active_ = true;
    buffer_.clear();
    frame_count_ = 0;
    gap_frames_ = 0;
    first_seq_ = 0;
    last_seq_ = 0;
    have_first_ = false;
    t_speech_start_ns_ = now_ns;
    t_first_frame_ns_ = now_ns;
  }

  void flush_pre_roll() noexcept {
    for (std::size_t i = 0; i < pre_roll_.size(); ++i) {
      append(pre_roll_.at(i));
    }
    pre_roll_.clear();
  }

  void append(const AudioFrame& frame) noexcept {
    const std::size_t len = frame.hdr.byte_len;
    if (buffer_.size() + len <= cfg_.max_chunk_bytes) {
      buffer_.insert(buffer_.end(), frame.payload, frame.payload + len);
    }
    if (!have_first_) {
      first_seq_ = frame.hdr.seq;
      t_first_frame_ns_ = frame.hdr.t_produce_ns;
      format_ = frame.hdr.format;
      have_first_ = true;
    }
    last_seq_ = frame.hdr.seq;
    ++frame_count_;
  }

  double elapsed_ms(std::uint64_t now_ns) const noexcept {
    return static_cast<double>(now_ns - t_speech_start_ns_) / 1e6;
  }

  std::optional<ChunkEvent> close_chunk(CloseReason reason, std::uint64_t now_ns) noexcept {
    ChunkEvent event{};
    event.chunk_id = next_chunk_id_++;
    event.first_seq = first_seq_;
    event.last_seq = last_seq_;
    event.t_first_frame_ns = t_first_frame_ns_;
    event.t_speech_start_ns = t_speech_start_ns_;
    event.t_endpoint_ns = now_ns;
    event.frame_count = frame_count_;
    event.byte_count = static_cast<std::uint32_t>(buffer_.size());
    event.dropped_frames = gap_frames_;
    event.reason = static_cast<std::uint16_t>(reason);
    event.format = format_;
    active_ = false;
    return event;
  }

  ChunkerConfig cfg_;
  SpeechThresholdGate gate_;
  PreRoll pre_roll_;
  std::vector<std::uint8_t> buffer_;
  std::optional<std::uint64_t> last_seq_seen_;
  std::uint64_t next_chunk_id_ = 0;
  std::uint64_t first_seq_ = 0;
  std::uint64_t last_seq_ = 0;
  std::uint64_t t_speech_start_ns_ = 0;
  std::uint64_t t_first_frame_ns_ = 0;
  std::uint32_t frame_count_ = 0;
  std::uint32_t gap_frames_ = 0;
  std::uint16_t format_ = 0;
  bool active_ = false;
  bool have_first_ = false;
};

}  // namespace vhp
