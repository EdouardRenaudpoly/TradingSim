# TradingSim — C++ Matching Engine

A price-time priority matching engine written in C++17. Supports six order types, lock-free data structures, and nanosecond-resolution latency tracking via the CPU timestamp counter.

---

## Build

Requires: `g++ >= 9`, `cmake >= 3.14`, internet access (GoogleTest is downloaded automatically).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Run

**Replay a CSV file:**
```bash
./build/engine --replay data/sample_data.csv
./build/engine --replay data/sample_data.csv --output metrics.csv
```

CSV format: `timestamp,symbol,price,quantity,side,trader_id[,type[,peak_size]]`

Valid values — `side`: `BUY`/`SELL`. `type`: `LIMIT`, `MARKET`, `IOC`, `FOK`, `POST_ONLY`, `ICEBERG`.

**Synthetic benchmark:**
```bash
./build/engine --benchmark --orders 100000
```

**Tests:**
```bash
cd build && ctest --output-on-failure
```

---

## Order types

| Type | Behaviour |
|---|---|
| LIMIT | Rests in book at specified price. Matches when the other side crosses. |
| MARKET | Sweeps the book immediately at any price. Residual is cancelled. |
| IOC | Immediate-or-Cancel. Fills what is available, cancels the rest. |
| FOK | Fill-or-Kill. Executes completely or is rejected outright. |
| POST_ONLY | Rejected if it would match immediately. Guarantees maker (resting) status. |
| ICEBERG | Only `peak_size` units are visible in the book. Replenishes from hidden quantity after each fill. |

---

## Benchmark

Measured on a single core, Release build (`-O3 -march=native`), 100 000 orders across 3 symbols and all 6 order types.

```
Throughput : ~280 000 orders/sec
Latency p50 :    ~293 ns
Latency p99 :  ~66 000 ns
Queue wait  :    ~120 ns avg
```

p50 is the median dispatch time per order (price-level lookup + match loop). p99 spikes come from iceberg replenishment cycles, which re-insert and re-match in a loop.

---

## Design decisions

Each section below explains a specific choice, why the obvious alternative does not work here, and where to find the relevant code.

---

### `std::atomic` instead of `std::mutex` in the memory pool and queue

The short version: a mutex puts a thread to sleep and wakes it up. In the kernel, that costs tens of thousands of nanoseconds. An atomic operation is a single CPU instruction.

When you lock a `std::mutex`, the OS scheduler may preempt your thread and let another run. When it comes back, the CPU cache is cold and the thread has lost its scheduling slot. For a matching engine where the target latency is a few hundred nanoseconds, one mutex acquisition can cost more than the entire matching cycle.

`std::atomic<T*>` maps to the `lock cmpxchg` instruction on x86. This is a hardware primitive: the CPU itself ensures atomicity without kernel involvement. The thread never sleeps, never yields, and the operation completes in roughly 5–20 cycles.

The tradeoff is that atomic code is harder to reason about. You have to think about memory ordering explicitly (see below). A mutex hides this behind a simpler API. For anything that is not on the hot path — file I/O, initialization, error handling — a mutex is the right choice.

Relevant code: `engine/memory_pool.hpp`, `engine/spsc_queue.hpp`.

---

### Memory ordering: `acquire` and `release`

Modern CPUs and compilers reorder instructions for performance. Two threads can observe memory writes in different orders unless you tell the hardware to synchronise.

`memory_order_release` on a write says: "every write that happened before this one must be visible to any thread that reads this location with `memory_order_acquire`." This forms a one-way barrier — cheap, because it only prevents reordering in one direction.

In the SPSC queue, the producer writes the item into the buffer and then does a `release` store on `head_`. The consumer does an `acquire` load on `head_`. This guarantees the consumer sees the fully-written item, not a half-written one. Without these annotations, the compiler or CPU is free to reorder the buffer write after the index update, and the consumer can read garbage.

`memory_order_seq_cst` (the default) is fully sequentially consistent — safe, but generates a memory fence (`mfence` on x86) which flushes the store buffer and is expensive. We only use it where correctness requires total order across all threads.

Relevant code: `engine/spsc_queue.hpp` lines 19–26, `engine/memory_pool.hpp` lines 23–38.

---

### `alignas(64)` and false sharing

