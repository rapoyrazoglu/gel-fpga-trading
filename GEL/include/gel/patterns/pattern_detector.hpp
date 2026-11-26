#pragma once

#include <string>
#include <vector>

namespace gel::patterns {

/**
 * @brief Trading signal from pattern detection
 */
enum class Signal {
    NONE,   ///< No signal (neutral)
    BUY,    ///< Buy signal (go long)
    SELL,   ///< Sell signal (go short)
    CLOSE   ///< Close position signal
};

/**
 * @brief Pattern detection result
 */
struct PatternResult {
    Signal signal{Signal::NONE};     ///< Trading signal
    double confidence{0.0};           ///< Confidence score (0.0 to 1.0)
    double expected_profit_pips{0.0}; ///< Expected profit in pips (from AI data)
    double win_rate{0.0};             ///< Historical win rate (from AI data)
    std::string pattern_name;         ///< Name of detected pattern
    std::string metadata;             ///< Additional pattern-specific metadata (JSON)

    /**
     * @brief Is this a valid signal?
     * @return true if signal is not NONE and confidence > 0
     */
    [[nodiscard]] bool is_valid() const {
        return signal != Signal::NONE && confidence > 0.0;
    }

    /**
     * @brief Should we trade this signal?
     * @param min_confidence Minimum confidence threshold (default 0.70)
     * @param min_win_rate Minimum win rate threshold (default 0.70)
     * @return true if signal meets quality thresholds
     */
    [[nodiscard]] bool is_tradeable(double min_confidence = 0.70, double min_win_rate = 0.70) const {
        return is_valid() &&
               confidence >= min_confidence &&
               win_rate >= min_win_rate;
    }
};

/**
 * @brief Abstract base class for all pattern detectors
 *
 * Pattern detectors analyze market data (ticks, OHLC, order flow) and
 * generate trading signals based on specific patterns.
 *
 * Design Philosophy:
 * - Jim Simons approach: Pattern recognition, NOT indicators
 * - Each pattern has statistical edge (win_rate >70%, Sharpe >2.0)
 * - AI team provides optimal parameters and expected returns
 *
 * Usage:
 * ```cpp
 * auto fvg_detector = std::make_unique<FairValueGapDetector>();
 * fvg_detector->load_config_from_csv("ai_results/fvg_analysis.csv");
 *
 * // Feed market data
 * for (const auto& tick : ticks) {
 *     fvg_detector->on_tick(tick);
 * }
 *
 * // Check for signals
 * auto result = fvg_detector->detect();
 * if (result.is_tradeable()) {
 *     // Execute trade
 * }
 * ```
 */
class IPatternDetector {
public:
    virtual ~IPatternDetector() = default;

    /**
     * @brief Detect pattern and generate trading signal
     * @return Pattern detection result with signal and confidence
     */
    virtual PatternResult detect() = 0;

    /**
     * @brief Get pattern detector name
     * @return Human-readable name (e.g., "Fair Value Gap", "Order Flow Imbalance")
     */
    [[nodiscard]] virtual std::string name() const = 0;

    /**
     * @brief Get pattern detector description
     * @return Detailed description of pattern and logic
     */
    [[nodiscard]] virtual std::string description() const = 0;

    /**
     * @brief Is detector ready to generate signals?
     * @return true if detector has enough data and is initialized
     */
    [[nodiscard]] virtual bool is_ready() const = 0;

    /**
     * @brief Reset detector state (clear buffers, history)
     */
    virtual void reset() = 0;

    /**
     * @brief Get current confidence threshold
     * @return Minimum confidence for signal generation
     */
    [[nodiscard]] virtual double get_confidence_threshold() const {
        return 0.70; // Default: 70% confidence
    }

    /**
     * @brief Get current win rate threshold
     * @return Minimum win rate for signal generation
     */
    [[nodiscard]] virtual double get_win_rate_threshold() const {
        return 0.70; // Default: 70% win rate
    }
};

} // namespace gel::patterns
