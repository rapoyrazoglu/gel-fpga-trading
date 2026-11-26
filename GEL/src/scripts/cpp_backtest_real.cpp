// Standalone C++ backtest with real CSV data
// Compile: g++ -O3 -std=c++17 -I../include cpp_backtest_real.cpp -o cpp_backtest

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <chrono>
#include <iomanip>

// Inline ATR Fibonacci Strategy (simplified for standalone compile)
#include <deque>
#include <cmath>
#include <algorithm>
#include <numeric>

struct TradeResult {
    std::string entry_time;
    std::string exit_time;
    double entry_price;
    double exit_price;
    double pnl_pips;
    std::string exit_reason;
};

class ATRFibonacciStrategy {
public:
    // Config - Original ATR Fibonacci v12 parameters from top10.json
    int atr_period = 10;
    int rsi_period = 7;
    double rsi_oversold = 30.0;  // Original: RSI < 30
    double vol_threshold = 0.0004;  // Original: 0.0004
    double tp_pips = 75.0;
    double sl_pips = 35.0;
    double pip = 0.0001;
    std::vector<double> fibonacci_levels = {0.382, 0.618};

    // State
    std::vector<double> prices;
    std::vector<double> highs;
    std::vector<double> lows;
    bool in_position = false;
    double entry_price = 0.0;
    std::string entry_time;

    // Results
    std::vector<TradeResult> trades;
    std::vector<int64_t> tts_measurements;
    int64_t tick_count = 0;
    int64_t signal_count = 0;

    double calculate_atr() {
        if (prices.size() < static_cast<size_t>(atr_period + 1)) return 0.0;

        double sum = 0.0;
        size_t start = prices.size() - atr_period - 1;

        for (int i = 0; i < atr_period; ++i) {
            size_t idx = start + i + 1;
            double high = std::max(prices[idx], prices[idx - 1]);
            double low = std::min(prices[idx], prices[idx - 1]);
            double prev_close = prices[idx - 1];
            double tr = std::max({high - low, std::abs(high - prev_close), std::abs(low - prev_close)});
            sum += tr;
        }
        return sum / atr_period;
    }

    double calculate_rsi() {
        if (prices.size() < static_cast<size_t>(rsi_period + 1)) return 50.0;

        double avg_gain = 0.0, avg_loss = 0.0;
        size_t start = prices.size() - rsi_period - 1;

        for (int i = 0; i < rsi_period; ++i) {
            size_t idx = start + i + 1;
            double change = prices[idx] - prices[idx - 1];
            if (change > 0) avg_gain += change;
            else avg_loss += std::abs(change);
        }

        avg_gain /= rsi_period;
        avg_loss /= rsi_period;

        if (avg_loss == 0.0) return avg_gain > 0 ? 100.0 : 50.0;
        double rs = avg_gain / avg_loss;
        return 100.0 - (100.0 / (1.0 + rs));
    }

    double calculate_volatility() {
        if (prices.size() < 20) return 0.0;

        size_t start = prices.size() - 20;
        double sum = 0.0, sq_sum = 0.0;
        int count = 0;

        for (size_t i = start + 1; i < prices.size(); ++i) {
            double ret = (prices[i] - prices[i - 1]) / prices[i - 1];
            sum += ret;
            sq_sum += ret * ret;
            count++;
        }

        if (count == 0) return 0.0;
        double mean = sum / count;
        double variance = (sq_sum / count) - (mean * mean);
        return std::sqrt(std::max(0.0, variance));
    }

    bool is_near_fibonacci(double price) {
        if (highs.size() < 10 || lows.size() < 10) return false;

        double swing_high = *std::max_element(highs.begin(), highs.end());
        double swing_low = *std::min_element(lows.begin(), lows.end());

        if (swing_high == swing_low) return false;

        double diff = swing_high - swing_low;
        constexpr double tolerance = 0.0003;

        for (double fib : fibonacci_levels) {
            double fib_price = swing_low + diff * (1.0 - fib);
            if (std::abs(price - fib_price) < tolerance) return true;
        }
        return false;
    }

    int process_tick(const std::string& timestamp, double bid, double ask) {
        auto start = std::chrono::high_resolution_clock::now();

        double mid = (bid + ask) * 0.5;
        tick_count++;

        // Store price history (keep last 50)
        if (prices.size() >= 50) prices.erase(prices.begin());
        prices.push_back(mid);

        if (highs.size() >= 30) highs.erase(highs.begin());
        if (lows.size() >= 30) lows.erase(lows.begin());
        highs.push_back(mid);
        lows.push_back(mid);

        // Need enough data
        if (prices.size() < 20) {
            auto end = std::chrono::high_resolution_clock::now();
            tts_measurements.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
            return 0;
        }

        // Calculate indicators
        double atr = calculate_atr();
        double rsi = calculate_rsi();
        double vol = calculate_volatility();

        // Manage position
        if (in_position) {
            double pnl_pips = (bid - entry_price) / pip;

            if (pnl_pips >= tp_pips) {
                trades.push_back({entry_time, timestamp, entry_price, bid, pnl_pips, "TP"});
                in_position = false;
            } else if (pnl_pips <= -sl_pips) {
                trades.push_back({entry_time, timestamp, entry_price, bid, pnl_pips, "SL"});
                in_position = false;
            }

            auto end = std::chrono::high_resolution_clock::now();
            tts_measurements.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
            return 0;
        }

        // Signal generation
        bool rsi_cond = rsi < rsi_oversold;
        bool vol_cond = vol > vol_threshold;
        bool fib_cond = is_near_fibonacci(mid);
        bool atr_cond = atr > vol_threshold * 1.5;

        int conditions = static_cast<int>(vol_cond) + static_cast<int>(fib_cond) + static_cast<int>(atr_cond);

        if (rsi_cond && conditions >= 2) {
            signal_count++;
            in_position = true;
            entry_price = ask;
            entry_time = timestamp;
        }

        auto end = std::chrono::high_resolution_clock::now();
        tts_measurements.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
        return rsi_cond && conditions >= 2 ? 1 : 0;
    }

