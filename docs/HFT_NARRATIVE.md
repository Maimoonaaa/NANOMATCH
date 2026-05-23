# NanoMatch — The HFT Narrative

> *"The most brilliant alpha model is completely useless if the underlying system
> executing the trade is a microsecond too slow."*

---

## The Problem, From Scratch

### What is a Stock Exchange?

A stock exchange is a marketplace where buyers and sellers of shares meet. If you
want to buy 100 shares of Apple at $150, and I want to sell 100 shares at $150,
a trade happens. The piece of software that decides *when* a trade happens, *who*
it happens between, and *at what price* is called the **Order Matching Engine**.

Every exchange in the world — NYSE, NASDAQ, BSE, NSE — has one at its core.
It is the most performance-critical piece of software in finance.

### What is a Limit Order Book?

The **Limit Order Book (LOB)** is the data structure at the heart of the matching
engine. It maintains two sorted lists:

```
ASKS (sellers, sorted lowest price first):
  $150.02 × 800 shares  ← best ask
  $150.03 × 600 shares
  $150.05 × 400 shares
  ...

── SPREAD ($0.02) ──

BIDS (buyers, sorted highest price first):
  $150.00 × 800 shares  ← best bid  (500 + 300 at same price)
  $149.99 × 1000 shares
  $149.98 × 200 shares
  ...
```

**A trade happens** when a new order's price is aggressive enough to cross
the spread — a buy order priced at or above the best ask, or a sell order priced
at or below the best bid.

**Price-time priority**: if two buyers both want to buy at $150.00, whoever
submitted their order *first* gets matched first. This is the fundamental fairness
rule of all exchanges.

### Why Is Speed So Critical?

In High-Frequency Trading (HFT), firms co-locate their servers in the same
data centre as the exchange. The physical distance is measured in metres. The
race to be first to react to a market event is measured in **nanoseconds**.

A firm whose matching engine takes 1 microsecond longer to process an order
than a competitor will lose every single trade opportunity. Over millions of
trades per day, that difference is worth billions of dollars.

---

## What NanoMatch Builds

NanoMatch is a fully functional, production-grade LOB engine that processes
**9.3 million orders per second** with a **p50 latency of 36–154 ns** (depending
on book depth), built entirely in C++20 with no external runtime dependencies.

---

## Architecture: Every Decision Explained

### Decision 1: Fixed-Point Price Representation

**Problem:** Floating-point arithmetic (`double`) has rounding errors and
non-deterministic latency (FP exceptions, denormals).

**Solution:** All prices are stored as `int64_t` with 4 implied decimal places:
```
$150.0100 → 1500100   (integer)
$150.0000 → 1500000
```
Integer comparison is a single CPU instruction. No rounding. No FP unit pressure.

### Decision 2: Flat Array Instead of a Tree

**The naive approach:** Use `std::map<Price, Queue<Order>>` — a red-black tree.
- Lookup: O(log N) — fine for small N, terrible for cache
- Each tree node is a separate heap allocation → pointer chasing → cache miss

**The NanoMatch approach:** Flat array of `PriceLevel` structs, indexed by price:
```cpp
int32_t price_to_idx(Price p) { return p / 100; }
// $150.01 → idx 15001
// Access: bid_levels_[15001]  ← one array dereference
```
- Lookup: O(1) — single array index
- Layout: contiguous in memory → hardware prefetcher can predict accesses
- Best bid/ask: a single integer cursor, updated incrementally

64K price levels × 64 bytes each = 4MB per side. Fits in L3 cache.

### Decision 3: Cache-Line-Aligned Structs

Every access to an `Order` or `PriceLevel` in a loop touches exactly **one
cache line** (64 bytes). No wasted loads, no false sharing.

