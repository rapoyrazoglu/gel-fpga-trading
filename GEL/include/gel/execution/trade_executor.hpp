// include/gel/execution/trade_executor.hpp
#pragma once

#include "gel/signals/signal_types.hpp"
#include "gel/position/position.hpp"
#include "gel/exit/exit_manager.hpp"
#include "gel/data/ohlc.hpp"
#include <vector>
#include <unordered_map>
#include <memory>
#include <optional>

namespace gel::execution {

    using Signal = gel::signals::TradingSignal;
    using Position = gel::position::Position;
    using Direction = gel::position::Direction;
    using ExitManager = gel::exit::ExitManager;
    using OHLC = gel::data::OHLCBar;

    struct ExecutionConfig {
        // Position Sizing
        double default_position_size = 1.0;    // Default lot size
        double max_position_size = 10.0;       // Maximum lot size
        bool use_fixed_sizing = true;          // Fixed vs dynamic sizing

        // Slippage Simulation
        double slippage_pips = 0.5;            // Assumed slippage
        double pip_size = 0.0001;              // EUR/USD pip size

        // Risk Management
        double max_risk_per_trade_pct = 2.0;   // Max 2% risk per trade
        double account_balance = 10000.0;       // Starting balance
        int max_concurrent_positions = 3;       // Max open positions

        // Execution Filters
        bool allow_multiple_same_direction = false;  // Multiple longs/shorts
        double min_signal_confidence = 0.5;          // Min confidence to execute
    };

    struct ExecutionResult {
        bool success;
        std::string message;
        std::optional<Position> position;
    };

    /**
     * TradeExecutor - Executes trading signals and manages positions
     *
     * Responsibilities:
     * - Open positions from signals
     * - Close positions (manual or via ExitManager)
     * - Position sizing and risk management
     * - Track account balance and P&L
     */
    class TradeExecutor {
    public:
        using time_point = std::chrono::system_clock::time_point;

        explicit TradeExecutor(
            ExecutionConfig config = ExecutionConfig{},
            std::shared_ptr<ExitManager> exit_manager = nullptr
        )
            : config_(config)
            , exit_manager_(exit_manager ? exit_manager : std::make_shared<ExitManager>())
            , account_balance_(config.account_balance)
        {}

        /**
         * Execute a trading signal
         * @param signal Trading signal to execute
         * @param current_bar Current OHLC bar (for execution price)
         * @return Execution result with position (if opened)
         */
        ExecutionResult execute_signal(const Signal& signal, const OHLC& current_bar) {
            // Validate signal
            if (auto validation = validate_signal(signal); !validation.success) {
                return validation;
            }

            // Execute based on signal type
            if (signal.is_entry_signal()) {
                return open_position(signal, current_bar);
            } else if (signal.is_exit_signal()) {
                return close_position(signal.pattern_id, current_bar, "Signal Exit");
            }

            return {false, "Unknown signal type", std::nullopt};
        }

        /**
         * Update all open positions with current bar
         * Checks for exits via ExitManager
         * @return Vector of closed positions
         */
        std::vector<Position> update_positions(const OHLC& current_bar) {
            std::vector<Position> closed_positions;

            for (auto& [id, pos] : open_positions_) {
                // Check exit conditions
                auto exit_signal = exit_manager_->check_exit(pos, current_bar.close, current_bar.timestamp);

                if (exit_signal.should_exit) {
                    // Close position
                    double exit_price = apply_slippage(current_bar.close, !pos.is_long());
                    pos.close(exit_price, current_bar.timestamp, exit_signal.reason);

                    // Update balance
                    double pnl_dollars = pos.realized_pnl_dollars();
                    account_balance_ += pnl_dollars;
                    total_pnl_ += pnl_dollars;

                    closed_positions.push_back(pos);
                    positions_to_remove_.push_back(id);
                }
            }

            // Remove closed positions
            for (const auto& id : positions_to_remove_) {
                closed_positions_.push_back(open_positions_[id]);
                open_positions_.erase(id);
            }
            positions_to_remove_.clear();

            return closed_positions;
        }

        /**
         * Force close a specific position
         */
        ExecutionResult close_position(const std::string& position_id, const OHLC& current_bar, const std::string& reason) {
            auto it = open_positions_.find(position_id);
            if (it == open_positions_.end()) {
                return {false, "Position not found: " + position_id, std::nullopt};
            }

            Position& pos = it->second;
            double exit_price = apply_slippage(current_bar.close, !pos.is_long());
            pos.close(exit_price, current_bar.timestamp, gel::position::ExitReason::MANUAL);

            // Update balance
            double pnl_dollars = pos.realized_pnl_dollars();
            account_balance_ += pnl_dollars;
            total_pnl_ += pnl_dollars;

            closed_positions_.push_back(pos);
            open_positions_.erase(position_id);

            return {true, "Position closed: " + reason, pos};
        }

