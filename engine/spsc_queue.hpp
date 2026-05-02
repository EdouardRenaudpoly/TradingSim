#pragma once
#include <array>
#include <atomic>
#include <optional>
#include <cstddef>

// Lock-free Single-Producer Single-Consumer ring buffer.
// Capacity must be a power of two (enables bitmask modulo — no division).
// head_ and tail_ on separate cache lines to eliminate false sharing.
template<typename T, std::size_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of 2");
    static constexpr std::size_t MASK = Capacity - 1;

public:
    // Called from producer thread only
    bool push(T item) noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t next_h = (h + 1) & MASK;
        if (next_h == tail_.load(std::memory_order_acquire))
            return false; // full
        buffer_[h] = item;
        head_.store(next_h, std::memory_order_release);
        return true;
    }

    // Called from consumer thread only
    std::optional<T> pop() noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire))
            return std::nullopt; // empty
        T item = buffer_[t];
        tail_.store((t + 1) & MASK, std::memory_order_release);
        return item;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

private:
    std::array<T, Capacity>      buffer_;
    alignas(64) std::atomic<std::size_t> head_{0}; // written by producer
    alignas(64) std::atomic<std::size_t> tail_{0}; // written by consumer
};