    void print_results() {
        int wins = 0, losses = 0;
        double total_pnl = 0.0, gross_profit = 0.0, gross_loss = 0.0;

        for (const auto& t : trades) {
            total_pnl += t.pnl_pips;
            if (t.pnl_pips > 0) {
                wins++;
                gross_profit += t.pnl_pips;
            } else {
                losses++;
                gross_loss += std::abs(t.pnl_pips);
            }
        }

        double win_rate = trades.empty() ? 0 : 100.0 * wins / trades.size();
        double pf = gross_loss > 0 ? gross_profit / gross_loss : 0;

        // TTS stats
        double avg_tts = 0, p99_tts = 0, max_tts = 0;
        if (!tts_measurements.empty()) {
            double sum = 0;
            for (auto t : tts_measurements) sum += t;
            avg_tts = sum / tts_measurements.size() / 1000.0;  // to microseconds

            auto sorted = tts_measurements;
            std::sort(sorted.begin(), sorted.end());
            p99_tts = sorted[static_cast<size_t>(sorted.size() * 0.99)] / 1000.0;
            max_tts = sorted.back() / 1000.0;
        }

        std::cout << "\n";
        std::cout << "============================================================\n";
        std::cout << "  ATR FIBONACCI v12 - C++ REAL DATA BACKTEST\n";
        std::cout << "============================================================\n";
        std::cout << "\n  TRADE STATISTICS\n";
        std::cout << "  -----------------\n";
        std::cout << "  Total Trades:    " << trades.size() << "\n";
        std::cout << "  Winning:         " << wins << "\n";
        std::cout << "  Losing:          " << losses << "\n";
        std::cout << "  Win Rate:        " << std::fixed << std::setprecision(1) << win_rate << "%\n";
        std::cout << "\n  PERFORMANCE\n";
        std::cout << "  -----------\n";
        std::cout << "  Total P&L:       " << std::showpos << total_pnl << " pips ($" << total_pnl * 10 << ")\n";
        std::cout << std::noshowpos;
        std::cout << "  Profit Factor:   " << std::setprecision(2) << pf << "\n";
        std::cout << "\n  TICK-TO-SIGNAL LATENCY\n";
        std::cout << "  ----------------------\n";
        std::cout << "  Ticks Processed: " << tick_count << "\n";
        std::cout << "  Signals:         " << signal_count << "\n";
        std::cout << "  Avg TTS:         " << std::setprecision(3) << avg_tts << " us\n";
        std::cout << "  P99 TTS:         " << p99_tts << " us\n";
        std::cout << "  Max TTS:         " << max_tts << " us\n";
        std::cout << "============================================================\n";
    }
};

int main(int argc, char* argv[]) {
    std::string csv_path = "data/EURUSD_all_ticks_20250601_20251014.csv";
    int max_rows = 1000000;  // 1M rows for quick test

    if (argc > 1) csv_path = argv[1];
    if (argc > 2) max_rows = std::stoi(argv[2]);

    std::cout << "============================================================\n";
    std::cout << "  GEL TRADING SYSTEM - C++ ATR FIBONACCI BACKTEST\n";
    std::cout << "============================================================\n";
    std::cout << "\nData file: " << csv_path << "\n";
    std::cout << "Max rows: " << max_rows << "\n";

    std::ifstream file(csv_path);
    if (!file.is_open()) {
        std::cerr << "ERROR: Cannot open file: " << csv_path << "\n";
        return 1;
    }

    ATRFibonacciStrategy strategy;

    std::string line;
    std::getline(file, line);  // Skip header

    int row_count = 0;
    auto start_time = std::chrono::high_resolution_clock::now();

    std::cout << "\nProcessing...\n";

    while (std::getline(file, line) && row_count < max_rows) {
        std::stringstream ss(line);
        std::string datetime, bid_str, ask_str;

        std::getline(ss, datetime, ',');
        std::getline(ss, bid_str, ',');
        std::getline(ss, ask_str, ',');

        try {
            double bid = std::stod(bid_str);
            double ask = std::stod(ask_str);
            strategy.process_tick(datetime, bid, ask);
            row_count++;

            if (row_count % 500000 == 0) {
                std::cout << "  Processed: " << row_count << " rows, Trades: " << strategy.trades.size() << "\n";
            }
        } catch (...) {
            continue;
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    std::cout << "\nTotal: " << row_count << " rows in " << duration << " ms\n";
    std::cout << "Throughput: " << (row_count * 1000.0 / duration) << " ticks/sec\n";

    strategy.print_results();

    return 0;
}
