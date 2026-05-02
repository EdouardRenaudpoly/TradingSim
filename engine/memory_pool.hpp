#pragma once
#include <array>
#include <atomic>
#include <cstddef>

// Lock-free slab allocator using an atomic free-list (Treiber stack).
// T must expose a `T* next` member used for intrusive linking.
//
// ABA note: safe when a single thread allocates and another deallocates
// (SPSC pattern). For MPMC, add a generation counter.
template<typename T, std::size_t Capacity>
class MemoryPool {
public:
    MemoryPool() noexcept {
        for (std::size_t i = 0; i + 1 < Capacity; ++i)
            storage_[i].next = &storage_[i + 1];
        storage_[Capacity - 1].next = nullptr;
        head_.store(&storage_[0], std::memory_order_relaxed);
    }

    // Called from producer thread — O(1) amortised
    T* allocate() noexcept {
        T* old = head_.load(std::memory_order_acquire);
        while (old) {
            if (head_.compare_exchange_weak(old, old->next,
                    std::memory_order_release,
                    std::memory_order_acquire))
                return old;
        }
        return nullptr; // pool exhausted
    }

    // Called from consumer/matcher thread — O(1)
    void deallocate(T* node) noexcept {
        node->next = head_.load(std::memory_order_relaxed);
        while (!head_.compare_exchange_weak(node->next, node,
                std::memory_order_release,
                std::memory_order_relaxed));
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    // Separate cache lines: storage array + head pointer never share a line
    alignas(64) std::array<T, Capacity> storage_;
    alignas(64) std::atomic<T*>         head_{nullptr};
};
