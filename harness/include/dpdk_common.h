// dpdk_port.h
//
// OOP wrapper around the DPDK port lifecycle for the sender/receiver pair.
// SenderPort owns TX queues -- one per receiver where the device allows it --
// and ReceiverPort an RX-only queue; both share the same bring-up sequence
// (mempool -> configure -> queue setup -> start) through a private helper so
// it's written once, not duplicated per class.
//
// SenderPort::payload_ptr()/finalize() are split on purpose: the caller
// (dpdk_sender.cpp) reads the shm-ring frame directly into mbuf memory at
// payload_ptr(), so there is exactly one copy from the ring to the wire --
// not a copy into a scratch buffer followed by a second copy into the mbuf.
// With several receivers that first copy is still made once; the other
// receivers' packets are filled from it via replicate().
#pragma once

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_udp.h>

#include <netinet/in.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "shm_ring.h"

namespace dpdk {

// Сколько раз подряд обойти очереди без единого отправленного пакета, прежде
// чем сдаться и посчитать остаток потерянным. Значение по умолчанию, ручка --
// SenderPort::set_tx_spins().
//
// Число не безобидное. Под перегрузкой почти каждый пакет обречён, и тогда
// стоимость одного сообщения -- это kMaxTxSpins холостых обращений к tx_burst,
// а не одно. Измерено на ENA при 2M msg/s на одного получателя: 92% всех
// вызовов tx_burst не отправили ничего, и это порядка половины ядра
// отправителя, потраченного на пакеты, которые всё равно будут выброшены.
// Занижение обменивает эту работу на более ранний отказ.
inline constexpr uint32_t kDefaultTxSpins = 10;
inline constexpr uint32_t kMaxRxSpins = 10;

// "AA:BB:CC:DD:EE:FF" -> rte_ether_addr. Hand-parsed, no dependency on
// rte_ether_unformat_addr (not present in every DPDK version).
inline bool parse_mac(const std::string& s, rte_ether_addr* out) {
  unsigned b[6];
  if (std::sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3],
                   &b[4], &b[5]) != 6) {
    return false;
  }
  for (int i = 0; i < 6; ++i) out->addr_bytes[i] = static_cast<uint8_t>(b[i]);
  return true;
}

