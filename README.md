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
./build/engine --replay data/sample_data.csv --db trades.db
```

CSV format: `timestamp,symbol,price,quantity,side,trader_id[,type[,peak_size]]`

Valid values — `side`: `BUY`/`SELL`. `type`: `LIMIT`, `MARKET`, `IOC`, `FOK`, `POST_ONLY`, `ICEBERG`.

With `--db`, all trades and per-trader metrics are written to a SQLite database. No installation required — SQLite is bundled as a vendored amalgamation (`third_party/sqlite/`).

```sql
-- Example queries after a replay
SELECT symbol, price, quantity, mid_at_fill, book_imbalance FROM trades;
SELECT trader_id, pnl, vwap, slippage FROM trader_metrics ORDER BY pnl DESC;
SELECT symbol, AVG(price) as avg_price, SUM(quantity) as total_vol FROM trades GROUP BY symbol;
```

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

Single core, Release build (`-O3 -march=native`), 100 000 orders across 3 symbols and all 6 order types.

```
Throughput :  358 971 orders/sec
Latency p50 :     174 ns
Latency p95 :   5 377 ns
Latency p99 :  54 573 ns
Queue wait  :     115 ns avg
```

p50 is the median dispatch time per order (price-level lookup + match loop). p99 spikes come from iceberg replenishment cycles which re-insert and re-match in a loop.

**Performance history** — three optimizations applied after initial implementation:

| Optimization | Throughput | p50 | p99 |
|---|---|---|---|
| Baseline | 311 658 /s | 244 ns | 61 931 ns |
| + template callbacks + flat order index + uint64 symbol key | **358 971 /s** | **174 ns** | **54 573 ns** |

The largest gain came from replacing `std::function` callbacks with template parameters in the match loop. `std::function` allocates on the heap for each lambda capture and dispatches through a virtual call — both eliminated with `if constexpr` template instantiation.

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

### Template callbacks instead of `std::function` in the match loop

`std::function` is a type-erased wrapper. When you construct one from a lambda that captures variables, it allocates memory on the heap and stores a virtual function pointer for the call. On every invocation, the call goes through that pointer — one indirect branch the CPU branch predictor cannot inline.

In the match loop, `on_filled` is called once per filled order. With `std::function`, that is one heap allocation when the callback is set up plus one indirect call per fill, on the path that runs thousands of times per second.

Replacing it with a template parameter lets the compiler see the concrete type at instantiation time. The callback is inlined entirely — zero heap allocation, zero indirect branch. The `if constexpr (!std::is_null_pointer_v<F>)` check eliminates the callback path at compile time when no callback is passed, so callers that don't need it pay nothing.

```cpp
// std::function version — heap alloc + virtual dispatch per call
std::vector<Trade> match(std::function<void(Order*)> on_filled = {});

// Template version — inlined, no allocation
template<typename F = std::nullptr_t>
std::vector<Trade> match(F on_filled = nullptr);
```

Relevant code: `engine/price_ladder.hpp` template section.

---

### Flat array instead of `unordered_map` for order-to-level lookup

When an order is filled or cancelled, the engine needs to know which price level it sits at. The naive approach is `unordered_map<order_id, level_index>`. Each lookup hashes a 64-bit integer, probes a bucket, and follows a pointer — roughly 20–50 ns under contention.

The replacement is a `std::vector<int32_t>` indexed by `order_id % pool_capacity`. This is a direct array access: one multiplication, one bounds check, one load. It works because order IDs are assigned sequentially from a pool of fixed size. A slot can only be reused after its previous occupant is fully done (filled or cancelled and deallocated back to the pool), so `id % capacity` is collision-free by construction.

Relevant code: `engine/price_ladder.hpp` (`order_level_`), `engine/price_ladder.cpp` constructor.

---

### Symbol stored as `uint64_t` instead of `std::string`

Trading symbols fit in 8 bytes (`char[8]`). Storing them as `std::string` means the map key is a heap-allocated object. Hashing it calls `strlen` then iterates over characters. Looking up `"AAPL"` in `unordered_map<string, PriceLadder>` is slower than it needs to be.

Reinterpreting the 8 bytes as a `uint64_t` gives an integer key. The hash for a 64-bit integer is a single multiply-shift operation (roughly 2 cycles). The map lookup is otherwise identical.

```cpp
static uint64_t symbolToKey(const char* s) noexcept {
    uint64_t key = 0;
    std::memcpy(&key, s, std::min(std::strlen(s), std::size_t(8)));
    return key;
}
```

Relevant code: `engine/matching_engine.hpp` (`symbolToKey`, `books_`).

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
