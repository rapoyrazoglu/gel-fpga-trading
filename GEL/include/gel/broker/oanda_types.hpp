// include/gel/broker/oanda_types.hpp
#pragma once

#include <string>
#include <optional>
#include <chrono>
#include <vector>

namespace gel::broker {

    /**
     * OANDA V20 REST API Types
     * Based on: https://developer.oanda.com/rest-live-v20/
     */

    // Time types
    using time_point = std::chrono::system_clock::time_point;

    // Account Types
    struct AccountSummary {
        std::string id;
        std::string alias;
        std::string currency;
        double balance;
        double nav;                    // Net Asset Value
        double unrealized_pl;
        double realized_pl;
        double margin_used;
        double margin_available;
        double position_value;
        int open_trade_count;
        int open_position_count;
        int pending_order_count;
        time_point created_time;
    };

    // Instrument Types
    struct Instrument {
        std::string name;              // e.g., "EUR_USD"
        std::string type;              // "CURRENCY", "CFD", "METAL"
        std::string display_name;
        int pip_location;              // -4 for EUR/USD (0.0001)
        int display_precision;
        int trade_units_precision;
        double minimum_trade_size;
        double maximum_trailing_stop_distance;
        double minimum_trailing_stop_distance;
        double maximum_position_size;
        double maximum_order_units;
        double margin_rate;
    };

    // Pricing Types
    struct PriceBucket {
        double price;
        int liquidity;
    };

    struct Price {
        std::string instrument;
        time_point time;
        double bid;
        double ask;
        double mid;
        std::vector<PriceBucket> bids;
        std::vector<PriceBucket> asks;
        bool tradeable;
    };

    // Candlestick Types
    enum class CandlestickGranularity {
        S5,   S10,  S15,  S30,         // Seconds
        M1,   M2,   M3,   M4,   M5,     // Minutes
        M10,  M15,  M30,                // Minutes
        H1,   H2,   H3,   H4,           // Hours
        H6,   H8,   H12,                // Hours
        D,                              // Day
        W,                              // Week
        M                               // Month
    };

    inline std::string to_string(CandlestickGranularity granularity) {
        switch (granularity) {
            case CandlestickGranularity::S5:  return "S5";
            case CandlestickGranularity::S10: return "S10";
            case CandlestickGranularity::S15: return "S15";
            case CandlestickGranularity::S30: return "S30";
            case CandlestickGranularity::M1:  return "M1";
            case CandlestickGranularity::M2:  return "M2";
            case CandlestickGranularity::M3:  return "M3";
            case CandlestickGranularity::M4:  return "M4";
            case CandlestickGranularity::M5:  return "M5";
            case CandlestickGranularity::M10: return "M10";
            case CandlestickGranularity::M15: return "M15";
            case CandlestickGranularity::M30: return "M30";
            case CandlestickGranularity::H1:  return "H1";
            case CandlestickGranularity::H2:  return "H2";
            case CandlestickGranularity::H3:  return "H3";
            case CandlestickGranularity::H4:  return "H4";
            case CandlestickGranularity::H6:  return "H6";
            case CandlestickGranularity::H8:  return "H8";
            case CandlestickGranularity::H12: return "H12";
            case CandlestickGranularity::D:   return "D";
            case CandlestickGranularity::W:   return "W";
            case CandlestickGranularity::M:   return "M";
            default: return "M5";
        }
    }

    struct CandlestickData {
        double o;  // Open
        double h;  // High
        double l;  // Low
        double c;  // Close
    };

    struct Candlestick {
        time_point time;
        CandlestickData bid;
        CandlestickData ask;
        CandlestickData mid;
        int volume;
        bool complete;
    };

    // Order Types
    enum class OrderType {
        MARKET,
        LIMIT,
        STOP,
        MARKET_IF_TOUCHED,
        TAKE_PROFIT,
        STOP_LOSS,
        TRAILING_STOP_LOSS
    };

