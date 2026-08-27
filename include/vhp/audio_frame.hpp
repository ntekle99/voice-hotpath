// The unit that crosses the process boundary.
//
// Fixed-size slots, deliberately. A variable-length frame would need either an
// allocation or a second indirection into a payload arena, and neither belongs
// on this path. 1024 bytes covers the two formats the gateway emits:
//   * pcm16 @ 24 kHz mono, 20 ms  -> 480 samples -> 960 bytes
//   * G.711 mu-law @ 8 kHz, 20 ms -> 160 samples -> 160 bytes
// The cost is ~7% slack on the larger format, paid in L2, in exchange for a
// branch-free index calculation and a ring that never allocates.
#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace vhp {

inline constexpr std::size_t kFramePayloadBytes = 1024;

enum class AudioFormat : std::uint16_t {
  kPcm16Le = 0,   // signed 16-bit little-endian, mono
  kMulaw8 = 1,    // G.711 mu-law, mono
};

enum FrameFlags : std::uint16_t {
  kFlagNone = 0,
  kFlagStreamStart = 1u << 0,
  kFlagStreamEnd = 1u << 1,
};

struct FrameHeader {
  std::uint64_t seq;            // monotonic, producer-assigned; gaps == loss
  std::uint64_t t_produce_ns;   // CLOCK_MONOTONIC at enqueue
  std::uint64_t t_capture_ns;   // when the transport received it (0 if unknown)
  std::uint32_t byte_len;       // valid bytes in payload
  std::uint32_t sample_rate;
  std::uint16_t format;         // AudioFormat
  std::uint16_t flags;          // FrameFlags
  std::uint32_t session_id;     // which call this frame belongs to
};

struct AudioFrame {
  FrameHeader hdr;
  std::uint8_t payload[kFramePayloadBytes];
};

static_assert(std::is_trivially_copyable_v<AudioFrame>);
static_assert(sizeof(FrameHeader) == 40, "header layout is an ABI contract");
static_assert(sizeof(AudioFrame) == 40 + kFramePayloadBytes);

inline constexpr std::size_t bytes_per_sample(AudioFormat f) {
  return f == AudioFormat::kPcm16Le ? 2 : 1;
}

inline double frame_duration_ms(const FrameHeader& h) {
  const std::size_t bps = bytes_per_sample(static_cast<AudioFormat>(h.format));
  if (h.sample_rate == 0 || bps == 0) return 0.0;
  return 1000.0 * static_cast<double>(h.byte_len / bps) / static_cast<double>(h.sample_rate);
}

// What the hot path publishes back to the orchestrator: a closed speech chunk,
// ready to be shipped to ASR. The audio itself is not copied here -- the
// consumer owns the assembled buffer and the event carries its identity.
struct ChunkEvent {
  std::uint32_t session_id;
  std::uint32_t pad_;
  std::uint64_t chunk_id;
  std::uint64_t first_seq;
  std::uint64_t last_seq;
  std::uint64_t t_first_frame_ns;    // produce-stamp of the first speech frame
  std::uint64_t t_speech_start_ns;   // when onset was declared
  std::uint64_t t_endpoint_ns;       // when the chunk was closed
  std::uint32_t frame_count;
  std::uint32_t byte_count;
  std::uint32_t dropped_frames;      // gaps inside this chunk
  std::uint16_t reason;              // CloseReason
  std::uint16_t format;
};

enum class CloseReason : std::uint16_t {
  kHangover = 0,   // silence held long enough -- the normal endpoint
  kMaxLength = 1,  // chunk hit the duration cap and was force-flushed
  kStreamEnd = 2,  // transport closed
};

static_assert(std::is_trivially_copyable_v<ChunkEvent>);

}  // namespace vhp
