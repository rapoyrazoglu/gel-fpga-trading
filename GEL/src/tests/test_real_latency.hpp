// src/tests/test_real_latency.hpp
#pragma once

#include "test_utils.hpp"
#include "gel/performance/latency_profiler.hpp"
#include "gel/data/tick.hpp"
#include "gel/data/ohlc.hpp"
#include <vector>
#include <random>
#include <cmath>

namespace gel::test {

    using namespace gel::performance;

    // Simple SMA implementation for testing
    class SimpleSMA {
    public:
        explicit SimpleSMA(size_t period) : period_(period) {
            values_.reserve(period);
        }

        double update(double value) {
            values_.push_back(value);
            if (values_.size() > period_) {
                values_.erase(values_.begin());
            }

            double sum = 0.0;
            for (double v : values_) {
                sum += v;
            }
            return sum / values_.size();
        }

    private:
        size_t period_;
        std::vector<double> values_;
    };

    // Simple EMA implementation
    class SimpleEMA {
    public:
        explicit SimpleEMA(size_t period)
            : alpha_(2.0 / (period + 1)), initialized_(false), ema_(0.0) {}

        double update(double value) {
            if (!initialized_) {
                ema_ = value;
                initialized_ = true;
            } else {
                ema_ = alpha_ * value + (1.0 - alpha_) * ema_;
            }
            return ema_;
        }

    private:
        double alpha_;
        bool initialized_;
        double ema_;
    };

    // Simple RSI implementation
    class SimpleRSI {
    public:
        explicit SimpleRSI(size_t period = 14) : period_(period), prev_price_(0.0), initialized_(false) {}

        double update(double price) {
            if (!initialized_) {
                prev_price_ = price;
                initialized_ = true;
                return 50.0;  // Neutral
            }

            double change = price - prev_price_;
            prev_price_ = price;

            gains_.push_back(change > 0 ? change : 0.0);
            losses_.push_back(change < 0 ? -change : 0.0);

            if (gains_.size() > period_) {
                gains_.erase(gains_.begin());
                losses_.erase(losses_.begin());
            }

            double avg_gain = 0.0, avg_loss = 0.0;
            for (size_t i = 0; i < gains_.size(); ++i) {
                avg_gain += gains_[i];
                avg_loss += losses_[i];
            }
            avg_gain /= gains_.size();
            avg_loss /= losses_.size();

            if (avg_loss == 0.0) return 100.0;
            double rs = avg_gain / avg_loss;
            return 100.0 - (100.0 / (1.0 + rs));
        }

    private:
        size_t period_;
        double prev_price_;
        bool initialized_;
        std::vector<double> gains_;
        std::vector<double> losses_;
    };

    /**
     * Test: Real indicator calculation latency
     */
    void test_real_indicator_latency() {
        LatencyProfiler profiler;

        // Create indicators
        SimpleSMA sma_fast(10);
        SimpleSMA sma_slow(50);
        SimpleEMA ema_fast(12);
        SimpleEMA ema_slow(26);
        SimpleRSI rsi(14);

        // Generate random price data
        std::mt19937 gen(42);
        std::normal_distribution<> price_change(-0.0001, 0.0003);
        double price = 1.10000;  // EUR/USD

        const int NUM_TICKS = 100000;

        auto total_start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < NUM_TICKS; ++i) {
            profiler.start_tick();

            // Simulate price movement
            price += price_change(gen);

            // === INDICATOR CALCULATION ===
            double sma_f = sma_fast.update(price);
            double sma_s = sma_slow.update(price);
            double ema_f = ema_fast.update(price);
            double ema_s = ema_slow.update(price);
            double rsi_val = rsi.update(price);

            profiler.mark_indicator_done();

            // === SIGNAL GENERATION ===
            bool buy_signal = (sma_f > sma_s) && (ema_f > ema_s) && (rsi_val < 70);
            bool sell_signal = (sma_f < sma_s) && (ema_f < ema_s) && (rsi_val > 30);

            profiler.mark_signal_done(buy_signal || sell_signal);

            // === DECISION ===
            // Simulate order preparation (minimal work)
            volatile double entry_price = price;
            volatile double stop_loss = buy_signal ? price - 0.001 : price + 0.001;
            volatile double take_profit = buy_signal ? price + 0.0015 : price - 0.0015;
            (void)entry_price; (void)stop_loss; (void)take_profit;

            profiler.end_tick(0.0);
        }

        auto total_end = std::chrono::high_resolution_clock::now();
        auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(total_end - total_start).count();

        auto stats = profiler.get_statistics();
        auto tick_to_ind = profiler.get_tick_to_indicator_stats();
        auto ind_to_sig = profiler.get_indicator_to_signal_stats();

