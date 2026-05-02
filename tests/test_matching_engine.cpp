#include <gtest/gtest.h>
#include "engine/matching_engine.hpp"

// Helpers to submit + drain in one call
static std::vector<Trade> submitAndProcess(MatchingEngine& e,
                                            uint64_t tid, const char* sym,
                                            double price, int32_t qty, Side side,
                                            OrderType type = OrderType::LIMIT,
                                            int32_t peak = 0) {
    e.submit(tid, sym, price, qty, side, type, peak);
    return e.processAll();
}

TEST(MatchingEngine, PostOnly_AcceptedWhenNoImmedateMatch) {
    MatchingEngine e;
    // No orders in book → post-only should rest
    auto trades = submitAndProcess(e, 1, "AAPL", 150.0, 100, Side::BUY, OrderType::POST_ONLY);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(e.stats().orders_rejected, 0u);
    // Book should now have the resting bid
    auto snap = e.bookSnapshot("AAPL");
    EXPECT_DOUBLE_EQ(snap.best_bid, 150.0);
}

TEST(MatchingEngine, PostOnly_RejectedWhenWouldCross) {
    MatchingEngine e;
    // Put a sell at 150 in the book first
    submitAndProcess(e, 2, "AAPL", 150.0, 100, Side::SELL, OrderType::LIMIT);
    // POST_ONLY buy at 150 → would cross → must be rejected
    auto trades = submitAndProcess(e, 1, "AAPL", 150.0, 100, Side::BUY, OrderType::POST_ONLY);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(e.stats().orders_rejected, 1u);
}

TEST(MatchingEngine, Iceberg_OnlyPeakVisibleInBook) {
    MatchingEngine e;
    // Iceberg BUY: total 200, visible 50
    submitAndProcess(e, 1, "AAPL", 150.0, 200, Side::BUY, OrderType::ICEBERG, 50);
    auto snap = e.bookSnapshot("AAPL");
    // Only peak_size (50) should be visible at best bid
    EXPECT_EQ(snap.bid_depth, 50);
}

TEST(MatchingEngine, Iceberg_ReplenishesAfterVisibleFilled) {
    MatchingEngine e;
    // Iceberg SELL: total 100, peak 30 → tranches: 30, 30, 30, 10
    submitAndProcess(e, 2, "AAPL", 150.0, 100, Side::SELL, OrderType::ICEBERG, 30);
    // Aggressive buyer takes 30 units → iceberg should replenish
    auto trades = submitAndProcess(e, 1, "AAPL", 151.0, 30, Side::BUY, OrderType::MARKET);
    EXPECT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 30);
    // Next tranche should now be visible
    auto snap = e.bookSnapshot("AAPL");
    EXPECT_EQ(snap.ask_depth, 30); // next 30 replenished
}

TEST(MatchingEngine, Iceberg_TotalQuantityConserved) {
    MatchingEngine e;
    // Iceberg SELL total=60 peak=20 → 3 tranches of 20
    submitAndProcess(e, 2, "AAPL", 150.0, 60, Side::SELL, OrderType::ICEBERG, 20);
    // Buy all 60 via market
    auto trades = submitAndProcess(e, 1, "AAPL", 0.0, 60, Side::BUY, OrderType::MARKET);
    int total_filled = 0;
    for (auto& t : trades) total_filled += t.quantity;
    EXPECT_EQ(total_filled, 60);
}

TEST(MatchingEngine, FOK_RejectedWhenInsufficientLiquidity) {
    MatchingEngine e;
    submitAndProcess(e, 2, "AAPL", 150.0, 10, Side::SELL);
    auto trades = submitAndProcess(e, 1, "AAPL", 150.0, 50, Side::BUY, OrderType::FOK);
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(e.stats().orders_rejected, 1u);
}

TEST(MatchingEngine, FOK_FilledCompletely) {
    MatchingEngine e;
    submitAndProcess(e, 2, "AAPL", 150.0, 100, Side::SELL);
    auto trades = submitAndProcess(e, 1, "AAPL", 150.0, 100, Side::BUY, OrderType::FOK);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 100);
}

TEST(MatchingEngine, OTR_OrderToTradeRatio) {
    MatchingEngine e;
    // 4 orders submitted, 1 trade generated
    submitAndProcess(e, 1, "AAPL", 149.0, 10, Side::BUY);  // no match
    submitAndProcess(e, 1, "AAPL", 148.0, 10, Side::BUY);  // no match
    submitAndProcess(e, 1, "AAPL", 147.0, 10, Side::BUY);  // no match
    // SELL at 148 → crosses best bid (149 >= 148) → 1 trade
    submitAndProcess(e, 2, "AAPL", 148.0, 10, Side::SELL);
    EXPECT_EQ(e.stats().orders_submitted, 4u);
    EXPECT_EQ(e.stats().trades_total,     1u);
    EXPECT_DOUBLE_EQ(e.stats().orderToTradeRatio(), 4.0);
}

TEST(MatchingEngine, FillRate) {
    MatchingEngine e;
    submitAndProcess(e, 1, "AAPL", 149.0, 10, Side::BUY);  // no fill
    submitAndProcess(e, 2, "AAPL", 149.0, 10, Side::SELL); // fills
    // 2 submitted, 1 resulted in a fill event (the SELL triggered the match)
    EXPECT_EQ(e.stats().orders_submitted, 2u);
    EXPECT_GE(e.stats().fillRate(), 0.0);
    EXPECT_LE(e.stats().fillRate(), 1.0);
}

TEST(MatchingEngine, SlippageIsNonNegative) {
    MatchingEngine e;
    submitAndProcess(e, 1, "AAPL", 150.0, 100, Side::BUY);
    submitAndProcess(e, 2, "AAPL", 150.0, 100, Side::SELL);
    // Slippage = |exec - mid| * qty; may be 0 if spread = 0 (no mid available)
    EXPECT_GE(e.stats().avgSlippage(), 0.0);
}

TEST(MatchingEngine, ImbalanceRange) {
    MatchingEngine e;
    submitAndProcess(e, 1, "AAPL", 150.0, 100, Side::BUY);
    submitAndProcess(e, 2, "AAPL", 150.0, 100, Side::SELL);
    EXPECT_GE(e.stats().avgImbalance(), 0.0);
    EXPECT_LE(e.stats().avgImbalance(), 1.0);
}
