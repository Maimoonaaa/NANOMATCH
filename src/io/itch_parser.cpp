// ─────────────────────────────────────────────────────────────────────────────
//  NanoMatch – ITCH 5.0 Parser Implementation
// ─────────────────────────────────────────────────────────────────────────────
#include <io/itch_parser.hpp>
#include <cstring>
#include <cstdio>

namespace nm::io {

// ITCH 5.0 message lengths (bytes, excluding the 2-byte length prefix)
static constexpr uint16_t ADD_ORDER_LEN    = 36;
static constexpr uint16_t ADD_ORDER_MPID   = 40;
static constexpr uint16_t EXEC_ORDER_LEN   = 31;
static constexpr uint16_t EXEC_ORDER_PX    = 35;
static constexpr uint16_t CANCEL_ORDER_LEN = 23;
static constexpr uint16_t DELETE_ORDER_LEN = 19;
static constexpr uint16_t REPLACE_LEN      = 35;

// ── Symbol → id helper ────────────────────────────────────────────────────────
static uint32_t itch_sym_to_id(const uint8_t* sym8) noexcept {
    // ITCH packs 8-char symbol, right-padded with spaces
    // Hash it into a 32-bit id (FNV-1a)
    uint32_t h = 2166136261u;
    for (int i = 0; i < 8; ++i) {
        if (sym8[i] == ' ') break;
        h ^= sym8[i];
        h *= 16777619u;
    }
    return h ? h : 1u;
}

// ─────────────────────────────────────────────────────────────────────────────
uint64_t ITCHParser::parse(const uint8_t* buf, std::size_t len,
                            const EventCallback& cb) {
    const uint8_t* p   = buf;
    const uint8_t* end = buf + len;

    while (p + 2 <= end) {
        uint16_t msg_len = be16(p);
        p += 2;

        if (__builtin_expect(p + msg_len > end, 0)) break;

        char msg_type = static_cast<char>(*p);
        ParsedEvent ev{};
        bool valid = false;

        switch (static_cast<ITCHMsgType>(msg_type)) {
            case ITCHMsgType::AddOrder:
                valid = parse_add_order(p, msg_len, ev);
                if (valid) ++stats_.add_orders;
                break;
            case ITCHMsgType::AddOrderMPID:
                valid = parse_add_order(p, msg_len, ev);   // same layout for first 36 bytes
                if (valid) ++stats_.add_orders;
                break;
            case ITCHMsgType::ExecuteOrder:
            case ITCHMsgType::ExecuteOrderPx:
                valid = parse_execute(p, msg_len, ev);
                if (valid) ++stats_.executes;
                break;
            case ITCHMsgType::ReduceOrder:
            case ITCHMsgType::DeleteOrder:
                valid = parse_cancel(p, msg_len, ev);
                if (valid) ++stats_.cancels;
                break;
            case ITCHMsgType::ReplaceOrder:
                valid = parse_replace(p, msg_len, ev);
                if (valid) ++stats_.replaces;
                break;
            default:
                ++stats_.unknown;
                break;
        }

        if (valid) {
            cb(ev);
            ++stats_.total_messages;
        }

        p += msg_len;
    }

    return stats_.total_messages;
}

uint64_t ITCHParser::parse_file(const char* path, const EventCallback& cb) {
    MMapFile f(path);
    return parse(f.data(), f.size(), cb);
}

// ── Add Order (type 'A') ──────────────────────────────────────────────────────
// Offset  Len  Field
//   0      1   Message Type ('A')
//   1      2   Stock Locate
//   3      2   Tracking Number
//   5      6   Timestamp (ns of day, big-endian 48-bit)
//  11      8   Order Reference Number
//  19      1   Buy/Sell Indicator
//  20      4   Shares
//  24      8   Stock (right-padded)
//  32      4   Price (fixed-point * 10000)
bool ITCHParser::parse_add_order(const uint8_t* msg, std::size_t len, ParsedEvent& ev) noexcept {
    if (len < 36) return false;

    ev.kind      = EventKind::Add;
    ev.ts        = be48(msg + 5);
    ev.order_ref = be64(msg + 11);
    ev.side      = (msg[19] == 'B') ? Side::Buy : Side::Sell;
    ev.qty       = be32(msg + 20);
    ev.symbol_id = itch_sym_to_id(msg + 24);
    // ITCH price is uint32 with 4 implied decimal places (same as our PRICE_SCALE)
    ev.price     = static_cast<Price>(be32(msg + 32));
    return ev.qty > 0 && ev.price > 0;
}

// ── Execute Order (type 'E') ──────────────────────────────────────────────────
// We treat execution = partial/full cancel of maker (the book handles it)
bool ITCHParser::parse_execute(const uint8_t* msg, std::size_t len, ParsedEvent& ev) noexcept {
    if (len < 31) return false;
    ev.kind      = EventKind::Execute;
    ev.ts        = be48(msg + 5);
    ev.order_ref = be64(msg + 11);
    ev.qty       = be32(msg + 19);
    return true;
}

// ── Cancel / Delete ───────────────────────────────────────────────────────────
bool ITCHParser::parse_cancel(const uint8_t* msg, std::size_t len, ParsedEvent& ev) noexcept {
    if (len < 19) return false;
    ev.kind      = EventKind::Cancel;
    ev.ts        = be48(msg + 5);
    ev.order_ref = be64(msg + 11);
    return true;
}

// ── Replace Order (type 'U') ──────────────────────────────────────────────────
// Offset  Len  Field
//   0      1   'U'
//   5      6   Timestamp
//  11      8   Original Ref
//  19      8   New Ref
//  27      4   Shares
//  31      4   Price
bool ITCHParser::parse_replace(const uint8_t* msg, std::size_t len, ParsedEvent& ev) noexcept {
    if (len < 35) return false;
    ev.kind          = EventKind::Replace;
    ev.ts            = be48(msg + 5);
    ev.order_ref     = be64(msg + 11);
    ev.new_order_ref = be64(msg + 19);
    ev.qty           = be32(msg + 27);
    ev.price         = static_cast<Price>(be32(msg + 31));
    return ev.qty > 0 && ev.price > 0;
}

} // namespace nm::io
