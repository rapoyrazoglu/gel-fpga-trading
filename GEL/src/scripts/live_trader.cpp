// GEL Live Trader - OANDA Paper Trading
// Simple text-based terminal application
// Build: cmake --build build --target live_trader

#include <iostream>
#include <string>
#include <vector>
#include <deque>
#include <cmath>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <sstream>
#include <iomanip>

#ifdef _WIN32
#define CURL_STATICLIB
#endif
#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ============================================================================
// OANDA CLIENT
// ============================================================================

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

class OandaClient {
public:
    std::string api_token;
    std::string account_id;
    std::string base_url = "https://api-fxpractice.oanda.com";

    bool get_price(double& bid, double& ask) {
        std::string url = base_url + "/v3/accounts/" + account_id + "/pricing?instruments=EUR_USD";
        std::string response = http_get(url);

        if (response.empty()) return false;

        try {
            auto j = json::parse(response);
            if (j.contains("prices") && !j["prices"].empty()) {
                bid = std::stod(j["prices"][0]["bids"][0]["price"].get<std::string>());
                ask = std::stod(j["prices"][0]["asks"][0]["price"].get<std::string>());
                return true;
            }
        } catch (...) {}
        return false;
    }

    bool get_account(double& balance, double& nav, double& unrealized_pnl) {
        std::string url = base_url + "/v3/accounts/" + account_id + "/summary";
        std::string response = http_get(url);

        if (response.empty()) return false;

        try {
            auto j = json::parse(response);
            if (j.contains("account")) {
                balance = std::stod(j["account"]["balance"].get<std::string>());
                nav = std::stod(j["account"]["NAV"].get<std::string>());
                unrealized_pnl = std::stod(j["account"]["unrealizedPL"].get<std::string>());
                return true;
            }
        } catch (...) {}
        return false;
    }

    std::string place_order(int units, double sl, double tp) {
        std::string url = base_url + "/v3/accounts/" + account_id + "/orders";

        std::ostringstream sl_str, tp_str;
        sl_str << std::fixed << std::setprecision(5) << sl;
        tp_str << std::fixed << std::setprecision(5) << tp;

        json order = {
            {"order", {
                {"type", "MARKET"},
                {"instrument", "EUR_USD"},
                {"units", std::to_string(units)},
                {"stopLossOnFill", {{"price", sl_str.str()}}},
                {"takeProfitOnFill", {{"price", tp_str.str()}}}
            }}
        };

        std::string response = http_post(url, order.dump());

        try {
            auto j = json::parse(response);
            if (j.contains("orderFillTransaction")) {
                return j["orderFillTransaction"]["tradeOpened"]["tradeID"].get<std::string>();
            }
        } catch (...) {}
        return "";
    }

    bool close_trade(const std::string& trade_id) {
        std::string url = base_url + "/v3/accounts/" + account_id + "/trades/" + trade_id + "/close";
        std::string response = http_put(url);
        return !response.empty();
    }

private:
    std::string http_get(const std::string& url) {
        CURL* curl = curl_easy_init();
        if (!curl) return "";

        std::string response;
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("Authorization: Bearer " + api_token).c_str());

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

        curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        return response;
    }

    std::string http_post(const std::string& url, const std::string& body) {
        CURL* curl = curl_easy_init();
        if (!curl) return "";

        std::string response;
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("Authorization: Bearer " + api_token).c_str());
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

        curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        return response;
    }

    std::string http_put(const std::string& url) {
        CURL* curl = curl_easy_init();
        if (!curl) return "";

        std::string response;
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("Authorization: Bearer " + api_token).c_str());

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

        curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        return response;
    }
};

// ============================================================================
// STRATEGIES
// ============================================================================

struct TradeResult {
    double pnl;
    std::string reason;
};

class Strategy {
public:
    virtual ~Strategy() = default;
    virtual std::string name() const = 0;
    virtual int process_tick(double bid, double ask) = 0;  // 0=nothing, 1=buy, -1=sell, 2=close

    bool in_position = false;
    double entry_price = 0;
    double tp_pips = 50;
    double sl_pips = 25;
    double pip = 0.0001;

    std::vector<TradeResult> trades;
    int tick_count = 0;

    double get_sl() const { return entry_price - sl_pips * pip; }
    double get_tp() const { return entry_price + tp_pips * pip; }

