// ─────────────────────────────────────────────────────────────────────────────
//  NanoMatch – Benchmark: Memory Allocators
//  Compares: SlabPool vs operator new vs malloc
// ─────────────────────────────────────────────────────────────────────────────
#include <benchmark/benchmark.h>
#include <memory/pool_allocator.hpp>
#include <core/types.hpp>

using namespace nm;
using namespace nm::mem;

// ── SlabPool alloc/free cycle ─────────────────────────────────────────────────
static void BM_SlabPool_AllocFree(benchmark::State& state) {
    SlabPool<Order, 1 << 20> pool;
    for (auto _ : state) {
        Order* o = pool.allocate();
        benchmark::DoNotOptimize(o);
        pool.deallocate(o);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel("slab_pool");
}
BENCHMARK(BM_SlabPool_AllocFree)->Iterations(5000000);

// ── new/delete cycle ──────────────────────────────────────────────────────────
static void BM_NewDelete_AllocFree(benchmark::State& state) {
    for (auto _ : state) {
        Order* o = new Order{};
        benchmark::DoNotOptimize(o);
        delete o;
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel("new_delete");
}
BENCHMARK(BM_NewDelete_AllocFree)->Iterations(5000000);

// ── malloc/free cycle ─────────────────────────────────────────────────────────
static void BM_MallocFree_AllocFree(benchmark::State& state) {
    for (auto _ : state) {
        void* p = std::malloc(sizeof(Order));
        benchmark::DoNotOptimize(p);
        std::free(p);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel("malloc_free");
}
BENCHMARK(BM_MallocFree_AllocFree)->Iterations(5000000);

// ── BumpArena alloc ───────────────────────────────────────────────────────────
static void BM_BumpArena_Alloc(benchmark::State& state) {
    BumpArena arena(64 * 1024 * 1024);  // 64 MB
    for (auto _ : state) {
        void* p = arena.allocate(64, 64);
        benchmark::DoNotOptimize(p);
        if (!p) arena.reset();
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel("bump_arena");
}
BENCHMARK(BM_BumpArena_Alloc)->Iterations(5000000);
