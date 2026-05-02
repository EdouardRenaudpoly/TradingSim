#include <gtest/gtest.h>
#include "engine/trade_store.hpp"
#include "engine/metrics.hpp"
#include <cstring>
#include <cstdio>

// ── helpers ────────────────────────────────────────────────────────────────────

static Trade makeTrade(uint64_t buyer, uint64_t seller, const char* sym,
                        double price, int32_t qty,
                        double mid = 0.0, float imb = 0.0f) {
    Trade t{};
    t.buyer_id       = buyer;
    t.seller_id      = seller;
    t.price          = price;
    t.quantity       = qty;
    t.timestamp_ns   = 999;
    t.mid_at_fill    = mid;
    t.book_imbalance = imb;
    std::strncpy(t.symbol, sym, 7);
    t.symbol[7] = '\0';
    return t;
}

// ── tests ──────────────────────────────────────────────────────────────────────

#ifdef HAVE_SQLITE3
#include "third_party/sqlite/sqlite3.h"

static const char* DB = "/tmp/test_trade_store.db";

class TradeStoreTest : public ::testing::Test {
protected:
    void SetUp()    override { std::remove(DB); }
    void TearDown() override { std::remove(DB); }
};

// Opens a second read-only connection to DB and returns the int64 result of sql.
static int64_t queryInt(const char* sql) {
    sqlite3* db = nullptr;
    sqlite3_open_v2(DB, &db, SQLITE_OPEN_READONLY, nullptr);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    int64_t v = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        v = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return v;
}

TEST_F(TradeStoreTest, InsertTrades_RowCountIsCorrect) {
    {
        TradeStore store(DB);
        store.insertTrades({
            makeTrade(1, 2, "AAPL", 150.0, 100, 149.5, 0.5f),
            makeTrade(3, 4, "MSFT", 300.0,  50, 299.0, -0.2f),
        });
    }
    EXPECT_EQ(queryInt("SELECT COUNT(*) FROM trades"), 2);
}

TEST_F(TradeStoreTest, InsertTrades_FieldValuesAreCorrect) {
    {
        TradeStore store(DB);
        store.insertTrades({ makeTrade(7, 8, "AAPL", 150.25, 75, 149.5, 0.3f) });
    }

    sqlite3* db = nullptr;
    sqlite3_open_v2(DB, &db, SQLITE_OPEN_READONLY, nullptr);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT buyer_id, seller_id, symbol, price, quantity, mid_at_fill, book_imbalance "
        "FROM trades",
        -1, &stmt, nullptr);

    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_EQ   (sqlite3_column_int64 (stmt, 0), 7);
    EXPECT_EQ   (sqlite3_column_int64 (stmt, 1), 8);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)), "AAPL");
    EXPECT_NEAR (sqlite3_column_double(stmt, 3), 150.25, 1e-9);
    EXPECT_EQ   (sqlite3_column_int   (stmt, 4), 75);
    EXPECT_NEAR (sqlite3_column_double(stmt, 5), 149.5,  1e-9);
    EXPECT_NEAR (sqlite3_column_double(stmt, 6), 0.3,    1e-6);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

TEST_F(TradeStoreTest, InsertMetrics_RowCountMatchesDistinctTraders) {
    // Trades involve traders 1, 2, and 3.
    Metrics m;
    m.record(makeTrade(1, 2, "AAPL", 150.0, 100, 149.0));
    m.record(makeTrade(3, 1, "AAPL", 200.0,  50, 199.0));
    {
        TradeStore store(DB);
        store.insertMetrics(m);
    }
    EXPECT_EQ(queryInt("SELECT COUNT(*) FROM trader_metrics"), 3);
}

TEST_F(TradeStoreTest, InsertMetrics_PnLIsCorrect) {
    // Buyer pays, seller receives. Seller 2 should have pnl = +150*100 = +15000.
    Metrics m;
    m.record(makeTrade(1, 2, "AAPL", 150.0, 100));
    {
        TradeStore store(DB);
        store.insertMetrics(m);
    }

    sqlite3* db = nullptr;
    sqlite3_open_v2(DB, &db, SQLITE_OPEN_READONLY, nullptr);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT pnl FROM trader_metrics WHERE trader_id = 2",
        -1, &stmt, nullptr);

    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_NEAR(sqlite3_column_double(stmt, 0), 15000.0, 1e-9);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

TEST_F(TradeStoreTest, EmptyBatch_DoesNotThrow) {
    TradeStore store(DB);
    EXPECT_NO_THROW(store.insertTrades({}));
    EXPECT_EQ(queryInt("SELECT COUNT(*) FROM trades"), 0);
}

TEST_F(TradeStoreTest, MultipleInsertBatches_Accumulate) {
    TradeStore store(DB);
    store.insertTrades({ makeTrade(1, 2, "AAPL", 150.0, 10) });
    store.insertTrades({ makeTrade(3, 4, "MSFT", 300.0, 20) });
    EXPECT_EQ(queryInt("SELECT COUNT(*) FROM trades"), 2);
}

TEST(TradeStore, InvalidPath_Throws) {
    EXPECT_THROW(
        TradeStore("/no_such_directory_xyz/test.db"),
        std::runtime_error
    );
}

#else // !HAVE_SQLITE3 — TradeStore compiles to no-ops that throw on construct

TEST(TradeStore, WithoutSQLite_ConstructThrows) {
    EXPECT_THROW(TradeStore("any.db"), std::runtime_error);
}

TEST(TradeStore, WithoutSQLite_AvailableReturnsFalse) {
    EXPECT_FALSE(TradeStore::available());
}

#endif
