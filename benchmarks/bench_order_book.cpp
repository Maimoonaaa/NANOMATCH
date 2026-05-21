// ─────────────────────────────────────────────────────────────────────────────
//  NanoMatch – Google Benchmark: Order Book
//
//  Run: ./nanomatch_bench --benchmark_filter=BM_OrderBook
//       --benchmark_repetitions=5
//       --benchmark_report_aggregates_only=true
// ─────────────────────────────────────────────────────────────────────────────
#include <benchmark/benchmark.h>
#include <core/order_book.hpp>
#include <core/matching_engine.hpp>

using namespace nm;

// ── Fixture: pre-seeded order book ───────────────────────────────────────────
class OrderBookFixture : public benchmark::Fixture {
public:
    void SetUp(const benchmark::State&) override {
        book = std::make_unique<OrderBook>(1, nullptr);
        Price base = from_double(100.0);
        Price tick = from_double(0.01);

        // Seed 1000 bids and 1000 asks
        for (int i = 0; i < 1000; ++i) {
            book->add_limit_order(i+1,        Side::Buy,  base - i*tick, 100);
            book->add_limit_order(i+1+100000, Side::Sell, base + i*tick, 100);
        }
        next_id = 200001;
    }
    std::unique_ptr<OrderBook> book;
    OrderId next_id = 1;
};

// ── Benchmark: add non-crossing limit order ──────────────────────────────────
BENCHMARK_F(OrderBookFixture, BM_AddLimitOrderPassive)(benchmark::State& state) {
    Price base = from_double(50.0);  // deep out-of-book – won't cross
    Price tick = from_double(0.01);
    int i = 0;
    for (auto _ : state) {
        book->add_limit_order(next_id++, Side::Buy, base - (i % 100) * tick, 100);
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel("add_limit_passive");
}

// ── Benchmark: add crossing limit order (triggers matching) ──────────────────
BENCHMARK_F(OrderBookFixture, BM_AddLimitOrderAggressive)(benchmark::State& state) {
    Price aggr_ask = from_double(99.0);  // crosses best bid
    for (auto _ : state) {
        book->add_limit_order(next_id++, Side::Sell, aggr_ask, 10);
        // Refresh: re-add bid to keep book seeded
        book->add_limit_order(next_id++, Side::Buy, aggr_ask - from_double(0.005), 10);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel("add_limit_aggressive");
}

// ── Benchmark: market order ───────────────────────────────────────────────────
BENCHMARK_F(OrderBookFixture, BM_MarketOrder)(benchmark::State& state) {
    for (auto _ : state) {
        book->add_market_order(next_id++, Side::Buy, 50);
        // Replenish ask side
        book->add_limit_order(next_id++, Side::Sell, from_double(100.01), 50);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel("market_order");
}

// ── Benchmark: cancel order ───────────────────────────────────────────────────
static void BM_CancelOrder(benchmark::State& state) {
    OrderBook book(1, nullptr);
    Price base = from_double(100.0);
    Price tick = from_double(0.01);

    // Pre-add orders for cancellation
    constexpr int POOL = 65536;
    for (int i = 1; i <= POOL; ++i) {
        book.add_limit_order(i, Side::Buy, base - (i % 50) * tick, 100);
    }

    int id = 1;
    for (auto _ : state) {
        book.cancel_order(id);
        // Replenish
        book.add_limit_order(id + POOL, Side::Buy, base - (id % 50) * tick, 100);
        ++id;
        if (id > POOL) id = 1;
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel("cancel_order");
}
BENCHMARK(BM_CancelOrder)->Iterations(1000000);

// ── Benchmark: BBO query ──────────────────────────────────────────────────────
BENCHMARK_F(OrderBookFixture, BM_BBOQuery)(benchmark::State& state) {
    for (auto _ : state) {
        auto bbo = book->best_bid_offer();
        benchmark::DoNotOptimize(bbo);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel("bbo_query");
}

// ── Benchmark: full engine (with SPSC) ───────────────────────────────────────
static void BM_EngineRoundtrip(benchmark::State& state) {
    MatchingEngine eng;
    uint32_t sym = eng.register_symbol("TEST");
    Price base   = from_double(100.0);
    Price tick   = from_double(0.01);
    OrderId id   = 1;

    // Seed
    for (int i = 0; i < 100; ++i) {
        eng.submit_limit(id++, sym, Side::Buy,  base - i*tick, 100);
        eng.submit_limit(id++, sym, Side::Sell, base + i*tick, 100);
    }

    for (auto _ : state) {
        eng.submit_limit(id++, Side::Buy  == Side::Buy ? Side::Buy : Side::Sell,
                         sym, base, 10);
        benchmark::DoNotOptimize(id);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EngineRoundtrip)->Iterations(500000);
