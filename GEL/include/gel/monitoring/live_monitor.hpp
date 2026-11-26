// include/gel/monitoring/live_monitor.hpp
#pragma once

#include <chrono>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <iostream>
#include <iomanip>
#include <sstream>

namespace gel::monitoring {

    using time_point = std::chrono::system_clock::time_point;
    using duration_ms = std::chrono::milliseconds;

    /**
     * LiveMonitor - Bloomberg-style real-time trading monitor
     *
     * Features:
     * - Real-time tick-to-signal latency tracking
     * - API request rate monitoring (120 req/sec limit)
     * - Balance change tracking (every update)
     * - Performance metrics (win rate, P&L, Sharpe)
     * - Visual terminal dashboard
     */
    class LiveMonitor {
    public:
        struct TradeMetrics {
            int total_trades = 0;
            int winning_trades = 0;
            int losing_trades = 0;
            double total_pnl = 0.0;
            double total_win_pnl = 0.0;
            double total_loss_pnl = 0.0;
            double max_drawdown = 0.0;
            double peak_balance = 0.0;

            double win_rate() const {
                return total_trades > 0 ? (double)winning_trades / total_trades * 100.0 : 0.0;
            }

            double profit_factor() const {
                return total_loss_pnl != 0.0 ? std::abs(total_win_pnl / total_loss_pnl) : 0.0;
            }

            double avg_win() const {
                return winning_trades > 0 ? total_win_pnl / winning_trades : 0.0;
            }

            double avg_loss() const {
                return losing_trades > 0 ? total_loss_pnl / losing_trades : 0.0;
            }
        };

        struct LatencyMetrics {
            duration_ms tick_to_signal{0};      // Tick arrival -> Signal generation
            duration_ms signal_to_order{0};     // Signal -> Order placement
            duration_ms order_to_fill{0};       // Order -> Fill confirmation
            duration_ms total_latency{0};       // Total: Tick -> Fill

            time_point tick_time;
            time_point signal_time;
            time_point order_time;
            time_point fill_time;
        };

        struct APIMetrics {
            std::atomic<int> requests_last_second{0};
            std::atomic<int> total_requests{0};
            std::atomic<int> failed_requests{0};
            std::atomic<int> rate_limit_hits{0};

            time_point window_start;
            std::deque<time_point> request_timestamps;  // Last 120 requests

            double current_rate() const {
                return requests_last_second.load();
            }

            double success_rate() const {
                int total = total_requests.load();
                int failed = failed_requests.load();
                return total > 0 ? (double)(total - failed) / total * 100.0 : 100.0;
            }
        };

        struct BalanceSnapshot {
            time_point timestamp;
            double balance;
            double unrealized_pnl;
            double realized_pnl;
            double daily_pnl;
            int open_positions;
        };

        struct Config {
            bool enable_terminal_ui = true;
            int update_interval_ms = 1000;      // Update dashboard every 1 second
            int history_window_sec = 300;       // Keep 5 minutes of history
            bool color_output = true;
            bool save_to_file = true;
            std::string log_file = "trading_monitor.log";
        };

        explicit LiveMonitor(Config config)
            : config_(std::move(config))
        {
            start_time_ = std::chrono::system_clock::now();
            api_metrics_.window_start = start_time_;
        }

        LiveMonitor()
            : LiveMonitor(Config{})
        {
        }

        // === Tick-to-Signal Latency Tracking ===

        void on_tick_received(const std::string& instrument, double price) {
            std::lock_guard<std::mutex> lock(mutex_);
            current_latency_.tick_time = std::chrono::system_clock::now();
            last_tick_price_ = price;
            last_tick_instrument_ = instrument;
            total_ticks_++;
        }

        void on_signal_generated(const std::string& signal_type) {
            std::lock_guard<std::mutex> lock(mutex_);
            current_latency_.signal_time = std::chrono::system_clock::now();
            current_latency_.tick_to_signal = std::chrono::duration_cast<duration_ms>(
                current_latency_.signal_time - current_latency_.tick_time
            );

            last_signal_type_ = signal_type;
            total_signals_++;

            // Track average latency
            latency_history_.push_back(current_latency_.tick_to_signal.count());
            if (latency_history_.size() > 1000) {
                latency_history_.pop_front();
            }
        }