```
PriceLevel (64 bytes, 1 cache line):
┌─────────────────────────────────────────────────────────────────┐
│ price(8) │ total_qty(8) │ head*(8) │ tail*(8) │ cnt(4) │ pad   │
└─────────────────────────────────────────────────────────────────┘

Order (64 bytes, 1 cache line):
┌─────────────────────────────────────────────────────────────────┐
│ id(8) │ price(8) │ qty(8) │ remaining(8) │ ts(8) │ sym(4)      │
│ side(1) │ type(1) │ tif(1) │ ← next*(8) + prev*(8) in pad      │
└─────────────────────────────────────────────────────────────────┘
```

The `_pad` field is repurposed as an **intrusive doubly-linked list** — the
`next` and `prev` pointers for the FIFO queue at each price level are stored
*inside the Order's padding bytes*. No separate list node allocation needed.

### Decision 4: SlabPool — No malloc() on the Hot Path

**Problem:** `malloc()` / `new` takes 50–200 ns and is non-deterministic
(it may call the OS, lock global state, or trigger a garbage collection).

**Solution:** Pre-allocate 1 million `Order` slots at startup using `mmap()`.
Maintain a free-list as an intrusive stack of slot indices:

```
alloc():  pop free-list head  → 2 instructions, ~5 ns
free():   push free-list head → 2 instructions, ~5 ns
```

The entire slab is `mlock()`'d into RAM — no page-fault jitter during trading.
`MADV_HUGEPAGE` requests 2MB huge pages to reduce TLB pressure.

### Decision 5: Zero-Copy Data Ingestion with mmap()

**Problem:** `fread()` copies file data from kernel buffer to user buffer — one
unnecessary copy, plus a syscall per chunk.

**Solution:** `mmap()` maps the file directly into the process address space.
The parser walks raw memory:
```cpp
const uint8_t* p = mmap_file.data();  // no copy — this IS the file
while (p < end) {
    // parse directly from mapped memory
}
```
For a 1GB ITCH file, this saves copying 1GB of data.

### Decision 6: SPSC Lock-Free Trade Log

**Problem:** Writing executed trades to disk/network on the matching thread
blocks the hot path. A mutex makes this deterministic but slow.

**Solution:** Single-Producer/Single-Consumer (SPSC) ring buffer. The matching
thread pushes `Trade` structs; a dedicated log thread drains them. No mutex.
No syscall. Just two atomic stores with Release/Acquire semantics.

```
Producer (matching thread)    Consumer (log thread)
        │                             │
        │  head_ [cache line 0]       │  tail_ [cache line 1]
        │  atomic release store       │  atomic acquire load
        ▼                             ▼
        ┌─────────────────────────────────────────┐
        │  circular buffer of 1M Trade structs    │
        └─────────────────────────────────────────┘
```

`head_` and `tail_` are on **separate cache lines** — otherwise both cores
would fight over the same 64-byte line (false sharing → MESI protocol
invalidations → 10–100x overhead).

### Decision 7: RDTSC Clock

**Problem:** `clock_gettime()` is a syscall or vDSO call — 20–50 ns overhead
per call. Unacceptable for measuring 36 ns operations.

**Solution:** `rdtsc` (Read Time-Stamp Counter) is a single CPU instruction:
```asm
rdtsc   ; returns 64-bit tick count in edx:eax
```
Calibrated against `std::chrono::steady_clock` at startup to convert ticks → ns.
Zero syscall overhead. ~3 ns per call.

---

## Matching Algorithm Walkthrough

Given a new BUY limit order at $150.01 for 500 shares:

```
1. Allocate Order from SlabPool                        O(1)  ~5 ns
2. Check best ask (ask_levels_[best_ask_idx_]):        O(1)  ~2 ns
   best_ask = $150.00 < $150.01 → PRICE CROSSES
3. Walk FIFO queue at ask level $150.00:               O(k orders at level)
   - Maker order A: 400 shares → fill 400, maker fully filled
     - Fire trade callback → push to SPSC ring
     - Remove maker from level, deallocate to SlabPool
   - Taker has 100 shares remaining
   - Maker order B: 200 shares → fill 100, maker partially filled
     - Fire trade callback
   - Taker fully filled
4. Taker remaining = 0 → deallocate to SlabPool       O(1)  ~5 ns
   Total: ~36 ns for a non-crossing order
         ~80–200 ns for a crossing order (1–3 levels swept)
```

