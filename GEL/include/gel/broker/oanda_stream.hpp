// include/gel/broker/oanda_stream.hpp
#pragma once

#include "oanda_types.hpp"
#include <functional>
#include <thread>
#include <atomic>
#include <curl/curl.h>

namespace gel::broker {

    /**
     * OandaStream - Real-time streaming for prices and events
     *
     * OANDA V20 API supports:
     * - Pricing stream (real-time bid/ask updates)
     * - Transaction stream (account events, fills, cancellations)
     *
     * Limit: 20 concurrent streaming connections per account
     */
    class OandaStream {
    public:
        using PriceCallback = std::function<void(const Price&)>;
        using TransactionCallback = std::function<void(const Transaction&)>;
        using HeartbeatCallback = std::function<void()>;

        struct Config {
            std::string api_token;
            std::string account_id;
            bool use_practice = true;
            std::string practice_url = "https://stream-fxpractice.oanda.com";
            std::string live_url = "https://stream-fxtrade.oanda.com";
        };

        explicit OandaStream(Config config)
            : config_(std::move(config))
        {
            base_url_ = config_.use_practice ? config_.practice_url : config_.live_url;
        }

        ~OandaStream() {
            stop_pricing_stream();
            stop_transaction_stream();
        }

        /**
         * Start streaming real-time pricing
         * @param instruments List of instruments to stream (e.g., {"EUR_USD", "GBP_USD"})
         * @param callback Called when new price update arrives
         */
        void start_pricing_stream(
            const std::vector<std::string>& instruments,
            PriceCallback price_callback,
            HeartbeatCallback heartbeat_callback = nullptr
        ) {
            if (pricing_stream_running_) {
                return;
            }

            pricing_stream_running_ = true;
            price_callback_ = std::move(price_callback);
            heartbeat_callback_ = std::move(heartbeat_callback);

            // Build instruments string
            std::string instruments_str;
            for (size_t i = 0; i < instruments.size(); ++i) {
                instruments_str += instruments[i];
                if (i < instruments.size() - 1) instruments_str += "%2C";
            }

            std::string endpoint = "/v3/accounts/" + config_.account_id +
                                 "/pricing/stream?instruments=" + instruments_str;

            pricing_thread_ = std::thread([this, endpoint]() {
                stream_worker(endpoint, StreamType::PRICING);
            });
        }

        /**
         * Start streaming account transactions
         * @param callback Called when new transaction occurs
         */
        void start_transaction_stream(
            TransactionCallback transaction_callback,
            HeartbeatCallback heartbeat_callback = nullptr
        ) {
            if (transaction_stream_running_) {
                return;
            }

            transaction_stream_running_ = true;
            transaction_callback_ = std::move(transaction_callback);
            transaction_heartbeat_callback_ = std::move(heartbeat_callback);

            std::string endpoint = "/v3/accounts/" + config_.account_id + "/transactions/stream";

            transaction_thread_ = std::thread([this, endpoint]() {
                stream_worker(endpoint, StreamType::TRANSACTION);
            });
        }

        /**
         * Stop pricing stream
         */
        void stop_pricing_stream() {
            if (pricing_stream_running_) {
                pricing_stream_running_ = false;
                if (pricing_thread_.joinable()) {
                    pricing_thread_.join();
                }
            }
        }

        /**
         * Stop transaction stream
         */
        void stop_transaction_stream() {
            if (transaction_stream_running_) {
                transaction_stream_running_ = false;
                if (transaction_thread_.joinable()) {
                    transaction_thread_.join();
                }
            }
        }

        /**
         * Check if pricing stream is running
         */
        [[nodiscard]] bool is_pricing_stream_running() const {
            return pricing_stream_running_;
        }

        /**
         * Check if transaction stream is running
         */
        [[nodiscard]] bool is_transaction_stream_running() const {
            return transaction_stream_running_;
        }

    private:
        Config config_;
        std::string base_url_;

        // Pricing stream
        std::atomic<bool> pricing_stream_running_{false};
        std::thread pricing_thread_;
        PriceCallback price_callback_;
        HeartbeatCallback heartbeat_callback_;

        // Transaction stream
        std::atomic<bool> transaction_stream_running_{false};
        std::thread transaction_thread_;
        TransactionCallback transaction_callback_;
        HeartbeatCallback transaction_heartbeat_callback_;

