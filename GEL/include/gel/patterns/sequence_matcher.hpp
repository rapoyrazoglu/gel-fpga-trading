#pragma once

#include "pattern_detector.hpp"
#include "gel/data/tick.hpp"
#include <vector>
#include <deque>
#include <optional>

namespace gel::patterns {

/**
 * @brief Price Sequence Pattern Matcher
 *
 * **What is Sequence Matching?**
 * Machine learning approach to pattern recognition:
 * 1. AI team clusters historical price sequences (e.g., last 20 price changes)
 * 2. Each cluster = recurring pattern with statistical properties (win rate, avg profit)
 * 3. Real-time matching: Find nearest cluster to current sequence
 * 4. If match is strong (high similarity + high win rate) → Generate signal
 *
 * **Example:**
 * AI found Cluster #42 (78% win rate, 15 pips avg profit):
 * - Pattern: [+0.0002, +0.0001, -0.0003, +0.0005, -0.0001, ...]
 * - 20 price deltas representing "small rise → pullback → strong rise"
 * - After this pattern, price rose 78% of the time
 *
 * Current market sequence:
 * - [+0.0003, +0.0001, -0.0002, +0.0004, -0.0001, ...]
 * - Euclidean distance to Cluster #42: 0.00014 (very close!)
 * - → Strong match → BUY signal
 *
 * **AI Integration:**
 * AI team performs clustering analysis (e.g., K-means with K=50) and provides:
 * - Top 10-20 clusters with highest Sharpe ratios
 * - Each cluster: pattern vector, win rate, avg profit, Sharpe
 * - Optimal distance threshold for matching
 * - Results in JSON format
 *
 * **FPGA Optimization (Week 3-4):**
 * - Store cluster centroids in BRAM
 * - Parallel distance calculation (all clusters simultaneously)
 * - Hardware-accelerated nearest neighbor search
 * - Latency: <200ns for 20 clusters
 */
class SequenceMatcher : public IPatternDetector {
public:
    /**
     * @brief Pattern cluster from AI analysis
     */
    struct Cluster {
        int id{0};                              ///< Cluster ID
        std::vector<double> pattern;            ///< Price deltas (e.g., 20 elements)
        double win_rate{0.0};                   ///< Historical win rate
        double avg_profit_pips{0.0};            ///< Average profit per trade
        double sharpe{0.0};                     ///< Sharpe ratio for this cluster
        Signal expected_signal{Signal::NONE};   ///< Expected trading direction (BUY/SELL)

        /**
         * @brief Calculate Euclidean distance to another sequence
         * @param other Other price delta sequence
         * @return Distance (lower = more similar)
         */
        [[nodiscard]] double distance(const std::vector<double>& other) const {
            if (pattern.size() != other.size()) {
                return std::numeric_limits<double>::max(); // Size mismatch
            }

            double sum_sq = 0.0;
            for (size_t i = 0; i < pattern.size(); ++i) {
                double diff = pattern[i] - other[i];
                sum_sq += diff * diff;
            }
            return std::sqrt(sum_sq);
        }
    };

    /**
     * @brief Pattern match result
     */
    struct Match {
        const Cluster* cluster{nullptr};  ///< Matched cluster
        double distance{0.0};             ///< Distance to cluster centroid
        double confidence{0.0};           ///< Confidence score (based on distance + win rate)

        [[nodiscard]] bool is_valid() const {
            return cluster != nullptr && confidence > 0.0;
        }
    };

    /**
     * @brief AI-provided configuration (from JSON analysis)
     */
    struct Config {
        size_t sequence_length{20};         ///< Length of price delta sequence (20 default)
        double max_distance{0.001};         ///< Max distance for valid match (0.001 default)
        double min_win_rate{0.70};          ///< Min win rate to trade cluster
        double min_sharpe{1.5};             ///< Min Sharpe to trade cluster
        double confidence_threshold{0.70};  ///< Min confidence to generate signal
    };

private:
    Config config_;

    // Loaded clusters from AI (sorted by Sharpe ratio descending)
    std::vector<Cluster> clusters_;

    // Price history (for building current sequence)
    std::deque<double> price_history_;

    // Current best match
    std::optional<Match> current_match_;

    // Detector state
    bool is_initialized_{false};

public:
    /**
     * @brief Constructor with default configuration
     */
    explicit SequenceMatcher(const Config& config = Config{})
        : config_(config) {}

    /**
     * @brief Load clusters from AI results JSON
     * @param json_path Path to AI analysis JSON (e.g., "ai_results/sequence_clusters.json")
     *
     * Expected JSON format:
     * ```json
     * {
     *   "sequence_length": 20,
     *   "max_distance": 0.0008,
     *   "clusters": [
     *     {
     *       "id": 1,
     *       "pattern": [0.0002, 0.0001, -0.0003, ...],
     *       "win_rate": 0.78,
     *       "avg_profit_pips": 15.2,
     *       "sharpe": 2.45,
     *       "signal": "BUY"
     *     },
     *     ...
     *   ]
     * }
     * ```
     */
    void load_clusters_from_json(const std::string& json_path);

    /**
     * @brief Add new price point
     * @param price New price
     */
    void add_price(double price);

    /**
     * @brief Add new tick
     * @param tick New tick
     */
    void add_tick(const data::Tick& tick) {
        add_price(tick.ask); // Use mid price: (bid + ask) / 2 or just ask
    }

    /**
     * @brief Build current price delta sequence
     * @return Vector of price deltas (most recent sequence_length deltas)
     */
    [[nodiscard]] std::vector<double> build_current_sequence() const;

    /**
     * @brief Find best matching cluster
     * @param sequence Current price delta sequence
     * @return Best match (or nullopt if no good match)
     */
    [[nodiscard]] std::optional<Match> find_best_match(const std::vector<double>& sequence) const;

    /**
     * @brief Detect pattern match and generate signal
     * @return Pattern result with signal and confidence
     */
    PatternResult detect() override;

    /**
     * @brief Get detector name
     */
    [[nodiscard]] std::string name() const override {
        return "Sequence Matcher (ML Clustering)";
    }

    /**
     * @brief Get detector description
     */
    [[nodiscard]] std::string description() const override {
        return "AI-powered price sequence pattern recognition";
    }

    /**
     * @brief Is detector ready?
     */
    [[nodiscard]] bool is_ready() const override {
        return is_initialized_ &&
               !clusters_.empty() &&
               price_history_.size() >= (config_.sequence_length + 1); // +1 for deltas
    }

    /**
     * @brief Reset detector state
     */
    void reset() override {
        price_history_.clear();
        current_match_.reset();
        is_initialized_ = false;
    }

    /**
     * @brief Get loaded clusters (for debugging/visualization)
     */
    [[nodiscard]] const std::vector<Cluster>& get_clusters() const {
        return clusters_;
    }

    /**
     * @brief Get current best match (for debugging/visualization)
     */
    [[nodiscard]] const std::optional<Match>& get_current_match() const {
        return current_match_;
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
        return config_.min_win_rate;
    }

private:
    /**
     * @brief Calculate confidence score for a match
     * @param match Match to score
     * @return Confidence (0.0 to 1.0)
     *
     * Factors:
     * - Distance (lower = higher confidence)
     * - Cluster win rate (higher = higher confidence)
     * - Cluster Sharpe ratio (higher = higher confidence)
     * - Sample size (more historical occurrences = higher confidence)
     */
    [[nodiscard]] double calculate_confidence(const Match& match) const;
};

} // namespace gel::patterns