---

## Hardware Specs Used for Testing

| Component        | Value                           |
|------------------|---------------------------------|
| CPU              | Intel Xeon @ 2.80 GHz           |
| L1 cache         | 32 KB (data) per core           |
| L2 cache         | 256 KB per core                 |
| L3 cache         | 33 MB shared                    |
| RAM              | DDR4 (shared container environment) |
| OS               | Ubuntu 24.04 LTS                |
| Kernel           | Linux 6.x x86_64                |
| Compiler         | GCC 13.3.0                      |
| Optimisation     | `-O3 -march=native -fno-omit-frame-pointer` |
| C++ Standard     | C++20                           |

> **Important:** Results were obtained in a **shared container** with a single
> vCPU, no CPU pinning, no real-time scheduling, and no NUMA optimisation.
> On a dedicated bare-metal server with `isolcpus=2`, `taskset -c 2`, and
> `chrt --fifo 99`, expect p50 < 50 ns and p99 < 500 ns consistently.

---

## Benchmark Reproduction

```bash
# Full build + demo
./build.sh release

# Throughput benchmark (1M orders)
./build.sh bench

# Generate 1M synthetic orders (GBM price model)
./build.sh gen

# Replay synthetic dataset
./nanomatch_bin --csv data/orders.csv

# Unit tests (22 tests, 0 failures)
./build.sh test

# Cache behaviour proof (no perf counters needed)
g++ -std=c++20 -O3 -march=native scripts/cache_proof.cpp -o cache_proof
./cache_proof

# Debug build: AddressSanitizer + UBSanitizer
./build.sh debug
```

### For Full Profiling (on your Linux machine)

```bash
# CPU flame graph
chmod +x scripts/profile.sh
./scripts/profile.sh flamegraph
# → opens docs/flamegraph_cpu.svg in browser

# L1/L2/L3 cache miss rates via perf
./scripts/profile.sh cache
# Expected: L1-dcache-miss-rate < 1%, LLC-miss-rate < 5%

# Intel VTune (if installed)
./scripts/profile.sh vtune
```

---

## Concepts Demonstrated

| Concept | Where in Code |
|---------|---------------|
| Cache locality (L1/L2/L3) | `PriceLevel` array layout, `Order` struct packing |
| Custom memory pools & arenas | `include/memory/pool_allocator.hpp` |
| Lock-free concurrency & memory ordering | `include/concurrency/spsc_ring_buffer.hpp` |
| Zero-copy I/O via mmap | `MMapFile` in `include/io/itch_parser.hpp` |
| Struct packing & false-sharing prevention | `PriceLevel` (64B), `SPSCRingBuffer` pad fields |
| Branch misprediction hints | `__builtin_expect` on all slow paths |
| RDTSC & pipeline hazards | `ClockCalib`, `rdtsc()` in `matching_engine.hpp` |
| Fixed-point arithmetic | `Price = int64_t × 10000` in `types.hpp` |
| Intrusive data structures | `Order::_pad` reused for linked-list pointers |
| Price-time priority | FIFO intrusive queue per `PriceLevel` |

---

## File Map (What to Read First)

1. `include/core/types.hpp` — start here. Understand `Order`, `Trade`, `Price`.
2. `include/core/order_book.hpp` — the LOB interface and `PriceLevel` struct.
3. `src/core/order_book.cpp` — the matching kernel (`match_order`).
4. `include/memory/pool_allocator.hpp` — `SlabPool<T,N>`.
5. `include/concurrency/spsc_ring_buffer.hpp` — the lock-free queue.
6. `include/io/itch_parser.hpp` — zero-copy binary parser.
7. `src/main.cpp` — CLI and threading model.
