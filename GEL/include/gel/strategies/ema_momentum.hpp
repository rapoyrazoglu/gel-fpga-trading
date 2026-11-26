// include/gel/strategies/ema_momentum.hpp
#pragma once

#include "gel/performance/latency_profiler.hpp"
#include <vector>
#include <chrono>
#include <optional>
#include <string>
#include <cmath>

namespace gel::strategies {

    /**
     * EMA Momentum Enhanced Strategy
     *
     * Capstone Project - Best performing strategy:
     * - Sharpe Ratio: 3.07
     * - Profit Factor: 1.51
     * - Win Rate: 42.3%
     *
     * Logic:
     * - Fast EMA(9) crosses above Slow EMA(50) = BUY
     * - Long only (direction: long_only)
     * - TP: 50 pips, SL: 25 pips
     */

    struct EMAMomentumConfig {
        int fast_period = 9;
        int slow_period = 50;
        double tp_pips = 50.0;
        double sl_pips = 25.0;
        double pip_size = 0.0001;  // EUR/USD
        bool long_only = true;
    };

    struct TradeResult {
        std::string entry_time;
        std::string exit_time;
        std::string side;  // "BUY" or "SELL"
        double entry_price;
        double exit_price;
        double pnl_pips;
        double pnl_dollars;
        std::string exit_reason;  // "TP", "SL", "SIGNAL"
        int64_t tts_nanoseconds;  // Tick-to-Signal latency
    };

    struct BacktestResult {
        int total_trades = 0;
        int winning_trades = 0;
        int losing_trades = 0;
        double win_rate = 0.0;
        double total_pnl_pips = 0.0;
        double total_pnl_dollars = 0.0;
        double profit_factor = 0.0;
        double max_drawdown_pips = 0.0;
        double sharpe_ratio = 0.0;

        // TTS Metrics
        double avg_tts_microseconds = 0.0;
        double p99_tts_microseconds = 0.0;
        double max_tts_microseconds = 0.0;
        int64_t total_ticks_processed = 0;
        double throughput_ticks_per_sec = 0.0;

        std::vector<TradeResult> trades;
    };

    class EMAMomentumStrategy {
    public:
        explicit EMAMomentumStrategy(EMAMomentumConfig config = EMAMomentumConfig{})
            : config_(config)
            , fast_alpha_(2.0 / (config.fast_period + 1))
            , slow_alpha_(2.0 / (config.slow_period + 1))
        {}

        /**
         * Process a single tick and generate signal
         * Returns: 1 = BUY, -1 = SELL, 0 = NO_SIGNAL
         */
        int process_tick(double bid, double ask, const std::string& timestamp = "") {
            profiler_.start_tick(timestamp);

            double mid = (bid + ask) / 2.0;
            tick_count_++;

            // Update EMAs
            if (!initialized_) {
                fast_ema_ = mid;
                slow_ema_ = mid;
                initialized_ = true;
                profiler_.mark_indicator_done();
                profiler_.mark_signal_done(false);
                profiler_.end_tick(0.0);
                return 0;
            }

            double prev_fast = fast_ema_;
            double prev_slow = slow_ema_;

            fast_ema_ = fast_alpha_ * mid + (1.0 - fast_alpha_) * fast_ema_;
            slow_ema_ = slow_alpha_ * mid + (1.0 - slow_alpha_) * slow_ema_;

            profiler_.mark_indicator_done();

            // Signal generation
            int signal = 0;

            // Crossover detection
            bool crossover_up = (prev_fast <= prev_slow) && (fast_ema_ > slow_ema_);
            bool crossover_down = (prev_fast >= prev_slow) && (fast_ema_ < slow_ema_);

            if (crossover_up) {
                signal = 1;  // BUY
            } else if (crossover_down && !config_.long_only) {
                signal = -1;  // SELL (if not long_only)
            }

            profiler_.mark_signal_done(signal != 0);
            profiler_.end_tick(0.0);

            last_signal_ = signal;
            return signal;
        }

