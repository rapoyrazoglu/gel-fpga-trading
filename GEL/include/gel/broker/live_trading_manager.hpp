// include/gel/broker/live_trading_manager.hpp
#pragma once

#include "oanda_client.hpp"
#include "oanda_stream.hpp"
#include "gel/signals/signal_generator.hpp"
#include "gel/execution/trade_executor.hpp"
#include "gel/exit/exit_manager.hpp"
#include "gel/patterns/fair_value_gap.hpp"
#include "gel/monitoring/live_monitor.hpp"
#include <memory>
#include <unordered_map>
#include <mutex>
#include <iostream>
#include <thread>

namespace gel::broker {

    /**
     * LiveTradingManager - Orchestrates live trading with OANDA
     *
     * Responsibilities:
     * - Real-time price streaming
     * - FVG pattern detection on live data
     * - Signal generation
     * - Order execution
     * - Position management (SL/TP updates)
     * - Exit monitoring
     *
     * Architecture:
     * - Streaming thread receives price updates
     * - Main thread processes signals and executes orders
     * - Exit manager monitors open positions
     */
    class LiveTradingManager {
    public:
        struct Config {
            // OANDA API
            std::string api_token;
            std::string account_id;
            bool use_practice = true;

            // Trading instruments
            std::vector<std::string> instruments = {"EUR_USD"};

            // Strategy config
            gel::signals::SignalConfig signal_config;
            gel::execution::ExecutionConfig execution_config;

            // Risk management
            double max_daily_loss = 1000.0;         // Max daily loss in USD
            double max_position_size = 10000.0;     // Max units per position
            int max_concurrent_positions = 3;

            // Monitoring
            bool verbose = true;
            bool enable_monitor = true;              // Enable Bloomberg-style monitor
            int update_interval_ms = 100;            // Position update interval
            int monitor_refresh_ms = 1000;           // Monitor dashboard refresh (1 sec)
        };

        explicit LiveTradingManager(Config config)
            : config_(std::move(config))
        {
            // Initialize OANDA client
            OandaClient::Config client_config;
            client_config.api_token = config_.api_token;
            client_config.account_id = config_.account_id;
            client_config.use_practice = config_.use_practice;
            client_ = std::make_shared<OandaClient>(client_config);

            // Initialize streaming
            OandaStream::Config stream_config;
            stream_config.api_token = config_.api_token;
            stream_config.account_id = config_.account_id;
            stream_config.use_practice = config_.use_practice;
            stream_ = std::make_shared<OandaStream>(stream_config);

            // Initialize strategy components
            signal_generator_ = std::make_shared<gel::signals::SignalGenerator>(config_.signal_config);

            auto exit_mgr = gel::exit::ExitManagerBuilder()
                .with_jim_simons_preset(config_.signal_config.stop_loss_pips)
                .build();
            exit_manager_ = std::shared_ptr<gel::exit::ExitManager>(std::move(exit_mgr));

            config_.execution_config.max_concurrent_positions = config_.max_concurrent_positions;
            trade_executor_ = std::make_shared<gel::execution::TradeExecutor>(
                config_.execution_config,
                exit_manager_
            );

            // Initialize FVG detector
            fvg_detector_ = std::make_shared<gel::patterns::FairValueGapDetector>();

            // Initialize live monitor
            if (config_.enable_monitor) {
                monitoring::LiveMonitor::Config monitor_config;
                monitor_config.update_interval_ms = config_.monitor_refresh_ms;
                monitor_config.enable_terminal_ui = true;
                monitor_config.color_output = true;
                monitor_config.save_to_file = true;
                monitor_config.log_file = "trading_monitor.log";
                monitor_ = std::make_shared<monitoring::LiveMonitor>(monitor_config);
            }
        }

        ~LiveTradingManager() {
            stop();
        }

        /**
         * Start live trading
         */
        void start() {
            if (running_) {
                return;
            }

            if (config_.verbose) {
                std::cout << "=== GEL Live Trading Manager ===\n";
                std::cout << "Account: " << config_.account_id << "\n";
                std::cout << "Mode: " << (config_.use_practice ? "PRACTICE" : "LIVE") << "\n";
                std::cout << "Instruments: ";
                for (const auto& inst : config_.instruments) {
                    std::cout << inst << " ";
                }
                std::cout << "\n";
            }

            // Track API request
            if (monitor_) monitor_->on_api_request("/v3/accounts/summary");

            // Get initial account state
            auto account = client_->get_account_summary();
            if (account.success) {
                initial_balance_ = account.data->balance;
                daily_pnl_ = 0.0;

                // Initialize monitor with initial balance
                if (monitor_) {
                    monitor_->set_initial_balance(initial_balance_);
                    monitor_->on_balance_update(
                        initial_balance_,
                        0.0,  // unrealized P&L
                        0.0,  // realized P&L
                        0     // open positions
                    );
                }

                if (config_.verbose) {
                    std::cout << "Initial Balance: $" << initial_balance_ << "\n";
                    std::cout << "Max Daily Loss: $" << config_.max_daily_loss << "\n\n";
                }
            } else {
                if (monitor_) monitor_->on_api_error(account.error_message);
                std::cerr << "Failed to get account summary: " << account.error_message << "\n";
                return;
            }

            running_ = true;

            // Start price streaming
            stream_->start_pricing_stream(
                config_.instruments,
                [this](const Price& price) { on_price_update(price); },
                [this]() { on_heartbeat(); }
            );

            // Start transaction streaming
            stream_->start_transaction_stream(
                [this](const Transaction& txn) { on_transaction(txn); }
            );

            // Start monitoring thread
            monitor_thread_ = std::thread([this]() { monitor_positions(); });

            if (config_.verbose) {
                std::cout << "Live trading started. Press Ctrl+C to stop.\n\n";
            }
        }

