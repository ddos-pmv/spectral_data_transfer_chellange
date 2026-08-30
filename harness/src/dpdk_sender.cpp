// dpdk_sender: reads frames from the producer's shared-memory ring and
// transmits them as raw UDP/IPv4/Ethernet packets to every receiver in the
// destination table.
//
// Fan-out is source replication because a VPC does not forward IP multicast
// between instances. The table is written so one row holding a group address is
// a genuine multicast send wherever the fabric does replicate; with N unicast
// rows the sender pays N packets per message, and that ceiling is what the
// per-queue counters at exit are for.
//
// One thread. Replication multiplies the per-packet cost but not the per-burst
// cost: the shm ring is read once per message however many receivers there are.
// A second core would not reduce the packet count, which is the actual limit.
//
// Never auto-exits on idle: it is transport infrastructure, not a reporting
// tool. Stop it with Ctrl+C / SIGTERM.
//
// Usage:
//   dpdk_sender [EAL args] -- --shm NAME --slots N --src-ip IP
//               --dest MAC@IP:PORT [--dest ...]
//               [--src-port P] [--burst N] [--pack N] [--min-pack N]
//               [--pack-wait-spins N] [--pack-backoff N]
//               [--mbufs N] [--txd N] [--count N]
//               [--tx-queues N] [--tx-spins N]
//
// Две независимые границы прохода:
//   --burst N -- сколько ДАТАГРАММ уходит в карту за один rte_eth_tx_burst.
//   --pack N  -- сколько СООБЩЕНИЙ едет в одной датаграмме.
// То есть за проход до burst*pack сообщений. Приёмник разбирает кадры по
// frame_size(type) из заголовка каждого, так что на провод не добавляется ни
// байта служебных данных.
//
// Батчинг адаптивный и порога не имеет: укладывается ровно столько, сколько
// кольцо отдало за проход. Пока отправитель успевает, датаграмма выходит
// однокадровой -- байт в байт та же, что при --pack 1, и задержка за батчинг
// не платит ничего. Кадры копятся только когда TX-кольцо перестаёт принимать,
// и тогда число пакетов падает до N раз -- а шейпер среды считает именно
// пакеты. Цена: датаграмма из двух и более кадров не помещается в 224-байтную
// inline-запись large LLQ, и карта снова читает тело через DMA.
//
// --min-pack N включает ПРИНУДИТЕЛЬНЫЙ батчинг: не отправлять датаграмму короче
// N кадров, даже если кольцо пусто. Это ровно то ожидание, которого адаптивный
// режим избегает; включать имеет смысл, только когда пакеты дороже микросекунд.
// Ждать бесконечно нельзя: пауза в потоке становится задержкой первого кадра
// один к одному, а паузы редки и потому попадают прямо в p99.9 и p99.99.
//
// Предел -- счётчик холостых опросов кольца, --pack-wait-spins, один на весь
// проход. Оборот -- чтение пустого слота, стоимость почти постоянная (замерено
// ~0.7 нс), так что счётчик оборотов и есть ограничение по времени в других
// единицах; clock_gettime на каждом обороте обошёлся бы дороже ожидания.
// Выбирать надо от темпа: добрать N кадров при R сообщений в секунду занимает
// ~N/R секунд, то есть при 400k msg/s четыре кадра -- ~7.5 мкс, ~10000
// оборотов. Меньший предел просто никогда не даст добрать -- это покажет
// pack_timeout.
//
// --pack-backoff N -- холостых оборотов МЕЖДУ опросами кольца, пока идёт добор.
// 0 (по умолчанию) -- опрашивать вплотную, путь тот же, что был.
//
// Смысл не в экономии тактов. Повторные acquire-load слота, лежащего в S,
// попадают в L1 и когерентного трафика не создают; платится в момент, когда
// producer делает release-store: линия инвалидируется, а у нас в полёте десятки
// спекулятивных загрузок именно её -- это memory-ordering machine clear, сброс
// конвейера. Реже опрашиваем -- меньше загрузок в полёте к этому моменту.
//
// Отправку задержка не сдвигает: момент отправки определяет producer, выдавший
// последний кадр, а не то, когда мы этот кадр заметили. Пока задержка короче
// наполнения (~9 мкс при min_pack 8 и 800k msg/s), она бесплатна. Поэтому она и
// стоит только здесь, а не в общей ветке пустого кольца.
//
// Оборот замерен в 0.51 нс. Учтите, что он растягивает и бюджет ожидания:
// pack_wait_spins считает итерации, а итерация теперь стоит ~2.2 + 0.51*backoff нс.
//
// Без --min-pack (по умолчанию) ожидания нет вовсе.
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
  uint32_t pack = 1;       // предел кадров в датаграмме; 1 = каждое сообщение своей
  uint32_t min_pack = 1;   // НЕ отправлять датаграмму короче; 1 = не ждать
  uint32_t pack_wait_spins = 10000;  // холостых опросов кольца в ожидании добора
  uint32_t pack_backoff = 0;   // холостых оборотов МЕЖДУ опросами; 0 = выключено
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
  uint16_t mtu = 1500;     // 9001 на ENA внутри VPC -- до 50 кадров в датаграмме
  uint16_t tx_queues = 0;  // 0 = one per destination, capped by the device
  uint32_t tx_spins = dpdk::kDefaultTxSpins;

  // Every receiver, in the order given. One receiver is not a special case --
  // it is a table with one row, which is why there is no second addressing form
  // here and no branch on the count anywhere downstream.
  std::vector<dpdk::DestSpec> dests;
};

