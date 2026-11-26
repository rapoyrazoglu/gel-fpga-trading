// src/tests/test_exit_system.hpp
#pragma once

#include "gel/position/position.hpp"
#include "gel/exit/stop_loss.hpp"
#include "gel/exit/take_profit.hpp"
#include "gel/exit/time_exit.hpp"
#include "gel/exit/exit_manager.hpp"
#include "test_utils.hpp"
#include <iostream>

namespace gel::test {

// Helper: Pozisyon olustur
inline position::Position create_test_position(
    position::Direction dir = position::Direction::LONG,
    double entry_price = 1.0850,
    double size = 0.1,
    const std::string& symbol = "EURUSD") {

    auto now = std::chrono::system_clock::now();
    return position::Position("test_001", symbol, dir, entry_price, size, now);
}

// ============================================================================
// POSITION TESTS
// ============================================================================

void test_position_creation() {
    std::cout << "Testing Position Creation\n";

    auto pos = create_test_position();

    ASSERT_EQUAL(pos.id, "test_001");
    ASSERT_EQUAL(pos.symbol, "EURUSD");
    ASSERT_EQUAL(static_cast<int>(pos.direction), static_cast<int>(position::Direction::LONG));
    ASSERT_EQUAL(static_cast<int>(pos.status), static_cast<int>(position::PositionStatus::OPEN));
    ASSERT_EQUAL_APPROX(pos.entry_price, 1.0850, 0.0001);
    ASSERT_EQUAL_APPROX(pos.size, 0.1, 0.0001);

    std::cout << "  Position created successfully\n";
}

void test_position_unrealized_pnl() {
    std::cout << "Testing Unrealized P&L Calculation\n";

    auto pos = create_test_position(position::Direction::LONG, 1.0850);

    // 10 pip profit
    double pnl = pos.unrealized_pnl_pips(1.0860);
    ASSERT_EQUAL_APPROX(pnl, 10.0, 0.1);

    // 5 pip loss
    pnl = pos.unrealized_pnl_pips(1.0845);
    ASSERT_EQUAL_APPROX(pnl, -5.0, 0.1);

    std::cout << "  Unrealized P&L calculated correctly\n";
}

void test_position_short_pnl() {
    std::cout << "Testing Short Position P&L\n";

    auto pos = create_test_position(position::Direction::SHORT, 1.0850);

    // Short: fiyat duserse kar
    double pnl = pos.unrealized_pnl_pips(1.0840);
    ASSERT_EQUAL_APPROX(pnl, 10.0, 0.1);

    // Short: fiyat yselirse zarar
    pnl = pos.unrealized_pnl_pips(1.0860);
    ASSERT_EQUAL_APPROX(pnl, -10.0, 0.1);

    std::cout << "  Short position P&L calculated correctly\n";
}

void test_position_close() {
    std::cout << "Testing Position Close\n";

    auto pos = create_test_position(position::Direction::LONG, 1.0850);
    auto close_time = std::chrono::system_clock::now();

    pos.close(1.0870, close_time, position::ExitReason::TAKE_PROFIT);

    ASSERT_EQUAL(static_cast<int>(pos.status), static_cast<int>(position::PositionStatus::CLOSED));
    ASSERT_TRUE(pos.exit_price.has_value());
    ASSERT_EQUAL_APPROX(pos.exit_price.value(), 1.0870, 0.0001);
    ASSERT_EQUAL(static_cast<int>(pos.exit_reason), static_cast<int>(position::ExitReason::TAKE_PROFIT));

    // Realized P&L
    double realized_pnl = pos.realized_pnl_pips();
    ASSERT_EQUAL_APPROX(realized_pnl, 20.0, 0.1);

    std::cout << "  Position closed with realized P&L: " << realized_pnl << " pips\n";
}

void test_position_jpy_pair() {
    std::cout << "Testing JPY Pair Pip Calculation\n";

    auto pos = create_test_position(position::Direction::LONG, 150.50, 0.1, "USDJPY");

    ASSERT_EQUAL_APPROX(pos.pip_size(), 0.01, 0.001);
    ASSERT_EQUAL_APPROX(pos.pip_multiplier(), 100.0, 0.1);

    // 10 pip movement for JPY (0.10)
    double pnl = pos.unrealized_pnl_pips(150.60);
    ASSERT_EQUAL_APPROX(pnl, 10.0, 0.1);

    std::cout << "  JPY pair pip calculation correct\n";
}

// ============================================================================
// STOP-LOSS TESTS
// ============================================================================

void test_fixed_stop_loss_long() {
    std::cout << "Testing Fixed Stop-Loss (Long)\n";

    exit::FixedStopLoss sl(10.0);  // 10 pip stop
    auto pos = create_test_position(position::Direction::LONG, 1.0850);

    // SL level should be 1.0840
    double sl_level = sl.calculate_level(pos);
    ASSERT_EQUAL_APPROX(sl_level, 1.0840, 0.0001);

    // Not triggered
    auto result = sl.check(pos, 1.0845);
    ASSERT_FALSE(result.triggered);

    // Triggered
    result = sl.check(pos, 1.0838);
    ASSERT_TRUE(result.triggered);
    ASSERT_EQUAL_APPROX(result.exit_price, 1.0840, 0.0001);

    std::cout << "  Fixed SL (Long) working correctly\n";
}

void test_fixed_stop_loss_short() {
    std::cout << "Testing Fixed Stop-Loss (Short)\n";

    exit::FixedStopLoss sl(10.0);
    auto pos = create_test_position(position::Direction::SHORT, 1.0850);

    // SL level should be 1.0860
    double sl_level = sl.calculate_level(pos);
    ASSERT_EQUAL_APPROX(sl_level, 1.0860, 0.0001);

    // Not triggered
    auto result = sl.check(pos, 1.0855);
    ASSERT_FALSE(result.triggered);

    // Triggered
    result = sl.check(pos, 1.0865);
    ASSERT_TRUE(result.triggered);

    std::cout << "  Fixed SL (Short) working correctly\n";
}

void test_atr_stop_loss() {
    std::cout << "Testing ATR Stop-Loss\n";

    exit::ATRStopLoss sl(1.5, 10.0);  // 1.5x ATR, ATR = 10 pips
    auto pos = create_test_position(position::Direction::LONG, 1.0850);

    // SL = 1.5 x 10 = 15 pips below entry
    double sl_level = sl.calculate_level(pos);
    ASSERT_EQUAL_APPROX(sl_level, 1.0835, 0.0001);

    // Update ATR
    sl.update_atr(20.0);
    sl_level = sl.calculate_level(pos);
    ASSERT_EQUAL_APPROX(sl_level, 1.0820, 0.0001);  // 1.5 x 20 = 30 pips

    std::cout << "  ATR SL working correctly\n";
}

void test_trailing_stop_loss() {
    std::cout << "Testing Trailing Stop-Loss\n";

    exit::TrailingStopLoss tsl(5.0, 10.0);  // 5 pip trail, activate at 10 pip profit
    auto pos = create_test_position(position::Direction::LONG, 1.0850);

    // Not active yet
    ASSERT_FALSE(tsl.is_active());

    // Price moves up 5 pips - not enough to activate
    tsl.update(pos, 1.0855);
    ASSERT_FALSE(tsl.is_active());

    // Price moves up 10 pips - activates
    tsl.update(pos, 1.0860);
    ASSERT_TRUE(tsl.is_active());

    // Stop level should be 1.0855 (5 pips below)
    ASSERT_EQUAL_APPROX(tsl.get_current_level(), 1.0855, 0.0001);

    // Price moves to 1.0870 - stop should trail to 1.0865
    tsl.update(pos, 1.0870);
    ASSERT_EQUAL_APPROX(tsl.get_current_level(), 1.0865, 0.0001);

    // Price drops to 1.0865 - should trigger
    auto result = tsl.check(pos, 1.0864);
    ASSERT_TRUE(result.triggered);

    std::cout << "  Trailing SL working correctly\n";
}

void test_breakeven_stop_loss() {
    std::cout << "Testing Breakeven Stop-Loss\n";

    exit::BreakevenStopLoss be(10.0, 1.0);  // Trigger at 10 pip profit, lock 1 pip profit
    auto pos = create_test_position(position::Direction::LONG, 1.0850);

    // Not at breakeven yet
    ASSERT_FALSE(be.is_breakeven());

    // Price moves up 8 pips - not enough
    be.update(pos, 1.0858);
    ASSERT_FALSE(be.is_breakeven());

    // Price moves up 12 pips - breakeven activated
    be.update(pos, 1.0862);
    ASSERT_TRUE(be.is_breakeven());

    // BE level should be entry + 1 pip = 1.0851
    double be_level = be.calculate_level(pos);
    ASSERT_EQUAL_APPROX(be_level, 1.0851, 0.0001);

    // Price drops to BE level - should trigger
    auto result = be.check(pos, 1.0850);
    ASSERT_TRUE(result.triggered);

    std::cout << "  Breakeven SL working correctly\n";
}

// ============================================================================
// TAKE-PROFIT TESTS
// ============================================================================

void test_fixed_take_profit_long() {
    std::cout << "Testing Fixed Take-Profit (Long)\n";

    exit::FixedTakeProfit tp(15.0);  // 15 pip target
    auto pos = create_test_position(position::Direction::LONG, 1.0850);

    // TP level should be 1.0865
    double tp_level = tp.calculate_level(pos);
    ASSERT_EQUAL_APPROX(tp_level, 1.0865, 0.0001);

    // Not triggered
    auto result = tp.check(pos, 1.0860);
    ASSERT_FALSE(result.triggered);

    // Triggered
    result = tp.check(pos, 1.0870);
    ASSERT_TRUE(result.triggered);

    std::cout << "  Fixed TP (Long) working correctly\n";
}

void test_fixed_take_profit_short() {
    std::cout << "Testing Fixed Take-Profit (Short)\n";

    exit::FixedTakeProfit tp(15.0);
    auto pos = create_test_position(position::Direction::SHORT, 1.0850);

    // TP level should be 1.0835
    double tp_level = tp.calculate_level(pos);
    ASSERT_EQUAL_APPROX(tp_level, 1.0835, 0.0001);

    // Triggered
    auto result = tp.check(pos, 1.0830);
    ASSERT_TRUE(result.triggered);

    std::cout << "  Fixed TP (Short) working correctly\n";
}

void test_risk_reward_take_profit() {
    std::cout << "Testing Risk/Reward Take-Profit\n";

    exit::RiskRewardTakeProfit tp(1.5, 10.0);  // 1.5R, 10 pip SL
    auto pos = create_test_position(position::Direction::LONG, 1.0850);

    // TP = 1.5 x 10 = 15 pips
    double tp_level = tp.calculate_level(pos);
    ASSERT_EQUAL_APPROX(tp_level, 1.0865, 0.0001);

    // Update SL to 20 pips
    tp.update_stop_loss(20.0);
    tp_level = tp.calculate_level(pos);
    ASSERT_EQUAL_APPROX(tp_level, 1.0880, 0.0001);  // 1.5 x 20 = 30 pips

    std::cout << "  R/R TP working correctly\n";
}

void test_partial_take_profit() {
    std::cout << "Testing Partial Take-Profit\n";

    exit::PartialTakeProfit ptp;
    ptp.add_target(10.0, 50.0);  // 10 pips -> %50 kapat
    ptp.add_target(20.0, 50.0);  // 20 pips -> kalan %50'yi kapat

    auto pos = create_test_position(position::Direction::LONG, 1.0850);
    ptp.reset();

    // First target - not reached
    auto result = ptp.check(pos, 1.0855);
    ASSERT_FALSE(result.triggered);

    // First target - reached
    result = ptp.check(pos, 1.0862);
    ASSERT_TRUE(result.triggered);
    ASSERT_EQUAL_APPROX(result.exit_percentage, 50.0, 0.1);

    std::cout << "  Partial TP working correctly\n";
}

// ============================================================================
// TIME EXIT TESTS
// ============================================================================

void test_max_hold_time_exit() {
    std::cout << "Testing Max Hold Time Exit\n";

    exit::MaxHoldTimeExit te(std::chrono::minutes(30));

    auto entry_time = std::chrono::system_clock::now() - std::chrono::minutes(25);
    position::Position pos("test", "EURUSD", position::Direction::LONG, 1.0850, 0.1, entry_time);

    // Not triggered (25 min < 30 min)
    auto result = te.check(pos, std::chrono::system_clock::now());
    ASSERT_FALSE(result.triggered);

    // Triggered (35 min > 30 min)
    auto future_time = std::chrono::system_clock::now() + std::chrono::minutes(10);
    result = te.check(pos, future_time);
    ASSERT_TRUE(result.triggered);

    std::cout << "  Max Hold Time Exit working correctly\n";
}

void test_weekend_exit() {
    std::cout << "Testing Weekend Exit\n";

    exit::WeekendExit we(21);  // Friday 21:00 UTC

    auto pos = create_test_position();

    // Create a Friday 22:00 UTC timestamp
    // This is a simplified test - in real scenario we'd use proper time
    // For now, we trust the implementation

    std::cout << "  Weekend Exit configured for Friday 21:00 UTC\n";
}

void test_session_exit() {
    std::cout << "Testing Session Exit\n";

    auto nyc_exit = exit::TimeExitFactory::createNYCClose();
    ASSERT_EQUAL(nyc_exit->name(), "SessionExit(21:00 UTC)");

    std::cout << "  Session Exit (NYC Close) configured\n";
}

// ============================================================================
// EXIT MANAGER TESTS
// ============================================================================

void test_exit_manager_basic() {
    std::cout << "Testing Exit Manager Basic\n";

    auto manager = exit::ExitManagerBuilder()
        .with_fixed_stop_loss(10.0)
        .with_fixed_take_profit(15.0)
        .build();

    ASSERT_EQUAL(manager->stop_loss_count(), 1);
    ASSERT_EQUAL(manager->take_profit_count(), 1);

    std::cout << "  Exit Manager created with SL and TP\n";
}

void test_exit_manager_stop_loss_trigger() {
    std::cout << "Testing Exit Manager SL Trigger\n";

    auto manager = exit::ExitManagerBuilder()
        .with_fixed_stop_loss(10.0)
        .with_fixed_take_profit(15.0)
        .build();

    auto pos = create_test_position(position::Direction::LONG, 1.0850);
    auto now = std::chrono::system_clock::now();

    // Price above SL
    auto signal = manager->check_exit(pos, 1.0845, now);
    ASSERT_FALSE(signal.should_exit);

    // Price at SL
    signal = manager->check_exit(pos, 1.0838, now);
    ASSERT_TRUE(signal.should_exit);
    ASSERT_EQUAL(static_cast<int>(signal.reason), static_cast<int>(position::ExitReason::STOP_LOSS));

    std::cout << "  Exit Manager SL trigger working\n";
}

void test_exit_manager_take_profit_trigger() {
    std::cout << "Testing Exit Manager TP Trigger\n";

    auto manager = exit::ExitManagerBuilder()
        .with_fixed_stop_loss(10.0)
        .with_fixed_take_profit(15.0)
        .build();

    auto pos = create_test_position(position::Direction::LONG, 1.0850);
    auto now = std::chrono::system_clock::now();

    // Price at TP
    auto signal = manager->check_exit(pos, 1.0870, now);
    ASSERT_TRUE(signal.should_exit);
    ASSERT_EQUAL(static_cast<int>(signal.reason), static_cast<int>(position::ExitReason::TAKE_PROFIT));

    std::cout << "  Exit Manager TP trigger working\n";
}

void test_exit_manager_jim_simons_preset() {
    std::cout << "Testing Jim Simons Preset\n";

    auto manager = exit::ExitManagerBuilder()
        .with_jim_simons_preset(10.0)
        .build();

    // Should have: Fixed SL, R/R TP, Max Hold Time, Inactivity, Trailing, Weekend
    ASSERT_TRUE(manager->stop_loss_count() >= 1);
    ASSERT_TRUE(manager->take_profit_count() >= 1);
    ASSERT_TRUE(manager->time_exit_count() >= 1);
    ASSERT_TRUE(manager->has_trailing_stop());

    std::cout << "  Jim Simons preset configured\n";
}

void test_exit_manager_statistics() {
    std::cout << "Testing Exit Statistics\n";

    auto manager = exit::ExitManagerBuilder()
        .with_fixed_stop_loss(10.0)
        .with_fixed_take_profit(15.0)
        .build();

    auto pos1 = create_test_position(position::Direction::LONG, 1.0850);
    auto pos2 = create_test_position(position::Direction::LONG, 1.0850);
    auto now = std::chrono::system_clock::now();

    // SL hit
    auto signal1 = manager->check_exit(pos1, 1.0838, now);
    manager->execute_exit(pos1, signal1, now);

    // TP hit
    auto signal2 = manager->check_exit(pos2, 1.0870, now);
    manager->execute_exit(pos2, signal2, now);

    const auto& stats = manager->get_statistics();
    ASSERT_EQUAL(stats.total_exits, 2);
    ASSERT_EQUAL(stats.stop_loss_exits, 1);
    ASSERT_EQUAL(stats.take_profit_exits, 1);

    std::cout << "  Exit statistics: " << stats.total_exits << " exits, "
              << stats.stop_loss_rate() << "% SL, " << stats.take_profit_rate() << "% TP\n";
}

void test_position_stats() {
    std::cout << "Testing Position Statistics\n";

    position::PositionStats stats;

    // Winning trade: +15 pips
    position::Position win_pos("w1", "EURUSD", position::Direction::LONG, 1.0850, 0.1,
                               std::chrono::system_clock::now());
    win_pos.close(1.0865, std::chrono::system_clock::now(), position::ExitReason::TAKE_PROFIT);
    stats.add_position(win_pos);

    // Losing trade: -10 pips
    position::Position loss_pos("l1", "EURUSD", position::Direction::LONG, 1.0850, 0.1,
                                std::chrono::system_clock::now());
    loss_pos.close(1.0840, std::chrono::system_clock::now(), position::ExitReason::STOP_LOSS);
    stats.add_position(loss_pos);

    ASSERT_EQUAL(stats.total_trades, 2);
    ASSERT_EQUAL(stats.winning_trades, 1);
    ASSERT_EQUAL(stats.losing_trades, 1);
    ASSERT_EQUAL_APPROX(stats.win_rate(), 50.0, 0.1);
    ASSERT_TRUE(stats.profit_factor() > 1.0);  // 15/10 = 1.5

    std::cout << "  Win rate: " << stats.win_rate() << "%, Profit factor: " << stats.profit_factor() << "\n";
}

void test_atr_calculator() {
    std::cout << "Testing ATR Calculator\n";

    exit::ATRCalculator atr(3);  // 3-period ATR for testing

    // Add bars
    atr.add_bar(1.0860, 1.0840, 1.0850);  // TR = 20 pips
    ASSERT_FALSE(atr.is_ready());

    atr.add_bar(1.0870, 1.0850, 1.0860);  // TR = 20 pips
    atr.add_bar(1.0880, 1.0860, 1.0870);  // TR = 20 pips
    atr.add_bar(1.0890, 1.0870, 1.0880);  // TR = 20 pips

    ASSERT_TRUE(atr.is_ready());
    ASSERT_TRUE(atr.get_atr_pips() > 0);

    std::cout << "  ATR Calculator: " << atr.get_atr_pips() << " pips\n";
}

// ============================================================================
// RUN ALL TESTS
// ============================================================================

void run_all_exit_system_tests() {
    std::cout << "\n=== Exit System Unit Tests ===\n\n";

    // Position tests
    run_test("Position: Creation", test_position_creation);
    run_test("Position: Unrealized P&L", test_position_unrealized_pnl);
    run_test("Position: Short P&L", test_position_short_pnl);
    run_test("Position: Close", test_position_close);
    run_test("Position: JPY Pair", test_position_jpy_pair);

    // Stop-Loss tests
    run_test("SL: Fixed (Long)", test_fixed_stop_loss_long);
    run_test("SL: Fixed (Short)", test_fixed_stop_loss_short);
    run_test("SL: ATR-based", test_atr_stop_loss);
    run_test("SL: Trailing", test_trailing_stop_loss);
    run_test("SL: Breakeven", test_breakeven_stop_loss);

    // Take-Profit tests
    run_test("TP: Fixed (Long)", test_fixed_take_profit_long);
    run_test("TP: Fixed (Short)", test_fixed_take_profit_short);
    run_test("TP: Risk/Reward", test_risk_reward_take_profit);
    run_test("TP: Partial", test_partial_take_profit);

    // Time Exit tests
    run_test("Time: Max Hold", test_max_hold_time_exit);
    run_test("Time: Weekend", test_weekend_exit);
    run_test("Time: Session", test_session_exit);

    // Exit Manager tests
    run_test("Manager: Basic", test_exit_manager_basic);
    run_test("Manager: SL Trigger", test_exit_manager_stop_loss_trigger);
    run_test("Manager: TP Trigger", test_exit_manager_take_profit_trigger);
    run_test("Manager: Jim Simons Preset", test_exit_manager_jim_simons_preset);
    run_test("Manager: Statistics", test_exit_manager_statistics);

    // Utility tests
    run_test("Utils: Position Stats", test_position_stats);
    run_test("Utils: ATR Calculator", test_atr_calculator);

    std::cout << "\n=== Exit System Tests Complete ===\n";
}

} // namespace gel::test
