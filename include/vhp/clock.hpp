// Monotonic timing and thread placement.
//
// CLOCK_MONOTONIC via clock_gettime is vDSO-backed on Linux/x86-64: no syscall,
// ~20ns. That is cheap enough to stamp every 20ms audio frame and still be
// invisible in the measurement. rdtscp is faster but needs calibration and a
// pinned, constant-TSC core to be meaningful; the accuracy is not the binding
// constraint at millisecond scale, so we do not use it.
#pragma once

#include <pthread.h>
#include <sched.h>
#include <time.h>

#include <cstdint>
#include <string>

namespace vhp {

inline std::uint64_t now_ns() noexcept {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

// Sleep until an absolute CLOCK_MONOTONIC deadline. Absolute deadlines are the
// point: sleeping for a *duration* accumulates drift, and drift shows up as a
// slow bias in an inter-arrival measurement that is easy to mistake for jitter.
inline void sleep_until_ns(std::uint64_t deadline_ns) noexcept {
  timespec ts{};
  ts.tv_sec = static_cast<time_t>(deadline_ns / 1'000'000'000ULL);
  ts.tv_nsec = static_cast<long>(deadline_ns % 1'000'000'000ULL);
  while (::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
  }
}

// Busy-wait to an absolute deadline. Burns a core; only for the pacer when we
// want the producer's own scheduling noise kept out of the measurement.
inline void spin_until_ns(std::uint64_t deadline_ns) noexcept {
  while (now_ns() < deadline_ns) {
    __builtin_ia32_pause();
  }
}

// Pin the calling thread to one core. Pinning removes migration jitter; it does
// NOT remove timer ticks, softirqs or other tenants on that core. For that you
// also need isolcpus/nohz_full at boot, which we do not assume here -- see the
// caveat printed by the benchmarks.
inline bool pin_to_core(int core) noexcept {
  cpu_set_t set;
  CPU_ZERO(&set);
  // glibc's CPU_SET expands to arithmetic that trips -Wsign-conversion. The
  // macro is correct; the warning is about its internals, not our call.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
  CPU_SET(static_cast<unsigned>(core), &set);
#pragma GCC diagnostic pop
  return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0;
}

inline bool try_set_realtime_fifo(int priority) noexcept {
  sched_param param{};
  param.sched_priority = priority;
  return ::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &param) == 0;
}

}  // namespace vhp