        /**
         * Run full backtest on price data
         */
        BacktestResult backtest(const std::vector<double>& bids,
                                const std::vector<double>& asks,
                                const std::vector<std::string>& timestamps = {}) {

            reset();
            BacktestResult result;

            auto start_time = std::chrono::high_resolution_clock::now();

            // Position tracking
            bool in_position = false;
            double entry_price = 0.0;
            std::string entry_time;
            int entry_idx = 0;

            // For Sharpe calculation
            std::vector<double> returns;
            double peak_equity = 0.0;
            double equity = 0.0;
            double gross_profit = 0.0;
            double gross_loss = 0.0;

            for (size_t i = 0; i < bids.size(); ++i) {
                double bid = bids[i];
                double ask = asks[i];
                std::string ts = timestamps.empty() ? std::to_string(i) : timestamps[i];

                // Get signal
                int signal = process_tick(bid, ask, ts);

                // Manage position
                if (in_position) {
                    double current_pnl_pips = (bid - entry_price) / config_.pip_size;

                    // Check TP
                    if (current_pnl_pips >= config_.tp_pips) {
                        TradeResult trade;
                        trade.entry_time = entry_time;
                        trade.exit_time = ts;
                        trade.side = "BUY";
                        trade.entry_price = entry_price;
                        trade.exit_price = bid;
                        trade.pnl_pips = current_pnl_pips;
                        trade.pnl_dollars = current_pnl_pips * 10.0;  // 1 mini lot
                        trade.exit_reason = "TP";
                        trade.tts_nanoseconds = get_last_tts_ns();

                        result.trades.push_back(trade);
                        equity += trade.pnl_pips;
                        gross_profit += trade.pnl_pips;
                        returns.push_back(trade.pnl_pips);

                        in_position = false;
                    }
                    // Check SL
                    else if (current_pnl_pips <= -config_.sl_pips) {
                        TradeResult trade;
                        trade.entry_time = entry_time;
                        trade.exit_time = ts;
                        trade.side = "BUY";
                        trade.entry_price = entry_price;
                        trade.exit_price = bid;
                        trade.pnl_pips = current_pnl_pips;
                        trade.pnl_dollars = current_pnl_pips * 10.0;
                        trade.exit_reason = "SL";
                        trade.tts_nanoseconds = get_last_tts_ns();

                        result.trades.push_back(trade);
                        equity += trade.pnl_pips;
                        gross_loss += std::abs(trade.pnl_pips);
                        returns.push_back(trade.pnl_pips);

                        in_position = false;
                    }

                    // Update max drawdown
                    if (equity > peak_equity) peak_equity = equity;
                    double dd = peak_equity - equity;
                    if (dd > result.max_drawdown_pips) result.max_drawdown_pips = dd;
                }
                // Enter new position
                else if (signal == 1) {
                    in_position = true;
                    entry_price = ask;  // Buy at ask
                    entry_time = ts;
                    entry_idx = static_cast<int>(i);
                }
            }

            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

            // Calculate results
            result.total_trades = static_cast<int>(result.trades.size());
            for (const auto& t : result.trades) {
                if (t.pnl_pips > 0) result.winning_trades++;
                else result.losing_trades++;
                result.total_pnl_pips += t.pnl_pips;
                result.total_pnl_dollars += t.pnl_dollars;
            }

            if (result.total_trades > 0) {
                result.win_rate = 100.0 * result.winning_trades / result.total_trades;
            }

            if (gross_loss > 0) {
                result.profit_factor = gross_profit / gross_loss;
            }

            // Sharpe ratio
            if (returns.size() > 1) {
                double mean = 0.0;
                for (double r : returns) mean += r;
                mean /= returns.size();

                double variance = 0.0;
                for (double r : returns) variance += (r - mean) * (r - mean);
                variance /= returns.size();
                double std_dev = std::sqrt(variance);

                if (std_dev > 0) {
                    result.sharpe_ratio = (mean / std_dev) * std::sqrt(252.0);  // Annualized
                }
            }

            // TTS metrics
            auto tts_stats = profiler_.get_statistics();
            result.avg_tts_microseconds = tts_stats.mean_us();
            result.p99_tts_microseconds = tts_stats.p99_us();
            result.max_tts_microseconds = tts_stats.max_us();
            result.total_ticks_processed = tick_count_;

            if (duration_ms > 0) {
                result.throughput_ticks_per_sec = (tick_count_ * 1000.0) / duration_ms;
            }

            return result;
        }

