#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  NanoMatch – CSV Order Parser
// ─────────────────────────────────────────────────────────────────────────────
#include <core/types.hpp>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <cstdint>

namespace nm::io {

struct CSVOrder {
    Timestamp  ts;
    OrderId    id;
    uint32_t   symbol_id;
    Side       side;
    OrderType  type;
    Price      price;
    Qty        qty;
    TIF        tif;
};

using CSVCallback = std::function<void(const CSVOrder&)>;

class CSVParser {
public:
    explicit CSVParser(bool has_header = true) : has_header_(has_header) {}

    uint64_t parse(const char* buf, std::size_t len, const CSVCallback& cb);
    uint64_t parse_file(const char* path, const CSVCallback& cb);

    void register_symbol(std::string_view sym, uint32_t id) {
        symbol_map_.emplace(std::string(sym), id);
    }

    uint64_t rows_parsed()  const noexcept { return rows_parsed_;  }
    uint64_t rows_skipped() const noexcept { return rows_skipped_; }

private:
    static uint64_t fast_atou(const char* p, const char* end) noexcept;
    static int64_t  fast_atoi(const char* p, const char* end) noexcept;

    bool     parse_row(const char* line, std::size_t len, CSVOrder& out) noexcept;
    uint32_t symbol_to_id(std::string_view sym) noexcept;

    bool     has_header_   = true;
    uint64_t rows_parsed_  = 0;
    uint64_t rows_skipped_ = 0;
    uint32_t next_sym_id_  = 1;
    std::unordered_map<std::string, uint32_t> symbol_map_;
};

} // namespace nm::io
