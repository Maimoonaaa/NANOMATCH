# NanoMatch — Ultra-Low Latency Order Matching Engine



---

## Verified Performance (CPU-only, no GPU)

| Metric | Result |
|---|---|
| Throughput | **11.6M orders/sec** |
| p50 latency | **36 ns** |
| p90 latency | **42 ns** |
| p99 latency | **171 ns** |
| Order struct size | **64 B** (exactly 1 cache line) |
| PriceLevel struct size | **64 B** (exactly 1 cache line) |

---

## What Is This?

NanoMatch is a production-grade **Limit Order Book (LOB)** engine written in C++20. It processes buy and sell orders for financial exchanges at sub-microsecond latency by eliminating every source of non-deterministic delay:

- No `malloc()` on the hot path — custom slab memory pool with `mmap`
- No cache misses — contiguous price-level arrays, cache-line-aligned structs
- No mutex on the trade log path — SPSC lock-free ring buffer
- No system calls during matching — zero-copy `mmap` for data ingestion
- No heap fragmentation — fixed-capacity slab arena

---

## Project Structure

```
nanomatch/
├── include/                     # All headers (the public API)
│   ├── core/
│   │   ├── types.hpp            # Order, Trade, Price, Side primitives
│   │   ├── order_book.hpp       # Limit Order Book class
│   │   └── matching_engine.hpp  # Engine coordinator + stats
│   ├── memory/
│   │   └── pool_allocator.hpp   # SlabPool<T,N> + BumpArena
│   ├── io/
│   │   ├── itch_parser.hpp      # NASDAQ TotalView-ITCH 5.0 binary parser
│   │   └── csv_parser.hpp       # Synthetic CSV parser
│   └── concurrency/
│       └── spsc_ring_buffer.hpp # Wait-free SPSC queue
│
├── src/                         # Implementations
│   ├── core/
│   │   ├── order_book.cpp
│   │   └── matching_engine.cpp
│   ├── io/
│   │   ├── itch_parser.cpp
│   │   └── csv_parser.cpp
│   ├── memory/pool_allocator.cpp
│   ├── concurrency/spsc_ring_buffer.cpp
│   └── main.cpp                 # CLI entry point
│
├── tests/                       # Unit tests (no external framework)
│   ├── test_main.cpp
│   ├── test_order_book.cpp      # 15 correctness tests
│   ├── test_memory_pool.cpp     # 6 allocator tests
│   ├── test_spsc.cpp            # 7 concurrency tests (incl. 100k concurrent)
│   └── test_parser.cpp          # 7 parser tests
│
├── benchmarks/                  # Google Benchmark suites
│   ├── bench_order_book.cpp
│   ├── bench_memory.cpp
│   └── bench_parser.cpp
│
├── scripts/
│   └── generate_orders.py       # Synthetic order generator (GBM price model)
│
├── data/                        # Data directory (gitignored)
│   └── .gitkeep
│
├── docs/
│   └── BENCHMARK_REPORT.md
│   └── HFT_MARRATIVE.md
├── build.sh                     # One-command build script
├── CMakeLists.txt               # CMake build system
└── README.md
```

---

## Quick Start

### Prerequisites

```bash
# Ubuntu / Debian
sudo apt install g++ cmake python3

# Optional for Google Benchmark targets
sudo apt install libbenchmark-dev

# Optional for profiling
sudo apt install linux-perf
```

### Build & Run

```bash
# Clone and build
git clone https://github.com/YOUR_USERNAME/nanomatch
cd nanomatch

# Run the built-in demo (no data needed)
./build.sh release

# Run all unit tests
./build.sh test

# Throughput benchmark (1M orders)
./build.sh bench

# Generate 1M synthetic orders and replay
./build.sh gen
./build.sh replay

# Debug build with AddressSanitizer + UBSanitizer
./build.sh debug
```

