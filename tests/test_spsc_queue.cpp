#include <gtest/gtest.h>
#include "engine/spsc_queue.hpp"
#include <thread>
#include <vector>

TEST(SPSCQueue, PushPopSingleThread) {
    SPSCQueue<int, 8> q;
    EXPECT_TRUE(q.empty());
    EXPECT_TRUE(q.push(42));
    EXPECT_FALSE(q.empty());
    auto v = q.pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 42);
    EXPECT_TRUE(q.empty());
}

TEST(SPSCQueue, PopEmptyReturnsNullopt) {
    SPSCQueue<int, 8> q;
    EXPECT_FALSE(q.pop().has_value());
}

TEST(SPSCQueue, FillThenDrain) {
    SPSCQueue<int, 4> q; // capacity 4, usable 3 (ring buffer)
    EXPECT_TRUE(q.push(1));
    EXPECT_TRUE(q.push(2));
    EXPECT_TRUE(q.push(3));
    EXPECT_FALSE(q.push(4)); // full
    EXPECT_EQ(*q.pop(), 1);
    EXPECT_EQ(*q.pop(), 2);
    EXPECT_EQ(*q.pop(), 3);
    EXPECT_FALSE(q.pop().has_value());
}

TEST(SPSCQueue, FIFOOrdering) {
    SPSCQueue<int, 16> q;
    for (int i = 0; i < 10; ++i) q.push(i);
    for (int i = 0; i < 10; ++i)
        EXPECT_EQ(*q.pop(), i);
}

TEST(SPSCQueue, ProducerConsumerThread) {
    SPSCQueue<int, 1024> q;
    constexpr int N = 500;
    std::vector<int> received;
    received.reserve(N);

    std::thread producer([&] {
        for (int i = 0; i < N; ++i) {
            while (!q.push(i)) {} // spin on full
        }
    });

    std::thread consumer([&] {
        int count = 0;
        while (count < N) {
            if (auto v = q.pop()) { received.push_back(*v); ++count; }
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), static_cast<size_t>(N));
    for (int i = 0; i < N; ++i)
        EXPECT_EQ(received[i], i);
}
