// src/tests/test_pattern_utils.hpp
#pragma once

#include "gel/data/ohlc.hpp"
#include "gel/data/tick.hpp"
#include <chrono>
#include <vector>

namespace gel::test {

// Type alias for convenience
using timestamp = std::chrono::system_clock::time_point;

/**
 * @brief Helper: Create OHLC bar
 */
inline data::OHLCBar create_ohlc(
    double open,
    double high,
    double low,
    double close,
    timestamp time = std::chrono::system_clock::now()
) {
    data::OHLCBar bar;
    bar.open = open;
    bar.high = high;
    bar.low = low;
    bar.close = close;
    bar.timestamp = time;
    bar.volume = 100.0; // Default volume
    return bar;
}

/**
 * @brief Helper: Create tick
 */
inline data::Tick create_tick(
    double bid,
    double ask,
    timestamp time = std::chrono::system_clock::now()
) {
    data::Tick tick;
    tick.bid = bid;
    tick.ask = ask;
    tick.timestamp = time;
    tick.bid_volume = 1.0;
    tick.ask_volume = 1.0;
    return tick;
}

/**
 * @brief Helper: Create BULLISH FVG scenario
 *
 * Bullish FVG: c2.high < c0.low (upward displacement, gap below)
 *
 * Example:
 * - c2 (oldest): High @ 1.0870
 * - c1 (middle): Big upward move
 * - c0 (newest): Low @ 1.0880
 * - Gap: [1.0870, 1.0880] (10 pips)
 *
 * Trading: Price returns to gap → BUY (discount zone)
 */
inline std::vector<data::OHLCBar> create_bullish_fvg_scenario() {
    auto t0 = std::chrono::system_clock::now();
    auto t1 = t0 + std::chrono::minutes(1);
    auto t2 = t1 + std::chrono::minutes(1);

    return {
        // c2 (index 0): Lower candle (1.0845 - 1.0870)
        create_ohlc(1.0850, 1.0870, 1.0845, 1.0860, t0),

        // c1 (index 1): Big upward displacement (1.0860 → 1.0950)
        create_ohlc(1.0860, 1.0955, 1.0855, 1.0950, t1),

        // c0 (index 2): High candle (1.0880 - 1.0965)
        // c0.low = 1.0880 > c2.high = 1.0870 → Gap detected!
        create_ohlc(1.0950, 1.0965, 1.0880, 1.0960, t2)
    };
    // Gap: [1.0870, 1.0880] (10 pips)
    // is_bullish = true
    // Signal when price @ 1.0875: BUY
}

/**
 * @brief Helper: Create BEARISH FVG scenario
 *
 * Bearish FVG: c2.low > c0.high (downward displacement, gap above)
 *
 * Example:
 * - c2 (oldest): Low @ 1.0820
 * - c1 (middle): Big downward move
 * - c0 (newest): High @ 1.0810
 * - Gap: [1.0810, 1.0820] (10 pips)
 *
 * Trading: Price returns to gap → SELL (premium zone)
 */
inline std::vector<data::OHLCBar> create_bearish_fvg_scenario() {
    auto t0 = std::chrono::system_clock::now();
    auto t1 = t0 + std::chrono::minutes(1);
    auto t2 = t1 + std::chrono::minutes(1);

    return {
        // c2 (index 0): High candle (1.0820 - 1.0880)
        create_ohlc(1.0860, 1.0880, 1.0820, 1.0870, t0),

        // c1 (index 1): Big downward displacement (1.0870 → 1.0750)
        create_ohlc(1.0870, 1.0875, 1.0745, 1.0750, t1),

        // c0 (index 2): Low candle (1.0740 - 1.0810)
        // c0.high = 1.0810 < c2.low = 1.0820 → Gap detected!
        create_ohlc(1.0750, 1.0810, 1.0740, 1.0800, t2)
    };
    // Gap: [1.0810, 1.0820] (10 pips)
    // is_bullish = false
    // Signal when price @ 1.0815: SELL
}

/**
 * @brief Helper: Create NO GAP scenario (negative test)
 */
inline std::vector<data::OHLCBar> create_no_gap_scenario() {
    auto t0 = std::chrono::system_clock::now();
    auto t1 = t0 + std::chrono::minutes(1);
    auto t2 = t1 + std::chrono::minutes(1);

    return {
        // Normal overlapping price movement, no gap
        create_ohlc(1.0800, 1.0820, 1.0795, 1.0810, t0),
        create_ohlc(1.0810, 1.0825, 1.0805, 1.0820, t1),
        create_ohlc(1.0820, 1.0835, 1.0815, 1.0830, t2)
    };
    // No gap: candles overlap
}

/**
 * @brief Helper: Create order flow BUY imbalance scenario
 *
 * Generates ticks with strong upward pressure (ratio >1.75)
 *
 * @param buy_count Number of BUY ticks (default 70)
 * @param sell_count Number of SELL ticks (default 30)
 * @return Vector of ticks with BUY imbalance (ratio = 70/30 = 2.33)
 */
inline std::vector<data::Tick> create_buy_imbalance_scenario(
    int buy_count = 70,
    int sell_count = 30
) {
    std::vector<data::Tick> ticks;
    auto base_time = std::chrono::system_clock::now();
    double price = 1.1000;

    // Generate BUY ticks (upticks)
    for (int i = 0; i < buy_count; ++i) {
        price += 0.00002; // Clear uptick (0.2 pips, 2x epsilon threshold)
        ticks.push_back(create_tick(
            price,
            price + 0.0001,
            base_time  // All ticks at same time (no rolling window cleanup)
        ));
    }

    // Generate SELL ticks (downticks)
    for (int i = 0; i < sell_count; ++i) {
        price -= 0.00002; // Clear downtick (0.2 pips, 2x epsilon threshold)
        ticks.push_back(create_tick(
            price,
            price + 0.0001,
            base_time  // All ticks at same time (no rolling window cleanup)
        ));
    }

    return ticks;
    // Ratio: 70/30 = 2.33 > 1.75 → Strong BUY signal
}

/**
 * @brief Helper: Create order flow SELL imbalance scenario
 *
 * Generates ticks with strong downward pressure (ratio <0.57)
 *
 * @param buy_count Number of BUY ticks (default 30)
 * @param sell_count Number of SELL ticks (default 70)
 * @return Vector of ticks with SELL imbalance (ratio = 30/70 = 0.43)
 */
inline std::vector<data::Tick> create_sell_imbalance_scenario(
    int buy_count = 30,
    int sell_count = 70
) {
    return create_buy_imbalance_scenario(buy_count, sell_count);
    // Ratio: 30/70 = 0.43 < 0.57 → Strong SELL signal
}

/**
 * @brief Helper: Create balanced order flow (no signal)
 */
inline std::vector<data::Tick> create_balanced_flow_scenario() {
    return create_buy_imbalance_scenario(50, 50);
    // Ratio: 50/50 = 1.0 → Neutral (no signal)
}

} // namespace gel::test
