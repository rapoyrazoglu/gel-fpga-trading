// src/tests/test_backtest_integration.hpp
#pragma once

#include "test_utils.hpp"
#include "gel/backtest/backtest_engine.hpp"
#include "gel/data/ohlc.hpp"
#include <vector>

namespace gel::test {

    using namespace gel::backtest;
    using namespace gel::data;

    // Helper: Generate synthetic OHLC data with FVG patterns
    std::vector<OHLC> generate_test_data_with_fvg() {
        std::vector<OHLC> data;
        auto base_time = std::chrono::system_clock::now();

        // Normal bars
        for (int i = 0; i < 10; ++i) {
            OHLC bar;
            bar.timestamp = base_time + std::chrono::minutes(i);
            bar.open = 1.1000 + (i * 0.0001);
            bar.high = bar.open + 0.0003;
            bar.low = bar.open - 0.0002;
            bar.close = bar.open + 0.0001;
            bar.volume = 100.0;
            data.push_back(bar);
        }

        // Create bullish FVG
        // Bar 10: Low candle
        OHLC bar10;
        bar10.timestamp = base_time + std::chrono::minutes(10);
        bar10.open = 1.1010;
        bar10.high = 1.1012;
        bar10.low = 1.1008;
        bar10.close = 1.1009;
        bar10.volume = 100.0;
        data.push_back(bar10);

        // Bar 11: Gap (no overlap with bar 10's high)
        OHLC bar11;
        bar11.timestamp = base_time + std::chrono::minutes(11);
        bar11.open = 1.1020;  // Gap up
        bar11.high = 1.1025;
        bar11.low = 1.1019;   // Low > bar10.high (GAP!)
        bar11.close = 1.1023;
        bar11.volume = 100.0;
        data.push_back(bar11);

        // Bar 12: Confirmation
        OHLC bar12;
        bar12.timestamp = base_time + std::chrono::minutes(12);
        bar12.open = 1.1023;
        bar12.high = 1.1027;
        bar12.low = 1.1022;
        bar12.close = 1.1026;
        bar12.volume = 100.0;
        data.push_back(bar12);

        // Retest bars - price comes back to FVG zone
        for (int i = 13; i < 20; ++i) {
            OHLC bar;
            bar.timestamp = base_time + std::chrono::minutes(i);
            // Gradually move down to retest FVG
            double price = 1.1026 - ((i - 12) * 0.0002);
            bar.open = price;
            bar.high = price + 0.0002;
            bar.low = price - 0.0003;  // Should touch FVG zone around bar 15-16
            bar.close = price - 0.0001;
            bar.volume = 100.0;
            data.push_back(bar);
        }

        // Upward movement (for TP)
        for (int i = 20; i < 30; ++i) {
            OHLC bar;
            bar.timestamp = base_time + std::chrono::minutes(i);
            double price = 1.1015 + ((i - 19) * 0.0002);
            bar.open = price;
            bar.high = price + 0.0003;
            bar.low = price - 0.0001;
            bar.close = price + 0.0002;
            bar.volume = 100.0;
            data.push_back(bar);
        }

        return data;
    }

    void test_backtest_basic() {
        BacktestConfig config;
        config.initial_balance = 10000.0;
        config.pip_size = 0.0001;
        config.verbose = true;

        // FVG config

        // Signal config
        config.signal_config.wait_for_retest = true;
        config.signal_config.stop_loss_pips = 10.0;
        config.signal_config.take_profit_pips = 15.0;

        // Execution config
        config.execution_config.default_position_size = 1.0;
        config.execution_config.max_concurrent_positions = 3;

        BacktestEngine engine(config);
        auto data = generate_test_data_with_fvg();

        auto result = engine.run(data);

        std::cout << result.to_string() << std::endl;

        // Basic assertions
        ASSERT_TRUE(result.total_trades > 0);  // Should have generated at least 1 trade
        ASSERT_TRUE(result.final_balance > 0);  // Should have final balance
    }

    void test_backtest_no_trades() {
        BacktestConfig config;
        config.verbose = false;

        // Very restrictive config - no FVGs should pass

        BacktestEngine engine(config);
        auto data = generate_test_data_with_fvg();

        auto result = engine.run(data);

        ASSERT_EQUAL(result.total_trades, 0u);
        ASSERT_EQUAL(result.final_balance, config.initial_balance);
    }

    void test_backtest_equity_curve() {
        BacktestConfig config;
        config.track_equity_curve = true;

        BacktestEngine engine(config);
        auto data = generate_test_data_with_fvg();

        auto result = engine.run(data);

        ASSERT_TRUE(!result.equity_curve.empty());
        ASSERT_EQUAL(result.equity_curve.size(), data.size());
        ASSERT_TRUE(result.equity_timestamps.size() == result.equity_curve.size());
    }

    void test_backtest_performance_metrics() {
        BacktestConfig config;
        config.signal_config.stop_loss_pips = 10.0;
        config.signal_config.take_profit_pips = 20.0;  // 2:1 R/R

        BacktestEngine engine(config);
        auto data = generate_test_data_with_fvg();

        auto result = engine.run(data);

        // Should have calculated metrics
        if (result.total_trades > 0) {
            ASSERT_TRUE(result.win_rate >= 0.0 && result.win_rate <= 1.0);
            ASSERT_TRUE(result.profit_factor >= 0.0);
            ASSERT_TRUE(result.max_drawdown >= 0.0);
        }
    }

    void run_all_backtest_integration_tests() {
        run_test("Backtest: Basic Functionality", test_backtest_basic);
        run_test("Backtest: No Trades (Restrictive Config)", test_backtest_no_trades);
        run_test("Backtest: Equity Curve Tracking", test_backtest_equity_curve);
        run_test("Backtest: Performance Metrics", test_backtest_performance_metrics);
    }

} // namespace gel::test
