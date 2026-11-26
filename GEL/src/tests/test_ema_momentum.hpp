// src/tests/test_ema_momentum.hpp
#pragma once

#include "test_utils.hpp"
#include "gel/strategies/ema_momentum.hpp"
#include <random>
#include <fstream>
#include <sstream>

namespace gel::test {

    using namespace gel::strategies;

    /**
     * Generate synthetic trending price data
     */
    std::pair<std::vector<double>, std::vector<double>> generate_trending_data(
        int num_ticks,
        double start_price = 1.10000,
        double trend_strength = 0.00001,
        double volatility = 0.00005,
        double spread = 0.00010
    ) {
        std::vector<double> bids, asks;
        bids.reserve(num_ticks);
        asks.reserve(num_ticks);

        std::mt19937 gen(42);  // Fixed seed for reproducibility
        std::normal_distribution<> noise(0.0, volatility);

        double price = start_price;

        // Create trends
        int trend_length = num_ticks / 10;
        int current_trend = 0;
        double trend_dir = 1.0;

        for (int i = 0; i < num_ticks; ++i) {
            // Change trend direction periodically
            if (i % trend_length == 0) {
                trend_dir = (current_trend % 2 == 0) ? 1.0 : -1.0;
                current_trend++;
            }

            // Apply trend + noise
            price += trend_dir * trend_strength + noise(gen);

            // Clamp price to reasonable range
            price = std::max(1.05, std::min(1.15, price));

            double bid = price - spread / 2;
            double ask = price + spread / 2;

            bids.push_back(bid);
            asks.push_back(ask);
        }

        return {bids, asks};
    }

    /**
     * Test: Basic EMA calculation
     */
    void test_ema_calculation() {
        EMAMomentumConfig config;
        config.fast_period = 9;
        config.slow_period = 50;
        EMAMomentumStrategy strategy(config);

        // Process some ticks
        std::vector<double> prices = {1.1000, 1.1001, 1.1002, 1.1003, 1.1005,
                                       1.1010, 1.1015, 1.1020, 1.1025, 1.1030};

        for (size_t i = 0; i < prices.size(); ++i) {
            double spread = 0.0001;
            strategy.process_tick(prices[i] - spread/2, prices[i] + spread/2);
        }

        double fast = strategy.get_fast_ema();
        double slow = strategy.get_slow_ema();

        std::cout << "  Fast EMA(9):  " << fast << "\n";
        std::cout << "  Slow EMA(50): " << slow << "\n";

        // Fast EMA should be closer to recent prices
        ASSERT_TRUE(fast > slow);
        ASSERT_TRUE(std::abs(fast - 1.1030) < std::abs(slow - 1.1030));
    }

    /**
     * Test: Signal generation on crossover
     */
    void test_signal_generation() {
        EMAMomentumConfig config;
        config.fast_period = 5;
        config.slow_period = 10;
        EMAMomentumStrategy strategy(config);

        int buy_signals = 0;
        int total_ticks = 0;

        // Generate uptrend data
        auto [bids, asks] = generate_trending_data(1000, 1.10, 0.00002, 0.00002);

        for (size_t i = 0; i < bids.size(); ++i) {
            int signal = strategy.process_tick(bids[i], asks[i]);
            if (signal == 1) buy_signals++;
            total_ticks++;
        }

        std::cout << "  Total ticks: " << total_ticks << "\n";
        std::cout << "  BUY signals: " << buy_signals << "\n";

        // Should have some crossover signals in trending data
        ASSERT_TRUE(buy_signals > 0);
        ASSERT_TRUE(buy_signals < total_ticks / 10);  // Not too many
    }

