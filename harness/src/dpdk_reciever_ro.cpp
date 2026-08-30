// dpdk_receiver_ro: то же, что dpdk_reciever, плюс окно переупорядочивания.
//
// Отдельный бинарник, а не флаг основного: путь, на котором сняты все числа в
// RESULTS.md, должен остаться нетронутым побайтово. Здесь --reorder N включает
// окно на N кадров (0 = выключено, поведение основного приёмника).
//
// Зачем. Потребитель обязан видеть строго возрастающий seq_id, и MonotonicGate
// отбрасывает всё, что пришло после уже отданного, -- то есть на переставленном
// пути перестановка превращается в ПОТЕРЮ. Пока путь один, порядок сохраняется
// сам, и окно не нужно. Он ломается там, где путей несколько: L3-маршрут с ECMP
// или поток одного получателя, размазанный по нескольким TX-очередям.
//
// Окно не копирует: паркуется сам mbuf, кадр остаётся там, куда его положила
// карта. Единственная копия на этой стороне -- всё та же публикация в кольцо.
//
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
#include <cstring>

#include <string>
#include <csignal>
#include <cstddef>

#include "dpdk_common.h"
#include "message.h"
#include "shm_ring.h"
#include "shm_segment.h"
#include "util.h"
#include "delivery.h"
#include "reorder.h"

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
  uint16_t mtu = 1500;   // должен быть НЕ МЕНЬШЕ, чем у отправителя
  uint32_t reorder = 0;  // кадров в окне переупорядочивания; 0 = выключено
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
    else if (a == "--mtu") c.mtu = static_cast<uint16_t>(std::stoul(next()));
    else if (a == "--reorder") c.reorder = static_cast<uint32_t>(std::stoul(next()));
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
  uint64_t dgrams = 0;         // датаграмм разобрано
  uint64_t dgram_frames = 0;   // кадров в них -- отношение показывает батчинг
  uint64_t empty_spins = 0;
};

