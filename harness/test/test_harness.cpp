// Standalone assertion-based tests for the metrics accumulator and the shm ring.
// No test framework -- just asserts, so this stays dependency-free and builds
// with a single g++ invocation.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "metrics.h"
#include "shm_ring.h"

static void test_metrics_basic() {
  metrics::Accumulator acc;
  // seq 1..100 all delivered, latency == seq nanoseconds.
  // Segments are split 1:2:3 of the end-to-end value so each distribution is
  // distinguishable and a mixed-up field shows as a failed assert.
  for (uint64_t i = 1; i <= 100; ++i) {
    acc.record(i, static_cast<int64_t>(6 * i), static_cast<int64_t>(i),
               static_cast<int64_t>(2 * i), static_cast<int64_t>(3 * i));
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
  for (uint64_t i = 2; i <= 100; i += 2) acc.record(i, 10, 1, 2, 3);
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
  acc.record(7, 111, 1, -2, 3);
  acc.record(8, 222, 4, 5, 6);

  const char* path = "/tmp/harness_metrics_test.csv";
  assert(acc.write_csv(path));

  FILE* f = std::fopen(path, "r");
  assert(f != nullptr);
  char header[64] = {0};
  unsigned long long seq = 0;
  long long lat = 0, a = 0, b = 0, c = 0;
  assert(std::fgets(header, sizeof(header), f) != nullptr);
  assert(std::strcmp(header, "seq,latency_ns,a_ns,b_ns,c_ns\n") == 0);
  assert(std::fscanf(f, "%llu,%lld,%lld,%lld,%lld", &seq, &lat, &a, &b, &c) == 5);
  assert(seq == 7 && lat == 111 && a == 1 && b == -2 && c == 3);
  assert(std::fscanf(f, "%llu,%lld,%lld,%lld,%lld", &seq, &lat, &a, &b, &c) == 5);
  assert(seq == 8 && lat == 222 && a == 4 && b == 5 && c == 6);
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

int main() {
  test_metrics_basic();
  test_metrics_drops();
  test_metrics_csv();
  test_ring_roundtrip();
  test_ring_lapping();
  printf("ALL TESTS PASSED\n");
  return 0;
}
