// include/gel/backtest/backtest_engine.hpp
#pragma once

#include "gel/data/ohlc.hpp"
#include "gel/patterns/fair_value_gap.hpp"
#include "gel/signals/signal_generator.hpp"
#include "gel/execution/trade_executor.hpp"
#include "gel/exit/exit_manager.hpp"
#include <vector>
#include <memory>
#include <string>
#include <sstream>
#include <iostream>
#include <algorithm>

namespace gel::backtest {

    using OHLC = gel::data::OHLCBar;
    using SignalGenerator = gel::signals::SignalGenerator;
    using TradeExecutor = gel::execution::TradeExecutor;
    using ExitManager = gel::exit::ExitManager;
    using Position = gel::position::Position;

    struct BacktestConfig {
        // Data settings
        std::string symbol = "EURUSD";
        double pip_size = 0.0001;

        // Initial capital
        double initial_balance = 10000.0;

        // Strategy settings (delegates to sub-components)
        gel::signals::SignalConfig signal_config;
        gel::execution::ExecutionConfig execution_config;

        // Backtest settings
        bool verbose = false;              // Print trade details
        bool track_equity_curve = true;    // Track balance over time
    };

    struct BacktestResult {
        // Trade statistics
        size_t total_trades = 0;
        size_t winning_trades = 0;
        size_t losing_trades = 0;
        double win_rate = 0.0;

        // P&L
        double total_pnl = 0.0;
        double total_pnl_pips = 0.0;
        double average_win = 0.0;
        double average_loss = 0.0;
        double largest_win = 0.0;
        double largest_loss = 0.0;

        // Risk metrics
        double profit_factor = 0.0;
        double expectancy = 0.0;
        double max_drawdown = 0.0;
        double max_drawdown_pct = 0.0;

        // Account
        double final_balance = 0.0;
        double total_return_pct = 0.0;

        // Equity curve
        std::vector<double> equity_curve;
        std::vector<std::chrono::system_clock::time_point> equity_timestamps;

        // All trades
        std::vector<Position> all_trades;

        // Summary
        std::string to_string() const {
            std::ostringstream oss;
            oss << "=== BACKTEST RESULTS ===\n";
            oss << "Total Trades: " << total_trades << "\n";
            oss << "Win Rate: " << (win_rate * 100.0) << "%\n";
            oss << "Total P&L: $" << total_pnl << " (" << total_pnl_pips << " pips)\n";
            oss << "Profit Factor: " << profit_factor << "\n";
            oss << "Expectancy: $" << expectancy << "\n";
            oss << "Max Drawdown: " << (max_drawdown_pct * 100.0) << "%\n";
            oss << "Final Balance: $" << final_balance << "\n";
            oss << "Total Return: " << (total_return_pct * 100.0) << "%\n";
            return oss.str();
        }
    };

    /**
     * BacktestEngine - Event-driven backtesting system
     *
     * Workflow:
     * 1. Load OHLC data
     * 2. For each bar:
     *    a. Update positions (check exits)
     *    b. Detect FVG patterns
     *    c. Generate signals
     *    d. Execute signals (open new positions)
     *    e. Track equity curve
     * 3. Calculate performance metrics
     */
    class BacktestEngine {
    public:
        explicit BacktestEngine(BacktestConfig config = BacktestConfig{})
            : config_(config)
        {
            // Initialize components
            signal_generator_ = std::make_shared<SignalGenerator>(config.signal_config);

            // Create exit manager
            auto exit_mgr_unique = gel::exit::ExitManagerBuilder()
                .with_jim_simons_preset(config.signal_config.stop_loss_pips)
                .build();
            auto exit_mgr = std::shared_ptr<gel::exit::ExitManager>(std::move(exit_mgr_unique));

            // Create executor with exit manager
            config.execution_config.account_balance = config.initial_balance;
            trade_executor_ = std::make_shared<TradeExecutor>(config.execution_config, exit_mgr);
        }

