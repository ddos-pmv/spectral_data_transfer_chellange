// dpdk_sender: reads frames from the producer's shared-memory ring and
// transmits each one as a raw UDP/IPv4/Ethernet packet to every receiver in
// the destination table.
//
// Fan-out is source replication because a VPC does not forward IP multicast
// between instances. The destination table is written so one row holding a group
// address is a genuine multicast send wherever the fabric does replicate; with N
// unicast rows the sender pays N packets per message, and that ceiling is what
// the per-queue counters at exit are for.
//
// One thread. Replication multiplies the per-packet cost but not the per-burst
// cost: the shm ring is read once per message however many receivers there are.
// A second core would not reduce the packet count, which is the actual limit.
//
// Never auto-exits on idle: it is transport infrastructure, not a
// reporting tool. Stop it with Ctrl+C / SIGTERM.
//
// Usage:
//   dpdk_sender [EAL args] -- --shm NAME --slots N --src-ip IP
//               --dest MAC@IP:PORT [--dest ...]
//               [--src-port P] [--burst N] [--mbufs N] [--txd N] [--count N]
//               [--tx-queues N] [--tx-spins N]
//
// --tx-queues caps the queue count (default: one per destination). Setting it to
// 1 puts every receiver on one queue -- the comparison that says whether a queue
// per receiver was worth it.
#include <rte_eal.h>
#include <rte_mbuf.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>

#include <charconv>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "dpdk_common.h"
#include "message.h"
#include "shm_ring.h"
#include "shm_segment.h"
#include "util.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

constexpr uint32_t kMaxDests = dpdk::SenderPort::kMaxDests;
constexpr uint32_t kMaxBurst = 512;

struct Config {
  uint64_t count = 0;
  std::string shm_name = "/fanout_ring";
  uint32_t slots = 1024;
  std::string src_ip_str = "10.0.0.1";
  uint16_t src_port = 40000;
  uint16_t burst = 32;
  uint32_t nb_mbufs = 8191;
  uint32_t cache_size = 256;
  // ENA tops out far lower than a typical PMD, and large LLQ (llq_policy=3)
  // halves it again -- 2048 would abort at bring-up. Print the device's own
  // tx_desc_lim from the dev_info banner if this needs changing.
  uint16_t txd = 512;
  uint16_t tx_queues = 0;  // 0 = one per destination, capped by the device
  uint32_t tx_spins = dpdk::kDefaultTxSpins;

  // Every receiver, in the order given. One receiver is not a special case --
  // it is a table with one row, which is why there is no second addressing form
  // here and no branch on the count anywhere downstream.
  std::vector<dpdk::DestSpec> dests;
};

// An unsigned argument, or a clean exit. std::stoul throws on anything
// non-numeric, and nothing here catches it -- "--burst x" would leave through
// std::terminate and abort the process instead of printing why. from_chars
// reports the failure instead of raising it, and the trailing-character check
// rejects "40001x", which stoul would happily accept.
uint32_t parse_uint(const char* flag, const std::string& v, uint64_t max) {
  uint64_t out = 0;
  const char* last = v.data() + v.size();
  const auto [ptr, ec] = std::from_chars(v.data(), last, out);
  if (ec != std::errc{} || ptr != last || out > max) {
    std::fprintf(stderr, "bad value for %s: '%s' (want 0..%llu)\n", flag,
                 v.c_str(), static_cast<unsigned long long>(max));
    std::exit(2);
  }
  return static_cast<uint32_t>(out);
}

// "MAC@IP:PORT", e.g. "86:c1:68:a5:6e:3c@10.0.0.2:40001". The MAC's own colons
// are why this splits on '@' first and then takes the *last* colon.
//
// Returns the destination rather than filling one through an out-parameter: a
// half-written DestSpec is then not a state the caller can reach, and there is
// no way to read the result without having checked that parsing succeeded.
std::optional<dpdk::DestSpec> parse_dest(const std::string& s) {
  const size_t at = s.find('@');
  if (at == std::string::npos) return std::nullopt;
  const size_t colon = s.rfind(':');
  if (colon == std::string::npos || colon < at) return std::nullopt;

  dpdk::DestSpec d;
  if (!dpdk::parse_mac(s.substr(0, at), &d.mac)) return std::nullopt;
  if (!dpdk::parse_ipv4(s.substr(at + 1, colon - at - 1), &d.ip_be)) {
    return std::nullopt;
  }

  uint16_t port = 0;
  const char* last = s.data() + s.size();
  const auto [ptr, ec] = std::from_chars(s.data() + colon + 1, last, port);
  if (ec != std::errc{} || ptr != last || port == 0) return std::nullopt;
  d.port = port;
  return d;
}

