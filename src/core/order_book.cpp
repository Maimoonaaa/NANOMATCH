// ─────────────────────────────────────────────────────────────────────────────
//  NanoMatch – OrderBook Implementation
// ─────────────────────────────────────────────────────────────────────────────
#include <core/order_book.hpp>
#include <algorithm>
#include <cstring>
#include <cassert>
#include <limits>

namespace nm {

OrderBook::OrderBook(uint32_t symbol_id, TradeCallback on_trade)
    : symbol_id_(symbol_id)
    , on_trade_(std::move(on_trade))
    , bid_levels_(new PriceLevel[MAX_PRICE_LEVELS])
    , ask_levels_(new PriceLevel[MAX_PRICE_LEVELS])
{
    for (std::size_t i = 0; i < MAX_PRICE_LEVELS; ++i) {
        bid_levels_[i] = PriceLevel{};
        ask_levels_[i] = PriceLevel{};
    }
    order_map_.reserve(1 << 16);
}

// ─────────────────────────────────────────────────────────────────────────────
bool OrderBook::add_limit_order(OrderId id, Side side, Price price,
                                 Qty qty, TIF tif, Timestamp ts) noexcept {
    if (__builtin_expect(price <= 0 || qty == 0, 0)) return false;

    Order* o = order_pool_.allocate();
    if (__builtin_expect(!o, 0)) return false;

    o->id        = id;
    o->price     = price;
    o->qty       = qty;
    o->remaining = qty;
    o->ts        = ts;
    o->symbol_id = symbol_id_;
    o->side      = side;
    o->type      = OrderType::Limit;
    o->tif       = tif;
    set_next(o, nullptr);
    set_prev(o, nullptr);

    // Try to match against opposite side first
    Qty filled = match_order(o, opposite(side));

    if (o->remaining == 0) {
        order_pool_.deallocate(o);
        return true;
    }
    if (tif == TIF::IOC || tif == TIF::FOK) {
        order_pool_.deallocate(o);
        return true;
    }

    // Rest in book
    int32_t idx = price_to_idx(price);
    if (__builtin_expect(!valid_idx(idx), 0)) {
        order_pool_.deallocate(o);
        return false;
    }

    PriceLevel* levels = (side == Side::Buy) ? bid_levels_.get() : ask_levels_.get();
    PriceLevel& lvl    = levels[idx];
    if (lvl.price == INVALID_PRICE) lvl.price = price;

    // Enqueue at tail (FIFO time priority)
    if (lvl.tail) {
        set_next(lvl.tail, o);
        set_prev(o, lvl.tail);
    } else {
        lvl.head = o;
        set_prev(o, nullptr);
    }
    lvl.tail = o;
    set_next(o, nullptr);
    lvl.total_qty += o->remaining;
    ++lvl.order_cnt;
    ++active_orders_;

    // Update best-price cursor
    if (side == Side::Buy) {
        if (best_bid_idx_ < 0 || idx > best_bid_idx_) best_bid_idx_ = idx;
    } else {
        if (best_ask_idx_ < 0 || idx < best_ask_idx_) best_ask_idx_ = idx;
    }

    order_map_[id] = o;
    return true;
    (void)filled;
}

// ─────────────────────────────────────────────────────────────────────────────
bool OrderBook::add_market_order(OrderId id, Side side, Qty qty, Timestamp ts) noexcept {
    if (__builtin_expect(qty == 0, 0)) return false;

    Order taker{};
    taker.id        = id;
    taker.price     = (side == Side::Buy) ? std::numeric_limits<Price>::max() : Price(0);
    taker.qty       = qty;
    taker.remaining = qty;
    taker.ts        = ts;
    taker.symbol_id = symbol_id_;
    taker.side      = side;
    taker.type      = OrderType::Market;
    taker.tif       = TIF::IOC;

    match_order(&taker, opposite(side));
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
bool OrderBook::cancel_order(OrderId id) noexcept {
    auto it = order_map_.find(id);
    if (__builtin_expect(it == order_map_.end(), 0)) return false;

    Order* o = it->second;
    order_map_.erase(it);

    int32_t idx        = price_to_idx(o->price);
    PriceLevel* levels = (o->side == Side::Buy) ? bid_levels_.get() : ask_levels_.get();
    PriceLevel& lvl    = levels[idx];

    remove_order_from_level(o, lvl);
    order_pool_.deallocate(o);
    --active_orders_;

    if (o->side == Side::Buy) update_best_bid();
    else                      update_best_ask();

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
bool OrderBook::modify_order(OrderId id, Price new_price, Qty new_qty,
                              Timestamp ts) noexcept {
    auto it = order_map_.find(id);
    if (__builtin_expect(it == order_map_.end(), 0)) return false;
    Side side = it->second->side;
    cancel_order(id);
    return add_limit_order(id, side, new_price, new_qty, TIF::GTC, ts);
}

// ─────────────────────────────────────────────────────────────────────────────
//  match_order – hot-path kernel
// ─────────────────────────────────────────────────────────────────────────────
Qty OrderBook::match_order(Order* taker, Side maker_side) noexcept {
    PriceLevel* levels = (maker_side == Side::Buy) ? bid_levels_.get() : ask_levels_.get();
    int32_t& best      = (maker_side == Side::Buy) ? best_bid_idx_ : best_ask_idx_;

    Qty total_filled = 0;

    while (taker->remaining > 0 && best >= 0 && best < static_cast<int32_t>(MAX_PRICE_LEVELS)) {
        PriceLevel& lvl = levels[best];

        if (lvl.empty()) {
            if (maker_side == Side::Buy) { --best; }
            else                          { ++best; }
            continue;
        }

        // Price check
        if (maker_side == Side::Buy) {
            if (lvl.price < taker->price) break;   // No more matchable bids
        } else {
            if (lvl.price > taker->price) break;   // No more matchable asks
        }

        // Walk FIFO queue at this level
        Order* maker = lvl.head;
        while (maker && taker->remaining > 0) {
            Qty fill = std::min(maker->remaining, taker->remaining);
            execute_trade(maker, taker, fill);
            total_filled += fill;

            Order* next_maker = get_next(maker);

            if (maker->remaining == 0) {
                lvl.head = next_maker;
                if (next_maker) set_prev(next_maker, nullptr);
                else            lvl.tail = nullptr;
                --lvl.order_cnt;
                order_map_.erase(maker->id);
                order_pool_.deallocate(maker);
                --active_orders_;
            }
            maker = next_maker;
        }

        // Recompute level qty
        lvl.total_qty = 0;
        for (Order* cur = lvl.head; cur; cur = get_next(cur))
            lvl.total_qty += cur->remaining;

        if (lvl.empty()) {
            if (maker_side == Side::Buy) { --best; }
            else                          { ++best; }
        }
    }

    if (best < 0 || best >= static_cast<int32_t>(MAX_PRICE_LEVELS)) best = -1;
    return total_filled;
}

// ─────────────────────────────────────────────────────────────────────────────
void OrderBook::execute_trade(Order* maker, Order* taker, Qty fill_qty) noexcept {
    maker->remaining -= fill_qty;
    taker->remaining -= fill_qty;
    ++total_trades_;
    total_volume_ += fill_qty;

    if (on_trade_) {
        Trade t{};
        t.maker_id       = maker->id;
        t.taker_id       = taker->id;
        t.price          = maker->price;
        t.qty            = fill_qty;
        t.ts             = taker->ts;
        t.aggressor_side = taker->side;
        on_trade_(t);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void OrderBook::remove_order_from_level(Order* o, PriceLevel& lvl) noexcept {
    Order* prev = get_prev(o);
    Order* next = get_next(o);

    if (prev) set_next(prev, next);
    else       lvl.head = next;

    if (next) set_prev(next, prev);
    else       lvl.tail = prev;

    lvl.total_qty -= o->remaining;
    --lvl.order_cnt;
}

// ─────────────────────────────────────────────────────────────────────────────
void OrderBook::update_best_bid() noexcept {
    while (best_bid_idx_ >= 0 && bid_levels_[best_bid_idx_].empty())
        --best_bid_idx_;
}

void OrderBook::update_best_ask() noexcept {
    while (best_ask_idx_ >= 0 &&
           best_ask_idx_ < static_cast<int32_t>(MAX_PRICE_LEVELS) &&
           ask_levels_[best_ask_idx_].empty())
        ++best_ask_idx_;
    if (best_ask_idx_ >= static_cast<int32_t>(MAX_PRICE_LEVELS))
        best_ask_idx_ = -1;
}

// ─────────────────────────────────────────────────────────────────────────────
OrderBook::BBO OrderBook::best_bid_offer() const noexcept {
    BBO bbo{};
    if (best_bid_idx_ >= 0) {
        const auto& lvl = bid_levels_[best_bid_idx_];
        bbo.best_bid = lvl.price;
        bbo.bid_qty  = lvl.total_qty;
    }
    if (best_ask_idx_ >= 0) {
        const auto& lvl = ask_levels_[best_ask_idx_];
        bbo.best_ask = lvl.price;
        bbo.ask_qty  = lvl.total_qty;
    }
    return bbo;
}

Qty OrderBook::qty_at_price(Side side, Price price) const noexcept {
    int32_t idx = price_to_idx(price);
    if (!valid_idx(idx)) return 0;
    const PriceLevel* levels = (side == Side::Buy) ? bid_levels_.get() : ask_levels_.get();
    return levels[idx].total_qty;
}

std::vector<OrderBook::LevelSnapshot>
OrderBook::top_levels(Side side, int depth) const {
    std::vector<LevelSnapshot> out;
    out.reserve(depth);
    const PriceLevel* levels = (side == Side::Buy) ? bid_levels_.get() : ask_levels_.get();

    if (side == Side::Buy) {
        for (int32_t i = best_bid_idx_; i >= 0 && (int)out.size() < depth; --i)
            if (!levels[i].empty())
                out.push_back({levels[i].price, levels[i].total_qty, levels[i].order_cnt});
    } else {
        for (int32_t i = (best_ask_idx_ >= 0 ? best_ask_idx_ : static_cast<int32_t>(MAX_PRICE_LEVELS));
             i < static_cast<int32_t>(MAX_PRICE_LEVELS) && (int)out.size() < depth; ++i)
            if (!levels[i].empty())
                out.push_back({levels[i].price, levels[i].total_qty, levels[i].order_cnt});
    }
    return out;
}

} // namespace nm
