#pragma once
#include "order_types.hpp"
#include <vector>
#include <unordered_map>
#include <functional>

class PriceLadder {
public:
    struct Snapshot {
        double  best_bid  = 0.0;
        double  best_ask  = 0.0;
        int32_t bid_depth = 0; // total qty at best bid level
        int32_t ask_depth = 0; // total qty at best ask level

        double spread()    const noexcept { return best_ask - best_bid; }
        double mid()       const noexcept { return (best_bid + best_ask) / 2.0; }

        // Book imbalance: +1 = all bids, -1 = all asks, 0 = balanced
        float imbalance() const noexcept {
            int32_t total = bid_depth + ask_depth;
            return total > 0
                ? static_cast<float>(bid_depth - ask_depth) / total
                : 0.0f;
        }
    };

    explicit PriceLadder(double min_price = 0.0,
                         double max_price = 500.0,
                         double tick_size = 0.01);

    bool insert(Order* order);
    bool cancel(uint64_t order_id, std::function<void(Order*)> on_removed = {});

    // Match crossing orders. on_filled(order) called when an order is fully depleted.
    std::vector<Trade> match(std::function<void(Order*)> on_filled = {});

    // Match a market order directly against resting orders without inserting it.
    // No price limit: sweeps the book until filled or no liquidity remains.
    std::vector<Trade> matchMarket(Order* order,
                                   std::function<void(Order*)> on_filled = {});

    bool    canFill(Side side, double price, int32_t qty) const;
    Snapshot snapshot() const;
    bool     empty()    const noexcept;

private:
    struct Level {
        Order*  head      = nullptr;
        Order*  tail      = nullptr;
        int32_t total_qty = 0;

        bool empty() const noexcept { return head == nullptr; }
        void push(Order* o) noexcept;
        void pop()          noexcept;
    };

    std::vector<Level> bids_;
    std::vector<Level> asks_;
    std::unordered_map<uint64_t, int32_t> order_level_;

    double  min_price_;
    double  tick_size_;
    int32_t num_levels_;
    int32_t best_bid_idx_ = -1;
    int32_t best_ask_idx_;

    int32_t priceToIdx(double price) const noexcept;
    double  idxToPrice(int32_t idx)  const noexcept;
    void    updateBestBid()                noexcept;
    void    updateBestAsk()                noexcept;
};
