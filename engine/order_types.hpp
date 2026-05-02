#pragma once
#include <cstdint>
#include <cstring>

enum class Side        : uint8_t { BUY, SELL };
enum class OrderType   : uint8_t {
    LIMIT,      // standard resting order
    MARKET,     // match at any price, cancel residual
    IOC,        // Immediate-or-Cancel: fill what's available, cancel rest
    FOK,        // Fill-or-Kill: fill completely or reject entirely
    POST_ONLY,  // rejected if it would immediately match (guarantees maker rebate)
    ICEBERG     // shows only peak_size in book; replenishes from hidden qty automatically
};
enum class OrderStatus : uint8_t { NEW, PARTIAL, FILLED, CANCELLED, REJECTED };

// Exactly one 64-byte cache line.
// Layout verified by static_assert below.
// peak_size occupies the 4-byte natural padding between symbol[8] and next*.
struct alignas(64) Order {
    uint64_t    id           = 0;
    uint64_t    trader_id    = 0;
    double      price        = 0.0;
    int32_t     quantity     = 0;   // total quantity (incl. hidden for ICEBERG)
    int32_t     remaining    = 0;   // current visible remaining in book
    uint64_t    timestamp_ns = 0;   // RDTSC at submission (for latency / time-in-queue)
    Side        side         = Side::BUY;
    OrderType   type         = OrderType::LIMIT;
    OrderStatus status       = OrderStatus::NEW;
    char        symbol[8]    = {};
    int32_t     peak_size    = 0;   // ICEBERG: visible tranche size; 0 = not iceberg
    // 1 byte implicit padding here (compiler aligns next* to 8 bytes)
    Order*      next         = nullptr; // intrusive list: pool free-list OR price-level queue

    void setSymbol(const char* s) noexcept {
        std::strncpy(symbol, s, 7);
        symbol[7] = '\0';
    }
};
static_assert(sizeof(Order) == 64, "Order must be exactly one cache line");

struct Trade {
    uint64_t buyer_id       = 0;
    uint64_t seller_id      = 0;
    double   price          = 0.0;
    int32_t  quantity       = 0;
    uint64_t timestamp_ns   = 0;
    char     symbol[8]      = {};
    double   mid_at_fill    = 0.0;  // mid-price at moment of fill (for slippage)
    float    book_imbalance = 0.0f; // (bid_depth-ask_depth)/(bid_depth+ask_depth) at fill
};
