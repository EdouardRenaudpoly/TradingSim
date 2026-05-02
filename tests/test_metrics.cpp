#include <gtest/gtest.h>
#include "engine/metrics.hpp"

static Trade makeTrade(uint64_t buyer, uint64_t seller, double price,
                        int32_t qty, double mid = 0.0) {
    Trade t{};
    t.buyer_id    = buyer;
    t.seller_id   = seller;
    t.price       = price;
    t.quantity    = qty;
    t.mid_at_fill = mid;
    return t;
}

TEST(Metrics, BuyerPnLIsNegative) {
    Metrics m;
    m.record(makeTrade(1, 2, 150.0, 10));
    EXPECT_DOUBLE_EQ(m.get(1).pnl, -1500.0);
}

TEST(Metrics, SellerPnLIsPositive) {
    Metrics m;
    m.record(makeTrade(1, 2, 150.0, 10));
    EXPECT_DOUBLE_EQ(m.get(2).pnl, 1500.0);
}

TEST(Metrics, PnLSumsToZero) {
    Metrics m;
    m.record(makeTrade(1, 2, 100.0, 5));
    m.record(makeTrade(3, 1, 200.0, 2));
    double total = m.get(1).pnl + m.get(2).pnl + m.get(3).pnl;
    EXPECT_NEAR(total, 0.0, 1e-9);
}

TEST(Metrics, TradeCountAndVolume) {
    Metrics m;
    m.record(makeTrade(1, 2, 150.0, 10));
    m.record(makeTrade(1, 2, 150.0,  5));
    EXPECT_EQ(m.get(1).trade_count, 2);
    EXPECT_EQ(m.get(1).volume,     15);
}

TEST(Metrics, SlippageWithMidPrice) {
    Metrics m;
    // Exec at 151, mid at 150 → slippage = |151-150| * 10 = 10
    m.record(makeTrade(1, 2, 151.0, 10, 150.0));
    EXPECT_NEAR(m.get(1).slippage, 10.0, 1e-9);
    EXPECT_NEAR(m.get(2).slippage, 10.0, 1e-9); // same for seller
}

TEST(Metrics, SlippageZeroWhenAtMid) {
    Metrics m;
    m.record(makeTrade(1, 2, 150.0, 10, 150.0)); // exec == mid
    EXPECT_NEAR(m.get(1).slippage, 0.0, 1e-9);
}

TEST(Metrics, VWAP_SingleTrade) {
    Metrics m;
    m.record(makeTrade(1, 2, 150.0, 100));
    EXPECT_DOUBLE_EQ(m.get(1).vwap, 150.0);
}

TEST(Metrics, VWAP_WeightedAverage) {
    Metrics m;
    m.record(makeTrade(1, 2, 100.0, 100)); // 100 * 100 = 10000
    m.record(makeTrade(1, 2, 200.0, 100)); // 100 * 200 = 20000
    // VWAP = (10000 + 20000) / 200 = 150
    EXPECT_DOUBLE_EQ(m.get(1).vwap, 150.0);
}

TEST(Metrics, UnknownTraderReturnsZero) {
    Metrics m;
    EXPECT_EQ(m.get(999).trade_count, 0);
    EXPECT_DOUBLE_EQ(m.get(999).pnl, 0.0);
}