        void on_order_placed(const std::string& order_id) {
            std::lock_guard<std::mutex> lock(mutex_);
            current_latency_.order_time = std::chrono::system_clock::now();
            current_latency_.signal_to_order = std::chrono::duration_cast<duration_ms>(
                current_latency_.order_time - current_latency_.signal_time
            );
            last_order_id_ = order_id;
        }

        void on_order_filled(double fill_price, double pnl = 0.0) {
            std::lock_guard<std::mutex> lock(mutex_);
            current_latency_.fill_time = std::chrono::system_clock::now();
            current_latency_.order_to_fill = std::chrono::duration_cast<duration_ms>(
                current_latency_.fill_time - current_latency_.order_time
            );
            current_latency_.total_latency = std::chrono::duration_cast<duration_ms>(
                current_latency_.fill_time - current_latency_.tick_time
            );

            last_fill_price_ = fill_price;

            // Update trade metrics
            metrics_.total_trades++;
            metrics_.total_pnl += pnl;

            if (pnl > 0) {
                metrics_.winning_trades++;
                metrics_.total_win_pnl += pnl;
            } else if (pnl < 0) {
                metrics_.losing_trades++;
                metrics_.total_loss_pnl += pnl;
            }

            // Store latency for this trade
            completed_latencies_.push_back(current_latency_);
            if (completed_latencies_.size() > 1000) {
                completed_latencies_.pop_front();
            }
        }

        // === API Request Monitoring ===

        void on_api_request(const std::string& endpoint) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto now = std::chrono::system_clock::now();

            api_metrics_.total_requests++;
            api_metrics_.request_timestamps.push_back(now);

            // Remove timestamps older than 1 second
            auto cutoff = now - std::chrono::seconds(1);
            while (!api_metrics_.request_timestamps.empty() &&
                   api_metrics_.request_timestamps.front() < cutoff) {
                api_metrics_.request_timestamps.pop_front();
            }

            api_metrics_.requests_last_second = api_metrics_.request_timestamps.size();

            // Check rate limit (120 req/sec)
            if (api_metrics_.requests_last_second > 120) {
                api_metrics_.rate_limit_hits++;
            }

            last_api_endpoint_ = endpoint;
        }

        void on_api_error(const std::string& error) {
            api_metrics_.failed_requests++;
            last_api_error_ = error;
        }

        // === Balance Tracking ===

        void on_balance_update(double balance, double unrealized_pnl,
                               double realized_pnl, int open_positions) {
            std::lock_guard<std::mutex> lock(mutex_);

            BalanceSnapshot snapshot;
            snapshot.timestamp = std::chrono::system_clock::now();
            snapshot.balance = balance;
            snapshot.unrealized_pnl = unrealized_pnl;
            snapshot.realized_pnl = realized_pnl;
            snapshot.daily_pnl = realized_pnl - initial_balance_ + balance;
            snapshot.open_positions = open_positions;

            balance_history_.push_back(snapshot);

            // Keep only recent history
            auto cutoff = snapshot.timestamp - std::chrono::seconds(config_.history_window_sec);
            while (!balance_history_.empty() && balance_history_.front().timestamp < cutoff) {
                balance_history_.pop_front();
            }

            // Track peak and drawdown
            if (balance > metrics_.peak_balance) {
                metrics_.peak_balance = balance;
            }

            double drawdown = metrics_.peak_balance - balance;
            if (drawdown > metrics_.max_drawdown) {
                metrics_.max_drawdown = drawdown;
            }

            current_balance_ = balance;
            current_unrealized_pnl_ = unrealized_pnl;
            current_open_positions_ = open_positions;
        }

        // === Dashboard Rendering ===

        void render_dashboard() {
            std::lock_guard<std::mutex> lock(mutex_);

            std::ostringstream oss;

            // Clear screen (ANSI escape code)
            if (config_.enable_terminal_ui) {
                oss << "\033[2J\033[H";  // Clear screen and move cursor to top
            }

            auto now = std::chrono::system_clock::now();
            auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_);

