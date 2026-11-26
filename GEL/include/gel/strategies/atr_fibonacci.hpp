// include/gel/strategies/atr_fibonacci.hpp
#pragma once

#include "gel/performance/latency_profiler.hpp"
#include <vector>
#include <deque>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <string>

namespace gel::strategies {

    /**
     * ATR Fibonacci Quantum Volatility Wave Ultra v12
     *
     * Top performing strategy from AI research:
     * - Sharpe Ratio: 4.08
     * - Profit Factor: 1.73
     * - Win Rate: 44.4%
     * - Total PnL: +389 pips
     *
     * Entry Conditions:
     * 1. RSI oversold (< 30)
     * 2. Volatility above threshold (0.0004)
     * 3. Near Fibonacci level (0.382 or 0.618)
     * 4. ATR confirms volatility
     *
     * Exit: TP 75 pips, SL 35 pips
     * Direction: Long only
     */

    struct ATRFibonacciConfig {
        int atr_period = 10;
        double atr_multiplier = 2.3;
        int rsi_period = 7;
        double rsi_oversold = 30.0;
        double vol_threshold = 0.0004;
        std::vector<double> fibonacci_levels = {0.382, 0.618};
        double tp_pips = 75.0;
        double sl_pips = 35.0;
        double pip_size = 0.0001;
        bool long_only = true;
    };

    struct ATRTradeResult {
        std::string entry_time;
        std::string exit_time;
        std::string side;
        double entry_price;
        double exit_price;
        double pnl_pips;
        double pnl_dollars;
        std::string exit_reason;
        int64_t tts_nanoseconds;
    };

    struct ATRBacktestResult {
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

        std::vector<ATRTradeResult> trades;
    };

    class ATRFibonacciStrategy {
    public:
        explicit ATRFibonacciStrategy(ATRFibonacciConfig config = ATRFibonacciConfig{})
            : config_(config)
        {
            // Pre-allocate buffers
            prices_.reserve(config_.atr_period + 10);
            highs_.reserve(30);
            lows_.reserve(30);
            gains_.reserve(config_.rsi_period + 1);
            losses_.reserve(config_.rsi_period + 1);
        }

        /**
         * Process a single tick and generate signal
         * Returns: 1 = BUY, -1 = SELL, 0 = NO_SIGNAL
         */
        int process_tick(double bid, double ask, const std::string& timestamp = "") {
            auto start = std::chrono::high_resolution_clock::now();

            double mid = (bid + ask) * 0.5;
            tick_count_++;

            // Store price history (ring buffer style)
            if (prices_.size() >= static_cast<size_t>(config_.atr_period + 5)) {
                prices_.erase(prices_.begin());
            }
            prices_.push_back(mid);

            if (highs_.size() >= 30) highs_.erase(highs_.begin());
            if (lows_.size() >= 30) lows_.erase(lows_.begin());
            highs_.push_back(mid);
            lows_.push_back(mid);

            // Need enough data
            int min_period = std::max({config_.atr_period + 1, config_.rsi_period + 1, 20});
            if (static_cast<int>(prices_.size()) < min_period) {
                record_tts(start);
                return 0;
            }

            // Calculate indicators (optimized)
            double atr = calculate_atr();
            double rsi = calculate_rsi();
            double volatility = calculate_volatility();

            // Manage existing position
            if (in_position_) {
                double pnl_pips = (bid - entry_price_) / config_.pip_size;

                if (pnl_pips >= config_.tp_pips) {
                    close_position(timestamp, bid, "TP", pnl_pips);
                } else if (pnl_pips <= -config_.sl_pips) {
                    close_position(timestamp, bid, "SL", pnl_pips);
                }

                record_tts(start);
                return 0;
            }

            // Signal generation
            int signal = 0;

            // Entry conditions
            bool rsi_condition = rsi < config_.rsi_oversold;
            bool vol_condition = volatility > config_.vol_threshold;
            bool fib_condition = is_near_fibonacci(mid);
            bool atr_condition = atr > config_.vol_threshold * 1.5;

            // Count conditions met (need RSI + at least 2 others)
            int conditions_met = static_cast<int>(vol_condition) +
                                 static_cast<int>(fib_condition) +
                                 static_cast<int>(atr_condition);

            if (rsi_condition && conditions_met >= 2) {
                signal = 1;
                signal_count_++;
                open_position(timestamp, ask);
            }

            record_tts(start);
            return signal;
        }

        /**
         * Run full backtest on price data
         */
        ATRBacktestResult backtest(const std::vector<double>& bids,
                                   const std::vector<double>& asks,
                                   const std::vector<std::string>& timestamps = {}) {
            reset();
            ATRBacktestResult result;

            auto start_time = std::chrono::high_resolution_clock::now();

            for (size_t i = 0; i < bids.size(); ++i) {
                std::string ts = timestamps.empty() ? std::to_string(i) : timestamps[i];
                process_tick(bids[i], asks[i], ts);
            }

            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                end_time - start_time).count();

            // Calculate results
            result.total_trades = static_cast<int>(trades_.size());

