#pragma once

#include "pattern_detector.hpp"
#include "gel/data/tick.hpp"
#include <deque>
#include <chrono>

namespace gel::patterns {

using timestamp = std::chrono::system_clock::time_point;

/**
 * @brief Order Flow Imbalance Analyzer
 *
 * **What is Order Flow?**
 * Order flow tracks aggressive buying vs aggressive selling in tick data:
 * - Uptick (price increased): Aggressive BUY (market order hit ask)
 * - Downtick (price decreased): Aggressive SELL (market order hit bid)
 * - No change: Classify based on bid/ask proximity
 *
 * **Imbalance Detection:**
 * Calculate buy/sell ratio over a rolling time window (e.g., 60 seconds):
 * - Imbalance Ratio = buy_count / sell_count
 * - High ratio (>2.0): Strong buying pressure → Expect price to rise → BUY signal
 * - Low ratio (<0.5): Strong selling pressure → Expect price to fall → SELL signal
 *
 * **Example:**
 * ```
 * Last 60 seconds:
 * - Buy ticks: 340
 * - Sell ticks: 180
 * - Ratio: 340/180 = 1.89
 *
 * If AI found optimal_threshold = 1.75:
 * → Ratio 1.89 > 1.75 → Strong buying → BUY signal
 * ```
 *
 * **AI Integration:**
 * AI team analyzes order flow imbalance and provides:
 * - Optimal time window (e.g., 60 seconds)
 * - Optimal imbalance threshold (e.g., 1.75 for BUY, 0.57 for SELL)
 * - Win rate (e.g., 72%)
 * - Average profit per trade (e.g., 8 pips)
 * - Sharpe estimate
 *
 * **FPGA Optimization (Week 3-4):**
 * - Tick classification in hardware (uptick/downtick detection)
 * - Circular buffer for rolling window (O(1) update)
 * - Parallel ratio calculation (buy_count / sell_count)
 * - Ultra-low latency (<50ns tick classification)
 */
class OrderFlowAnalyzer : public IPatternDetector {
public:
    /**
     * @brief Tick classification
     */
    enum class TickType {
        BUY,    ///< Aggressive buy (uptick or at ask)
        SELL,   ///< Aggressive sell (downtick or at bid)
        NEUTRAL ///< No clear direction
    };

    /**
     * @brief Order flow imbalance result
     */
    struct Imbalance {
        int buy_count{0};         ///< Number of buy ticks in window
        int sell_count{0};        ///< Number of sell ticks in window
        double ratio{1.0};        ///< buy_count / sell_count
        double confidence{0.0};   ///< Confidence score (based on sample size + ratio extremity)

        /**
         * @brief Total ticks in window
         */
        [[nodiscard]] int total_count() const {
            return buy_count + sell_count;
        }

        /**
         * @brief Is imbalance strong enough to trade?
         */
        [[nodiscard]] bool is_strong_buy(double threshold) const {
            return ratio > threshold && total_count() >= 50; // Min 50 ticks for statistical significance
        }

        [[nodiscard]] bool is_strong_sell(double threshold) const {
            return ratio < (1.0 / threshold) && total_count() >= 50;
        }
    };

    /**
     * @brief AI-provided configuration (from CSV analysis)
     */
    struct Config {
        std::chrono::seconds window_duration{60};  ///< Rolling window size (60s default)
        double buy_threshold{1.75};                ///< Min ratio for BUY signal (1.75 default)
        double sell_threshold{0.57};               ///< Max ratio for SELL signal (0.57 = 1/1.75)
        double win_rate{0.70};                     ///< Historical win rate
        double avg_profit_pips{8.0};               ///< Average profit per trade
        double confidence_threshold{0.70};         ///< Min confidence to generate signal

        /// GCC 14 compatibility: explicit constructor makes this non-aggregate
        Config() : window_duration{60}, buy_threshold{1.75}, sell_threshold{0.57},
                   win_rate{0.70}, avg_profit_pips{8.0}, confidence_threshold{0.70} {}
    };

private:
    /**
     * @brief Classified tick with timestamp
     */
    struct ClassifiedTick {
        TickType type;
        timestamp time;
        double price;
    };

    Config config_;

    // Tick buffer (rolling window)
    std::deque<ClassifiedTick> tick_buffer_;

    // Previous tick (for uptick/downtick classification)
    std::optional<data::Tick> prev_tick_;

    // Current imbalance
    Imbalance current_imbalance_;

    // Detector state
    bool is_initialized_{false};

public:
    /**
     * @brief Constructor with default configuration
     */
    explicit OrderFlowAnalyzer(const Config& config = Config{})
        : config_(config) {}

    /**
     * @brief Load configuration from AI results CSV
     * @param csv_path Path to AI analysis CSV (e.g., "ai_results/order_flow_analysis.csv")
     *
     * Expected CSV format:
     * ```
     * window_seconds,buy_threshold,sell_threshold,win_rate,avg_profit_pips,sharpe_estimate
     * 60,1.75,0.57,0.72,8.4,2.12
     * ```
     */
    void load_config_from_csv(const std::string& csv_path);

    /**
     * @brief Add new tick and update order flow
     * @param tick New tick
     */
    void add_tick(const data::Tick& tick);

    /**
     * @brief Classify tick as BUY, SELL, or NEUTRAL
     * @param tick Current tick
     * @return Tick classification
     */
    [[nodiscard]] TickType classify_tick(const data::Tick& tick) const;

    /**
     * @brief Calculate current imbalance
     * @return Order flow imbalance over rolling window
     */
    [[nodiscard]] Imbalance calculate_imbalance() const;

    /**
     * @brief Detect order flow imbalance and generate signal
     * @return Pattern result with signal and confidence
     */
    PatternResult detect() override;

    /**
     * @brief Get detector name
     */
    [[nodiscard]] std::string name() const override {
        return "Order Flow Imbalance";
    }

    /**
     * @brief Get detector description
     */
    [[nodiscard]] std::string description() const override {
        return "Buy/Sell pressure analysis - Momentum opportunity";
    }

    /**
     * @brief Is detector ready?
     */
    [[nodiscard]] bool is_ready() const override {
        return is_initialized_ && tick_buffer_.size() >= 50; // Min 50 ticks
    }

    /**
     * @brief Reset detector state
     */
    void reset() override {
        tick_buffer_.clear();
        prev_tick_.reset();
        current_imbalance_ = Imbalance{};
        is_initialized_ = false;
    }

    /**
     * @brief Get current imbalance (for debugging/visualization)
     */
    [[nodiscard]] const Imbalance& get_current_imbalance() const {
        return current_imbalance_;
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
     * @brief Calculate confidence score for imbalance
     * @param imbalance Imbalance to score
     * @return Confidence (0.0 to 1.0)
     *
     * Factors:
     * - Sample size (more ticks = higher confidence)
     * - Ratio extremity (further from 1.0 = higher confidence)
     * - Consistency over time (sustained imbalance = higher confidence)
     */
    [[nodiscard]] double calculate_confidence(const Imbalance& imbalance) const;

    /**
     * @brief Remove old ticks outside rolling window
     * @param current_time Current timestamp
     */
    void cleanup_old_ticks(timestamp current_time);
};

} // namespace gel::patterns
