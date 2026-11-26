// include/gel/broker/oanda_client.hpp
#pragma once

// Disable Windows min/max macros
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "oanda_types.hpp"
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace gel::broker {

    /**
     * OandaClient - OANDA V20 REST API Client
     *
     * Features:
     * - Account management
     * - Market data (pricing, candles)
     * - Order execution
     * - Trade management
     * - Position tracking
     * - Transaction history
     *
     * Rate Limits (per IP address):
     * - REST: 120 requests/second (HTTP 429 on excess)
     * - Streaming: 20 concurrent connections
     * - Connection: Max 2 new connections/second
     */
    class OandaClient {
    public:
        struct Config {
            std::string api_token;
            std::string account_id;
            bool use_practice = true;              // Practice vs Live
            std::string practice_url = "https://api-fxpractice.oanda.com";
            std::string live_url = "https://api-fxtrade.oanda.com";
            int timeout_seconds = 10;
        };

        explicit OandaClient(Config config)
            : config_(std::move(config))
        {
            curl_global_init(CURL_GLOBAL_DEFAULT);
            base_url_ = config_.use_practice ? config_.practice_url : config_.live_url;
        }

        ~OandaClient() {
            curl_global_cleanup();
        }

        // === ACCOUNT ENDPOINTS ===

        /**
         * Get account summary (balance, NAV, P&L, etc.)
         * GET /v3/accounts/{accountID}/summary
         */
        ApiResponse<AccountSummary> get_account_summary() {
            std::string endpoint = "/v3/accounts/" + config_.account_id + "/summary";
            auto response = make_request("GET", endpoint);

            if (!response.success) {
                return {false, response.error_message, response.http_code, std::nullopt};
            }

            try {
                auto json = nlohmann::json::parse(*response.data);
                AccountSummary summary = parse_account_summary(json["account"]);
                return {true, "", 200, summary};
            } catch (const std::exception& e) {
                return {false, std::string("JSON parse error: ") + e.what(), 500, std::nullopt};
            }
        }

        /**
         * Get list of tradeable instruments
         * GET /v3/accounts/{accountID}/instruments
         */
        ApiResponse<std::vector<Instrument>> get_instruments() {
            std::string endpoint = "/v3/accounts/" + config_.account_id + "/instruments";
            auto response = make_request("GET", endpoint);

            if (!response.success) {
                return {false, response.error_message, response.http_code, std::nullopt};
            }

            try {
                auto json = nlohmann::json::parse(*response.data);
                std::vector<Instrument> instruments;
                for (const auto& item : json["instruments"]) {
                    instruments.push_back(parse_instrument(item));
                }
                return {true, "", 200, instruments};
            } catch (const std::exception& e) {
                return {false, std::string("JSON parse error: ") + e.what(), 500, std::nullopt};
            }
        }

        // === PRICING ENDPOINTS ===

        /**
         * Get current pricing for instruments
         * GET /v3/accounts/{accountID}/pricing
         */
        ApiResponse<std::vector<Price>> get_pricing(const std::vector<std::string>& instruments) {
            std::string instruments_str;
            for (size_t i = 0; i < instruments.size(); ++i) {
                instruments_str += instruments[i];
                if (i < instruments.size() - 1) instruments_str += "%2C"; // URL-encoded comma
            }

            std::string endpoint = "/v3/accounts/" + config_.account_id +
                                 "/pricing?instruments=" + instruments_str;
            auto response = make_request("GET", endpoint);

            if (!response.success) {
                return {false, response.error_message, response.http_code, std::nullopt};
            }

            try {
                auto json = nlohmann::json::parse(*response.data);
                std::vector<Price> prices;
                for (const auto& item : json["prices"]) {
                    prices.push_back(parse_price(item));
                }
                return {true, "", 200, prices};
            } catch (const std::exception& e) {
                return {false, std::string("JSON parse error: ") + e.what(), 500, std::nullopt};
            }
        }

        /**
         * Get candlestick data (OHLC)
         * GET /v3/instruments/{instrument}/candles
         *
         * @param count Number of candles (max 5000)
         */
        ApiResponse<std::vector<Candlestick>> get_candles(
            const std::string& instrument,
            CandlestickGranularity granularity,
            int count = 500
        ) {
            // Cap count at 5000 (OANDA API limit)
            const int max_count = 5000;
            int capped_count = (count > max_count) ? max_count : count;

            std::string endpoint = "/v3/instruments/" + instrument + "/candles";
            endpoint += "?granularity=" + to_string(granularity);
            endpoint += "&count=" + std::to_string(capped_count);
            endpoint += "&price=MBA"; // Mid, Bid, Ask

            auto response = make_request("GET", endpoint);

            if (!response.success) {
                return {false, response.error_message, response.http_code, std::nullopt};
            }

            try {
                auto json = nlohmann::json::parse(*response.data);
                std::vector<Candlestick> candles;
                for (const auto& item : json["candles"]) {
                    candles.push_back(parse_candlestick(item));
                }
                return {true, "", 200, candles};
            } catch (const std::exception& e) {
                return {false, std::string("JSON parse error: ") + e.what(), 500, std::nullopt};
            }
        }

        /**
         * Get candlestick data with time range
         * GET /v3/instruments/{instrument}/candles
         */
        ApiResponse<std::vector<Candlestick>> get_candles_range(
            const std::string& instrument,
            CandlestickGranularity granularity,
            time_point from,
            time_point to
        ) {
            std::string endpoint = "/v3/instruments/" + instrument + "/candles";
            endpoint += "?granularity=" + to_string(granularity);
            endpoint += "&from=" + time_to_rfc3339(from);
            endpoint += "&to=" + time_to_rfc3339(to);
            endpoint += "&price=MBA";

            auto response = make_request("GET", endpoint);

            if (!response.success) {
                return {false, response.error_message, response.http_code, std::nullopt};
            }

            try {
                auto json = nlohmann::json::parse(*response.data);
                std::vector<Candlestick> candles;
                for (const auto& item : json["candles"]) {
                    candles.push_back(parse_candlestick(item));
                }
                return {true, "", 200, candles};
            } catch (const std::exception& e) {
                return {false, std::string("JSON parse error: ") + e.what(), 500, std::nullopt};
            }
        }

        // === ORDER ENDPOINTS ===

        /**
         * Create a new order
         * POST /v3/accounts/{accountID}/orders
         */
        ApiResponse<Order> create_order(const OrderRequest& order_req) {
            std::string endpoint = "/v3/accounts/" + config_.account_id + "/orders";

            nlohmann::json order_json = build_order_json(order_req);
            nlohmann::json body = {{"order", order_json}};

            auto response = make_request("POST", endpoint, body.dump());

            if (!response.success) {
                return {false, response.error_message, response.http_code, std::nullopt};
            }

            try {
                auto json = nlohmann::json::parse(*response.data);

                // Check for order fill transaction
                if (json.contains("orderFillTransaction")) {
                    Order order = parse_order_from_fill(json["orderFillTransaction"]);
                    return {true, "", 201, order};
                } else if (json.contains("orderCreateTransaction")) {
                    Order order = parse_order_from_create(json["orderCreateTransaction"]);
                    return {true, "", 201, order};
                } else {
                    return {false, "No order transaction in response", 500, std::nullopt};
                }
            } catch (const std::exception& e) {
                return {false, std::string("JSON parse error: ") + e.what(), 500, std::nullopt};
            }
        }

        /**
         * Get list of pending orders
         * GET /v3/accounts/{accountID}/pendingOrders
         */
        ApiResponse<std::vector<Order>> get_pending_orders() {
            std::string endpoint = "/v3/accounts/" + config_.account_id + "/pendingOrders";
            auto response = make_request("GET", endpoint);

            if (!response.success) {
                return {false, response.error_message, response.http_code, std::nullopt};
            }

            try {
                auto json = nlohmann::json::parse(*response.data);
                std::vector<Order> orders;
                for (const auto& item : json["orders"]) {
                    orders.push_back(parse_order(item));
                }
                return {true, "", 200, orders};
            } catch (const std::exception& e) {
                return {false, std::string("JSON parse error: ") + e.what(), 500, std::nullopt};
            }
        }

        /**
         * Cancel an order
         * PUT /v3/accounts/{accountID}/orders/{orderID}/cancel
         */
        ApiResponse<bool> cancel_order(const std::string& order_id) {
            std::string endpoint = "/v3/accounts/" + config_.account_id + "/orders/" + order_id + "/cancel";
            auto response = make_request("PUT", endpoint);

            return {response.success, response.error_message, response.http_code, response.success};
        }

        // === TRADE ENDPOINTS ===

        /**
         * Get list of open trades
         * GET /v3/accounts/{accountID}/openTrades
         */
        ApiResponse<std::vector<Trade>> get_open_trades() {
            std::string endpoint = "/v3/accounts/" + config_.account_id + "/openTrades";
            auto response = make_request("GET", endpoint);

            if (!response.success) {
                return {false, response.error_message, response.http_code, std::nullopt};
            }

            try {
                auto json = nlohmann::json::parse(*response.data);
                std::vector<Trade> trades;
                for (const auto& item : json["trades"]) {
                    trades.push_back(parse_trade(item));
                }
                return {true, "", 200, trades};
            } catch (const std::exception& e) {
                return {false, std::string("JSON parse error: ") + e.what(), 500, std::nullopt};
            }
        }

        /**
         * Close a trade
         * PUT /v3/accounts/{accountID}/trades/{tradeID}/close
         */
        ApiResponse<bool> close_trade(const std::string& trade_id, std::optional<double> units = std::nullopt) {
            std::string endpoint = "/v3/accounts/" + config_.account_id + "/trades/" + trade_id + "/close";

            std::string body;
            if (units) {
                nlohmann::json json_body = {{"units", std::to_string(*units)}};
                body = json_body.dump();
            }

            auto response = make_request("PUT", endpoint, body);
            return {response.success, response.error_message, response.http_code, response.success};
        }

        /**
         * Modify trade (update SL/TP)
         * PUT /v3/accounts/{accountID}/trades/{tradeID}/orders
         */
        ApiResponse<bool> modify_trade(
            const std::string& trade_id,
            std::optional<double> stop_loss,
            std::optional<double> take_profit,
            std::optional<double> trailing_stop_distance
        ) {
            std::string endpoint = "/v3/accounts/" + config_.account_id + "/trades/" + trade_id + "/orders";

            nlohmann::json body;

            if (stop_loss) {
                body["stopLoss"] = {{"price", std::to_string(*stop_loss)}};
            }
            if (take_profit) {
                body["takeProfit"] = {{"price", std::to_string(*take_profit)}};
            }
            if (trailing_stop_distance) {
                body["trailingStopLoss"] = {{"distance", std::to_string(*trailing_stop_distance)}};
            }

            auto response = make_request("PUT", endpoint, body.dump());
            return {response.success, response.error_message, response.http_code, response.success};
        }

        // === POSITION ENDPOINTS ===

        /**
         * Get list of open positions
         * GET /v3/accounts/{accountID}/openPositions
         */
        ApiResponse<std::vector<Position>> get_open_positions() {
            std::string endpoint = "/v3/accounts/" + config_.account_id + "/openPositions";
            auto response = make_request("GET", endpoint);

            if (!response.success) {
                return {false, response.error_message, response.http_code, std::nullopt};
            }

            try {
                auto json = nlohmann::json::parse(*response.data);
                std::vector<Position> positions;
                for (const auto& item : json["positions"]) {
                    positions.push_back(parse_position(item));
                }
                return {true, "", 200, positions};
            } catch (const std::exception& e) {
                return {false, std::string("JSON parse error: ") + e.what(), 500, std::nullopt};
            }
        }

        /**
         * Close position (all trades for instrument)
         * PUT /v3/accounts/{accountID}/positions/{instrument}/close
         */
        ApiResponse<bool> close_position(const std::string& instrument) {
            std::string endpoint = "/v3/accounts/" + config_.account_id + "/positions/" + instrument + "/close";
            auto response = make_request("PUT", endpoint);
            return {response.success, response.error_message, response.http_code, response.success};
        }

    private:
        Config config_;
        std::string base_url_;

        // HTTP request helper
        struct HttpResponse {
            bool success;
            std::string error_message;
            int http_code;
            std::optional<std::string> data;
        };

        static size_t write_callback(void* contents, size_t size, size_t nmemb, std::string* userp) {
            userp->append((char*)contents, size * nmemb);
            return size * nmemb;
        }

        HttpResponse make_request(const std::string& method, const std::string& endpoint, const std::string& body = "") {
            CURL* curl = curl_easy_init();
            if (!curl) {
                return {false, "Failed to initialize CURL", 0, std::nullopt};
            }

            std::string url = base_url_ + endpoint;
            std::string response_data;
            struct curl_slist* headers = nullptr;

            // Set headers
            headers = curl_slist_append(headers, ("Authorization: Bearer " + config_.api_token).c_str());
            headers = curl_slist_append(headers, "Content-Type: application/json");
            headers = curl_slist_append(headers, "Accept-Datetime-Format: RFC3339");

            // Set CURL options
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, config_.timeout_seconds);

            // Set method and body
            if (method == "POST") {
                curl_easy_setopt(curl, CURLOPT_POST, 1L);
                if (!body.empty()) {
                    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
                }
            } else if (method == "PUT") {
                curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
                if (!body.empty()) {
                    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
                }
            }

            // Perform request
            CURLcode res = curl_easy_perform(curl);
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

            // Cleanup
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);

            if (res != CURLE_OK) {
                return {false, curl_easy_strerror(res), static_cast<int>(http_code), std::nullopt};
            }

            if (http_code >= 200 && http_code < 300) {
                return {true, "", static_cast<int>(http_code), response_data};
            } else {
                return {false, "HTTP " + std::to_string(http_code), static_cast<int>(http_code), response_data};
            }
        }

        // JSON parsers
        AccountSummary parse_account_summary(const nlohmann::json& json);
        Instrument parse_instrument(const nlohmann::json& json);
        Price parse_price(const nlohmann::json& json);
        Candlestick parse_candlestick(const nlohmann::json& json);
        Order parse_order(const nlohmann::json& json);
        Order parse_order_from_fill(const nlohmann::json& json);
        Order parse_order_from_create(const nlohmann::json& json);
        Trade parse_trade(const nlohmann::json& json);
        Position parse_position(const nlohmann::json& json);

        // Builders
        nlohmann::json build_order_json(const OrderRequest& order);

        // Helpers
        std::string time_to_rfc3339(time_point tp);
        time_point rfc3339_to_time(const std::string& rfc3339);
    };

} // namespace gel::broker
