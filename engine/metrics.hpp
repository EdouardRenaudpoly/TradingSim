#pragma once
#include "order_types.hpp"
#include <string>
#include <unordered_map>

struct TraderMetrics {
    double   pnl            = 0.0;
    double   slippage       = 0.0; // sum of |exec_price - mid_at_fill| * qty
    int32_t  trade_count    = 0;
    int32_t  volume         = 0;
    double   vwap           = 0.0; // volume-weighted average execution price
};

class Metrics {
public:
    void record(const Trade& trade);

    const TraderMetrics& get(uint64_t trader_id) const;
    const std::unordered_map<uint64_t, TraderMetrics>& all() const noexcept { return data_; }
    void print()                            const;
    void exportCSV(const std::string& path) const;

private:
    std::unordered_map<uint64_t, TraderMetrics> data_;
    static const TraderMetrics empty_;
};
