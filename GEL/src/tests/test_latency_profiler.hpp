// src/tests/test_latency_profiler.hpp
#pragma once

#include "test_utils.hpp"
#include "gel/performance/latency_profiler.hpp"
#include "gel/data/tick.hpp"
#include <thread>
#include <random>

namespace gel::test {

    using namespace gel::performance;

    /**
     * Test: Basic latency measurement
     */
    void test_basic_latency_measurement() {
        LatencyProfiler profiler;

        // Simulate 100 tick processing cycles
        for (int i = 0; i < 100; ++i) {
            profiler.start_tick("tick_" + std::to_string(i));

            // Simulate indicator calculation (100-500 µs)
            std::this_thread::sleep_for(std::chrono::microseconds(100 + (i % 400)));
            profiler.mark_indicator_done();

            // Simulate signal generation (50-200 µs)
            std::this_thread::sleep_for(std::chrono::microseconds(50 + (i % 150)));
            profiler.mark_signal_done(i % 5 == 0);  // 20% generate signals

            // Simulate decision (10-50 µs)
            std::this_thread::sleep_for(std::chrono::microseconds(10 + (i % 40)));
            profiler.end_tick((i % 5 == 0) ? (i % 2 == 0 ? 10.0 : -5.0) : 0.0);
        }

        auto stats = profiler.get_statistics();

        ASSERT_TRUE(stats.sample_count == 100);
        ASSERT_TRUE(stats.min_ns > 0);
        ASSERT_TRUE(stats.max_ns > stats.min_ns);
        ASSERT_TRUE(stats.mean_ns > 0);
        ASSERT_TRUE(stats.p50_ns > 0);
        ASSERT_TRUE(stats.p99_ns >= stats.p50_ns);

        std::cout << "  Sample count: " << stats.sample_count << "\n";
        std::cout << "  Mean latency: " << stats.mean_us() << " µs\n";
        std::cout << "  P99 latency:  " << stats.p99_us() << " µs\n";
    }

