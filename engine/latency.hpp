#pragma once
#include <cstdint>
#include <vector>
#include <algorithm>
#include <numeric>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <chrono>

// __rdtsc() reads the CPU timestamp counter in ~1 cycle with no syscall.
// Calibration converts cycles to nanoseconds once at startup.
#if defined(__x86_64__) || defined(_M_X64)
#  include <x86intrin.h>
inline uint64_t rdtsc() noexcept { return __rdtsc(); }
#else
#  include <chrono>
inline uint64_t rdtsc() noexcept {
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}
#endif

inline double estimateCpuGhz() {
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    uint64_t c0 = rdtsc();
    volatile uint64_t spin = 0;
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               clock::now() - t0).count() < 10)
        ++spin;
    uint64_t c1 = rdtsc();
    auto t1 = clock::now();
    double ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    return static_cast<double>(c1 - c0) / ns; // cycles/ns = GHz
}

// Accumulates cycle counts and reports percentile latency.
class LatencyTracker {
public:
    void record(uint64_t cycles) { samples_.push_back(cycles); }

    void print(double cpu_ghz = -1.0) const {
        if (samples_.empty()) { std::cout << "No latency samples.\n"; return; }

        if (cpu_ghz <= 0.0) cpu_ghz = estimateCpuGhz();

        auto s = samples_;
        std::sort(s.begin(), s.end());

        auto ns = [&](uint64_t c) {
            return static_cast<double>(c) / cpu_ghz;
        };
        auto pct = [&](double p) -> uint64_t {
            return s[static_cast<std::size_t>(p / 100.0 * (s.size() - 1))];
        };

        double mean_cycles = std::accumulate(s.begin(), s.end(), 0.0) / s.size();

        std::cout << std::fixed << std::setprecision(1);
        std::cout << "\n=== Matching Latency (" << s.size() << " samples, "
                  << std::setprecision(2) << cpu_ghz << " GHz) ===\n";
        std::cout << std::setprecision(1);
        std::cout << "  mean   : " << ns(static_cast<uint64_t>(mean_cycles)) << " ns\n";
        std::cout << "  p50    : " << ns(pct(50))   << " ns\n";
        std::cout << "  p95    : " << ns(pct(95))   << " ns\n";
        std::cout << "  p99    : " << ns(pct(99))   << " ns\n";
        std::cout << "  p99.9  : " << ns(pct(99.9)) << " ns\n";
        std::cout << "  min    : " << ns(s.front()) << " ns\n";
        std::cout << "  max    : " << ns(s.back())  << " ns\n";
    }

    void exportCSV(const std::string& path, double cpu_ghz = -1.0) const {
        if (cpu_ghz <= 0.0) cpu_ghz = estimateCpuGhz();
        std::ofstream f(path);
        f << "sample,cycles,ns\n";
        for (std::size_t i = 0; i < samples_.size(); ++i)
            f << i << "," << samples_[i] << ","
              << std::fixed << std::setprecision(1)
              << static_cast<double>(samples_[i]) / cpu_ghz << "\n";
    }

    std::size_t count() const noexcept { return samples_.size(); }
    void clear() noexcept { samples_.clear(); }

private:
    std::vector<uint64_t> samples_;
};
