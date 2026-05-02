#include "matching_engine.hpp"
#include <cstring>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <thread>

void MatchingEngine::startMatcherThread() {
    matcher_thread_ = std::jthread([this](std::stop_token stop) {
        matcherLoop(stop);
    });
}

void MatchingEngine::stopMatcherThread() {
    matcher_thread_.request_stop();
    if (matcher_thread_.joinable())
        matcher_thread_.join();
}

// Dedicated consumer thread: busy-polls when the queue has work, yields when idle.
// Exits when stop is requested AND the queue is drained.
void MatchingEngine::matcherLoop(std::stop_token stop) {
    while (!stop.stop_requested() || !queue_.empty()) {
        processAll();
        if (queue_.empty())
            std::this_thread::yield();
    }
}

void EngineStats::print() const {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n=== Engine Stats ===\n";
    std::cout << "  Orders submitted   : " << orders_submitted  << "\n";
    std::cout << "  Orders filled      : " << orders_filled     << "\n";
    std::cout << "  Orders rejected    : " << orders_rejected   << "\n";
    std::cout << "  Trades executed    : " << trades_total      << "\n";
    std::cout << "  Order-to-Trade     : " << std::setprecision(2) << orderToTradeRatio() << "\n";
    std::cout << "  Fill rate          : " << fillRate() * 100.0 << " %\n";
    std::cout << "  Avg slippage/trade : " << std::setprecision(4) << avgSlippage() << "\n";
    std::cout << "  Avg imbalance    : " << std::setprecision(3) << avgImbalance() << "\n";
    std::cout << "  Avg queue wait     : " << std::setprecision(1) << avgQueueWaitNs() << " ns\n";
}

PriceLadder& MatchingEngine::getOrCreateBook(const char* symbol) {
    auto& ptr = books_[symbolToKey(symbol)];
    if (!ptr) ptr = std::make_unique<PriceLadder>(0.0, 500.0, 0.01, POOL_SIZE);
    return *ptr;
}

PriceLadder::Snapshot MatchingEngine::bookSnapshot(const char* symbol) {
    auto it = books_.find(symbolToKey(symbol));
    return it != books_.end() ? it->second->snapshot() : PriceLadder::Snapshot{};
}

Order* MatchingEngine::submit(uint64_t trader_id, const char* symbol,
                               double price, int32_t quantity,
                               Side side, OrderType type,
                               int32_t peak_size) noexcept {
    Order* o = pool_.allocate();
    if (!o) return nullptr;

    o->id           = next_id_.fetch_add(1, std::memory_order_relaxed);
    o->trader_id    = trader_id;
    o->price        = price;
    o->quantity     = quantity;
    o->remaining    = (type == OrderType::ICEBERG && peak_size > 0)
                          ? std::min(peak_size, quantity)
                          : quantity;
    o->side         = side;
    o->type         = type;
    o->peak_size    = peak_size;
    o->status       = OrderStatus::NEW;
    o->timestamp_ns = rdtsc();
    o->next         = nullptr;
    o->setSymbol(symbol);

    if (!queue_.push(o)) {
        pool_.deallocate(o);
        return nullptr;
    }

    stats_.orders_submitted++;
    return o;
}

std::vector<Trade> MatchingEngine::processAll() {
    if (cpu_ghz_ <= 0.0) cpu_ghz_ = estimateCpuGhz();

    std::vector<Trade> all;

    while (auto opt = queue_.pop()) {
        Order* o = *opt;

        uint64_t tsc_now     = rdtsc();
        uint64_t wait_cycles = tsc_now - o->timestamp_ns;
        stats_.total_queue_wait_ns += static_cast<double>(wait_cycles) / cpu_ghz_;

        uint64_t t0     = tsc_now;
        auto     trades = dispatchOrder(o);
        uint64_t t1     = rdtsc();

        latency_.record(t1 - t0);
        processed_.fetch_add(1, std::memory_order_relaxed);

        all.insert(all.end(), trades.begin(), trades.end());
    }
    return all;
}

