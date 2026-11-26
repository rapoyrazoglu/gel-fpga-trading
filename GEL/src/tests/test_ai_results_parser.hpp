// src/tests/test_ai_results_parser.hpp
#pragma once

#include "gel/analysis/ai_results_parser.hpp"
#include "test_utils.hpp"
#include <iostream>

namespace gel::test {

void test_parse_fvg_csv() {
    std::cout << "Testing FVG CSV Parsing\n";

    auto result = gel::analysis::AIResultsParser::parse_fvg_csv("ai_results/fvg_analysis_mock.csv");

    ASSERT_EQUAL_APPROX(result.min_gap_size, 0.0005, 0.0001);
    ASSERT_EQUAL_APPROX(result.win_rate, 0.78, 0.01);
    ASSERT_EQUAL_APPROX(result.sharpe_estimate, 2.34, 0.01);
    ASSERT_EQUAL(result.sample_count, 456);
    ASSERT_TRUE(result.is_tradeable(2.0, 0.70));

    std::cout << "  ✓ FVG CSV parsed correctly\n";
}

void test_parse_order_flow_csv() {
    std::cout << "Testing Order Flow CSV Parsing\n";

    auto result = gel::analysis::AIResultsParser::parse_order_flow_csv("ai_results/order_flow_analysis_mock.csv");

    ASSERT_EQUAL(result.optimal_window_seconds, 60);
    ASSERT_EQUAL_APPROX(result.buy_threshold, 1.75, 0.01);
    ASSERT_EQUAL_APPROX(result.win_rate, 0.72, 0.01);
    ASSERT_TRUE(result.is_tradeable(2.0, 0.70));

    std::cout << "  ✓ Order Flow CSV parsed correctly\n";
}

void test_validate_and_select_best() {
    std::cout << "Testing Pattern Selection\n";

    auto fvg = gel::analysis::AIResultsParser::parse_fvg_csv("ai_results/fvg_analysis_mock.csv");
    auto flow = gel::analysis::AIResultsParser::parse_order_flow_csv("ai_results/order_flow_analysis_mock.csv");

    auto [best_pattern, best_sharpe] = gel::analysis::AIResultsParser::validate_and_select_best(fvg, flow, 2.0);

    ASSERT_TRUE(best_sharpe >= 2.0);
    ASSERT_TRUE(best_pattern == "FVG" || best_pattern == "OrderFlow");

    std::cout << "  ✓ Best pattern: " << best_pattern << " (Sharpe: " << best_sharpe << ")\n";
}

void run_all_ai_parser_tests() {
    std::cout << "\n=== AI Results Parser Tests ===\n\n";

    run_test("AI Parser: FVG CSV", test_parse_fvg_csv);
    run_test("AI Parser: Order Flow CSV", test_parse_order_flow_csv);
    run_test("AI Parser: Best Pattern Selection", test_validate_and_select_best);

    std::cout << "\n=== AI Parser Tests Complete ===\n";
}

} // namespace gel::test
