# Internals — C++ and HFT Design Decisions

---

## Table of contents

1. [Mutex vs atomic](#1-mutex-vs-atomic)
2. [Memory ordering — acquire, release, seq_cst](#2-memory-ordering--acquire-release-seq_cst)
3. [The SPSC queue](#3-the-spsc-queue)
4. [The memory pool (Treiber stack)](#4-the-memory-pool-treiber-stack)
5. [Cache lines, false sharing, and alignas(64)](#5-cache-lines-false-sharing-and-alignas64)
6. [Price ladder — flat array vs std::map](#6-price-ladder--flat-array-vs-stdmap)
7. [Intrusive linked list for price-level queues](#7-intrusive-linked-list-for-price-level-queues)
8. [RDTSC — measuring time without a syscall](#8-rdtsc--measuring-time-without-a-syscall)
9. [Template callbacks vs std::function](#9-template-callbacks-vs-stdfunction)
10. [Flat array for order-to-level lookup](#10-flat-array-for-order-to-level-lookup)
11. [Symbol as uint64_t](#11-symbol-as-uint64_t)
12. [Producer/consumer threading](#12-producerconsumer-threading)
13. [Iceberg orders and p99 latency spikes](#13-iceberg-orders-and-p99-latency-spikes)
14. [Reading benchmark numbers](#14-reading-benchmark-numbers)
15. [C++20 features used](#15-c20-features-used)

---

## 1. Mutex vs atomic

When a thread calls `pthread_mutex_lock()` on a contested lock, the OS marks the thread as blocked, removes it from the run queue, and context-switches to another thread. When the lock is released the blocked thread is rescheduled — but its CPU registers must be restored and its L1/L2 cache is cold because another thread used the CPU while it waited. Total cost: tens of thousands of nanoseconds.

`std::atomic<T*>` maps to the `lock cmpxchg` instruction on x86. The operation is a single hardware instruction: no syscall, no kernel involvement, no context switch. Cost: 5–20 CPU cycles.

The memory pool and the SPSC queue are the only places in the engine that use atomics. Everything else — file I/O, SQLite, initialisation — uses standard synchronisation because those paths are not latency-sensitive.

Relevant code: `engine/memory_pool.hpp`, `engine/spsc_queue.hpp`.

---

## 2. Memory ordering — acquire, release, seq_cst

Modern CPUs and compilers reorder instructions to improve throughput. In multi-threaded code this can cause one thread to observe memory writes in a different order than another thread produced them.

### memory_order_relaxed

No ordering guarantees beyond atomicity. Used for counters that no other thread reads immediately: `next_id_.fetch_add(1, std::memory_order_relaxed)`.

### memory_order_release / memory_order_acquire

`release` on a write: all prior writes are guaranteed visible to any thread that subsequently does an `acquire` load on the same location.

In the SPSC queue:
```cpp
// Producer
buffer_[h] = item;
head_.store(next_h, std::memory_order_release); // "item is ready"

// Consumer
if (t == head_.load(std::memory_order_acquire)) // "I can now safely read the item"
    return std::nullopt;
T item = buffer_[t];
```

Without release/acquire, the CPU could reorder `head_.store` before `buffer_[h] = item`. The consumer would read the updated index but see garbage in the buffer slot. This happens in practice on ARM CPUs.

### memory_order_seq_cst

Fully sequentially consistent. Generates an `mfence` instruction on x86, which flushes the CPU store buffer (~20–40 cycles). This is the default when no order is specified — correct but expensive. Only the places in the engine that need total order across multiple atomics use it.

Relevant code: `engine/spsc_queue.hpp` lines 16–35, `engine/memory_pool.hpp`.

---

## 3. The SPSC queue

The queue is a fixed-size ring buffer. The producer increments `head_`, the consumer increments `tail_`. When they are equal the queue is empty; when `head_ + 1 == tail_` it is full.

**Why not std::queue:** `std::queue` allocates a node per element via `malloc`. Under contention, `malloc` takes an internal lock. On the hot path that is unacceptable. The ring buffer is a fixed allocation — `push` and `pop` are a bounds check, an array access, and an atomic store.

**Power-of-two capacity:** Index wrapping uses `& MASK` instead of `% Capacity`. Modulo compiles to an integer division (20–30 cycles). A bitmask is one AND instruction (1 cycle). Enforced by `static_assert((Capacity & (Capacity - 1)) == 0)`.

**Why SPSC and not MPSC/MPMC:** A multi-producer or multi-consumer queue requires CAS loops on both head and tail. SPSC is the minimal case: exactly one thread writes `head_`, exactly one reads `tail_`. They never contend on the same atomic variable.

Relevant code: `engine/spsc_queue.hpp`.

---

## 4. The memory pool (Treiber stack)

### Pre-allocated slab

All 65 536 `Order` objects are allocated at construction in one contiguous array: `std::array<Order, 65536> storage_`. Allocation and deallocation thereafter are O(1) pointer operations with no system calls and no interaction with `malloc`.

### Lock-free free list

The free list is a stack implemented with a single atomic pointer `head_`. Each free `Order` uses its `next` field to point to the next free slot.

**Allocate:**
```cpp
T* old = head_.load(memory_order_acquire);
while (old) {
    if (head_.compare_exchange_weak(old, old->next, ...))
        return old;  // CAS succeeded: we took the top element
    // CAS failed: another thread raced us; retry with the new head
}
```

**Deallocate:**
```cpp
node->next = head_.load(memory_order_relaxed);
while (!head_.compare_exchange_weak(node->next, node, ...));
```

`compare_exchange_weak` maps to `lock cmpxchg`: atomically check that `head_` still equals `old`, and if so swap it with `old->next`. If another thread changed `head_` in the meantime, the CAS fails and the loop retries.

**compare_exchange_weak vs strong:** `_weak` can fail spuriously on LL/SC architectures (ARM). It is preferred in retry loops because the hardware implements it more efficiently. `_strong` guarantees no spurious failures but costs more.

Relevant code: `engine/memory_pool.hpp`.

---

## 5. Cache lines, false sharing, and alignas(64)

The CPU reads memory in 64-byte blocks called cache lines. When core A writes to a location, the cache coherence protocol (MESI) invalidates that cache line in core B's L1 cache. Core B must reload the line on its next access, even if it was reading a different variable that happened to share the same 64 bytes.

### alignas(64) on head_ and tail_

```cpp
alignas(64) std::atomic<std::size_t> head_{0};
alignas(64) std::atomic<std::size_t> tail_{0};
```

Without this, `head_` and `tail_` would sit adjacent in memory, sharing a cache line. Every write to `head_` by the producer would invalidate `tail_` in the consumer's cache, and vice versa — one cache miss per push and pop for no logical reason. Forcing them onto separate cache lines eliminates this.

### alignas(64) on Order

```cpp
struct alignas(64) Order { ... };
static_assert(sizeof(Order) == 64);
```

Each `Order` occupies exactly one cache line. The matching loop walks a linked list of orders at a price level. With this layout, loading each order requires exactly one cache line fetch. Without alignment, two adjacent orders could straddle a cache line boundary, requiring two fetches per order.

Relevant code: `engine/order_types.hpp` line 19, `engine/spsc_queue.hpp` lines 47–48.

---

## 6. Price ladder — flat array vs std::map

### std::map (rejected)

`std::map<double, queue<Order*>>` is a red-black tree. Each node is a separate heap allocation scattered in memory. Finding a price level is O(log n) with pointer-chased comparisons — each comparison may be a cache miss. The matching loop walks levels inward from the best price: in a tree, in-order traversal follows pointers at each step, each potentially a cache miss.

### Flat array (used)

```cpp
std::vector<Level> bids_(num_levels_);
std::vector<Level> asks_(num_levels_);
// access: bids_[priceToIdx(price)]
```

`priceToIdx` is one multiplication and one addition. Array access is one load. If the ladder is in cache, this is 1–4 cycles total.

The matching loop walks adjacent slots inward from best price. This is a sequential memory scan — the hardware prefetcher recognises the pattern and loads the next cache line before it is needed.

**Memory cost:** [0, 500] with tick 0.01 → 50 001 levels × 2 sides × 32 bytes ≈ 3.2 MB per book. This fits in L3 cache on modern processors.

Relevant code: `engine/price_ladder.hpp`, `engine/price_ladder.cpp`.

---

## 7. Intrusive linked list for price-level queues

Each price level is a FIFO queue of resting orders. `std::list<Order*>` would allocate a list node per element via `malloc`, which under contention takes an internal lock.

An intrusive list stores the link pointer inside the element itself:

```cpp
struct alignas(64) Order {
    // ... trading fields ...
    Order* next;  // intrusive link: free-list in pool, or price-level queue
};
```

Inserting an order into a price level is two pointer assignments: `tail->next = order; tail = order`. No allocation. The `Order` already lives in the memory pool — we reuse the pointer field we paid for.

**Dual use of `next`:** While an order is in the free list it links free slots. Once allocated and inserted into a price level it links orders at that level. The two uses never overlap: an order cannot be simultaneously free and in the book.

Relevant code: `engine/order_types.hpp` line 32, `engine/price_ladder.cpp` lines 7–20.

---

## 8. RDTSC — measuring time without a syscall

`clock_gettime(CLOCK_MONOTONIC)` costs ~20–50 ns even with the vDSO optimisation (which avoids a full syscall by mapping the clock into user space). It reads a kernel data structure from memory and converts to nanoseconds.

`__rdtsc()` reads the CPU timestamp counter register directly in one instruction. No memory access, no kernel involvement. Cost: ~1 CPU cycle.

**Calibration:** RDTSC returns cycles, not nanoseconds. We calibrate once at startup by comparing RDTSC ticks against `steady_clock` over 10 ms to derive `cpu_ghz`, then convert: `nanoseconds = tsc_delta / cpu_ghz`.

**Limitation:** If the CPU changes frequency between calibration and measurement (power saving on laptops), the conversion drifts. On server hardware with a fixed-frequency CPU this is not an issue.

Relevant code: `engine/latency.hpp`.

---

## 9. Template callbacks vs std::function

The `reclaim` callback in the match loop is called once per filled order. With `std::function`:

```cpp
std::vector<Trade> match(std::function<void(Order*)> on_filled = {});
```

Constructing `std::function` from a lambda that captures variables allocates a heap object to store the captures. Each call dispatches through a virtual function pointer — one indirect branch the CPU branch predictor cannot reliably inline.

With a template parameter:

```cpp
template<typename F = std::nullptr_t>
std::vector<Trade> match(F on_filled = nullptr) {
    // ...
    if constexpr (!std::is_null_pointer_v<std::decay_t<F>>)
        on_filled(bid);
}
```

When `F` is a concrete lambda type, the compiler sees the exact function at instantiation time and inlines the body directly into the match loop. Zero heap allocation, zero indirect branch.

When `F` is `std::nullptr_t` (the default), the `if constexpr` branch is eliminated at compile time. The machine code for the no-callback case contains no trace of a callback.

Relevant code: `engine/price_ladder.hpp`, template section.

---

## 10. Flat array for order-to-level lookup

When an order is cancelled, the engine needs to know which price level it sits at. With `unordered_map<uint64_t, int32_t>`: hash the key, probe a bucket, follow a pointer — 20–50 ns.

With a flat array:

```cpp
std::vector<int32_t> order_level_;  // size = pool_capacity
// write: order_level_[order->id % order_cap_] = idx;
// read:  idx = order_level_[order->id % order_cap_];
```

One modulo and one array load — 1–4 cycles.

**Why this is collision-free:** Order IDs are assigned sequentially. The pool has 65 536 slots and at most 65 536 live orders simultaneously. If order ID 65 537 is alive, order ID 1 must already be dead (its pool slot was reused). Therefore `order_level_[65537 % 65536]` is safe: the previous occupant of that slot has already been cleaned up.

Relevant code: `engine/price_ladder.hpp` (`order_level_`), `engine/price_ladder.cpp` constructor.

---

## 11. Symbol as uint64_t

Trading symbols are 1–8 ASCII characters. The engine stores one `PriceLadder` per symbol in a hash map.

With `std::string` as key: hashing calls `strlen` then iterates over characters. Even with SSO (no heap allocation for short strings), the hash function iterates byte by byte.

With `uint64_t`:
```cpp
static uint64_t symbolToKey(const char* s) noexcept {
    uint64_t key = 0;
    std::memcpy(&key, s, std::min(strlen(s), size_t(8)));
    return key;
}
```

The 8 bytes of the symbol are reinterpreted as one integer. Hashing a `uint64_t` is a single multiply-shift operation (~2 cycles). The map lookup is otherwise identical.

Relevant code: `engine/matching_engine.hpp` (`symbolToKey`, `books_`).

---

## 12. Producer/consumer threading

In single-thread mode (`processAll()` called by the same thread that submits), the lock-free data structures are never actually contested. The design is only exercised under real concurrency when `startMatcherThread()` is used.

In two-thread mode the producer (main thread) and consumer (matcher thread) run simultaneously. Their only shared state is the SPSC queue. The acquire/release ordering ensures the consumer sees fully-written orders. The `alignas(64)` placement of `head_` and `tail_` prevents false sharing between the two cores.

**yield vs busy-spin:** When the queue is empty, `matcherLoop()` calls `std::this_thread::yield()` to give the CPU to other threads. In a production HFT system the consumer would busy-spin (no yield) to minimise wake-up latency, at the cost of 100% CPU usage on a dedicated core. Yield is the correct tradeoff for a demo that shares a machine.

**Clean shutdown:** `stopMatcherThread()` stores `stop_flag_ = true` with `memory_order_release`, then joins the thread. The join guarantees all queued orders are processed before stats are read.

Relevant code: `engine/matching_engine.cpp` (`matcherLoop`, `startMatcherThread`, `stopMatcherThread`).

---

## 13. Iceberg orders and p99 latency spikes

When the visible tranche of an iceberg is fully filled, `dispatchOrder()` allocates a new `Order` from the pool, copies the fields, sets `remaining = min(peak_size, hidden_remaining)`, and inserts it back into the book via the `renewals` list. If an aggressive market order sweeps multiple tranches, each fill triggers another re-insertion and re-match cycle. One call to `dispatchOrder()` executes multiple match cycles.

This is why p99 is an order of magnitude above p50 in the synthetic benchmark (67 919 ns vs 644 ns). With only LIMIT orders, every dispatch is a single match cycle and p99 drops close to p50. The NASDAQ benchmark shows a much lower p99 (2 339 ns) because real order flow contains very few icebergs.

Relevant code: `engine/matching_engine.cpp` (`dispatchOrder`, `reclaim` lambda).

---

## 14. Reading benchmark numbers

### Synthetic vs real data

The synthetic benchmark distributes prices uniformly across a $4 range with all six order types, maximising fills and iceberg replenishments. It is a worst-case stress test.

The NASDAQ benchmark replays real orders that cluster near the market price and are predominantly LIMIT type. The matching loop exits sooner (fewer levels crossed), and icebergs are rare. This is why real data shows 3× higher throughput and much lower p99 than synthetic.

Both numbers are useful: synthetic gives a reproducible worst-case baseline; real data validates performance under realistic market conditions.

### Queue wait time

"Avg queue wait" is the time from `submit()` to the moment `processAll()` dequeues the order. In single-thread mode it is near zero. In two-thread mode a large value means the consumer cannot keep up with the producer — the queue is building up.

### Latency distribution

- **p50**: median dispatch time — price-level lookup + match loop for a typical order.
- **p95**: heavier orders, slight cache miss on a cold price level.
- **p99**: iceberg replenishment cycles, or a cold symbol book.
- **p99.9 / max**: OS scheduler preemption, page fault. Not meaningful for performance characterisation.

---

## 15. C++20 features used

### std::jthread and std::stop_token

`std::jthread` replaces `std::thread` + `std::atomic<bool> stop_flag_` for the matcher thread. Two improvements:

1. **Automatic join on destruction**: `jthread`'s destructor calls `request_stop()` then `join()` automatically. The explicit `~MatchingEngine()` destructor is no longer needed — the compiler-generated one is correct.

2. **Cooperative cancellation via `std::stop_token`**: Instead of checking a raw `atomic<bool>`, the matcher loop receives a `std::stop_token` and calls `stop.stop_requested()`. The stop signal is propagated through the token, not through a shared atomic variable that could be misused.

```cpp
// Before (C++17)
std::thread        matcher_thread_;
std::atomic<bool>  stop_flag_{false};

void matcherLoop() {
    while (!stop_flag_.load(std::memory_order_relaxed) || !queue_.empty()) { ... }
}

// After (C++20)
std::jthread matcher_thread_;

void matcherLoop(std::stop_token stop) {
    while (!stop.stop_requested() || !queue_.empty()) { ... }
}
```

Relevant code: `engine/matching_engine.hpp`, `engine/matching_engine.cpp`.

### Concepts — IntrusiveNode

```cpp
template<typename T>
concept IntrusiveNode = requires(T t) { { t.next } -> std::convertible_to<T*>; };

template<IntrusiveNode T, std::size_t Capacity>
class MemoryPool { ... };
```

`MemoryPool` requires its element type to expose a `T* next` pointer for intrusive free-list linking. Previously this was documented in a comment; the concept makes the constraint compile-time enforceable. Passing a type without a `next` pointer produces a clear error at instantiation rather than a cryptic linker failure.

Relevant code: `engine/memory_pool.hpp`.

### [[likely]] and [[unlikely]]

```cpp
if (next_h == tail_.load(std::memory_order_acquire)) [[unlikely]]
    return false;  // queue full — rare on a healthy system

while (old) [[likely]] {  // pool exhaustion is the exceptional case
```

These attributes tell the compiler which branch is taken in the common case. The compiler uses this to lay out the machine code so the hot path is a straight line (no branch taken, no pipeline flush). On x86 this also affects static branch prediction hints in the instruction encoding.

Relevant code: `engine/spsc_queue.hpp`, `engine/memory_pool.hpp`.
