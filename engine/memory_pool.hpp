#pragma once
#include <array>
#include <atomic>
#include <cstddef>

// Lock-free slab allocator. Uses a Treiber stack (atomic CAS free-list).
// T must have a `T* next` member for intrusive linking.
template<typename T, std::size_t Capacity>
class MemoryPool {
public:
    MemoryPool() noexcept {
        for (std::size_t i = 0; i + 1 < Capacity; ++i)
            storage_[i].next = &storage_[i + 1];
        storage_[Capacity - 1].next = nullptr;
        head_.store(&storage_[0], std::memory_order_relaxed);
    }

    T* allocate() noexcept {
        // compare_exchange_weak: single CPU instruction (lock cmpxchg), no syscall.
        // acquire: ensures we see the full object before using it.
        T* old = head_.load(std::memory_order_acquire);
        while (old) {
            if (head_.compare_exchange_weak(old, old->next,
                    std::memory_order_release,
                    std::memory_order_acquire))
                return old;
        }
        return nullptr;
    }

    void deallocate(T* node) noexcept {
        node->next = head_.load(std::memory_order_relaxed);
        while (!head_.compare_exchange_weak(node->next, node,
                std::memory_order_release,
                std::memory_order_relaxed));
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    // storage_ and head_ on separate cache lines: a write to head_ by one thread
    // does not invalidate the storage_ cache line read by another (false sharing).
    alignas(64) std::array<T, Capacity> storage_;
    alignas(64) std::atomic<T*>         head_{nullptr};
};
