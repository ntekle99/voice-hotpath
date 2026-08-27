// vhp_producer -- a deterministic, paced frame source.
//
// This is the reference producer and the load generator. Two properties are
// deliberate:
//
//   Determinism. The signal comes from a fixed-seed xorshift, so the same
//   --seed produces the same audio, the same energy sequence and therefore the
//   same gate decisions. Without that, an A/B of two gate configurations is
//   comparing two different inputs and the difference is noise.
//
//   Absolute-deadline pacing. Frames are emitted on a 20 ms grid anchored at
//   start, not by sleeping 20 ms in a loop. Sleeping for a duration accumulates
//   drift, which shows up in the consumer as a slow bias and is easy to
//   misattribute to the pipeline.
//
// --stall-every-ms / --stall-ms inject a pause on the send path. That is the
// controlled stand-in for a V8 major GC: it is what the Node producer does to
// this pipeline for free, and running with and without it is how you size the
// benefit of moving the path out of the runtime.
#include <getopt.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "vhp/clock.hpp"
#include "vhp/histogram.hpp"
#include "vhp/pipeline.hpp"

namespace {

std::atomic<bool> g_running{true};
void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }

// xorshift64*. Cheap, deterministic, good enough for shaping noise; nobody is
// doing statistics on the bits themselves.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
  std::uint64_t next() noexcept {
    state_ ^= state_ >> 12; state_ ^= state_ << 25; state_ ^= state_ >> 27;
    return state_ * 0x2545F4914F6CDD1DULL;
  }
  // Uniform in [-1, 1).
  double uniform() noexcept {
    return static_cast<double>(static_cast<std::int64_t>(next() >> 11)) / (1ULL << 52) - 1.0;
  }
 private:
  std::uint64_t state_;
};

struct Options {
  std::string frame_shm = vhp::kDefaultFrameShm;
  std::string uds_path;
  bool create_shm = false;
  double frame_ms = 20.0;
  std::uint32_t sample_rate = 24'000;
  vhp::AudioFormat format = vhp::AudioFormat::kPcm16Le;
  double seconds = 10.0;
  double speech_ms = 1500.0;
  double gap_ms = 900.0;
  // Intra-utterance structure. Real speech is not a continuous block: there are
  // short gaps between words, and a hangover shorter than one of them cuts the
  // utterance in half. Without this the endpoint sweep is degenerate -- constant
  // -energy speech can never be falsely cut, so every hangover setting scores a
  // perfect false-cut rate and the frontier is a flat line.
  double word_gap_ms = 0.0;
  double word_every_ms = 320.0;
  double speech_amplitude = 0.25;   // normalised
  double noise_floor = 0.004;       // normalised
  double stall_every_ms = 0.0;
  double stall_ms = 0.0;
  std::uint64_t seed = 0xC0FFEE;
  // Concurrent calls. Each tick emits one frame per session, so N sessions is
  // N x 50 frames/second into the same ring -- the real multi-call load shape.
  std::uint32_t sessions = 1;
  int core = -1;
  bool spin_pace = false;
};

void usage(const char* argv0) {
  std::fprintf(stderr,
      "usage: %s [options]\n"
      "  sink:      --shm-frames NAME | --uds PATH   (--create to make the shm ring)\n"
      "  load:      --sessions N    concurrent calls, one frame each per tick\n"
      "  signal:    --seconds F --speech-ms F --gap-ms F --amplitude F --noise F --seed N\n"
      "             --word-gap-ms F --word-every-ms F   (inter-word pauses inside an\n"
      "             utterance; a hangover shorter than word-gap-ms will falsely cut it)\n"
      "  format:    --rate HZ --mulaw --frame-ms F\n"
      "  stalls:    --stall-every-ms F --stall-ms F   (emulate a GC pause on the send path)\n"
      "  placement: --core N --spin\n", argv0);
}

