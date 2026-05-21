// ─────────────────────────────────────────────────────────────────────────────
//  NanoMatch – Matching Engine Implementation
// ─────────────────────────────────────────────────────────────────────────────
#include <core/matching_engine.hpp>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cassert>

namespace nm {

// ─────────────────────────────────────────────────────────────────────────────
//  ClockCalib – measure RDTSC ticks per nanosecond
// ─────────────────────────────────────────────────────────────────────────────
ClockCalib::ClockCalib() {
    using namespace std::chrono;
    const int N = 5;
    double sum = 0.0;
    for (int i = 0; i < N; ++i) {
        auto wall0  = steady_clock::now();
        uint64_t t0 = rdtsc();
        std::this_thread::sleep_for(milliseconds(100));
        uint64_t t1 = rdtsc();
        auto wall1  = steady_clock::now();
        double ns   = static_cast<double>(duration_cast<nanoseconds>(wall1 - wall0).count());
        sum += static_cast<double>(t1 - t0) / ns;
    }
    ticks_per_ns = sum / N;
    std::fprintf(stderr, "[NanoMatch] RDTSC calibration: %.3f ticks/ns\n", ticks_per_ns);
}

// ─────────────────────────────────────────────────────────────────────────────
//  EngineStats
// ─────────────────────────────────────────────────────────────────────────────
double EngineStats::percentile(double p) const {
    if (latency_samples.empty()) return 0.0;
    std::vector<uint64_t> v(latency_samples);
    std::sort(v.begin(), v.end());
    std::size_t idx = static_cast<std::size_t>(std::ceil(p / 100.0 * v.size())) - 1;
    idx = std::min(idx, v.size() - 1);
    return static_cast<double>(v[idx]);
}

void EngineStats::print_summary() const {
    std::printf("\n═══════════════════════════════════════════\n");
    std::printf("  NanoMatch Engine Performance Summary\n");
    std::printf("═══════════════════════════════════════════\n");
    std::printf("  Orders received : %llu\n", (unsigned long long)orders_received);
    std::printf("  Orders rejected : %llu\n", (unsigned long long)orders_rejected);
    std::printf("  Trades executed : %llu\n", (unsigned long long)trades_executed);
    std::printf("  Volume traded   : %llu\n", (unsigned long long)volume_traded);
    if (!latency_samples.empty()) {
        std::printf("  Latency p50     : %.0f ns\n", p50());
        std::printf("  Latency p90     : %.0f ns\n", p90());
        std::printf("  Latency p99     : %.0f ns\n", p99());
    }
    std::printf("═══════════════════════════════════════════\n\n");
}

// ─────────────────────────────────────────────────────────────────────────────
//  MatchingEngine
// ─────────────────────────────────────────────────────────────────────────────
MatchingEngine::MatchingEngine() : clock_() {}

uint32_t MatchingEngine::register_symbol(const std::string& name) {
    auto it = symbol_name_to_id_.find(name);
    if (it != symbol_name_to_id_.end()) return it->second;

    uint32_t id = next_sym_id_++;
    symbol_name_to_id_[name] = id;

    auto cb = [this](const Trade& t) noexcept { on_trade(t); };
    books_.emplace(id, std::make_unique<OrderBook>(id, cb));
    return id;
}

OrderBook* MatchingEngine::book(uint32_t symbol_id) noexcept {
    auto it = books_.find(symbol_id);
    return (it != books_.end()) ? it->second.get() : nullptr;
}

void MatchingEngine::on_trade(const Trade& t) noexcept {
    ++stats_.trades_executed;
    stats_.volume_traded += t.qty;
    // Non-blocking push to SPSC log (intentionally fire-and-forget)
    [[maybe_unused]] bool pushed = trade_log.try_push(t);
}

// ── Hot-path submit methods ──────────────────────────────────────────────────
void MatchingEngine::submit_limit(OrderId id, uint32_t sym, Side side,
                                   Price price, Qty qty, TIF tif) noexcept {
    ++stats_.orders_received;
    OrderBook* b = book(sym);
    if (__builtin_expect(!b, 0)) { ++stats_.orders_rejected; return; }

    uint64_t t0 = rdtsc();
    b->add_limit_order(id, side, price, qty, tif, t0);
    uint64_t t1 = rdtsc();

    stats_.record_latency(clock_.ticks_to_ns(t1 - t0));
}

void MatchingEngine::submit_market(OrderId id, uint32_t sym,
                                    Side side, Qty qty) noexcept {
    ++stats_.orders_received;
    OrderBook* b = book(sym);
    if (__builtin_expect(!b, 0)) { ++stats_.orders_rejected; return; }

    uint64_t t0 = rdtsc();
    b->add_market_order(id, side, qty, t0);
    uint64_t t1 = rdtsc();

    stats_.record_latency(clock_.ticks_to_ns(t1 - t0));
}

void MatchingEngine::submit_cancel(OrderId id, uint32_t sym) noexcept {
    OrderBook* b = book(sym);
    if (__builtin_expect(!b, 0)) return;
    b->cancel_order(id);
}

// ── Replay CSV ───────────────────────────────────────────────────────────────
void MatchingEngine::replay_csv(const char* path, bool verbose) {
    io::CSVParser parser;

    // Pre-register a default symbol; parser will auto-assign IDs
    uint64_t row = 0;
    auto cb = [&](const io::CSVOrder& o) {
        // Auto-create book for new symbol IDs
        if (!book(o.symbol_id)) {
            books_.emplace(o.symbol_id,
                std::make_unique<OrderBook>(o.symbol_id,
                    [this](const Trade& t){ on_trade(t); }));
        }
        ++row;
        if (verbose && row % 100000 == 0) {
            std::fprintf(stderr, "\r  Replayed %llu orders...", (unsigned long long)row);
            std::fflush(stderr);
        }

        if (o.type == OrderType::Limit) {
            submit_limit(o.id, o.symbol_id, o.side, o.price, o.qty, o.tif);
        } else if (o.type == OrderType::Market) {
            submit_market(o.id, o.symbol_id, o.side, o.qty);
        } else if (o.type == OrderType::Cancel) {
            submit_cancel(o.id, o.symbol_id);
        }
    };

    parser.parse_file(path, cb);
    if (verbose) std::fprintf(stderr, "\n");
}

// ── Replay ITCH ──────────────────────────────────────────────────────────────
void MatchingEngine::replay_itch(const char* path, bool verbose) {
    io::ITCHParser parser;

    uint64_t msg = 0;
    auto cb = [&](const io::ParsedEvent& ev) {
        if (!book(ev.symbol_id)) {
            books_.emplace(ev.symbol_id,
                std::make_unique<OrderBook>(ev.symbol_id,
                    [this](const Trade& t){ on_trade(t); }));
        }
        ++msg;
        if (verbose && msg % 500000 == 0) {
            std::fprintf(stderr, "\r  Replayed %llu ITCH msgs...", (unsigned long long)msg);
            std::fflush(stderr);
        }

        switch (ev.kind) {
            case io::EventKind::Add:
                submit_limit(ev.order_ref, ev.symbol_id, ev.side, ev.price, ev.qty);
                break;
            case io::EventKind::Execute:
            case io::EventKind::Cancel:
            case io::EventKind::Reduce:
                submit_cancel(ev.order_ref, ev.symbol_id);
                break;
            case io::EventKind::Replace:
                submit_cancel(ev.order_ref, ev.symbol_id);
                submit_limit(ev.new_order_ref, ev.symbol_id, ev.side, ev.price, ev.qty);
                break;
            default: break;
        }
    };

    parser.parse_file(path, cb);
    if (verbose) std::fprintf(stderr, "\n");
}

// ── Trade log drain ──────────────────────────────────────────────────────────
std::size_t MatchingEngine::drain_trade_log(std::size_t max) noexcept {
    std::size_t count = 0;
    while (count < max) {
        auto t = trade_log.try_pop();
        if (!t) break;
        ++count;
        // In production: write to file / send to risk system
    }
    return count;
}

// ── Book snapshot print ──────────────────────────────────────────────────────
void MatchingEngine::print_book_snapshot(uint32_t sym_id, int depth) const {
    auto it = books_.find(sym_id);
    if (it == books_.end()) { std::printf("Symbol %u not found\n", sym_id); return; }
    const OrderBook& ob = *it->second;

    auto asks = ob.top_levels(Side::Sell, depth);
    auto bids = ob.top_levels(Side::Buy,  depth);
    auto bbo  = ob.best_bid_offer();

    std::printf("\n──── Order Book [sym=%u] ────────────────────\n", sym_id);
    std::printf("  %-12s  %-8s  %-6s\n", "PRICE", "QTY", "ORDERS");

    // Print asks in reverse (highest ask first for visual clarity)
    for (auto it2 = asks.rbegin(); it2 != asks.rend(); ++it2) {
        std::printf("  ASK  %-8.4f  %-8llu  %-6u\n",
            to_double(it2->price),
            (unsigned long long)it2->qty,
            it2->orders);
    }

    if (bbo.valid()) {
        std::printf("  ── SPREAD: %.4f bps ──\n", bbo.spread_bps());
    }

    for (const auto& lvl : bids) {
        std::printf("  BID  %-8.4f  %-8llu  %-6u\n",
            to_double(lvl.price),
            (unsigned long long)lvl.qty,
            lvl.orders);
    }
    std::printf("──────────────────────────────────────────\n");
    std::printf("  Active orders: %llu | Trades: %llu\n",
        (unsigned long long)ob.active_orders(),
        (unsigned long long)ob.total_trades());
    std::printf("──────────────────────────────────────────\n\n");
}

} // namespace nm