        /**
         * Run backtest on OHLC data
         * @param ohlc_data Vector of OHLC bars
         * @return Backtest results
         */
        BacktestResult run(const std::vector<OHLC>& ohlc_data) {
            if (ohlc_data.empty()) {
                throw std::runtime_error("No OHLC data provided");
            }

            reset();

            if (config_.verbose) {
                std::cout << "Starting backtest with " << ohlc_data.size() << " bars\n";
                std::cout << "Symbol: " << config_.symbol << "\n";
                std::cout << "Initial Balance: $" << config_.initial_balance << "\n\n";
            }

            // Event-driven loop
            for (size_t i = 0; i < ohlc_data.size(); ++i) {
                const auto& bar = ohlc_data[i];

                // 1. Update existing positions (check exits)
                auto closed = trade_executor_->update_positions(bar);
                if (config_.verbose && !closed.empty()) {
                    for (const auto& pos : closed) {
                        print_trade_summary(pos);
                    }
                }

                // 2. Detect FVG patterns (TODO: implement proper FVG detection)
                std::vector<gel::patterns::FairValueGapDetector::Gap> fvgs; // Empty for now

                // 3. Generate signals
                auto signals = signal_generator_->process(bar, fvgs);

                // 4. Execute signals
                for (const auto& signal : signals) {
                    auto result = trade_executor_->execute_signal(signal, bar);
                    if (config_.verbose && result.success) {
                        std::cout << "  [OPEN] " << signal.reason
                                  << " @ " << bar.close
                                  << " (confidence: " << signal.confidence << ")\n";
                    }
                }

                // 5. Track equity
                if (config_.track_equity_curve) {
                    equity_curve_.push_back(trade_executor_->get_account_balance());
                    equity_timestamps_.push_back(bar.timestamp);
                }

                // Track peak for drawdown
                if (equity_curve_.back() > peak_equity_) {
                    peak_equity_ = equity_curve_.back();
                }
            }

            // Close all remaining positions
            if (!ohlc_data.empty()) {
                auto remaining = trade_executor_->close_all_positions(ohlc_data.back());
                if (config_.verbose && !remaining.empty()) {
                    std::cout << "\nClosing " << remaining.size() << " remaining positions...\n";
                }
            }

            // Calculate results
            return calculate_results();
        }

    private:
        BacktestConfig config_;

        // Components
        std::shared_ptr<SignalGenerator> signal_generator_;
        std::shared_ptr<TradeExecutor> trade_executor_;

        // Tracking
        std::vector<double> equity_curve_;
        std::vector<std::chrono::system_clock::time_point> equity_timestamps_;
        double peak_equity_ = 0.0;

        void reset() {
            equity_curve_.clear();
            equity_timestamps_.clear();
            peak_equity_ = config_.initial_balance;
        }

        BacktestResult calculate_results() {
            BacktestResult result;

            // Get all trades
            result.all_trades = trade_executor_->get_closed_positions();
            result.total_trades = result.all_trades.size();

            if (result.total_trades == 0) {
                result.final_balance = config_.initial_balance;
                return result;
            }

            // Calculate trade statistics
            double total_wins = 0.0;
            double total_losses = 0.0;

            for (const auto& trade : result.all_trades) {
                double pnl = trade.realized_pnl_dollars();
                double pnl_pips = trade.realized_pnl_pips();

                result.total_pnl += pnl;
                result.total_pnl_pips += pnl_pips;

                if (pnl > 0) {
                    result.winning_trades++;
                    total_wins += pnl;
                    result.largest_win = std::max(result.largest_win, pnl);
                } else {
                    result.losing_trades++;
                    total_losses += std::abs(pnl);
                    result.largest_loss = std::min(result.largest_loss, pnl);
                }
            }

            // Ratios
            result.win_rate = static_cast<double>(result.winning_trades) / result.total_trades;
            result.average_win = result.winning_trades > 0 ? total_wins / result.winning_trades : 0.0;
            result.average_loss = result.losing_trades > 0 ? total_losses / result.losing_trades : 0.0;
            result.profit_factor = total_losses > 0 ? total_wins / total_losses : 0.0;
            result.expectancy = (result.win_rate * result.average_win) - ((1.0 - result.win_rate) * result.average_loss);

            // Account metrics
            result.final_balance = trade_executor_->get_account_balance();
            result.total_return_pct = (result.final_balance - config_.initial_balance) / config_.initial_balance;

            // Drawdown
            result.max_drawdown = calculate_max_drawdown();
            result.max_drawdown_pct = peak_equity_ > 0 ? result.max_drawdown / peak_equity_ : 0.0;

            // Equity curve
            result.equity_curve = equity_curve_;
            result.equity_timestamps = equity_timestamps_;

            return result;
        }

        double calculate_max_drawdown() {
            if (equity_curve_.empty()) return 0.0;

            double max_dd = 0.0;
            double peak = equity_curve_[0];

            for (double equity : equity_curve_) {
                if (equity > peak) {
                    peak = equity;
                }
                double dd = peak - equity;
                max_dd = std::max(max_dd, dd);
            }

            return max_dd;
        }

        void print_trade_summary(const Position& pos) {
            double pnl = pos.realized_pnl_dollars();
            std::string result = pnl > 0 ? "[WIN]" : "[LOSS]";
            std::cout << "  " << result << " "
                      << (pos.is_long() ? "LONG" : "SHORT")
                      << " P&L: $" << pnl
                      << " (" << pos.realized_pnl_pips() << " pips)\n";
        }
    };

} // namespace gel::backtest