    enum class TimeInForce {
        FOK,  // Fill or Kill
        IOC,  // Immediate or Cancel
        GTC,  // Good til Cancelled
        GTD,  // Good til Date
        DAY
    };

    enum class OrderState {
        PENDING,
        FILLED,
        TRIGGERED,
        CANCELLED
    };

    struct OrderRequest {
        OrderType type;
        std::string instrument;
        double units;                           // Positive = LONG, Negative = SHORT
        TimeInForce time_in_force = TimeInForce::FOK;
        std::optional<double> price;            // For LIMIT, STOP, MIT
        std::optional<double> price_bound;      // Max slippage protection
        std::optional<double> stop_loss_price;
        std::optional<double> take_profit_price;
        std::optional<double> trailing_stop_distance;
        std::optional<time_point> gtd_time;
    };

    struct Order {
        std::string id;
        time_point create_time;
        OrderState state;
        OrderType type;
        std::string instrument;
        double units;
        TimeInForce time_in_force;
        std::optional<double> price;
        std::optional<double> stop_loss_price;
        std::optional<double> take_profit_price;
        std::optional<double> filled_price;
        std::optional<time_point> filled_time;
        std::optional<std::string> trade_id;
    };

    // Trade Types
    struct Trade {
        std::string id;
        std::string instrument;
        double price;
        double units;                  // Positive = LONG, Negative = SHORT
        time_point open_time;
        OrderState state;
        double initial_units;
        double current_units;
        double realized_pl;
        double unrealized_pl;
        std::optional<double> stop_loss_price;
        std::optional<double> take_profit_price;
        std::optional<double> trailing_stop_distance;
    };

    // Position Types
    struct Position {
        std::string instrument;
        double long_units;
        double short_units;
        double long_unrealized_pl;
        double short_unrealized_pl;
        double pl;
        double unrealized_pl;
        double margin_used;
    };

    // Transaction Types
    enum class TransactionType {
        CREATE,
        CLOSE,
        REOPEN,
        CLIENT_CONFIGURE,
        CLIENT_CONFIGURE_REJECT,
        TRANSFER_FUNDS,
        TRANSFER_FUNDS_REJECT,
        MARKET_ORDER,
        MARKET_ORDER_REJECT,
        LIMIT_ORDER,
        LIMIT_ORDER_REJECT,
        STOP_ORDER,
        STOP_ORDER_REJECT,
        MARKET_IF_TOUCHED_ORDER,
        MARKET_IF_TOUCHED_ORDER_REJECT,
        TAKE_PROFIT_ORDER,
        TAKE_PROFIT_ORDER_REJECT,
        STOP_LOSS_ORDER,
        STOP_LOSS_ORDER_REJECT,
        TRAILING_STOP_LOSS_ORDER,
        TRAILING_STOP_LOSS_ORDER_REJECT,
        ORDER_FILL,
        ORDER_CANCEL,
        ORDER_CANCEL_REJECT,
        ORDER_CLIENT_EXTENSIONS_MODIFY,
        ORDER_CLIENT_EXTENSIONS_MODIFY_REJECT,
        TRADE_CLIENT_EXTENSIONS_MODIFY,
        TRADE_CLIENT_EXTENSIONS_MODIFY_REJECT,
        MARGIN_CALL_ENTER,
        MARGIN_CALL_EXTEND,
        MARGIN_CALL_EXIT,
        DELAYED_TRADE_CLOSURE,
        DAILY_FINANCING,
        RESET_RESETTABLE_PL
    };

    struct Transaction {
        std::string id;
        time_point time;
        std::string account_id;
        TransactionType type;
        std::optional<std::string> instrument;
        std::optional<double> units;
        std::optional<double> price;
        std::optional<double> pl;
        std::optional<std::string> reason;
    };

    // API Response wrapper
    template<typename T>
    struct ApiResponse {
        bool success;
        std::string error_message;
        int http_code;
        std::optional<T> data;
    };

} // namespace gel::broker
