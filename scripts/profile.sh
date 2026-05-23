#!/usr/bin/env bash
# =============================================================================
#  NanoMatch — Visual Profiling Evidence
#
#  This script generates CPU flame graphs and cache-miss evidence.
#  Run this on your own Linux machine (not in a container).
#
#  Prerequisites:
#    sudo apt install linux-perf git
#    git clone https://github.com/brendangregg/FlameGraph ~/FlameGraph
#
#  Usage:
#    chmod +x scripts/profile.sh
#    ./scripts/profile.sh flamegraph     # CPU flame graph
#    ./scripts/profile.sh cache          # Cache miss analysis
#    ./scripts/profile.sh vtune          # Intel VTune hotspots
#    ./scripts/profile.sh all            # Everything
# =============================================================================
set -euo pipefail

CXX=${CXX:-g++}
FLAMEGRAPH_DIR=${FLAMEGRAPH_DIR:-~/FlameGraph}
INCLUDES="-Iinclude"
COMMON="-std=c++20 -pthread $INCLUDES"
ALL_SRC="src/core/order_book.cpp src/core/matching_engine.cpp \
         src/io/itch_parser.cpp src/io/csv_parser.cpp \
         src/memory/pool_allocator.cpp src/concurrency/spsc_ring_buffer.cpp"

cmd=${1:-all}

build_perf_binary() {
    echo "▶ Building with frame pointers (required for perf)..."
    $CXX $COMMON -O3 -march=native -fno-omit-frame-pointer -g \
        $ALL_SRC src/main.cpp -o nanomatch_perf
    echo "✓ nanomatch_perf built"
}

check_perf() {
    if ! command -v perf &>/dev/null; then
        echo "ERROR: perf not found. Install: sudo apt install linux-perf"
        exit 1
    fi
    local paranoid
    paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 3)
    if [ "$paranoid" -gt 1 ]; then
        echo "WARNING: perf_event_paranoid=$paranoid — lowering to 1..."
        echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid
    fi
}

flamegraph() {
    build_perf_binary
    check_perf

    echo "▶ Recording CPU profile (10 seconds @ 9999 Hz)..."
    perf record -g -F 9999 --call-graph dwarf \
        taskset -c 0 ./nanomatch_perf --bench 2>/dev/null

    echo "▶ Generating flame graph..."
    if [ ! -d "$FLAMEGRAPH_DIR" ]; then
        echo "Cloning FlameGraph tools..."
        git clone https://github.com/brendangregg/FlameGraph "$FLAMEGRAPH_DIR"
    fi

    perf script | "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" \
                | "$FLAMEGRAPH_DIR/flamegraph.pl" \
                    --title "NanoMatch CPU Flame Graph" \
                    --width 1600 \
                > docs/flamegraph_cpu.svg

    echo "✓ docs/flamegraph_cpu.svg"
    echo "  Open in browser to see hot functions."
    echo ""
    echo "  Expected hot functions:"
    echo "    nm::OrderBook::match_order      (matching kernel)"
    echo "    nm::OrderBook::execute_trade    (trade execution)"
    echo "    nm::mem::SlabPool::allocate     (O(1) pool alloc)"
    echo "    nm::mem::SlabPool::deallocate   (O(1) pool free)"
}

cache_analysis() {
    build_perf_binary
    check_perf

    echo "▶ Measuring L1/L2/L3 cache miss rates..."
    mkdir -p docs

    perf stat -e \
        cache-references,cache-misses,\
        L1-dcache-loads,L1-dcache-load-misses,\
        L1-dcache-stores,L1-dcache-store-misses,\
        LLC-loads,LLC-load-misses \
        ./nanomatch_perf --bench 2>&1 | tee docs/cache_stats.txt

    echo ""
    echo "✓ docs/cache_stats.txt"
    echo ""
    echo "  Interpreting results:"
    echo "    L1-dcache-load-misses / L1-dcache-loads < 1%  → excellent cache locality"
    echo "    LLC-load-misses / LLC-loads              < 5%  → fits in L3"
    echo ""
    echo "  NanoMatch design choices that achieve this:"
    echo "    1. PriceLevel array (64B each) → sequential access = prefetcher-friendly"
    echo "    2. Order SlabPool (mmap contiguous) → no scattered heap nodes"
    echo "    3. Order struct = 64B (1 cache line) → no partial-line loads"
    echo "    4. SPSC head_/tail_ on separate cache lines → no false sharing"
}

vtune_profile() {
    if ! command -v vtune &>/dev/null; then
        echo "ERROR: vtune not found."
        echo "Download: https://www.intel.com/content/www/us/en/developer/tools/oneapi/vtune-profiler.html"
        exit 1
    fi
    build_perf_binary
    echo "▶ Running VTune hotspot analysis..."
    mkdir -p docs/vtune_result
    vtune -collect hotspots \
          -knob sampling-mode=hw \
          -result-dir docs/vtune_result \
          -- ./nanomatch_perf --bench 2>/dev/null
    vtune -report hotspots \
          -result-dir docs/vtune_result \
          -format text \
          > docs/vtune_hotspots.txt
    echo "✓ docs/vtune_hotspots.txt"
    echo "✓ docs/vtune_result/ (open with VTune GUI for flame graph + memory access patterns)"
}

run_cache_proof() {
    echo "▶ Building cache behaviour proof..."
    $CXX $COMMON -O3 -march=native scripts/cache_proof.cpp -o /tmp/nm_cache_proof
    /tmp/nm_cache_proof | tee docs/cache_proof_output.txt
    echo "✓ docs/cache_proof_output.txt"
}

case "$cmd" in
    flamegraph) flamegraph ;;
    cache)      cache_analysis ;;
    vtune)      vtune_profile ;;
    proof)      run_cache_proof ;;
    all)
        echo "=== NanoMatch Full Profiling Suite ==="
        flamegraph
        cache_analysis
        echo "=== Done. Check docs/ for all outputs ==="
        ;;
    *)
        echo "Usage: $0 [flamegraph|cache|vtune|proof|all]"
        exit 1
        ;;
esac
