// Wire messages for the fan-out harness. Every frame begins with a common
// Header carrying the sequence id and send timestamp the consumer uses to
// measure delivery; the rest of the frame describes a market-data event -- a
// trade, a top-of-book (BBO) update, or a 5-level order book snapshot.
#pragma once

#include <algorithm>

#include <cstdint>

namespace msg {

inline constexpr uint32_t kBookDepth = 4;

enum class Type : uint8_t {
  Trade = 0,
  Bbo = 1,
  OrderBook = 2,
};

// Common framing header at the start of every message. seq_id and send_ts_ns
// are the framing contract consumers rely on and must not move/change size;
// the rest is ours to shape.
//
// flags_and_type bit layout (LSB first):
//   bits [0:1] Type (this file's enum)
//   bits [2:7] per-type flags -- meaning depends on Type:
//     Trade:     bit2 aggressor_side, bit3 is_block_trade, bit4 is_rpi,
//                bit5 is_liquidation, bits[6:7] tick_direction (0-3)
//     OrderBook: bit2 is_snapshot
//     Bbo:       unused
//   bits [8:31] reserved for future flags
//
// It's a u32 rather than the u8 this actually needs because Header would pad
// out to the same 24 bytes regardless (alignof(Header) is 8, from the two
// u64 fields) -- a u8 flags field leaves 3 bytes of pure padding, so making
// it u32 spends nothing extra and banks the headroom for future flags.
struct Header {
  uint64_t seq_id;          // monotonic, starts at 1
  uint64_t send_ts_ns;      // producer send timestamp, ns since epoch
  uint32_t instrument_id;   // symbol+venue, resolved once out of band -- see body TODO
  uint32_t flags_and_type;  // see bit layout above

  // Transport stamps, appended so seq_id/send_ts_ns keep their offsets (the
  // framing contract the consumer measures on). Together with send_ts_ns (t0)
  // and the consumer's own read (t3) they cut end-to-end latency into three
  // segments:
  //
  //   A = tx_ts_ns - send_ts_ns   producer ring -> sender, incl. burst assembly
  //   B = rx_ts_ns - tx_ts_ns     the wire: TX, network, RX
  //   C = t3       - rx_ts_ns     receiver -> consumer ring
  //
  // A and C stay inside one host, so they are exact. B is the only segment
  // that crosses the host boundary and therefore the only one carrying the
  // two clocks' offset -- which is constant within a run, so B's *shape*
  // (p99-p50, jitter) is still exact and only its absolute level is unknown.
  // That is why no clock synchronisation is needed to optimise against these.
  uint64_t tx_ts_ns;        // stamped by the sender just before the NIC
  uint64_t rx_ts_ns;        // stamped by the receiver right after the poll
};

inline constexpr uint32_t kTypeBits = 2;
inline constexpr uint32_t kTypeMask = (1u << kTypeBits) - 1;

inline constexpr uint32_t kTradeFlagAggressorSide = 1u << 2;
inline constexpr uint32_t kTradeFlagBlockTrade = 1u << 3;
inline constexpr uint32_t kTradeFlagRpi = 1u << 4;
inline constexpr uint32_t kTradeFlagLiquidation = 1u << 5;
inline constexpr uint32_t kTradeTickDirectionShift = 6;  // 2 bits, values 0-3

inline constexpr uint32_t kBookFlagSnapshot = 1u << 2;

inline uint32_t pack_type(Type t) {
  return static_cast<uint32_t>(t) & kTypeMask;
}
inline Type unpack_type(uint32_t flags_and_type) {
  return static_cast<Type>(flags_and_type & kTypeMask);
}

// No alignas(64) here: cache-line alignment is the ring Slot's concern, not
// the wire format's -- padding a message out to 64 bytes just to hit that
// boundary wastes bytes on every transport that isn't the shm ring itself.
struct Trade {
  Header header;
  uint64_t seller_order_id;
  uint64_t buyer_order_id;
  uint64_t trade_id;
  uint64_t match_engine_ts_ns;
  int64_t price_ticks;
  int64_t quantity_lots;
  // uint8_t padding[56];
};

struct Bbo {
  Header header;
  uint64_t update_id;
  uint64_t match_engine_ts_ns;
  int64_t bid_price_ticks;
  int64_t ask_price_ticks;
  int64_t bid_size_lots;
  int64_t ask_size_lots;
  uint32_t bid_order_count;
  uint32_t ask_order_count;
  // uint8_t padding[48];
};

// Structure-of-arrays book levels: grouping all price_ticks together, then
// all size_lots, then all order_count avoids the 4-byte pad that a
// per-level {price_ticks, size_lots, order_count} struct needs after each
// order_count (to re-align the next level's price_ticks to 8) -- 10 of those
// pads (5 bid + 5 ask) was real waste for pure alignment, not data.
template <uint32_t Depth>
struct Levels {
  int64_t price_ticks[Depth];
  int64_t size_lots[Depth];
  uint32_t order_count[Depth];
};

struct OrderBook {
  Header header;
  uint64_t update_id;
  uint64_t prev_update_id;
  uint64_t match_engine_ts_ns;
  Levels<kBookDepth> bidsLevels;
  Levels<kBookDepth> asksLevels;
  uint32_t checksum;
  // uint8_t padding[44];
};

inline constexpr uint32_t kMaxFrame =std::max(std::max(sizeof(Bbo) ,sizeof(OrderBook)), sizeof(Trade));
static_assert(kMaxFrame > 0, "kMaxFrame must be cumputed at compile time");
static_assert(sizeof(Trade) <= kMaxFrame, "kMaxFrame must fit every message");
static_assert(sizeof(Bbo) <= kMaxFrame, "kMaxFrame must fit every message");

}  // namespace msg