        /**
         * Close all open positions
         */
        std::vector<Position> close_all_positions(const OHLC& current_bar) {
            std::vector<Position> closed;
            std::vector<std::string> ids;

            for (const auto& [id, _] : open_positions_) {
                ids.push_back(id);
            }

            for (const auto& id : ids) {
                auto result = close_position(id, current_bar, "Close All");
                if (result.success && result.position) {
                    closed.push_back(*result.position);
                }
            }

            return closed;
        }

        // Getters
        [[nodiscard]] size_t get_open_position_count() const { return open_positions_.size(); }
        [[nodiscard]] const auto& get_open_positions() const { return open_positions_; }
        [[nodiscard]] const auto& get_closed_positions() const { return closed_positions_; }
        [[nodiscard]] double get_account_balance() const { return account_balance_; }
        [[nodiscard]] double get_total_pnl() const { return total_pnl_; }
        [[nodiscard]] size_t get_total_trades() const { return closed_positions_.size(); }

        // Statistics
        [[nodiscard]] gel::position::PositionStats calculate_stats() const {
            return gel::position::PositionStats::calculate(closed_positions_);
        }

    private:
        ExecutionConfig config_;
        std::shared_ptr<ExitManager> exit_manager_;

        // Position tracking
        std::unordered_map<std::string, Position> open_positions_;
        std::vector<Position> closed_positions_;
        std::vector<std::string> positions_to_remove_;

        // Account state
        double account_balance_;
        double total_pnl_ = 0.0;
        size_t position_id_counter_ = 0;

        /**
         * Validate signal before execution
         */
        ExecutionResult validate_signal(const Signal& signal) {
            // Check confidence threshold
            if (signal.confidence < config_.min_signal_confidence) {
                return {false, "Signal confidence too low", std::nullopt};
            }

            // Check max positions
            if (open_positions_.size() >= config_.max_concurrent_positions) {
                return {false, "Max concurrent positions reached", std::nullopt};
            }

            // Check for same direction positions
            if (!config_.allow_multiple_same_direction && signal.is_entry_signal()) {
                for (const auto& [_, pos] : open_positions_) {
                    bool same_direction = (signal.is_long() && pos.is_long()) ||
                                        (signal.is_short() && !pos.is_long());
                    if (same_direction) {
                        return {false, "Already have position in same direction", std::nullopt};
                    }
                }
            }

            return {true, "Signal validated", std::nullopt};
        }

        /**
         * Open a new position from signal
         */
        ExecutionResult open_position(const Signal& signal, const OHLC& current_bar) {
            // Calculate position size
            double position_size = calculate_position_size(signal);

            // Calculate entry price with slippage
            double entry_price = signal.entry_price.value_or(current_bar.close);
            entry_price = apply_slippage(entry_price, signal.is_long());

            // Create position
            Position pos;
            pos.id = generate_position_id();
            pos.direction = signal.is_long() ? Direction::LONG : Direction::SHORT;
            pos.entry_price = entry_price;
            pos.size = position_size;
            pos.entry_time = current_bar.timestamp;
            pos.pip_value = config_.pip_size;

            // Store metadata
            pos.signal_confidence = signal.confidence;
            pos.pattern_id = signal.pattern_id;

            // Add to open positions
            open_positions_[pos.id] = pos;

            // Register with exit manager
            if (signal.stop_loss) {
                // TODO: Configure exit manager for this position
            }

            return {true, "Position opened: " + pos.id, pos};
        }

        /**
         * Calculate position size based on risk
         */
        double calculate_position_size(const Signal& signal) {
            if (config_.use_fixed_sizing) {
                return config_.default_position_size;
            }

            // Risk-based sizing
            if (signal.stop_loss && signal.entry_price) {
                double risk_dollars = account_balance_ * (config_.max_risk_per_trade_pct / 100.0);
                double sl_distance = std::abs(*signal.entry_price - *signal.stop_loss);
                double position_size = risk_dollars / sl_distance;

                // Clamp to max
                return std::min(position_size, config_.max_position_size);
            }

            return config_.default_position_size;
        }

        /**
         * Apply slippage to execution price
         */
        double apply_slippage(double price, bool is_buy) {
            double slippage = config_.slippage_pips * config_.pip_size;
            return is_buy ? (price + slippage) : (price - slippage);
        }

        /**
         * Generate unique position ID
         */
        std::string generate_position_id() {
            return "POS_" + std::to_string(++position_id_counter_);
        }
    };

} // namespace gel::execution
