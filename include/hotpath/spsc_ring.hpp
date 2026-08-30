#pragma once

// A bounded ring with one producer and one consumer, which is the shape the telemetry path has:
// the executor's read loop pushes and the writer thread pops, and neither ever swaps roles. That
// restriction is what removes the CAS. Each side owns one index and only ever reads the other's,
// so a release store on one side paired with an acquire load on the other is the whole protocol.
//
// The slots are allocated once, in the constructor, so a push copies into storage that already
// exists. Nothing on the producer's side allocates, takes a lock, or makes a syscall.

#include <atomic>
#include <bit>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace hotpath {

// std::hardware_destructive_interference_size is not offered by every standard library this
// builds against. 64 bytes is the line on x86-64 and on Apple silicon.
inline constexpr std::size_t kCacheLineBytes = 64;

template <typename T>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>,
                  "a slot is overwritten in place by the producer and read by the consumer, so it "
                  "cannot own anything that needs a destructor");

public:
    // Rounded up to a power of two so the wrap is a mask. A capacity below two leaves no slot to
    // distinguish empty from full.
    explicit SpscRing(std::size_t capacity)
        : slots_(std::bit_ceil(capacity < 2 ? std::size_t{2} : capacity)),
          mask_(slots_.size() - 1) {}

    // False when the ring is full. The caller decides what a dropped row costs; this never waits.
    [[nodiscard]] bool try_push(const T& value) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_cache_ >= slots_.size()) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (head - tail_cache_ >= slots_.size()) {
                return false;
            }
        }
        slots_[head & mask_] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_cache_) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (tail == head_cache_) {
                return false;
            }
        }
        out = slots_[tail & mask_];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Rows pushed but not yet popped. Either side may call it, and the answer is stale the moment
    // it is returned; it exists to report a backlog, not to decide anything.
    [[nodiscard]] std::size_t size() const noexcept {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

private:
    // The indices are monotonic counters rather than wrapped positions, so `head - tail` is the
    // occupancy without a spare slot and without an ambiguous equality. They are unsigned and the
    // subtraction stays correct across their own overflow.
    alignas(kCacheLineBytes) std::atomic<std::size_t> head_{0};
    std::size_t tail_cache_ = 0;

    alignas(kCacheLineBytes) std::atomic<std::size_t> tail_{0};
    std::size_t head_cache_ = 0;

    alignas(kCacheLineBytes) std::vector<T> slots_;
    std::size_t mask_;
};

}  // namespace hotpath
