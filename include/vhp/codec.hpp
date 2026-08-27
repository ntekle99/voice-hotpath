// G.711 mu-law decode, as a compile-time table.
//
// Bit-for-bit port of mulawToLinear() in src/talk/audio-codec.ts. There are 256
// possible inputs, so the "decoder" is a constexpr lookup table built at compile
// time -- no branches, no shifts, one L1 hit per sample.
#pragma once

#include <array>
#include <cstdint>

namespace vhp {

constexpr std::int16_t mulaw_to_linear(std::uint8_t value) {
  const int mu = (~static_cast<int>(value)) & 0xff;
  const int sign = mu & 0x80;
  const int exponent = (mu >> 4) & 0x07;
  const int mantissa = mu & 0x0f;
  int sample = ((mantissa << 3) + 132) << exponent;
  sample -= 132;
  const int signed_sample = sign ? -sample : sample;
  // clamp16, mirroring the TypeScript. The table maxes out at 32124 so this is
  // a no-op today; it stays so the port cannot silently diverge if the constants
  // are ever touched on either side.
  return static_cast<std::int16_t>(signed_sample < -32768   ? -32768
                                   : signed_sample > 32767  ? 32767
                                                            : signed_sample);
}

inline constexpr std::array<std::int16_t, 256> kMulawTable = [] {
  std::array<std::int16_t, 256> table{};
  for (int i = 0; i < 256; ++i) {
    table[static_cast<std::size_t>(i)] = mulaw_to_linear(static_cast<std::uint8_t>(i));
  }
  return table;
}();

static_assert(kMulawTable[0xff] == 0);
static_assert(kMulawTable[0x00] == -32124);

}  // namespace vhp
