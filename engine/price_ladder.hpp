#pragma once
#include "order_types.hpp"
#include <vector>
#include <algorithm>
#include <cstring>
#include <type_traits>

class PriceLadder {
public:
    struct Snapshot {
        double  best_bid  = 0.0;
        double  best_ask  = 0.0;
        int32_t bid_depth = 0;
        int32_t ask_depth = 0;

        double spread()    const noexcept { return best_ask - best_bid; }
        double mid()       const noexcept { return (best_bid + best_ask) / 2.0; }

        float imbalance() const noexcept {
            int32_t total = bid_depth + ask_depth;
            return total > 0
                ? static_cast<float>(bid_depth - ask_depth) / total
                : 0.0f;
        }
    };

    // max_orders must equal the memory pool capacity: order_id % max_orders is
    // collision-free because the pool recycles a slot only after its order is done.
    explicit PriceLadder(double      min_price  = 0.0,
                         double      max_price  = 500.0,
                         double      tick_size  = 0.01,
                         std::size_t max_orders = 65536);

    bool insert(Order* order);

    // F = std::nullptr_t (default): callback path is eliminated at compile time.
    // Avoids std::function heap allocation and virtual dispatch on the hot path.
    template<typename F = std::nullptr_t>
    bool cancel(uint64_t order_id, F on_removed = nullptr);

    template<typename F = std::nullptr_t>
    std::vector<Trade> match(F on_filled = nullptr);

    template<typename F = std::nullptr_t>
    std::vector<Trade> matchMarket(Order* order, F on_filled = nullptr);

    bool     canFill(Side side, double price, int32_t qty) const;
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

    // Flat array replacing unordered_map: direct index instead of hash lookup per fill.
    // Sentinel -1 means slot is unused. Safe modular indexing: see constructor note.
    std::vector<int32_t> order_level_;
    std::size_t          order_cap_;

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

// ── Template implementations ───────────────────────────────────────────────────

template<typename F>
bool PriceLadder::cancel(uint64_t order_id, F on_removed) {
    int32_t& slot = order_level_[order_id % order_cap_];
    if (slot == -1) return false;

    int32_t idx = slot;
    slot = -1;

    for (int pass = 0; pass < 2; ++pass) {
        Level& level = (pass == 0) ? bids_[idx] : asks_[idx];
        Side   side  = (pass == 0) ? Side::BUY  : Side::SELL;

        Order* prev = nullptr;
        Order* cur  = level.head;
        while (cur) {
            if (cur->id == order_id) {
                if (prev) prev->next  = cur->next;
                else      level.head  = cur->next;
                if (level.tail == cur) level.tail = prev;
                level.total_qty -= cur->remaining;
                cur->status = OrderStatus::CANCELLED;
                cur->next   = nullptr;
                if (level.empty()) {
                    if (side == Side::BUY) updateBestBid();
                    else                   updateBestAsk();
                }
                if constexpr (!std::is_null_pointer_v<std::decay_t<F>>)
                    on_removed(cur);
                return true;
            }
            prev = cur;
            cur  = cur->next;
        }
    }
    return false;
}

template<typename F>
std::vector<Trade> PriceLadder::match(F on_filled) {
    std::vector<Trade> trades;

    while (best_bid_idx_ >= 0 &&
           best_ask_idx_ < num_levels_ &&
           best_bid_idx_ >= best_ask_idx_) {

        Level& bid_lvl = bids_[best_bid_idx_];
        Level& ask_lvl = asks_[best_ask_idx_];

        Order* bid = bid_lvl.head;
        Order* ask = ask_lvl.head;

        int32_t filled     = std::min(bid->remaining, ask->remaining);
        double  exec_price = idxToPrice(best_ask_idx_);

        Trade t;
        t.buyer_id     = bid->trader_id;
        t.seller_id    = ask->trader_id;
        t.price        = exec_price;
        t.quantity     = filled;
        t.timestamp_ns = bid->timestamp_ns;
        std::memcpy(t.symbol, bid->symbol, 8);
        trades.push_back(t);

        bid->remaining    -= filled;
        ask->remaining    -= filled;
        bid_lvl.total_qty -= filled;
        ask_lvl.total_qty -= filled;

        if (bid->remaining == 0) {
            bid->status = OrderStatus::FILLED;
            order_level_[bid->id % order_cap_] = -1;
            bid_lvl.pop();
            if (bid_lvl.empty()) updateBestBid();
            if constexpr (!std::is_null_pointer_v<std::decay_t<F>>)
                on_filled(bid);
        } else {
            bid->status = OrderStatus::PARTIAL;
        }

        if (ask->remaining == 0) {
            ask->status = OrderStatus::FILLED;
            order_level_[ask->id % order_cap_] = -1;
            ask_lvl.pop();
            if (ask_lvl.empty()) updateBestAsk();
            if constexpr (!std::is_null_pointer_v<std::decay_t<F>>)
                on_filled(ask);
        } else {
            ask->status = OrderStatus::PARTIAL;
        }
    }

    return trades;
}

template<typename F>
std::vector<Trade> PriceLadder::matchMarket(Order* order, F on_filled) {
    std::vector<Trade> trades;

    while (order->remaining > 0) {
        Level* passive_lvl = nullptr;
        double exec_price  = 0.0;
        bool   is_buy      = (order->side == Side::BUY);

        if (is_buy) {
            if (best_ask_idx_ >= num_levels_) break;
            passive_lvl = &asks_[best_ask_idx_];
            exec_price  = idxToPrice(best_ask_idx_);
        } else {
            if (best_bid_idx_ < 0) break;
            passive_lvl = &bids_[best_bid_idx_];
            exec_price  = idxToPrice(best_bid_idx_);
        }

        Order*  passive = passive_lvl->head;
        int32_t filled  = std::min(order->remaining, passive->remaining);

        Trade t;
        t.buyer_id     = is_buy ? order->trader_id : passive->trader_id;
        t.seller_id    = is_buy ? passive->trader_id : order->trader_id;
        t.price        = exec_price;
        t.quantity     = filled;
        t.timestamp_ns = order->timestamp_ns;
        std::memcpy(t.symbol, order->symbol, 8);
        trades.push_back(t);

        order->remaining       -= filled;
        passive->remaining     -= filled;
        passive_lvl->total_qty -= filled;

        if (passive->remaining == 0) {
            passive->status = OrderStatus::FILLED;
            order_level_[passive->id % order_cap_] = -1;
            passive_lvl->pop();
            if (is_buy) { if (passive_lvl->empty()) updateBestAsk(); }
            else         { if (passive_lvl->empty()) updateBestBid(); }
            if constexpr (!std::is_null_pointer_v<std::decay_t<F>>)
                on_filled(passive);
        } else {
            passive->status = OrderStatus::PARTIAL;
        }
    }

    if (order->remaining == 0) order->status = OrderStatus::FILLED;
    return trades;
}
