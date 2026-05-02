#pragma once
#include <array>
#include <atomic>
#include <optional>
#include <cstddef>

// Lock-free single-producer single-consumer ring buffer.
// Capacity must be a power of two: index wrap uses bitmask (& MASK) instead of
// modulo, which avoids a division instruction on every push/pop.
template<typename T, std::size_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
    static constexpr std::size_t MASK = Capacity - 1;

public:
    bool push(T item) noexcept {
        const std::size_t h      = head_.load(std::memory_order_relaxed);
        const std::size_t next_h = (h + 1) & MASK;
        if (next_h == tail_.load(std::memory_order_acquire))
            return false;
        buffer_[h] = item;
        // release: guarantees the buffer write is visible before the index update.
        head_.store(next_h, std::memory_order_release);
        return true;
    }

    std::optional<T> pop() noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire))
            return std::nullopt;
        T item = buffer_[t];
        // release: guarantees the item read completes before we advance tail_.
        tail_.store((t + 1) & MASK, std::memory_order_release);
        return item;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

private:
    std::array<T, Capacity> buffer_;
    // head_ written by producer, tail_ written by consumer.
    // Separate cache lines prevent a write by one thread from invalidating
    // the other thread's cache line (false sharing).
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};
