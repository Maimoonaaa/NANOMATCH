// ─────────────────────────────────────────────────────────────────────────────
//  NanoMatch – Memory Pool Unit Tests
// ─────────────────────────────────────────────────────────────────────────────
#include <memory/pool_allocator.hpp>
#include <core/types.hpp>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <string>
#include <functional>

using namespace nm;
using namespace nm::mem;

#define ASSERT_TRUE(expr) \
    do { if (!(expr)) throw std::runtime_error("ASSERT_TRUE(" #expr ") at line " + std::to_string(__LINE__)); } while(0)
#define ASSERT_EQ(a,b) \
    do { if (!((a)==(b))) throw std::runtime_error(std::string("ASSERT_EQ: ") + std::to_string(a) + " != " + std::to_string(b)); } while(0)

namespace {

void test_pool_initial_state() {
    SlabPool<Order, 1024> pool;
    ASSERT_EQ(pool.capacity(), 1024ULL);
    ASSERT_EQ(pool.free_slots(), 1024ULL);
    ASSERT_EQ(pool.used_slots(), 0ULL);
}

void test_pool_alloc_dealloc() {
    SlabPool<Order, 1024> pool;
    Order* o = pool.allocate();
    ASSERT_TRUE(o != nullptr);
    ASSERT_EQ(pool.used_slots(), 1ULL);
    pool.deallocate(o);
    ASSERT_EQ(pool.used_slots(), 0ULL);
}

void test_pool_exhaustion() {
    SlabPool<Order, 4> pool;
    Order* ptrs[4];
    for (int i = 0; i < 4; ++i) {
        ptrs[i] = pool.allocate();
        ASSERT_TRUE(ptrs[i] != nullptr);
    }
    Order* extra = pool.allocate();
    ASSERT_TRUE(extra == nullptr);  // pool exhausted
    // Free one, re-allocate
    pool.deallocate(ptrs[0]);
    Order* reclaimed = pool.allocate();
    ASSERT_TRUE(reclaimed != nullptr);
    for (int i = 1; i < 4; ++i) pool.deallocate(ptrs[i]);
    pool.deallocate(reclaimed);
    ASSERT_EQ(pool.free_slots(), 4ULL);
}

void test_pool_no_overlap() {
    SlabPool<Order, 256> pool;
    Order* a = pool.allocate();
    Order* b = pool.allocate();
    ASSERT_TRUE(a != b);
    pool.deallocate(a);
    pool.deallocate(b);
}

void test_bump_arena_basic() {
    BumpArena arena(1024 * 1024);  // 1MB
    void* p1 = arena.allocate(64, 64);
    ASSERT_TRUE(p1 != nullptr);
    void* p2 = arena.allocate(64, 64);
    ASSERT_TRUE(p2 != nullptr);
    ASSERT_TRUE(p1 != p2);
    ASSERT_EQ(arena.used(), 128ULL);
}

void test_bump_arena_reset() {
    BumpArena arena(4096);
    arena.allocate(1024);
    ASSERT_EQ(arena.used(), 1024ULL);
    arena.reset();
    ASSERT_EQ(arena.used(), 0ULL);
}

} // namespace

static std::vector<std::pair<std::string, std::function<void()>>> mem_tests = {
    {"PoolAllocator::initial_state",     test_pool_initial_state},
    {"PoolAllocator::alloc_dealloc",     test_pool_alloc_dealloc},
    {"PoolAllocator::exhaustion",        test_pool_exhaustion},
    {"PoolAllocator::no_overlap",        test_pool_no_overlap},
    {"BumpArena::basic",                 test_bump_arena_basic},
    {"BumpArena::reset",                 test_bump_arena_reset},
};

static int run_mem_tests() {
    std::printf("\n[Memory Pool Tests]\n");
    for (auto& [name, fn] : mem_tests) {
        std::printf("  %-50s ", name.c_str());
        std::fflush(stdout);
        try { fn(); std::printf("PASS\n"); }
        catch (const std::exception& e) { std::printf("FAIL: %s\n", e.what()); return 1; }
    }
    return 0;
}
int mem_test_result = run_mem_tests();
