// Latency + delivery metrics. The consumer feeds one observation per received
// message (send->recv latency, sequence id); at report time we sort the
// collected samples for exact percentiles and derive the drop rate from seq
// gaps.
//
// Samples are kept in memory for the whole run and only written out at the end
// (write_csv). Writing per message would put file I/O on the very loop we are
// timing, so the numbers would carry filesystem jitter that has nothing to do
// with the transport.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace metrics {

struct Report {
  uint64_t received = 0;
  uint64_t expected = 0;   // last_seq - first_seq + 1
  uint64_t dropped = 0;
  double drop_rate = 0.0;

  uint64_t lat_min = 0;
  uint64_t lat_max = 0;
  double lat_mean = 0.0;
  uint64_t p01 = 0, p50 = 0, p99 = 0, p999 = 0, p9999 = 0;
};

struct Sample {
  uint64_t seq_id;
  uint64_t latency_ns;
};

class Accumulator {
 public:
  explicit Accumulator(size_t reserve = 0) {
    if (reserve) {
      // resize+clear rather than reserve: reserve only asks the allocator for
      // address space, so the first pass through the buffer would take a page
      // fault every 4 KiB *inside* the measurement loop. resize writes to every
      // page now; clear keeps the capacity.
      samples_.resize(reserve);
      samples_.clear();
    }
  }

  void record(uint64_t seq_id, uint64_t latency_ns) {
    if (received_ == 0) {
      first_seq_ = seq_id;
      last_seq_ = seq_id;
    } else {
      if (seq_id < first_seq_) first_seq_ = seq_id;
      if (seq_id > last_seq_) last_seq_ = seq_id;
    }
    ++received_;
    samples_.push_back(Sample{seq_id, latency_ns});
    sum_ += static_cast<double>(latency_ns);
  }

  Report report() const {
    Report r;
    r.received = received_;
    if (received_ == 0) return r;

    r.expected = last_seq_ - first_seq_ + 1;
    r.dropped = r.expected > r.received ? r.expected - r.received : 0;
    r.drop_rate = r.expected ? static_cast<double>(r.dropped) /
                                   static_cast<double>(r.expected)
                             : 0.0;

    std::vector<uint64_t> s;
    s.reserve(samples_.size());
    for (const Sample& sample : samples_) s.push_back(sample.latency_ns);
    std::sort(s.begin(), s.end());
    r.lat_min = s.front();
    r.lat_max = s.back();
    r.lat_mean = sum_ / static_cast<double>(received_);
    r.p01 = percentile(s, 0.01);
    r.p50 = percentile(s, 0.50);
    r.p99 = percentile(s, 0.99);
    r.p999 = percentile(s, 0.999);
    r.p9999 = percentile(s, 0.9999);
    return r;
  }

  // Dump the raw samples as `seq,latency_ns` rows, in arrival order. Call once,
  // after the measurement loop.
  bool write_csv(const std::string& path) const {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "seq,latency_ns\n");
    for (const Sample& s : samples_) {
      std::fprintf(f, "%llu,%llu\n", (unsigned long long)s.seq_id,
                   (unsigned long long)s.latency_ns);
    }
    return std::fclose(f) == 0;
  }

 private:
  // Nearest-rank percentile on a sorted vector, p in [0,1].
  static uint64_t percentile(const std::vector<uint64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    size_t n = sorted.size();
    size_t rank = static_cast<size_t>(p * static_cast<double>(n));
    if (static_cast<double>(rank) < p * static_cast<double>(n)) ++rank;
    if (rank < 1) rank = 1;
    if (rank > n) rank = n;
    return sorted[rank - 1];
  }

  uint64_t received_ = 0;
  uint64_t first_seq_ = 0;
  uint64_t last_seq_ = 0;
  double sum_ = 0.0;
  std::vector<Sample> samples_;
};

}  // namespace metrics
