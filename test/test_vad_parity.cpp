// Golden-vector parity between the C++ gate and the TypeScript one.
//
// test/vad_reference.json is produced by tools/gen_vad_reference.mjs, which
// imports the REAL src/talk/audio-energy.ts. This test regenerates the identical
// audio in C++ and asserts:
//
//   * every frame's RMS and peak match to within float noise, and
//   * every onset decision matches exactly.
//
// The decisions are the part that must be exact. Energies can differ in the last
// bits without consequence; a single disagreeing decision means the two
// implementations would chunk differently, and every A/B run through the replay
// harness would be measuring that difference instead of the change under test.
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "test_util.hpp"
#include "vhp/energy.hpp"
#include "vhp/vad.hpp"

namespace {

struct RefFrame { double rms; double peak; bool accepted; };

// Purpose-built scanner for the one JSON shape we emit. Pulling in a JSON
// library for four fields would be a worse trade than thirty lines here.
std::vector<RefFrame> load_reference(const std::string& path) {
  std::ifstream in(path);
  if (!in) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return {}; }
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();

  std::vector<RefFrame> frames;
  std::size_t pos = 0;
  while ((pos = text.find("\"rms\":", pos)) != std::string::npos) {
    RefFrame frame{};
    frame.rms = std::strtod(text.c_str() + pos + 6, nullptr);
    const std::size_t peak_pos = text.find("\"peak\":", pos);
    const std::size_t acc_pos = text.find("\"accepted\":", pos);
    if (peak_pos == std::string::npos || acc_pos == std::string::npos) break;
    frame.peak = std::strtod(text.c_str() + peak_pos + 7, nullptr);
    // Skip whitespace after the colon: the reference is pretty-printed, and a
    // fixed offset here silently reads every boolean as false.
    std::size_t value_pos = acc_pos + 11;
    while (value_pos < text.size() && std::isspace(static_cast<unsigned char>(text[value_pos]))) {
      ++value_pos;
    }
    frame.accepted = text.compare(value_pos, 4, "true") == 0;
    frames.push_back(frame);
    pos = acc_pos;
  }
  return frames;
}

// Byte-identical to Rng in src/vhp_producer.cpp and to the BigInt version in
// tools/gen_vad_reference.mjs.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}
  std::uint64_t next() noexcept {
    state_ ^= state_ >> 12; state_ ^= state_ << 25; state_ ^= state_ >> 27;
    return state_ * 0x2545F4914F6CDD1DULL;
  }
  double uniform() noexcept {
    return static_cast<double>(static_cast<std::int64_t>(next() >> 11)) / (1ULL << 52) - 1.0;
  }
 private:
  std::uint64_t state_;
};

}  // namespace

int main(int argc, char** argv) {
  const std::string path = argc > 1 ? argv[1] : "test/vad_reference.json";
  const auto reference = load_reference(path);
  CHECK_MSG(!reference.empty(), "reference vector missing -- run tools/gen_vad_reference.mjs");
  if (reference.empty()) return vhptest::summarize("test_vad_parity");

  // Must mirror the constants in tools/gen_vad_reference.mjs.
  constexpr std::uint32_t kSampleRate = 24'000;
  constexpr double kFrameMs = 20.0;
  constexpr std::size_t kSamples = static_cast<std::size_t>(kSampleRate * kFrameMs / 1000.0);
  constexpr double kSpeechMs = 1500.0, kGapMs = 900.0;
  constexpr double kAmplitude = 0.25, kNoise = 0.004;

  vhp::GateConfig gate_config;
  gate_config.rms_threshold = 0.02;
  gate_config.peak_threshold = 0.10;
  gate_config.speech_frames = 2;
  gate_config.silence_frames = 12;
  gate_config.cooldown_ms = 0.0;
  vhp::SpeechThresholdGate gate(gate_config);

  Rng rng(0xC0FFEE);
  std::vector<std::uint8_t> pcm(kSamples * 2);
  int decision_mismatches = 0;
  double worst_rms_delta = 0.0, worst_peak_delta = 0.0;

  for (std::size_t seq = 0; seq < reference.size(); ++seq) {
    const double phase_ms = std::fmod(static_cast<double>(seq) * kFrameMs, kSpeechMs + kGapMs);
    const double amplitude = phase_ms < kSpeechMs ? kAmplitude : kNoise;
    for (std::size_t i = 0; i < kSamples; ++i) {
      const auto sample = static_cast<std::int16_t>(rng.uniform() * amplitude * 32767.0);
      std::memcpy(pcm.data() + i * 2, &sample, 2);
    }
    const vhp::AudioEnergy energy = vhp::energy_pcm16(pcm.data(), pcm.size());
    const bool accepted = gate.accept(energy, static_cast<double>(seq) * kFrameMs);

    worst_rms_delta = std::max(worst_rms_delta, std::fabs(energy.rms_norm - reference[seq].rms));
    worst_peak_delta = std::max(worst_peak_delta, std::fabs(energy.peak_norm - reference[seq].peak));
    if (accepted != reference[seq].accepted) {
      ++decision_mismatches;
      if (decision_mismatches <= 5) {
        std::fprintf(stderr, "  frame %zu: cpp=%d ts=%d (rms %.9f vs %.9f)\n", seq, accepted,
                     reference[seq].accepted, energy.rms_norm, reference[seq].rms);
      }
    }
  }

  std::fprintf(stderr, "parity over %zu frames: worst rms delta %.3e, worst peak delta %.3e\n",
               reference.size(), worst_rms_delta, worst_peak_delta);
  CHECK_EQ(decision_mismatches, 0);
  CHECK(worst_rms_delta < 1e-12);
  CHECK(worst_peak_delta < 1e-12);
  return vhptest::summarize("test_vad_parity");
}
