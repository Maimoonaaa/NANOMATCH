// ─────────────────────────────────────────────────────────────────────────────
//  NanoMatch – CSV Parser Implementation
// ─────────────────────────────────────────────────────────────────────────────
#include <io/csv_parser.hpp>
#include <io/itch_parser.hpp>   // MMapFile
#include <cstring>
#include <string>

namespace nm::io {

uint64_t CSVParser::fast_atou(const char* p, const char* end) noexcept {
    uint64_t v = 0;
    while (p < end && static_cast<unsigned>(*p - '0') <= 9u)
        v = v * 10u + static_cast<unsigned>(*p++ - '0');
    return v;
}

int64_t CSVParser::fast_atoi(const char* p, const char* end) noexcept {
    if (p < end && *p == '-')
        return -static_cast<int64_t>(fast_atou(p + 1, end));
    return static_cast<int64_t>(fast_atou(p, end));
}

uint32_t CSVParser::symbol_to_id(std::string_view sym) noexcept {
    std::string key(sym);
    auto it = symbol_map_.find(key);
    if (it != symbol_map_.end()) return it->second;
    uint32_t id = next_sym_id_++;
    symbol_map_.emplace(std::move(key), id);
    return id;
}

// CSV: timestamp_ns,order_id,symbol,side,type,price,qty,tif
bool CSVParser::parse_row(const char* line, std::size_t len, CSVOrder& out) noexcept {
    const char* p   = line;
    const char* end = line + len;

    // Skip carriage return if present
    while (end > p && (end[-1] == '\r' || end[-1] == '\n')) --end;

    auto field_end = [&](const char* s) -> const char* {
        while (s < end && *s != ',') ++s;
        return s;
    };

    // 0: timestamp_ns
    const char* fe = field_end(p);
    if (fe >= end) return false;
    out.ts = static_cast<Timestamp>(fast_atou(p, fe));
    p = fe + 1;

    // 1: order_id
    fe = field_end(p);
    if (fe >= end) return false;
    out.id = static_cast<OrderId>(fast_atou(p, fe));
    p = fe + 1;

    // 2: symbol
    fe = field_end(p);
    if (fe >= end) return false;
    out.symbol_id = symbol_to_id(std::string_view(p, static_cast<std::size_t>(fe - p)));
    p = fe + 1;

    // 3: side
    fe = field_end(p);
    if (fe >= end) return false;
    out.side = (*p == 'B' || *p == 'b') ? Side::Buy : Side::Sell;
    p = fe + 1;

    // 4: type
    fe = field_end(p);
    if (fe >= end) return false;
    switch (*p) {
        case 'L': case 'l': out.type = OrderType::Limit;  break;
        case 'M': case 'm': out.type = OrderType::Market; break;
        case 'C': case 'c': out.type = OrderType::Cancel; break;
        default:             out.type = OrderType::Limit;  break;
    }
    p = fe + 1;

    // 5: price
    fe = field_end(p);
    if (fe >= end) return false;
    out.price = fast_atoi(p, fe);
    p = fe + 1;

    // 6: qty
    fe = field_end(p);
    out.qty = static_cast<Qty>(fast_atou(p, fe));

    // 7: tif (optional)
    out.tif = TIF::GTC;
    if (fe < end) {
        p = fe + 1;
        fe = field_end(p);
        if (p < fe) {
            if      (p[0] == 'I' || p[0] == 'i') out.tif = TIF::IOC;
            else if (p[0] == 'F' || p[0] == 'f') out.tif = TIF::FOK;
        }
    }

    return out.qty > 0 || out.type == OrderType::Cancel;
}

uint64_t CSVParser::parse(const char* buf, std::size_t len, const CSVCallback& cb) {
    const char* p   = buf;
    const char* end = buf + len;

    if (has_header_ && p < end) {
        while (p < end && *p != '\n') ++p;
        if (p < end) ++p;
    }

    CSVOrder row{};
    while (p < end) {
        const char* line_end = p;
        while (line_end < end && *line_end != '\n') ++line_end;
        std::size_t line_len = static_cast<std::size_t>(line_end - p);
        if (line_len > 0 && *p != '#') {
            if (parse_row(p, line_len, row)) {
                cb(row);
                ++rows_parsed_;
            } else {
                ++rows_skipped_;
            }
        }
        p = line_end + (line_end < end ? 1 : 0);
    }
    return rows_parsed_;
}

uint64_t CSVParser::parse_file(const char* path, const CSVCallback& cb) {
    MMapFile f(path);
    return parse(reinterpret_cast<const char*>(f.data()), f.size(), cb);
}

} // namespace nm::io
