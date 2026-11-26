// src/live_trading_main.cpp
#include "gel/broker/live_trading_manager.hpp"
#include <iostream>
#include <csignal>
#include <atomic>

using namespace gel::broker;

// Global manager for signal handling
std::unique_ptr<LiveTradingManager> g_manager;
std::atomic<bool> g_shutdown_requested{false};

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\n\nShutdown requested...\n";
        g_shutdown_requested = true;
        if (g_manager) {
            g_manager->stop();
        }
    }
}

int main(int argc, char** argv) {
    std::cout << "=== GEL Live Trading System ===\n";
    std::cout << "OANDA V20 REST API Integration\n\n";

    // Check for API token
    const char* api_token = std::getenv("OANDA_API_TOKEN");
    const char* account_id = std::getenv("OANDA_ACCOUNT_ID");

    if (!api_token || !account_id) {
        std::cerr << "ERROR: Missing OANDA credentials!\n\n";
        std::cerr << "Please set environment variables:\n";
        std::cerr << "  export OANDA_API_TOKEN=\"your-token-here\"\n";
        std::cerr << "  export OANDA_ACCOUNT_ID=\"your-account-id\"\n\n";
        std::cerr << "Get your credentials from:\n";
        std::cerr << "  Practice: https://www.oanda.com/demo-account/tpa/personal_token\n";
        std::cerr << "  Live: https://www.oanda.com/account/tpa/personal_token\n";
        return 1;
    }

    // Setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Configure live trading
    LiveTradingManager::Config config;
    config.api_token = api_token;
    config.account_id = account_id;
    config.use_practice = true;  // IMPORTANT: Set to false for live trading!
    config.instruments = {"EUR_USD"};

    // Strategy config (Jim Simons preset)
    config.signal_config.wait_for_retest = true;
    config.signal_config.stop_loss_pips = 10.0;
    config.signal_config.take_profit_pips = 15.0;  // 1.5R
    config.signal_config.min_fvg_size_pips = 3.0;
    config.signal_config.max_fvg_size_pips = 30.0;
    config.signal_config.retest_tolerance_pips = 2.0;
    config.signal_config.max_bars_to_wait = 20;
    config.signal_config.pip_size = 0.0001;

    // Execution config
    config.execution_config.default_position_size = 1000.0;  // 1000 units = 0.01 lot
    config.execution_config.slippage_pips = 0.5;
    config.execution_config.pip_size = 0.0001;
    config.execution_config.max_concurrent_positions = 3;
    config.execution_config.allow_multiple_same_direction = false;

    // Risk management
    config.max_daily_loss = 100.0;                   // Max $100 daily loss
    config.max_position_size = 10000.0;              // Max 10,000 units = 0.1 lot
    config.max_concurrent_positions = 3;

    // Monitoring
    config.verbose = true;
    config.enable_monitor = true;         // Bloomberg-style real-time monitor
    config.update_interval_ms = 100;      // Position check interval
    config.monitor_refresh_ms = 1000;     // Dashboard refresh: every 1 second

    std::cout << "Configuration:\n";
    std::cout << "  Mode: " << (config.use_practice ? "PRACTICE" : "LIVE") << "\n";
    std::cout << "  Instruments: EUR_USD\n";
    std::cout << "  Stop Loss: " << config.signal_config.stop_loss_pips << " pips\n";
    std::cout << "  Take Profit: " << config.signal_config.take_profit_pips << " pips\n";
    std::cout << "  Max Daily Loss: $" << config.max_daily_loss << "\n";
    std::cout << "  Position Size: " << config.max_position_size << " units\n\n";

    if (!config.use_practice) {
        std::cout << "WARNING: LIVE TRADING MODE ENABLED!\n";
        std::cout << "Are you sure you want to continue? (yes/no): ";
        std::string response;
        std::cin >> response;
        if (response != "yes") {
            std::cout << "Aborted.\n";
            return 0;
        }
    }

    try {
        // Create manager
        g_manager = std::make_unique<LiveTradingManager>(config);

        // Start trading
        g_manager->start();

        // Main loop - wait for shutdown signal
        // Dashboard is automatically rendered by monitor thread
        while (g_manager->is_running() && !g_shutdown_requested) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        // Stop trading
        if (g_manager->is_running()) {
            g_manager->stop();
        }

    } catch (const std::exception& e) {
        std::cerr << "\nFATAL ERROR: " << e.what() << "\n";
        return 1;
    }

    std::cout << "\nGoodbye!\n";
    return 0;
}
