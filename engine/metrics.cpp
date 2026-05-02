#include "metrics.hpp"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <cmath>

const TraderMetrics Metrics::empty_{};

void Metrics::record(const Trade& trade) {
    auto update = [&](uint64_t id, bool is_buyer) {
        auto& m = data_[id];
        m.trade_count++;
        m.volume += trade.quantity;

        if (is_buyer)
            m.pnl -= trade.price * trade.quantity;
        else
            m.pnl += trade.price * trade.quantity;

        if (trade.mid_at_fill > 0.0)
            m.slippage += std::abs(trade.price - trade.mid_at_fill) * trade.quantity;

        double prev_vol = static_cast<double>(m.volume - trade.quantity);
        m.vwap = (m.vwap * prev_vol + trade.price * trade.quantity) / m.volume;
    };

    update(trade.buyer_id,  true);
    update(trade.seller_id, false);
}

const TraderMetrics& Metrics::get(uint64_t trader_id) const {
    auto it = data_.find(trader_id);
    return it != data_.end() ? it->second : empty_;
}

void Metrics::print() const {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n=== Trader P&L ===\n";
    std::cout << std::left
              << std::setw(12) << "TraderID"
              << std::setw(10) << "Trades"
              << std::setw(10) << "Volume"
              << std::setw(14) << "PnL"
              << std::setw(14) << "Slippage"
              << std::setw(12) << "VWAP"
              << "\n";
    for (const auto& [id, m] : data_)
        std::cout << std::setw(12) << id
                  << std::setw(10) << m.trade_count
                  << std::setw(10) << m.volume
                  << std::setw(14) << m.pnl
                  << std::setw(14) << m.slippage
                  << std::setw(12) << m.vwap
                  << "\n";
}

void Metrics::exportCSV(const std::string& path) const {
    std::ofstream f(path);
    f << "trader_id,trades,volume,pnl,slippage,vwap\n";
    for (const auto& [id, m] : data_)
        f << id << "," << m.trade_count << "," << m.volume << ","
          << m.pnl << "," << m.slippage << "," << m.vwap << "\n";
}
