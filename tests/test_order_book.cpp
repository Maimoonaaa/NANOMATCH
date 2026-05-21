// ─────────────────────────────────────────────────────────────────────────────
//  NanoMatch – Order Book Unit Tests
// ─────────────────────────────────────────────────────────────────────────────
#include <core/order_book.hpp>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

// ── Minimal test macros (duplicated from test_main to keep self-contained) ────
#define ASSERT_TRUE(expr) \
    do { if (!(expr)) throw std::runtime_error("ASSERT_TRUE(" #expr ") failed at line " + std::to_string(__LINE__)); } while(0)
#define ASSERT_EQ(a,b) \
    do { if (!((a)==(b))) throw std::runtime_error(std::string("ASSERT_EQ failed: ") + std::to_string(a) + " != " + std::to_string(b) + " at line " + std::to_string(__LINE__)); } while(0)
#define ASSERT_GT(a,b) \
    do { if (!((a)>(b))) throw std::runtime_error("ASSERT_GT failed at line " + std::to_string(__LINE__)); } while(0)

struct RegisterTest {
    RegisterTest(const char* name, std::function<void()> fn);
};
// Forward declared in test_main.cpp – we just use the macro pattern here
// and link against test_main.cpp which provides the registry.

using namespace nm;

// ── Helper ────────────────────────────────────────────────────────────────────
static Price px(double d) { return from_double(d); }

// We can't use the TEST macro without including test_main.h – define inline tests
// and call them from a test function registered in test_main.

namespace {

void test_empty_book_bbo() {
    OrderBook book(1);
    auto bbo = book.best_bid_offer();
    ASSERT_TRUE(bbo.best_bid == INVALID_PRICE);
    ASSERT_TRUE(bbo.best_ask == INVALID_PRICE);
    ASSERT_TRUE(!bbo.valid());
}

void test_add_single_bid() {
    OrderBook book(1);
    bool ok = book.add_limit_order(1, Side::Buy, px(100.0), 500);
    ASSERT_TRUE(ok);
    auto bbo = book.best_bid_offer();
    ASSERT_EQ(bbo.best_bid, px(100.0));
    ASSERT_EQ(bbo.bid_qty,  500ULL);
    ASSERT_EQ(book.active_orders(), 1ULL);
}

void test_add_single_ask() {
    OrderBook book(1);
    bool ok = book.add_limit_order(1, Side::Sell, px(101.0), 300);
    ASSERT_TRUE(ok);
    auto bbo = book.best_bid_offer();
    ASSERT_EQ(bbo.best_ask, px(101.0));
    ASSERT_EQ(bbo.ask_qty,  300ULL);
}

void test_price_time_priority() {
    // Two bids at same price – first added should be filled first
    std::vector<OrderId> filled_makers;
    OrderBook book(1, [&](const Trade& t){ filled_makers.push_back(t.maker_id); });

    book.add_limit_order(1, Side::Buy,  px(100.0), 100);  // first in queue
    book.add_limit_order(2, Side::Buy,  px(100.0), 100);  // second
    book.add_limit_order(3, Side::Sell, px(100.0), 100);  // aggressive – matches first

    ASSERT_EQ(filled_makers.size(), 1ULL);
    ASSERT_EQ(filled_makers[0], 1ULL);  // order 1 filled first (time priority)
    ASSERT_EQ(book.active_orders(), 1ULL);  // order 2 still resting
}

void test_price_priority_bids() {
    // Higher bid gets matched first
    std::vector<OrderId> filled_makers;
    OrderBook book(1, [&](const Trade& t){ filled_makers.push_back(t.maker_id); });

    book.add_limit_order(1, Side::Buy,  px(100.0), 100);
    book.add_limit_order(2, Side::Buy,  px(101.0), 100);  // better price – priority
    book.add_limit_order(3, Side::Sell, px(100.0), 100);  // sells at 100 or better

    ASSERT_EQ(filled_makers.size(), 1ULL);
    ASSERT_EQ(filled_makers[0], 2ULL);  // order 2 had better price
}

void test_no_match_when_spread_exists() {
    OrderBook book(1);
    book.add_limit_order(1, Side::Buy,  px(99.0),  100);
    book.add_limit_order(2, Side::Sell, px(100.0), 100);
    ASSERT_EQ(book.total_trades(), 0ULL);
    ASSERT_EQ(book.active_orders(), 2ULL);
}

void test_full_fill_removes_from_book() {
    OrderBook book(1);
    book.add_limit_order(1, Side::Buy,  px(100.0), 200);
    book.add_limit_order(2, Side::Sell, px(100.0), 200);  // fully fills bid
    ASSERT_EQ(book.total_trades(), 1ULL);
    ASSERT_EQ(book.active_orders(), 0ULL);
}

void test_partial_fill() {
    OrderBook book(1);
    book.add_limit_order(1, Side::Buy,  px(100.0), 500);
    book.add_limit_order(2, Side::Sell, px(100.0), 200);  // partial fill of bid
    ASSERT_EQ(book.total_trades(), 1ULL);
    ASSERT_EQ(book.active_orders(), 1ULL);  // bid partially rests
    ASSERT_EQ(book.qty_at_price(Side::Buy, px(100.0)), 300ULL);
}

void test_cancel_order() {
    OrderBook book(1);
    book.add_limit_order(1, Side::Buy, px(100.0), 500);
    bool ok = book.cancel_order(1);
    ASSERT_TRUE(ok);
    ASSERT_EQ(book.active_orders(), 0ULL);
    auto bbo = book.best_bid_offer();
    ASSERT_TRUE(bbo.best_bid == INVALID_PRICE);
}

void test_cancel_nonexistent() {
    OrderBook book(1);
    bool ok = book.cancel_order(9999);
    ASSERT_TRUE(!ok);
}

void test_market_order() {
    int trades = 0;
    OrderBook book(1, [&](const Trade&){ ++trades; });
    book.add_limit_order(1, Side::Sell, px(100.0), 1000);
    book.add_market_order(2, Side::Buy, 500);
    ASSERT_EQ(trades, 1);
    ASSERT_EQ(book.qty_at_price(Side::Sell, px(100.0)), 500ULL);
}

void test_market_order_sweeps_multiple_levels() {
    int trades = 0;
    OrderBook book(1, [&](const Trade&){ ++trades; });
    book.add_limit_order(1, Side::Sell, px(100.0), 100);
    book.add_limit_order(2, Side::Sell, px(100.5), 100);
    book.add_limit_order(3, Side::Sell, px(101.0), 100);
    book.add_market_order(4, Side::Buy, 250);
    ASSERT_GT(trades, 1);
}

void test_ioc_no_fill() {
    OrderBook book(1);
    // No resting asks – IOC should be cancelled
    book.add_limit_order(1, Side::Buy, px(100.0), 100, TIF::IOC);
    // IOC that doesn't cross – should not rest
    ASSERT_EQ(book.active_orders(), 0ULL);
}

void test_modify_order() {
    OrderBook book(1);
    book.add_limit_order(1, Side::Buy, px(99.0), 100);
    book.modify_order(1, px(101.0), 50);
    auto bbo = book.best_bid_offer();
    ASSERT_EQ(bbo.best_bid, px(101.0));
    ASSERT_EQ(book.qty_at_price(Side::Buy, px(101.0)), 50ULL);
}

void test_bbo_spread() {
    OrderBook book(1);
    book.add_limit_order(1, Side::Buy,  px(99.50), 100);
    book.add_limit_order(2, Side::Sell, px(100.50), 100);
    auto bbo = book.best_bid_offer();
    ASSERT_TRUE(bbo.valid());
    ASSERT_GT(bbo.spread_bps(), 0.0);
}

} // anonymous namespace

// ── Registration ──────────────────────────────────────────────────────────────
#include <functional>
#include <vector>
#include <string>
extern std::vector<struct TC2>* get_reg();

struct TC2 { std::string name; std::function<void()> fn; };
static std::vector<std::pair<std::string, std::function<void()>>> ob_tests = {
    {"OrderBook::empty_book_bbo",            test_empty_book_bbo},
    {"OrderBook::add_single_bid",            test_add_single_bid},
    {"OrderBook::add_single_ask",            test_add_single_ask},
    {"OrderBook::price_time_priority",       test_price_time_priority},
    {"OrderBook::price_priority_bids",       test_price_priority_bids},
    {"OrderBook::no_match_spread",           test_no_match_when_spread_exists},
    {"OrderBook::full_fill_removes",         test_full_fill_removes_from_book},
    {"OrderBook::partial_fill",              test_partial_fill},
    {"OrderBook::cancel_order",              test_cancel_order},
    {"OrderBook::cancel_nonexistent",        test_cancel_nonexistent},
    {"OrderBook::market_order",              test_market_order},
    {"OrderBook::market_sweeps_levels",      test_market_order_sweeps_multiple_levels},
    {"OrderBook::ioc_no_fill",               test_ioc_no_fill},
    {"OrderBook::modify_order",              test_modify_order},
    {"OrderBook::bbo_spread",                test_bbo_spread},
};

// We call these from test_main's registry via a global constructor trick
struct ObTestRegistrar {
    ObTestRegistrar() {
        // Directly run and report; test_main provides the harness
        // This approach avoids needing a shared header for TEST() macro
        for (auto& [name, fn] : ob_tests) {
            std::printf("  %-50s ", name.c_str());
            try { fn(); std::printf("PASS\n"); }
            catch (const std::exception& e) { std::printf("FAIL: %s\n", e.what()); }
        }
    }
};

// Trick: run via global constructor before main exits
// (In a real project, use a proper test framework like Catch2 or gtest)
static int run_ob_tests() {
    std::printf("\n[OrderBook Tests]\n");
    for (auto& [name, fn] : ob_tests) {
        std::printf("  %-50s ", name.c_str());
        std::fflush(stdout);
        try { fn(); std::printf("PASS\n"); }
        catch (const std::exception& e) { std::printf("FAIL: %s\n", e.what()); return 1; }
    }
    return 0;
}
int ob_test_result = run_ob_tests();