        /**
         * Stop live trading
         */
        void stop() {
            if (!running_) {
                return;
            }

            running_ = false;

            // Stop streaming
            stream_->stop_pricing_stream();
            stream_->stop_transaction_stream();

            // Wait for monitor thread
            if (monitor_thread_.joinable()) {
                monitor_thread_.join();
            }

            // Close all positions
            close_all_positions();

            if (config_.verbose) {
                std::cout << "\nLive trading stopped.\n";
                print_summary();
            }
        }

        /**
         * Check if trading is active
         */
        [[nodiscard]] bool is_running() const {
            return running_;
        }

        /**
         * Get current P&L
         */
        [[nodiscard]] double get_daily_pnl() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return daily_pnl_;
        }

        /**
         * Get open position count
         */
        [[nodiscard]] int get_open_position_count() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return oanda_positions_.size();
        }

    private:
        Config config_;
        std::shared_ptr<OandaClient> client_;
        std::shared_ptr<OandaStream> stream_;

        // Strategy components
        std::shared_ptr<gel::signals::SignalGenerator> signal_generator_;
        std::shared_ptr<gel::execution::TradeExecutor> trade_executor_;
        std::shared_ptr<gel::exit::ExitManager> exit_manager_;
        std::shared_ptr<gel::patterns::FairValueGapDetector> fvg_detector_;

        // Monitoring
        std::shared_ptr<monitoring::LiveMonitor> monitor_;

        // State
        std::atomic<bool> running_{false};
        std::thread monitor_thread_;
        mutable std::mutex mutex_;

        // Account state
        double initial_balance_ = 0.0;
        double daily_pnl_ = 0.0;
        int total_trades_ = 0;

        // Price data (for FVG detection)
        std::unordered_map<std::string, std::vector<gel::data::OHLCBar>> ohlc_buffers_;
        std::unordered_map<std::string, Price> latest_prices_;

        // Position tracking (OANDA trade ID -> internal position)
        std::unordered_map<std::string, Trade> oanda_positions_;

        /**
         * Price update callback (from stream)
         */
        void on_price_update(const Price& price) {
            std::lock_guard<std::mutex> lock(mutex_);

            // Track tick received (for latency monitoring)
            if (monitor_) {
                monitor_->on_tick_received(price.instrument, price.mid);
            }

            // Store latest price
            latest_prices_[price.instrument] = price;

            // Convert to OHLC bar (simplified - in production, aggregate by timeframe)
            // For now, treat each price as a separate bar
            gel::data::OHLCBar bar;
            bar.timestamp = price.time;
            bar.open = price.mid;
            bar.high = price.mid;
            bar.low = price.mid;
            bar.close = price.mid;
            bar.volume = 1.0;

            // Add to buffer
            auto& buffer = ohlc_buffers_[price.instrument];
            buffer.push_back(bar);

            // Keep only last 100 bars
            if (buffer.size() > 100) {
                buffer.erase(buffer.begin());
            }

            // Detect FVG patterns
            if (buffer.size() >= 3) {
                auto fvgs = detect_fvg_patterns(buffer);

                // Generate signals
                auto signals = signal_generator_->process(bar, fvgs);

                // Track signal generation
                if (!signals.empty() && monitor_) {
                    monitor_->on_signal_generated(signals[0].reason);
                }

                // Execute signals
                for (const auto& signal : signals) {
                    execute_signal(signal, price);
                }
            }
        }

        /**
         * Transaction callback (from stream)
         */
        void on_transaction(const Transaction& txn) {
            std::lock_guard<std::mutex> lock(mutex_);

            if (txn.type == TransactionType::ORDER_FILL) {
                if (config_.verbose) {
                    std::cout << "[FILL] " << *txn.instrument
                              << " @ " << *txn.price
                              << " (" << *txn.units << " units)\n";
                }
                total_trades_++;

                // Track order fill (for latency monitoring)
                if (monitor_ && txn.price && txn.pl) {
                    monitor_->on_order_filled(*txn.price, *txn.pl);
                }
            } else if (txn.type == TransactionType::ORDER_CANCEL) {
                if (config_.verbose) {
                    std::cout << "[CANCEL] Order cancelled: " << txn.id << "\n";
                }
            }

            if (txn.pl && *txn.pl != 0.0) {
                daily_pnl_ += *txn.pl;

                if (config_.verbose) {
                    std::cout << "[P&L] Trade closed: $" << *txn.pl
                              << " | Daily P&L: $" << daily_pnl_ << "\n";
                }

                // Update monitor balance
                if (monitor_) {
                    monitor_->on_balance_update(
                        initial_balance_ + daily_pnl_,
                        0.0,  // TODO: Get actual unrealized P&L
                        daily_pnl_,
                        oanda_positions_.size()
                    );
                }

                // Check daily loss limit
                if (daily_pnl_ < -config_.max_daily_loss) {
                    std::cerr << "DAILY LOSS LIMIT REACHED! Stopping trading.\n";
                    stop();
                }
            }
        }

        /**
         * Heartbeat callback
         */
        void on_heartbeat() {
            // Keep-alive signal (sent every ~5 seconds)
            // Can be used to detect connection issues
        }

        /**
         * Monitor positions periodically
         */
        void monitor_positions() {
            auto last_dashboard_update = std::chrono::system_clock::now();

            while (running_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(config_.update_interval_ms));

                std::lock_guard<std::mutex> lock(mutex_);

                // Track API request
                if (monitor_) monitor_->on_api_request("/v3/accounts/openPositions");

                // Get open positions from OANDA
                auto positions = client_->get_open_positions();
                if (!positions.success) {
                    if (monitor_) monitor_->on_api_error(positions.error_message);
                    continue;
                }

                // Update local position tracking
                for (const auto& pos : *positions.data) {
                    // Check if we should exit based on our exit rules
                    // (OANDA already handles SL/TP, but we can add custom logic here)
                }

                // Render dashboard at configured interval
                auto now = std::chrono::system_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_dashboard_update
                );

                if (monitor_ && elapsed.count() >= config_.monitor_refresh_ms) {
                    monitor_->render_dashboard();
                    last_dashboard_update = now;
                }
            }
        }

        /**
         * Detect FVG patterns from OHLC buffer
         */
        std::vector<gel::patterns::FairValueGapDetector::Gap> detect_fvg_patterns(
            const std::vector<gel::data::OHLCBar>& bars
        ) {
            if (bars.size() < 3) {
                return {};
            }

            std::vector<gel::patterns::FairValueGapDetector::Gap> fvgs;

            // Feed last 3 bars to FVG detector
            fvg_detector_->reset();
            size_t start = bars.size() >= 3 ? bars.size() - 3 : 0;
            for (size_t i = start; i < bars.size(); ++i) {
                fvg_detector_->add_ohlc(bars[i]);
            }

            // Check for FVG
            auto fvg = fvg_detector_->detect_gap();
            if (fvg) {
                fvgs.push_back(*fvg);
            }

            return fvgs;
        }

        /**
         * Execute trading signal
         */
        void execute_signal(const gel::signals::TradingSignal& signal, const Price& current_price) {
            // Create OANDA order
            OrderRequest order;
            order.type = OrderType::MARKET;
            order.instrument = config_.instruments[0];  // TODO: map from signal
            order.units = signal.is_long() ? config_.max_position_size : -config_.max_position_size;
            order.time_in_force = TimeInForce::FOK;

            // Set SL/TP
            if (signal.stop_loss) {
                order.stop_loss_price = *signal.stop_loss;
            }
            if (signal.take_profit) {
                order.take_profit_price = *signal.take_profit;
            }

            // Price bound for slippage protection
            if (signal.is_long()) {
                order.price_bound = current_price.ask * 1.001;  // Max 0.1% slippage
            } else {
                order.price_bound = current_price.bid * 0.999;
            }

            // Track API request
            if (monitor_) monitor_->on_api_request("/v3/accounts/orders");

            // Execute
            auto result = client_->create_order(order);
            if (result.success) {
                // Track order placed
                if (monitor_) {
                    monitor_->on_order_placed(result.data->id);
                }

                if (config_.verbose) {
                    std::cout << "[ORDER] " << signal.reason
                              << " | " << (signal.is_long() ? "BUY" : "SELL")
                              << " @ " << current_price.mid
                              << " | SL: " << order.stop_loss_price.value_or(0.0)
                              << " | TP: " << order.take_profit_price.value_or(0.0) << "\n";
                }
            } else {
                if (monitor_) monitor_->on_api_error(result.error_message);
                std::cerr << "[ERROR] Order failed: " << result.error_message << "\n";
            }
        }

        /**
         * Close all open positions
         */
        void close_all_positions() {
            auto positions = client_->get_open_positions();
            if (positions.success) {
                for (const auto& pos : *positions.data) {
                    client_->close_position(pos.instrument);
                }
            }
        }

        /**
         * Print trading summary
         */
        void print_summary() {
            std::cout << "\n=== TRADING SUMMARY ===\n";
            std::cout << "Initial Balance: $" << initial_balance_ << "\n";
            std::cout << "Daily P&L: $" << daily_pnl_ << "\n";
            std::cout << "Total Trades: " << total_trades_ << "\n";
            std::cout << "Return: " << ((daily_pnl_ / initial_balance_) * 100.0) << "%\n";
        }
    };

} // namespace gel::broker
