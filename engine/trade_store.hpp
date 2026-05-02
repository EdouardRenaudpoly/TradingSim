#pragma once
#include "order_types.hpp"
#include "metrics.hpp"
#include <string>
#include <vector>

// Writes trades and per-trader metrics to a SQLite database.
// When compiled without HAVE_SQLITE3 all methods are no-ops that throw on construct.

#ifdef HAVE_SQLITE3
#  include "../third_party/sqlite/sqlite3.h"
#endif

class TradeStore {
public:
    // Opens (or creates) the database at `path`. Throws std::runtime_error on failure.
    explicit TradeStore(const std::string& path);
    ~TradeStore();

    TradeStore(const TradeStore&)            = delete;
    TradeStore& operator=(const TradeStore&) = delete;

    void insertTrades(const std::vector<Trade>& trades);
    void insertMetrics(const Metrics& metrics);

    static constexpr bool available() noexcept {
#ifdef HAVE_SQLITE3
        return true;
#else
        return false;
#endif
    }

private:
#ifdef HAVE_SQLITE3
    sqlite3*      db_   = nullptr;
    sqlite3_stmt* ins_trade_   = nullptr;
    sqlite3_stmt* ins_metrics_ = nullptr;

    void exec(const char* sql);
    void createSchema();
    void prepareStatements();
#endif
};
