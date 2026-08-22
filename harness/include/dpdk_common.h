// dpdk_port.h
//
// OOP wrapper around the DPDK port lifecycle for the sender/receiver pair.
// SenderPort owns a TX-only queue, ReceiverPort an RX-only queue; both share
// the same bring-up sequence (mempool -> configure -> queue setup -> start)
// through a private helper so it's written once, not duplicated per class.
//
// SenderPort::payload_ptr()/finalize() are split on purpose: the caller
// (dpdk_sender.cpp) reads the shm-ring frame directly into mbuf memory at
// payload_ptr(), so there is exactly one copy from the ring to the wire --
// not a copy into a scratch buffer followed by a second copy into the mbuf.
#pragma once

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_udp.h>

#include <netinet/in.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "shm_ring.h"

namespace dpdk {

inline constexpr uint32_t kMaxTxSpins = 10;
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

namespace detail {

struct BringUp {
  uint16_t port_id;
  rte_mempool* mempool;
  rte_ether_addr mac;
  bool tx_ip_cksum_offload;
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

inline bool supports_ip_cksum_offload(const rte_eth_dev_info& info) {
  return (info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM) != 0;
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

inline void configure_device(uint16_t port_id, bool need_rxq, bool need_txq,
                              bool ip_cksum_offload) {
  rte_eth_conf eth_conf{};
  if (ip_cksum_offload) {
    eth_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_IPV4_CKSUM;
  }
  int ret = rte_eth_dev_configure(port_id, need_rxq ? 1 : 0, need_txq ? 1 : 0,
                                   &eth_conf);
  if (ret != 0) {
    std::fprintf(stderr, "rte_eth_dev_configure failed: %s\n",
                 rte_strerror(-ret));
    std::exit(1);
  }
}

inline void adjust_descriptor_counts(uint16_t port_id, uint16_t* nb_rxd,
                                      uint16_t* nb_txd) {
  int ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, nb_rxd, nb_txd);
  if (ret != 0) {
    std::fprintf(stderr, "rte_eth_dev_adjust_nb_rx_tx_desc failed: %s\n",
                 rte_strerror(-ret));
    std::exit(1);
  }
}

inline void setup_rx_queue(uint16_t port_id, uint16_t nb_rxd,
                            rte_mempool* pool) {
  int ret = rte_eth_rx_queue_setup(port_id, 0, nb_rxd, rte_socket_id(),
                                    nullptr, pool);
  if (ret != 0) {
    std::fprintf(stderr, "rte_eth_rx_queue_setup failed: %s\n",
                 rte_strerror(-ret));
    std::exit(1);
  }
}

inline void setup_tx_queue(uint16_t port_id, uint16_t nb_txd,
                            const rte_eth_dev_info& dev_info,
                            bool ip_cksum_offload) {
  rte_eth_txconf txconf = dev_info.default_txconf;
  txconf.offloads = ip_cksum_offload ? RTE_ETH_TX_OFFLOAD_IPV4_CKSUM : 0;
  int ret =
      rte_eth_tx_queue_setup(port_id, 0, nb_txd, rte_socket_id(), &txconf);
  if (ret != 0) {
    std::fprintf(stderr, "rte_eth_tx_queue_setup failed: %s\n",
                 rte_strerror(-ret));
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
inline BringUp bring_up(const std::string& pool_name, uint32_t nb_mbufs,
                         uint32_t cache_size, uint16_t nb_rxd, uint16_t nb_txd,
                         bool need_rxq [[maybe_unused]], bool need_txq [[maybe_unused]]) {
  BringUp b{};
  b.port_id = find_first_port();

  const rte_eth_dev_info dev_info = query_dev_info(b.port_id);
  b.tx_ip_cksum_offload = need_txq && supports_ip_cksum_offload(dev_info);

  b.mempool = create_mbuf_pool(pool_name, nb_mbufs, cache_size);

  configure_device(b.port_id, /*need_rxq*/ true, /*need_txq*/ true, b.tx_ip_cksum_offload);
  adjust_descriptor_counts(b.port_id, &nb_rxd, &nb_txd);

  /*if (need_rxq)*/ setup_rx_queue(b.port_id, nb_rxd, b.mempool);
  /*if (need_txq)*/ setup_tx_queue(b.port_id, nb_txd, dev_info, b.tx_ip_cksum_offload);

  start_device(b.port_id);
  b.mac = read_mac_address(b.port_id);

  std::fprintf(stderr, "dpdk: port %u up (ip_cksum_hw=%d)\n", b.port_id,
               b.tx_ip_cksum_offload);
  return b;
}

}  // namespace detail

// ---------------------------------------------------------------------
// SenderPort -- TX-only DPDK port + mempool + a fixed UDP/IPv4 header
// template (peer MAC/IP/ports are set once at construction).
// ---------------------------------------------------------------------
class SenderPort {
 public:
  static constexpr uint16_t kHeaderLen = static_cast<uint16_t>(
      sizeof(rte_ether_hdr) + sizeof(rte_ipv4_hdr) + sizeof(rte_udp_hdr));

  static_assert(kHeaderLen + shm::kFrameCap <= RTE_MBUF_DEFAULT_DATAROOM, "frame + L2/L3/L4 headers must fit on mbuf");

  SenderPort(const std::string& pool_name, uint32_t nb_mbufs,
             uint32_t cache_size, uint16_t nb_txd,
             const rte_ether_addr& dst_mac, uint32_t src_ip_be,
             uint32_t dst_ip_be, uint16_t src_port, uint16_t dst_port)
      : src_ip_be_(src_ip_be),
        dst_ip_be_(dst_ip_be),
        src_port_(src_port),
        dst_port_(dst_port),
        dst_mac_(dst_mac) {
    detail::BringUp b =
        detail::bring_up(pool_name, nb_mbufs, cache_size, /*nb_rxd=*/0,
                          nb_txd, /*need_rxq=*/false, /*need_txq=*/true);


    port_id_ = b.port_id;
    mempool_ = b.mempool;
    src_mac_ = b.mac;
    tx_ip_cksum_offload_ = b.tx_ip_cksum_offload;
    owns_port_ = true;
  }

  ~SenderPort() { teardown(); }

  SenderPort(SenderPort&& o) noexcept { *this = std::move(o); }
  SenderPort& operator=(SenderPort&& o) noexcept {
    if (this != &o) {
      teardown();
      port_id_ = o.port_id_;
      mempool_ = o.mempool_;
      src_mac_ = o.src_mac_;
      dst_mac_ = o.dst_mac_;
      src_ip_be_ = o.src_ip_be_;
      dst_ip_be_ = o.dst_ip_be_;
      src_port_ = o.src_port_;
      dst_port_ = o.dst_port_;
      tx_ip_cksum_offload_ = o.tx_ip_cksum_offload_;
      owns_port_ = o.owns_port_;
      o.owns_port_ = false;
    }
    return *this;
  }
  SenderPort(const SenderPort&) = delete;
  SenderPort& operator=(const SenderPort&) = delete;

  // One mbuf from the pool. nullptr if the pool is momentarily empty.
  rte_mbuf* alloc() const noexcept { return rte_pktmbuf_alloc(mempool_); }


  bool alloc_bulk(rte_mbuf** mbufs, uint32_t count) const noexcept {
    int ret = rte_pktmbuf_alloc_bulk(mempool_, mbufs, count);
    return ret == 0;
  }

  // Where to write/copy the payload: right after where the Ethernet/IPv4/UDP
  // headers will go once finalize() runs. Nothing is written there yet --
  // callers are expected to read/copy the payload in themselves (e.g.
  // straight from a shm ring slot) before calling finalize().
  uint8_t* payload_ptr(rte_mbuf* m) const {
    return rte_pktmbuf_mtod(m, uint8_t*) + kHeaderLen;
  }

  // Writes L2/L3/L4 headers in front of a payload already sitting at
  // payload_ptr(m), and sets the mbuf's length fields / checksum offload
  // flags accordingly. Reads top-to-bottom as: header, header, header,
  // lengths, checksum -- each step is its own small function below.
  void finalize(rte_mbuf* m, uint16_t payload_len) const {
    const uint16_t udp_len =
        static_cast<uint16_t>(sizeof(rte_udp_hdr) + payload_len);
    const uint16_t ip_len =
        static_cast<uint16_t>(sizeof(rte_ipv4_hdr) + udp_len);

    write_ethernet_header(m);
    write_ipv4_header(m, ip_len);
    write_udp_header(m, udp_len);
    set_mbuf_lengths(m, payload_len);
    apply_ip_checksum(m);
  }

  // Give back an mbuf that ended up unused (e.g. the ring had nothing new).
  void discard(rte_mbuf* m) const { rte_pktmbuf_free(m); }

  void free_bulk(rte_mbuf**m, uint32_t n) const {rte_pktmbuf_free_bulk(m,n); }

  // Sends batch and retry kMaxTsSpins times if not hole batch sent
  inline uint32_t send_with_retry(rte_mbuf** pkts, uint32_t n) const noexcept {
    uint32_t sent = 0, spins = 0;
    while (sent < n && spins <  kMaxTxSpins) {
      uint16_t k = rte_eth_tx_burst(port_id_, 0, pkts + sent, n - sent);
      sent += k;
      spins = k ? 0 : spins + 1;
    }
    return sent;
  }

 private:
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

  // --- one function per header, each only touches its own fields ---
  void write_ethernet_header(rte_mbuf* m) const {
    rte_ether_hdr* eth = ethernet_header(m);
    eth->dst_addr = dst_mac_;
    eth->src_addr = src_mac_;
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
  }

  void write_ipv4_header(rte_mbuf* m, uint16_t ip_len) const {
    rte_ipv4_hdr* ip = ipv4_header(m);
    ip->version_ihl = 0x45;  // IPv4, 20-byte header, no options
    ip->type_of_service = 0;
    ip->total_length = rte_cpu_to_be_16(ip_len);
    ip->packet_id = 0;
    ip->fragment_offset = 0;
    ip->time_to_live = 64;
    ip->next_proto_id = IPPROTO_UDP;
    ip->hdr_checksum = 0;  // filled in by apply_ip_checksum()
    ip->src_addr = src_ip_be_;
    ip->dst_addr = dst_ip_be_;
  }

  void write_udp_header(rte_mbuf* m, uint16_t udp_len) const {
    rte_udp_hdr* udp = udp_header(m);
    udp->src_port = rte_cpu_to_be_16(src_port_);
    udp->dst_port = rte_cpu_to_be_16(dst_port_);
    udp->dgram_len = rte_cpu_to_be_16(udp_len);
    udp->dgram_cksum = 0;  // legal "unchecked" value for UDP over IPv4
  }

  void set_mbuf_lengths(rte_mbuf* m, uint16_t payload_len) const {
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
  uint32_t dst_ip_be_ = 0;
  uint16_t src_port_ = 0;
  uint16_t dst_port_ = 0;

  uint16_t port_id_ = 0;

  rte_ether_addr src_mac_{};
  rte_ether_addr dst_mac_{};

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
    detail::BringUp b =
        detail::bring_up(pool_name, nb_mbufs, cache_size, nb_rxd,
                          /*nb_txd=*/0, /*need_rxq=*/true, /*need_txq=*/false);
    port_id_ = b.port_id;
    mempool_ = b.mempool;
    owns_port_ = true;
  }

  ~ReceiverPort() { teardown(); }

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