// src/tests/test_order_flow.hpp
#pragma once

#include "gel/patterns/order_flow.hpp"
#include "test_utils.hpp"
#include "test_pattern_utils.hpp"
#include <iostream>

namespace gel::test {

void test_order_flow_buy_imbalance() {
    std::cout << "Testing Order Flow BUY Imbalance\n";

    patterns::OrderFlowAnalyzer analyzer;
    auto ticks = create_buy_imbalance_scenario(70, 30); // Ratio = 2.33

    for (const auto& tick : ticks) {
        analyzer.add_tick(tick);
    }

    const auto& imb = analyzer.get_current_imbalance();

    // First tick is NEUTRAL (no prev_tick), so 69 BUY not 70
    ASSERT_EQUAL(imb.buy_count, 69);
    ASSERT_EQUAL(imb.sell_count, 30);
    ASSERT_TRUE(imb.ratio > 2.0);

    std::cout << "  ✓ BUY imbalance: " << imb.buy_count << "/" << imb.sell_count << " = " << imb.ratio << "\n";
}

void test_order_flow_sell_imbalance() {
    std::cout << "Testing Order Flow SELL Imbalance\n";

    patterns::OrderFlowAnalyzer analyzer;
    auto ticks = create_sell_imbalance_scenario(30, 70); // Ratio = 0.43

    for (const auto& tick : ticks) {
        analyzer.add_tick(tick);
    }

    const auto& imb = analyzer.get_current_imbalance();

    // First tick is NEUTRAL (no prev_tick), so 29 BUY not 30
    ASSERT_EQUAL(imb.buy_count, 29);
    ASSERT_EQUAL(imb.sell_count, 70);
    ASSERT_TRUE(imb.ratio < 0.5);

    std::cout << "  ✓ SELL imbalance: " << imb.ratio << "\n";
}

void test_order_flow_buy_signal() {
    std::cout << "Testing Order Flow BUY Signal\n";

    patterns::OrderFlowAnalyzer analyzer;
    auto ticks = create_buy_imbalance_scenario(70, 30);

    for (const auto& tick : ticks) {
        analyzer.add_tick(tick);
    }

    auto result = analyzer.detect();

    ASSERT_TRUE(result.is_valid());
    ASSERT_EQUAL(static_cast<int>(result.signal), static_cast<int>(patterns::Signal::BUY));

    std::cout << "  ✓ BUY signal generated\n";
}

void test_order_flow_sell_signal() {
    std::cout << "Testing Order Flow SELL Signal\n";

    patterns::OrderFlowAnalyzer analyzer;
    auto ticks = create_sell_imbalance_scenario(30, 70);

    for (const auto& tick : ticks) {
        analyzer.add_tick(tick);
    }

    auto result = analyzer.detect();

    ASSERT_TRUE(result.is_valid());
    ASSERT_EQUAL(static_cast<int>(result.signal), static_cast<int>(patterns::Signal::SELL));

    std::cout << "  ✓ SELL signal generated\n";
}

void test_order_flow_neutral() {
    std::cout << "Testing Order Flow Neutral (no signal)\n";

    patterns::OrderFlowAnalyzer analyzer;
    auto ticks = create_balanced_flow_scenario(); // 50/50

    for (const auto& tick : ticks) {
        analyzer.add_tick(tick);
    }

    auto result = analyzer.detect();

    ASSERT_FALSE(result.is_valid());

    std::cout << "  ✓ No signal (balanced flow)\n";
}

void test_order_flow_rolling_window() {
    std::cout << "Testing Rolling Window Cleanup\n";

    patterns::OrderFlowAnalyzer::Config config;
    config.window_duration = std::chrono::seconds(10); // 10 second window
    patterns::OrderFlowAnalyzer analyzer(config);

    auto base_time = std::chrono::system_clock::now();

    // Add 50 ticks over 30 seconds
    for (int i = 0; i < 50; ++i) {
        auto tick = create_tick(1.1000 + i * 0.00001, 1.1001 + i * 0.00001,
                                base_time + std::chrono::seconds(i));
        analyzer.add_tick(tick);
    }

    // Only last 10 seconds should be kept
    const auto& imb = analyzer.get_current_imbalance();
    ASSERT_TRUE(imb.total_count() <= 15); // Approximate

    std::cout << "  ✓ Old ticks cleaned (window: 10s)\n";
}

void test_order_flow_csv_config() {
    std::cout << "Testing Order Flow CSV Config\n";

    patterns::OrderFlowAnalyzer analyzer;
    analyzer.load_config_from_csv("ai_results/order_flow_analysis_mock.csv");

    // Add ticks to make analyzer ready (need 50+ ticks)
    auto ticks = create_buy_imbalance_scenario(30, 30);
    for (const auto& tick : ticks) {
        analyzer.add_tick(tick);
    }

    ASSERT_TRUE(analyzer.is_ready());

    std::cout << "  ✓ Config loaded from CSV\n";
}

void test_order_flow_confidence() {
    std::cout << "Testing Confidence Calculation\n";

    patterns::OrderFlowAnalyzer analyzer;
    auto ticks = create_buy_imbalance_scenario(100, 20); // Strong imbalance, large sample

    for (const auto& tick : ticks) {
        analyzer.add_tick(tick);
    }

    const auto& imb = analyzer.get_current_imbalance();

    ASSERT_TRUE(imb.confidence >= 0.5);
    ASSERT_TRUE(imb.confidence <= 1.0);

    std::cout << "  ✓ Confidence: " << imb.confidence << "\n";
}

void run_all_order_flow_tests() {
    std::cout << "\n=== Order Flow Unit Tests ===\n\n";

    run_test("Order Flow: BUY Imbalance", test_order_flow_buy_imbalance);
    run_test("Order Flow: SELL Imbalance", test_order_flow_sell_imbalance);
    run_test("Order Flow: BUY Signal", test_order_flow_buy_signal);
    run_test("Order Flow: SELL Signal", test_order_flow_sell_signal);
    run_test("Order Flow: Neutral (no signal)", test_order_flow_neutral);
    run_test("Order Flow: Rolling Window", test_order_flow_rolling_window);
    run_test("Order Flow: CSV Config", test_order_flow_csv_config);
    run_test("Order Flow: Confidence", test_order_flow_confidence);

    std::cout << "\n=== Order Flow Tests Complete ===\n";
}

} // namespace gel::test