            // Header
            oss << "╔══════════════════════════════════════════════════════════════════════════════╗\n";
            oss << "║                    GEL TRADING SYSTEM - LIVE MONITOR                         ║\n";
            oss << "╠══════════════════════════════════════════════════════════════════════════════╣\n";

            // Uptime and timestamp
            oss << "║ Uptime: " << std::setw(8) << uptime.count() << "s"
                << " │ Time: " << format_time(now)
                << " │ Status: " << (is_healthy() ? "🟢 HEALTHY" : "🔴 DEGRADED") << " ║\n";

            oss << "╠══════════════════════════════════════════════════════════════════════════════╣\n";

            // === BALANCE & P&L ===
            oss << "║ 💰 BALANCE & P&L                                                              ║\n";
            oss << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
            oss << "║ Current Balance:   $" << std::setw(12) << std::fixed << std::setprecision(2)
                << current_balance_ << " │ Open Positions: " << std::setw(3) << current_open_positions_ << "               ║\n";
            oss << "║ Unrealized P&L:    $" << std::setw(12) << std::fixed << std::setprecision(2)
                << current_unrealized_pnl_ << " │ Daily P&L:      $" << std::setw(12)
                << (metrics_.total_pnl) << " ║\n";
            oss << "║ Peak Balance:      $" << std::setw(12) << metrics_.peak_balance
                << " │ Max Drawdown:   $" << std::setw(12) << metrics_.max_drawdown << " ║\n";

            oss << "╠══════════════════════════════════════════════════════════════════════════════╣\n";

            // === TRADE METRICS ===
            oss << "║ 📊 TRADE PERFORMANCE                                                          ║\n";
            oss << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
            oss << "║ Total Trades: " << std::setw(6) << metrics_.total_trades
                << " │ Wins: " << std::setw(6) << metrics_.winning_trades
                << " │ Losses: " << std::setw(6) << metrics_.losing_trades << "                      ║\n";
            oss << "║ Win Rate:     " << std::setw(6) << std::fixed << std::setprecision(2)
                << metrics_.win_rate() << "% │ Profit Factor: " << std::setw(6)
                << metrics_.profit_factor() << "                             ║\n";
            oss << "║ Avg Win:      $" << std::setw(8) << metrics_.avg_win()
                << " │ Avg Loss:      $" << std::setw(8) << metrics_.avg_loss() << "                     ║\n";

            oss << "╠══════════════════════════════════════════════════════════════════════════════╣\n";

            // === LATENCY METRICS ===
            oss << "║ ⚡ LATENCY (Tick-to-Signal-to-Fill)                                          ║\n";
            oss << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
            oss << "║ Last Tick→Signal:   " << std::setw(6) << current_latency_.tick_to_signal.count() << " ms"
                << " │ Avg: " << std::setw(6) << avg_tick_to_signal_latency() << " ms                        ║\n";
            oss << "║ Last Signal→Order:  " << std::setw(6) << current_latency_.signal_to_order.count() << " ms"
                << " │ Avg: " << std::setw(6) << avg_signal_to_order_latency() << " ms                        ║\n";
            oss << "║ Last Order→Fill:    " << std::setw(6) << current_latency_.order_to_fill.count() << " ms"
                << " │ Avg: " << std::setw(6) << avg_order_to_fill_latency() << " ms                        ║\n";
            oss << "║ Last Total Latency: " << std::setw(6) << current_latency_.total_latency.count() << " ms"
                << " │ Avg: " << std::setw(6) << avg_total_latency() << " ms                        ║\n";

            oss << "╠══════════════════════════════════════════════════════════════════════════════╣\n";

            // === API METRICS ===
            oss << "║ 🌐 API MONITORING (Limit: 120 req/sec)                                       ║\n";
            oss << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
            oss << "║ Current Rate:  " << std::setw(6) << api_metrics_.current_rate() << " req/s"
                << " │ Total Requests: " << std::setw(10) << api_metrics_.total_requests.load() << "               ║\n";
            oss << "║ Success Rate:  " << std::setw(6) << std::fixed << std::setprecision(2)
                << api_metrics_.success_rate() << " %"
                << " │ Failed:         " << std::setw(10) << api_metrics_.failed_requests.load() << "               ║\n";
            oss << "║ Rate Limit Hits: " << std::setw(8) << api_metrics_.rate_limit_hits.load()
                << " │ Last Endpoint:  " << std::setw(25) << std::left << last_api_endpoint_ << std::right << "    ║\n";