void MatchingEngine::annotateAndRecord(std::vector<Trade>& trades,
                                        const PriceLadder::Snapshot& snap) {
    double mid  = snap.mid();
    float  imb  = snap.imbalance();

    for (auto& t : trades) {
        t.mid_at_fill    = mid;
        t.book_imbalance = imb;
        stats_.total_slippage  += std::abs(t.price - mid) * t.quantity;
        stats_.total_imbalance += std::abs(imb);
        stats_.trades_total++;
    }
}

std::vector<Trade> MatchingEngine::dispatchOrder(Order* order) {
    PriceLadder& book = getOrCreateBook(order->symbol);

    auto snap_before = book.snapshot(); // captured before insertion for slippage annotation

    std::vector<Order*> renewals; // iceberg tranches to re-insert after each match cycle

    auto reclaim = [&](Order* filled) {
        auto it = icebergs_.find(filled->id);
        if (it != icebergs_.end()) {
            auto& state = it->second;
            if (state.hidden_remaining > 0) {
                int32_t tranche = std::min(state.peak_size, state.hidden_remaining);
                state.hidden_remaining -= tranche;

                Order* r = pool_.allocate();
                if (r) {
                    *r            = *filled;
                    r->id         = next_id_.fetch_add(1, std::memory_order_relaxed);
                    r->remaining  = tranche;
                    r->quantity   = tranche;
                    r->status     = OrderStatus::NEW;
                    r->next       = nullptr;
                    r->timestamp_ns = rdtsc();

                    if (state.hidden_remaining > 0)
                        icebergs_[r->id] = {state.peak_size, state.hidden_remaining};

                    renewals.push_back(r);
                }
                icebergs_.erase(it);
                pool_.deallocate(filled);
                return;
            }
            icebergs_.erase(it);
        }
        pool_.deallocate(filled);
    };

    std::vector<Trade> trades;

    switch (order->type) {

    case OrderType::LIMIT:
        book.insert(order);
        trades = book.match(reclaim);
        break;

    case OrderType::MARKET: {
        auto t = book.matchMarket(order, reclaim);
        trades.insert(trades.end(), t.begin(), t.end());

        while (order->remaining > 0 && !renewals.empty()) {
            std::vector<Order*> batch;
            std::swap(batch, renewals);
            for (Order* r : batch) book.insert(r);
            auto more = book.matchMarket(order, reclaim);
            trades.insert(trades.end(), more.begin(), more.end());
        }

        if (order->remaining > 0) order->status = OrderStatus::CANCELLED;
        pool_.deallocate(order);
        break;
    }

    case OrderType::IOC:
        book.insert(order);
        trades = book.match(reclaim);
        if (order->remaining > 0) book.cancel(order->id, reclaim);
        break;

    case OrderType::FOK:
        if (!book.canFill(order->side, order->price, order->remaining)) {
            order->status = OrderStatus::REJECTED;
            stats_.orders_rejected++;
            pool_.deallocate(order);
            return {};
        }
        book.insert(order);
        trades = book.match(reclaim);
        break;

    case OrderType::POST_ONLY:
        if (book.canFill(order->side, order->price, 1)) {
            order->status = OrderStatus::REJECTED;
            stats_.orders_rejected++;
            pool_.deallocate(order);
            return {};
        }
        book.insert(order);
        break;

    case OrderType::ICEBERG:
        if (order->peak_size > 0)
            icebergs_[order->id] = {
                order->peak_size,
                order->quantity - order->remaining // hidden = total - first tranche
            };
        book.insert(order);
        trades = book.match(reclaim);
        break;
    }

    // Swap-loop: match(reclaim) can push new renewals while we iterate,
    // so we snapshot the current batch before each pass to avoid UB.
    while (!renewals.empty()) {
        std::vector<Order*> batch;
        std::swap(batch, renewals);
        for (Order* r : batch) book.insert(r);
        auto more = book.match(reclaim);
        trades.insert(trades.end(), more.begin(), more.end());
    }

    annotateAndRecord(trades, snap_before);

    if (!trades.empty()) stats_.orders_filled++;

    return trades;
}
