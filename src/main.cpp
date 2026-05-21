// =============================================================================
//  NanoMatch — Main Entry Point
//
//  Usage:
//    ./nanomatch_bin --demo                   built-in scenario (default)
//    ./nanomatch_bin --bench                  1M-order throughput benchmark
//    ./nanomatch_bin --csv  <orders.csv>      replay synthetic CSV
//    ./nanomatch_bin --itch <orders.itch>     replay NASDAQ ITCH binary
//    ./nanomatch_bin --depth <N>              book snapshot depth (default 10)
// =============================================================================
#include <core/matching_engine.hpp>
#include <memory>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>

using namespace nm;

// =============================================================================
//  Demo — handcrafted scenario exercising every code path
// =============================================================================
static void run_demo(MatchingEngine& eng) {
    uint32_t sym = eng.register_symbol("AAPL");
    std::printf("\n[DEMO] Symbol AAPL registered as id=%u\n\n", sym);

    Price base = from_double(150.0);
    Price tick = from_double(0.01);   // 1 cent

    // ── Seed resting limit orders ─────────────────────────────────────────────
    std::printf("[DEMO] Seeding limit order book...\n");

    // Bids
    eng.submit_limit(1, sym, Side::Buy,  base - tick*1, 500,  TIF::GTC);
    eng.submit_limit(2, sym, Side::Buy,  base - tick*1, 300,  TIF::GTC);  // same level
    eng.submit_limit(3, sym, Side::Buy,  base - tick*2, 1000, TIF::GTC);
    eng.submit_limit(4, sym, Side::Buy,  base - tick*3, 200,  TIF::GTC);

    // Asks
    eng.submit_limit(5, sym, Side::Sell, base + tick*1, 400,  TIF::GTC);
    eng.submit_limit(6, sym, Side::Sell, base + tick*1, 200,  TIF::GTC);  // same level
    eng.submit_limit(7, sym, Side::Sell, base + tick*2, 800,  TIF::GTC);
    eng.submit_limit(8, sym, Side::Sell, base + tick*3, 600,  TIF::GTC);

    eng.print_book_snapshot(sym, 5);

    // ── Aggressive limit — crosses spread → immediate fill ────────────────────
    std::printf("[DEMO] Aggressive BUY limit @ ask price (crosses spread)...\n");
    eng.submit_limit(9, sym, Side::Buy, base + tick*1, 350, TIF::GTC);
    eng.print_book_snapshot(sym, 5);

    // ── Market order ──────────────────────────────────────────────────────────
    std::printf("[DEMO] Market SELL 700 shares...\n");
    eng.submit_market(10, sym, Side::Sell, 700);
    eng.print_book_snapshot(sym, 5);

    // ── Cancel ────────────────────────────────────────────────────────────────
    std::printf("[DEMO] Cancelling order 3 (bid @ %.2f x1000)...\n",
                to_double(base - tick*2));
    eng.submit_cancel(3, sym);
    eng.print_book_snapshot(sym, 5);

    // ── IOC — fill what you can, discard rest ─────────────────────────────────
    std::printf("[DEMO] IOC BUY 10000 (more than book depth — partial fill)...\n");
    eng.submit_limit(11, sym, Side::Buy, base + tick*5, 10000, TIF::IOC);
    eng.print_book_snapshot(sym, 5);

    eng.stats().print_summary();
}

// =============================================================================
//  Throughput benchmark — 1M orders, rdtsc-timed
// =============================================================================
static void run_throughput_bench(MatchingEngine& eng) {
    uint32_t sym = eng.register_symbol("BENCH");
    Price base   = from_double(100.0);
    Price tick   = from_double(0.01);

    constexpr int N = 1'000'000;
    std::printf("\n[BENCH] Sending %d orders...\n", N);

    auto t0 = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < N; ++i) {
        // Alternate bids/asks around mid → constant matching activity
        Side  s  = (i & 1) ? Side::Buy : Side::Sell;
        Price px = base + ((i % 10) - 5) * tick;
        eng.submit_limit(static_cast<OrderId>(i + 100), sym, s, px, 100);
    }

    auto   t1  = std::chrono::high_resolution_clock::now();
    double ms  = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double ops = N / (ms / 1000.0);

    std::printf("[BENCH] %d orders in %.2f ms → %.0f orders/sec\n", N, ms, ops);
    std::printf("[BENCH] Average latency: %.2f μs/order\n", ms * 1000.0 / N);

    eng.stats().print_summary();
}

// =============================================================================
//  main
// =============================================================================
int main(int argc, char** argv) {
    std::printf("╔══════════════════════════════════════════╗\n");
    std::printf("║         NanoMatch — v1.0.0               ║\n");
    std::printf("║  Ultra-Low Latency Order Matching Engine ║\n");
    std::printf("╚══════════════════════════════════════════╝\n\n");

    // MatchingEngine owns large mmap'd pools — must live on heap, not stack
    auto engine_ptr = std::make_unique<MatchingEngine>();
    MatchingEngine& engine = *engine_ptr;

    bool        demo      = false;
    bool        bench     = false;
    bool        csv       = false;
    bool        itch      = false;
    const char* data_path = nullptr;
    int         depth     = 10;

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--demo"))              demo  = true;
        else if (!strcmp(argv[i], "--bench"))             bench = true;
        else if (!strcmp(argv[i], "--csv")  && i+1<argc) { csv  = true; data_path = argv[++i]; }
        else if (!strcmp(argv[i], "--itch") && i+1<argc) { itch = true; data_path = argv[++i]; }
        else if (!strcmp(argv[i], "--depth")&& i+1<argc) { depth = std::atoi(argv[++i]); }
    }

    // Log thread: drain SPSC trade ring in background
    std::atomic<bool> running{true};
    std::thread log_thread([&] {
        while (running.load(std::memory_order_relaxed)) {
            engine.drain_trade_log(1024);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        // Final drain before exit
        engine.drain_trade_log(engine.trade_log.capacity());
    });

    if (demo || (!csv && !itch && !bench))
        run_demo(engine);

    if (bench)
        run_throughput_bench(engine);

    if (csv && data_path) {
        std::printf("[CSV] Replaying %s ...\n", data_path);
        engine.replay_csv(data_path, /*verbose=*/true);
        engine.stats().print_summary();
    }

    if (itch && data_path) {
        std::printf("[ITCH] Replaying %s ...\n", data_path);
        engine.replay_itch(data_path, /*verbose=*/true);
        engine.stats().print_summary();
    }

    running.store(false, std::memory_order_relaxed);
    log_thread.join();
    return 0;
}