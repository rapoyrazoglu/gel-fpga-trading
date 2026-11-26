// EMA Momentum Strategy - C++ Real Data Backtest
// This strategy gave +208 pips in Python, let's verify in C++

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <chrono>
#include <iomanip>
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

class EMAMomentumStrategy {
public:
    // Config - Same as Python backtest
    int fast_period = 9;
    int slow_period = 50;
    double tp_pips = 50.0;
    double sl_pips = 25.0;
    double pip = 0.0001;

    // EMA state
    double fast_alpha;
    double slow_alpha;
    double fast_ema = 0.0;
    double slow_ema = 0.0;
    bool initialized = false;

    // Position state
    bool in_position = false;
    double entry_price = 0.0;
    std::string entry_time;

    // Results
    std::vector<TradeResult> trades;
    std::vector<int64_t> tts_measurements;
    int64_t tick_count = 0;
    int64_t signal_count = 0;

    EMAMomentumStrategy() {
        fast_alpha = 2.0 / (fast_period + 1);
        slow_alpha = 2.0 / (slow_period + 1);
    }

    int process_tick(const std::string& timestamp, double bid, double ask) {
        auto start = std::chrono::high_resolution_clock::now();

        double mid = (bid + ask) * 0.5;
        tick_count++;

        // Initialize EMAs
        if (!initialized) {
            fast_ema = mid;
            slow_ema = mid;
            initialized = true;
            auto end = std::chrono::high_resolution_clock::now();
            tts_measurements.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
            return 0;
        }

        // Store previous values
        double prev_fast = fast_ema;
        double prev_slow = slow_ema;

        // Update EMAs
        fast_ema = fast_alpha * mid + (1.0 - fast_alpha) * fast_ema;
        slow_ema = slow_alpha * mid + (1.0 - slow_alpha) * slow_ema;

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

        // Signal generation - crossover
        bool crossover_up = (prev_fast <= prev_slow) && (fast_ema > slow_ema);

        if (crossover_up) {
            signal_count++;
            in_position = true;
            entry_price = ask;
            entry_time = timestamp;
        }

        auto end = std::chrono::high_resolution_clock::now();
        tts_measurements.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
        return crossover_up ? 1 : 0;
    }

    void print_results() {
        int wins = 0, losses = 0;
        double total_pnl = 0.0, gross_profit = 0.0, gross_loss = 0.0;
        double equity = 0.0, peak = 0.0, max_dd = 0.0;
        std::vector<double> returns;

        for (const auto& t : trades) {
            total_pnl += t.pnl_pips;
            returns.push_back(t.pnl_pips);

            if (t.pnl_pips > 0) {
                wins++;
                gross_profit += t.pnl_pips;
            } else {
                losses++;
                gross_loss += std::abs(t.pnl_pips);
            }

            equity += t.pnl_pips;
            if (equity > peak) peak = equity;
            double dd = peak - equity;
            if (dd > max_dd) max_dd = dd;
        }

        double win_rate = trades.empty() ? 0 : 100.0 * wins / trades.size();
        double pf = gross_loss > 0 ? gross_profit / gross_loss : 0;

        // Sharpe ratio
        double sharpe = 0;
        if (returns.size() > 1) {
            double sum = std::accumulate(returns.begin(), returns.end(), 0.0);
            double mean = sum / returns.size();
            double sq_sum = 0;
            for (double r : returns) sq_sum += (r - mean) * (r - mean);
            double std_dev = std::sqrt(sq_sum / returns.size());
            if (std_dev > 0) sharpe = (mean / std_dev) * std::sqrt(252.0);
        }

        // TTS stats
        double avg_tts = 0, p99_tts = 0, max_tts = 0;
        if (!tts_measurements.empty()) {
            double sum = 0;
            for (auto t : tts_measurements) sum += t;
            avg_tts = sum / tts_measurements.size() / 1000.0;

            auto sorted = tts_measurements;
            std::sort(sorted.begin(), sorted.end());
            p99_tts = sorted[static_cast<size_t>(sorted.size() * 0.99)] / 1000.0;
            max_tts = sorted.back() / 1000.0;
        }

        std::cout << "\n";
        std::cout << "============================================================\n";
        std::cout << "  EMA MOMENTUM - C++ REAL DATA BACKTEST\n";
        std::cout << "============================================================\n";
        std::cout << "  Strategy: EMA(" << fast_period << "/" << slow_period << "), ";
        std::cout << "TP:" << tp_pips << " SL:" << sl_pips << "\n";
        std::cout << "\n  TRADE STATISTICS\n";
        std::cout << "  -----------------\n";
        std::cout << "  Total Trades:    " << trades.size() << "\n";
        std::cout << "  Winning:         " << wins << "\n";
        std::cout << "  Losing:          " << losses << "\n";
        std::cout << "  Win Rate:        " << std::fixed << std::setprecision(1) << win_rate << "%\n";
        std::cout << "\n  PERFORMANCE\n";
        std::cout << "  -----------\n";
        std::cout << "  Total P&L:       " << std::showpos << std::setprecision(1) << total_pnl << " pips ($" << total_pnl * 10 << ")\n";
        std::cout << std::noshowpos;
        std::cout << "  Profit Factor:   " << std::setprecision(2) << pf << "\n";
        std::cout << "  Sharpe Ratio:    " << sharpe << "\n";
        std::cout << "  Max Drawdown:    " << max_dd << " pips\n";
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
    int max_rows = 10000000;  // Default 10M

    if (argc > 1) csv_path = argv[1];
    if (argc > 2) max_rows = std::stoi(argv[2]);

    std::cout << "============================================================\n";
    std::cout << "  GEL TRADING SYSTEM - C++ EMA MOMENTUM BACKTEST\n";
    std::cout << "============================================================\n";
    std::cout << "\nData file: " << csv_path << "\n";
    std::cout << "Max rows: " << max_rows << "\n";

    std::ifstream file(csv_path);
    if (!file.is_open()) {
        std::cerr << "ERROR: Cannot open file: " << csv_path << "\n";
        return 1;
    }

    EMAMomentumStrategy strategy;

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

            if (row_count % 1000000 == 0) {
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

    if (strategy.trades.size() > 0 &&
        std::accumulate(strategy.trades.begin(), strategy.trades.end(), 0.0,
            [](double sum, const TradeResult& t) { return sum + t.pnl_pips; }) > 0) {
        std::cout << "\n[OK] BACKTEST SUCCESSFUL - Positive P&L!\n";
    } else if (strategy.trades.size() > 0) {
        std::cout << "\n[WARN] BACKTEST COMPLETE - Negative P&L\n";
    } else {
        std::cout << "\n[FAIL] BACKTEST FAILED - No trades\n";
    }

    return 0;
}
