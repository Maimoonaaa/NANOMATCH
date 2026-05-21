cmake_minimum_required(VERSION 3.20)
project(NanoMatch VERSION 1.0.0 LANGUAGES CXX)

# ─── C++17 / 20 ────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── Build type ────────────────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ─── Compiler flags ────────────────────────────────────────────────────────────
# Aggressive optimisation; hardware-tuned; no UB; no false-sharing
set(COMMON_FLAGS
    -Wall -Wextra -Wpedantic
    -march=native                     # use every ISA extension available
    -mtune=native
    -O3
    -fno-omit-frame-pointer           # keep frames for perf/VTune
    -funroll-loops
    -ffast-math
)

set(RELEASE_FLAGS
    ${COMMON_FLAGS}
    -DNDEBUG
)

set(DEBUG_FLAGS
    ${COMMON_FLAGS}
    -g3
    -fsanitize=address,undefined
)

add_compile_options(
    "$<$<CONFIG:Release>:${RELEASE_FLAGS}>"
    "$<$<CONFIG:Debug>:${DEBUG_FLAGS}>"
)

add_link_options(
    "$<$<CONFIG:Debug>:-fsanitize=address,undefined>"
)

# ─── Dependencies ──────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)

# Google Benchmark (optional – graceful degradation)
find_package(benchmark QUIET)

# ─── Core library ──────────────────────────────────────────────────────────────
add_library(nanomatch_core STATIC
    src/core/order_book.cpp
    src/core/matching_engine.cpp
    src/memory/pool_allocator.cpp
    src/io/itch_parser.cpp
    src/io/csv_parser.cpp
    src/concurrency/spsc_ring_buffer.cpp
)

target_include_directories(nanomatch_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(nanomatch_core PUBLIC Threads::Threads)

# ─── Main executable ───────────────────────────────────────────────────────────
add_executable(nanomatch src/main.cpp)
target_link_libraries(nanomatch PRIVATE nanomatch_core)

# ─── Benchmarks ────────────────────────────────────────────────────────────────
if(benchmark_FOUND)
    add_executable(nanomatch_bench
        benchmarks/bench_order_book.cpp
        benchmarks/bench_memory.cpp
        benchmarks/bench_parser.cpp
    )
    target_link_libraries(nanomatch_bench PRIVATE nanomatch_core benchmark::benchmark benchmark::benchmark_main)
    message(STATUS "Google Benchmark found – benchmark targets enabled")
else()
    message(WARNING "Google Benchmark not found – skipping benchmark targets. Install with: sudo apt install libbenchmark-dev")
endif()

# ─── Unit tests ────────────────────────────────────────────────────────────────
enable_testing()
add_executable(nanomatch_tests
    tests/test_order_book.cpp
    tests/test_memory_pool.cpp
    tests/test_spsc.cpp
    tests/test_parser.cpp
    tests/test_main.cpp
)
target_link_libraries(nanomatch_tests PRIVATE nanomatch_core)
add_test(NAME unit_tests COMMAND nanomatch_tests)

# ─── Install ───────────────────────────────────────────────────────────────────
install(TARGETS nanomatch DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)
