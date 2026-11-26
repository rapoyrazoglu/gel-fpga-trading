#pragma once

#include "pattern_detector.hpp"
#include "gel/data/ohlc.hpp"
#include "gel/data/tick.hpp"
#include <deque>
#include <optional>
#include <chrono>

namespace gel::patterns {

using timestamp = std::chrono::system_clock::time_point;

/**
 * @brief Fair Value Gap (FVG) Pattern Detector
 *
 * **What is FVG?**
 * Fair Value Gap is a 3-candle pattern indicating liquidity imbalance:
 * - Bullish FVG: Gap between candle[0].high and candle[2].low (gap UP)
 * - Bearish FVG: Gap between candle[0].low and candle[2].high (gap DOWN)
 *
 * **Trading Logic:**
 * - FVG acts as a "magnet" - price tends to return and fill the gap (mean reversion)
 * - When FVG detected: Wait for price to touch gap → Enter trade
 * - Exit when gap is filled or time-based exit
 *
 * **Example (Bullish FVG):**
 * ```
 * Candle[0]: OHLC(1.0800, 1.0850, 1.0795, 1.0820)  <-- Low: 1.0795
 * Candle[1]: OHLC(1.0820, 1.0900, 1.0815, 1.0895)  <-- Big move up (creates gap)
 * Candle[2]: OHLC(1.0895, 1.0920, 1.0890, 1.0910)  <-- High: 1.0890
 *
 * Gap: 1.0890 - 1.0850 = 0.0040 (40 pips gap)
 * → Expect price to drop back to fill gap [1.0850 - 1.0890]
 * → SELL signal when price reaches 1.0890
 * ```
 *
 * **AI Integration:**
 * AI team analyzes historical FVG patterns and provides:
 * - Optimal min_gap_size (e.g., 5 pips = 0.0005 for EUR/USD)
 * - Average fill time (e.g., 45 minutes)
 * - Win rate (e.g., 78%)
 * - Average profit per trade (e.g., 12 pips)
 * - Sharpe estimate
 *
 * **FPGA Optimization (Week 3-4):**
 * - 3-candle sliding window (circular buffer)
 * - Parallel gap calculation (upper/lower bands)
 * - Low-latency signal generation (<100ns)
 */
class FairValueGapDetector : public IPatternDetector {
public:
    /**
     * @brief FVG gap structure
     */
    struct Gap {
        double low{0.0};          ///< Gap lower bound
        double high{0.0};         ///< Gap upper bound
        timestamp time_detected;  ///< When gap was detected
        bool is_bullish{false};   ///< Bullish (expect price drop) or Bearish (expect price rise)
        double confidence{0.0};   ///< Confidence score (based on gap size + volatility)

        /**
         * @brief Gap size in pips (for EUR/USD: 1 pip = 0.0001)
         */
        [[nodiscard]] double size_pips() const {
            return (high - low) / 0.0001;
        }

        /**
         * @brief Is price inside gap?
         */
        [[nodiscard]] bool contains(double price) const {
            return price >= low && price <= high;
        }

        /**
         * @brief Is gap filled by current price?
         */
        [[nodiscard]] bool is_filled(double price) const {
            if (is_bullish) {
                return price <= low; // Price dropped to lower bound
            } else {
                return price >= high; // Price rose to upper bound
            }
        }
    };

    /**
     * @brief AI-provided configuration (from CSV analysis)
     */
    struct Config {
        double min_gap_size{0.0005};       ///< Min gap size in price units (5 pips default)
        double avg_fill_time_minutes{0.3}; ///< Average time for gap to fill (AI: 0.3 min median)
        double win_rate{0.1899};           ///< Historical win rate (AI: 18.99% on 201 patterns)
        double avg_profit_pips{7.52};      ///< Average profit per trade (AI: 7.52 pips)
        double confidence_threshold{0.50}; ///< Min confidence to generate signal (lowered from 0.70)

