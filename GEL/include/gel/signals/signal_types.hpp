// include/gel/signals/signal_types.hpp
#pragma once

#include <chrono>
#include <string>
#include <optional>

namespace gel::signals {

    enum class SignalType {
        BUY,
        SELL,
        CLOSE_LONG,
        CLOSE_SHORT,
        NO_SIGNAL
    };

    enum class SignalSource {
        FVG,              // Fair Value Gap
        ORDER_FLOW,       // Order Flow Analysis
        COMBINED,         // FVG + Order Flow
        TIME_EXIT,        // Time-based exit
        STOP_LOSS,        // Stop loss hit
        TAKE_PROFIT       // Take profit hit
    };

    struct TradingSignal {
        using time_point = std::chrono::system_clock::time_point;

        SignalType type;
        SignalSource source;
        time_point timestamp;

        // Price levels
        double signal_price;           // Price when signal generated
        std::optional<double> entry_price;    // Suggested entry price
        std::optional<double> stop_loss;      // Suggested SL
        std::optional<double> take_profit;    // Suggested TP

        // Signal strength
        double confidence;             // 0.0 - 1.0

        // Metadata
        std::string pattern_id;        // e.g., "FVG_BULLISH_123"
        std::string reason;            // Human-readable reason

        // Helper methods
        [[nodiscard]] bool is_entry_signal() const {
            return type == SignalType::BUY || type == SignalType::SELL;
        }

        [[nodiscard]] bool is_exit_signal() const {
            return type == SignalType::CLOSE_LONG || type == SignalType::CLOSE_SHORT;
        }

        [[nodiscard]] bool is_long() const {
            return type == SignalType::BUY;
        }

        [[nodiscard]] bool is_short() const {
            return type == SignalType::SELL;
        }
    };

    struct SignalConfig {
        // FVG Configuration
        double min_fvg_size_pips = 5.0;
        double max_fvg_size_pips = 50.0;

        // Entry Configuration
        bool wait_for_retest = true;         // Wait for price to return to FVG
        double retest_tolerance_pips = 2.0;  // How close to FVG to trigger
        int max_bars_to_wait = 50;           // Max bars to wait for retest

        // Exit Configuration (delegates to ExitManager)
        double stop_loss_pips = 10.0;
        double take_profit_pips = 15.0;
        int max_hold_bars = 30;

        // Signal Filtering
        double min_confidence = 0.5;         // Minimum confidence to trade
        bool filter_overlapping_fvg = true;  // Skip if multiple FVGs overlap

        // Risk Management
        double pip_size = 0.0001;            // EUR/USD pip size
    };

} // namespace gel::signals
