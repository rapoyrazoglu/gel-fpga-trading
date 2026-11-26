#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace gel::analysis {

/**
 * @brief Parser for AI team's analysis results
 *
 * **Purpose:**
 * AI team analyzes 8.5M EUR/USD ticks (2024-2025) and provides:
 * 1. FVG pattern analysis (CSV)
 * 2. Order Flow imbalance analysis (CSV)
 * 3. Price sequence clustering (JSON)
 *
 * This parser loads their results into C++ structures for use by
 * pattern detectors and backtesting engine.
 *
 * **Integration Timeline:**
 * - Tuesday-Wednesday: Implement parser (this class)
 * - Thursday evening: AI team delivers results
 * - Friday: Load results, configure detectors, run backtest
 */
class AIResultsParser {
public:
    /**
     * @brief Fair Value Gap analysis results
     */
    struct FVGResult {
        double min_gap_size{0.0};          ///< Optimal min gap size (e.g., 0.0005 = 5 pips)
        double avg_fill_time_minutes{0.0}; ///< Average time for gap to fill
        double win_rate{0.0};              ///< Win rate (e.g., 0.78 = 78%)
        double avg_profit_pips{0.0};       ///< Average profit per trade
        double sharpe_estimate{0.0};       ///< Estimated Sharpe ratio
        int sample_count{0};               ///< Number of FVG patterns found in data

        /**
         * @brief Is this result valid and tradeable?
         */
        [[nodiscard]] bool is_tradeable(double min_sharpe = 2.0, double min_win_rate = 0.70) const {
            return sharpe_estimate >= min_sharpe &&
                   win_rate >= min_win_rate &&
                   sample_count >= 100; // Min 100 samples for statistical significance
        }
    };

    /**
     * @brief Order Flow analysis results
     */
    struct OrderFlowResult {
        int optimal_window_seconds{60};    ///< Optimal rolling window size
        double buy_threshold{1.75};        ///< Optimal buy signal threshold
        double sell_threshold{0.57};       ///< Optimal sell signal threshold
        double win_rate{0.0};              ///< Win rate
        double avg_profit_pips{0.0};       ///< Average profit per trade
        double sharpe_estimate{0.0};       ///< Estimated Sharpe ratio
        int sample_count{0};               ///< Number of imbalance events found

        [[nodiscard]] bool is_tradeable(double min_sharpe = 2.0, double min_win_rate = 0.70) const {
            return sharpe_estimate >= min_sharpe &&
                   win_rate >= min_win_rate &&
                   sample_count >= 100;
        }
    };

    /**
     * @brief Price sequence cluster
     */
    struct SequenceCluster {
        int id{0};                         ///< Cluster ID
        std::vector<double> pattern;       ///< Price delta pattern
        double win_rate{0.0};              ///< Win rate for this cluster
        double avg_profit_pips{0.0};       ///< Average profit per trade
        double sharpe{0.0};                ///< Sharpe ratio
        std::string signal;                ///< Expected signal ("BUY", "SELL", "NONE")
        int occurrence_count{0};           ///< How many times this pattern appeared

        [[nodiscard]] bool is_tradeable(double min_sharpe = 2.0, double min_win_rate = 0.70) const {
            return sharpe >= min_sharpe &&
                   win_rate >= min_win_rate &&
                   occurrence_count >= 50 &&
                   signal != "NONE";
        }
    };

    /**
     * @brief Sequence clustering analysis results
     */
    struct SequenceResult {
        int sequence_length{20};           ///< Length of price delta sequences
        double max_distance{0.001};        ///< Max distance for valid match
        std::vector<SequenceCluster> clusters; ///< All clusters (sorted by Sharpe desc)