            oss << "╠══════════════════════════════════════════════════════════════════════════════╣\n";

            // === SIGNAL TRACKING ===
            oss << "║ 🎯 SIGNAL TRACKING                                                            ║\n";
            oss << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
            oss << "║ Total Ticks:   " << std::setw(10) << total_ticks_
                << " │ Total Signals:  " << std::setw(10) << total_signals_ << "                 ║\n";
            oss << "║ Signal Rate:   " << std::setw(6) << std::fixed << std::setprecision(2)
                << signal_rate() << " %"
                << " │ Last Signal:    " << std::setw(25) << std::left << last_signal_type_ << std::right << "    ║\n";
            oss << "║ Last Tick:     $" << std::setw(8) << last_tick_price_
                << " │ Instrument:     " << std::setw(10) << last_tick_instrument_ << "                  ║\n";

            oss << "╚══════════════════════════════════════════════════════════════════════════════╝\n";

            std::cout << oss.str() << std::flush;

            // Save to file if enabled
            if (config_.save_to_file) {
                // TODO: Append to log file
            }
        }

        [[nodiscard]] bool is_healthy() const {
            // Check if system is healthy
            bool api_ok = api_metrics_.current_rate() < 110;  // Under 110 req/sec
            bool latency_ok = avg_total_latency() < 500;       // Under 500ms
            bool balance_ok = current_balance_ > initial_balance_ * 0.9;  // Not lost >10%

            return api_ok && latency_ok && balance_ok;
        }

        void set_initial_balance(double balance) {
            initial_balance_ = balance;
            current_balance_ = balance;
            metrics_.peak_balance = balance;
        }

    private:
        Config config_;
        mutable std::mutex mutex_;

        time_point start_time_;
        double initial_balance_ = 0.0;
        double current_balance_ = 0.0;
        double current_unrealized_pnl_ = 0.0;
        int current_open_positions_ = 0;

        // Metrics
        TradeMetrics metrics_;
        LatencyMetrics current_latency_;
        APIMetrics api_metrics_;

        // History
        std::deque<BalanceSnapshot> balance_history_;
        std::deque<LatencyMetrics> completed_latencies_;
        std::deque<long long> latency_history_;

        // Tracking
        std::atomic<int> total_ticks_{0};
        std::atomic<int> total_signals_{0};

        std::string last_tick_instrument_;
        double last_tick_price_ = 0.0;
        std::string last_signal_type_;
        std::string last_order_id_;
        double last_fill_price_ = 0.0;
        std::string last_api_endpoint_;
        std::string last_api_error_;

        // Helper functions
        double avg_tick_to_signal_latency() const {
            if (completed_latencies_.empty()) return 0.0;
            double sum = 0;
            for (const auto& l : completed_latencies_) {
                sum += l.tick_to_signal.count();
            }
            return sum / completed_latencies_.size();
        }

        double avg_signal_to_order_latency() const {
            if (completed_latencies_.empty()) return 0.0;
            double sum = 0;
            for (const auto& l : completed_latencies_) {
                sum += l.signal_to_order.count();
            }
            return sum / completed_latencies_.size();
        }

        double avg_order_to_fill_latency() const {
            if (completed_latencies_.empty()) return 0.0;
            double sum = 0;
            for (const auto& l : completed_latencies_) {
                sum += l.order_to_fill.count();
            }
            return sum / completed_latencies_.size();
        }

        double avg_total_latency() const {
            if (completed_latencies_.empty()) return 0.0;
            double sum = 0;
            for (const auto& l : completed_latencies_) {
                sum += l.total_latency.count();
            }
            return sum / completed_latencies_.size();
        }

        double signal_rate() const {
            return total_ticks_ > 0 ? (double)total_signals_ / total_ticks_ * 100.0 : 0.0;
        }

        std::string format_time(time_point tp) const {
            auto tt = std::chrono::system_clock::to_time_t(tp);
            std::tm tm = *std::localtime(&tt);
            std::ostringstream oss;
            oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
            return oss.str();
        }
    };

} // namespace gel::monitoring
