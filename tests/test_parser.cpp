// ─────────────────────────────────────────────────────────────────────────────
//  NanoMatch – Parser Unit Tests
// ─────────────────────────────────────────────────────────────────────────────
#include <io/csv_parser.hpp>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <string>
#include <functional>
#include <cstring>

using namespace nm;
using namespace nm::io;

#define ASSERT_TRUE(expr) \
    do { if (!(expr)) throw std::runtime_error("ASSERT_TRUE(" #expr ") at line " + std::to_string(__LINE__)); } while(0)
#define ASSERT_EQ(a,b) \
    do { if (!((a)==(b))) throw std::runtime_error(std::string("ASSERT_EQ: ") + std::to_string((long long)(a)) + " != " + std::to_string((long long)(b))); } while(0)

namespace {

static const char* SAMPLE_CSV =
    "timestamp_ns,order_id,symbol,side,type,price,qty,tif\n"
    "1000,1,AAPL,B,L,1500000,100,GTC\n"
    "2000,2,AAPL,S,L,1500100,200,GTC\n"
    "3000,3,GOOG,B,M,0,50,IOC\n"
    "4000,4,AAPL,S,C,0,0,GTC\n";

void test_csv_row_count() {
    CSVParser parser;
    std::vector<CSVOrder> orders;
    parser.parse(SAMPLE_CSV, strlen(SAMPLE_CSV),
        [&](const CSVOrder& o){ orders.push_back(o); });
    ASSERT_EQ(orders.size(), 4ULL);  // all 4 rows including cancel
}

void test_csv_side_parsing() {
    CSVParser parser;
    std::vector<CSVOrder> orders;
    parser.parse(SAMPLE_CSV, strlen(SAMPLE_CSV),
        [&](const CSVOrder& o){ orders.push_back(o); });
    ASSERT_TRUE(orders[0].side == Side::Buy);
    ASSERT_TRUE(orders[1].side == Side::Sell);
}

void test_csv_type_parsing() {
    CSVParser parser;
    std::vector<CSVOrder> orders;
    parser.parse(SAMPLE_CSV, strlen(SAMPLE_CSV),
        [&](const CSVOrder& o){ orders.push_back(o); });
    ASSERT_TRUE(orders[0].type == OrderType::Limit);
    ASSERT_TRUE(orders[2].type == OrderType::Market);
}

void test_csv_price_parsing() {
    CSVParser parser;
    std::vector<CSVOrder> orders;
    parser.parse(SAMPLE_CSV, strlen(SAMPLE_CSV),
        [&](const CSVOrder& o){ orders.push_back(o); });
    ASSERT_EQ(orders[0].price, 1500000LL);  // $150.0000
    ASSERT_EQ(orders[1].price, 1500100LL);  // $150.0100
}

void test_csv_tif_parsing() {
    CSVParser parser;
    std::vector<CSVOrder> orders;
    parser.parse(SAMPLE_CSV, strlen(SAMPLE_CSV),
        [&](const CSVOrder& o){ orders.push_back(o); });
    ASSERT_TRUE(orders[0].tif == TIF::GTC);
    ASSERT_TRUE(orders[2].tif == TIF::IOC);
}

void test_csv_symbol_consistency() {
    // Both AAPL orders should get same symbol_id
    CSVParser parser;
    std::vector<CSVOrder> orders;
    parser.parse(SAMPLE_CSV, strlen(SAMPLE_CSV),
        [&](const CSVOrder& o){ orders.push_back(o); });
    ASSERT_EQ(orders[0].symbol_id, orders[1].symbol_id);  // both AAPL
    ASSERT_TRUE(orders[0].symbol_id != orders[2].symbol_id);  // AAPL != GOOG
}

void test_csv_large_batch() {
    // Generate 10k rows and verify count
    std::string csv = "timestamp_ns,order_id,symbol,side,type,price,qty,tif\n";
    for (int i = 0; i < 10000; ++i) {
        csv += std::to_string(i) + "," + std::to_string(i+1) +
               ",AAPL,B,L,1500000,100,GTC\n";
    }
    CSVParser parser;
    uint64_t count = 0;
    parser.parse(csv.data(), csv.size(), [&](const CSVOrder&){ ++count; });
    ASSERT_EQ(count, 10000ULL);
}

} // namespace

static std::vector<std::pair<std::string, std::function<void()>>> parser_tests = {
    {"CSVParser::row_count",            test_csv_row_count},
    {"CSVParser::side_parsing",         test_csv_side_parsing},
    {"CSVParser::type_parsing",         test_csv_type_parsing},
    {"CSVParser::price_parsing",        test_csv_price_parsing},
    {"CSVParser::tif_parsing",          test_csv_tif_parsing},
    {"CSVParser::symbol_consistency",   test_csv_symbol_consistency},
    {"CSVParser::large_batch_10k",      test_csv_large_batch},
};

static int run_parser_tests() {
    std::printf("\n[Parser Tests]\n");
    for (auto& [name, fn] : parser_tests) {
        std::printf("  %-50s ", name.c_str());
        std::fflush(stdout);
        try { fn(); std::printf("PASS\n"); }
        catch (const std::exception& e) { std::printf("FAIL: %s\n", e.what()); return 1; }
    }
    return 0;
}
int parser_test_result = run_parser_tests();
