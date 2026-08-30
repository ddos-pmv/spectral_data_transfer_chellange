// Standalone assertion-based tests for the metrics accumulator and the shm ring.
// No test framework -- just asserts, so this stays dependency-free and builds
// with a single g++ invocation.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "message.h"
#include "metrics.h"
#include "shm_ring.h"

static void test_metrics_basic() {
  metrics::Accumulator acc;
  // seq 1..100 all delivered, latency == seq nanoseconds.
  // Segments are split 1:2:3 of the end-to-end value so each distribution is
  // distinguishable and a mixed-up field shows as a failed assert.
  for (uint64_t i = 1; i <= 100; ++i) {
    acc.record(i, /*send_ts_ns=*/1000 + i, static_cast<int64_t>(6 * i),
               static_cast<int64_t>(i), static_cast<int64_t>(2 * i),
               static_cast<int64_t>(3 * i));
  }
  metrics::Report r = acc.report();

  assert(r.received == 100);
  assert(r.expected == 100);
  assert(r.dropped == 0);
  assert(r.drop_rate == 0.0);
  assert(r.e2e.min == 6);
  assert(r.e2e.max == 600);
  // Nearest-rank: p50 of 1..100 -> rank ceil(0.5*100)=50 -> value 50.
  assert(r.e2e.p50 == 300);
  assert(r.e2e.p99 == 594);
  // Fewer than 1e6 samples, so p99.999 can only land on the maximum.
  assert(r.e2e.p99999 == 600);
  assert(r.a.p50 == 50);
  assert(r.b.p50 == 100);
  assert(r.c.p50 == 150);
  assert(r.a.mean > 50.0 && r.a.mean < 51.0);
  printf("test_metrics_basic OK\n");
}

static void test_metrics_drops() {
  metrics::Accumulator acc;
  // Deliver only even sequence ids 2,4,...,100 -> 50 received, 100 expected.
  for (uint64_t i = 2; i <= 100; i += 2) acc.record(i, 1000 + i, 10, 1, 2, 3);
  metrics::Report r = acc.report();

  assert(r.received == 50);
  assert(r.expected == 99);  // last(100) - first(2) + 1
  assert(r.dropped == 49);
  assert(r.drop_rate > 0.49 && r.drop_rate < 0.50);
  printf("test_metrics_drops OK\n");
}

static void test_metrics_csv() {
  metrics::Accumulator acc(4);
  // Negative b: the wire segment crosses hosts and legitimately comes out
  // negative when the receiving clock runs behind. It must survive the CSV.
  acc.record(7, /*send_ts_ns=*/1007, 111, 1, -2, 3);
  acc.record(8, /*send_ts_ns=*/1008, 222, 4, 5, 6);

  const char* path = "/tmp/harness_metrics_test.csv";
  assert(acc.write_csv(path));

  FILE* f = std::fopen(path, "r");
  assert(f != nullptr);
  char header[80] = {0};
  unsigned long long seq = 0, ts = 0;
  long long lat = 0, a = 0, b = 0, c = 0;
  assert(std::fgets(header, sizeof(header), f) != nullptr);
  // send_ts_ns is what lets an externally measured clock offset be joined to a
  // sample, so its presence in the CSV is part of the contract, not a detail.
  assert(std::strcmp(header, "seq,send_ts_ns,latency_ns,a_ns,b_ns,c_ns\n") == 0);
  assert(std::fscanf(f, "%llu,%llu,%lld,%lld,%lld,%lld", &seq, &ts, &lat, &a, &b, &c) == 6);
  assert(seq == 7 && ts == 1007 && lat == 111 && a == 1 && b == -2 && c == 3);
  assert(std::fscanf(f, "%llu,%llu,%lld,%lld,%lld,%lld", &seq, &ts, &lat, &a, &b, &c) == 6);
  assert(seq == 8 && ts == 1008 && lat == 222 && a == 4 && b == 5 && c == 6);
  std::fclose(f);
  std::remove(path);
  printf("test_metrics_csv OK\n");
}

static void test_ring_roundtrip() {
  const uint32_t slots = 8;
  std::vector<uint8_t> mem(shm::region_size(slots));
  shm::Ring prod;
  prod.attach(mem.data(), slots, /*init=*/true);
  shm::Ring cons;
  cons.attach(mem.data(), slots, /*init=*/false);

  // Publish 5 frames (fits in the ring, no lapping).
  for (uint32_t i = 0; i < 5; ++i) {
    uint8_t frame[16];
    std::memset(frame, static_cast<int>(i), sizeof(frame));
    prod.publish(frame, sizeof(frame));
  }

  uint64_t read_index = 0;
  for (uint32_t i = 0; i < 5; ++i) {
    uint8_t out[64];
    uint32_t len = 0;
    uint64_t resume = 0;
    auto st = cons.read(read_index, out, &len, &resume);
    assert(st == shm::Ring::FrameStatus::kOk);
    assert(len == 16);
    assert(out[0] == static_cast<uint8_t>(i));
    ++read_index;
  }
  // Next read is empty (nothing published yet).
  uint8_t out[64];
  uint32_t len = 0;
  uint64_t resume = 0;
  assert(cons.read(read_index, out, &len, &resume) ==
         shm::Ring::FrameStatus::kEmpty);
  printf("test_ring_roundtrip OK\n");
}