// "a.b.c.d" -> big-endian uint32_t, the form rte_ipv4_hdr fields expect.
inline bool parse_ipv4(const std::string& s, uint32_t* out_be) {
  unsigned a, b, c, d;
  if (std::sscanf(s.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
  *out_be = rte_cpu_to_be_32((a << 24) | (b << 16) | (c << 8) | d);
  return true;
}

// One receiver: where its packets are addressed. The sender holds a table of
// these -- fan-out is "the table has more than one row", not a separate code
// path. A multicast group is expressible as a single row (group MAC + 239/8
// address) on a fabric that replicates; on a VPC, which does not, the table
// simply carries one row per receiver and the sender replicates itself.
struct DestSpec {
  rte_ether_addr mac{};
  uint32_t ip_be = 0;
  uint16_t port = 0;
};

// 12 bytes of plain data. Worth asserting rather than assuming, because the
// whole sender passes these around by value -- into the vector, into
// SenderPort's table -- on the understanding that a copy is all there is. Any
// future member that owns something would silently make those copies expensive
// and would make std::move on a DestSpec suddenly mean something.
static_assert(std::is_trivially_copyable_v<DestSpec>,
              "DestSpec is stored and passed by value; keep it a POD");

namespace detail {

struct BringUp {
  uint16_t port_id;
  rte_mempool* mempool;
  rte_ether_addr mac;
  bool tx_ip_cksum_offload;
  bool tx_mbuf_fast_free;
  uint16_t nb_rxq;          // RX queues actually created
  uint16_t nb_txq;          // TX queues actually created
  uint16_t max_rx_queues;   // what the device would have allowed
  uint16_t max_tx_queues;
};

inline uint16_t find_first_port() {
  uint16_t port_id;
  RTE_ETH_FOREACH_DEV(port_id) { return port_id; }
  std::fprintf(stderr, "dpdk: no available ethernet devices\n");
  std::exit(1);
}

inline rte_eth_dev_info query_dev_info(uint16_t port_id) {
  rte_eth_dev_info info{};
  int ret = rte_eth_dev_info_get(port_id, &info);
  if (ret != 0) {
    std::fprintf(stderr, "rte_eth_dev_info_get failed: %s\n",
                 rte_strerror(-ret));
    std::exit(1);
  }
  return info;
}

// Печатается один раз при старте, ДО проверки дескрипторов: когда та
// обрывает запуск, лимиты устройства уже на экране, и видно, какое число
// вписывать в host.conf. Уходит в тот же лог, что и статистика прогона, --
// потом не приходится гадать, на какой карте были получены числа.
//
// Нулевые thresh означают "PMD решает сам": tx_free_thresh -- сколько
// отправленных дескрипторов накопится, прежде чем драйвер пойдёт их
// освобождать, и при большой TX-очереди именно он определяет, как быстро
// mbuf возвращаются в пул.
inline void print_dev_info(uint16_t port_id, const rte_eth_dev_info& info) {
  std::fprintf(stderr,
      "---- dev_info: port %u ----\n"
      "driver      : %s\n"
      "queues max  : rx %u, tx %u\n"
      "rx_desc_lim : min %u, max %u, align %u\n"
      "tx_desc_lim : min %u, max %u, align %u, seg_max %u\n"
      "mtu         : min %u, max %u (rx pktlen max %u)\n"
      "tx thresh   : free %u, rs %u\n"
      "rx thresh   : free %u\n"
      "offload capa: rx 0x%016llx, tx 0x%016llx\n",
      static_cast<unsigned>(port_id),
      info.driver_name ? info.driver_name : "(unknown)",
      static_cast<unsigned>(info.max_rx_queues),
      static_cast<unsigned>(info.max_tx_queues),
      static_cast<unsigned>(info.rx_desc_lim.nb_min),
      static_cast<unsigned>(info.rx_desc_lim.nb_max),
      static_cast<unsigned>(info.rx_desc_lim.nb_align),
      static_cast<unsigned>(info.tx_desc_lim.nb_min),
      static_cast<unsigned>(info.tx_desc_lim.nb_max),
      static_cast<unsigned>(info.tx_desc_lim.nb_align),
      static_cast<unsigned>(info.tx_desc_lim.nb_seg_max),
      static_cast<unsigned>(info.min_mtu),
      static_cast<unsigned>(info.max_mtu),
      static_cast<unsigned>(info.max_rx_pktlen),
      static_cast<unsigned>(info.default_txconf.tx_free_thresh),
      static_cast<unsigned>(info.default_txconf.tx_rs_thresh),
      static_cast<unsigned>(info.default_rxconf.rx_free_thresh),
      static_cast<unsigned long long>(info.rx_offload_capa),
      static_cast<unsigned long long>(info.tx_offload_capa));
}

inline bool supports_ip_cksum_offload(const rte_eth_dev_info& info) {
  return (info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM) != 0;
}

// Not a card feature but a promise: every mbuf handed to tx_burst comes from one
// pool, is unsegmented, and is unshared. The PMD then frees the batch with one
// put_bulk instead of per-mbuf refcount/segment/clone checks.
//
// SenderPort satisfies this structurally -- one pool, one segment, and copies
// rather than clones on fan-out. That last one is load-bearing: indirect mbufs
// would break the promise and make this flag memory corruption, not a slowdown.
inline bool supports_mbuf_fast_free(const rte_eth_dev_info& info) {
  return (info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE) != 0;
}

inline rte_mempool* create_mbuf_pool(const std::string& name,
                                      uint32_t nb_mbufs,
                                      uint32_t cache_size) {
  rte_mempool* pool =
      rte_pktmbuf_pool_create(name.c_str(), nb_mbufs, cache_size, 0,
                               RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
  if (!pool) {
    std::fprintf(stderr, "rte_pktmbuf_pool_create failed: %s\n",
                 rte_strerror(rte_errno));
    std::exit(1);
  }
  return pool;
}

inline void configure_device(uint16_t port_id, uint16_t nb_rxq, uint16_t nb_txq,
                              uint64_t tx_offloads) {
  rte_eth_conf eth_conf{};
  eth_conf.txmode.offloads = tx_offloads;
  int ret = rte_eth_dev_configure(port_id, nb_rxq, nb_txq, &eth_conf);
  if (ret != 0) {
    std::fprintf(stderr, "rte_eth_dev_configure failed: %s\n",
                 rte_strerror(-ret));
    std::exit(1);
  }
}

// Refuses to run on a mismatch rather than quietly using a different number:
// ring depth changes the tail and must not be substituted behind our back.
//
// A null pointer means "no queues this direction" -- the DPDK call accepts NULL
// and skips that side, so a TX-only port is not aborted over an RX minimum it
// never allocates against.
inline void adjust_descriptor_counts_strong(uint16_t port_id, uint16_t* nb_rxd,
                                      uint16_t* nb_txd) {
  const uint16_t wanted_rxd = nb_rxd ? *nb_rxd : 0;
  const uint16_t wanted_txd = nb_txd ? *nb_txd : 0;
  int ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, nb_rxd, nb_txd);
  if (ret != 0) {
    std::fprintf(stderr, "rte_eth_dev_adjust_nb_rx_tx_desc failed: %s\n",
                 rte_strerror(-ret));
    std::exit(1);
  }

  if (nb_rxd && *nb_rxd != wanted_rxd) {
    std::fprintf(stderr,
                 "RX descriptors: requested %u, device allows %u\n",
                 wanted_rxd, *nb_rxd);
    std::exit(1);
  }
  if (nb_txd && *nb_txd != wanted_txd) {
    // On ENA this almost always means large LLQ (llq_policy=3): 256-byte queue
    // entries in the same device memory halve how many of them fit, so the TX
    // ring maximum drops. That trade is worth taking -- inline packets are why
    // the tail is low -- so the fix is to lower --txd to the number printed
    // here, not to turn large LLQ off.
    std::fprintf(stderr,
                 "TX descriptors: requested %u, device allows %u\n"
                 "  (on ENA with llq_policy=3 the TX ring is half depth; "
                 "set --txd / SENDER_TXD to %u)\n",
                 wanted_txd, *nb_txd, *nb_txd);
    std::exit(1);
  }
}

inline void setup_rx_queue(uint16_t port_id, uint16_t queue_id, uint16_t nb_rxd,
                            rte_mempool* pool) {
  int ret = rte_eth_rx_queue_setup(port_id, queue_id, nb_rxd, rte_socket_id(),
                                    nullptr, pool);
  if (ret != 0) {
    std::fprintf(stderr, "rte_eth_rx_queue_setup(q=%u) failed: %s\n",
                 static_cast<unsigned>(queue_id), rte_strerror(-ret));
    std::exit(1);
  }
}

inline void setup_tx_queue(uint16_t port_id, uint16_t queue_id, uint16_t nb_txd,
                            const rte_eth_dev_info& dev_info,
                            uint64_t tx_offloads) {
  rte_eth_txconf txconf = dev_info.default_txconf;
  txconf.offloads = tx_offloads;
  int ret = rte_eth_tx_queue_setup(port_id, queue_id, nb_txd, rte_socket_id(),
                                    &txconf);
  if (ret != 0) {
    std::fprintf(stderr, "rte_eth_tx_queue_setup(q=%u) failed: %s\n",
                 static_cast<unsigned>(queue_id), rte_strerror(-ret));
    std::exit(1);
  }
}

inline void start_device(uint16_t port_id) {
  int ret = rte_eth_dev_start(port_id);
  if (ret != 0) {
    std::fprintf(stderr, "rte_eth_dev_start failed: %s\n",
                 rte_strerror(-ret));
    std::exit(1);
  }
}

inline rte_ether_addr read_mac_address(uint16_t port_id) {
  rte_ether_addr mac{};
  int ret = rte_eth_macaddr_get(port_id, &mac);
  if (ret != 0) {
    std::fprintf(stderr, "rte_eth_macaddr_get failed: %s\n",
                 rte_strerror(-ret));
    std::exit(1);
  }
  return mac;
}

// Same sequence for both port classes -- this is the checklist, each step
// is one named function above. Order matters: mempool before configure,
// configure before queue setup, queue setup before start.
// Clamped rather than an error: queue count is a property of the instance size,
// not of the configuration, so the caller adapts instead of refusing to start.
// Never silent.
inline uint16_t clamp_queues(const char* dir, uint16_t wanted, uint16_t avail) {
  if (wanted <= avail) return wanted;
  std::fprintf(stderr,
               "dpdk: asked for %u %s queues, device allows %u -- sharing\n",
               static_cast<unsigned>(wanted), dir,
               static_cast<unsigned>(avail));
  return avail;
}

// Each direction is a pair -- how many queues, how deep -- and a count of zero
// means the port does not do that direction at all: no descriptors requested,
// no device limit checked, no queue created.
//
// Order matters: mempool before configure, configure before queue setup, queue
// setup before start.
inline BringUp bring_up(const std::string& pool_name, uint32_t nb_mbufs,
                         uint32_t cache_size,
                         uint16_t wanted_rxq, uint16_t nb_rxd,
                         uint16_t wanted_txq, uint16_t nb_txd) {
  BringUp b{};
  b.port_id = find_first_port();

  const rte_eth_dev_info dev_info = query_dev_info(b.port_id);
  print_dev_info(b.port_id, dev_info);

  b.max_rx_queues = dev_info.max_rx_queues;
  b.max_tx_queues = dev_info.max_tx_queues;
  b.nb_rxq = wanted_rxq ? clamp_queues("RX", wanted_rxq, b.max_rx_queues) : 0;
  b.nb_txq = wanted_txq ? clamp_queues("TX", wanted_txq, b.max_tx_queues) : 0;

  if (b.nb_rxq == 0 && b.nb_txq == 0) {
    std::fprintf(stderr, "dpdk: a port with neither RX nor TX queues\n");
    std::exit(1);
  }

  b.tx_ip_cksum_offload = b.nb_txq != 0 && supports_ip_cksum_offload(dev_info);
  b.tx_mbuf_fast_free = b.nb_txq != 0 && supports_mbuf_fast_free(dev_info);

  uint64_t tx_offloads = 0;
  if (b.tx_ip_cksum_offload) tx_offloads |= RTE_ETH_TX_OFFLOAD_IPV4_CKSUM;
  if (b.tx_mbuf_fast_free) tx_offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

  b.mempool = create_mbuf_pool(pool_name, nb_mbufs, cache_size);

  configure_device(b.port_id, b.nb_rxq, b.nb_txq, tx_offloads);

  // Null for a direction we do not use -- see adjust_descriptor_counts_strong.
  adjust_descriptor_counts_strong(b.port_id, b.nb_rxq ? &nb_rxd : nullptr,
                                   b.nb_txq ? &nb_txd : nullptr);

  for (uint16_t q = 0; q < b.nb_rxq; ++q) {
    setup_rx_queue(b.port_id, q, nb_rxd, b.mempool);
  }
  for (uint16_t q = 0; q < b.nb_txq; ++q) {
    setup_tx_queue(b.port_id, q, nb_txd, dev_info, tx_offloads);
  }

  start_device(b.port_id);
  b.mac = read_mac_address(b.port_id);

  std::fprintf(stderr,
               "dpdk: port %u up (ip_cksum_hw=%d, mbuf_fast_free=%d, "
               "rx_queues=%u, tx_queues=%u)\n",
               b.port_id, b.tx_ip_cksum_offload, b.tx_mbuf_fast_free,
               static_cast<unsigned>(b.nb_rxq),
               static_cast<unsigned>(b.nb_txq));
  return b;
}

}  // namespace detail

// ---------------------------------------------------------------------
// Счётчики порта в конце прогона -- ответ на вопрос "во что упёрлись".
//
// ethtool на этой карте бесполезен: она отдана DPDK, ядро её не обслуживает
// и показывает чужие или застывшие числа. Единственный живой источник --
// xstats самого PMD. У net_ena там лежат счётчики лимитов инстанса
// (*_allowance_exceeded): ненулевой pps_allowance_exceeded значит, что
// упор в квоту ENI, и никакая длина очереди этого не лечит.
//
// Порт должен быть ещё поднят, поэтому звать нужно ДО разрушения
// SenderPort/ReceiverPort -- их деструктор делает rte_eth_dev_stop/close.
//
// Печатаем только ненулевые счётчики, иначе у ENA вывод на сотню строк.
// Исключение -- строки про allowance: там ровно ноль и есть полезный ответ,
// поэтому они печатаются всегда, если PMD их отдаёт.
inline void print_port_stats(uint16_t port_id) {
  rte_eth_stats s{};
  int ret = rte_eth_stats_get(port_id, &s);
  if (ret == 0) {
    std::fprintf(stderr,
        "\n---- port %u: rte_eth_stats ----\n"
        "opackets : %llu\n"
        "obytes   : %llu\n"
        "oerrors  : %llu\t(NIC отверг пакет на передаче)\n"
        "ipackets : %llu\n"
        "ibytes   : %llu\n"
        "ierrors  : %llu\n"
        "imissed  : %llu\t(не хватило RX-дескрипторов)\n"
        "rx_nombuf: %llu\t(пул mbuf пуст на приёме)\n",
        static_cast<unsigned>(port_id),
        static_cast<unsigned long long>(s.opackets),
        static_cast<unsigned long long>(s.obytes),
        static_cast<unsigned long long>(s.oerrors),
        static_cast<unsigned long long>(s.ipackets),
        static_cast<unsigned long long>(s.ibytes),
        static_cast<unsigned long long>(s.ierrors),
        static_cast<unsigned long long>(s.imissed),
        static_cast<unsigned long long>(s.rx_nombuf));
  } else {
    std::fprintf(stderr, "\nrte_eth_stats_get: %s\n", rte_strerror(-ret));
  }

  // Per-queue TX counters are not in rte_eth_stats any more (DPDK dropped the
  // q_* arrays in favour of xstats), so "did each queue actually carry traffic"
  // is answered by the tx_q<N>_packets rows the loop below prints, and by the
  // sender's own per-destination accounting at exit.
  const int n = rte_eth_xstats_get_names(port_id, nullptr, 0);
  if (n <= 0) {
    std::fprintf(stderr, "xstats: PMD их не отдаёт (%d)\n", n);
    return;
  }
  std::vector<rte_eth_xstat_name> names(static_cast<size_t>(n));
  std::vector<rte_eth_xstat> vals(static_cast<size_t>(n));
  if (rte_eth_xstats_get_names(port_id, names.data(), n) != n ||
      rte_eth_xstats_get(port_id, vals.data(), n) != n) {
    std::fprintf(stderr, "xstats: чтение не удалось\n");
    return;
  }

  std::fprintf(stderr, "---- port %u: xstats ----\n",
               static_cast<unsigned>(port_id));
  unsigned shown = 0;
  for (int i = 0; i < n; ++i) {
    // id -- индекс в массиве имён, а не позиция в массиве значений.
    const char* name = names[vals[i].id].name;
    const bool allowance = std::strstr(name, "allowance") != nullptr;
    if (vals[i].value == 0 && !allowance) continue;
    std::fprintf(stderr, "%-40s %llu%s\n", name,
                 static_cast<unsigned long long>(vals[i].value),
                 allowance ? "\t(лимит ENI)" : "");
    ++shown;
  }
  if (shown == 0) std::fprintf(stderr, "(все счётчики нулевые)\n");
}

// ---------------------------------------------------------------------
// SenderPort -- TX-only DPDK port + mempool + a table of destinations.
//
// Receivers differ only in the 42 bytes of header in front of an identical
// payload, so each destination owns a prebuilt header template and replication
// is: copy template, patch the three per-packet fields, copy payload.
//
// Each destination also gets a TX queue (`d % n_queues`). One queue per receiver
// makes inter-receiver ordering a hardware property; when the instance offers
// fewer, destinations share and the caller interleaves by message so no receiver
// waits behind another's whole burst.
// ---------------------------------------------------------------------
class SenderPort {
 public:
  static constexpr uint16_t kHeaderLen = static_cast<uint16_t>(
      sizeof(rte_ether_hdr) + sizeof(rte_ipv4_hdr) + sizeof(rte_udp_hdr));

  static constexpr uint32_t kMaxDests = 16;

  static_assert(kHeaderLen + shm::kFrameCap <= RTE_MBUF_DEFAULT_DATAROOM, "frame + L2/L3/L4 headers must fit on mbuf");
  static_assert(kHeaderLen + shm::kFrameCap <= 224, "packet must fit ENA large-LLQ inline entry");

  // `queue_cap` of 0 means "one queue per destination, as many as the device
  // allows" -- the default. A non-zero value caps it, which is what makes the
  // queue count a measurable variable rather than an assumption: the same run
  // can be repeated with every destination sharing one queue.
  SenderPort(const std::string& pool_name, uint32_t nb_mbufs,
             uint32_t cache_size, uint16_t nb_txd,
             const std::vector<DestSpec>& dests, uint32_t src_ip_be,
             uint16_t src_port, uint16_t queue_cap = 0)
      : src_ip_be_(src_ip_be), src_port_(src_port) {
    if (dests.empty() || dests.size() > kMaxDests) {
      std::fprintf(stderr, "SenderPort: need 1..%u destinations, got %zu\n",
                   kMaxDests, dests.size());
      std::exit(1);
    }

    uint16_t wanted_txq = static_cast<uint16_t>(dests.size());
    if (queue_cap != 0 && queue_cap < wanted_txq) wanted_txq = queue_cap;

    // TX only: the sender never receives, so it asks for no RX queues and
    // names no RX descriptor count.
    detail::BringUp b =
        detail::bring_up(pool_name, nb_mbufs, cache_size,
                          /*wanted_rxq=*/0, /*nb_rxd=*/0,
                          /*wanted_txq=*/wanted_txq, /*nb_txd=*/nb_txd);

    port_id_ = b.port_id;
    mempool_ = b.mempool;
    src_mac_ = b.mac;
    tx_ip_cksum_offload_ = b.tx_ip_cksum_offload;
    nb_txq_ = b.nb_txq;
    max_tx_queues_ = b.max_tx_queues;
    n_dests_ = static_cast<uint32_t>(dests.size());
    owns_port_ = true;

    for (uint32_t d = 0; d < n_dests_; ++d) {
      dests_[d].spec = dests[d];
      dests_[d].queue_id = static_cast<uint16_t>(d % nb_txq_);
      build_header_template(d);
    }
  }

  ~SenderPort() { teardown(); }

  uint16_t port_id() const { return port_id_; }
  uint16_t n_queues() const { return nb_txq_; }
  uint16_t max_tx_queues() const { return max_tx_queues_; }

  uint32_t tx_spins() const { return tx_spins_; }
  void set_tx_spins(uint32_t n) { tx_spins_ = n ? n : 1; }
  uint32_t n_dests() const { return n_dests_; }
  uint16_t queue_of(uint32_t dest_idx) const { return dests_[dest_idx].queue_id; }
  const DestSpec& dest(uint32_t dest_idx) const { return dests_[dest_idx].spec; }

  SenderPort(const SenderPort&) = delete;
  SenderPort& operator=(const SenderPort&) = delete;
  SenderPort(SenderPort&&) = delete;
  SenderPort& operator=(SenderPort&&) = delete;

  bool alloc_bulk(rte_mbuf** mbufs, uint32_t count) const noexcept {
    if (count == 0) return true;
    return rte_pktmbuf_alloc_bulk(mempool_, mbufs, count) == 0;
  }

  // Where to write/copy the payload: right after where the Ethernet/IPv4/UDP
  // headers will go once finalize() runs. Nothing is written there yet --
  // callers are expected to read/copy the payload in themselves (e.g.
  // straight from a shm ring slot) before calling finalize().
  uint8_t* payload_ptr(rte_mbuf* m) const {
    return rte_pktmbuf_mtod(m, uint8_t*) + kHeaderLen;
  }
  const uint8_t* payload_ptr(const rte_mbuf* m) const {
    return rte_pktmbuf_mtod(m, const uint8_t*) + kHeaderLen;
  }

  // Pulls a whole outgoing packet into L1 in a writable state, ahead of the
  // code that fills it.
  //
  // All four lines are issued explicitly rather than prefetching lines 0 and 2
  // and leaving the adjacent-line prefetcher to fill in 1 and 3. That trick
  // needs the packet to start on a 128-byte boundary, and here it does so only
  // every other time: the mempool lays objects out at a stride of
  // 64 (header) + 128 (rte_mbuf) + 2176 (buffer) = 2368 bytes, which is 64
  // modulo 128, so consecutive mbufs alternate and for half of them the
  // hardware completes the pair in the wrong direction. Four uops is a cheap
  // price for not depending on that.
  //
  // prefetch0_write, not prefetch0: every one of these lines gets stored to, so
  // the line is wanted in E/M straight away. A read prefetch would land it in S
  // and the first store would still have to pay for the upgrade.
  inline static void prefetch_packet(rte_mbuf* m) {
    static_assert(kHeaderLen + shm::kFrameCap <= 4 * 64,
                  "prefetch_packet covers exactly four cache lines");
    // __builtin_prefetch(p, 1, 3) rather than rte_prefetch0_write(): on x86 the
    // DPDK wrapper is defined as exactly this call, but it carries an
    // "ABI not yet stable" deprecation attribute that fires on every use and
    // buries the real warnings under -Wall -Wextra.
    uint8_t* p = rte_pktmbuf_mtod(m, uint8_t*);
    __builtin_prefetch(p, /*write=*/1, /*locality=*/3);
    __builtin_prefetch(p + 64, 1, 3);
    __builtin_prefetch(p + 128, 1, 3);
    __builtin_prefetch(p + 192, 1, 3);
  }

  // Puts destination `dest_idx`'s headers in front of a payload already sitting
  // at payload_ptr(m). The template carries every field that does not depend on
  // this payload; only three do. Copying the whole template each time (rather
  // than patching in place) is also what resets hdr_checksum to zero, which
  // rte_ipv4_cksum() requires -- mbufs here are reused across iterations, so
  // there is no allocator to do that for us.
  void finalize(rte_mbuf* m, uint32_t dest_idx, uint16_t payload_len) const {
    std::memcpy(rte_pktmbuf_mtod(m, void*), dests_[dest_idx].hdr, kHeaderLen);
    patch_lengths(m, payload_len);
    apply_ip_checksum(m);
  }

  // The fan-out step: `src` already holds a finalized packet for destination 0,
  // `dst` becomes the same payload addressed to `dest_idx`.
  //
  // Copies rather than clones. A clone would segment the mbuf, and a segmented
  // packet no longer fits ENA's large-LLQ inline entry -- the NIC would go back
  // to DMA-reading the body, which is the round trip this design exists to avoid.
  //
  // Fixed kFrameCap, not payload_len: a constant size inlines to vector moves,
  // a variable one is a call into memcpy. The surplus is never transmitted --
  // data_len comes from payload_len.
  void replicate(const rte_mbuf* src, rte_mbuf* dst, uint32_t dest_idx,
                 uint16_t payload_len) const {
    std::memcpy(payload_ptr(dst), payload_ptr(src), shm::kFrameCap);
    finalize(dst, dest_idx, payload_len);
  }

  void discard(rte_mbuf* m) const { rte_pktmbuf_free(m); }
  void free_bulk(rte_mbuf** m, uint32_t n) const { rte_pktmbuf_free_bulk(m, n); }

  // One non-blocking pass at a single queue. The retry policy lives in
  // send_all() below, which needs to see every queue before spinning on any.
  uint16_t tx_burst(uint16_t queue_id, rte_mbuf** pkts, uint16_t n) const noexcept {
    return rte_eth_tx_burst(port_id_, queue_id, pkts, n);
  }

  // What the caller hands send_all(): one entry per queue that has packets.
  struct QueueBatch {
    rte_mbuf** pkts = nullptr;
    uint16_t total = 0;
    uint16_t sent = 0;
  };

  // One pass over every queue before retrying any. A per-queue retry loop would
  // spin on queue 0 while queue 1's packets sit unsent, making one receiver pay
  // for a full ring that is not even its own.
  void send_all(QueueBatch* batches, uint16_t n_batches) const noexcept {
    uint32_t spins = 0;
    for (;;) {
      bool pending = false;
      for (uint16_t q = 0; q < n_batches; ++q) {
        QueueBatch& b = batches[q];
        if (b.sent >= b.total) continue;
        const uint16_t k = tx_burst(static_cast<uint16_t>(q), b.pkts + b.sent,
                                    static_cast<uint16_t>(b.total - b.sent));
        b.sent = static_cast<uint16_t>(b.sent + k);
        if (b.sent < b.total) pending = true;
      }
      if (!pending) return;
      spins += 1;
      if (spins >= tx_spins_) return;
    }
  }

 private:
  struct Dest {
    DestSpec spec;
    uint16_t queue_id = 0;
    alignas(16) uint8_t hdr[kHeaderLen];
  };

  // --- header pointer accessors -- one place that knows the packet layout ---
  static rte_ether_hdr* ethernet_header(rte_mbuf* m) {
    return rte_pktmbuf_mtod(m, rte_ether_hdr*);
  }
  static rte_ipv4_hdr* ipv4_header(rte_mbuf* m) {
    return reinterpret_cast<rte_ipv4_hdr*>(ethernet_header(m) + 1);
  }
  static rte_udp_hdr* udp_header(rte_mbuf* m) {
    return reinterpret_cast<rte_udp_hdr*>(ipv4_header(m) + 1);
  }

  // Built once per destination at construction. Everything that is constant for
  // the life of the process lives here; total_length, dgram_len and the checksum
  // are the only per-packet fields, and they are patched by patch_lengths().
  void build_header_template(uint32_t d) {
    Dest& dst = dests_[d];
    std::memset(dst.hdr, 0, kHeaderLen);

    auto* eth = reinterpret_cast<rte_ether_hdr*>(dst.hdr);
    eth->dst_addr = dst.spec.mac;
    eth->src_addr = src_mac_;
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(eth + 1);
    ip->version_ihl = 0x45;  // IPv4, 20-byte header, no options
    ip->type_of_service = 0;
    ip->total_length = 0;  // per packet
    ip->packet_id = 0;
    ip->fragment_offset = 0;
    ip->time_to_live = 64;
    ip->next_proto_id = IPPROTO_UDP;
    ip->hdr_checksum = 0;  // per packet; must be zero when rte_ipv4_cksum runs
    ip->src_addr = src_ip_be_;
    ip->dst_addr = dst.spec.ip_be;

    auto* udp = reinterpret_cast<rte_udp_hdr*>(ip + 1);
    udp->src_port = rte_cpu_to_be_16(src_port_);
    udp->dst_port = rte_cpu_to_be_16(dst.spec.port);
    udp->dgram_len = 0;    // per packet
    udp->dgram_cksum = 0;  // legal "unchecked" value for UDP over IPv4
  }

  void patch_lengths(rte_mbuf* m, uint16_t payload_len) const {
    const uint16_t udp_len =
        static_cast<uint16_t>(sizeof(rte_udp_hdr) + payload_len);
    const uint16_t ip_len =
        static_cast<uint16_t>(sizeof(rte_ipv4_hdr) + udp_len);

    ipv4_header(m)->total_length = rte_cpu_to_be_16(ip_len);
    udp_header(m)->dgram_len = rte_cpu_to_be_16(udp_len);

    m->data_len = static_cast<uint16_t>(kHeaderLen + payload_len);
    m->pkt_len = m->data_len;
    m->l2_len = sizeof(rte_ether_hdr);
    m->l3_len = sizeof(rte_ipv4_hdr);
  }

  void apply_ip_checksum(rte_mbuf* m) const {
    if (tx_ip_cksum_offload_) {
      m->ol_flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM;
    } else {
      rte_ipv4_hdr* ip = ipv4_header(m);
      ip->hdr_checksum = rte_ipv4_cksum(ip);
    }
  }

  void teardown() {
    if (owns_port_) {
      rte_eth_dev_stop(port_id_);
      rte_eth_dev_close(port_id_);
      // Mempool is intentionally not freed here: rte_mempool_free() would
      // require every mbuf to already be back in the pool, which we can't
      // guarantee mid-teardown. The OS reclaims the hugepage-backed memory
      // on process exit either way.
    }
  }

  rte_mempool* mempool_ = nullptr;

  uint32_t src_ip_be_ = 0;
  uint16_t src_port_ = 0;

  uint16_t port_id_ = 0;
  uint16_t nb_txq_ = 0;
  uint16_t max_tx_queues_ = 0;
  uint32_t n_dests_ = 0;

  rte_ether_addr src_mac_{};
  Dest dests_[kMaxDests]{};

  uint32_t tx_spins_ = kDefaultTxSpins;

  bool tx_ip_cksum_offload_ = false;
  bool owns_port_ = false;
};

// ---------------------------------------------------------------------
// ReceiverPort -- RX-only DPDK port + mempool.
// ---------------------------------------------------------------------
class ReceiverPort {
 public:
  enum class ParseStatus { kOk, kNotUdp, kWrongPort };

  ReceiverPort(const std::string& pool_name, uint32_t nb_mbufs,
               uint32_t cache_size, uint16_t nb_rxd) {
    // RX only: the receiver never transmits, so it asks for no TX queues and
    // names no TX descriptor count.
    detail::BringUp b =
        detail::bring_up(pool_name, nb_mbufs, cache_size,
                          /*wanted_rxq=*/1, /*nb_rxd=*/nb_rxd,
                          /*wanted_txq=*/0, /*nb_txd=*/0);
    port_id_ = b.port_id;
    mempool_ = b.mempool;
    owns_port_ = true;
  }

  ~ReceiverPort() { teardown(); }

  uint16_t port_id() const { return port_id_; }

  ReceiverPort(ReceiverPort&& o) noexcept { *this = std::move(o); }
  ReceiverPort& operator=(ReceiverPort&& o) noexcept {
    if (this != &o) {
      teardown();
      port_id_ = o.port_id_;
      mempool_ = o.mempool_;
      owns_port_ = o.owns_port_;
      o.owns_port_ = false;
    }
    return *this;
  }
  ReceiverPort(const ReceiverPort&) = delete;
  ReceiverPort& operator=(const ReceiverPort&) = delete;

  inline uint16_t receive(rte_mbuf** pkts, uint16_t max_pkts) const {
    return rte_eth_rx_burst(port_id_, 0, pkts, max_pkts);
  }

  // Parses Ethernet/IPv4/UDP headers in place. On kOk, *out_payload points
  // directly into mbuf memory -- no copy happens here, publishing it into
  // the shm ring is the only copy on this side. The caller frees the mbuf
  // once done reading the payload, regardless of the result. Reads as a
  // short checklist: right ethertype? right IP protocol? right port? --
  // each check is its own named helper below.
  static ParseStatus extract_udp_payload(const rte_mbuf* m,
                                          uint16_t want_port_be,
                                          const uint8_t** out_payload,
                                          uint16_t* out_len) {
    const rte_ether_hdr* eth = ethernet_header(m);
    if (!is_ipv4(eth)) return ParseStatus::kNotUdp;

    const rte_ipv4_hdr* ip = ipv4_header(eth);
    if (!is_udp(ip)) return ParseStatus::kNotUdp;

    const rte_udp_hdr* udp = udp_header(ip);
    if (udp->dst_port != want_port_be) return ParseStatus::kWrongPort;

    *out_len = payload_length(udp);
    *out_payload = reinterpret_cast<const uint8_t*>(udp + 1);
    return ParseStatus::kOk;
  }

 private:
  // --- header pointer accessors -- one place that knows the packet layout ---
  static const rte_ether_hdr* ethernet_header(const rte_mbuf* m) {
    return rte_pktmbuf_mtod(m, const rte_ether_hdr*);
  }
  static const rte_ipv4_hdr* ipv4_header(const rte_ether_hdr* eth) {
    return reinterpret_cast<const rte_ipv4_hdr*>(eth + 1);
  }
  static const rte_udp_hdr* udp_header(const rte_ipv4_hdr* ip) {
    const uint8_t ihl_bytes =
        static_cast<uint8_t>((ip->version_ihl & 0x0F) * 4);
    return reinterpret_cast<const rte_udp_hdr*>(
        reinterpret_cast<const uint8_t*>(ip) + ihl_bytes);
  }

  // --- small named predicates/readers, each checks or reads one thing ---
  static bool is_ipv4(const rte_ether_hdr* eth) {
    return eth->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
  }
  static bool is_udp(const rte_ipv4_hdr* ip) {
    return ip->next_proto_id == IPPROTO_UDP;
  }
  static uint16_t payload_length(const rte_udp_hdr* udp) {
    const uint16_t udp_total = rte_be_to_cpu_16(udp->dgram_len);
    return udp_total > sizeof(rte_udp_hdr)
               ? static_cast<uint16_t>(udp_total - sizeof(rte_udp_hdr))
               : 0;
  }

  void teardown() {
    if (owns_port_) {
      rte_eth_dev_stop(port_id_);
      rte_eth_dev_close(port_id_);
    }
  }

  rte_mempool* mempool_ = nullptr;
  uint16_t port_id_ = 0;
  bool owns_port_ = false;
};

}  // namespace dpdk