// An unsigned argument, or a clean exit. from_chars reports a bad value instead
// of throwing the way stoul does, and the trailing-character check rejects
// "40001x", which stoul would happily accept.
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
    else if (a == "--mtu") c.mtu = static_cast<decltype(c.mtu)>(parse_uint("--mtu", next(), 9216));
    else if (a == "--tx-queues") c.tx_queues = static_cast<decltype(c.tx_queues)>(parse_uint("--tx-queues", next(), 65535));
    else if (a == "--tx-spins") c.tx_spins = parse_uint("--tx-spins", next(), 1000);
    else if (a == "--pack") c.pack = static_cast<decltype(c.pack)>(
        parse_uint("--pack", next(), dpdk::SenderPort::kMaxPack));
    else if (a == "--min-pack") c.min_pack = static_cast<decltype(c.min_pack)>(
        parse_uint("--min-pack", next(), dpdk::SenderPort::kMaxPack));
    else if (a == "--pack-backoff") c.pack_backoff =
        static_cast<decltype(c.pack_backoff)>(
            parse_uint("--pack-backoff", next(), 100000));
    else if (a == "--pack-wait-spins") c.pack_wait_spins =
        static_cast<decltype(c.pack_wait_spins)>(
            parse_uint("--pack-wait-spins", next(), 10000000));
    else {
      std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }

  if (c.pack < 1) {
    std::fprintf(stderr, "--pack must be at least 1\n");
    std::exit(2);
  }
  if (c.min_pack < 1) c.min_pack = 1;
  if (c.min_pack > c.pack) {
    std::fprintf(stderr, "--min-pack %u exceeds --pack %u: нечего добирать\n",
                 c.min_pack, c.pack);
    std::exit(2);
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

// bufs[d][i] is destination d's packet for message i. A destination's packets
// are contiguous, so every destination hands its array straight to tx_burst --
// no gather buffer and no interleaving, whether or not it shares a queue.
rte_mbuf* g_bufs[kMaxDests][kMaxBurst];

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
                          cfg.src_port, cfg.tx_queues, cfg.mtu);

  // Зажимаем ПОСЛЕ старта порта: карта могла дать MTU меньше запрошенного, и
  // тогда --pack, законный на бумаге, порождал бы пакеты, которые она отбросит.
  const uint16_t pack_cap = sender.max_pack();
  if (cfg.pack > pack_cap) {
    std::fprintf(stderr, "--pack %u не влезает в MTU %u -- берём %u\n",
                 cfg.pack, static_cast<unsigned>(sender.mtu()), pack_cap);
    cfg.pack = pack_cap;
    if (cfg.min_pack > cfg.pack) cfg.min_pack = cfg.pack;
  }

  sender.set_tx_spins(cfg.tx_spins);

  const uint32_t n_dests = sender.n_dests();
  const uint16_t n_queues = sender.n_queues();
  check_mempool_size(cfg, n_dests, n_queues);

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
  uint64_t dgrams = 0, dgram_msgs = 0, bad_frames = 0, pack_timeouts = 0;
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
    //
    // Проход собирает до cfg.burst датаграмм по cfg.pack кадров. Состояние
    // текущей датаграммы живёт между витками, и закрывается она там, где это
    // выясняется: набралась до pack, либо кольцо иссякло.
    uint32_t n = 0;        // датаграмм собрано
    uint32_t n_msgs = 0;   // сообщений в них
    uint16_t used = 0;     // байт в текущей датаграмме
    uint16_t frames = 0;   // кадров в ней
    uint32_t wait_spins = 0;

    // Место под ещё один кадр держится инвариантом frames < cfg.pack на входе в
    // чтение: датаграмма закрывается сразу по достижении предела, поэтому
    // used <= (pack-1)*kFrameCap, и после записи <= pack*kFrameCap = kMaxPayload.
    auto close_dgram = [&]() {
      lens[n] = used;
      sender.finalize(g_bufs[0][n], /*dest_idx=*/0, used);
      ++n;
      dgram_msgs += frames;
      used = 0;
      frames = 0;
      // n может дойти ровно до cfg.burst -- тогда цикл кончился, и предвыбирать
      // нечего: g_bufs[0][cfg.burst] уже за границей занятой части.
      if (n < cfg.burst) dpdk::SenderPort::prefetch_packet(g_bufs[0][n]);
    };

    while (n < cfg.burst) {
      uint32_t len = 0;
      uint64_t resume = 0;
      const auto st =
          ring.read(read_index, sender.payload_ptr(g_bufs[0][n]) + used, &len, &resume);

      if (st == shm::Ring::FrameStatus::kOk) {
        used = static_cast<uint16_t>(used + len);
        ++frames;
        ++n_msgs;
        ++read_index;
        if (frames == cfg.pack) close_dgram();
        continue;
      }
      if (st == shm::Ring::FrameStatus::kLapped) {
        ++lapped;
        read_index = resume;
        continue;
      }

      // kEmpty: кольцо пусто. Без --min-pack отправляем что набралось -- именно
      // это и есть отказ платить задержкой за батчинг.
      ++empty_spins;
      if (frames > 0 && frames < cfg.min_pack && !g_stop) {
        if (++wait_spins < cfg.pack_wait_spins) {
          // Ждать мы всё равно будем: момент отправки задаёт producer, а не то,
          // когда мы заметим кадр. Пока задержка короче наполнения, она не
          // сдвигает отправку вовсе -- и только здесь она поэтому и уместна.
          dpdk::idle_spin(cfg.pack_backoff);
          continue;
        }
        ++pack_timeouts;   // предел вышел, а кадров недобрали
      }
      break;
    }
    if (frames > 0) close_dgram();   // недобранный хвост уходит как есть

    if (n == 0) continue;
    ++bursts;
    burst_msgs += n_msgs;
    msgs += n_msgs;
    dgrams += n;

    // --- stamp, then replicate --------------------------------------------
    const uint64_t tx_ts = util::now_ns();
    for (uint32_t i = 0; i < n; ++i) {
      // Штамп нужен в каждом кадре датаграммы, а границы кадров восстанавливаются
      // по frame_size(type) -- ровно так же, как их разбирает приёмник.
      uint8_t* base = sender.payload_ptr(g_bufs[0][i]);
      for (uint16_t off = 0; off + sizeof(msg::Header) <= lens[i]; ) {
        std::memcpy(base + off + offsetof(msg::Header, tx_ts_ns),
                    &tx_ts, sizeof(tx_ts));
        const uint32_t fsz = msg::frame_size(base[off + offsetof(msg::Header, type)]);
        // 0 на неизвестном типе: прибавить его -- вечный цикл, не испорченный кадр.
        if (fsz == 0) [[unlikely]] { ++bad_frames; break; }
        off = static_cast<uint16_t>(off + fsz);
      }
      // The replication targets are the cold memory here: unlike the source, which
      // this loop rereads n_dests times and which stays in L1, each is a fresh set
      // of lines to take ownership of. Only the bodies are prefetched -- the mbuf
      // structs are already hot from alloc_bulk's reset at the bottom of the loop.
      const bool more = i + 1 < n;
      for (uint32_t d = 1; d < n_dests; ++d) {
        if (more) dpdk::SenderPort::prefetch_packet(g_bufs[d][i + 1]);
        sender.replicate(g_bufs[0][i], g_bufs[d][i], d, lens[i]);
      }
    }

    // --- hand every destination its packets --------------------------------
    // Destination-major: one batch per destination, its own array, in order.
    // Nothing is gathered and nothing is interleaved, so the burst handed to the
    // card is the same array replicate() just filled. When destinations share a
    // queue this puts dest 0's whole burst ahead of dest 1's instead of
    // alternating; the last message waits behind the same number of packets
    // either way, and bursts only grow when the ring is already backing up.
    for (uint32_t d = 0; d < n_dests; ++d) {
      batches[d].pkts = g_bufs[d];
      batches[d].queue_id = sender.queue_of(d);
      batches[d].total = static_cast<uint16_t>(n);
      batches[d].sent = 0;
    }

    sender.send_all(batches, static_cast<uint16_t>(n_dests));

    // --- account and refill -------------------------------------------------
    // tx_burst consumes a prefix: g_bufs[d][0..sent_d) are dead pointers (the PMD
    // freed them) and get replaced. The unsent tail keeps its mbufs and is simply
    // overwritten with new frames next iteration.
    bool refill_ok = true;
    for (uint32_t d = 0; d < n_dests; ++d) {
      const uint32_t sent_d = batches[d].sent;
      dstats[d].sent += sent_d;
      dstats[d].dropped += n - sent_d;
      if (!sender.alloc_bulk(g_bufs[d], sent_d)) [[unlikely]] {
        ++alloc_fail;
        refill_ok = false;
      }
    }
    if (!refill_ok) break;
  }

  // Фактический темп, а не заказанный: из кольца читается одно количество, а на
  // провод уходит меньше на tx_dropped. С pps_allowance_exceeded сопоставимо
  // только отправленное -- предложенного карта не видела.
  const double elapsed_s =
      static_cast<double>(util::now_ns() - t_start) / 1e9;
  uint64_t total_sent = 0, total_dropped = 0;
  for (uint32_t d = 0; d < n_dests; ++d) {
    total_sent += dstats[d].sent;
    total_dropped += dstats[d].dropped;
  }

  // До выхода из main: деструктор порта делает dev_stop/close.
  dpdk::print_port_stats(sender.port_id());

  const uint64_t ring_polls = msgs + empty_spins + lapped;
  const uint64_t pkt_offered = static_cast<uint64_t>(dgrams) * n_dests;

  dpdk::stats::title("dpdk_sender");

  dpdk::stats::section("настройка");
  {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%u / %u очередей", n_dests,
                  static_cast<unsigned>(n_queues));
    dpdk::stats::text("dests", buf, "получателей и TX-очередей под них");
    std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(sender.mtu()));
    char note[64];
    std::snprintf(note, sizeof(note), "на нём помещается до %u кадров",
                  static_cast<unsigned>(sender.max_pack()));
    dpdk::stats::text("mtu", buf, note);
    std::snprintf(buf, sizeof(buf), "%u x %u = %u", cfg.burst, cfg.pack,
                  cfg.burst * cfg.pack);
    dpdk::stats::text("burst x pack", buf, "датаграмм x кадров = сообщений за проход");
    std::snprintf(buf, sizeof(buf), "%u", cfg.min_pack);
    dpdk::stats::text("min_pack", buf,
                      cfg.min_pack > 1 ? "кадров ждать перед отправкой"
                                       : "1 = добора не ждём вовсе");
    std::snprintf(buf, sizeof(buf), "%u", cfg.pack_backoff);
    dpdk::stats::text("pack_backoff", buf,
                      cfg.pack_backoff ? "холостых оборотов между опросами кольца"
                                       : "0 = опрашиваем кольцо вплотную");
  }

  dpdk::stats::section("объём");
  dpdk::stats::count("msg_read", msgs, "сообщений прочитано из кольца");
  dpdk::stats::count("dgram_built", dgrams, "датаграмм собрано");
  dpdk::stats::count("pkt_offered", pkt_offered, "= dgram_built x dests");
  dpdk::stats::count("pkt_sent", total_sent, "ПРИНЯТО картой (только это сравнимо с pps_allowance)");
  dpdk::stats::count_pct("pkt_dropped", total_dropped, pkt_offered,
                          "TX-кольцо было полно");

  dpdk::stats::section("за проход");
  dpdk::stats::count("passes", bursts, "оборотов главного цикла, что-то отправивших");
  dpdk::stats::ratio_of("msg/pass", burst_msgs, bursts,
                         static_cast<uint64_t>(cfg.burst) * cfg.pack, "предел burst x pack");
  dpdk::stats::ratio_of("dgram/pass", dgrams, bursts, cfg.burst, "предел --burst");
  dpdk::stats::ratio_of("frame/dgram", dgram_msgs, dgrams, cfg.pack,
                         "предел --pack; 1.00 = батчинг не включался");

  dpdk::stats::section("опросы кольца");
  dpdk::stats::count("ring_polls", ring_polls, "всего обращений к ring.read()");
  dpdk::stats::count_pct("ring_empty", empty_spins, ring_polls, "вернули kEmpty");
  dpdk::stats::count("ring_lapped", lapped, "producer обогнал нас, кадры пропущены");
  dpdk::stats::count("pack_timeout", pack_timeouts, "проходов, где добор не успел за предел");
  dpdk::stats::count("bad_frames", bad_frames, "неизвестный type при разборе границ кадров");
  dpdk::stats::count("alloc_fail", alloc_fail, "пул mbuf опустел");

  dpdk::stats::section("темп");
  {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f s", elapsed_s);
    dpdk::stats::text("elapsed", buf, "");
  }
  dpdk::stats::rate("msg_in", msgs, elapsed_s, "msg/s", "прочитано из кольца");
  dpdk::stats::rate("dgram_out", dgrams, elapsed_s, "dgram/s", "= msg_in / frame_dgram");
  dpdk::stats::rate("pkt_out", total_sent, elapsed_s, "pkt/s", "ушло в карту");

  dpdk::stats::section("по получателям");
  for (uint32_t d = 0; d < n_dests; ++d) {
    char key[32], note[96];
    std::snprintf(key, sizeof(key), "dest %u (q%u)", d,
                  static_cast<unsigned>(sender.queue_of(d)));
    std::snprintf(note, sizeof(note), "tx_dropped %s",
                  dpdk::stats::grouped(dstats[d].dropped).c_str());
    dpdk::stats::count(key, dstats[d].sent, note);
  }
  std::fprintf(stderr, "\n");
  return 0;
}
