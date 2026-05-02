#include "trade_store.hpp"
#include <stdexcept>

#ifdef HAVE_SQLITE3

TradeStore::TradeStore(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        throw std::runtime_error("TradeStore: cannot open db: " + err);
    }
    // WAL: concurrent reads don't block writes (important for post-trade querying).
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA synchronous=NORMAL;");
    createSchema();
    prepareStatements();
}

TradeStore::~TradeStore() {
    if (ins_trade_)   sqlite3_finalize(ins_trade_);
    if (ins_metrics_) sqlite3_finalize(ins_metrics_);
    if (db_)          sqlite3_close(db_);
}

void TradeStore::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error(std::string("TradeStore SQL error: ") + msg);
    }
}

void TradeStore::createSchema() {
    exec(R"(
        CREATE TABLE IF NOT EXISTS trades (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            symbol        TEXT    NOT NULL,
            buyer_id      INTEGER NOT NULL,
            seller_id     INTEGER NOT NULL,
            price         REAL    NOT NULL,
            quantity      INTEGER NOT NULL,
            timestamp_ns  INTEGER NOT NULL,
            mid_at_fill   REAL    NOT NULL,
            book_imbalance REAL   NOT NULL
        );
    )");
    exec(R"(
        CREATE TABLE IF NOT EXISTS trader_metrics (
            trader_id   INTEGER PRIMARY KEY,
            pnl         REAL    NOT NULL,
            volume      INTEGER NOT NULL,
            trade_count INTEGER NOT NULL,
            vwap        REAL    NOT NULL,
            slippage    REAL    NOT NULL
        );
    )");
}

void TradeStore::prepareStatements() {
    const char* ins_t = R"(
        INSERT INTO trades
            (symbol, buyer_id, seller_id, price, quantity, timestamp_ns, mid_at_fill, book_imbalance)
        VALUES (?,?,?,?,?,?,?,?);
    )";
    if (sqlite3_prepare_v2(db_, ins_t, -1, &ins_trade_, nullptr) != SQLITE_OK)
        throw std::runtime_error("TradeStore: prepare trades stmt failed");

    const char* ins_m = R"(
        INSERT OR REPLACE INTO trader_metrics
            (trader_id, pnl, volume, trade_count, vwap, slippage)
        VALUES (?,?,?,?,?,?);
    )";
    if (sqlite3_prepare_v2(db_, ins_m, -1, &ins_metrics_, nullptr) != SQLITE_OK)
        throw std::runtime_error("TradeStore: prepare metrics stmt failed");
}

void TradeStore::insertTrades(const std::vector<Trade>& trades) {
    if (trades.empty()) return;

    exec("BEGIN;");
    for (const auto& t : trades) {
        sqlite3_bind_text   (ins_trade_, 1, t.symbol,          -1, SQLITE_STATIC);
        sqlite3_bind_int64  (ins_trade_, 2, static_cast<sqlite3_int64>(t.buyer_id));
        sqlite3_bind_int64  (ins_trade_, 3, static_cast<sqlite3_int64>(t.seller_id));
        sqlite3_bind_double (ins_trade_, 4, t.price);
        sqlite3_bind_int    (ins_trade_, 5, t.quantity);
        sqlite3_bind_int64  (ins_trade_, 6, static_cast<sqlite3_int64>(t.timestamp_ns));
        sqlite3_bind_double (ins_trade_, 7, t.mid_at_fill);
        sqlite3_bind_double (ins_trade_, 8, static_cast<double>(t.book_imbalance));

        if (sqlite3_step(ins_trade_) != SQLITE_DONE)
            throw std::runtime_error("TradeStore: insert trade failed");
        sqlite3_reset(ins_trade_);
    }
    exec("COMMIT;");
}

void TradeStore::insertMetrics(const Metrics& metrics) {
    exec("BEGIN;");
    for (const auto& [tid, m] : metrics.all()) {
        sqlite3_bind_int64  (ins_metrics_, 1, static_cast<sqlite3_int64>(tid));
        sqlite3_bind_double (ins_metrics_, 2, m.pnl);
        sqlite3_bind_int64  (ins_metrics_, 3, static_cast<sqlite3_int64>(m.volume));
        sqlite3_bind_int    (ins_metrics_, 4, m.trade_count);
        sqlite3_bind_double (ins_metrics_, 5, m.vwap);
        sqlite3_bind_double (ins_metrics_, 6, m.slippage);

        if (sqlite3_step(ins_metrics_) != SQLITE_DONE)
            throw std::runtime_error("TradeStore: insert metrics failed");
        sqlite3_reset(ins_metrics_);
    }
    exec("COMMIT;");
}

#else // HAVE_SQLITE3 not defined — compile to no-ops

TradeStore::TradeStore(const std::string&) {
    throw std::runtime_error(
        "TradeStore: SQLite support not compiled in. "
        "Remove --db flag or rebuild with -DWITH_SQLITE=ON.");
}
TradeStore::~TradeStore() {}
void TradeStore::insertTrades(const std::vector<Trade>&) {}
void TradeStore::insertMetrics(const Metrics&) {}

#endif
