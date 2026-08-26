// dpdk_receiver: polls a ReceiverPort for UDP/IPv4 packets, parses the
// Ethernet/IP/UDP framing in place, and republishes the payload (the
// original msg::Header + body, byte-for-byte) into a local shared-memory
// ring -- the exact layout consumer.cpp expects when run with --from-edge.
//
// Usage:
//   dpdk_receiver [EAL args] -- --shm NAME --slots N [--port P] [--burst N]
#include <rte_eal.h>
#include <rte_mbuf.h>

#include <cstdio>
#include <cstdlib>

#include <string>
#include <csignal>
#include <cstddef>

#include "dpdk_common.h"
#include "message.h"
#include "shm_ring.h"
#include "shm_segment.h"
#include "util.h"
#include "delivery.h"

namespace {

  volatile std::sig_atomic_t g_stop = 0;
  void on_signal(int) { g_stop = 1; }

struct Config {
  std::string shm_name = "/fanout_ring";
  uint32_t slots = 1024;
  uint16_t udp_port = 40001;
  uint16_t burst = 32;
  uint32_t nb_mbufs = 8191;
  uint32_t cache_size = 256;
  uint16_t rxd = 2048;
};

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
    else if (a == "--slots") c.slots = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--port") c.udp_port = static_cast<uint16_t>(std::stoul(next()));
    else if (a == "--burst") c.burst = static_cast<uint16_t>(std::stoul(next()));
    else if (a == "--mbufs") c.nb_mbufs = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--rxd") c.rxd = static_cast<uint32_t>(std::stoul(next()));
    else {
      std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }
  if (c.burst > 64) c.burst = 64;
  return c;
}

// ---- shm ring attach ------------------------------------------------------

// Bundles the mapping (Segment) with the view over it (Ring) so the mapping
// can't accidentally outlive -- or be outlived by -- the ring using it.
struct RingHandle {
  shm::Segment segment;
  shm::Ring ring;
};

RingHandle create_output_ring(const std::string& name, uint32_t slots) {
  RingHandle h{shm::Segment::open(name, shm::region_size(slots),
                                   /*create=*/true),
               {}};
  h.ring.attach(h.segment.base(), slots, /*init=*/true);
  return h;
}

// ---- hot path -------------------------------------------------------------

struct Stats {
  uint64_t received = 0;
  uint64_t bursts = 0;
  uint64_t burst_pkts = 0;
  uint64_t non_udp = 0;
  uint64_t wrong_port = 0;
  uint64_t bad_len = 0;  // right port, but the length field is not usable
  uint64_t empty_spins = 0;
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

  dpdk::ReceiverPort receiver("receiver_pool", cfg.nb_mbufs, cfg.cache_size,
                               /*nb_rxd=*/cfg.rxd);

  RingHandle out = create_output_ring(cfg.shm_name, cfg.slots);

  std::fprintf(stderr, "dpdk_receiver: listening udp/%u -> shm=%s\n",
               cfg.udp_port, cfg.shm_name.c_str());

  const uint16_t want_port_be = rte_cpu_to_be_16(cfg.udp_port);

  Stats stats;
  delivery::MonotonicGate gate;

  rte_mbuf* burst[64];

  while (!g_stop) {
    uint16_t n = receiver.receive(burst, cfg.burst);
    ++stats.bursts;
    stats.burst_pkts += n;
    if (n==0) [[unlikely]] {
      ++stats.empty_spins;
      continue;
    }

    rte_prefetch0(rte_pktmbuf_mtod(burst[0], void*));
    for (uint16_t i = 0; i < n; ++i) {
      const uint8_t* payload = nullptr;
      uint16_t payload_len = 0;

      if (i + 1 < n) {
        rte_prefetch0(rte_pktmbuf_mtod(burst[i + 1], void*));
        rte_prefetch0(rte_pktmbuf_mtod_offset(burst[i+1], void*, 128));
      }

      auto status = dpdk::ReceiverPort::extract_udp_payload(burst[i], want_port_be,
                                                              &payload,
                                                              &payload_len);

      if (status == dpdk::ReceiverPort::ParseStatus::kOk) {
        if (payload_len >= sizeof(msg::Header) && payload_len <= shm::kFrameCap) {
          uint64_t seq_id = 0;
          std::memcpy(&seq_id, payload + offsetof(msg::Header, seq_id), sizeof(seq_id));
          if (gate.admit(seq_id)) {
            out.ring.publish(payload, payload_len);  // the one copy on this side
            ++stats.received;
          }
        } else {
          ++stats.bad_len;
        }
      } else if (status == dpdk::ReceiverPort::ParseStatus::kWrongPort) {
        ++stats.wrong_port;
      } else {
        ++stats.non_udp;
      }
    }
    rte_pktmbuf_free_bulk(burst, n);
  }

  const delivery::MonotonicGate::Stats& gate_stats = gate.stats();
  std::fprintf(stderr,
               "\n---- dpdk_receiver ----\n"
               "received       : %llu\t(published into the ring)\n"
               "wrong_port     : %llu\t(udp, but not our port)\n"
               "non_udp        : %llu\t(not ipv4/udp at all)\n"
               "bad_len        : %llu\t(our port, unusable length)\n"
               "empty_spins    : %llu\t(receiver.receive(burst, cfg.burst) returned zero)\n"
               "avg_burst      : %.2f\t(avg packets in burst)\n"
               "gate_delivered : %llu\t\n"
               "gate_suppressed: %llu\t\n"
               "gate_gaps      : %llu\t\n"
               "gate_missing   : %llu\t\n"
               "injected_drops : 0\t\n",
               static_cast<unsigned long long>(stats.received),
               static_cast<unsigned long long>(stats.wrong_port),
               static_cast<unsigned long long>(stats.non_udp),
               static_cast<unsigned long long>(stats.bad_len),
               static_cast<unsigned long long>(stats.empty_spins),
               static_cast<double>(stats.burst_pkts)/static_cast<double>(stats.bursts),
               static_cast<unsigned long long>(gate_stats.delivered),
               static_cast<unsigned long long>(gate_stats.suppressed),
               static_cast<unsigned long long>(gate_stats.gaps),
               static_cast<unsigned long long>(gate_stats.missing));
  return 0;
}