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

#include "dpdk_common.h"
#include "message.h"
#include "shm_ring.h"
#include "shm_segment.h"
#include "util.h"

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
  uint64_t non_udp = 0;
  uint64_t wrong_port = 0;
  uint64_t bad_len = 0;  // right port, but the length field is not usable
};

// Parses one received mbuf; on a match, republishes its payload into the
// ring. Always frees the mbuf -- the ring copies what it needs before this
// returns, so freeing right after is safe either way.
void handle_packet(rte_mbuf* m, uint16_t want_port_be, shm::Ring& ring,
                    Stats& stats) {
  const uint8_t* payload = nullptr;
  uint16_t payload_len = 0;

  auto status = dpdk::ReceiverPort::extract_udp_payload(m, want_port_be,
                                                          &payload,
                                                          &payload_len);

  if (status == dpdk::ReceiverPort::ParseStatus::kOk) {
    if (payload_len > 0 && payload_len <= shm::kFrameCap) {
      ring.publish(payload, payload_len);  // the one copy on this side
      ++stats.received;
    } else {
      ++stats.bad_len;
    }
  } else if (status == dpdk::ReceiverPort::ParseStatus::kWrongPort) {
    ++stats.wrong_port;
  } else {
    ++stats.non_udp;
  }
  rte_pktmbuf_free(m);
}

// Busy-spins until SIGINT/SIGTERM: pull a burst, handle each packet, repeat.
Stats run_receiver_loop(dpdk::ReceiverPort& receiver, shm::Ring& ring,
                         uint16_t udp_port, uint16_t burst_size) {
  const uint16_t want_port_be = rte_cpu_to_be_16(udp_port);
  Stats stats;
  rte_mbuf* burst[64];

  while (!g_stop) {
    uint16_t n = receiver.receive(burst, burst_size);
    if (n == 0) {
      rte_pause();
      continue;
    }
    for (uint16_t i = 0; i < n; ++i) {
      handle_packet(burst[i], want_port_be, ring, stats);
    }
  }
  return stats;
}

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
                               /*nb_rxd=*/1024);

  RingHandle out = create_output_ring(cfg.shm_name, cfg.slots);

  std::fprintf(stderr, "dpdk_receiver: listening udp/%u -> shm=%s\n",
               cfg.udp_port, cfg.shm_name.c_str());

  const Stats stats =
      run_receiver_loop(receiver, out.ring, cfg.udp_port, cfg.burst);

  std::fprintf(stderr,
               "\n---- dpdk_receiver ----\n"
               "received   : %llu\t(published into the ring)\n"
               "wrong_port : %llu\t(udp, but not our port)\n"
               "non_udp    : %llu\t(not ipv4/udp at all)\n"
               "bad_len    : %llu\t(our port, unusable length)\n",
               static_cast<unsigned long long>(stats.received),
               static_cast<unsigned long long>(stats.wrong_port),
               static_cast<unsigned long long>(stats.non_udp),
               static_cast<unsigned long long>(stats.bad_len));
  return 0;
}