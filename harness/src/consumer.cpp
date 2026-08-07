// Consumer: reads events from the shared-memory broadcast ring, stamps a receive
// timestamp, and computes delivery metrics (latency percentiles, drop rate) from
// the per-message send timestamp and sequence id. Fixed measurement end of the
// harness.
//
// Usage: consumer [--shm NAME] [--slots N] [--count N] [--warmup N]
//                 [--from-edge] [--csv FILE] [--idle-ms MS]
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "message.h"
#include "metrics.h"
#include "shm_ring.h"
#include "shm_segment.h"
#include "util.h"

namespace {

struct Config {
  std::string shm_name = "/fanout_ring";
  uint32_t slots = 1024;
  uint64_t count = 0;
  uint64_t warmup = 0;
  bool from_edge = false;
  std::string csv;
  uint64_t idle_ms = 2000;
};

Config parse_args(int argc, char** argv) {
  Config c;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value for %s\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--shm") c.shm_name = next();
    else if (a == "--slots") c.slots = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--count") c.count = std::stoull(next());
    else if (a == "--warmup") c.warmup = std::stoull(next());
    else if (a == "--from-edge") c.from_edge = true;
    else if (a == "--csv") c.csv = next();
    else if (a == "--idle-ms") c.idle_ms = std::stoull(next());
    else {
      fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }
  return c;
}

void print_report(const metrics::Report& r) {
  printf("---- delivery metrics ----\n");
  printf("received     : %llu\n", (unsigned long long)r.received);
  printf("expected     : %llu\n", (unsigned long long)r.expected);
  printf("dropped      : %llu\n", (unsigned long long)r.dropped);
  printf("drop_rate    : %.4f%%\n", r.drop_rate * 100.0);
  printf("latency (ns) : min=%llu mean=%.0f max=%llu\n",
         (unsigned long long)r.lat_min, r.lat_mean,
         (unsigned long long)r.lat_max);
  printf("  p01        : %llu\n", (unsigned long long)r.p01);
  printf("  p50        : %llu\n", (unsigned long long)r.p50);
  printf("  p99        : %llu\n", (unsigned long long)r.p99);
  printf("  p99.9      : %llu\n", (unsigned long long)r.p999);
  printf("  p99.99     : %llu\n", (unsigned long long)r.p9999);
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg = parse_args(argc, argv);

  shm::Segment seg =
      shm::Segment::open(cfg.shm_name, shm::region_size(cfg.slots), /*create=*/false);
  shm::Ring ring;
  ring.attach(seg.base(), cfg.slots, /*init=*/false);

  // Sized for exactly what the run will record, so no reallocation lands in the
  // middle of the measurement.
  metrics::Accumulator acc(cfg.count ? cfg.count : 1u << 20);

  uint64_t read_index = cfg.from_edge ? ring.live_edge() : 0;
  uint64_t consumed = 0;  // frames taken off the ring, warm-up included
  uint64_t received = 0;  // frames actually recorded
  uint64_t lapped_events = 0;
  const uint64_t idle_ns = cfg.idle_ms * 1000000ull;
  uint64_t last_progress = util::now_ns();

  uint8_t frame[shm::kFrameCap];
  while (cfg.count == 0 || received < cfg.count) {
    uint32_t len = 0;
    uint64_t resume = 0;
    auto st = ring.read(read_index, frame, &len, &resume);

    if (st == shm::Ring::FrameStatus::kOk) {
      const uint64_t recv_ts = util::now_ns();
      const auto* hdr = reinterpret_cast<const msg::Header*>(frame);
      const uint64_t latency =
          recv_ts > hdr->send_ts_ns ? recv_ts - hdr->send_ts_ns : 0;
      // The first `warmup` frames are timed and then thrown away: same code
      // path, so caches, branch predictors and the ring pages are warm by the
      // time the first recorded sample is taken.
      if (consumed >= cfg.warmup) {
        acc.record(hdr->seq_id, latency);
        ++received;
      }
      ++consumed;
      ++read_index;
      last_progress = recv_ts;
    } else if (st == shm::Ring::FrameStatus::kLapped) {
      ++lapped_events;
      read_index = resume;  // skip the gap; drops show up as seq gaps in metrics
    } else {  // kEmpty
      if (util::now_ns() - last_progress > idle_ns) break;  // producer done
    }
  }

  // Written once, off the measurement loop -- see the note in metrics.h.
  if (!cfg.csv.empty() && !acc.write_csv(cfg.csv)) {
    fprintf(stderr, "consumer: failed to write %s\n", cfg.csv.c_str());
    return 1;
  }

  fprintf(stderr, "consumer: lapped %llu times\n",
          (unsigned long long)lapped_events);
  print_report(acc.report());
  // A short run means the producer stopped early or the requested sample was
  // never reached -- the caller must not treat it as a valid measurement.
  return (cfg.count && received < cfg.count) ? 3 : 0;
}
