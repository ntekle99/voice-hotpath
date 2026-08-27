// vhp_consumer -- the audio hot path.
//
//   ingest (shm from the gateway, or a UDS reader thread)
//     -> SPSC ring
//       -> pinned VAD thread: energy + gate + chunk assembly   [never blocks]
//         -> SPSC event ring
//           -> publisher thread: JSONL on stdout               [all syscalls]
//
// The pinned thread does no allocation, no locking, no I/O and no logging. Every
// syscall in the process happens on the ingest or publisher thread. That is the
// entire discipline; the numbers follow from it.
#include <getopt.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "vhp/clock.hpp"
#include "vhp/metrics.hpp"
#include "vhp/pipeline.hpp"
#include "vhp/session.hpp"
#include "vhp/vad.hpp"

namespace {

std::atomic<bool> g_running{true};
void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }

struct Options {
  std::string frame_shm = vhp::kDefaultFrameShm;
  std::string event_shm = vhp::kDefaultEventShm;
  std::string uds_path;              // non-empty selects UDS ingest
  int core = -1;                     // -1 = do not pin
  bool realtime = false;
  bool spin = true;
  double frame_ms = 20.0;
  double seconds = 0.0;              // 0 = until signalled
  vhp::ChunkerConfig chunker{};
  std::uint32_t max_sessions = 32;
  double session_idle_ms = 2000.0;
  std::string csv_prefix;
};

void usage(const char* argv0) {
  std::fprintf(stderr,
      "usage: %s [options]\n"
      "  ingest:\n"
      "    --shm-frames NAME     shared-memory frame ring (default %s)\n"
      "    --shm-events NAME     shared-memory event ring (default %s)\n"
      "    --uds PATH            listen on a unix socket instead of shm\n"
      "  placement:\n"
      "    --core N              pin the vad thread to core N\n"
      "    --rt                  request SCHED_FIFO (needs privileges)\n"
      "    --yield               sched_yield when idle instead of spinning\n"
      "  gate (see README for the latency/false-cut sweep):\n"
      "    --rms F               normalised rms threshold (default 0.02)\n"
      "    --peak F              normalised peak threshold (default 0.10)\n"
      "    --speech-frames N     sustained frames before onset (default 2)\n"
      "    --silence-frames N    hangover frames before endpoint (default 12)\n"
      "    --preroll N           frames kept before onset (default 5)\n"
      "    --max-chunk-ms F      force-flush cap (default 15000)\n"
      "  sessions:\n"
      "    --max-sessions N      concurrent calls admitted (default 32)\n"
      "    --session-idle-ms F   close a call after this much silence (default 2000)\n"
      "  run:\n"
      "    --frame-ms F          nominal cadence for jitter (default 20)\n"
      "    --seconds F           stop after N seconds\n"
      "    --csv-prefix PATH     dump histograms to PATH-<metric>.csv\n",
      argv0, vhp::kDefaultFrameShm, vhp::kDefaultEventShm);
}