### CMake (alternative)

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
./nanomatch --demo
./nanomatch_tests
```

---

## Architecture Deep Dive

### 1. Price Representation

All prices are **fixed-point integers** with 4 decimal places:  
`Price = USD_value × 10,000`  
e.g., $150.0100 = `1500100`

This avoids floating-point rounding and allows integer comparison on the hot path.

### 2. Order Struct (64 bytes, 1 cache line)

```
┌──────────────────────────────────────────────────────────────────┐
│ id(8) │ price(8) │ qty(8) │ remaining(8) │ ts(8) │ sym(4)       │
│ side(1) │ type(1) │ tif(1) │ _pad[17] ← stores next/prev ptrs   │
└──────────────────────────────────────────────────────────────────┘
```

The `_pad` field is re-used by the OrderBook as an **intrusive doubly-linked list** — no separate node allocation needed.

### 3. PriceLevel (64 bytes, 1 cache line)

```
┌──────────────────────────────────────────────────────────────────┐
│ price(8) │ total_qty(8) │ head*(8) │ tail*(8) │ order_cnt(4)    │
│ _pad[28]                                                         │
└──────────────────────────────────────────────────────────────────┘
```

Each price level owns a FIFO intrusive linked list of `Order*`. Accessing the head order (for matching) always touches exactly one additional cache line.

### 4. Price→Index Mapping (O(1))

```cpp
int32_t price_to_idx(Price p) { return p / PRICE_TICK_UNITS; }
// PRICE_TICK_UNITS = 100 → 1 index unit = $0.01
// 64K levels covers $0 to $655.35
```

Array access replaces any tree lookup. Best-bid / best-ask tracked with a single integer cursor, updated incrementally.

### 5. Memory Pool (SlabPool)

```
┌─────────────────────────────────────────────────────────────────┐
│  mmap(64MB)  →  1M × 64B Order slots, locked into RAM (mlock)  │
│                                                                  │
│  Free list: intrusive singly-linked stack of slot indices       │
│  alloc(): pop head (2 ops)   free(): push head (2 ops)          │
└─────────────────────────────────────────────────────────────────┘
```

### 6. SPSC Trade Log

```
Producer (Matching Thread)          Consumer (Log Thread)
      │                                     │
      │  try_push(trade)                    │  try_pop()
      ▼                                     ▼
┌─────────────────────────────────────────────────────────────────┐
│  head_ [cache line 0]  │  tail_ [cache line 1]  │  buf[1M]      │
│  atomic release store  │  atomic acquire load   │               │
└─────────────────────────────────────────────────────────────────┘
```

### 7. Matching Algorithm

```
add_limit_order(BUY @ $150.01, qty=500):
  1. Allocate Order from SlabPool           O(1)
  2. Walk ask levels from best_ask down:    O(k levels swept)
     For each level:
       FIFO walk orders (time priority)
       Execute trades, decrement qty
  3. If remaining > 0 AND GTC:
       Insert at tail of bid_levels[idx]    O(1)
       Update best_bid_idx cursor           O(1)
       Register in order_map               O(1) amortised
```

---

## Data Sources

### NASDAQ TotalView-ITCH

Download sample `.pcap` files from:  
https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/

```bash
./nanomatch_bin --itch data/sample.itch
```

### Synthetic Generator

```bash
# 5M rows, 5 symbols, custom seed
python3 scripts/generate_orders.py -n 5000000 -s AAPL GOOG MSFT AMZN NVDA --seed 1337
./nanomatch_bin --csv data/orders.csv
```

---

## Benchmarking

### Built-in rdtsc bench

```bash
./build.sh bench
```

### Google Benchmark (if installed)

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make nanomatch_bench
./nanomatch_bench --benchmark_format=json > results.json
```

### Linux perf (CPU flame graph)

```bash
./build.sh perf
# Then generate flame graph:
perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg
```

### Intel VTune

```bash
vtune -collect hotspots -result-dir vtune_result -- ./nanomatch_bin --bench
vtune -report hotspots -result-dir vtune_result
```

---

## Concepts Demonstrated

| Concept | Where |
|---|---|
| Cache locality (L1/L2/L3) | `SlabPool`, `PriceLevel` array layout |
| Custom memory pools | `include/memory/pool_allocator.hpp` |
| Lock-free concurrency | `include/concurrency/spsc_ring_buffer.hpp` |
| Zero-copy I/O via mmap | `MMapFile` in `include/io/itch_parser.hpp` |
| Struct packing / false-sharing prevention | `PriceLevel`, `SPSCRingBuffer` cache-line separation |
| Branch misprediction hints | `__builtin_expect` throughout hot paths |
| RDTSC timing | `ClockCalib` in `include/core/matching_engine.hpp` |
| Intrusive data structures | Order doubly-linked list inside `_pad` |
| Fixed-point arithmetic | `Price = int64_t` with 4 decimal places |

---