        /**
         * @brief Get top N clusters by Sharpe ratio
         */
        [[nodiscard]] std::vector<SequenceCluster> get_top_clusters(size_t n, double min_sharpe = 2.0) const {
            std::vector<SequenceCluster> top;
            for (const auto& cluster : clusters) {
                if (cluster.is_tradeable(min_sharpe, 0.70) && top.size() < n) {
                    top.push_back(cluster);
                }
            }
            return top;
        }
    };

public:
    /**
     * @brief Parse FVG analysis CSV
     * @param csv_path Path to CSV file (e.g., "ai_results/fvg_analysis.csv")
     * @return FVG analysis results
     *
     * Expected CSV format:
     * ```
     * min_gap_size,avg_fill_time_minutes,win_rate,avg_profit_pips,sharpe_estimate,sample_count
     * 0.0005,45.2,0.78,12.3,2.34,456
     * ```
     */
    static FVGResult parse_fvg_csv(const std::string& csv_path) {
        std::ifstream file(csv_path);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open FVG CSV: " + csv_path);
        }

        FVGResult result;

        // Skip header line
        std::string header;
        std::getline(file, header);

        // Read data line
        std::string line;
        if (std::getline(file, line)) {
            std::istringstream iss(line);
            std::string token;

            // Parse: min_gap_size,avg_fill_time_minutes,win_rate,avg_profit_pips,sharpe_estimate,sample_count
            std::getline(iss, token, ',');
            result.min_gap_size = std::stod(token);

            std::getline(iss, token, ',');
            result.avg_fill_time_minutes = std::stod(token);

            std::getline(iss, token, ',');
            result.win_rate = std::stod(token);

            std::getline(iss, token, ',');
            result.avg_profit_pips = std::stod(token);

            std::getline(iss, token, ',');
            result.sharpe_estimate = std::stod(token);

            std::getline(iss, token, ',');
            result.sample_count = std::stoi(token);
        } else {
            throw std::runtime_error("FVG CSV has no data rows");
        }

