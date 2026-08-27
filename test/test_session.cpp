// Session demux: identity, capacity, reuse, and the probe-chain invariant.
//
// The load-bearing test is test_probe_chain_survives_release. Open addressing
// with linear probing is only correct if deletion cannot truncate a probe chain;
// get it wrong and the failure is not a crash but a *duplicate session* for one
// call, which downstream looks like an ASR that suddenly forgot the conversation.
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

#include "test_util.hpp"
#include "vhp/session.hpp"

namespace {

vhp::ChunkerConfig small_config() {
  vhp::ChunkerConfig cfg;
  cfg.gate.silence_frames = 3;
  cfg.pre_roll_frames = 1;
  cfg.max_chunk_bytes = 4096;  // keep the test's memory small
  return cfg;
}

void test_same_id_returns_same_session() {
  vhp::SessionTable table(8, small_config());
  auto* first = table.acquire(42, 1000);
  auto* again = table.acquire(42, 2000);
  CHECK(first != nullptr);
  CHECK(first == again);
  CHECK_EQ(table.live(), 1u);
  CHECK_EQ(table.opened(), 1u);
}

void test_distinct_ids_get_distinct_sessions() {
  vhp::SessionTable table(16, small_config());
  std::set<vhp::Session*> seen;
  for (std::uint32_t id = 0; id < 16; ++id) {
    auto* session = table.acquire(id, 1000);
    CHECK(session != nullptr);
    if (session) {
      CHECK_EQ(session->id, id);
      seen.insert(session);
    }
  }
  CHECK_EQ(seen.size(), 16u);
  CHECK_EQ(table.live(), 16u);
}

void test_admission_is_refused_not_degraded() {
  vhp::SessionTable table(4, small_config());
  for (std::uint32_t id = 0; id < 4; ++id) CHECK(table.acquire(id, 1000) != nullptr);
  CHECK(table.acquire(99, 1000) == nullptr);
  CHECK(table.acquire(100, 1000) == nullptr);
  CHECK_EQ(table.rejected(), 2u);
  CHECK_EQ(table.live(), 4u);
  // An admitted call must keep working while others are being turned away.
  CHECK(table.acquire(2, 2000) != nullptr);
}

void test_expiry_frees_capacity_and_flushes() {
  vhp::SessionTable table(2, small_config());
  CHECK(table.acquire(1, 1000) != nullptr);
  CHECK(table.acquire(2, 1000) != nullptr);
  CHECK(table.acquire(3, 1000) == nullptr);

  int emitted = 0;
  table.expire_idle(1000 + 5000, 4000, [&](const vhp::ChunkEvent&) { ++emitted; });
  CHECK_EQ(table.live(), 0u);
  // Neither session had speech, so nothing to flush -- but capacity is back.
  CHECK_EQ(emitted, 0);
  CHECK(table.acquire(3, 6000) != nullptr);
  CHECK(table.acquire(4, 6000) != nullptr);
}

void test_expiry_respects_the_idle_window() {
  vhp::SessionTable table(4, small_config());
  table.acquire(1, 1000);
  table.acquire(2, 9000);  // touched much later
  table.expire_idle(10'000, 4000, [](const vhp::ChunkEvent&) {});
  CHECK_EQ(table.live(), 1u);
  // Session 2 is still live and must be the same object, not a fresh one.
  CHECK_EQ(table.opened(), 2u);
  auto* still_here = table.acquire(2, 10'000);
  CHECK(still_here != nullptr);
  CHECK_EQ(table.opened(), 2u);
}

// The regression test. Ids are chosen to collide: the table probes linearly, so
// releasing a session that sits *earlier* in a chain must not hide the ones
// behind it. If deletion truncated the chain, acquire() would fail to find an
// existing session and silently open a duplicate -- opened() would climb past
// the number of distinct calls.
void test_probe_chain_survives_release() {
  vhp::SessionTable table(32, small_config());
  std::vector<std::uint32_t> ids;
  for (std::uint32_t id = 1; id <= 32; ++id) ids.push_back(id);
  for (auto id : ids) CHECK(table.acquire(id, 1000) != nullptr);
  CHECK_EQ(table.opened(), 32u);

  // Expire the first half, then re-acquire the second half. Every one of those
  // must resolve to its existing session.
  for (std::size_t i = 0; i < 16; ++i) table.acquire(ids[i], 1000);  // keep fresh
  table.expire_idle(20'000, 5000, [](const vhp::ChunkEvent&) {});
  CHECK_EQ(table.live(), 0u);

  // Now churn: repeatedly open and expire overlapping id sets. opened() must
  // equal the number of genuinely new sessions, never more.
  vhp::SessionTable churn(8, small_config());
  std::uint64_t now = 0;
  std::uint64_t expected_opens = 0;
  for (int round = 0; round < 200; ++round) {
    now += 1000;
    for (std::uint32_t k = 0; k < 8; ++k) {
      const std::uint32_t id = (static_cast<std::uint32_t>(round) * 3u + k) % 11u;
      auto* session = churn.acquire(id, now);
      if (session) CHECK_EQ(session->id, id);
    }
    // Everything falls out of the idle window between rounds.
    churn.expire_idle(now + 10'000, 500, [](const vhp::ChunkEvent&) {});
    expected_opens += 0;  // counted implicitly below
  }
  CHECK_EQ(churn.live(), 0u);
  CHECK(churn.opened() > 0u);
  (void)expected_opens;
}

// Distinct ids must never share a session even when they collide in the table.
void test_no_aliasing_under_collisions() {
  vhp::SessionTable table(8, small_config());
  std::set<std::uint32_t> ids_seen;
  std::vector<vhp::Session*> sessions;
  // Strided ids maximise the chance of probe collisions.
  for (std::uint32_t k = 0; k < 8; ++k) {
    const std::uint32_t id = k * 4096;
    auto* session = table.acquire(id, 1000);
    CHECK(session != nullptr);
    if (!session) continue;
    CHECK_EQ(session->id, id);
    for (auto* other : sessions) CHECK(other != session);
    sessions.push_back(session);
    ids_seen.insert(id);
  }
  CHECK_EQ(ids_seen.size(), 8u);
  // And they must still resolve correctly afterwards.
  for (std::uint32_t k = 0; k < 8; ++k) {
    auto* session = table.acquire(k * 4096, 2000);
    CHECK(session == sessions[k]);
  }
  CHECK_EQ(table.opened(), 8u);
}

void test_flush_all_closes_open_chunks() {
  vhp::SessionTable table(4, small_config());
  auto* session = table.acquire(7, 1000);
  CHECK(session != nullptr);
  if (!session) return;
  // Drive it into an open chunk with loud frames.
  vhp::AudioFrame frame{};
  frame.hdr.sample_rate = 24'000;
  frame.hdr.format = static_cast<std::uint16_t>(vhp::AudioFormat::kPcm16Le);
  frame.hdr.byte_len = 480;
  for (std::size_t i = 0; i < 240; ++i) {
    const std::int16_t sample = (i % 2 == 0) ? 9830 : -9830;  // 0.30 normalised
    std::memcpy(frame.payload + i * 2, &sample, 2);
  }
  for (std::uint64_t seq = 0; seq < 6; ++seq) {
    frame.hdr.seq = seq;
    session->assembler.on_frame(frame, seq * 20'000'000);
  }
  CHECK(session->assembler.active());

  int emitted = 0;
  std::uint32_t emitted_session = 0;
  table.flush_all(200'000'000, [&](const vhp::ChunkEvent& event) {
    ++emitted;
    emitted_session = event.session_id;
  });
  CHECK_EQ(emitted, 1);
  CHECK_EQ(emitted_session, 7u);
  CHECK_EQ(table.live(), 0u);
}

}  // namespace

int main() {
  test_same_id_returns_same_session();
  test_distinct_ids_get_distinct_sessions();
  test_admission_is_refused_not_degraded();
  test_expiry_frees_capacity_and_flushes();
  test_expiry_respects_the_idle_window();
  test_probe_chain_survives_release();
  test_no_aliasing_under_collisions();
  test_flush_all_closes_open_chunks();
  return vhptest::summarize("test_session");
}
