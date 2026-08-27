// Frame energy: absolute peak and RMS.
//
// Port of readPcm16AudioStats / calculateMulawRms from src/talk/audio-energy.ts.
//
// One inconsistency in the TypeScript is worth naming rather than inheriting:
// readPcm16AudioStats reports RMS in raw int16 units (0..32768) while
// calculateMulawRms reports it normalised (0..1), so the same numeric threshold
// means different things depending on the transport. We compute both and treat
// the normalised pair as canonical for thresholds, which makes one gate config
// valid for both formats.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "vhp/audio_frame.hpp"
#include "vhp/codec.hpp"

namespace vhp {

inline constexpr double kPcm16MaxAmplitude = 32768.0;

struct AudioEnergy {
  double peak = 0.0;       // raw int16 units, matches readPcm16AudioStats().peak
  double rms = 0.0;        // raw int16 units, matches readPcm16AudioStats().rms
  double peak_norm = 0.0;  // peak / 32768
  double rms_norm = 0.0;   // matches calculateMulawRms()
  std::uint32_t samples = 0;
};

// Accumulating in int64 rather than double: an int16 square is <= 2^30 and a
// 20 ms frame is <= 480 samples, so the sum cannot exceed 2^39 -- exact, and it
// keeps the inner loop integer-only so the compiler can vectorise it.
inline AudioEnergy energy_pcm16(const std::uint8_t* bytes, std::size_t byte_len) noexcept {
  const std::size_t n = byte_len / 2;
  std::int64_t sum_squares = 0;
  std::int32_t peak = 0;
  for (std::size_t i = 0; i < n; ++i) {
    std::int16_t sample;
    std::memcpy(&sample, bytes + i * 2, sizeof(sample));
    const std::int32_t magnitude = sample < 0 ? -static_cast<std::int32_t>(sample)
                                              : static_cast<std::int32_t>(sample);
    if (magnitude > peak) peak = magnitude;
    sum_squares += static_cast<std::int64_t>(sample) * static_cast<std::int64_t>(sample);
  }
  AudioEnergy out;
  out.samples = static_cast<std::uint32_t>(n);
  out.peak = static_cast<double>(peak);
  out.rms = n > 0 ? std::sqrt(static_cast<double>(sum_squares) / static_cast<double>(n)) : 0.0;
  out.peak_norm = out.peak / kPcm16MaxAmplitude;
  out.rms_norm = out.rms / kPcm16MaxAmplitude;
  return out;
}

inline AudioEnergy energy_mulaw(const std::uint8_t* bytes, std::size_t byte_len) noexcept {
  std::int64_t sum_squares = 0;
  std::int32_t peak = 0;
  for (std::size_t i = 0; i < byte_len; ++i) {
    const std::int32_t sample = kMulawTable[bytes[i]];
    const std::int32_t magnitude = sample < 0 ? -sample : sample;
    if (magnitude > peak) peak = magnitude;
    sum_squares += static_cast<std::int64_t>(sample) * static_cast<std::int64_t>(sample);
  }
  AudioEnergy out;
  out.samples = static_cast<std::uint32_t>(byte_len);
  out.peak = static_cast<double>(peak);
  out.rms = byte_len > 0
                ? std::sqrt(static_cast<double>(sum_squares) / static_cast<double>(byte_len))
                : 0.0;
  out.peak_norm = out.peak / kPcm16MaxAmplitude;
  out.rms_norm = out.rms / kPcm16MaxAmplitude;
  return out;
}

inline AudioEnergy frame_energy(const AudioFrame& frame) noexcept {
  return static_cast<AudioFormat>(frame.hdr.format) == AudioFormat::kMulaw8
             ? energy_mulaw(frame.payload, frame.hdr.byte_len)
             : energy_pcm16(frame.payload, frame.hdr.byte_len);
}

}  // namespace vhp