bool parse_args(int argc, char** argv, Options& opt) {
  enum { kShm = 1000, kUds, kCreate, kSeconds, kSpeech, kGap, kWordGap, kWordEvery, kAmp,
         kNoise, kSeed, kRate, kMulaw, kFrameMs, kStallEvery, kStall, kCore, kSpin,
         kSessions, kHelp };
  static option longopts[] = {
      {"shm-frames", required_argument, nullptr, kShm},
      {"uds", required_argument, nullptr, kUds},
      {"create", no_argument, nullptr, kCreate},
      {"seconds", required_argument, nullptr, kSeconds},
      {"speech-ms", required_argument, nullptr, kSpeech},
      {"gap-ms", required_argument, nullptr, kGap},
      {"word-gap-ms", required_argument, nullptr, kWordGap},
      {"word-every-ms", required_argument, nullptr, kWordEvery},
      {"amplitude", required_argument, nullptr, kAmp},
      {"noise", required_argument, nullptr, kNoise},
      {"seed", required_argument, nullptr, kSeed},
      {"sessions", required_argument, nullptr, kSessions},
      {"rate", required_argument, nullptr, kRate},
      {"mulaw", no_argument, nullptr, kMulaw},
      {"frame-ms", required_argument, nullptr, kFrameMs},
      {"stall-every-ms", required_argument, nullptr, kStallEvery},
      {"stall-ms", required_argument, nullptr, kStall},
      {"core", required_argument, nullptr, kCore},
      {"spin", no_argument, nullptr, kSpin},
      {"help", no_argument, nullptr, kHelp},
      {nullptr, 0, nullptr, 0}};
  int c;
  while ((c = getopt_long(argc, argv, "", longopts, nullptr)) != -1) {
    switch (c) {
      case kShm: opt.frame_shm = optarg; break;
      case kUds: opt.uds_path = optarg; break;
      case kCreate: opt.create_shm = true; break;
      case kSeconds: opt.seconds = std::atof(optarg); break;
      case kSpeech: opt.speech_ms = std::atof(optarg); break;
      case kGap: opt.gap_ms = std::atof(optarg); break;
      case kWordGap: opt.word_gap_ms = std::atof(optarg); break;
      case kWordEvery: opt.word_every_ms = std::atof(optarg); break;
      case kAmp: opt.speech_amplitude = std::atof(optarg); break;
      case kNoise: opt.noise_floor = std::atof(optarg); break;
      case kSeed: opt.seed = std::strtoull(optarg, nullptr, 0); break;
      case kSessions:
        opt.sessions = static_cast<std::uint32_t>(std::strtoul(optarg, nullptr, 10));
        if (opt.sessions == 0) opt.sessions = 1;
        break;
      case kRate: opt.sample_rate = static_cast<std::uint32_t>(std::strtoul(optarg, nullptr, 10)); break;
      case kMulaw: opt.format = vhp::AudioFormat::kMulaw8; opt.sample_rate = 8'000; break;
      case kFrameMs: opt.frame_ms = std::atof(optarg); break;
      case kStallEvery: opt.stall_every_ms = std::atof(optarg); break;
      case kStall: opt.stall_ms = std::atof(optarg); break;
      case kCore: opt.core = std::atoi(optarg); break;
      case kSpin: opt.spin_pace = true; break;
      case kHelp: default: usage(argv[0]); return false;
    }
  }
  return true;
}

std::int16_t encode_mulaw(std::int16_t sample) {
  // ITU G.711, matching linearToMulaw() in src/talk/audio-codec.ts.
  constexpr int kBias = 132, kClip = 32635;
  int value = sample;
  const int sign = value < 0 ? 0x80 : 0;
  if (value < 0) value = -value;
  if (value > kClip) value = kClip;
  value += kBias;
  int exponent = 7;
  for (int mask = 0x4000; (value & mask) == 0 && exponent > 0; mask >>= 1) --exponent;
  const int mantissa = (value >> (exponent + 3)) & 0x0f;
  return static_cast<std::int16_t>(~(sign | (exponent << 4) | mantissa) & 0xff);
}

