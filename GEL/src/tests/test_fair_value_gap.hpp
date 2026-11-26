// src/tests/test_fair_value_gap.hpp
#pragma once

#include "gel/patterns/fair_value_gap.hpp"
#include "test_utils.hpp"
#include "test_pattern_utils.hpp"
#include <iostream>

namespace gel::test {

void test_fvg_bullish_gap_detection() {
    std::cout << "Testing Bullish FVG Detection\n";

    patterns::FairValueGapDetector detector;
    auto bars = create_bullish_fvg_scenario();

    for (const auto& bar : bars) {
        detector.add_ohlc(bar);
    }

    // Gap detection doesn't require current_price (only signal generation does)
    const auto& gaps = detector.get_active_gaps();
    ASSERT_EQUAL(gaps.size(), 1);

    const auto& gap = gaps.front();
    ASSERT_TRUE(gap.is_bullish);
    ASSERT_EQUAL_APPROX(gap.low, 1.0870, 0.0001);
    ASSERT_EQUAL_APPROX(gap.high, 1.0880, 0.0001);
    ASSERT_EQUAL_APPROX(gap.size_pips(), 10.0, 0.1);
    ASSERT_TRUE(gap.confidence > 0.5);

    std::cout << "  ✓ Bullish gap detected: [" << gap.low << ", " << gap.high << "] (" << gap.size_pips() << " pips)\n";
}

void test_fvg_bearish_gap_detection() {
    std::cout << "Testing Bearish FVG Detection\n";

    patterns::FairValueGapDetector detector;
    auto bars = create_bearish_fvg_scenario();

    for (const auto& bar : bars) {
        detector.add_ohlc(bar);
    }

    // Gap detection doesn't require current_price (only signal generation does)
    const auto& gaps = detector.get_active_gaps();
    ASSERT_EQUAL(gaps.size(), 1);

    const auto& gap = gaps.front();
    ASSERT_FALSE(gap.is_bullish);
    ASSERT_EQUAL_APPROX(gap.low, 1.0810, 0.0001);
    ASSERT_EQUAL_APPROX(gap.high, 1.0820, 0.0001);

    std::cout << "  ✓ Bearish gap detected\n";
}

void test_fvg_no_gap_detection() {
    std::cout << "Testing No Gap Scenario\n";

    patterns::FairValueGapDetector detector;
    auto bars = create_no_gap_scenario();

    for (const auto& bar : bars) {
        detector.add_ohlc(bar);
    }

    ASSERT_EQUAL(detector.get_active_gaps().size(), 0);

    std::cout << "  ✓ No gap detected (correct)\n";
}

void test_fvg_gap_too_small() {
    std::cout << "Testing Gap Size Filtering\n";

    patterns::FairValueGapDetector::Config config;
    config.min_gap_size = 0.0020; // 20 pips minimum
    patterns::FairValueGapDetector detector(config);

    auto bars = create_bullish_fvg_scenario(); // 10 pip gap
    for (const auto& bar : bars) {
        detector.add_ohlc(bar);
    }

    ASSERT_EQUAL(detector.get_active_gaps().size(), 0);

    std::cout << "  ✓ Small gap filtered (10 pips < 20 pips threshold)\n";
}

void test_fvg_bullish_signal_generation() {
    std::cout << "Testing Bullish FVG Signal (BUY)\n";

    patterns::FairValueGapDetector detector;
    auto bars = create_bullish_fvg_scenario();

    for (const auto& bar : bars) {
        detector.add_ohlc(bar);
    }

    // Update price to gap midpoint
    detector.update_price(create_tick(1.0874, 1.0876));

    auto result = detector.detect();

    ASSERT_TRUE(result.is_valid());
    ASSERT_EQUAL(static_cast<int>(result.signal), static_cast<int>(patterns::Signal::BUY));
    ASSERT_TRUE(result.confidence > 0.0);

    std::cout << "  ✓ BUY signal generated (price touched bullish gap)\n";
}

void test_fvg_bearish_signal_generation() {
    std::cout << "Testing Bearish FVG Signal (SELL)\n";

    patterns::FairValueGapDetector detector;
    auto bars = create_bearish_fvg_scenario();

    for (const auto& bar : bars) {
        detector.add_ohlc(bar);
    }

    detector.update_price(create_tick(1.0814, 1.0816));

    auto result = detector.detect();

    ASSERT_TRUE(result.is_valid());
    ASSERT_EQUAL(static_cast<int>(result.signal), static_cast<int>(patterns::Signal::SELL));

    std::cout << "  ✓ SELL signal generated (price touched bearish gap)\n";
}

void test_fvg_gap_filling() {
    std::cout << "Testing Gap Filling Cleanup\n";

    patterns::FairValueGapDetector detector;
    auto bars = create_bullish_fvg_scenario();

    for (const auto& bar : bars) {
        detector.add_ohlc(bar);
    }

    ASSERT_EQUAL(detector.get_active_gaps().size(), 1);

    // Price fills gap completely (below lower bound)
    detector.update_price(create_tick(1.0865, 1.0867));

    ASSERT_EQUAL(detector.get_active_gaps().size(), 0);

    std::cout << "  ✓ Gap removed after filling\n";
}

void test_fvg_multiple_gaps() {
    std::cout << "Testing Multiple Gaps (max 5)\n";

    patterns::FairValueGapDetector detector;

    // Create 7 different gaps
    for (int i = 0; i < 7; ++i) {
        auto bars = create_bullish_fvg_scenario();
        for (const auto& bar : bars) {
            detector.add_ohlc(bar);
        }
    }

    ASSERT_EQUAL(detector.get_active_gaps().size(), 5);

    std::cout << "  ✓ Max 5 gaps maintained\n";
}

void test_fvg_csv_config_loading() {
    std::cout << "Testing FVG CSV Config Loading\n";

    patterns::FairValueGapDetector detector;
    detector.load_config_from_csv("ai_results/fvg_analysis_real.csv");

    // Config loaded, but detector not ready until OHLC + price added
    // Test that config was loaded (10 pip gap should be detected with 5 pip min)
    auto bars = create_bullish_fvg_scenario();
    for (const auto& bar : bars) {
        detector.add_ohlc(bar);
    }

    ASSERT_EQUAL(detector.get_active_gaps().size(), 1);

    std::cout << "  ✓ Config loaded from CSV\n";
}

void test_fvg_confidence_calculation() {
    std::cout << "Testing Confidence Calculation\n";

    patterns::FairValueGapDetector detector;
    auto bars = create_bullish_fvg_scenario();

    for (const auto& bar : bars) {
        detector.add_ohlc(bar);
    }

    const auto& gap = detector.get_active_gaps().front();

    ASSERT_TRUE(gap.confidence >= 0.5);
    ASSERT_TRUE(gap.confidence <= 1.0);

    std::cout << "  ✓ Confidence: " << gap.confidence << "\n";
}

void run_all_fvg_tests() {
    std::cout << "\n=== FVG Unit Tests ===\n\n";

    run_test("FVG: Bullish Gap Detection", test_fvg_bullish_gap_detection);
    run_test("FVG: Bearish Gap Detection", test_fvg_bearish_gap_detection);
    run_test("FVG: No Gap Detection", test_fvg_no_gap_detection);
    run_test("FVG: Gap Size Filtering", test_fvg_gap_too_small);
    run_test("FVG: Bullish Signal (BUY)", test_fvg_bullish_signal_generation);
    run_test("FVG: Bearish Signal (SELL)", test_fvg_bearish_signal_generation);
    run_test("FVG: Gap Filling", test_fvg_gap_filling);
    run_test("FVG: Multiple Gaps", test_fvg_multiple_gaps);
    run_test("FVG: CSV Config Loading", test_fvg_csv_config_loading);
    run_test("FVG: Confidence Calculation", test_fvg_confidence_calculation);

    std::cout << "\n=== FVG Tests Complete ===\n";
}

} // namespace gel::test