    void get_stats(int& num_trades, double& total_pnl, double& win_rate, double& pf) {
        num_trades = trades.size();
        total_pnl = 0;
        int wins = 0;
        double gross_profit = 0, gross_loss = 0;

        for (const auto& t : trades) {
            total_pnl += t.pnl;
            if (t.pnl > 0) {
                wins++;
                gross_profit += t.pnl;
            } else {
                gross_loss += std::abs(t.pnl);
            }
        }

        win_rate = num_trades > 0 ? 100.0 * wins / num_trades : 0;
        pf = gross_loss > 0 ? gross_profit / gross_loss : 0;
    }
};

// ATR Fibonacci v12
class ATRFibonacciV12 : public Strategy {
public:
    std::string name() const override { return "ATR Fibonacci v12"; }

    int atr_period = 10;
    int rsi_period = 7;
    double rsi_oversold = 30.0;
    double vol_threshold = 0.0004;

    std::deque<double> prices;
    std::deque<double> gains, losses;
    double prev_price = 0;
    double rsi = 50;

    ATRFibonacciV12() {
        tp_pips = 75;
        sl_pips = 35;
    }

    int process_tick(double bid, double ask) override {
        tick_count++;
        double mid = (bid + ask) / 2;
        prices.push_back(mid);
        if (prices.size() > 100) prices.pop_front();

        if (prices.size() < 20) {
            prev_price = mid;
            return 0;
        }

        // RSI calculation
        if (prev_price > 0) {
            double change = mid - prev_price;
            gains.push_back(change > 0 ? change : 0);
            losses.push_back(change < 0 ? -change : 0);
            if (gains.size() > rsi_period) { gains.pop_front(); losses.pop_front(); }

            if (gains.size() >= rsi_period) {
                double avg_gain = 0, avg_loss = 0;
                for (auto g : gains) avg_gain += g;
                for (auto l : losses) avg_loss += l;
                avg_gain /= rsi_period;
                avg_loss /= rsi_period;
                if (avg_loss > 0) rsi = 100 - (100 / (1 + avg_gain / avg_loss));
            }
        }

        // Volatility
        double high = *std::max_element(prices.end() - 20, prices.end());
        double low = *std::min_element(prices.end() - 20, prices.end());
        double volatility = high - low;

        prev_price = mid;

        // Position management
        if (in_position) {
            double pnl = (bid - entry_price) / pip;
            if (pnl >= tp_pips) {
                trades.push_back({pnl, "TP"});
                in_position = false;
                return 2;  // Close
            }
            if (pnl <= -sl_pips) {
                trades.push_back({pnl, "SL"});
                in_position = false;
                return 2;  // Close
            }
            return 0;
        }

        // Entry signal
        if (rsi < rsi_oversold && volatility > vol_threshold) {
            in_position = true;
            entry_price = ask;
            return 1;  // Buy
        }

        return 0;
    }
};

// ATR Fibonacci v44
class ATRFibonacciV44 : public Strategy {
public:
    std::string name() const override { return "ATR Fibonacci v44"; }

    std::deque<double> prices;
    std::deque<double> gains, losses;
    double prev_price = 0;
    double rsi = 50;

    ATRFibonacciV44() {
        tp_pips = 85;
        sl_pips = 35;
    }

    int process_tick(double bid, double ask) override {
        tick_count++;
        double mid = (bid + ask) / 2;
        prices.push_back(mid);
        if (prices.size() > 100) prices.pop_front();

        if (prices.size() < 25) {
            prev_price = mid;
            return 0;
        }

        // RSI
        if (prev_price > 0) {
            double change = mid - prev_price;
            gains.push_back(change > 0 ? change : 0);
            losses.push_back(change < 0 ? -change : 0);
            if (gains.size() > 7) { gains.pop_front(); losses.pop_front(); }

            if (gains.size() >= 7) {
                double avg_gain = 0, avg_loss = 0;
                for (auto g : gains) avg_gain += g;
                for (auto l : losses) avg_loss += l;
                avg_gain /= 7;
                avg_loss /= 7;
                if (avg_loss > 0) rsi = 100 - (100 / (1 + avg_gain / avg_loss));
            }
        }

        double high = *std::max_element(prices.end() - 25, prices.end());
        double low = *std::min_element(prices.end() - 25, prices.end());
        double volatility = high - low;

        prev_price = mid;

        if (in_position) {
            double pnl = (bid - entry_price) / pip;
            if (pnl >= tp_pips) { trades.push_back({pnl, "TP"}); in_position = false; return 2; }
            if (pnl <= -sl_pips) { trades.push_back({pnl, "SL"}); in_position = false; return 2; }
            return 0;
        }

        if (rsi < 30 && volatility > 0.0004) {
            in_position = true;
            entry_price = ask;
            return 1;
        }

        return 0;
    }
};

