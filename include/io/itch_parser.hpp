#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  NanoMatch – NASDAQ TotalView-ITCH 5.0 Parser
//
//  Zero-copy binary parser for .pcap/.itch files.
//  ITCH is a one-directional protocol: NASDAQ → subscriber.
//  We extract Add-Order, Execute, Cancel, Replace messages.
//
//  Zero-copy design:
//    • mmap() the file – OS maps it into address space, no read() calls
//    • Parser walks the raw memory buffer directly
//    • No allocation per message – fills caller-provided Order struct
//
//  Reference: https://www.nasdaqtrader.com/content/technicalsupport/
//             specifications/dataproducts/NQTVITCHSpecification.pdf
// ─────────────────────────────────────────────────────────────────────────────
#include <core/types.hpp>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string_view>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdexcept>

namespace nm::io {

// ── ITCH 5.0 message types (single-byte tag) ─────────────────────────────────
enum class ITCHMsgType : char {
    AddOrder       = 'A',
    AddOrderMPID   = 'F',
    ExecuteOrder   = 'E',
    ExecuteOrderPx = 'C',
    ReduceOrder    = 'X',
    DeleteOrder    = 'D',
    ReplaceOrder   = 'U',
    Trade          = 'P',
    SystemEvent    = 'S',
    StockDirectory = 'R',
};

// ── Parsed event union (avoids dynamic dispatch) ──────────────────────────────
enum class EventKind : uint8_t { Add, Execute, Cancel, Reduce, Replace, Trade, Unknown };

struct ParsedEvent {
    EventKind kind = EventKind::Unknown;
    OrderId   order_ref;
    OrderId   new_order_ref;   // for Replace
    Price     price;
    Qty       qty;
    Side      side;
    uint32_t  symbol_id;
    Timestamp ts;              // nanoseconds of day
};

// ── Callback signature ────────────────────────────────────────────────────────
using EventCallback = std::function<void(const ParsedEvent&)>;

// ─────────────────────────────────────────────────────────────────────────────
//  MMapFile – RAII mmap wrapper
// ─────────────────────────────────────────────────────────────────────────────
class MMapFile {
public:
    explicit MMapFile(const char* path) {
        fd_ = ::open(path, O_RDONLY);
        if (fd_ < 0) throw std::runtime_error(std::string("Cannot open: ") + path);

        struct stat st{};
        if (::fstat(fd_, &st) < 0) throw std::runtime_error("fstat failed");
        size_ = static_cast<std::size_t>(st.st_size);

        data_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd_, 0);
        if (data_ == MAP_FAILED) throw std::runtime_error("mmap failed");

        // Advise sequential access pattern → prefetch aggressively
        ::madvise(data_, size_, MADV_SEQUENTIAL | MADV_WILLNEED);
    }

    ~MMapFile() {
        if (data_ && data_ != MAP_FAILED) ::munmap(data_, size_);
        if (fd_ >= 0) ::close(fd_);
    }

    MMapFile(const MMapFile&) = delete;
    MMapFile& operator=(const MMapFile&) = delete;

    [[nodiscard]] const uint8_t* data() const noexcept { return static_cast<const uint8_t*>(data_); }
    [[nodiscard]] std::size_t    size() const noexcept { return size_; }

private:
    int         fd_   = -1;
    void*       data_ = nullptr;
    std::size_t size_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
//  ITCHParser
// ─────────────────────────────────────────────────────────────────────────────
class ITCHParser {
public:
    ITCHParser() = default;

    // Parse a raw ITCH binary buffer and fire callback for each message.
    // buf: pointer to mmap'd file data
    // len: size in bytes
    // Returns number of messages parsed.
    uint64_t parse(const uint8_t* buf, std::size_t len, const EventCallback& cb);

    // Convenience: open a file with mmap and parse it
    uint64_t parse_file(const char* path, const EventCallback& cb);

    struct Stats {
        uint64_t total_messages;
        uint64_t add_orders;
        uint64_t executes;
        uint64_t cancels;
        uint64_t replaces;
        uint64_t unknown;
    };
    [[nodiscard]] Stats stats() const noexcept { return stats_; }

private:
    // ── Network-byte-order (big-endian) readers ─────────────────────────────
    static uint16_t be16(const uint8_t* p) noexcept {
        return static_cast<uint16_t>(p[0] << 8 | p[1]);
    }
    static uint32_t be32(const uint8_t* p) noexcept {
        return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
    }
    static uint64_t be64(const uint8_t* p) noexcept {
        return (uint64_t)be32(p)<<32 | be32(p+4);
    }
    static uint64_t be48(const uint8_t* p) noexcept {
        return (uint64_t)be16(p)<<32 | be32(p+2);
    }

    bool parse_add_order(const uint8_t* msg, std::size_t len, ParsedEvent& ev) noexcept;
    bool parse_execute  (const uint8_t* msg, std::size_t len, ParsedEvent& ev) noexcept;
    bool parse_cancel   (const uint8_t* msg, std::size_t len, ParsedEvent& ev) noexcept;
    bool parse_replace  (const uint8_t* msg, std::size_t len, ParsedEvent& ev) noexcept;

    Stats stats_{};
};

} // namespace nm::io
