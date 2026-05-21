// ─────────────────────────────────────────────────────────────────────────────
//  NanoMatch – SPSC Ring Buffer Unit Tests
// ─────────────────────────────────────────────────────────────────────────────
#include <concurrency/spsc_ring_buffer.hpp>
#include <core/types.hpp>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include <functional>

using namespace nm;
using namespace nm::conc;

#define ASSERT_TRUE(expr) \
    do { if (!(expr)) throw std::runtime_error("ASSERT_TRUE(" #expr ") at line " + std::to_string(__LINE__)); } while(0)
#define ASSERT_EQ(a,b) \
    do { if (!((a)==(b))) throw std::runtime_error(std::string("ASSERT_EQ: ") + std::to_string((long long)(a)) + " != " + std::to_string((long long)(b))); } while(0)

namespace {

void test_spsc_empty() {
    SPSCRingBuffer<int, 16> rb;
    ASSERT_TRUE(rb.empty());
    ASSERT_EQ(rb.size(), 0ULL);
}

void test_spsc_push_pop() {
    SPSCRingBuffer<int, 16> rb;
    bool ok = rb.try_push(42);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(!rb.empty());
    auto v = rb.try_pop();
    ASSERT_TRUE(v.has_value());
    ASSERT_EQ(*v, 42);
    ASSERT_TRUE(rb.empty());
}

void test_spsc_fifo_order() {
    SPSCRingBuffer<int, 32> rb;
    for (int i = 0; i < 10; ++i) rb.try_push(i);
    for (int i = 0; i < 10; ++i) {
        auto v = rb.try_pop();
        ASSERT_TRUE(v.has_value());
        ASSERT_EQ(*v, i);
    }
}

void test_spsc_full() {
    SPSCRingBuffer<int, 8> rb;  // capacity = 7 (ring buffer loses one slot)
    int pushed = 0;
    while (rb.try_push(pushed)) ++pushed;
    // Should have pushed capacity-1 = 7 items
    ASSERT_TRUE(pushed > 0 && pushed < 8);
}

void test_spsc_pop_empty() {
    SPSCRingBuffer<int, 16> rb;
    auto v = rb.try_pop();
    ASSERT_TRUE(!v.has_value());
}

void test_spsc_trade_roundtrip() {
    SPSCRingBuffer<Trade, 1024> rb;
    Trade t{};
    t.maker_id = 1; t.taker_id = 2; t.price = 100'000; t.qty = 500;
    bool ok = rb.try_push(t);
    ASSERT_TRUE(ok);
    auto result = rb.try_pop();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->maker_id, 1ULL);
    ASSERT_EQ(result->qty, 500ULL);
}

void test_spsc_concurrent() {
    // Single-producer, single-consumer – the canonical use case
    SPSCRingBuffer<uint64_t, 1 << 16> rb;
    constexpr int N = 100000;
    std::atomic<bool> done{false};
    std::vector<uint64_t> received;
    received.reserve(N);

    std::thread consumer([&] {
        while (received.size() < N) {
            auto v = rb.try_pop();
            if (v) received.push_back(*v);
        }
        done.store(true);
    });

    for (int i = 0; i < N; ++i) {
        while (!rb.try_push(static_cast<uint64_t>(i)))
            __builtin_ia32_pause();
    }

    consumer.join();

    ASSERT_EQ(received.size(), (std::size_t)N);
    for (int i = 0; i < N; ++i) {
        ASSERT_EQ(received[i], (uint64_t)i);
    }
}

} // namespace

static std::vector<std::pair<std::string, std::function<void()>>> spsc_tests = {
    {"SPSC::empty",               test_spsc_empty},
    {"SPSC::push_pop",            test_spsc_push_pop},
    {"SPSC::fifo_order",          test_spsc_fifo_order},
    {"SPSC::full",                test_spsc_full},
    {"SPSC::pop_empty",           test_spsc_pop_empty},
    {"SPSC::trade_roundtrip",     test_spsc_trade_roundtrip},
    {"SPSC::concurrent_100k",     test_spsc_concurrent},
};

static int run_spsc_tests() {
    std::printf("\n[SPSC Ring Buffer Tests]\n");
    for (auto& [name, fn] : spsc_tests) {
        std::printf("  %-50s ", name.c_str());
        std::fflush(stdout);
        try { fn(); std::printf("PASS\n"); }
        catch (const std::exception& e) { std::printf("FAIL: %s\n", e.what()); return 1; }
    }
    return 0;
}
int spsc_test_result = run_spsc_tests();