// EMA Momentum 9/50
class EMAMomentum : public Strategy {
public:
    std::string name() const override { return "EMA Momentum 9/50"; }

    double fast_ema = 0, slow_ema = 0;
    double prev_fast = 0, prev_slow = 0;
    bool initialized = false;

    EMAMomentum() {
        tp_pips = 50;
        sl_pips = 25;
    }

    int process_tick(double bid, double ask) override {
        tick_count++;
        double mid = (bid + ask) / 2;

        if (!initialized) {
            fast_ema = slow_ema = mid;
            initialized = true;
            return 0;
        }

        prev_fast = fast_ema;
        prev_slow = slow_ema;

        double fast_alpha = 2.0 / 10;
        double slow_alpha = 2.0 / 51;
        fast_ema = fast_alpha * mid + (1 - fast_alpha) * fast_ema;
        slow_ema = slow_alpha * mid + (1 - slow_alpha) * slow_ema;

        if (in_position) {
            double pnl = (bid - entry_price) / pip;
            if (pnl >= tp_pips) { trades.push_back({pnl, "TP"}); in_position = false; return 2; }
            if (pnl <= -sl_pips) { trades.push_back({pnl, "SL"}); in_position = false; return 2; }
            return 0;
        }

        // Crossover
        if (prev_fast <= prev_slow && fast_ema > slow_ema) {
            in_position = true;
            entry_price = ask;
            return 1;
        }

        return 0;
    }
};

// Bollinger v66
class BollingerV66 : public Strategy {
public:
    std::string name() const override { return "Bollinger Wave v66"; }

    std::deque<double> prices;
    std::deque<double> gains, losses;
    double prev_price = 0;
    double rsi = 50;

    BollingerV66() {
        tp_pips = 75;
        sl_pips = 35;
    }