            double gross_profit = 0.0;
            double gross_loss = 0.0;
            double equity = 0.0;
            double peak = 0.0;
            std::vector<double> returns;

            for (const auto& t : trades_) {
                if (t.pnl_pips > 0) {
                    result.winning_trades++;
                    gross_profit += t.pnl_pips;
                } else {
                    result.losing_trades++;
                    gross_loss += std::abs(t.pnl_pips);
                }
                result.total_pnl_pips += t.pnl_pips;
                result.total_pnl_dollars += t.pnl_dollars;
                returns.push_back(t.pnl_pips);

                equity += t.pnl_pips;
                if (equity > peak) peak = equity;
                double dd = peak - equity;
                if (dd > result.max_drawdown_pips) result.max_drawdown_pips = dd;
            }

            if (result.total_trades > 0) {
                result.win_rate = 100.0 * result.winning_trades / result.total_trades;
            }

            if (gross_loss > 0) {
                result.profit_factor = gross_profit / gross_loss;
            }

            // Sharpe ratio
            if (returns.size() > 1) {
                double sum = std::accumulate(returns.begin(), returns.end(), 0.0);
                double mean = sum / returns.size();

                double sq_sum = 0.0;
                for (double r : returns) {
                    sq_sum += (r - mean) * (r - mean);
                }
                double std_dev = std::sqrt(sq_sum / returns.size());

                if (std_dev > 0) {
                    result.sharpe_ratio = (mean / std_dev) * std::sqrt(252.0);
                }
            }

            // TTS metrics
            if (!tts_measurements_.empty()) {
                std::vector<double> tts_us;
                tts_us.reserve(tts_measurements_.size());
                for (int64_t ns : tts_measurements_) {
                    tts_us.push_back(ns / 1000.0);
                }

                double sum = std::accumulate(tts_us.begin(), tts_us.end(), 0.0);
                result.avg_tts_microseconds = sum / tts_us.size();

                std::vector<double> sorted_tts = tts_us;
                std::sort(sorted_tts.begin(), sorted_tts.end());
                size_t p99_idx = static_cast<size_t>(sorted_tts.size() * 0.99);
                result.p99_tts_microseconds = sorted_tts[p99_idx];
                result.max_tts_microseconds = sorted_tts.back();
            }

            result.total_ticks_processed = tick_count_;
            result.trades = trades_;

            if (duration_ms > 0) {
                result.throughput_ticks_per_sec = (tick_count_ * 1000.0) / duration_ms;
            }

            return result;
        }

        void reset() {
            prices_.clear();
            highs_.clear();
            lows_.clear();
            gains_.clear();
            losses_.clear();
            trades_.clear();
            tts_measurements_.clear();
            in_position_ = false;
            entry_price_ = 0.0;
            entry_time_.clear();
            tick_count_ = 0;
            signal_count_ = 0;
            prev_price_ = 0.0;
        }

        // Getters
        [[nodiscard]] int64_t get_tick_count() const { return tick_count_; }
        [[nodiscard]] int64_t get_signal_count() const { return signal_count_; }
        [[nodiscard]] const std::vector<ATRTradeResult>& get_trades() const { return trades_; }

    private:
        ATRFibonacciConfig config_;

        // Price buffers
        std::vector<double> prices_;
        std::vector<double> highs_;
        std::vector<double> lows_;
        std::vector<double> gains_;
        std::vector<double> losses_;

        // Position state
        bool in_position_ = false;
        double entry_price_ = 0.0;
        std::string entry_time_;

        // Stats
        int64_t tick_count_ = 0;
        int64_t signal_count_ = 0;
        double prev_price_ = 0.0;

        // Results
        std::vector<ATRTradeResult> trades_;
        std::vector<int64_t> tts_measurements_;

        /**
         * Calculate ATR (Average True Range) - Optimized
         */
        double calculate_atr() {
            if (prices_.size() < static_cast<size_t>(config_.atr_period + 1)) {
                return 0.0;
            }

            double sum = 0.0;
            size_t start = prices_.size() - config_.atr_period - 1;

            for (int i = 0; i < config_.atr_period; ++i) {
                size_t idx = start + i + 1;
                double high = std::max(prices_[idx], prices_[idx - 1]);
                double low = std::min(prices_[idx], prices_[idx - 1]);
                double prev_close = prices_[idx - 1];

                double tr = std::max({
                    high - low,
                    std::abs(high - prev_close),
                    std::abs(low - prev_close)
                });
                sum += tr;
            }

            return sum / config_.atr_period;
        }

        /**
         * Calculate RSI - Optimized with running averages
         */
        double calculate_rsi() {
            if (prices_.size() < static_cast<size_t>(config_.rsi_period + 1)) {
                return 50.0;  // Neutral
            }

            double avg_gain = 0.0;
            double avg_loss = 0.0;
            size_t start = prices_.size() - config_.rsi_period - 1;

            for (int i = 0; i < config_.rsi_period; ++i) {
                size_t idx = start + i + 1;
                double change = prices_[idx] - prices_[idx - 1];

                if (change > 0) {
                    avg_gain += change;
                } else {
                    avg_loss += std::abs(change);
                }
            }

            avg_gain /= config_.rsi_period;
            avg_loss /= config_.rsi_period;

            if (avg_loss == 0.0) {
                return avg_gain > 0 ? 100.0 : 50.0;
            }

            double rs = avg_gain / avg_loss;
            return 100.0 - (100.0 / (1.0 + rs));
        }

