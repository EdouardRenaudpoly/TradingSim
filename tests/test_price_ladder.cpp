#include <gtest/gtest.h>
#include "engine/price_ladder.hpp"
#include "engine/memory_pool.hpp"

// Simple pool for tests
static MemoryPool<Order, 256> pool;

static Order* makeOrder(uint64_t id, uint64_t tid, const char* sym,
                         double price, int32_t qty, Side side,
                         OrderType type = OrderType::LIMIT) {
    Order* o   = pool.allocate();
    o->id       = id;
    o->trader_id= tid;
    o->price    = price;
    o->quantity = qty;
    o->remaining= qty;
    o->side     = side;
    o->type     = type;
    o->status   = OrderStatus::NEW;
    o->next     = nullptr;
    o->setSymbol(sym);
    return o;
}

TEST(PriceLadder, EmptyOnConstruct) {
    PriceLadder book;
    EXPECT_TRUE(book.empty());
    auto s = book.snapshot();
    EXPECT_EQ(s.bid_depth, 0);
    EXPECT_EQ(s.ask_depth, 0);
}

TEST(PriceLadder, NoMatchWhenSpreadPositive) {
    PriceLadder book;
    book.insert(makeOrder(1, 1, "AAPL", 149.0, 100, Side::BUY));
    book.insert(makeOrder(2, 2, "AAPL", 151.0, 100, Side::SELL));
    EXPECT_TRUE(book.match().empty());
    EXPECT_FALSE(book.empty());
}

TEST(PriceLadder, FullMatchBidCrossesAsk) {
    PriceLadder book;
    book.insert(makeOrder(1, 1, "AAPL", 151.0, 100, Side::BUY));
    book.insert(makeOrder(2, 2, "AAPL", 150.0, 100, Side::SELL));
    auto trades = book.match();
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 100);
    EXPECT_DOUBLE_EQ(trades[0].price, 150.0); // passive (ask) price
    EXPECT_EQ(trades[0].buyer_id,  1u);
    EXPECT_EQ(trades[0].seller_id, 2u);
    EXPECT_TRUE(book.empty());
}

TEST(PriceLadder, PartialFill) {
    PriceLadder book;
    book.insert(makeOrder(1, 1, "AAPL", 150.0, 100, Side::BUY));
    book.insert(makeOrder(2, 2, "AAPL", 150.0,  40, Side::SELL));
    auto trades = book.match();
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 40);
    // Bid still in book with 60 remaining
    auto s = book.snapshot();
    EXPECT_EQ(s.bid_depth, 60);
}

TEST(PriceLadder, PriceTimePriority) {
    PriceLadder book;
    // Two bids at same price — first inserted should match first
    auto* b1 = makeOrder(1, 1, "AAPL", 150.0, 50, Side::BUY);
    auto* b2 = makeOrder(2, 2, "AAPL", 150.0, 50, Side::BUY);
    book.insert(b1);
    book.insert(b2);
    book.insert(makeOrder(3, 3, "AAPL", 150.0, 50, Side::SELL));
    auto trades = book.match();
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].buyer_id, 1u); // b1 matched first
}

TEST(PriceLadder, CancelOrder) {
    PriceLadder book;
    book.insert(makeOrder(1, 1, "AAPL", 150.0, 100, Side::BUY));
    EXPECT_TRUE(book.cancel(1));
    EXPECT_TRUE(book.empty());
    EXPECT_FALSE(book.cancel(99)); // non-existent
}

TEST(PriceLadder, FOK_RejectWhenInsufficientLiquidity) {
    PriceLadder book;
    book.insert(makeOrder(1, 1, "AAPL", 150.0, 10, Side::SELL));
    // FOK BUY for 100 units — only 10 available → should be rejected
    EXPECT_FALSE(book.canFill(Side::BUY, 150.0, 100));
}

TEST(PriceLadder, FOK_AcceptWhenSufficientLiquidity) {
    PriceLadder book;
    book.insert(makeOrder(1, 1, "AAPL", 150.0, 200, Side::SELL));
    EXPECT_TRUE(book.canFill(Side::BUY, 150.0, 100));
}

TEST(PriceLadder, Snapshot) {
    PriceLadder book;
    book.insert(makeOrder(1, 1, "AAPL", 149.0, 80,  Side::BUY));
    book.insert(makeOrder(2, 2, "AAPL", 151.0, 120, Side::SELL));
    auto s = book.snapshot();
    EXPECT_DOUBLE_EQ(s.best_bid,  149.0);
    EXPECT_DOUBLE_EQ(s.best_ask,  151.0);
    EXPECT_DOUBLE_EQ(s.spread(),    2.0);
    EXPECT_EQ(s.bid_depth,  80);
    EXPECT_EQ(s.ask_depth, 120);
}

TEST(PriceLadder, MultipleTradesInOneMatch) {
    PriceLadder book;
    book.insert(makeOrder(1, 1, "AAPL", 150.0, 50, Side::SELL));
    book.insert(makeOrder(2, 2, "AAPL", 149.0, 50, Side::SELL));
    // One large bid that crosses both levels
    book.insert(makeOrder(3, 3, "AAPL", 152.0, 100, Side::BUY));
    auto trades = book.match();
    EXPECT_EQ(trades.size(), 2u);
    EXPECT_TRUE(book.empty());
}