        std::cout << "\n=== REAL INDICATOR LATENCY TEST ===\n";
        std::cout << "Ticks processed: " << NUM_TICKS << "\n";
        std::cout << "Total time: " << total_duration << " ms\n";
        std::cout << "Throughput: " << (NUM_TICKS * 1000.0 / total_duration) << " ticks/sec\n\n";

        std::cout << "Indicators used:\n";
        std::cout << "  - SMA(10), SMA(50)\n";
        std::cout << "  - EMA(12), EMA(26)\n";
        std::cout << "  - RSI(14)\n\n";

        std::cout << "--- Total Tick-to-Signal ---\n";
        std::cout << "  Min:  " << stats.min_us() << " µs\n";
        std::cout << "  Max:  " << stats.max_us() << " µs\n";
        std::cout << "  Mean: " << stats.mean_us() << " µs\n";
        std::cout << "  P50:  " << stats.p50_us() << " µs\n";
        std::cout << "  P99:  " << stats.p99_us() << " µs\n\n";

        std::cout << "--- Tick-to-Indicator (5 indicators) ---\n";
        std::cout << "  Mean: " << tick_to_ind.mean_us() << " µs\n";
        std::cout << "  P99:  " << tick_to_ind.p99_us() << " µs\n\n";

        std::cout << "--- Indicator-to-Signal ---\n";
        std::cout << "  Mean: " << ind_to_sig.mean_us() << " µs\n";
        std::cout << "  P99:  " << ind_to_sig.p99_us() << " µs\n\n";

        // Assertions
        ASSERT_TRUE(stats.sample_count == NUM_TICKS);
        ASSERT_TRUE(stats.mean_us() < 100.0);  // Should be under 100 µs

        std::cout << "=== CONCLUSION ===\n";
        std::cout << "Real tick-to-signal: ~" << stats.mean_us() << " µs\n";
        std::cout << "This is CPU processing only (no network I/O)\n";
    }

    /**
     * Test: Compare CPU vs simulated FPGA
     */
    void test_cpu_vs_fpga_comparison() {
        std::cout << "\n=== CPU vs FPGA COMPARISON ===\n\n";

        // CPU Implementation
        SimpleSMA cpu_sma(20);
        std::vector<double> test_data(10000);
        std::mt19937 gen(42);
        std::normal_distribution<> dist(1.1, 0.01);
        for (auto& v : test_data) v = dist(gen);

        // Measure CPU SMA
        auto cpu_start = std::chrono::high_resolution_clock::now();
        for (int iter = 0; iter < 100; ++iter) {
            SimpleSMA sma(20);
            for (double price : test_data) {
                volatile double result = sma.update(price);
                (void)result;
            }
        }
        auto cpu_end = std::chrono::high_resolution_clock::now();
        auto cpu_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(cpu_end - cpu_start).count();
        double cpu_per_update = cpu_ns / (100.0 * 10000.0);

        std::cout << "CPU SMA(20) Performance:\n";
        std::cout << "  Per update: " << cpu_per_update << " ns\n";
        std::cout << "  Throughput: " << (1e9 / cpu_per_update) << " updates/sec\n\n";

        // FPGA would be (estimated):
        // - Fixed-point arithmetic: ~10 clock cycles
        // - At 200 MHz: 10 / 200e6 = 50 ns per update
        // - But with pipelining: 1 update per clock = 5 ns
        double fpga_estimated_ns = 5.0;  // Pipelined FPGA at 200 MHz

        std::cout << "FPGA SMA(20) Estimated (200 MHz, pipelined):\n";
        std::cout << "  Per update: " << fpga_estimated_ns << " ns\n";
        std::cout << "  Throughput: " << (1e9 / fpga_estimated_ns) << " updates/sec\n\n";

        std::cout << "Speedup: " << (cpu_per_update / fpga_estimated_ns) << "x\n\n";

        std::cout << "=== KEY INSIGHT ===\n";
        std::cout << "CPU is fast enough for most trading (~" << cpu_per_update << " ns)\n";
        std::cout << "FPGA advantage comes from:\n";
        std::cout << "  1. Deterministic latency (no OS jitter)\n";
        std::cout << "  2. Parallel processing (multiple indicators)\n";
        std::cout << "  3. Direct hardware I/O (bypass network stack)\n";
        std::cout << "  4. Sub-microsecond for HFT requirements\n";

        ASSERT_TRUE(cpu_per_update < 1000.0);  // Should be under 1 µs
    }

    /**
     * Run real latency tests
     */
    void run_real_latency_tests() {
        std::cout << "\n===== REAL LATENCY TESTS =====\n";

        run_test("Real Indicator Latency", test_real_indicator_latency);
        run_test("CPU vs FPGA Comparison", test_cpu_vs_fpga_comparison);

        std::cout << "\n===== REAL LATENCY TESTS COMPLETE =====\n";
    }

} // namespace gel::test
