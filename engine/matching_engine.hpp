#pragma once
#include "order_types.hpp"
#include "memory_pool.hpp"
#include "spsc_queue.hpp"
#include "latency.hpp"
#include "price_ladder.hpp"
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// System-level metrics (engine-wide, not per-trader)
struct EngineStats {
    uint64_t orders_submitted  = 0;
    uint64_t orders_filled     = 0; // had ≥ 1 fill
    uint64_t orders_rejected   = 0;
    uint64_t trades_total      = 0;
    double   total_slippage    = 0.0; // sum |exec - mid_at_fill| * qty
    double   total_imbalance   = 0.0; // sum of |imbalance| at each trade
    double   total_queue_wait_ns = 0.0; // sum of submission→dispatch wait

    // Derived metrics
    double orderToTradeRatio() const noexcept {
        return trades_total > 0
            ? static_cast<double>(orders_submitted) / trades_total : 0.0;
    }
    double fillRate() const noexcept {
        return orders_submitted > 0
            ? static_cast<double>(orders_filled) / orders_submitted : 0.0;
    }
    double avgSlippage() const noexcept {
        return trades_total > 0 ? total_slippage / trades_total : 0.0;
    }
    double avgImbalance() const noexcept {
        return trades_total > 0 ? total_imbalance / trades_total : 0.0;
    }
    double avgQueueWaitNs() const noexcept {
        return orders_submitted > 0
            ? total_queue_wait_ns / orders_submitted : 0.0;
    }

    void print() const;
};

class MatchingEngine {
public:
    static constexpr std::size_t POOL_SIZE  = 1 << 16; // 65 536 pre-allocated orders
    static constexpr std::size_t QUEUE_SIZE = 1 << 12; // 4 096 SPSC slots

    MatchingEngine() = default;

    // Producer thread — allocate from pool, push to SPSC queue.
    // For ICEBERG: set peak_size > 0 and quantity = total (hidden + first tranche).
    Order* submit(uint64_t    trader_id,
                  const char* symbol,
                  double      price,
                  int32_t     quantity,
                  Side        side,
                  OrderType   type      = OrderType::LIMIT,
                  int32_t     peak_size = 0) noexcept;

    // Consumer/matcher thread — drain queue, match, record all metrics.
    std::vector<Trade> processAll();

    // Read current book state (e.g. to capture mid-price before submitting)
    PriceLadder::Snapshot bookSnapshot(const char* symbol);

    const LatencyTracker& latency()    const noexcept { return latency_; }
    const EngineStats&    stats()      const noexcept { return stats_; }
    uint64_t ordersProcessed()         const noexcept {
        return processed_.load(std::memory_order_relaxed);
    }

private:
    MemoryPool<Order, POOL_SIZE>   pool_;
    SPSCQueue<Order*, QUEUE_SIZE>  queue_;
    LatencyTracker                 latency_;
    EngineStats                    stats_;
    std::atomic<uint64_t>          next_id_{1};
    std::atomic<uint64_t>          processed_{0};
    double                         cpu_ghz_ = -1.0; // calibrated lazily

    std::unordered_map<std::string, std::unique_ptr<PriceLadder>> books_;

    // ICEBERG state: hidden quantity remaining after current visible tranche
    struct IcebergState { int32_t peak_size; int32_t hidden_remaining; };
    std::unordered_map<uint64_t, IcebergState> icebergs_; // keyed by order id

    PriceLadder&       getOrCreateBook(const char* symbol);
    std::vector<Trade> dispatchOrder(Order* order);
    void               annotateAndRecord(std::vector<Trade>& trades,
                                         const PriceLadder::Snapshot& snap_before);
};