// Отдать в кольцо то, что окно признало готовым. Порядок гарантирован окном,
// поэтому gate здесь только считает: отбрасывать ему уже нечего, кроме
// настоящих опозданий.
inline void publish_ready(const reorder::Frame* ready, uint32_t k,
                           delivery::MonotonicGate& gate, RingHandle& out,
                           Stats& stats) {
  for (uint32_t j = 0; j < k; ++j) {
    if (gate.admit(ready[j].seq)) {
      out.ring.publish(ready[j].payload, ready[j].len);
      ++stats.received;
    }
  }
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

  dpdk::ReceiverPort receiver("receiver_ro_pool", cfg.nb_mbufs, cfg.cache_size,
                               /*nb_rxd=*/cfg.rxd, cfg.mtu);

  RingHandle out = create_output_ring(cfg.shm_name, cfg.slots);

  std::fprintf(stderr, "dpdk_receiver_ro: listening udp/%u -> shm=%s\n",
               cfg.udp_port, cfg.shm_name.c_str());

  const uint16_t want_port_be = rte_cpu_to_be_16(cfg.udp_port);

  Stats stats;
  delivery::MonotonicGate gate;

  reorder::Window window(cfg.reorder);
  // +1: закрывшаяся дыра выпускает всю накопленную за ней цепочку плюс сам кадр.
  reorder::Frame ready[reorder::kMaxCapacity + 1];
  if (window.capacity() > 0) {
    std::fprintf(stderr, "dpdk_receiver_ro: окно переупорядочивания на %u кадров\n",
                 window.capacity());
  }

  rte_mbuf* burst[64];

  while (!g_stop) {
    uint16_t n = receiver.receive(burst, cfg.burst);
    // t2, one clock read per burst: every packet in it was already DMA'd
    // before receive() returned, so a per-packet stamp would only be timing
    // our own parse loop below.
    const uint64_t rx_ts = util::now_ns();
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
        // A datagram carries one or more frames back to back. Their boundaries
        // come from frame_size(type) in each frame's own header, so the sender
        // adds no length prefix and the unbatched case walks exactly one frame.
        //
        // rx_ts is the same for every frame here, and correctly so: they arrived
        // in one datagram, at one instant. Splitting them apart is this loop's
        // work, and timing our own loop is not what segment B measures.
        uint16_t off = 0;
        uint16_t frames = 0;
        while (off + sizeof(msg::Header) <= payload_len) {
          uint8_t* f = const_cast<uint8_t*>(payload) + off;
          const uint32_t fsz = msg::frame_size(f[offsetof(msg::Header, type)]);
          // Unknown type, or a frame the datagram cannot hold: the rest of this
          // payload cannot be walked, so stop rather than guess an offset.
          if (fsz == 0 || off + fsz > payload_len) {
            ++stats.bad_len;
            break;
          }
          uint64_t seq_id = 0;
          std::memcpy(&seq_id, f + offsetof(msg::Header, seq_id), sizeof(seq_id));
          // Штамп ставится ДО окна: rx_ts -- это момент, когда кадр пришёл с
          // провода, а не когда окно решило его выпустить. Иначе отрезок b
          // включал бы ожидание переупорядочивания, которое к проводу
          // отношения не имеет.
          std::memcpy(f + offsetof(msg::Header, rx_ts_ns), &rx_ts, sizeof(rx_ts));
          const uint32_t k = window.offer({burst[i], f, static_cast<uint16_t>(fsz), seq_id},
                                          ready);
          publish_ready(ready, k, gate, out, stats);
          off = static_cast<uint16_t>(off + fsz);
          ++frames;
        }
        if (frames == 0) [[unlikely]] ++stats.bad_len;
        stats.dgrams += 1;
        stats.dgram_frames += frames;
      } else if (status == dpdk::ReceiverPort::ParseStatus::kWrongPort) {
        ++stats.wrong_port;
      } else {
        ++stats.non_udp;
      }
    }
    rte_pktmbuf_free_bulk(burst, n);
  }

  // Слить окно: придержанные кадры не должны пропасть молча при Ctrl+C.
  publish_ready(ready, window.drain(ready), gate, out, stats);

  const delivery::MonotonicGate::Stats& gate_stats = gate.stats();
  // До выхода из main: деструктор порта делает dev_stop/close.
  dpdk::print_port_stats(receiver.port_id());

  const uint64_t polls = stats.bursts;
  const uint64_t polls_hit = polls - stats.empty_spins;
  const uint64_t pkt_parsed = stats.dgrams + stats.wrong_port + stats.non_udp;

  dpdk::stats::title("dpdk_receiver_ro");

  dpdk::stats::section("настройка");
  {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(receiver.mtu()));
    dpdk::stats::text("mtu", buf, "должен быть не меньше, чем у отправителя");
    std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(cfg.burst));
    dpdk::stats::text("burst", buf, "пакетов за один rte_eth_rx_burst");
  }

  dpdk::stats::section("объём");
  dpdk::stats::count("pkt_recv", stats.burst_pkts, "пакетов отдала карта");
  dpdk::stats::count("dgram_ok", stats.dgrams, "наш порт, заголовки разобраны");
  dpdk::stats::count("frame_out", stats.dgram_frames, "кадров извлечено из них");
  dpdk::stats::count("msg_published", stats.received, "опубликовано в кольцо после gate");
  dpdk::stats::count_pct("wrong_port", stats.wrong_port, pkt_parsed, "udp, но не наш порт");
  dpdk::stats::count_pct("non_udp", stats.non_udp, pkt_parsed, "не ipv4/udp вовсе");
  dpdk::stats::count("bad_len", stats.bad_len, "наш порт, но границы кадров не разобрать");

  dpdk::stats::section("за проход");
  dpdk::stats::count("polls", polls, "всего обращений к rte_eth_rx_burst");
  dpdk::stats::count_pct("polls_empty", stats.empty_spins, polls, "вернули ноль пакетов");
  dpdk::stats::ratio("pkt/poll", stats.burst_pkts, polls, "по ВСЕМ опросам, с пустыми");
  dpdk::stats::ratio_of("pkt/poll_hit", stats.burst_pkts, polls_hit, cfg.burst,
                         "только по непустым; предел --burst");
  dpdk::stats::ratio("frame/dgram", stats.dgram_frames, stats.dgrams,
                      "1.00 = отправитель не батчил");

  if (window.capacity() > 0) {
    const reorder::Window::Stats& w = window.stats();
    dpdk::stats::section("окно переупорядочивания");
    dpdk::stats::count("in_order", w.in_order, "прошло насквозь, ничего не ждало");
    dpdk::stats::count("parked", w.parked, "придержано до заполнения дыры");
    dpdk::stats::count("recovered", w.recovered, "спасено -- без окна было бы потерей");
    dpdk::stats::count("forced", w.forced, "вытеснено из полного окна, разрыв признан");
    dpdk::stats::count("late", w.late, "пришло позже отданного, окно не помогло");
    dpdk::stats::ratio_of("peak", w.peak, 1, window.capacity(), "занятость окна");
  }

  dpdk::stats::section("gate (монотонность seq_id)");
  dpdk::stats::count("delivered", gate_stats.delivered, "seq_id больше предыдущего");
  dpdk::stats::count("suppressed", gate_stats.suppressed, "дубль или отставший -- отброшен");
  dpdk::stats::count("gaps", gate_stats.gaps, "разрывов последовательности");
  dpdk::stats::count("missing", gate_stats.missing, "сообщений в этих разрывах");
  std::fprintf(stderr, "\n");

  return 0;
}