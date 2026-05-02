# Project Overview

## What this project is

A price-time priority matching engine written in C++20. It simulates the core component of a stock exchange: receiving orders from traders, maintaining an order book per symbol, and matching buy orders against sell orders to produce trades.

The engine is designed around the latency and throughput constraints of a high-frequency trading system. Every data structure and algorithm choice is motivated by performance at the nanosecond scale.

---

## Architecture

```
CSV file / benchmark generator
         │
         ▼
    Simulation (or main)
         │  submit(order)
         ▼
   MatchingEngine
         │
    ┌────┴────┐
    │         │
 SPSCQueue  MemoryPool
    │         │
    └────┬────┘
         │  dispatchOrder(order)
         ▼
    PriceLadder (one per symbol)
         │
    ┌────┴────┐
    │         │
  bids[]   asks[]      ← flat arrays indexed by price
    │
    ▼
  Trades → Metrics / TradeStore (SQLite)
```

**MatchingEngine** is the orchestrator. It owns the memory pool, the SPSC queue, and a map of price ladders keyed by symbol. The producer calls `submit()` to push an order onto the queue. The consumer calls `processAll()` (or runs as a dedicated thread via `startMatcherThread()`) to drain the queue and dispatch each order to the correct book.

**PriceLadder** is the order book for one symbol. It holds two flat arrays `bids_[]` and `asks_[]` where each slot is a FIFO queue of orders at that price level. It implements the matching logic for all six order types.

**MemoryPool** is a lock-free slab allocator. All `Order` objects live in a pre-allocated array. Allocation and deallocation are single CAS operations — no calls to `malloc`.

**SPSCQueue** is a lock-free ring buffer. The producer writes into it and the consumer reads from it without any mutex.

---

## Data flow for a single order

1. Caller invokes `engine.submit(trader_id, "AAPL", 150.0, 100, BUY, LIMIT)`.
2. `submit()` allocates an `Order` from the pool, fills its fields, records the RDTSC timestamp, and pushes it onto the SPSC queue.
3. `processAll()` pops the order, records queue wait time (now_tsc − submit_tsc), and calls `dispatchOrder()`.
4. `dispatchOrder()` looks up or creates the `PriceLadder` for "AAPL", takes a book snapshot (for slippage annotation), then routes to the correct matching path based on order type.
5. The matching loop walks the opposite side of the book. For each resting order that crosses, a `Trade` is produced. Filled orders are returned to the pool via the `reclaim` callback.
6. Trades are annotated with `mid_at_fill` and `book_imbalance` from the pre-match snapshot and forwarded to `Metrics` and optionally `TradeStore`.

---

## Order types

| Type | Matching behavior |
|---|---|
| LIMIT | Inserted into the book at its price. Matches immediately if the other side crosses; remainder rests. |
| MARKET | Sweeps the book at any price. Does not rest — any unfilled quantity is cancelled. |
| IOC | Immediate-or-Cancel. Inserted and matched once; unfilled remainder is cancelled immediately. |
| FOK | Fill-or-Kill. Before insertion, `canFill()` checks whether the full quantity is available. If not, the order is rejected without touching the book. |
| POST_ONLY | Rejected if it would match immediately (it would become a taker). Guarantees the order rests as a maker. |
| ICEBERG | Only `peak_size` units are visible in the book. After each fill of the visible tranche, a new tranche is inserted from the hidden quantity. Managed via the `icebergs_` map in `MatchingEngine`. |

---

## Metrics produced

**Per-trade:**
- `price`, `quantity`, `buyer_id`, `seller_id`, `symbol`
- `mid_at_fill` — midpoint of best bid/ask at the moment of the match
- `book_imbalance` — (bid_depth − ask_depth) / (bid_depth + ask_depth) at best level

**Per-trader (accumulated across all trades):**
- `pnl` — realized profit/loss (sell proceeds − buy cost, from the perspective of each side)
- `vwap` — volume-weighted average fill price
- `slippage` — sum of |fill_price − mid_at_fill| × quantity

**Engine-wide:**
- Throughput (orders/sec), fill rate, order-to-trade ratio
- Latency distribution: mean, p50, p95, p99, p99.9, min, max (in nanoseconds, via RDTSC)
- Average queue wait time (submission to dispatch)

---

## Persistence layer

With `--db <file>`, all trades and per-trader metrics are written to a SQLite database. SQLite is bundled as a vendored amalgamation (`third_party/sqlite/sqlite3.c`) — no installation required.

The engine compiles and runs fully without SQLite. The `HAVE_SQLITE3` preprocessor flag gates all database code. If `third_party/sqlite/sqlite3.c` is absent at cmake time, the flag is not set and `TradeStore` compiles to no-ops.

Schema:
```sql
CREATE TABLE trades (
    id INTEGER PRIMARY KEY,
    symbol TEXT, buyer_id INTEGER, seller_id INTEGER,
    price REAL, quantity INTEGER, timestamp_ns INTEGER,
    mid_at_fill REAL, book_imbalance REAL
);
CREATE TABLE trader_metrics (
    trader_id INTEGER PRIMARY KEY,
    trades INTEGER, volume INTEGER,
    pnl REAL, vwap REAL, slippage REAL
);
```

---

## Real market data

`data/nasdaq_sample.csv` contains 50 000 Add Order messages from the NASDAQ ITCH feed on January 3, 2003, covering MSFT, CSCO, INTC, KLAC, DELL, GE, IBM, and SUNW.

`tools/itch_to_csv.py` converts a raw NASDAQ ITCH v2 text file into the engine's CSV format. ITCH is the wire protocol used by NASDAQ to publish its full order book feed to subscribers.

---

## Threading model

**Single-thread mode** (default, used in tests and `--replay`): the caller submits and calls `processAll()` in the same thread. The SPSC queue is used as a staging buffer but producer and consumer are the same thread.

**Two-thread mode** (`startMatcherThread()`): a dedicated consumer thread runs `matcherLoop()` which calls `processAll()` continuously. The calling thread is the producer and only calls `submit()`. Used in `--benchmark` and `--replay-benchmark`. This is the configuration that exercises the lock-free design under real concurrency.

---

## Project structure

```
engine/
  order_types.hpp        Order, Trade, Side, OrderType, OrderStatus
  memory_pool.hpp        Lock-free slab allocator (Treiber stack)
  spsc_queue.hpp         Single-producer single-consumer ring buffer
  latency.hpp            RDTSC calibration + LatencyTracker
  price_ladder.hpp/cpp   Array-indexed order book, matching logic
  matching_engine.hpp/cpp  Orchestration, iceberg state, stats
  metrics.hpp/cpp        Per-trader PnL, VWAP, slippage
  simulation.hpp/cpp     CSV replay
  trade_store.hpp/cpp    SQLite persistence (optional)
tests/
  test_matching_engine.cpp
  test_price_ladder.cpp
  test_spsc_queue.cpp
  test_memory_pool.cpp
  test_metrics.cpp
  test_trade_store.cpp
  test_performance.cpp
tools/
  itch_to_csv.py         NASDAQ ITCH v2 → CSV converter
data/
  nasdaq_sample.csv      50k real NASDAQ orders (Jan 3 2003)
  sample_data.csv        Minimal synthetic sample
docs/
  overview.md            This file
  internals.md           Deep dive on C++ and HFT design decisions
```