static void test_ring_lapping() {
  const uint32_t slots = 4;
  std::vector<uint8_t> mem(shm::region_size(slots));
  shm::Ring prod;
  prod.attach(mem.data(), slots, /*init=*/true);
  shm::Ring cons;
  cons.attach(mem.data(), slots, /*init=*/false);

  // Publish 10 frames into a 4-slot ring -> reader sitting at index 0 is lapped.
  for (uint32_t i = 0; i < 10; ++i) {
    uint8_t frame[8];
    std::memset(frame, static_cast<int>(i), sizeof(frame));
    prod.publish(frame, sizeof(frame));
  }

  uint8_t out[64];
  uint32_t len = 0;
  uint64_t resume = 0;
  auto st = cons.read(0, out, &len, &resume);
  assert(st == shm::Ring::FrameStatus::kLapped);
  // Producer wrote 10, ring holds 4 -> safe resume position is 10 - 4 = 6.
  assert(resume == 6);

  // Reading from the resume point yields the frame published at index 6.
  st = cons.read(resume, out, &len, &resume);
  assert(st == shm::Ring::FrameStatus::kOk);
  assert(out[0] == 6);
  printf("test_ring_lapping OK\n");
}

// Batching: the sender packs several frames into one datagram, and the receiver
// walks them apart using frame_size(type) out of each frame's own header. There
// is no length prefix on the wire, so the two walks are the whole contract --
// and a disagreement between them silently corrupts every frame after the first.
//
// The frame layout is exercised with all three message types interleaved at
// random, because the walk only works if a frame's length really is implied by
// its type: a single wrong size desynchronises the rest of the datagram.
void test_udp_batching() {
  constexpr uint16_t kFrameCap = msg::kMaxFrame;
  constexpr uint16_t kMaxPack = 8;
  std::mt19937 rng(12345);

  uint64_t next_seq = 0, packed = 0, unpacked = 0, dgrams = 0;

  for (int trial = 0; trial < 20000; ++trial) {
    const uint16_t pack = 1 + rng() % kMaxPack;
    const uint16_t frames = 1 + rng() % pack;

    uint8_t buf[kMaxPack * kFrameCap];
    uint16_t used = 0;
    std::vector<uint64_t> sent;
    for (uint16_t k = 0; k < frames; ++k) {
      const uint8_t types[3] = {1, 2, 3};
      const uint8_t t = types[rng() % 3];
      const uint32_t fsz = msg::frame_size(t);
      assert(fsz != 0);
      assert(used + fsz <= sizeof(buf));
      memset(buf + used, 0, fsz);
      const uint64_t seq = ++next_seq;
      memcpy(buf + used + offsetof(msg::Header, seq_id), &seq, sizeof(seq));
      buf[used + offsetof(msg::Header, type)] = t;
      sent.push_back(seq);
      used = static_cast<uint16_t>(used + fsz);
      ++packed;
    }
    ++dgrams;

    // Sender's stamping walk: every frame gets tx_ts, not just the first.
    const uint64_t tx_ts = 0xA1B2C3D4E5F60718ull;
    uint16_t stamped = 0;
    for (uint16_t off = 0; off + sizeof(msg::Header) <= used;) {
      memcpy(buf + off + offsetof(msg::Header, tx_ts_ns), &tx_ts, sizeof(tx_ts));
      const uint32_t fsz = msg::frame_size(buf[off + offsetof(msg::Header, type)]);
      assert(fsz != 0);
      off = static_cast<uint16_t>(off + fsz);
      ++stamped;
    }
    assert(stamped == frames);

    // Receiver's walk: same boundaries, same order, same payloads.
    std::vector<uint64_t> got;
    for (uint16_t off = 0; off + sizeof(msg::Header) <= used;) {
      const uint32_t fsz = msg::frame_size(buf[off + offsetof(msg::Header, type)]);
      assert(fsz != 0 && off + fsz <= used);
      uint64_t seq = 0, ts = 0;
      memcpy(&seq, buf + off + offsetof(msg::Header, seq_id), sizeof(seq));
      memcpy(&ts, buf + off + offsetof(msg::Header, tx_ts_ns), sizeof(ts));
      assert(ts == tx_ts);
      got.push_back(seq);
      off = static_cast<uint16_t>(off + fsz);
      ++unpacked;
    }
    assert(got == sent);
  }

  assert(packed == unpacked);
  // An unknown type must report 0 rather than a plausible size: both walks stop
  // on it, and a guessed length would desynchronise the rest of the datagram.
  assert(msg::frame_size(0) == 0);
  assert(msg::frame_size(99) == 0);
  printf("test_udp_batching OK (%llu frames in %llu datagrams, %.2f per datagram)\n",
         (unsigned long long)packed, (unsigned long long)dgrams,
         (double)packed / (double)dgrams);
}

int main() {
  test_metrics_basic();
  test_metrics_drops();
  test_metrics_csv();
  test_ring_roundtrip();
  test_ring_lapping();
  test_udp_batching();
  printf("ALL TESTS PASSED\n");
  return 0;
}