        file.close();
        return result;
    }

    /**
     * @brief Parse Order Flow analysis CSV
     * @param csv_path Path to CSV file (e.g., "ai_results/order_flow_analysis.csv")
     * @return Order Flow analysis results
     *
     * Expected CSV format:
     * ```
     * window_seconds,buy_threshold,sell_threshold,win_rate,avg_profit_pips,sharpe_estimate,sample_count
     * 60,1.75,0.57,0.72,8.4,2.12,312
     * ```
     */
    static OrderFlowResult parse_order_flow_csv(const std::string& csv_path) {
        std::ifstream file(csv_path);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open Order Flow CSV: " + csv_path);
        }

        OrderFlowResult result;

        // Skip header line
        std::string header;
        std::getline(file, header);

        // Read data line
        std::string line;
        if (std::getline(file, line)) {
            std::istringstream iss(line);
            std::string token;

            // Parse: window_seconds,buy_threshold,sell_threshold,win_rate,avg_profit_pips,sharpe_estimate,sample_count
            std::getline(iss, token, ',');
            result.optimal_window_seconds = std::stoi(token);

            std::getline(iss, token, ',');
            result.buy_threshold = std::stod(token);

            std::getline(iss, token, ',');
            result.sell_threshold = std::stod(token);

            std::getline(iss, token, ',');
            result.win_rate = std::stod(token);

            std::getline(iss, token, ',');
            result.avg_profit_pips = std::stod(token);

            std::getline(iss, token, ',');
            result.sharpe_estimate = std::stod(token);

            std::getline(iss, token, ',');
            result.sample_count = std::stoi(token);
        } else {
            throw std::runtime_error("Order Flow CSV has no data rows");
        }

        file.close();
        return result;
    }

    /**
     * @brief Parse sequence clustering JSON
     * @param json_path Path to JSON file (e.g., "ai_results/sequence_clusters.json")
     * @return Sequence clustering results
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
     *       "signal": "BUY",
     *       "occurrence_count": 156
     *     },
     *     ...
     *   ]
     * }
     * ```
     *
     * Note: This is a simplified parser. For production, use a JSON library
     * like nlohmann/json or RapidJSON.
     */
    static SequenceResult parse_sequence_json(const std::string& json_path) {
        // TODO: Implement JSON parsing
        // For now, throw - will implement when AI team provides sample JSON
        throw std::runtime_error("JSON parsing not yet implemented - waiting for AI team sample data");

        // Pseudo-code for when we implement:
        // 1. Read JSON file
        // 2. Parse sequence_length, max_distance
        // 3. Parse clusters array
        // 4. For each cluster: parse id, pattern, win_rate, etc.
        // 5. Sort clusters by sharpe descending
        // 6. Return SequenceResult
    }

    /**
     * @brief Validate AI results meet minimum trading requirements
     * @param fvg FVG results
     * @param order_flow Order Flow results
     * @param min_sharpe Minimum Sharpe ratio (default 2.0)
     * @return Best pattern name and Sharpe
     *
     * Returns the pattern with highest Sharpe that meets requirements.
     * Throws if no pattern meets minimum requirements.
     */
    static std::pair<std::string, double> validate_and_select_best(
        const FVGResult& fvg,
        const OrderFlowResult& order_flow,
        double min_sharpe = 2.0
    ) {
        std::string best_pattern;
        double best_sharpe = 0.0;

        // Check FVG
        if (fvg.is_tradeable(min_sharpe, 0.70)) {
            if (fvg.sharpe_estimate > best_sharpe) {
                best_pattern = "FVG";
                best_sharpe = fvg.sharpe_estimate;
            }
        }

        // Check Order Flow
        if (order_flow.is_tradeable(min_sharpe, 0.70)) {
            if (order_flow.sharpe_estimate > best_sharpe) {
                best_pattern = "OrderFlow";
                best_sharpe = order_flow.sharpe_estimate;
            }
        }

        // Check if we found anything
        if (best_sharpe < min_sharpe) {
            throw std::runtime_error(
                "No pattern meets minimum requirements (Sharpe >=" + std::to_string(min_sharpe) + ")\n" +
                "FVG Sharpe: " + std::to_string(fvg.sharpe_estimate) + "\n" +
                "OrderFlow Sharpe: " + std::to_string(order_flow.sharpe_estimate)
            );
        }

        return {best_pattern, best_sharpe};
    }

    /**
     * @brief Print summary of AI results (for logging/debugging)
     */
    static std::string summary(const FVGResult& fvg, const OrderFlowResult& order_flow) {
        std::ostringstream oss;
        oss << "=== AI Analysis Results ===\n\n";

        oss << "Fair Value Gap (FVG):\n";
        oss << "  Min gap size:  " << fvg.min_gap_size << " (" << (fvg.min_gap_size / 0.0001) << " pips)\n";
        oss << "  Win rate:      " << (fvg.win_rate * 100.0) << "%\n";
        oss << "  Avg profit:    " << fvg.avg_profit_pips << " pips\n";
        oss << "  Sharpe:        " << fvg.sharpe_estimate << "\n";
        oss << "  Samples:       " << fvg.sample_count << "\n";
        oss << "  Tradeable:     " << (fvg.is_tradeable() ? "YES" : "NO") << "\n\n";

        oss << "Order Flow Imbalance:\n";
        oss << "  Window:        " << order_flow.optimal_window_seconds << " seconds\n";
        oss << "  Buy threshold: " << order_flow.buy_threshold << "\n";
        oss << "  Win rate:      " << (order_flow.win_rate * 100.0) << "%\n";
        oss << "  Avg profit:    " << order_flow.avg_profit_pips << " pips\n";
        oss << "  Sharpe:        " << order_flow.sharpe_estimate << "\n";
        oss << "  Samples:       " << order_flow.sample_count << "\n";
        oss << "  Tradeable:     " << (order_flow.is_tradeable() ? "YES" : "NO") << "\n";

        return oss.str();
    }
};

} // namespace gel::analysis
