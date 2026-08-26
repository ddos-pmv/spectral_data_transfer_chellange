// dpdk_sender: reads frames from the producer's shared-memory ring and
// transmits each one as a raw UDP/IPv4/Ethernet packet through a
// SenderPort. The shm-ring read writes straight into the mbuf's payload
// location (sender.payload_ptr(m)) -- there is no intermediate scratch
// buffer, so this is one copy total (ring slot -> mbuf), not two.
//
// Never auto-exits on idle: it is transport infrastructure, not a
// reporting tool. Stop it with Ctrl+C / SIGTERM.
//
// Usage:
//   dpdk_sender [EAL args] -- --shm NAME --slots N --dst-mac MAC
//               --src-ip IP --dst-ip IP [--src-port P] [--dst-port P]
//               [--burst N] [--mbufs N]
#include <rte_eal.h>
#include <rte_mbuf.h>

#include <cstdio>
#include <cstdlib>
#include <csignal>

#include <string>

#include "dpdk_common.h"
#include "message.h"
#include "shm_ring.h"
#include "shm_segment.h"
#include "util.h"

namespace {

  volatile std::sig_atomic_t g_stop = 0;
  void on_signal(int) { g_stop = 1; }

struct Config {
  uint64_t count = 0;
  std::string shm_name = "/fanout_ring";
  uint32_t slots = 1024;
  std::string dst_mac_str;
  std::string src_ip_str = "10.0.0.1";
  std::string dst_ip_str = "10.0.0.2";
  uint16_t src_port = 40000;
  uint16_t dst_port = 40001;
  uint16_t burst = 32;
  uint32_t nb_mbufs = 8191;
  uint32_t cache_size = 256;
  uint16_t txd = 2048;
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
    else if (a == "--dst-mac") c.dst_mac_str = next();
    else if (a == "--src-ip") c.src_ip_str = next();
    else if (a == "--dst-ip") c.dst_ip_str = next();
    else if (a == "--src-port") c.src_port = static_cast<uint16_t>(std::stoul(next()));
    else if (a == "--dst-port") c.dst_port = static_cast<uint16_t>(std::stoul(next()));
    else if (a == "--burst") c.burst = static_cast<uint16_t>(std::stoul(next()));
    else if (a == "--mbufs") c.nb_mbufs = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--count") c.count = static_cast<uint64_t>(std::stoull(next()));
    else if (a == "--txd") c.txd = static_cast<uint16_t>(std::stoul(next()));
    else {
      std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }
  if (c.dst_mac_str.empty()) {
    std::fprintf(stderr, "--dst-mac is required (peer's NIC MAC address)\n");
    std::exit(2);
  }
  if (c.burst > 1024) c.burst = 1024;
  return c;
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

  rte_ether_addr dst_mac{};
  if (!dpdk::parse_mac(cfg.dst_mac_str, &dst_mac)) {
    rte_exit(EXIT_FAILURE, "bad --dst-mac: %s\n", cfg.dst_mac_str.c_str());
  }
  uint32_t src_ip_be = 0, dst_ip_be = 0;
  if (!dpdk::parse_ipv4(cfg.src_ip_str, &src_ip_be) ||
      !dpdk::parse_ipv4(cfg.dst_ip_str, &dst_ip_be)) {
    rte_exit(EXIT_FAILURE, "bad --src-ip/--dst-ip\n");
      }

  dpdk::SenderPort sender("sender_pool", cfg.nb_mbufs, cfg.cache_size,
                           /*nb_txd=*/cfg.txd, dst_mac, src_ip_be, dst_ip_be,
                           cfg.src_port, cfg.dst_port);

  shm::Segment seg =
      shm::Segment::open(cfg.shm_name, shm::region_size(cfg.slots),
                          /*create=*/false);
  shm::Ring ring;
  ring.attach(seg.base(), cfg.slots, /*init=*/false);

  uint64_t read_index = ring.live_edge();
  uint64_t sent_msgs = 0, lapped = 0, alloc_fail = 0, tx_dropped = 0, empty_spins = 0, bursts = 0, burst_pkts = 0;

  rte_mbuf* burst[1024];

  std::fprintf(stderr, "dpdk_sender: shm=%s -> %s:%u dst_mac=%s\n",
               cfg.shm_name.c_str(), cfg.dst_ip_str.c_str(), cfg.dst_port,
               cfg.dst_mac_str.c_str());

  if (!sender.alloc_bulk(burst, cfg.burst)) {
    std::fprintf(stderr, "cannot alloc bulk");
    rte_exit(EXIT_FAILURE, "cannot alloc bulk");
  }

  rte_prefetch0_write(rte_pktmbuf_mtod(burst[0], void*));
  while (!g_stop && (cfg.count == 0 || sent_msgs < cfg.count)) {
    uint32_t n = 0;
    while (n < cfg.burst) {
      uint32_t len = 0;
      uint64_t resume = 0;

      if (n + 1 < cfg.burst) {
        rte_prefetch0_write(rte_pktmbuf_mtod(burst[n+1], void*));
        rte_prefetch0_write(rte_pktmbuf_mtod_offset(burst[n+1], void*, 128));
      }
      // Ring copies the frame straight into mbuf memory -- no scratch buffer.
      auto st = ring.read(read_index, sender.payload_ptr(burst[n]), &len, &resume);
      if (st == shm::Ring::FrameStatus::kOk) {
        sender.finalize(burst[n++], static_cast<uint16_t>(len));
        ++read_index;
      } else if (st == shm::Ring::FrameStatus::kLapped) {
        ++lapped;
        read_index = resume;
      } else {
        ++empty_spins;
        break;  // kEmpty: nothing new right now
      }
    }
    ++bursts;
    burst_pkts += n;
    uint32_t sent_in_burst = sender.send_with_retry(burst, n);
    sent_msgs += sent_in_burst;

    // alloc sent mbufs
    if (sent_in_burst != 0 && !sender.alloc_bulk(burst, sent_in_burst)) [[unlikely]] {++alloc_fail; break;}

    auto dropped_in_burst = n - sent_in_burst;
    tx_dropped += dropped_in_burst;
  }

  // До выхода из main: деструктор порта делает dev_stop/close.
  dpdk::print_port_stats(sender.port_id());

  std::fprintf(stderr,
               "\n---- dpdk_sender ----\n"
               "sent       : %llu\n"
               "lapped     : %llu\t(producer overwrote us; frames skipped)\n"
               "tx_dropped : %llu\t(our own loss: tx ring stayed full)\n"
               "alloc_fail : %llu\t(mbuf pool empty)\n"
               "empty_spins: %llu\t(read from ring return none)\n"
               "avg_burst : %.2f\t(avg packets in burst)\n",
               static_cast<unsigned long long>(sent_msgs),
               static_cast<unsigned long long>(lapped),
               static_cast<unsigned long long>(tx_dropped),
               static_cast<unsigned long long>(alloc_fail),
               static_cast<unsigned long long>(empty_spins),
               static_cast<double>(burst_pkts)/static_cast<double>(bursts));
  return 0;
}