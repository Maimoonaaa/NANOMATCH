#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  NanoMatch – build.sh
#  One-shot build script.  All targets built from source.
#
#  Usage:
#    ./build.sh                  # Release build + run demo
#    ./build.sh debug            # Debug + ASan/UBSan build
#    ./build.sh test             # Build and run unit tests
#    ./build.sh bench            # Release build and run throughput bench
#    ./build.sh gen              # Generate 1M-row synthetic CSV
#    ./build.sh perf             # Linux perf profiling run
#    ./build.sh clean            # Remove all build artefacts
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

CXX=${CXX:-g++}
JOBS=$(nproc 2>/dev/null || echo 4)

SRC_CORE="src/core/order_book.cpp src/core/matching_engine.cpp"
SRC_IO="src/io/itch_parser.cpp src/io/csv_parser.cpp"
SRC_MEM="src/memory/pool_allocator.cpp"
SRC_CONC="src/concurrency/spsc_ring_buffer.cpp"
ALL_SRC="$SRC_CORE $SRC_IO $SRC_MEM $SRC_CONC"
INCLUDES="-Iinclude"
COMMON_FLAGS="$INCLUDES -std=c++20 -pthread"
RELEASE_FLAGS="-O3 -march=native -fno-omit-frame-pointer -DNDEBUG"
DEBUG_FLAGS="-O0 -g3 -fsanitize=address,undefined"

cmd=${1:-release}

case "$cmd" in
  release)
    echo "▶ Building release…"
    $CXX $COMMON_FLAGS $RELEASE_FLAGS $ALL_SRC src/main.cpp -o nanomatch_bin
    echo "✓ Built: ./nanomatch_bin"
    echo ""
    ./nanomatch_bin --demo
    ;;

  debug)
    echo "▶ Building debug (ASan + UBSan)…"
    $CXX $COMMON_FLAGS $DEBUG_FLAGS $ALL_SRC src/main.cpp -o nanomatch_debug
    echo "✓ Built: ./nanomatch_debug"
    ./nanomatch_debug --demo
    ;;

  test)
    echo "▶ Building unit tests…"
    $CXX $COMMON_FLAGS -O2 \
      $ALL_SRC \
      tests/test_main.cpp \
      tests/test_order_book.cpp \
      tests/test_memory_pool.cpp \
      tests/test_spsc.cpp \
      tests/test_parser.cpp \
      -o nanomatch_tests
    echo "✓ Running tests…"
    ./nanomatch_tests
    ;;

  bench)
    echo "▶ Building release for benchmark…"
    $CXX $COMMON_FLAGS $RELEASE_FLAGS $ALL_SRC src/main.cpp -o nanomatch_bin
    echo "✓ Running throughput benchmark (1M orders)…"
    ./nanomatch_bin --bench
    ;;

  gen)
    echo "▶ Generating synthetic order CSV (1M rows)…"
    mkdir -p data
    python3 scripts/generate_orders.py -o data/orders.csv -n 1000000
    echo "✓ data/orders.csv ready"
    ;;

  replay)
    echo "▶ Building release and replaying CSV…"
    $CXX $COMMON_FLAGS $RELEASE_FLAGS $ALL_SRC src/main.cpp -o nanomatch_bin
    if [ ! -f data/orders.csv ]; then
      echo "  Generating CSV first…"
      mkdir -p data
      python3 scripts/generate_orders.py -o data/orders.csv -n 1000000
    fi
    ./nanomatch_bin --csv data/orders.csv
    ;;

  perf)
    echo "▶ Building with perf-compatible flags…"
    $CXX $COMMON_FLAGS $RELEASE_FLAGS $ALL_SRC src/main.cpp -o nanomatch_bin
    echo "▶ Running under Linux perf (requires sudo or perf_event_paranoid ≤ 1)…"
    perf record -g -F 9999 ./nanomatch_bin --bench 2>/dev/null || \
      echo "  perf failed – try: echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid"
    perf report --stdio 2>/dev/null | head -40 || true
    ;;

  clean)
    echo "▶ Cleaning…"
    rm -f nanomatch_bin nanomatch_debug nanomatch_tests nanomatch_bench
    rm -rf build/ build_debug/
    echo "✓ Clean"
    ;;

  *)
    echo "Unknown command: $cmd"
    echo "Usage: $0 [release|debug|test|bench|gen|replay|perf|clean]"
    exit 1
    ;;
esac