void fill_payload(vhp::AudioFrame& frame, Rng& rng, double amplitude, vhp::AudioFormat format,
                  std::size_t samples) {
  if (format == vhp::AudioFormat::kPcm16Le) {
    for (std::size_t i = 0; i < samples; ++i) {
      const auto s = static_cast<std::int16_t>(rng.uniform() * amplitude * 32767.0);
      std::memcpy(frame.payload + i * 2, &s, 2);
    }
    frame.hdr.byte_len = static_cast<std::uint32_t>(samples * 2);
  } else {
    for (std::size_t i = 0; i < samples; ++i) {
      const auto s = static_cast<std::int16_t>(rng.uniform() * amplitude * 32767.0);
      frame.payload[i] = static_cast<std::uint8_t>(encode_mulaw(s));
    }
    frame.hdr.byte_len = static_cast<std::uint32_t>(samples);
  }
}

int connect_uds(const std::string& path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) { std::perror("socket"); return -1; }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) return fd;
    vhp::sleep_until_ns(vhp::now_ns() + 100'000'000ULL);
  }
  std::perror("connect");
  ::close(fd);
  return -1;
}

bool write_all(int fd, const std::uint8_t* data, std::size_t len) {
  while (len > 0) {
    const ssize_t n = ::write(fd, data, len);
    if (n <= 0) return false;
    data += n;
    len -= static_cast<std::size_t>(n);
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse_args(argc, argv, opt)) return 2;
  ::signal(SIGINT, on_signal);
  ::signal(SIGPIPE, SIG_IGN);
  if (opt.core >= 0) vhp::pin_to_core(opt.core);

  const std::size_t samples_per_frame =
      static_cast<std::size_t>(opt.sample_rate * opt.frame_ms / 1000.0);
  const std::size_t payload_bytes =
      samples_per_frame * vhp::bytes_per_sample(opt.format);
  if (payload_bytes > vhp::kFramePayloadBytes) {
    std::fprintf(stderr, "frame of %zu B exceeds the %zu B slot payload\n", payload_bytes,
                 vhp::kFramePayloadBytes);
    return 2;
  }

  std::unique_ptr<vhp::FrameRing> shm;
  int uds_fd = -1;
  if (!opt.uds_path.empty()) {
    uds_fd = connect_uds(opt.uds_path);
    if (uds_fd < 0) return 1;
  } else {
    try {
      shm = std::make_unique<vhp::FrameRing>(opt.create_shm
                                                 ? vhp::FrameRing::create(opt.frame_shm)
                                                 : vhp::FrameRing::attach(opt.frame_shm));
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[prod] %s\n", e.what());
      return 1;
    }
  }

  Rng rng(opt.seed);
  vhp::Histogram pacing{1'000'000'000ULL};
  const std::uint64_t frame_ns = static_cast<std::uint64_t>(opt.frame_ms * 1e6);
  const auto total_frames =
      static_cast<std::uint64_t>(opt.seconds * 1000.0 / opt.frame_ms);
  const std::uint64_t start_ns = vhp::now_ns();

  std::vector<std::uint8_t> wire(4 + sizeof(vhp::FrameHeader) + vhp::kFramePayloadBytes);
  vhp::AudioFrame frame{};
  frame.hdr.sample_rate = opt.sample_rate;
  frame.hdr.format = static_cast<std::uint16_t>(opt.format);

  std::uint64_t dropped = 0;
  std::uint64_t next_stall_ns =
      opt.stall_every_ms > 0.0
          ? start_ns + static_cast<std::uint64_t>(opt.stall_every_ms * 1e6)
          : UINT64_MAX;
  const double cycle_ms = opt.speech_ms + opt.gap_ms;

  for (std::uint64_t seq = 0; seq < total_frames && g_running.load(std::memory_order_relaxed);
       ++seq) {
    const std::uint64_t target_ns = start_ns + seq * frame_ns;
    if (opt.spin_pace) vhp::spin_until_ns(target_ns);
    else vhp::sleep_until_ns(target_ns);

    const std::uint64_t woke_ns = vhp::now_ns();
    // How late we were versus the grid, corrected for coordinated omission: a
    // long stall means several frames were never emitted, and their lateness is
    // exactly what a naive "measure each send" loop silently discards.
    pacing.record_with_expected_interval(woke_ns > target_ns ? woke_ns - target_ns : 0, frame_ns);

    if (woke_ns >= next_stall_ns) {
      vhp::spin_until_ns(woke_ns + static_cast<std::uint64_t>(opt.stall_ms * 1e6));
      next_stall_ns = vhp::now_ns() + static_cast<std::uint64_t>(opt.stall_every_ms * 1e6);
    }

    bool write_failed = false;
    for (std::uint32_t session = 0; session < opt.sessions; ++session) {
      // Stagger each call's utterance schedule across the cycle. Synchronised
      // callers are the easy case: every session goes quiet at the same instant,
      // so the endpoint work never overlaps and the measured capacity is
      // flattering and wrong.
      const double offset_ms =
          cycle_ms * static_cast<double>(session) / static_cast<double>(opt.sessions);
      const double phase_ms = std::fmod(static_cast<double>(seq) * opt.frame_ms + offset_ms,
                                        cycle_ms > 0 ? cycle_ms : 1.0);
      bool speech = phase_ms < opt.speech_ms;
      if (speech && opt.word_gap_ms > 0.0 && opt.word_every_ms > 0.0) {
        // Silent for word_gap_ms at the end of every word_every_ms window. The
        // first word starts at phase 0 so an utterance always opens with speech.
        speech = std::fmod(phase_ms, opt.word_every_ms) < (opt.word_every_ms - opt.word_gap_ms);
      }
      fill_payload(frame, rng, speech ? opt.speech_amplitude : opt.noise_floor, opt.format,
                   samples_per_frame);

      frame.hdr.seq = seq;
      frame.hdr.session_id = session;
      frame.hdr.t_capture_ns = target_ns;
      frame.hdr.flags = seq == 0 ? vhp::kFlagStreamStart : vhp::kFlagNone;
      frame.hdr.t_produce_ns = vhp::now_ns();

      if (shm) {
        if (shm->ring().try_push(frame) == vhp::PushResult::kFull) ++dropped;
      } else {
        const std::uint32_t len = frame.hdr.byte_len;
        std::memcpy(wire.data(), &len, 4);
        std::memcpy(wire.data() + 4, &frame.hdr, sizeof(vhp::FrameHeader));
        std::memcpy(wire.data() + 4 + sizeof(vhp::FrameHeader), frame.payload, len);
        if (!write_all(uds_fd, wire.data(), 4 + sizeof(vhp::FrameHeader) + len)) {
          write_failed = true;
          break;
        }
      }
    }
    if (write_failed) break;
  }

  if (uds_fd >= 0) ::close(uds_fd);
  std::fprintf(stderr, "\n--- vhp producer ---------------------------------------------------\n");
  std::fputs(pacing.summary("send lateness vs grid").c_str(), stderr);
  std::fprintf(stderr,
               "sessions=%u frames=%llu (%llu per session) dropped_by_backpressure=%llu "
               "seed=0x%llx\n",
               opt.sessions,
               static_cast<unsigned long long>(total_frames) * opt.sessions,
               static_cast<unsigned long long>(total_frames),
               static_cast<unsigned long long>(dropped),
               static_cast<unsigned long long>(opt.seed));
  std::fprintf(stderr, "--------------------------------------------------------------------\n");
  return 0;
}
