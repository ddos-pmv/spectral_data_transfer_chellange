// Producer: generates a stream of market-data events (trade, BBO, or order-book
// snapshots), stamping each with a monotonic sequence id and a send timestamp,
// and publishes them into a shared-memory broadcast ring for one or more
// consumers. Fixed end of the benchmark harness -- a candidate replaces the
// transport, not this.
//
// Usage: producer [--shm NAME] [--slots N] [--count N] [--rate MSGS_PER_SEC]
//                 [--type trade|bbo|book|mixed]
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "message.h"
#include "shm_ring.h"
#include "shm_segment.h"
#include "util.h"

namespace {

enum class Kind { Trade, Bbo, Book, Mixed };

struct Config {
  std::string shm_name = "/fanout_ring";
  uint32_t slots = 1024;
  uint64_t count = 1000000;
  double rate = 0.0;
  Kind kind = Kind::Mixed;
};

bool is_power_of_two(uint32_t x) { return x != 0 && (x & (x - 1)) == 0; }

Kind parse_kind(const std::string& s) {
  if (s == "trade") return Kind::Trade;
  if (s == "bbo") return Kind::Bbo;
  if (s == "book") return Kind::Book;
  if (s == "mixed") return Kind::Mixed;
  fprintf(stderr, "--type must be trade|bbo|book|mixed (got %s)\n", s.c_str());
  std::exit(2);
}

Config parse_args(int argc, char** argv) {
  Config c;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value for %s\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--shm") c.shm_name = next();
    else if (a == "--slots") c.slots = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--count") c.count = std::stoull(next());
    else if (a == "--rate") c.rate = std::stod(next());
    else if (a == "--type") c.kind = parse_kind(next());
    else {
      fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }
  if (!is_power_of_two(c.slots)) {
    fprintf(stderr, "--slots must be a power of two (got %u)\n", c.slots);
    std::exit(2);
  }
  return c;
}

// All messages are BTCUSDT@BINANCE for now -- single-instrument placeholder
// until symbol/venue get a real out-of-band id<->name dictionary.
constexpr uint32_t kInstrumentId = 1;

void fill_header(msg::Header& h, uint64_t seq, msg::Type type,
                  uint32_t flags_bits) {
  h.seq_id = seq;
  h.instrument_id = kInstrumentId;
  h.flags_and_type = msg::pack_type(type) | flags_bits;
  h.send_ts_ns = util::now_ns();  // stamp as late as possible before publish
}

uint32_t build_trade(void* buf, uint64_t seq) {
  auto& m = *reinterpret_cast<msg::Trade*>(buf);
  m.trade_id = 100000 + seq;
  m.buyer_order_id = 500000 + seq * 2;
  m.seller_order_id = 500001 + seq * 2;
  m.match_engine_ts_ns = util::now_ns();
  const double price = 65000.0 + static_cast<double>(seq % 500) * 0.5;
  const double quantity = 0.001 + static_cast<double>(seq % 100) * 0.01;
  m.price_ticks = static_cast<int64_t>(price * 100.0);
  m.quantity_lots = static_cast<int64_t>(quantity * 1000.0);

  const uint32_t tick_direction = seq % 4;
  const uint32_t aggressor_side = seq & 1;
  const uint32_t flags_bits =
      (tick_direction << msg::kTradeTickDirectionShift) |
      (aggressor_side ? msg::kTradeFlagAggressorSide : 0);
  // is_block_trade / is_rpi / is_liquidation flags exist (kTradeFlag*) but
  // this synthetic feed never sets them -- left at 0.
  fill_header(m.header, seq, msg::Type::Trade, flags_bits);
  return sizeof(msg::Trade);
}

uint32_t build_bbo(void* buf, uint64_t seq) {
  auto& m = *reinterpret_cast<msg::Bbo*>(buf);
  m.update_id = 900000 + seq;
  m.match_engine_ts_ns = util::now_ns();
  const double mid = 65000.0 + static_cast<double>(seq % 500) * 0.5;
  const double bid_price = mid - 0.5;
  const double ask_price = mid + 0.5;
  const double bid_size = 1.5 + static_cast<double>(seq % 50) * 0.1;
  const double ask_size = 1.5 + static_cast<double>((seq + 7) % 50) * 0.1;
  m.bid_price_ticks = static_cast<int64_t>(bid_price * 100.0);
  m.ask_price_ticks = static_cast<int64_t>(ask_price * 100.0);
  m.bid_size_lots = static_cast<int64_t>(bid_size * 1000.0);
  m.ask_size_lots = static_cast<int64_t>(ask_size * 1000.0);
  m.bid_order_count = 3 + static_cast<uint32_t>(seq % 10);
  m.ask_order_count = 3 + static_cast<uint32_t>((seq + 3) % 10);
  fill_header(m.header, seq, msg::Type::Bbo, /*flags_bits=*/0);
  return sizeof(msg::Bbo);
}

uint32_t build_book(void* buf, uint64_t seq) {
  auto& m = *reinterpret_cast<msg::OrderBook*>(buf);
  m.update_id = 900000 + seq;
  m.prev_update_id = m.update_id - 1;
  m.match_engine_ts_ns = util::now_ns();
  const double mid = 65000.0 + static_cast<double>(seq % 500) * 0.5;
  for (uint32_t i = 0; i < msg::kBookDepth; ++i) {
    const double bid_price = mid - 0.5 - static_cast<double>(i);
    const double bid_size = 1.0 + static_cast<double>((seq + i) % 40) * 0.1;
    m.bidsLevels.price_ticks[i] = static_cast<int64_t>(bid_price * 100.0);
    m.bidsLevels.size_lots[i] = static_cast<int64_t>(bid_size * 1000.0);
    m.bidsLevels.order_count[i] = 2 + static_cast<uint32_t>((seq + i) % 8);

    const double ask_price = mid + 0.5 + static_cast<double>(i);
    const double ask_size =
        1.0 + static_cast<double>((seq + i + 5) % 40) * 0.1;
    m.asksLevels.price_ticks[i] = static_cast<int64_t>(ask_price * 100.0);
    m.asksLevels.size_lots[i] = static_cast<int64_t>(ask_size * 1000.0);
    m.asksLevels.order_count[i] =
        2 + static_cast<uint32_t>((seq + i + 5) % 8);
  }
  m.checksum = static_cast<uint32_t>(seq * 2654435761u);
  fill_header(m.header, seq, msg::Type::OrderBook, msg::kBookFlagSnapshot);
  return sizeof(msg::OrderBook);
}

uint32_t build(Kind kind, uint64_t seq, void* buf) {
  Kind k = kind;
  if (k == Kind::Mixed) {
    switch (seq % 3) {
      case 0: k = Kind::Trade; break;
      case 1: k = Kind::Bbo; break;
      default: k = Kind::Book; break;
    }
  }
  switch (k) {
    case Kind::Trade: return build_trade(buf, seq);
    case Kind::Bbo: return build_bbo(buf, seq);
    default: return build_book(buf, seq);
  }
}

const char* kind_name(Kind k) {
  switch (k) {
    case Kind::Trade: return "trade";
    case Kind::Bbo: return "bbo";
    case Kind::Book: return "book";
    default: return "mixed";
  }
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg = parse_args(argc, argv);

  shm::Segment seg =
      shm::Segment::open(cfg.shm_name, shm::region_size(cfg.slots), /*create=*/true);
  shm::Ring ring;
  ring.attach(seg.base(), cfg.slots, /*init=*/true);

  alignas(64) uint8_t frame[shm::kFrameCap];

  const uint64_t interval_ns =
      cfg.rate > 0.0 ? static_cast<uint64_t>(1e9 / cfg.rate) : 0;
  uint64_t next_send = util::now_ns();

  fprintf(stderr, "producer: shm=%s slots=%u count=%llu rate=%.0f type=%s\n",
          cfg.shm_name.c_str(), cfg.slots,
          static_cast<unsigned long long>(cfg.count), cfg.rate,
          kind_name(cfg.kind));

  uint64_t seq = 0;
  while (cfg.count == 0 || seq < cfg.count) {
    if (interval_ns) {
      while (util::now_ns() < next_send) {
      }
      next_send += interval_ns;
    }
    ++seq;
    const uint32_t len = build(cfg.kind, seq, frame);
    ring.publish(frame, len);
  }

  fprintf(stderr, "producer: sent %llu messages\n",
          static_cast<unsigned long long>(seq));
  seg.unlink();
  return 0;
}