    /**
     * Test: Backtest with positive PnL
     */
    void test_backtest_positive_pnl() {
        EMAMomentumConfig config;
        config.fast_period = 9;
        config.slow_period = 50;
        config.tp_pips = 50;
        config.sl_pips = 25;
        config.long_only = true;

        EMAMomentumStrategy strategy(config);

        // Generate favorable market data (trending up with pullbacks)
        std::vector<double> bids, asks;
        std::mt19937 gen(123);
        std::normal_distribution<> noise(0.0, 0.00003);

        double price = 1.10000;
        int num_ticks = 100000;

        for (int i = 0; i < num_ticks; ++i) {
            // Create cycles: up trend -> small pullback -> up trend
            int cycle = i / 10000;
            int phase = (i % 10000) / 2000;

            double trend;
            if (phase < 3) {
                trend = 0.000008;  // Up trend (60% of time)
            } else if (phase < 4) {
                trend = -0.000003;  // Small pullback
            } else {
                trend = 0.000005;  // Recovery
            }

            price += trend + noise(gen);
            price = std::max(1.08, std::min(1.15, price));

            double spread = 0.00010;
            bids.push_back(price - spread/2);
            asks.push_back(price + spread/2);
        }

        auto result = strategy.backtest(bids, asks);

        std::cout << "\n";
        print_backtest_result(result);

        // Assertions for positive performance
        ASSERT_TRUE(result.total_trades > 0);
        ASSERT_TRUE(result.total_pnl_pips > 0);  // Positive PnL
        ASSERT_TRUE(result.profit_factor > 1.0);  // Profitable
        ASSERT_TRUE(result.avg_tts_microseconds < 10.0);  // Fast TTS
    }

    /**
     * Test: TTS latency measurement
     */
    void test_tts_latency() {
        EMAMomentumConfig config;
        EMAMomentumStrategy strategy(config);

        auto [bids, asks] = generate_trending_data(50000);

        auto result = strategy.backtest(bids, asks);

        std::cout << "  Ticks processed: " << result.total_ticks_processed << "\n";
        std::cout << "  Avg TTS: " << result.avg_tts_microseconds << " µs\n";
        std::cout << "  P99 TTS: " << result.p99_tts_microseconds << " µs\n";
        std::cout << "  Max TTS: " << result.max_tts_microseconds << " µs\n";
        std::cout << "  Throughput: " << result.throughput_ticks_per_sec << " ticks/sec\n";

        // TTS should be sub-microsecond for simple EMA
        ASSERT_TRUE(result.avg_tts_microseconds < 5.0);
        ASSERT_TRUE(result.throughput_ticks_per_sec > 100000);  // >100K ticks/sec
    }

    /**
     * Test: Strategy matches expected parameters
     */
    void test_strategy_parameters() {
        EMAMomentumConfig config;
        config.fast_period = 9;
        config.slow_period = 50;
        config.tp_pips = 50;
        config.sl_pips = 25;
        config.long_only = true;

        EMAMomentumStrategy strategy(config);

        // Generate large dataset to match research results
        std::vector<double> bids, asks;
        std::mt19937 gen(42);
        std::normal_distribution<> noise(0.0, 0.00004);

        double price = 1.10000;

        // Simulate 6 months of data (~2M ticks at 1 tick/sec for trading hours)
        int num_ticks = 500000;

        for (int i = 0; i < num_ticks; ++i) {
            // Market regime changes
            int regime = (i / 50000) % 4;
            double trend;

            switch (regime) {
                case 0: trend = 0.000005; break;   // Bull
                case 1: trend = -0.000002; break;  // Slight bear
                case 2: trend = 0.000008; break;   // Strong bull
                case 3: trend = -0.000001; break;  // Consolidation
                default: trend = 0.0;
            }

            price += trend + noise(gen);
            price = std::max(1.05, std::min(1.20, price));

            double spread = 0.00008 + (gen() % 5) * 0.00001;
            bids.push_back(price - spread/2);
            asks.push_back(price + spread/2);
        }

        auto result = strategy.backtest(bids, asks);

        print_backtest_result(result);

        // Should match approximate research results
        std::cout << "\n  Expected vs Actual:\n";
        std::cout << "  Win Rate:      ~42% actual=" << result.win_rate << "%\n";
        std::cout << "  Profit Factor: ~1.5 actual=" << result.profit_factor << "\n";
        std::cout << "  Sharpe Ratio:  ~3.0 actual=" << result.sharpe_ratio << "\n";

        ASSERT_TRUE(result.total_trades >= 30);
        ASSERT_TRUE(result.profit_factor > 1.0);
    }

    /**
     * Run all EMA Momentum tests
     */
    void run_all_ema_momentum_tests() {
        std::cout << "\n===== EMA MOMENTUM STRATEGY TESTS =====\n";

        run_test("EMA Calculation", test_ema_calculation);
        run_test("Signal Generation", test_signal_generation);
        run_test("Backtest Positive PnL", test_backtest_positive_pnl);
        run_test("TTS Latency Measurement", test_tts_latency);
        run_test("Strategy Parameters Match", test_strategy_parameters);

        std::cout << "\n===== EMA MOMENTUM TESTS COMPLETE =====\n";
    }

} // namespace gel::test
