#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  NanoMatch – PoolAllocator
//
//  A slab-based, O(1) alloc/free arena that eliminates malloc() on the
//  critical path.
//
//  Design:
//    • Pre-allocates one large contiguous block (mmap'd, locked into RAM)
//    • Maintains a free-list as an intrusive stack of indices
//    • Zero heap fragmentation; deterministic latency
//    • Thread-unsafe by design – each thread has its own pool
// ─────────────────────────────────────────────────────────────────────────────
#include <cstddef>
#include <cstdint>
#include <cassert>
#include <sys/mman.h>
#include <stdexcept>
#include <array>
#include <string>

namespace nm::mem {

// ── Slab ─────────────────────────────────────────────────────────────────────
// A typed, fixed-capacity pool.  T must be at least sizeof(uint32_t).
template<typename T, std::size_t Capacity>
class SlabPool {
    static_assert(sizeof(T) >= sizeof(uint32_t), "T too small for freelist");
    static_assert(Capacity <= (1u << 24), "Capacity exceeds 16M slots");

public:
    SlabPool() {
        // mmap anonymous memory – bypasses glibc allocator & gets huge pages
        void* raw = ::mmap(
            nullptr,
            sizeof(T) * Capacity,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
            -1, 0
        );
        if (raw == MAP_FAILED) {
            throw std::runtime_error("SlabPool: mmap failed");
        }
        // Attempt transparent huge pages
        ::madvise(raw, sizeof(T) * Capacity, MADV_HUGEPAGE);

        data_ = static_cast<T*>(raw);

        // Lock pages into physical RAM – no page-fault jitter
        ::mlock(data_, sizeof(T) * Capacity);

        // Build freelist: slot i points to i+1
        free_head_ = 0;
        for (uint32_t i = 0; i < Capacity - 1; ++i) {
            *reinterpret_cast<uint32_t*>(&data_[i]) = i + 1;
        }
        *reinterpret_cast<uint32_t*>(&data_[Capacity - 1]) = INVALID;
        free_count_ = Capacity;
    }

    ~SlabPool() {
        ::munmap(data_, sizeof(T) * Capacity);
    }

    // Non-copyable, non-movable (pointer stability matters)
    SlabPool(const SlabPool&) = delete;
    SlabPool& operator=(const SlabPool&) = delete;

    // ── O(1) allocate ──────────────────────────────────────────────────────
    [[nodiscard]] T* allocate() noexcept {
        if (__builtin_expect(free_head_ == INVALID, 0)) return nullptr;
        uint32_t idx = free_head_;
        free_head_ = *reinterpret_cast<uint32_t*>(&data_[idx]);
        --free_count_;
        return &data_[idx];
    }

    // ── O(1) free ──────────────────────────────────────────────────────────
    void deallocate(T* ptr) noexcept {
        assert(ptr >= data_ && ptr < data_ + Capacity && "ptr out of range");
        uint32_t idx = static_cast<uint32_t>(ptr - data_);
        *reinterpret_cast<uint32_t*>(ptr) = free_head_;
        free_head_ = idx;
        ++free_count_;
    }

    [[nodiscard]] std::size_t free_slots()  const noexcept { return free_count_; }
    [[nodiscard]] std::size_t used_slots()  const noexcept { return Capacity - free_count_; }
    [[nodiscard]] std::size_t capacity()    const noexcept { return Capacity; }

private:
    static constexpr uint32_t INVALID = ~uint32_t(0);
    T*        data_      = nullptr;
    uint32_t  free_head_ = INVALID;
    uint32_t  free_count_ = 0;
};

// ── Arena bumping allocator ───────────────────────────────────────────────────
// Fast bump-pointer arena for variable-length scratch buffers.
// Reset in O(1) by moving bump pointer back to base.
class BumpArena {
public:
    explicit BumpArena(std::size_t bytes) {
        void* raw = ::mmap(
            nullptr, bytes,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS,
            -1, 0
        );
        if (raw == MAP_FAILED) throw std::runtime_error("BumpArena: mmap failed");
        ::madvise(raw, bytes, MADV_HUGEPAGE);
        base_  = static_cast<uint8_t*>(raw);
        cursor_ = base_;
        end_    = base_ + bytes;
        cap_    = bytes;
    }

    ~BumpArena() { ::munmap(base_, cap_); }

    [[nodiscard]] void* allocate(std::size_t n, std::size_t align = 8) noexcept {
        uint8_t* aligned = reinterpret_cast<uint8_t*>(
            (reinterpret_cast<uintptr_t>(cursor_) + align - 1) & ~(align - 1)
        );
        if (__builtin_expect(aligned + n > end_, 0)) return nullptr;
        cursor_ = aligned + n;
        return aligned;
    }

    void reset() noexcept { cursor_ = base_; }

    [[nodiscard]] std::size_t used()  const noexcept { return cursor_ - base_; }
    [[nodiscard]] std::size_t avail() const noexcept { return end_ - cursor_; }

private:
    uint8_t* base_   = nullptr;
    uint8_t* cursor_ = nullptr;
    uint8_t* end_    = nullptr;
    std::size_t cap_ = 0;
};

} // namespace nm::mem