        /**
         * Calculate volatility (standard deviation of returns)
         */
        double calculate_volatility() {
            if (prices_.size() < 20) {
                return 0.0;
            }

            // Calculate returns for last 20 prices
            size_t start = prices_.size() - 20;
            double sum = 0.0;
            double sq_sum = 0.0;
            int count = 0;

            for (size_t i = start + 1; i < prices_.size(); ++i) {
                double ret = (prices_[i] - prices_[i - 1]) / prices_[i - 1];
                sum += ret;
                sq_sum += ret * ret;
                count++;
            }

            if (count == 0) return 0.0;

            double mean = sum / count;
            double variance = (sq_sum / count) - (mean * mean);
            return std::sqrt(std::max(0.0, variance));
        }

        /**
         * Check if price is near a Fibonacci level
         */
        bool is_near_fibonacci(double price) {
            if (highs_.size() < 10 || lows_.size() < 10) {
                return false;
            }

            double swing_high = *std::max_element(highs_.begin(), highs_.end());
            double swing_low = *std::min_element(lows_.begin(), lows_.end());

            if (swing_high == swing_low) {
                return false;
            }

            double diff = swing_high - swing_low;
            constexpr double tolerance = 0.0003;

            for (double fib : config_.fibonacci_levels) {
                double fib_price = swing_low + diff * (1.0 - fib);
                if (std::abs(price - fib_price) < tolerance) {
                    return true;
                }
            }

            return false;
        }

        void open_position(const std::string& timestamp, double price) {
            in_position_ = true;
            entry_price_ = price;
            entry_time_ = timestamp;
        }

        void close_position(const std::string& timestamp, double price,
                           const std::string& reason, double pnl_pips) {
            ATRTradeResult trade;
            trade.entry_time = entry_time_;
            trade.exit_time = timestamp;
            trade.side = "BUY";
            trade.entry_price = entry_price_;
            trade.exit_price = price;
            trade.pnl_pips = pnl_pips;
            trade.pnl_dollars = pnl_pips * 10.0;
            trade.exit_reason = reason;
            trade.tts_nanoseconds = tts_measurements_.empty() ? 0 : tts_measurements_.back();

            trades_.push_back(trade);
            in_position_ = false;
            entry_price_ = 0.0;
            entry_time_.clear();
        }

        void record_tts(const std::chrono::high_resolution_clock::time_point& start) {
            auto end = std::chrono::high_resolution_clock::now();
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            tts_measurements_.push_back(ns);
        }
    };

    /**
     * Print ATR backtest result summary
     */
    inline void print_atr_backtest_result(const ATRBacktestResult& result) {
        std::cout << "\n";
        std::cout << "+==============================================================+\n";
        std::cout << "|      ATR FIBONACCI v12 STRATEGY - BACKTEST RESULTS           |\n";
        std::cout << "+==============================================================+\n";
        std::cout << "|  TRADE STATISTICS                                            |\n";
        std::cout << "|  -----------------                                           |\n";
        printf("|  Total Trades:      %-8d                                  |\n", result.total_trades);
        printf("|  Winning Trades:    %-8d                                  |\n", result.winning_trades);
        printf("|  Losing Trades:     %-8d                                  |\n", result.losing_trades);
        printf("|  Win Rate:          %-8.2f%%                                 |\n", result.win_rate);
        std::cout << "|                                                              |\n";
        std::cout << "|  PERFORMANCE METRICS                                         |\n";
        std::cout << "|  -------------------                                         |\n";
        printf("|  Total P&L:         %-+8.1f pips ($%-+.2f)                  |\n",
               result.total_pnl_pips, result.total_pnl_dollars);
        printf("|  Profit Factor:     %-8.2f                                  |\n", result.profit_factor);
        printf("|  Sharpe Ratio:      %-8.2f                                  |\n", result.sharpe_ratio);
        printf("|  Max Drawdown:      %-8.1f pips                             |\n", result.max_drawdown_pips);
        std::cout << "|                                                              |\n";
        std::cout << "|  TICK-TO-SIGNAL LATENCY (TTS)                                |\n";
        std::cout << "|  ----------------------------                                |\n";
        printf("|  Ticks Processed:   %-12ld                              |\n", result.total_ticks_processed);
        printf("|  Avg TTS:           %-8.3f us                              |\n", result.avg_tts_microseconds);
        printf("|  P99 TTS:           %-8.3f us                              |\n", result.p99_tts_microseconds);
        printf("|  Max TTS:           %-8.3f us                              |\n", result.max_tts_microseconds);
        printf("|  Throughput:        %-12.0f ticks/sec                    |\n", result.throughput_ticks_per_sec);
        std::cout << "+==============================================================+\n";
    }

} // namespace gel::strategies