    int process_tick(double bid, double ask) override {
        tick_count++;
        double mid = (bid + ask) / 2;
        prices.push_back(mid);
        if (prices.size() > 25) prices.pop_front();

        if (prices.size() < 25) {
            prev_price = mid;
            return 0;
        }

        // Bollinger Bands
        double sum = 0;
        for (auto p : prices) sum += p;
        double sma = sum / prices.size();

        double sq_sum = 0;
        for (auto p : prices) sq_sum += (p - sma) * (p - sma);
        double std_dev = std::sqrt(sq_sum / prices.size());
        double lower_band = sma - 1.5 * std_dev;

        // RSI
        if (prev_price > 0) {
            double change = mid - prev_price;
            gains.push_back(change > 0 ? change : 0);
            losses.push_back(change < 0 ? -change : 0);
            if (gains.size() > 7) { gains.pop_front(); losses.pop_front(); }

            if (gains.size() >= 7) {
                double avg_gain = 0, avg_loss = 0;
                for (auto g : gains) avg_gain += g;
                for (auto l : losses) avg_loss += l;
                avg_gain /= 7;
                avg_loss /= 7;
                if (avg_loss > 0) rsi = 100 - (100 / (1 + avg_gain / avg_loss));
            }
        }

        prev_price = mid;

        if (in_position) {
            double pnl = (bid - entry_price) / pip;
            if (pnl >= tp_pips) { trades.push_back({pnl, "TP"}); in_position = false; return 2; }
            if (pnl <= -sl_pips) { trades.push_back({pnl, "SL"}); in_position = false; return 2; }
            return 0;
        }

        if (mid < lower_band && rsi < 25) {
            in_position = true;
            entry_price = ask;
            return 1;
        }

        return 0;
    }
};

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char* argv[]) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    std::cout << "========================================\n";
    std::cout << "  GEL LIVE TRADER - OANDA Paper Trade\n";
    std::cout << "========================================\n\n";

    // Credentials come from the environment only; nothing is compiled in.
    const char* token_env = std::getenv("OANDA_API_TOKEN");
    const char* account_env = std::getenv("OANDA_ACCOUNT_ID");
    if (!token_env || !account_env || !*token_env || !*account_env) {
        std::cerr << "ERROR: set OANDA_API_TOKEN and OANDA_ACCOUNT_ID "
                     "(practice-account credentials)\n";
        return 1;
    }

    OandaClient client;
    client.api_token = token_env;
    client.account_id = account_env;

    std::cout << "Using OANDA Demo Account: " << client.account_id << "\n\n";

    // Test connection
    double balance, nav, unrealized;
    if (!client.get_account(balance, nav, unrealized)) {
        std::cout << "ERROR: Failed to connect to OANDA\n";
        return 1;
    }
    std::cout << "Connected! Balance: $" << std::fixed << std::setprecision(2) << balance << "\n\n";

    // Strategy selection - check command line first
    int choice = 0;

    if (argc > 1) {
        choice = std::atoi(argv[1]);
        std::cout << "Strategy from command line: " << choice << "\n";
    } else {
        std::cout << "Select Strategy:\n";
        std::cout << "  1. ATR Fibonacci v12 (TP:75 SL:35)\n";
        std::cout << "  2. ATR Fibonacci v44 (TP:85 SL:35)\n";
        std::cout << "  3. EMA Momentum 9/50 (TP:50 SL:25)\n";
        std::cout << "  4. Bollinger Wave v66 (TP:75 SL:35)\n";
        std::cout << "\nChoice (1-4): ";
        std::cin >> choice;
    }

    Strategy* strategy = nullptr;
    switch (choice) {
        case 1: strategy = new ATRFibonacciV12(); break;
        case 2: strategy = new ATRFibonacciV44(); break;
        case 3: strategy = new EMAMomentum(); break;
        case 4: strategy = new BollingerV66(); break;
        default:
            std::cout << "Invalid choice\n";
            return 1;
    }

    std::cout << "\nStarting " << strategy->name() << "...\n";
    std::cout << "Press Ctrl+C to stop\n\n";

    std::string open_trade_id;
    auto start_time = std::chrono::steady_clock::now();

    // Main loop
    while (true) {
        double bid, ask;
        if (!client.get_price(bid, ask)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        int signal = strategy->process_tick(bid, ask);

        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);

        // Handle signals
        if (signal == 1) {  // Buy
            std::cout << std::put_time(std::localtime(&time), "[%H:%M:%S]");
            std::cout << " BUY @ " << std::fixed << std::setprecision(5) << strategy->entry_price;
            std::cout << " | SL:" << strategy->get_sl() << " TP:" << strategy->get_tp() << "\n";

            open_trade_id = client.place_order(1000, strategy->get_sl(), strategy->get_tp());
            if (!open_trade_id.empty()) {
                std::cout << "  -> Order filled: " << open_trade_id << "\n";
            }
        }
        else if (signal == 2) {  // Close
            const auto& last = strategy->trades.back();
            std::cout << std::put_time(std::localtime(&time), "[%H:%M:%S]");
            std::cout << " CLOSE " << last.reason;
            std::cout << " | P&L: " << std::showpos << last.pnl << std::noshowpos << " pips\n";

            if (!open_trade_id.empty()) {
                client.close_trade(open_trade_id);
                open_trade_id = "";
            }
        }

        // Print status every 10 ticks
        if (strategy->tick_count % 10 == 0) {
            int num_trades;
            double total_pnl, win_rate, pf;
            strategy->get_stats(num_trades, total_pnl, win_rate, pf);

            auto elapsed = std::chrono::steady_clock::now() - start_time;
            auto minutes = std::chrono::duration_cast<std::chrono::minutes>(elapsed).count();

            std::cout << "\r[" << minutes << "m] ";
            std::cout << "Bid:" << std::fixed << std::setprecision(5) << bid << " ";
            std::cout << "Ask:" << ask << " | ";
            std::cout << "Trades:" << num_trades << " ";
            std::cout << "P&L:" << std::showpos << std::setprecision(1) << total_pnl << std::noshowpos << " ";
            if (strategy->in_position) {
                double pnl = (bid - strategy->entry_price) / 0.0001;
                std::cout << "[LONG " << std::showpos << pnl << std::noshowpos << "]";
            }
            std::cout << "          " << std::flush;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    delete strategy;
    curl_global_cleanup();
    return 0;
}