    /**
     * Test: ScopedTimer usage
     */
    void test_scoped_timer() {
        Duration elapsed;

        {
            ScopedTimer timer(elapsed);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Should be at least 10ms (10,000,000 ns)
        ASSERT_TRUE(elapsed.count() >= 9000000);  // Allow some tolerance

        std::cout << "  Scoped timer measured: " << (elapsed.count() / 1000000.0) << " ms\n";
    }

    /**
     * Test: Quick measurement recording
     */
    void test_quick_measurement() {
        LatencyProfiler profiler;

        // Record 1000 quick measurements
        for (int i = 0; i < 1000; ++i) {
            auto latency = Duration(1000 + (i * 100));  // 1µs to 100µs
            profiler.record_quick_measurement(latency, i % 10 == 0, i % 2 == 0 ? 5.0 : -3.0);
        }

        auto stats = profiler.get_statistics();

        ASSERT_TRUE(stats.sample_count == 1000);
        ASSERT_TRUE(stats.min_ns == 1000);
        ASSERT_TRUE(stats.max_ns == 1000 + 999 * 100);

        std::cout << "  Quick measurements: " << stats.sample_count << "\n";
        std::cout << "  Min: " << stats.min_us() << " µs, Max: " << stats.max_us() << " µs\n";
    }

    /**
     * Test: Latency-PnL correlation
     */
    void test_latency_pnl_correlation() {
        LatencyProfiler profiler;

        // Simulate: lower latency = higher win rate
        std::random_device rd;
        std::mt19937 gen(42);  // Fixed seed for reproducibility
        std::normal_distribution<> latency_dist(5000, 1000);  // Mean 5µs, std 1µs

        for (int i = 0; i < 500; ++i) {
            int64_t latency = static_cast<int64_t>(std::max(100.0, latency_dist(gen)));
            bool signal = true;

            // Lower latency -> higher chance of winning
            double pnl;
            if (latency < 4500) {
                pnl = (gen() % 100 < 70) ? 15.0 : -8.0;  // 70% win rate
            } else if (latency < 5500) {
                pnl = (gen() % 100 < 50) ? 12.0 : -10.0;  // 50% win rate
            } else {
                pnl = (gen() % 100 < 30) ? 10.0 : -12.0;  // 30% win rate
            }

            profiler.record_quick_measurement(Duration(latency), signal, pnl);
        }

        auto corr = profiler.get_latency_pnl_correlation();

        std::cout << "  Correlation coefficient: " << corr.correlation_coefficient << "\n";
        std::cout << "  Avg latency (winners): " << (corr.avg_latency_winners_ns / 1000.0) << " µs\n";
        std::cout << "  Avg latency (losers):  " << (corr.avg_latency_losers_ns / 1000.0) << " µs\n";
        std::cout << "  Winning trades: " << corr.winning_trades << "\n";
        std::cout << "  Losing trades:  " << corr.losing_trades << "\n";

        // With our setup, winners should have lower latency
        ASSERT_TRUE(corr.avg_latency_winners_ns < corr.avg_latency_losers_ns);
        // Correlation should be negative (lower latency = higher PnL)
        ASSERT_TRUE(corr.correlation_coefficient < 0);
    }

    /**
     * Test: Stage-by-stage profiling
     */
    void test_stage_profiling() {
        LatencyProfiler profiler;

        for (int i = 0; i < 50; ++i) {
            profiler.start_tick("tick_" + std::to_string(i));

            // Stage 1: Tick to Indicator (fast)
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            profiler.mark_indicator_done();

            // Stage 2: Indicator to Signal (medium)
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            profiler.mark_signal_done(true);

            // Stage 3: Signal to Decision (fast)
            std::this_thread::sleep_for(std::chrono::microseconds(25));
            profiler.end_tick(10.0);
        }

        auto tick_to_ind = profiler.get_tick_to_indicator_stats();
        auto ind_to_sig = profiler.get_indicator_to_signal_stats();
        auto sig_to_dec = profiler.get_signal_to_decision_stats();
        auto total = profiler.get_statistics();

        std::cout << "  Tick->Indicator: " << tick_to_ind.mean_us() << " µs\n";
        std::cout << "  Indicator->Signal: " << ind_to_sig.mean_us() << " µs\n";
        std::cout << "  Signal->Decision: " << sig_to_dec.mean_us() << " µs\n";
        std::cout << "  Total: " << total.mean_us() << " µs\n";

        ASSERT_TRUE(tick_to_ind.sample_count == 50);
        ASSERT_TRUE(ind_to_sig.sample_count == 50);
        ASSERT_TRUE(sig_to_dec.sample_count == 50);

        // Verify stage order makes sense
        ASSERT_TRUE(tick_to_ind.mean_ns < ind_to_sig.mean_ns);
    }

    /**
     * Test: Throughput calculation
     */
    void test_throughput() {
        LatencyProfiler profiler;

        auto start = std::chrono::high_resolution_clock::now();

        // Process 100 ticks as fast as possible
        for (int i = 0; i < 100; ++i) {
            profiler.start_tick("tick_" + std::to_string(i));
            profiler.mark_indicator_done();
            profiler.mark_signal_done(false);
            profiler.end_tick(0.0);
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        double throughput = profiler.get_throughput();

        std::cout << "  Processed 100 ticks in " << duration_ms << " ms\n";
        std::cout << "  Throughput: " << throughput << " ticks/sec\n";

        ASSERT_TRUE(throughput > 0);
        ASSERT_TRUE(profiler.count() == 100);
    }

    /**
     * Test: Report generation
     */
    void test_report_generation() {
        LatencyProfiler profiler;

        // Generate some test data
        for (int i = 0; i < 200; ++i) {
            profiler.start_tick("tick_" + std::to_string(i));
            std::this_thread::sleep_for(std::chrono::microseconds(10 + (i % 90)));
            profiler.mark_indicator_done();
            std::this_thread::sleep_for(std::chrono::microseconds(5 + (i % 45)));
            profiler.mark_signal_done(i % 3 == 0);
            std::this_thread::sleep_for(std::chrono::microseconds(2 + (i % 18)));
            profiler.end_tick(i % 3 == 0 ? (i % 2 == 0 ? 8.0 : -4.0) : 0.0);
        }

        std::string report = profiler.generate_report();

        ASSERT_TRUE(!report.empty());
        ASSERT_TRUE(report.find("LATENCY PROFILER REPORT") != std::string::npos);
        ASSERT_TRUE(report.find("Total Measurements: 200") != std::string::npos);

        std::cout << "\n" << report;
    }

    /**
     * Test: Pipeline profiler
     */
    void test_pipeline_profiler() {
        PipelineProfiler pipeline;

        // Simulate full pipeline for 50 ticks
        for (int i = 0; i < 50; ++i) {
            // Tick ingestion
            pipeline.tick_ingestion.start_tick();
            std::this_thread::sleep_for(std::chrono::microseconds(5));
            pipeline.tick_ingestion.end_tick();

            // OHLC aggregation
            pipeline.ohlc_aggregation.start_tick();
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            pipeline.ohlc_aggregation.end_tick();

            // Indicator calculation
            pipeline.indicator_calc.start_tick();
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            pipeline.indicator_calc.end_tick();

            // Pattern detection
            pipeline.pattern_detection.start_tick();
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            pipeline.pattern_detection.end_tick();

            // Signal generation
            pipeline.signal_generation.start_tick();
            std::this_thread::sleep_for(std::chrono::microseconds(20));
            pipeline.signal_generation.end_tick();
        }

        std::string report = pipeline.generate_full_report();

        ASSERT_TRUE(!report.empty());
        ASSERT_TRUE(report.find("FULL PIPELINE PROFILER REPORT") != std::string::npos);

        std::cout << "\n" << report;
    }

    /**
     * Test: Thread safety
     */
    void test_thread_safety() {
        LatencyProfiler profiler;

        auto worker = [&profiler](int thread_id, int count) {
            for (int i = 0; i < count; ++i) {
                profiler.record_quick_measurement(
                    Duration(1000 * (thread_id + 1)),
                    true,
                    thread_id % 2 == 0 ? 5.0 : -3.0
                );
            }
        };

        // Launch 4 threads, each recording 250 measurements
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back(worker, t, 250);
        }

        for (auto& t : threads) {
            t.join();
        }

        ASSERT_TRUE(profiler.count() == 1000);

        auto stats = profiler.get_statistics();
        std::cout << "  Thread-safe measurements: " << stats.sample_count << "\n";
    }

    /**
     * Test: High-frequency tick simulation
     */
    void test_high_frequency_simulation() {
        LatencyProfiler profiler;

        // Simulate 10,000 ticks (no sleep to test raw performance)
        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < 10000; ++i) {
            profiler.start_tick();

            // Simulate some minimal work
            volatile int x = 0;
            for (int j = 0; j < 100; ++j) x += j;

            profiler.mark_indicator_done();

            for (int j = 0; j < 50; ++j) x += j;

            profiler.mark_signal_done(i % 100 == 0);

            for (int j = 0; j < 25; ++j) x += j;

            profiler.end_tick(i % 100 == 0 ? 10.0 : 0.0);
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        auto stats = profiler.get_statistics();

        std::cout << "  Processed 10,000 ticks in " << duration_ms << " ms\n";
        std::cout << "  Mean tick latency: " << stats.mean_us() << " µs\n";
        std::cout << "  P99 latency: " << stats.p99_us() << " µs\n";
        std::cout << "  Throughput: " << profiler.get_throughput() << " ticks/sec\n";

        ASSERT_TRUE(profiler.count() == 10000);
        ASSERT_TRUE(stats.mean_us() < 1000);  // Should be sub-millisecond
    }

    /**
     * Test: Statistics accuracy
     */
    void test_statistics_accuracy() {
        LatencyProfiler profiler;

        // Record known values: 1, 2, 3, ..., 100 (in microseconds as nanoseconds)
        for (int i = 1; i <= 100; ++i) {
            profiler.record_quick_measurement(Duration(i * 1000), false, 0.0);
        }

        auto stats = profiler.get_statistics();

        // Min should be 1µs (1000 ns)
        ASSERT_TRUE(stats.min_ns == 1000);

        // Max should be 100µs (100000 ns)
        ASSERT_TRUE(stats.max_ns == 100000);

        // Mean should be 50.5µs (50500 ns)
        double expected_mean = 50500.0;
        ASSERT_TRUE(std::abs(stats.mean_ns - expected_mean) < 1.0);

        // P50 (median) should be around 50µs
        ASSERT_TRUE(stats.p50_ns >= 49000 && stats.p50_ns <= 51000);

        std::cout << "  Min: " << stats.min_us() << " µs (expected: 1)\n";
        std::cout << "  Max: " << stats.max_us() << " µs (expected: 100)\n";
        std::cout << "  Mean: " << stats.mean_us() << " µs (expected: 50.5)\n";
        std::cout << "  P50: " << stats.p50_us() << " µs (expected: ~50)\n";
    }

    /**
     * Run all latency profiler tests
     */
    void run_all_latency_profiler_tests() {
        std::cout << "\n===== LATENCY PROFILER TESTS =====\n";

        run_test("Basic Latency Measurement", test_basic_latency_measurement);
        run_test("Scoped Timer", test_scoped_timer);
        run_test("Quick Measurement", test_quick_measurement);
        run_test("Latency-PnL Correlation", test_latency_pnl_correlation);
        run_test("Stage Profiling", test_stage_profiling);
        run_test("Throughput Calculation", test_throughput);
        run_test("Report Generation", test_report_generation);
        run_test("Pipeline Profiler", test_pipeline_profiler);
        run_test("Thread Safety", test_thread_safety);
        run_test("High-Frequency Simulation", test_high_frequency_simulation);
        run_test("Statistics Accuracy", test_statistics_accuracy);

        std::cout << "\n===== LATENCY PROFILER TESTS COMPLETE =====\n";
    }

} // namespace gel::test
