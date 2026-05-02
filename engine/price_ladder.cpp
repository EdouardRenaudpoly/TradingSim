#include "price_ladder.hpp"
#include <cmath>
#include <cstring>
#include <algorithm>


void PriceLadder::Level::push(Order* o) noexcept {
    o->next = nullptr;
    if (tail) tail->next = o;
    else      head = o;
    tail = o;
    total_qty += o->remaining;
}

void PriceLadder::Level::pop() noexcept {
    if (!head) return;
    total_qty -= head->remaining;
    head = head->next;
    if (!head) tail = nullptr;
}


PriceLadder::PriceLadder(double min_price, double max_price, double tick_size,
                         std::size_t max_orders)
    : min_price_(min_price)
    , tick_size_(tick_size)
    , num_levels_(static_cast<int32_t>((max_price - min_price) / tick_size) + 1)
    , best_ask_idx_(num_levels_)
    , order_cap_(max_orders)
{
    bids_.resize(num_levels_);
    asks_.resize(num_levels_);
    order_level_.assign(max_orders, -1);
}

int32_t PriceLadder::priceToIdx(double price) const noexcept {
    return static_cast<int32_t>((price - min_price_) / tick_size_ + 0.5);
}

double PriceLadder::idxToPrice(int32_t idx) const noexcept {
    return min_price_ + idx * tick_size_;
}

void PriceLadder::updateBestBid() noexcept {
    while (best_bid_idx_ >= 0 && bids_[best_bid_idx_].empty())
        --best_bid_idx_;
}

void PriceLadder::updateBestAsk() noexcept {
    while (best_ask_idx_ < num_levels_ && asks_[best_ask_idx_].empty())
        ++best_ask_idx_;
}

bool PriceLadder::insert(Order* order) {
    int32_t idx = priceToIdx(order->price);
    if (idx < 0 || idx >= num_levels_) {
        order->status = OrderStatus::REJECTED;
        return false;
    }

    if (order->side == Side::BUY) {
        bids_[idx].push(order);
        if (idx > best_bid_idx_) best_bid_idx_ = idx;
    } else {
        asks_[idx].push(order);
        if (idx < best_ask_idx_) best_ask_idx_ = idx;
    }
    order_level_[order->id % order_cap_] = idx;
    return true;
}

bool PriceLadder::canFill(Side side, double price, int32_t qty) const {
    int32_t available = 0;
    if (side == Side::BUY) {
        int32_t limit_idx = priceToIdx(price);
        for (int32_t i = best_ask_idx_; i <= limit_idx && i < num_levels_; ++i) {
            available += asks_[i].total_qty;
            if (available >= qty) return true;
        }
    } else {
        int32_t limit_idx = priceToIdx(price);
        for (int32_t i = best_bid_idx_; i >= limit_idx && i >= 0; --i) {
            available += bids_[i].total_qty;
            if (available >= qty) return true;
        }
    }
    return false;
}

PriceLadder::Snapshot PriceLadder::snapshot() const {
    Snapshot s;
    if (best_bid_idx_ >= 0) {
        s.best_bid  = idxToPrice(best_bid_idx_);
        s.bid_depth = bids_[best_bid_idx_].total_qty;
    }
    if (best_ask_idx_ < num_levels_) {
        s.best_ask  = idxToPrice(best_ask_idx_);
        s.ask_depth = asks_[best_ask_idx_].total_qty;
    }
    return s;
}

bool PriceLadder::empty() const noexcept {
    return best_bid_idx_ < 0 && best_ask_idx_ >= num_levels_;
}