A CPU cache line is 64 bytes on every modern x86 processor. The cache operates at cache-line granularity: if two threads read or write different variables that happen to sit in the same cache line, every write by one thread invalidates the cache line in the other thread's L1 cache. This is called false sharing, and it causes cache misses with no logical data dependency.

`struct alignas(64) Order` forces the struct to start at a 64-byte boundary and, because `sizeof(Order) == 64` (verified by `static_assert`), each order occupies exactly one cache line. No two orders share a line. Iterating through a queue of orders is a sequential scan — each order is fetched from memory exactly once.

In `SPSCQueue`, `head_` and `tail_` are each `alignas(64)`. Without this, they would sit next to each other in the same cache line. Every time the producer writes `head_`, the CPU would invalidate the line in the consumer's cache, even though the consumer only touches `tail_`. That is a false sharing cache miss on every push and pop.

Relevant code: `engine/order_types.hpp` line 19, `engine/spsc_queue.hpp` lines 45–46.

---

### Price ladder instead of `std::map`

The standard way to represent an order book is `std::map<double, queue<Order*>>`. Inserting or looking up a price level is `O(log n)`, where `n` is the number of distinct price levels. For 10 000 active price levels, that is about 13 comparisons, each potentially a cache miss because `std::map` is a red-black tree with pointer-chased nodes scattered in memory.

This engine uses two flat arrays `bids_[]` and `asks_[]` indexed by price: `idx = (price - min_price) / tick_size`. Inserting at a price level is a single array index write — `O(1)` and branch-free. The arrays sit in contiguous memory, so walking adjacent price levels (as the matching loop does) hits the L1 cache.

The cost is fixed memory allocation: the arrays are sized for the full price range at construction. For `[0.0, 500.0]` with tick `0.01`, that is 50 001 levels × 2 sides × `sizeof(Level)` = ~2.4 MB. That is acceptable for a single-symbol book.

Relevant code: `engine/price_ladder.hpp` lines 57–58, `engine/price_ladder.cpp` lines 23–35.

---

### RDTSC instead of `clock_gettime`

`clock_gettime(CLOCK_MONOTONIC)` is a syscall. Even with vDSO (the kernel's trick to avoid a full context switch for common syscalls), it costs roughly 20–50 ns and involves a memory read from a kernel-mapped page.

`__rdtsc()` reads the CPU's timestamp counter register directly — no syscall, no memory access, roughly 1 CPU cycle. It returns a cycle count, not nanoseconds, so we calibrate once at startup by comparing `rdtsc()` against `steady_clock` over 10 ms.

The limitation is that RDTSC counts cycles, not wall time. If the CPU changes frequency (power saving, turbo boost), the conversion drifts. On server hardware where the CPU runs at a fixed frequency, this is not an issue. On a laptop, the numbers are approximate.

Relevant code: `engine/latency.hpp` lines 14–40.

---

### Intrusive linked list for price-level queues

Each price level holds a FIFO queue of orders. The standard approach is `std::queue<Order*>`, which allocates a node per element from the heap. Each heap allocation under contention involves a lock inside `malloc`.

This engine uses an intrusive list: the `next` pointer is a field inside `Order` itself. No allocation happens when an order joins a queue — we just wire the pointer. The `Order` is already alive in the memory pool; we reuse the field we paid for.

The field `Order* next` serves double duty: inside the memory pool, it links free slots in the Treiber stack free-list. Once an order is allocated and inserted into a price level, it links it in the price-level FIFO. The two uses never overlap.

Relevant code: `engine/order_types.hpp` line 32, `engine/price_ladder.cpp` lines 7–20.

---

## Project structure

```
engine/
  order_types.hpp      — Order, Trade, enums
  memory_pool.hpp      — lock-free slab allocator (Treiber stack)
  spsc_queue.hpp       — single-producer single-consumer ring buffer
  latency.hpp          — RDTSC + LatencyTracker
  price_ladder.hpp/cpp — array-indexed order book
  matching_engine.hpp/cpp — orchestration, iceberg state, stats
  metrics.hpp/cpp      — per-trader PnL, VWAP, slippage
  simulation.hpp/cpp   — CSV replay
tests/
  test_matching_engine.cpp
  test_price_ladder.cpp
  test_spsc_queue.cpp
  test_memory_pool.cpp
  test_metrics.cpp
```