        /// GCC 14 compatibility: explicit constructor makes this non-aggregate
        Config() : min_gap_size{0.0005}, avg_fill_time_minutes{0.3}, win_rate{0.1899},
                   avg_profit_pips{7.52}, confidence_threshold{0.50} {}
    };

private:
    Config config_;

    // OHLC buffer (3-candle sliding window for FVG detection)
    std::deque<data::OHLCBar> ohlc_buffer_;
    static constexpr size_t WINDOW_SIZE = 3;

    // Detected gaps (not yet filled)
    std::deque<Gap> active_gaps_;
    static constexpr size_t MAX_ACTIVE_GAPS = 5; // Track up to 5 unfilled gaps

    // Current price (latest tick)
    std::optional<double> current_price_;

    // Detector state
    bool is_initialized_{false};

public:
    /**
     * @brief Constructor with default configuration
     */
    explicit FairValueGapDetector(const Config& config = Config{})
        : config_(config) {}

    /**
     * @brief Load configuration from AI results CSV
     * @param csv_path Path to AI analysis CSV (e.g., "ai_results/fvg_analysis.csv")
     *
     * Expected CSV format:
     * ```
     * min_gap_size,avg_fill_time_minutes,win_rate,avg_profit_pips,sharpe_estimate
     * 0.0005,45.2,0.78,12.3,2.34
     * ```
     */
    void load_config_from_csv(const std::string& csv_path);

    /**
     * @brief Add OHLC bar to sliding window
     * @param ohlc New OHLC bar
     */
    void add_ohlc(const data::OHLCBar& ohlc);

    /**
     * @brief Update current price (from tick)
     * @param tick Latest tick
     */
    void update_price(const data::Tick& tick);

    /**
     * @brief Detect FVG pattern in latest 3 candles
     * @return Optional gap (nullopt if no valid gap found)
     */
    [[nodiscard]] std::optional<Gap> detect_gap() const;

    /**
     * @brief Check if any active gap is ready to trade
     * @return Pattern result with signal and confidence
     */
    PatternResult detect() override;

    /**
     * @brief Get detector name
     */
    [[nodiscard]] std::string name() const override {
        return "Fair Value Gap (FVG)";
    }

    /**
     * @brief Get detector description
     */
    [[nodiscard]] std::string description() const override {
        return "3-candle liquidity gap pattern - Mean reversion opportunity";
    }

    /**
     * @brief Is detector ready?
     */
    [[nodiscard]] bool is_ready() const override {
        return is_initialized_ && ohlc_buffer_.size() >= WINDOW_SIZE && current_price_.has_value();
    }

    /**
     * @brief Reset detector state
     */
    void reset() override {
        ohlc_buffer_.clear();
        active_gaps_.clear();
        current_price_.reset();
        is_initialized_ = false;
    }

    /**
     * @brief Get active gaps (for debugging/visualization)
     */
    [[nodiscard]] const std::deque<Gap>& get_active_gaps() const {
        return active_gaps_;
    }

    /**
     * @brief Get confidence threshold
     */
    [[nodiscard]] double get_confidence_threshold() const override {
        return config_.confidence_threshold;
    }

    /**
     * @brief Get win rate threshold
     */
    [[nodiscard]] double get_win_rate_threshold() const override {
        return config_.win_rate;
    }

private:
    /**
     * @brief Calculate confidence score for a gap
     * @param gap Gap to score
     * @return Confidence (0.0 to 1.0)
     *
     * Factors:
     * - Gap size (larger = higher confidence)
     * - Gap relative to recent volatility (larger relative gap = higher confidence)
     * - Distance to gap (closer = higher confidence)
     */
    [[nodiscard]] double calculate_confidence(const Gap& gap) const;

    /**
     * @brief Remove filled gaps from active list
     */
    void cleanup_filled_gaps();
};

} // namespace gel::patterns
