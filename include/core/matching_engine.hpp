#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  NanoMatch – Matching Engine
//
//  Manages a fleet of OrderBook instances (one per symbol) and routes
//  incoming orders. Also owns the SPSC trade log ring buffer.
//
//  Threading model:
//    ┌──────────────┐  Orders   ┌─────────────────┐  Trades  ┌────────────┐
//    │ Parser Thread│ ────────▶ │ Matching Thread  │ ────────▶│ Log Thread │
//    └──────────────┘           └─────────────────┘  SPSC     └────────────┘
//
//  The SPSC ring buffer decouples matching from I/O, so trade logging
//  never stalls the hot path.
// ─────────────────────────────────────────────────────────────────────────────
#include <core/types.hpp>
#include <core/order_book.hpp>
#include <concurrency/spsc_ring_buffer.hpp>
#include <io/itch_parser.hpp>
#include <io/csv_parser.hpp>
#include <memory>
#include <unordered_map>
#include <vector>
#include <string>
#include <chrono>

namespace nm {

// ─────────────────────────────────────────────────────────────────────────────
//  RDTSC clock – nanosecond timestamps without syscall
// ─────────────────────────────────────────────────────────────────────────────
inline uint64_t rdtsc() noexcept {
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

// Calibrate ticks-per-ns once at startup
struct ClockCalib {
    double ticks_per_ns;
    explicit ClockCalib();
    uint64_t ticks_to_ns(uint64_t ticks) const noexcept {
        return static_cast<uint64_t>(ticks / ticks_per_ns);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  EngineStats – accumulated counters (read from log thread)
// ─────────────────────────────────────────────────────────────────────────────
struct EngineStats {
    uint64_t orders_received = 0;
    uint64_t orders_rejected = 0;
    uint64_t trades_executed = 0;
    uint64_t volume_traded   = 0;

    // Latency histogram (ns) – ring buffer of last N measurements
    static constexpr int HIST_SIZE = 1 << 16;
    std::vector<uint64_t> latency_samples;

    void record_latency(uint64_t ns) {
        latency_samples.push_back(ns);
    }

    // Percentile calculation (sorts a copy – call offline)
    double percentile(double p) const;
    double p50() const { return percentile(50.0); }
    double p90() const { return percentile(90.0); }
    double p99() const { return percentile(99.0); }

    void print_summary() const;
};

// ─────────────────────────────────────────────────────────────────────────────
//  MatchingEngine
// ─────────────────────────────────────────────────────────────────────────────
class MatchingEngine {
public:
    static constexpr std::size_t TRADE_LOG_CAPACITY = 1 << 20; // 1M trades in ring

    explicit MatchingEngine();
    ~MatchingEngine() = default;

    // ── Symbol management ───────────────────────────────────────────────────
    uint32_t register_symbol(const std::string& name);
    [[nodiscard]] OrderBook* book(uint32_t symbol_id) noexcept;

    // ── Order submission (hot path) ─────────────────────────────────────────
    void submit_limit (OrderId id, uint32_t sym, Side side,
                       Price price, Qty qty, TIF tif = TIF::GTC) noexcept;
    void submit_market(OrderId id, uint32_t sym, Side side, Qty qty) noexcept;
    void submit_cancel(OrderId id, uint32_t sym) noexcept;

    // ── Data ingestion ──────────────────────────────────────────────────────
    void replay_csv  (const char* path, bool verbose = false);
    void replay_itch (const char* path, bool verbose = false);

    // ── Trade log drain (call from log thread) ──────────────────────────────
    std::size_t drain_trade_log(std::size_t max = 4096) noexcept;

    // ── Stats & reporting ───────────────────────────────────────────────────
    [[nodiscard]] const EngineStats& stats() const noexcept { return stats_; }
    void print_book_snapshot(uint32_t sym_id, int depth = 5) const;

    // ── SPSC ring buffer (trades) ────────────────────────────────────────────
    conc::SPSCRingBuffer<Trade, TRADE_LOG_CAPACITY> trade_log;

private:
    void on_trade(const Trade& t) noexcept;

    std::unordered_map<std::string, uint32_t>          symbol_name_to_id_;
    std::unordered_map<uint32_t, std::unique_ptr<OrderBook>> books_;
    uint32_t next_sym_id_ = 1;

    ClockCalib clock_;
    EngineStats stats_;
};

} // namespace nm