        enum class StreamType {
            PRICING,
            TRANSACTION
        };

        static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
            auto* stream = static_cast<OandaStream*>(userp);
            std::string data((char*)contents, size * nmemb);
            stream->process_stream_data(data);
            return size * nmemb;
        }

        void stream_worker(const std::string& endpoint, StreamType type) {
            CURL* curl = curl_easy_init();
            if (!curl) {
                return;
            }

            std::string url = base_url_ + endpoint;
            struct curl_slist* headers = nullptr;

            headers = curl_slist_append(headers, ("Authorization: Bearer " + config_.api_token).c_str());
            headers = curl_slist_append(headers, "Accept-Datetime-Format: RFC3339");

            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
            curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

            auto& running = (type == StreamType::PRICING) ? pricing_stream_running_ : transaction_stream_running_;

            while (running) {
                CURLcode res = curl_easy_perform(curl);

                if (res != CURLE_OK) {
                    // Connection lost, retry after 1 second
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }

            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
        }

        void process_stream_data(const std::string& data) {
            // Stream data comes as newline-separated JSON objects
            std::istringstream iss(data);
            std::string line;

            while (std::getline(iss, line)) {
                if (line.empty()) continue;

                try {
                    auto json = nlohmann::json::parse(line);

                    // Check message type
                    if (json.contains("type")) {
                        std::string type = json["type"];

                        if (type == "PRICE" && price_callback_) {
                            Price price = parse_streaming_price(json);
                            price_callback_(price);
                        } else if (type == "HEARTBEAT") {
                            if (heartbeat_callback_) {
                                heartbeat_callback_();
                            } else if (transaction_heartbeat_callback_) {
                                transaction_heartbeat_callback_();
                            }
                        } else if (transaction_callback_) {
                            // Transaction stream event
                            Transaction txn = parse_streaming_transaction(json);
                            transaction_callback_(txn);
                        }
                    }
                } catch (const std::exception&) {
                    // Skip malformed JSON
                    continue;
                }
            }
        }

        Price parse_streaming_price(const nlohmann::json& json) {
            Price price;
            price.instrument = json.value("instrument", "");
            price.time = rfc3339_to_time(json.value("time", ""));
            price.tradeable = json.value("tradeable", false);

            if (json.contains("bids") && !json["bids"].empty()) {
                price.bid = std::stod(json["bids"][0].value("price", "0"));
            }

            if (json.contains("asks") && !json["asks"].empty()) {
                price.ask = std::stod(json["asks"][0].value("price", "0"));
            }

            price.mid = (price.bid + price.ask) / 2.0;
            return price;
        }

        Transaction parse_streaming_transaction(const nlohmann::json& json) {
            Transaction txn;
            txn.id = json.value("id", "");
            txn.time = rfc3339_to_time(json.value("time", ""));
            txn.account_id = json.value("accountID", "");

            std::string type_str = json.value("type", "");
            // Map type string to enum (simplified)
            if (type_str.find("MARKET_ORDER") != std::string::npos) {
                txn.type = TransactionType::MARKET_ORDER;
            } else if (type_str.find("ORDER_FILL") != std::string::npos) {
                txn.type = TransactionType::ORDER_FILL;
            } else if (type_str.find("ORDER_CANCEL") != std::string::npos) {
                txn.type = TransactionType::ORDER_CANCEL;
            } else {
                txn.type = TransactionType::CREATE;
            }

            if (json.contains("instrument")) {
                txn.instrument = json["instrument"];
            }
            if (json.contains("units")) {
                txn.units = std::stod(json["units"].get<std::string>());
            }
            if (json.contains("price")) {
                txn.price = std::stod(json["price"].get<std::string>());
            }
            if (json.contains("pl")) {
                txn.pl = std::stod(json["pl"].get<std::string>());
            }
            if (json.contains("reason")) {
                txn.reason = json["reason"];
            }

            return txn;
        }

        time_point rfc3339_to_time(const std::string& rfc3339) {
            if (rfc3339.empty()) {
                return std::chrono::system_clock::now();
            }

            std::tm tm = {};
            std::istringstream ss(rfc3339);
            ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");

            return std::chrono::system_clock::from_time_t(std::mktime(&tm));
        }
    };

} // namespace gel::broker
