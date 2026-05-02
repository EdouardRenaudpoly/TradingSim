#include "engine/simulation.hpp"
#include "engine/matching_engine.hpp"
#include "engine/latency.hpp"
#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <random>
#include <thread>

static void usage(const char* prog) {
    std::cerr
        << "Usage:\n"
        << "  " << prog << " --replay <data.csv> [--output <metrics.csv>] [--db <trades.db>]\n"
        << "  " << prog << " --benchmark [--orders <N>]\n"
        << "  " << prog << " --replay-benchmark <data.csv>\n";
}

static void runBenchmark(int n_orders) {
    std::cout << "Benchmark: " << n_orders << " synthetic orders"
              << " (producer thread + matcher thread)\n\n";

    // Heap allocation: MemoryPool holds 65536 Orders × 64 bytes = 4 MB inline.
    // That exceeds the typical thread stack budget when combined with ASAN/sanitizers.
    auto ep     = std::make_unique<MatchingEngine>();
    auto& engine = *ep;

    static const char* symbols[] = { "AAPL", "MSFT", "TSLA" };
    static const OrderType types[] = {
        OrderType::LIMIT, OrderType::MARKET, OrderType::IOC,
        OrderType::FOK,   OrderType::POST_ONLY, OrderType::ICEBERG
    };

    // Consumer thread starts first and spins waiting for work.
    engine.startMatcherThread();
    auto t_start = std::chrono::steady_clock::now();

    // Producer runs on the main thread.
    std::mt19937                      rng(42);
    std::uniform_real_distribution<>  price_dist(148.0, 152.0);
    std::uniform_int_distribution<>   qty_dist(1, 100);
    std::uniform_int_distribution<>   side_dist(0, 1);
    std::uniform_int_distribution<>   type_dist(0, 5);
    std::uniform_int_distribution<>   peak_dist(5, 20);

    for (int i = 0; i < n_orders; ++i) {
        const char* sym   = symbols[i % 3];
        double      price = price_dist(rng);
        int32_t     qty   = qty_dist(rng);
        Side        side  = side_dist(rng) ? Side::BUY : Side::SELL;
        OrderType   type  = types[type_dist(rng)];
        int32_t     peak  = (type == OrderType::ICEBERG) ? peak_dist(rng) : 0;
        uint64_t    tid   = static_cast<uint64_t>(i % 10 + 1);

        // Spin until the queue has room (consumer may be temporarily behind).
        while (!engine.submit(tid, sym, price, qty, side, type, peak))
            std::this_thread::yield();
    }

    // Signal consumer to stop once the queue drains, then wait for it.
    engine.stopMatcherThread();
    auto t_end    = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t_end - t_start).count();

    std::cout << "Processed  : " << engine.ordersProcessed() << " orders\n";
    std::cout << "Elapsed    : " << elapsed * 1000.0 << " ms\n";
    std::cout << "Throughput : "
              << static_cast<int>(engine.ordersProcessed() / elapsed)
              << " orders/sec\n";

    engine.stats().print();
    engine.latency().print();
}

static void runReplayBenchmark(const std::string& csv_path) {
    // Step 1: load CSV into memory to isolate parsing overhead from matching.
    std::cout << "Loading: " << csv_path << "\n";
    auto loaderp = std::make_unique<Simulation>();
    auto rows    = loaderp->loadCSV(csv_path);
    loaderp.reset(); // free Simulation (contains its own 4 MB engine) before creating another
    std::cout << "Loaded " << rows.size() << " orders\n\n";

    // Step 2: fresh engine, producer/consumer threads, same measurement as --benchmark.
    auto ep     = std::make_unique<MatchingEngine>();
    auto& engine = *ep;
    engine.startMatcherThread();
    auto t_start = std::chrono::steady_clock::now();

    for (const auto& r : rows) {
        while (!engine.submit(r.trader_id, r.symbol, r.price, r.quantity,
                              r.side, r.type, r.peak_size))
            std::this_thread::yield();
    }

    engine.stopMatcherThread();
    auto t_end    = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t_end - t_start).count();

    std::cout << "Processed  : " << engine.ordersProcessed() << " orders\n";
    std::cout << "Elapsed    : " << elapsed * 1000.0 << " ms\n";
    std::cout << "Throughput : "
              << static_cast<int>(engine.ordersProcessed() / elapsed)
              << " orders/sec\n";

    engine.stats().print();
    engine.latency().print();
}

int main(int argc, char* argv[]) {
    std::string replay_path;
    std::string replay_bench_path;
    std::string output_path;
    std::string db_path;
    bool        benchmark = false;
    int         n_orders  = 100'000;

    for (int i = 1; i < argc; ++i) {
        if      (!std::strcmp(argv[i], "--replay")           && i+1 < argc) replay_path       = argv[++i];
        else if (!std::strcmp(argv[i], "--replay-benchmark") && i+1 < argc) replay_bench_path = argv[++i];
        else if (!std::strcmp(argv[i], "--output")           && i+1 < argc) output_path       = argv[++i];
        else if (!std::strcmp(argv[i], "--db")               && i+1 < argc) db_path           = argv[++i];
        else if (!std::strcmp(argv[i], "--benchmark"))                       benchmark         = true;
        else if (!std::strcmp(argv[i], "--orders")           && i+1 < argc) n_orders          = std::atoi(argv[++i]);
        else { usage(argv[0]); return 1; }
    }

    if (benchmark)                  { runBenchmark(n_orders);            return 0; }
    if (!replay_bench_path.empty()) { runReplayBenchmark(replay_bench_path); return 0; }
    if (replay_path.empty()) { usage(argv[0]); return 1; }

    try {
        Simulation sim;
        std::cout << "Replaying: " << replay_path << "\n";
        sim.replayCSV(replay_path);
        sim.printMetrics();
        if (!output_path.empty()) {
            sim.exportMetrics(output_path);
            std::cout << "Metrics exported to: " << output_path << "\n";
        }
        if (!db_path.empty()) {
            sim.exportDB(db_path);
            std::cout << "Database written to: " << db_path << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "[error] " << e.what() << "\n";
        return 1;
    }
    return 0;
}
