#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  NanoMatch – Order Types
//  All primitive types used across the engine.
//  Kept in a single header to maximise inlining & avoid circular deps.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>
#include <limits>
#include <string_view>

namespace nm {

// ── Price representation ──────────────────────────────────────────────────────
// Fixed-point: 1 unit = 1e-4 USD  (4 decimal places)
// Range: ±$2,147,483  –  sufficient for all equity markets
using Price  = int64_t;
using Qty    = uint64_t;
using OrderId = uint64_t;
using Timestamp = uint64_t;    // nanoseconds since epoch (RDTSC-derived)

static constexpr Price PRICE_SCALE    = 10'000;
static constexpr Price INVALID_PRICE  = std::numeric_limits<Price>::min();
static constexpr Qty   INVALID_QTY    = std::numeric_limits<Qty>::max();
static constexpr OrderId INVALID_OID  = 0;

// ── Side ─────────────────────────────────────────────────────────────────────
enum class Side : uint8_t { Buy = 0, Sell = 1 };

inline constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

// ── Order type ────────────────────────────────────────────────────────────────
enum class OrderType : uint8_t {
    Limit  = 0,
    Market = 1,
    Cancel = 2,
    Modify = 3
};

// ── Time-In-Force ─────────────────────────────────────────────────────────────
enum class TIF : uint8_t {
    GTC = 0,   // Good-Till-Cancelled
    IOC = 1,   // Immediate-Or-Cancel
    FOK = 2    // Fill-Or-Kill
};

// ── Order – the hot-path struct ───────────────────────────────────────────────
// Carefully packed to exactly 64 bytes (one cache line).
// Fields are sorted by alignment requirement (largest first).
struct alignas(64) Order {
    OrderId   id;          //  8
    Price     price;       //  8
    Qty       qty;         //  8
    Qty       remaining;   //  8
    Timestamp ts;          //  8   nanoseconds
    uint32_t  symbol_id;   //  4
    Side      side;        //  1
    OrderType type;        //  1
    TIF       tif;         //  1
    // _pad layout (used by OrderBook intrusive linked list):
    //   _pad[0..7]  = next Order*  (8 bytes)
    //   _pad[8..15] = prev Order*  (8 bytes)
    //   _pad[16]    = spare        (1 byte)
    // Total: 8+8+8+8+8+4+1+1+1+17 = 64 B
    uint8_t   _pad[17];
};
static_assert(sizeof(Order) == 64, "Order must be exactly one cache line");

// ── Trade – filled execution record ──────────────────────────────────────────
struct Trade {
    OrderId   maker_id;
    OrderId   taker_id;
    Price     price;
    Qty       qty;
    Timestamp ts;
    Side      aggressor_side;
};

// ── Price → double helpers ────────────────────────────────────────────────────
inline double to_double(Price p) noexcept {
    return static_cast<double>(p) / PRICE_SCALE;
}
inline Price from_double(double d) noexcept {
    return static_cast<Price>(d * PRICE_SCALE + 0.5);
}

} // namespace nm