Config parse_args(int argc, char** argv) {
  Config c;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--shm") c.shm_name = next();
    else if (a == "--slots") c.slots = static_cast<decltype(c.slots)>(parse_uint("--slots", next(), UINT32_MAX));
    else if (a == "--dest") {
      const std::string spec = next();
      // No std::move on the result: DestSpec is 12 bytes of plain data (there
      // is a static_assert on that), so its move is its copy.
      const std::optional<dpdk::DestSpec> d = parse_dest(spec);
      if (!d) {
        std::fprintf(stderr, "bad --dest '%s', want MAC@IP:PORT\n", spec.c_str());
        std::exit(2);
      }
      c.dests.push_back(*d);
    }
    else if (a == "--src-ip") c.src_ip_str = next();
    else if (a == "--src-port") c.src_port = static_cast<decltype(c.src_port)>(parse_uint("--src-port", next(), 65535));
    else if (a == "--burst") c.burst = static_cast<decltype(c.burst)>(parse_uint("--burst", next(), 65535));
    else if (a == "--mbufs") c.nb_mbufs = static_cast<decltype(c.nb_mbufs)>(parse_uint("--mbufs", next(), UINT32_MAX));
    else if (a == "--count") c.count = parse_uint("--count", next(), UINT32_MAX);
    else if (a == "--txd") c.txd = static_cast<decltype(c.txd)>(parse_uint("--txd", next(), 65535));
    else if (a == "--tx-queues") c.tx_queues = static_cast<decltype(c.tx_queues)>(parse_uint("--tx-queues", next(), 65535));
    else if (a == "--tx-spins") c.tx_spins = parse_uint("--tx-spins", next(), 1000);
    else {
      std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }

  if (c.dests.empty()) {
    std::fprintf(stderr,
                 "need at least one --dest MAC@IP:PORT (repeatable)\n");
    std::exit(2);
  }
  if (c.dests.size() > kMaxDests) {
    std::fprintf(stderr, "too many --dest (%zu), max %u\n", c.dests.size(),
                 kMaxDests);
    std::exit(2);
  }
  if (c.burst > kMaxBurst) c.burst = kMaxBurst;
  if (c.burst == 0) c.burst = 1;
  return c;
}

// Checked at startup rather than discovered as alloc_fail mid-run: we hold
// n_dests * burst mbufs, and every TX queue can have a full ring awaiting
// reclaim.
void check_mempool_size(const Config& cfg, uint32_t n_dests, uint16_t n_queues) {
  const uint64_t held = static_cast<uint64_t>(n_dests) * cfg.burst;
  const uint64_t in_flight = static_cast<uint64_t>(n_queues) * cfg.txd;
  const uint64_t need = held + in_flight + cfg.cache_size;
  if (cfg.nb_mbufs < need) {
    std::fprintf(stderr,
                 "--mbufs %u is too small: %u dests x %u burst = %llu held, "
                 "%u queues x %u txd = %llu in flight, + %u cache = %llu needed\n",
                 cfg.nb_mbufs, n_dests, static_cast<unsigned>(cfg.burst),
                 static_cast<unsigned long long>(held),
                 static_cast<unsigned>(n_queues), static_cast<unsigned>(cfg.txd),
                 static_cast<unsigned long long>(in_flight), cfg.cache_size,
                 static_cast<unsigned long long>(need));
    std::exit(2);
  }
}

// bufs[d][i] is destination d's packet for message i, so a destination's packets
// are contiguous and the common case (a queue to itself) hands its array
// straight to tx_burst with no gather.
rte_mbuf* g_bufs[kMaxDests][kMaxBurst];

// Scratch for the shared-queue case only: destinations that share a queue are
// merged into one array, message-major.
rte_mbuf* g_gather[kMaxDests * kMaxBurst];

struct DestStats {
  uint64_t sent = 0;
  uint64_t dropped = 0;
};

}  // namespace

