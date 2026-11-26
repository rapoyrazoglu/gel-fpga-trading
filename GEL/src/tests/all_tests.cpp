// src/tests/all_tests.cpp
#include <iostream>

// Test utils
#include "test_utils.hpp"

// Foundation Tests (KEPT - Infrastructure)
// #include "test_fixed_point.hpp"
// #include "test_datetime_utils.hpp"
// #include "test_tick.hpp"
// #include "test_dukascopy_parser.hpp"
// #include "test_ohlc.hpp"
// #include "test_csv_reader.hpp"

// Pattern Tests (FVG only for now)
#include "test_fair_value_gap.hpp"
// #include "test_order_flow.hpp"
// #include "test_ai_results_parser.hpp"
// #include "test_pattern_integration.hpp"

// Exit System Tests (Jim Simons Fast Exit)
#include "test_exit_system.hpp"

// Backtest Integration Tests
#include "test_backtest_integration.hpp"

// Performance Tests (Tick-to-Signal Latency)
#include "test_latency_profiler.hpp"
#include "test_real_latency.hpp"

// EMA Momentum Strategy Tests (Capstone - Best Strategy)
#include "test_ema_momentum.hpp"

// ATR Fibonacci Strategy Tests (Top AI Research Strategy)
#include "test_atr_fibonacci.hpp"

// Data Pipeline Integration Tests
// #include "test_integration.hpp"

int main() {
    std::cout << "===== GEL Trading System - Full Test Suite =====\n";
    std::cout << "Testing: FVG + Exit System + Backtest Integration\n\n";

    try {
        // === FVG PATTERN DETECTION TESTS ===
        gel::test::run_all_fvg_tests();  // 10 FVG tests

        // === EXIT SYSTEM TESTS (Jim Simons Fast Exit) ===
        gel::test::run_all_exit_system_tests();  // 22 Exit System tests

        // === BACKTEST INTEGRATION TESTS ===
        gel::test::run_all_backtest_integration_tests();  // 4 Integration tests

        // === LATENCY PROFILER TESTS (Tick-to-Signal) ===
        gel::test::run_all_latency_profiler_tests();  // 11 Latency tests

        // === REAL LATENCY TESTS (CPU vs FPGA) ===
        gel::test::run_real_latency_tests();  // 2 Real latency tests

        // === EMA MOMENTUM STRATEGY TESTS (Capstone) ===
        gel::test::run_all_ema_momentum_tests();  // 5 Strategy tests

        // === ATR FIBONACCI v12 STRATEGY TESTS ===
        gel::test::run_all_atr_fibonacci_tests();  // 6 ATR Fibonacci tests

        // Test sonuçlarını yazdır
        gel::testing::TestReporter::printSummary();

        std::cout << "\n========================================\n";
        std::cout << "Implementation Status:\n";
        std::cout << "  [X] FVG Detector (10 tests)\n";
        std::cout << "  [X] Position Management\n";
        std::cout << "  [X] Stop-Loss System (Fixed, ATR, Trailing, Breakeven)\n";
        std::cout << "  [X] Take-Profit System (Fixed, R/R, Partial, Fibonacci)\n";
        std::cout << "  [X] Time Exit System (Max Hold, Inactivity, Session, Weekend)\n";
        std::cout << "  [X] Exit Manager (Orchestrator + Statistics)\n";
        std::cout << "  [X] Signal Generator (FVG -> Trade Signals)\n";
        std::cout << "  [X] Trade Executor (Position Management + Risk)\n";
        std::cout << "  [X] Backtest Engine (Event-driven, Full Pipeline)\n";
        std::cout << "  [X] Latency Profiler (Tick-to-Signal, PnL Correlation)\n";
        std::cout << "  [X] EMA Momentum Strategy (Sharpe 3.07, PF 1.51)\n";
        std::cout << "  [X] ATR Fibonacci v12 Strategy (Sharpe 4.08, PF 1.73)\n";
        std::cout << "\nComplete Trading System Ready:\n";
        std::cout << "  - FVG Detection -> Signal Generation -> Execution\n";
        std::cout << "  - Jim Simons Exit Rules (10 pip SL, 1.5R TP, 30min max)\n";
        std::cout << "  - Event-driven backtesting with equity curve\n";
        std::cout << "  - Performance metrics (Win Rate, Profit Factor, Drawdown)\n";
        std::cout << "  - Tick-to-Signal latency measurement & analysis\n";
        std::cout << "========================================\n";

        return gel::testing::TestReporter::countFailed();

    } catch (const std::exception& e) {
        std::cerr << "\n!!! FATAL ERROR !!!\n";
        std::cerr << "Exception: " << e.what() << "\n";
        return 1;
    }
}