// ─────────────────────────────────────────────────────────────────────────────
//  NanoMatch – Benchmark: Parsers
// ─────────────────────────────────────────────────────────────────────────────
#include <benchmark/benchmark.h>
#include <io/csv_parser.hpp>
#include <sstream>

using namespace nm::io;

// ── Generate synthetic CSV in-memory ─────────────────────────────────────────
static std::string make_csv(int rows) {
    std::string s;
    s.reserve(rows * 60);
    s += "timestamp_ns,order_id,symbol,side,type,price,qty,tif\n";
    for (int i = 0; i < rows; ++i) {
        s += std::to_string(i * 1000);
        s += ',';
        s += std::to_string(i + 1);
        s += ",AAPL,";
        s += (i & 1) ? 'B' : 'S';
        s += ",L,1500000,100,GTC\n";
    }
    return s;
}

static void BM_CSVParse_100k(benchmark::State& state) {
    std::string csv = make_csv(100000);
    CSVParser parser;
    uint64_t total = 0;

    for (auto _ : state) {
        total = 0;
        parser.parse(csv.data(), csv.size(), [&](const CSVOrder&){ ++total; });
    }
    benchmark::DoNotOptimize(total);
    state.SetItemsProcessed(state.iterations() * 100000);
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(csv.size()));
}
BENCHMARK(BM_CSVParse_100k);
