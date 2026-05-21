#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  NanoMatch – Limit Order Book (LOB)
// ─────────────────────────────────────────────────────────────────────────────
#include <core/types.hpp>
#include <memory/pool_allocator.hpp>
#include <array>
#include <memory>
#include <unordered_map>
#include <vector>
#include <functional>
#include <cstring>
#include <limits>

namespace nm {

// ─────────────────────────────────────────────────────────────────────────────
//  PriceLevel – one rung on the order book ladder
//  Explicit layout – no implicit compiler padding surprises.
//  Exactly 64 bytes (one cache line).
//
//  Layout:
//    price     8
//    total_qty 8
//    head      8
//    tail      8
//    order_cnt 4
//    _pad      28
//    ──────────
//    total     64
// ─────────────────────────────────────────────────────────────────────────────
struct alignas(64) PriceLevel {
    Price    price      = INVALID_PRICE;   //  8
    Qty      total_qty  = 0;               //  8
    Order*   head       = nullptr;         //  8
    Order*   tail       = nullptr;         //  8
    uint32_t order_cnt  = 0;               //  4
    uint8_t  _pad[28]   = {};              // 28  → total 64

    bool empty() const noexcept { return order_cnt == 0; }
};
static_assert(sizeof(PriceLevel) == 64, "PriceLevel must be exactly one cache line");

// ─────────────────────────────────────────────────────────────────────────────
//  OrderBook
// ─────────────────────────────────────────────────────────────────────────────
class OrderBook {
public:
    static constexpr std::size_t MAX_ORDERS       = 1 << 20;   // 1M orders
    static constexpr std::size_t MAX_PRICE_LEVELS = 1 << 16;   // 64K levels per side
    static constexpr Price       PRICE_TICK_UNITS  = 100; // 1 index unit = $0.01

    using TradeCallback = std::function<void(const Trade&)>;

    explicit OrderBook(uint32_t symbol_id, TradeCallback on_trade = nullptr);
    ~OrderBook() = default;

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    // ── Primary interface ───────────────────────────────────────────────────
    bool add_limit_order (OrderId id, Side side, Price price, Qty qty,
                          TIF tif = TIF::GTC, Timestamp ts = 0) noexcept;
    bool add_market_order(OrderId id, Side side, Qty qty, Timestamp ts = 0) noexcept;
    bool cancel_order    (OrderId id) noexcept;
    bool modify_order    (OrderId id, Price new_price, Qty new_qty,
                          Timestamp ts = 0) noexcept;

    // ── Queries ─────────────────────────────────────────────────────────────
    struct BBO {
        Price best_bid  = INVALID_PRICE;
        Price best_ask  = INVALID_PRICE;
        Qty   bid_qty   = 0;
        Qty   ask_qty   = 0;
        bool  valid() const noexcept {
            return best_bid != INVALID_PRICE && best_ask != INVALID_PRICE;
        }
        double spread_bps() const noexcept {
            if (!valid()) return 0.0;
            return static_cast<double>(best_ask - best_bid) /
                   static_cast<double>(best_bid) * 10'000.0;
        }
    };

    [[nodiscard]] BBO  best_bid_offer()              const noexcept;
    [[nodiscard]] Qty  qty_at_price(Side s, Price p) const noexcept;

    struct LevelSnapshot { Price price; Qty qty; uint32_t orders; };
    std::vector<LevelSnapshot> top_levels(Side side, int depth = 10) const;

    // ── Stats ────────────────────────────────────────────────────────────────
    [[nodiscard]] uint64_t total_trades()  const noexcept { return total_trades_;  }
    [[nodiscard]] uint64_t total_volume()  const noexcept { return total_volume_;  }
    [[nodiscard]] uint64_t active_orders() const noexcept { return active_orders_; }

    void set_trade_callback(TradeCallback cb) noexcept { on_trade_ = std::move(cb); }

private:
    // HALF removed - using absolute price index

    [[nodiscard]] int32_t price_to_idx(Price p) const noexcept {
        return static_cast<int32_t>(p / PRICE_TICK_UNITS);
    }
    [[nodiscard]] bool valid_idx(int32_t i) const noexcept {
        return i >= 0 && i < static_cast<int32_t>(MAX_PRICE_LEVELS);
    }

    // Intrusive list helpers stored in Order::_pad
    static Order*  get_next(const Order* o) noexcept {
        Order* r; std::memcpy(&r, &o->_pad[0], 8); return r;
    }
    static Order*  get_prev(const Order* o) noexcept {
        Order* r; std::memcpy(&r, &o->_pad[8], 8); return r;
    }
    static void set_next(Order* o, Order* v) noexcept { std::memcpy(&o->_pad[0], &v, 8); }
    static void set_prev(Order* o, Order* v) noexcept { std::memcpy(&o->_pad[8], &v, 8); }

    Qty  match_order(Order* taker, Side maker_side) noexcept;
    void execute_trade(Order* maker, Order* taker, Qty fill_qty) noexcept;
    void remove_order_from_level(Order* o, PriceLevel& lvl) noexcept;
    void update_best_bid() noexcept;
    void update_best_ask() noexcept;

    uint32_t      symbol_id_;
    TradeCallback on_trade_;

    std::unique_ptr<PriceLevel[]> bid_levels_;
    std::unique_ptr<PriceLevel[]> ask_levels_;

    int32_t best_bid_idx_ = -1;
    int32_t best_ask_idx_ = -1;

    mem::SlabPool<Order, MAX_ORDERS> order_pool_;
    std::unordered_map<OrderId, Order*> order_map_;

    uint64_t total_trades_  = 0;
    uint64_t total_volume_  = 0;
    uint64_t active_orders_ = 0;
};

} // namespace nm
