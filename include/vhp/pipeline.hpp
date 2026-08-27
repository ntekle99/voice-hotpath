// Shared sizing, names and the wire format used by the TypeScript bridge.
#pragma once

#include <cstdint>

#include "vhp/audio_frame.hpp"
#include "vhp/shm_ring.hpp"

namespace vhp {

// 1024 slots at 20 ms is 20.5 s of buffer -- far more than the pipeline should
// ever hold. It is sized so that the ring being full always means something is
// genuinely wrong (a wedged consumer), never ordinary scheduling noise.
inline constexpr std::size_t kFrameRingCapacity = 1024;
// Sized for the burst, not the average. Chunk events are rare per call (~1 every
// 2 s) but they arrive in clusters: staggered calls still endpoint in waves, and
// at 512 concurrent sessions a wave is hundreds of events while the publisher is
// mid-write. 256 slots dropped events at that load; 4096 does not, and it costs
// 256 KB.
inline constexpr std::size_t kEventRingCapacity = 4096;

inline constexpr const char* kDefaultFrameShm = "/vhp_frames";
inline constexpr const char* kDefaultEventShm = "/vhp_events";

using FrameRing = ShmRing<AudioFrame, kFrameRingCapacity>;
using EventRing = ShmRing<ChunkEvent, kEventRingCapacity>;
using FrameQueue = SpscRing<AudioFrame, kFrameRingCapacity, true>;
using EventQueue = SpscRing<ChunkEvent, kEventRingCapacity, true>;

// Unix-domain-socket wire format, for feeding the hot path from the existing
// Node gateway without a native addon:
//
//   [u32 le payload_len][FrameHeader (40 B, little-endian)][payload_len bytes]
//
// Little-endian and packed on both sides. Node's Buffer writes match the C++
// struct layout exactly; test/test_wire.cpp asserts the offsets so a change to
// FrameHeader cannot silently break the bridge.
inline constexpr std::uint32_t kMaxWirePayload = kFramePayloadBytes;

static_assert(offsetof(FrameHeader, seq) == 0);
static_assert(offsetof(FrameHeader, t_produce_ns) == 8);
static_assert(offsetof(FrameHeader, t_capture_ns) == 16);
static_assert(offsetof(FrameHeader, byte_len) == 24);
static_assert(offsetof(FrameHeader, sample_rate) == 28);
static_assert(offsetof(FrameHeader, format) == 32);
static_assert(offsetof(FrameHeader, flags) == 34);
static_assert(offsetof(FrameHeader, session_id) == 36);

}  // namespace vhp
