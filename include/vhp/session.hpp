// Multi-call session demultiplexing.
//
// One hot-path thread serves every concurrent call. Frames from all sessions
// arrive interleaved on a single ring and are demultiplexed here by session_id.
//
// Three things drive the design:
//
//   Everything is preallocated. Every session's ChunkAssembler -- including its
//   assembled-audio buffer -- is constructed at startup. A call connecting mid
//   -flight must not allocate, because that allocation would land on the pinned
//   thread and show up in the tail of every *other* call in progress.
//
//   Lookup is open-addressed with linear probing over a power-of-two table. No
//   hashing beyond a multiply, no node chasing, no rehash. At the session counts
//   this serves (tens), a probe almost always hits on the first slot.
//
//   Admission is explicit. When the table is full, new sessions are refused and
//   counted. The alternative -- growing without bound -- degrades every call in
//   progress to serve one more, which is the wrong trade for a real-time path.
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "vhp/vad.hpp"

namespace vhp {

struct Session {
  explicit Session(const ChunkerConfig& config) : assembler(config) {}

  ChunkAssembler assembler;
  std::uint32_t id = 0;
  bool active = false;
  // Per-session, not global: with N calls interleaved on one ring the global
  // frame spacing is 20ms/N, so a single global cadence measurement would
  // report a "jitter" that is really just the multiplexing.
  std::uint64_t prev_frame_ns = 0;
  std::uint64_t last_frame_ns = 0;
  std::uint64_t last_loud_produce_ns = 0;
  std::uint64_t last_seq = 0;
  std::uint64_t frames = 0;
  bool have_last_seq = false;
};

class SessionTable {
 public:
  SessionTable(std::uint32_t max_sessions, const ChunkerConfig& config)
      : max_sessions_(max_sessions == 0 ? 1 : max_sessions),
        slot_mask_(next_pow2(std::size_t{max_sessions_} * 2) - 1),
        slots_(slot_mask_ + 1, kEmpty) {
    sessions_.reserve(max_sessions_);
    free_list_.reserve(max_sessions_);
    for (std::uint32_t i = 0; i < max_sessions_; ++i) {
      sessions_.push_back(std::make_unique<Session>(config));
    }
    // Reverse, so pop_back() hands out low indices first and a fresh table
    // allocates sessions in a predictable order -- easier to read in a dump.
    for (std::uint32_t i = max_sessions_; i > 0; --i) free_list_.push_back(i - 1);
  }

  // Returns the session for `id`, opening one if needed, or nullptr when the
  // table is full -- which the caller must treat as a rejected call, not as an
  // error to retry.
  //
  // Deletion writes a tombstone rather than clearing the slot. Clearing would
  // truncate the probe chain of any id that hashes earlier and collides here,
  // so the next lookup for that id would stop short, report "not found", and
  // open a *second* session for a call that already had one. Downstream that
  // reads as an ASR stream that suddenly forgot the conversation.
  //
  // Session storage is a separate free list rather than being bound to a slot.
  // Binding sessions to slots also fixes the probe chain, but it strands
  // capacity: a freed session reachable only from a distant slot cannot be
  // reused by an id whose chain never passes through it, and the table reports
  // "full" while holding idle sessions.
  Session* acquire(std::uint32_t id, std::uint64_t now_ns) noexcept {
    std::size_t slot = probe_start(id);
    std::size_t first_tomb = kNoSlot;
    std::size_t insert_at = kNoSlot;

    for (std::size_t probes = 0; probes <= slot_mask_; ++probes) {
      const std::uint32_t occupant = slots_[slot];
      if (occupant == kEmpty) {
        insert_at = first_tomb != kNoSlot ? first_tomb : slot;
        break;
      }
      if (occupant == kTombstone) {
        if (first_tomb == kNoSlot) first_tomb = slot;
      } else {
        Session& candidate = *sessions_[occupant];
        if (candidate.active && candidate.id == id) {
          candidate.last_frame_ns = now_ns;
          return &candidate;
        }
      }
      slot = (slot + 1) & slot_mask_;
    }
    // Probed every slot without hitting an empty one: a tombstone is the only
    // place left to insert.
    if (insert_at == kNoSlot) insert_at = first_tomb;
    if (insert_at == kNoSlot || free_list_.empty()) {
      ++rejected_;
      return nullptr;
    }

    if (slots_[insert_at] == kTombstone) --tombstones_;
    const std::uint32_t index = free_list_.back();
    free_list_.pop_back();
    slots_[insert_at] = index;

    Session& session = *sessions_[index];
    session.id = id;
    session.active = true;
    session.prev_frame_ns = 0;
    session.last_frame_ns = now_ns;
    session.last_loud_produce_ns = 0;
    session.have_last_seq = false;
    session.frames = 0;
    ++live_;
    ++opened_;
    if (live_ > peak_live_) peak_live_ = live_;
    return &session;
  }

