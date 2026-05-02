#include <gtest/gtest.h>
#include "engine/memory_pool.hpp"
#include "engine/order_types.hpp"

TEST(MemoryPool, AllocateAndDeallocate) {
    MemoryPool<Order, 4> pool;
    Order* a = pool.allocate();
    Order* b = pool.allocate();
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a, b);

    pool.deallocate(a);
    Order* c = pool.allocate(); // should reuse a's slot
    EXPECT_NE(c, nullptr);
}

TEST(MemoryPool, ExhaustThenFail) {
    MemoryPool<Order, 2> pool;
    Order* a = pool.allocate();
    Order* b = pool.allocate();
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(pool.allocate(), nullptr); // exhausted
    pool.deallocate(a);
    EXPECT_NE(pool.allocate(), nullptr); // one freed slot
}

TEST(MemoryPool, AllSlotsDistinct) {
    constexpr std::size_t N = 64;
    MemoryPool<Order, N> pool;
    std::vector<Order*> ptrs;
    ptrs.reserve(N);
    for (std::size_t i = 0; i < N; ++i) {
        Order* p = pool.allocate();
        ASSERT_NE(p, nullptr);
        ptrs.push_back(p);
    }
    // All pointers must be unique
    std::sort(ptrs.begin(), ptrs.end());
    auto it = std::adjacent_find(ptrs.begin(), ptrs.end());
    EXPECT_EQ(it, ptrs.end());
}

TEST(MemoryPool, Capacity) {
    EXPECT_EQ((MemoryPool<Order, 128>::capacity()), 128u);
}
