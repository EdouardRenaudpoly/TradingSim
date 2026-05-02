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


PriceLadder::PriceLadder(double min_price, double max_price, double tick_size)
    : min_price_(min_price)
    , tick_size_(tick_size)
    , num_levels_(static_cast<int32_t>((max_price - min_price) / tick_size) + 1)
    , best_ask_idx_(num_levels_) // sentinel: no asks yet
{
    bids_.resize(num_levels_);
    asks_.resize(num_levels_);
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
    order_level_[order->id] = idx;
    return true;
}

bool PriceLadder::cancel(uint64_t order_id,
                          std::function<void(Order*)> on_removed) {
    auto it = order_level_.find(order_id);
    if (it == order_level_.end()) return false;

    int32_t idx  = it->second;
    order_level_.erase(it);

    // Try bids first, then asks
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
                if (on_removed) on_removed(cur);
                return true;
            }
            prev = cur;
            cur  = cur->next;
        }
    }
    return false;
}

std::vector<Trade> PriceLadder::match(std::function<void(Order*)> on_filled) {
    std::vector<Trade> trades;

    while (best_bid_idx_ >= 0 &&
           best_ask_idx_ < num_levels_ &&
           best_bid_idx_ >= best_ask_idx_) {

        Level& bid_lvl = bids_[best_bid_idx_];
        Level& ask_lvl = asks_[best_ask_idx_];

        Order* bid = bid_lvl.head;
        Order* ask = ask_lvl.head;

        int32_t filled    = std::min(bid->remaining, ask->remaining);
        double  exec_price = idxToPrice(best_ask_idx_); // passive (resting) side sets price

        Trade t;
        t.buyer_id    = bid->trader_id;
        t.seller_id   = ask->trader_id;
        t.price       = exec_price;
        t.quantity    = filled;
        t.timestamp_ns = bid->timestamp_ns;
        // mid_at_fill and book_imbalance are set by MatchingEngine after match() returns
        std::memcpy(t.symbol, bid->symbol, 8);
        trades.push_back(t);

        bid->remaining -= filled;
        ask->remaining -= filled;
        bid_lvl.total_qty -= filled;
        ask_lvl.total_qty -= filled;

        if (bid->remaining == 0) {
            bid->status = OrderStatus::FILLED;
            order_level_.erase(bid->id);
            bid_lvl.pop();
            if (bid_lvl.empty()) updateBestBid();
            if (on_filled) on_filled(bid);
        } else {
            bid->status = OrderStatus::PARTIAL;
        }

        if (ask->remaining == 0) {
            ask->status = OrderStatus::FILLED;
            order_level_.erase(ask->id);
            ask_lvl.pop();
            if (ask_lvl.empty()) updateBestAsk();
            if (on_filled) on_filled(ask);
        } else {
            ask->status = OrderStatus::PARTIAL;
        }
    }

    return trades;
}

// matchMarket: sweep book without inserting the market order.
// Avoids the sentinel-price issue (1e18 would be out of ladder bounds).
std::vector<Trade> PriceLadder::matchMarket(Order* order,
                                             std::function<void(Order*)> on_filled) {
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

        order->remaining   -= filled;
        passive->remaining -= filled;
        passive_lvl->total_qty -= filled;

        if (passive->remaining == 0) {
            passive->status = OrderStatus::FILLED;
            order_level_.erase(passive->id);
            passive_lvl->pop();
            if (is_buy) { if (passive_lvl->empty()) updateBestAsk(); }
            else         { if (passive_lvl->empty()) updateBestBid(); }
            if (on_filled) on_filled(passive);
        } else {
            passive->status = OrderStatus::PARTIAL;
        }
    }

    if (order->remaining == 0) order->status = OrderStatus::FILLED;
    return trades;
}

bool PriceLadder::canFill(Side side, double price, int32_t qty) const {
    int32_t available = 0;
    if (side == Side::BUY) {
        // Need sell orders at ask_price <= bid_price
        int32_t limit_idx = priceToIdx(price);
        for (int32_t i = best_ask_idx_; i <= limit_idx && i < num_levels_; ++i) {
            available += asks_[i].total_qty;
            if (available >= qty) return true;
        }
    } else {
        // Need buy orders at bid_price >= ask_price
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