bool parse_args(int argc, char** argv, Options& opt) {
  enum {
    kFrameShm = 1000, kEventShm, kUds, kCore, kRt, kYield, kRms, kPeak,
    kSpeechFrames, kSilenceFrames, kPreroll, kMaxChunk, kFrameMs, kSeconds, kCsv,
    kMaxSessions, kSessionIdle, kHelp
  };
  static option longopts[] = {
      {"shm-frames", required_argument, nullptr, kFrameShm},
      {"shm-events", required_argument, nullptr, kEventShm},
      {"uds", required_argument, nullptr, kUds},
      {"core", required_argument, nullptr, kCore},
      {"rt", no_argument, nullptr, kRt},
      {"yield", no_argument, nullptr, kYield},
      {"rms", required_argument, nullptr, kRms},
      {"peak", required_argument, nullptr, kPeak},
      {"speech-frames", required_argument, nullptr, kSpeechFrames},
      {"silence-frames", required_argument, nullptr, kSilenceFrames},
      {"preroll", required_argument, nullptr, kPreroll},
      {"max-chunk-ms", required_argument, nullptr, kMaxChunk},
      {"frame-ms", required_argument, nullptr, kFrameMs},
      {"seconds", required_argument, nullptr, kSeconds},
      {"csv-prefix", required_argument, nullptr, kCsv},
      {"max-sessions", required_argument, nullptr, kMaxSessions},
      {"session-idle-ms", required_argument, nullptr, kSessionIdle},
      {"help", no_argument, nullptr, kHelp},
      {nullptr, 0, nullptr, 0}};
  int c;
  while ((c = getopt_long(argc, argv, "", longopts, nullptr)) != -1) {
    switch (c) {
      case kFrameShm: opt.frame_shm = optarg; break;
      case kEventShm: opt.event_shm = optarg; break;
      case kUds: opt.uds_path = optarg; break;
      case kCore: opt.core = std::atoi(optarg); break;
      case kRt: opt.realtime = true; break;
      case kYield: opt.spin = false; break;
      case kRms: opt.chunker.gate.rms_threshold = std::atof(optarg); break;
      case kPeak: opt.chunker.gate.peak_threshold = std::atof(optarg); break;
      case kSpeechFrames: opt.chunker.gate.speech_frames = static_cast<std::uint32_t>(std::strtoul(optarg, nullptr, 10)); break;
      case kSilenceFrames: opt.chunker.gate.silence_frames = static_cast<std::uint32_t>(std::strtoul(optarg, nullptr, 10)); break;
      case kPreroll: opt.chunker.pre_roll_frames = static_cast<std::uint32_t>(std::strtoul(optarg, nullptr, 10)); break;
      case kMaxChunk: opt.chunker.max_chunk_ms = std::atof(optarg); break;
      case kFrameMs: opt.frame_ms = std::atof(optarg); break;
      case kSeconds: opt.seconds = std::atof(optarg); break;
      case kCsv: opt.csv_prefix = optarg; break;
      case kMaxSessions:
        opt.max_sessions = static_cast<std::uint32_t>(std::strtoul(optarg, nullptr, 10));
        break;
      case kSessionIdle: opt.session_idle_ms = std::atof(optarg); break;
      case kHelp: default: usage(argv[0]); return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------- UDS ingest
//
// Node cannot mmap shared memory without a native addon, so this path exists to
// let the existing TypeScript gateway feed the hot path today. It costs a copy
// and a syscall per frame relative to shm -- measured, not assumed: run both and
// compare the "shm transport+queue" line.
class UdsIngest {
 public:
  UdsIngest(std::string path, vhp::FrameQueue& queue, vhp::HotPathMetrics& metrics)
      : path_(std::move(path)), queue_(queue), metrics_(metrics) {}

  bool start() {
    ::unlink(path_.c_str());
    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) { std::perror("socket"); return false; }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      std::perror("bind"); return false;
    }
    if (::listen(listen_fd_, 1) != 0) { std::perror("listen"); return false; }
    std::fprintf(stderr, "[vhp] listening on %s\n", path_.c_str());
    thread_ = std::thread([this] { run(); });
    return true;
  }

  void stop() {
    if (listen_fd_ >= 0) { ::shutdown(listen_fd_, SHUT_RDWR); ::close(listen_fd_); listen_fd_ = -1; }
    if (conn_fd_ >= 0) { ::shutdown(conn_fd_, SHUT_RDWR); }
    if (thread_.joinable()) thread_.join();
    ::unlink(path_.c_str());
  }

 private:
  void run() {
    while (g_running.load(std::memory_order_relaxed)) {
      conn_fd_ = ::accept(listen_fd_, nullptr, nullptr);
      if (conn_fd_ < 0) return;
      std::fprintf(stderr, "[vhp] producer connected\n");
      pump();
      ::close(conn_fd_);
      conn_fd_ = -1;
      std::fprintf(stderr, "[vhp] producer disconnected\n");
    }
  }

  // Length-prefixed frames. Partial reads are normal on a stream socket; the
  // buffer carries the remainder across read() calls.
  void pump() {
    std::vector<std::uint8_t> buf(1 << 16);
    std::size_t filled = 0;
    while (g_running.load(std::memory_order_relaxed)) {
      const ssize_t n = ::read(conn_fd_, buf.data() + filled, buf.size() - filled);
      if (n <= 0) return;
      filled += static_cast<std::size_t>(n);
      std::size_t offset = 0;
      while (filled - offset >= 4) {
        std::uint32_t payload_len;
        std::memcpy(&payload_len, buf.data() + offset, 4);
        if (payload_len > vhp::kMaxWirePayload) {
          std::fprintf(stderr, "[vhp] oversized frame (%u), dropping connection\n", payload_len);
          return;
        }
        const std::size_t need = 4 + sizeof(vhp::FrameHeader) + payload_len;
        if (filled - offset < need) break;
        vhp::AudioFrame frame{};
        std::memcpy(&frame.hdr, buf.data() + offset + 4, sizeof(vhp::FrameHeader));
        std::memcpy(frame.payload, buf.data() + offset + 4 + sizeof(vhp::FrameHeader), payload_len);
        frame.hdr.byte_len = payload_len;
        if (frame.hdr.t_produce_ns == 0) frame.hdr.t_produce_ns = vhp::now_ns();
        if (queue_.try_push(frame) == vhp::PushResult::kFull) {
          // Bounded queue, explicit shed. Unbounded buffering here would trade a
          // visible drop for an invisible and unbounded latency climb.
          ++metrics_.ring_full_drops;
        }
        offset += need;
      }
      if (offset > 0) {
        std::memmove(buf.data(), buf.data() + offset, filled - offset);
        filled -= offset;
      }
      if (filled == buf.size()) filled = 0;  // desync guard
    }
  }

  std::string path_;
  vhp::FrameQueue& queue_;
  vhp::HotPathMetrics& metrics_;
  std::thread thread_;
  int listen_fd_ = -1;
  int conn_fd_ = -1;
};

// ------------------------------------------------------------- event publisher
void publish_loop(vhp::EventQueue& events, std::atomic<bool>& done) {
  vhp::ChunkEvent event{};
  bool dirty = false;
  while (!done.load(std::memory_order_acquire) || !events.empty()) {
    if (!events.try_pop(event)) {
      // Flush only when the queue actually drains. Flushing per event turns
      // every chunk into a write syscall, and at a few hundred concurrent calls
      // that is slow enough to back the event ring up and drop events -- which
      // is worse than a few milliseconds of output latency.
      if (dirty) { std::fflush(stdout); dirty = false; }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    static const char* kReasons[] = {"hangover", "max_length", "stream_end"};
    const char* reason = event.reason < 3 ? kReasons[event.reason] : "unknown";
    std::printf(
        "{\"session\":%u,\"chunk_id\":%llu,\"first_seq\":%llu,\"last_seq\":%llu,\"frames\":%u,"
        "\"bytes\":%u,\"dropped\":%u,\"reason\":\"%s\",\"speech_ms\":%.1f,"
        "\"endpoint_ns\":%llu}\n",
        event.session_id, static_cast<unsigned long long>(event.chunk_id),
        static_cast<unsigned long long>(event.first_seq),
        static_cast<unsigned long long>(event.last_seq), event.frame_count, event.byte_count,
        event.dropped_frames, reason,
        static_cast<double>(event.t_endpoint_ns - event.t_speech_start_ns) / 1e6,
        static_cast<unsigned long long>(event.t_endpoint_ns));
    dirty = true;
  }
  std::fflush(stdout);
}

// ---------------------------------------------------------------- the hot path
void hot_loop(const Options& opt, vhp::FrameQueue& frames, vhp::EventQueue& events,
              vhp::HotPathMetrics& metrics) {
  if (opt.core >= 0 && !vhp::pin_to_core(opt.core)) {
    std::fprintf(stderr, "[vhp] warning: could not pin to core %d\n", opt.core);
  }
  if (opt.realtime && !vhp::try_set_realtime_fifo(80)) {
    std::fprintf(stderr, "[vhp] warning: SCHED_FIFO denied, running SCHED_OTHER\n");
  }

  // Size the per-session assembled-audio buffer from the actual chunk cap
  // rather than a fixed 1 MB. At 32 sessions the difference is 32 MB vs 23 MB;
  // at 256 it is the difference between fitting in RAM and not.
  vhp::ChunkerConfig chunker = opt.chunker;
  chunker.max_chunk_bytes = static_cast<std::size_t>(
      opt.chunker.max_chunk_ms / 1000.0 * 24'000.0 * 2.0 * 1.1);
  vhp::SessionTable sessions(opt.max_sessions, chunker);
  std::fprintf(stderr, "[vhp] %u session slots x %zu KB chunk buffer = %zu MB resident\n",
               opt.max_sessions, chunker.max_chunk_bytes / 1024,
               static_cast<std::size_t>(opt.max_sessions) * chunker.max_chunk_bytes / (1024 * 1024));
  const std::uint64_t nominal_ns = static_cast<std::uint64_t>(opt.frame_ms * 1e6);
  const std::uint64_t idle_ns = static_cast<std::uint64_t>(opt.session_idle_ms * 1e6);
  const std::uint64_t deadline_ns =
      opt.seconds > 0.0 ? vhp::now_ns() + static_cast<std::uint64_t>(opt.seconds * 1e9) : 0;
  // Sweeping every frame would put an O(max_sessions) walk on the per-frame
  // path for something that only needs to happen on a human timescale.
  const std::uint64_t sweep_interval_ns = 250'000'000ULL;
  std::uint64_t next_sweep_ns = vhp::now_ns() + sweep_interval_ns;

  const auto emit = [&](const vhp::ChunkEvent& event) {
    ++metrics.chunks;
    if (events.try_push(event) == vhp::PushResult::kFull) ++metrics.event_drops;
  };

  vhp::AudioFrame frame{};
  std::uint64_t prev_observation_ns = 0;
  std::uint64_t observed_depth = 0;

  while (g_running.load(std::memory_order_relaxed)) {
    const std::uint64_t loop_ns = vhp::now_ns();
    // Integrate depth over time using the clock read the loop already takes.
    if (prev_observation_ns != 0) {
      const std::uint64_t dt = loop_ns - prev_observation_ns;
      metrics.depth_area_ns += observed_depth * dt;
      metrics.depth_time_ns += dt;
    }
    prev_observation_ns = loop_ns;
    if (deadline_ns && loop_ns > deadline_ns) break;
    if (loop_ns >= next_sweep_ns) {
      sessions.expire_idle(loop_ns, idle_ns, emit);
      next_sweep_ns = loop_ns + sweep_interval_ns;
    }
    if (!frames.try_pop(frame)) {
      observed_depth = 0;  // a failed pop means the ring was empty
      if (opt.spin) __builtin_ia32_pause();
      else std::this_thread::yield();
      continue;
    }
    const std::uint64_t t_dequeue = vhp::now_ns();

    // Queueing + transport. Guarded against a producer whose clock we do not
    // share; a misconfigured one would otherwise poison the histogram.
    if (frame.hdr.t_produce_ns != 0 && t_dequeue >= frame.hdr.t_produce_ns) {
      metrics.transport.record(t_dequeue - frame.hdr.t_produce_ns);
    }
    observed_depth = frames.size();
    metrics.depth.record(observed_depth);

    vhp::Session* session = sessions.acquire(frame.hdr.session_id, t_dequeue);
    if (session == nullptr) {
      // Admission control, not an error: the table is full and this call is
      // refused so the ones already in progress keep their latency budget.
      ++metrics.frames_refused;
      continue;
    }

    // Cadence is per session. With N calls interleaved on one ring the global
    // frame spacing is 20ms/N, so measuring it globally would report the
    // multiplexing as jitter.
    if (session->prev_frame_ns != 0) {
      const std::uint64_t delta = t_dequeue - session->prev_frame_ns;
      const std::uint64_t deviation = delta > nominal_ns ? delta - nominal_ns : nominal_ns - delta;
      // Coordinated-omission correction: a stall longer than the cadence means
      // frames that should have been processed never were, and their latency is
      // exactly what a naive loop fails to record.
      metrics.jitter.record_with_expected_interval(deviation, nominal_ns);
    }
    session->prev_frame_ns = t_dequeue;

    if (session->have_last_seq && frame.hdr.seq > session->last_seq + 1) {
      metrics.seq_gaps += frame.hdr.seq - session->last_seq - 1;
    }
    session->last_seq = frame.hdr.seq;
    session->have_last_seq = true;
    ++session->frames;

    auto maybe_event = session->assembler.on_frame(frame, t_dequeue);

    // A frame the gate accepted as loud: remember it, so endpoint lag is
    // measured from real speech end rather than from chunk close.
    if (session->assembler.gate().speaking() &&
        session->assembler.gate().quiet_frames() == 0) {
      session->last_loud_produce_ns = frame.hdr.t_produce_ns;
    }

    metrics.service.record(vhp::now_ns() - t_dequeue);
    ++metrics.frames;

    if (maybe_event) {
      maybe_event->session_id = session->id;
      if (session->last_loud_produce_ns &&
          maybe_event->t_endpoint_ns > session->last_loud_produce_ns) {
        metrics.endpoint_lag.record(maybe_event->t_endpoint_ns - session->last_loud_produce_ns);
      }
      emit(*maybe_event);
    }
  }

  sessions.flush_all(vhp::now_ns(), emit);
  metrics.sessions_peak = sessions.peak_live();
  metrics.sessions_opened = sessions.opened();
  metrics.sessions_rejected = sessions.rejected();
  metrics.sessions_capacity = sessions.capacity();
}

void dump_csv(const Options& opt, const vhp::HotPathMetrics& m) {
  if (opt.csv_prefix.empty()) return;
  const std::pair<const char*, const vhp::Histogram*> series[] = {
      {"transport", &m.transport}, {"service", &m.service}, {"jitter", &m.jitter},
      {"depth", &m.depth}, {"endpoint_lag", &m.endpoint_lag}};
  for (const auto& [name, hist] : series) {
    const std::string path = opt.csv_prefix + "-" + name + ".csv";
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) { std::perror(path.c_str()); continue; }
    const std::string csv = hist->to_csv();
    std::fwrite(csv.data(), 1, csv.size(), f);
    std::fclose(f);
    std::fprintf(stderr, "[vhp] wrote %s\n", path.c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse_args(argc, argv, opt)) return 2;

  ::signal(SIGINT, on_signal);
  ::signal(SIGTERM, on_signal);
  ::signal(SIGPIPE, SIG_IGN);

  vhp::HotPathMetrics metrics;
  auto events = std::make_unique<vhp::EventQueue>();
  std::atomic<bool> hot_done{false};
  std::thread publisher(publish_loop, std::ref(*events), std::ref(hot_done));

  try {
    if (!opt.uds_path.empty()) {
      auto queue = std::make_unique<vhp::FrameQueue>();
      UdsIngest ingest(opt.uds_path, *queue, metrics);
      if (!ingest.start()) { g_running = false; }
      hot_loop(opt, *queue, *events, metrics);
      g_running = false;
      ingest.stop();
    } else {
      auto frames = vhp::FrameRing::create(opt.frame_shm);
      std::fprintf(stderr, "[vhp] created %s (%zu slots, %zu B/slot)\n", opt.frame_shm.c_str(),
                   vhp::kFrameRingCapacity, sizeof(vhp::AudioFrame));
      hot_loop(opt, frames.ring(), *events, metrics);
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[vhp] fatal: %s\n", e.what());
    hot_done.store(true, std::memory_order_release);
    publisher.join();
    return 1;
  }

  hot_done.store(true, std::memory_order_release);
  publisher.join();

  std::fputs(metrics.report(opt.frame_ms, opt.uds_path.empty() ? "shm" : "uds").c_str(),
             stderr);
  dump_csv(opt, metrics);
  return 0;
}
