// Latency + delivery metrics. The consumer feeds one observation per received
// message; at report time we sort the collected samples for exact percentiles
// and derive the drop rate from sequence gaps.
//
// End-to-end latency is cut into three segments using the transport stamps in
// msg::Header (see the note there):
//
//   A = tx_ts_ns - send_ts_ns   producer ring -> sender
//   B = rx_ts_ns - tx_ts_ns     the wire
//   C = recv_ts  - rx_ts_ns     receiver -> consumer
//
// Everything is int64_t, not uint64_t. A and C stay on one host and cannot go
// negative, but B crosses the host boundary and carries the two clocks'
// offset, so a negative B is a normal and *informative* reading: it says the
// receiving host's clock runs behind. Clamping it to zero -- which is what the
// old `recv > send ? recv - send : 0` guard did to end-to-end -- would have
// silently turned a clock-offset problem into a pile of zero-latency samples.
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

// Summary of one distribution. p99999 (p99.999) needs at least 1e6 samples to
// mean anything at all -- with fewer, nearest-rank just returns the maximum.
struct Dist {
  int64_t min = 0;
  int64_t max = 0;
  double mean = 0.0;
  int64_t p01 = 0, p50 = 0, p99 = 0, p999 = 0, p9999 = 0, p99999 = 0;
};

struct Report {
  uint64_t received = 0;
  uint64_t expected = 0;   // last_seq - first_seq + 1
  uint64_t dropped = 0;
  double drop_rate = 0.0;

  Dist e2e;  // t3 - t0, the metric the task is judged on
  Dist a;    // producer -> sender     (same host, exact)
  Dist b;    // the wire               (cross host, offset-shifted)
  Dist c;    // receiver -> consumer   (same host, exact)
};

struct Sample {
  uint64_t seq_id;
  int64_t e2e_ns;
  int64_t a_ns;
  int64_t b_ns;
  int64_t c_ns;
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

  void record(uint64_t seq_id, int64_t e2e_ns, int64_t a_ns, int64_t b_ns,
              int64_t c_ns) {
    if (received_ == 0) {
      first_seq_ = seq_id;
      last_seq_ = seq_id;
    } else {
      if (seq_id < first_seq_) first_seq_ = seq_id;
      if (seq_id > last_seq_) last_seq_ = seq_id;
    }
    ++received_;
    samples_.push_back(Sample{seq_id, e2e_ns, a_ns, b_ns, c_ns});
    sum_e2e_ += static_cast<double>(e2e_ns);
    sum_a_ += static_cast<double>(a_ns);
    sum_b_ += static_cast<double>(b_ns);
    sum_c_ += static_cast<double>(c_ns);
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

    // One scratch vector reused for all four distributions: percentile() needs
    // a sorted copy, and holding four of them at once would be four times the
    // peak memory for no reason.
    std::vector<int64_t> s;
    s.reserve(samples_.size());
    const double n = static_cast<double>(received_);
    r.e2e = summarize(s, &Sample::e2e_ns, sum_e2e_ / n);
    r.a = summarize(s, &Sample::a_ns, sum_a_ / n);
    r.b = summarize(s, &Sample::b_ns, sum_b_ / n);
    r.c = summarize(s, &Sample::c_ns, sum_c_ / n);
    return r;
  }

  // Dump the raw samples in arrival order. latency_ns keeps its old name so
  // existing analysis keeps reading it; the three segments are new columns.
  bool write_csv(const std::string& path) const {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "seq,latency_ns,a_ns,b_ns,c_ns\n");
    for (const Sample& s : samples_) {
      std::fprintf(f, "%llu,%lld,%lld,%lld,%lld\n",
                   (unsigned long long)s.seq_id, (long long)s.e2e_ns,
                   (long long)s.a_ns, (long long)s.b_ns, (long long)s.c_ns);
    }
    return std::fclose(f) == 0;
  }

 private:
  Dist summarize(std::vector<int64_t>& scratch, int64_t Sample::*field,
                 double mean) const {
    scratch.clear();
    for (const Sample& s : samples_) scratch.push_back(s.*field);
    std::sort(scratch.begin(), scratch.end());

    Dist d;
    d.min = scratch.front();
    d.max = scratch.back();
    d.mean = mean;
    d.p01 = percentile(scratch, 0.01);
    d.p50 = percentile(scratch, 0.50);
    d.p99 = percentile(scratch, 0.99);
    d.p999 = percentile(scratch, 0.999);
    d.p9999 = percentile(scratch, 0.9999);
    d.p99999 = percentile(scratch, 0.99999);
    return d;
  }

  // Nearest-rank percentile on a sorted vector, p in [0,1].
  static int64_t percentile(const std::vector<int64_t>& sorted, double p) {
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
  double sum_e2e_ = 0.0, sum_a_ = 0.0, sum_b_ = 0.0, sum_c_ = 0.0;
  std::vector<Sample> samples_;
};

}  // namespace metrics