int main(int argc, char** argv) {
  int consumed = rte_eal_init(argc, argv);
  if (consumed < 0) rte_exit(EXIT_FAILURE, "rte_eal_init failed\n");
  argc -= consumed;
  argv += consumed;

  Config cfg = parse_args(argc, argv);

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  uint32_t src_ip_be = 0;
  if (!dpdk::parse_ipv4(cfg.src_ip_str, &src_ip_be)) {
    rte_exit(EXIT_FAILURE, "bad --src-ip: %s\n", cfg.src_ip_str.c_str());
  }

  dpdk::SenderPort sender("sender_pool", cfg.nb_mbufs, cfg.cache_size,
                          /*nb_txd=*/cfg.txd, cfg.dests, src_ip_be,
                          cfg.src_port, cfg.tx_queues);

  sender.set_tx_spins(cfg.tx_spins);

  const uint32_t n_dests = sender.n_dests();
  const uint16_t n_queues = sender.n_queues();
  check_mempool_size(cfg, n_dests, n_queues);

  // Which destinations each queue carries, and where its slice of g_gather
  // starts. Built once; the hot loop only walks it. With a queue per receiver
  // every plan holds a single destination.
  //
  // Flat fixed arrays rather than vector<vector>: the hot loop reads this twice
  // per burst (dispatch and refill), and through nested vectors each read is a
  // chain of dependent loads -- the outer buffer, then the inner vector object,
  // then its heap buffer, which may be anywhere -- before the destination index
  // is even known. Here a plan is 32 bytes with no indirection at all, and two
  // of them share a cache line.
  //
  // Indexing plans[] by queue id is safe because n_queues <= n_dests <=
  // kMaxDests always: the port asks for one TX queue per destination and the
  // device can only hand back fewer. batches[] below already relies on the same
  // bound.
  struct QueuePlan {
    size_t gather_off = 0;          // this queue's slice of g_gather
    uint32_t count = 0;             // destinations on this queue
    uint8_t dests[kMaxDests] = {};  // kMaxDests is 16, so a byte each is plenty
  };
  QueuePlan plans[kMaxDests];

  for (uint32_t d = 0; d < n_dests; ++d) {
    QueuePlan& p = plans[sender.queue_of(d)];
    p.dests[p.count++] = static_cast<uint8_t>(d);
  }
  // Queues can carry different numbers of destinations, so the gather offset is
  // a running sum rather than q * something.
  for (uint16_t q = 1; q < n_queues; ++q) {
    plans[q].gather_off = plans[q - 1].gather_off +
                          static_cast<size_t>(cfg.burst) * plans[q - 1].count;
  }

  std::fprintf(stderr, "dpdk_sender: shm=%s, %u destination(s) over %u tx queue(s)\n",
               cfg.shm_name.c_str(), n_dests, static_cast<unsigned>(n_queues));
  for (uint32_t d = 0; d < n_dests; ++d) {
    const dpdk::DestSpec& s = sender.dest(d);
    const uint32_t ip = rte_be_to_cpu_32(s.ip_be);
    std::fprintf(stderr,
                 "  dest %u -> %02x:%02x:%02x:%02x:%02x:%02x %u.%u.%u.%u:%u  (tx queue %u)\n",
                 d, static_cast<unsigned>(s.mac.addr_bytes[0]),
                 static_cast<unsigned>(s.mac.addr_bytes[1]),
                 static_cast<unsigned>(s.mac.addr_bytes[2]),
                 static_cast<unsigned>(s.mac.addr_bytes[3]),
                 static_cast<unsigned>(s.mac.addr_bytes[4]),
                 static_cast<unsigned>(s.mac.addr_bytes[5]),
                 (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff,
                 static_cast<unsigned>(s.port),
                 static_cast<unsigned>(sender.queue_of(d)));
  }


  shm::Segment seg =
      shm::Segment::open(cfg.shm_name, shm::region_size(cfg.slots),
                          /*create=*/false);
  shm::Ring ring;
  ring.attach(seg.base(), cfg.slots, /*init=*/false);

  uint64_t read_index = ring.live_edge();
  const uint64_t t_start = util::now_ns();
  uint64_t msgs = 0, lapped = 0, alloc_fail = 0, empty_spins = 0;
  uint64_t bursts = 0, burst_msgs = 0;
  DestStats dstats[kMaxDests];

  for (uint32_t d = 0; d < n_dests; ++d) {
    if (!sender.alloc_bulk(g_bufs[d], cfg.burst)) {
      rte_exit(EXIT_FAILURE, "cannot alloc %u mbufs for dest %u\n",
               static_cast<unsigned>(cfg.burst), d);
    }
  }

  uint16_t lens[kMaxBurst];
  dpdk::SenderPort::QueueBatch batches[kMaxDests];

  dpdk::SenderPort::prefetch_packet(g_bufs[0][0]);
  while (!g_stop && (cfg.count == 0 || msgs < cfg.count)) {
    // --- drain the ring into destination 0's mbufs -------------------------
    // One read per message regardless of the receiver count: this is the copy
    // that crosses cores (the producer wrote that cache line), and it is the
    // expensive one. The other receivers are served from the result below.
    uint32_t n = 0;
    while (n < cfg.burst) {
      uint32_t len = 0;
      uint64_t resume = 0;

      if (n + 1 < cfg.burst) {
        dpdk::SenderPort::prefetch_packet(g_bufs[0][n + 1]);
      }
      auto st = ring.read(read_index, sender.payload_ptr(g_bufs[0][n]), &len, &resume);
      if (st == shm::Ring::FrameStatus::kOk) {
        lens[n] = static_cast<uint16_t>(len);
        sender.finalize(g_bufs[0][n], /*dest_idx=*/0, lens[n]);
        ++n;
        ++read_index;
      } else if (st == shm::Ring::FrameStatus::kLapped) {
        ++lapped;
        read_index = resume;
      } else {
        ++empty_spins;
        break;  // kEmpty: nothing new right now
      }
    }
    if (n == 0) continue;
    ++bursts;
    burst_msgs += n;
    msgs += n;

    // --- stamp, then replicate --------------------------------------------
    const uint64_t tx_ts = util::now_ns();
    for (uint32_t i = 0; i < n; ++i) {
      std::memcpy(sender.payload_ptr(g_bufs[0][i]) + offsetof(msg::Header, tx_ts_ns),
                  &tx_ts, sizeof(tx_ts));
      // The replication targets are the cold memory on this path: they came out
      // of the pool a whole reuse cycle ago, and unlike the source (which this
      // loop rereads n_dests times and which stays in L1) every one of them is
      // a fresh set of lines to take ownership of.
      // Only the packet bodies are prefetched, not the mbuf structs: those are
      // already hot, because rte_pktmbuf_alloc_bulk resets every mbuf it hands
      // out and the refill at the bottom of this loop did exactly that.
      const bool more = i + 1 < n;
      for (uint32_t d = 1; d < n_dests; ++d) {
        if (more) dpdk::SenderPort::prefetch_packet(g_bufs[d][i + 1]);
        sender.replicate(g_bufs[0][i], g_bufs[d][i], d, lens[i]);
      }
    }

    // --- hand every queue its packets --------------------------------------
    for (uint16_t q = 0; q < n_queues; ++q) {
      const QueuePlan& p = plans[q];
      if (p.count == 1) {
        // The queue-per-receiver case: already contiguous, nothing to gather.
        batches[q].pkts = g_bufs[p.dests[0]];
        batches[q].total = static_cast<uint16_t>(n);
      } else {
        // Shared queue: interleave message-major (m0d0, m0d1, m1d0, ...) so a
        // receiver's k-th message trails another's by the destination count,
        // not by the whole burst.
        rte_mbuf** out = g_gather + p.gather_off;
        uint32_t k = 0;
        for (uint32_t i = 0; i < n; ++i) {
          for (uint32_t j = 0; j < p.count; ++j) out[k++] = g_bufs[p.dests[j]][i];
        }
        batches[q].pkts = out;
        batches[q].total = static_cast<uint16_t>(k);
      }
      batches[q].sent = 0;
    }

    sender.send_all(batches, n_queues);

    // --- account and refill -------------------------------------------------
    // tx_burst consumes a prefix, so for every destination the packets that went
    // out are g_bufs[d][0..sent_d). Those pointers are dead (the PMD frees the
    // mbufs) and get replaced; the unsent tail keeps its mbufs and is simply
    // refilled with new frames next iteration.
    bool refill_ok = true;
    for (uint16_t q = 0; q < n_queues; ++q) {
      const QueuePlan& p = plans[q];
      const uint32_t stride = p.count;
      const uint32_t sent = batches[q].sent;
      for (uint32_t j = 0; j < stride; ++j) {
        // In the interleaved layout destination j occupies positions
        // j, j+stride, j+2*stride...; `sent` of them went out.
        const uint32_t sent_d =
            stride == 1 ? sent : sent / stride + ((sent % stride) > j ? 1u : 0u);
        const uint32_t d = p.dests[j];
        dstats[d].sent += sent_d;
        dstats[d].dropped += n - sent_d;
        if (!sender.alloc_bulk(g_bufs[d], sent_d)) [[unlikely]] {
          ++alloc_fail;
          refill_ok = false;
        }
      }
    }
    if (!refill_ok) break;
  }

  // Фактический темп, а не заказанный. Два разных числа, и путать их нельзя:
  // из кольца мы читаем одно количество, а на провод уходит меньше на
  // tx_dropped. С pps-квотой ENI и с pps_allowance_exceeded сопоставляется
  // только отправленное -- предложенное карта не видела.
  const double elapsed_s =
      static_cast<double>(util::now_ns() - t_start) / 1e9;
  uint64_t total_sent = 0, total_dropped = 0;
  for (uint32_t d = 0; d < n_dests; ++d) {
    total_sent += dstats[d].sent;
    total_dropped += dstats[d].dropped;
  }

  // До выхода из main: деструктор порта делает dev_stop/close.
  dpdk::print_port_stats(sender.port_id());

  std::fprintf(stderr,
               "\n---- dpdk_sender ----\n"
               "messages   : %llu\t(read from the ring and fanned out)\n"
               "dests      : %u over %u tx queue(s) (device max %u)\n"
               "lapped     : %llu\t(producer overwrote us; frames skipped)\n"
               "alloc_fail : %llu\t(mbuf pool empty)\n"
               "empty_spins: %llu\t(read from ring return none)\n"
               "avg_burst  : %.2f\t(avg messages in burst, wanted to send by burst)\n"
               "tx_spins   : %u\t(ретраев на полном TX-кольце, прежде чем выбросить)\n"
               "elapsed    : %.1f s\n"
               "msg in     : %.0f msg/s\t(прочитано из кольца)\n"
               "pkt offered: %.0f pkt/s\t(msg in x %u получателей)\n"
               "pkt sent   : %.0f pkt/s\t(ПРИНЯТО картой -- только это\n"
               "             \t\t сопоставимо с pps_allowance_exceeded)\n"
               "tx_dropped : %llu\t(%.1f%% -- TX-кольцо было полно)\n",
               static_cast<unsigned long long>(msgs),
               n_dests, static_cast<unsigned>(n_queues),
               static_cast<unsigned>(sender.max_tx_queues()),
               static_cast<unsigned long long>(lapped),
               static_cast<unsigned long long>(alloc_fail),
               static_cast<unsigned long long>(empty_spins),
               bursts ? static_cast<double>(burst_msgs) / static_cast<double>(bursts) : 0.0,
               sender.tx_spins(),
               elapsed_s,
               elapsed_s > 0 ? static_cast<double>(msgs) / elapsed_s : 0.0,
               elapsed_s > 0 ? static_cast<double>(msgs) * n_dests / elapsed_s : 0.0,
               n_dests,
               elapsed_s > 0 ? static_cast<double>(total_sent) / elapsed_s : 0.0,
               static_cast<unsigned long long>(total_dropped),
               (total_sent + total_dropped)
                   ? static_cast<double>(total_dropped) * 100.0 /
                         static_cast<double>(total_sent + total_dropped)
                   : 0.0);

  for (uint32_t d = 0; d < n_dests; ++d) {
    std::fprintf(stderr,
                 "dest %u (q%u) : sent %llu, tx_dropped %llu\t(our own loss: tx ring stayed full)\n",
                 d, static_cast<unsigned>(sender.queue_of(d)),
                 static_cast<unsigned long long>(dstats[d].sent),
                 static_cast<unsigned long long>(dstats[d].dropped));
  }
  return 0;
}