        /**
         * Reset strategy state
         */
        void reset() {
            fast_ema_ = 0.0;
            slow_ema_ = 0.0;
            initialized_ = false;
            last_signal_ = 0;
            tick_count_ = 0;
            profiler_.clear();
        }

        // Getters
        [[nodiscard]] double get_fast_ema() const { return fast_ema_; }
        [[nodiscard]] double get_slow_ema() const { return slow_ema_; }
        [[nodiscard]] int get_last_signal() const { return last_signal_; }
        [[nodiscard]] int64_t get_tick_count() const { return tick_count_; }

        [[nodiscard]] performance::LatencyStatistics get_tts_stats() const {
            return profiler_.get_statistics();
        }

        [[nodiscard]] std::string generate_report() const {
            return profiler_.generate_report();
        }

    private:
        EMAMomentumConfig config_;

        double fast_alpha_;
        double slow_alpha_;
        double fast_ema_ = 0.0;
        double slow_ema_ = 0.0;
        bool initialized_ = false;
        int last_signal_ = 0;
        int64_t tick_count_ = 0;

        performance::LatencyProfiler profiler_;

        int64_t get_last_tts_ns() const {
            auto measurements = profiler_.get_measurements();
            if (!measurements.empty()) {
                return measurements.back().total_tick_to_signal.count();
            }
            return 0;
        }
    };

    /**
     * Print backtest result summary
     */
    inline void print_backtest_result(const BacktestResult& result) {
        std::cout << "\n";
        std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
        std::cout << "║          EMA MOMENTUM STRATEGY - BACKTEST RESULTS            ║\n";
        std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
        std::cout << "║  TRADE STATISTICS                                            ║\n";
        std::cout << "║  ─────────────────                                           ║\n";
        printf("║  Total Trades:      %-8d                                  ║\n", result.total_trades);
        printf("║  Winning Trades:    %-8d                                  ║\n", result.winning_trades);
        printf("║  Losing Trades:     %-8d                                  ║\n", result.losing_trades);
        printf("║  Win Rate:          %-8.2f%%                                 ║\n", result.win_rate);
        std::cout << "║                                                              ║\n";
        std::cout << "║  PERFORMANCE METRICS                                         ║\n";
        std::cout << "║  ───────────────────                                         ║\n";
        printf("║  Total P&L:         %-+8.1f pips ($%-+.2f)                  ║\n",
               result.total_pnl_pips, result.total_pnl_dollars);
        printf("║  Profit Factor:     %-8.2f                                  ║\n", result.profit_factor);
        printf("║  Sharpe Ratio:      %-8.2f                                  ║\n", result.sharpe_ratio);
        printf("║  Max Drawdown:      %-8.1f pips                             ║\n", result.max_drawdown_pips);
        std::cout << "║                                                              ║\n";
        std::cout << "║  TICK-TO-SIGNAL LATENCY (TTS)                                ║\n";
        std::cout << "║  ────────────────────────────                                ║\n";
        printf("║  Ticks Processed:   %-12ld                              ║\n", result.total_ticks_processed);
        printf("║  Avg TTS:           %-8.3f µs                              ║\n", result.avg_tts_microseconds);
        printf("║  P99 TTS:           %-8.3f µs                              ║\n", result.p99_tts_microseconds);
        printf("║  Max TTS:           %-8.3f µs                              ║\n", result.max_tts_microseconds);
        printf("║  Throughput:        %-12.0f ticks/sec                    ║\n", result.throughput_ticks_per_sec);
        std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
    }

} // namespace gel::strategies