  // Close sessions that have gone quiet. A call that hangs up stops sending
  // frames but never says so over this transport, so without a sweep the slot
  // is held forever and the table fills with ghosts. Any chunk still open is
  // flushed through `emit` rather than dropped.
  template <typename EmitFn>
  void expire_idle(std::uint64_t now_ns, std::uint64_t idle_ns, EmitFn&& emit) {
    for (std::uint32_t i = 0; i < max_sessions_; ++i) {
      Session& session = *sessions_[i];
      if (!session.active || now_ns - session.last_frame_ns < idle_ns) continue;
      if (auto tail = session.assembler.flush(now_ns)) {
        tail->session_id = session.id;
        emit(*tail);
      }
      release(i);
    }
    maybe_rebuild();
  }

  template <typename EmitFn>
  void flush_all(std::uint64_t now_ns, EmitFn&& emit) {
    for (std::uint32_t i = 0; i < max_sessions_; ++i) {
      Session& session = *sessions_[i];
      if (!session.active) continue;
      if (auto tail = session.assembler.flush(now_ns)) {
        tail->session_id = session.id;
        emit(*tail);
      }
      release(i);
    }
    maybe_rebuild();
  }

  std::uint32_t live() const noexcept { return live_; }
  std::uint32_t peak_live() const noexcept { return peak_live_; }
  std::uint64_t opened() const noexcept { return opened_; }
  std::uint64_t rejected() const noexcept { return rejected_; }
  std::uint32_t capacity() const noexcept { return max_sessions_; }

 private:
  static constexpr std::uint32_t kEmpty = 0xFFFFFFFFu;
  static constexpr std::uint32_t kTombstone = 0xFFFFFFFEu;
  static constexpr std::size_t kNoSlot = static_cast<std::size_t>(-1);

  static std::size_t next_pow2(std::size_t value) {
    std::size_t result = 1;
    while (result < value) result <<= 1;
    return result;
  }

  // Fibonacci hashing: one multiply, and it spreads sequential ids -- which is
  // exactly what session ids are -- instead of clustering them.
  std::size_t probe_start(std::uint32_t id) const noexcept {
    return static_cast<std::size_t>(
               (static_cast<std::uint64_t>(id) * 0x9E3779B97F4A7C15ULL) >> 32) &
           slot_mask_;
  }

  void release(std::uint32_t index) noexcept {
    Session& session = *sessions_[index];
    std::size_t slot = probe_start(session.id);
    for (std::size_t probes = 0; probes <= slot_mask_; ++probes) {
      if (slots_[slot] == index) {
        slots_[slot] = kTombstone;
        ++tombstones_;
        break;
      }
      if (slots_[slot] == kEmpty) break;  // unreachable; defensive
      slot = (slot + 1) & slot_mask_;
    }
    session.active = false;
    free_list_.push_back(index);
    if (live_ > 0) --live_;
  }

  // Tombstones lengthen every probe. At these table sizes a full rebuild is a
  // few dozen stores, so it is cheaper to rebuild than to reason about drift.
  // It runs from expire_idle/flush_all, never from the per-frame path.
  void maybe_rebuild() noexcept {
    if (tombstones_ * 4 <= slots_.size()) return;
    std::fill(slots_.begin(), slots_.end(), kEmpty);
    tombstones_ = 0;
    for (std::uint32_t i = 0; i < max_sessions_; ++i) {
      if (!sessions_[i]->active) continue;
      std::size_t slot = probe_start(sessions_[i]->id);
      while (slots_[slot] != kEmpty) slot = (slot + 1) & slot_mask_;
      slots_[slot] = i;
    }
  }

  std::uint32_t max_sessions_;
  std::size_t slot_mask_;
  std::vector<std::uint32_t> slots_;
  std::vector<std::unique_ptr<Session>> sessions_;
  std::vector<std::uint32_t> free_list_;
  std::size_t tombstones_ = 0;
  std::uint32_t live_ = 0;
  std::uint32_t peak_live_ = 0;
  std::uint64_t opened_ = 0;
  std::uint64_t rejected_ = 0;
};

}  // namespace vhp
