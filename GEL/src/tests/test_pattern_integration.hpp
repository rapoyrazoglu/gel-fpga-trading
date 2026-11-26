// src/tests/test_pattern_integration.hpp
#pragma once

#include "gel/patterns/fair_value_gap.hpp"
#include "gel/patterns/order_flow.hpp"
#include "test_utils.hpp"
#include "test_pattern_utils.hpp"
#include <iostream>

namespace gel::test {

void test_fvg_end_to_end() {
    std::cout << "Testing FVG End-to-End Pipeline\n";

    // 1. Load config from CSV
    patterns::FairValueGapDetector detector;
    detector.load_config_from_csv("ai_results/fvg_analysis_real.csv");

    // 2. Feed OHLC bars
    auto bars = create_bullish_fvg_scenario();
    for (const auto& bar : bars) {
        detector.add_ohlc(bar);
    }

    // 3. Update price (touch gap)
    detector.update_price(create_tick(1.0874, 1.0876));

    // 4. Detect pattern
    auto result = detector.detect();

    // 5. Validate signal
    ASSERT_TRUE(result.is_valid());
    ASSERT_TRUE(result.is_tradeable());
    ASSERT_EQUAL(static_cast<int>(result.signal), static_cast<int>(patterns::Signal::BUY));

    std::cout << "  ✓ FVG E2E: Config loaded → Gap detected → Signal generated\n";
}

void test_order_flow_end_to_end() {
    std::cout << "Testing Order Flow End-to-End Pipeline\n";

    // 1. Load config from CSV
    patterns::OrderFlowAnalyzer analyzer;
    analyzer.load_config_from_csv("ai_results/order_flow_analysis_mock.csv");

    // 2. Feed ticks
    auto ticks = create_buy_imbalance_scenario(70, 30);
    for (const auto& tick : ticks) {
        analyzer.add_tick(tick);
    }

    // 3. Detect pattern
    auto result = analyzer.detect();

    // 4. Validate signal
    ASSERT_TRUE(result.is_valid());
    ASSERT_TRUE(result.is_tradeable());
    ASSERT_EQUAL(static_cast<int>(result.signal), static_cast<int>(patterns::Signal::BUY));

    std::cout << "  ✓ Order Flow E2E: Config loaded → Imbalance calculated → Signal generated\n";
}

void run_all_pattern_integration_tests() {
    std::cout << "\n=== Pattern Integration Tests (E2E) ===\n\n";

    run_test("Integration: FVG E2E", test_fvg_end_to_end);
    run_test("Integration: Order Flow E2E", test_order_flow_end_to_end);

    std::cout << "\n=== Integration Tests Complete ===\n";
}

} // namespace gel::test
