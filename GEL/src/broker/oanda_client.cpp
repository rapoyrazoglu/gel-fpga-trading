// src/broker/oanda_client.cpp
#include "gel/broker/oanda_client.hpp"
#include <iomanip>
#include <sstream>

namespace gel::broker {

    using json = nlohmann::json;

    // === JSON PARSERS ===

    AccountSummary OandaClient::parse_account_summary(const json& j) {
        AccountSummary summary;
        summary.id = j.value("id", "");
        summary.alias = j.value("alias", "");
        summary.currency = j.value("currency", "USD");
        summary.balance = std::stod(j.value("balance", "0"));
        summary.nav = std::stod(j.value("NAV", "0"));
        summary.unrealized_pl = std::stod(j.value("unrealizedPL", "0"));
        summary.realized_pl = std::stod(j.value("pl", "0"));
        summary.margin_used = std::stod(j.value("marginUsed", "0"));
        summary.margin_available = std::stod(j.value("marginAvailable", "0"));
        summary.position_value = std::stod(j.value("positionValue", "0"));
        summary.open_trade_count = j.value("openTradeCount", 0);
        summary.open_position_count = j.value("openPositionCount", 0);
        summary.pending_order_count = j.value("pendingOrderCount", 0);
        summary.created_time = rfc3339_to_time(j.value("createdTime", ""));
        return summary;
    }

    Instrument OandaClient::parse_instrument(const json& j) {
        Instrument inst;
        inst.name = j.value("name", "");
        inst.type = j.value("type", "CURRENCY");
        inst.display_name = j.value("displayName", "");
        inst.pip_location = j.value("pipLocation", -4);
        inst.display_precision = j.value("displayPrecision", 5);
        inst.trade_units_precision = j.value("tradeUnitsPrecision", 0);
        inst.minimum_trade_size = std::stod(j.value("minimumTradeSize", "1"));
        inst.maximum_trailing_stop_distance = std::stod(j.value("maximumTrailingStopDistance", "100000"));
        inst.minimum_trailing_stop_distance = std::stod(j.value("minimumTrailingStopDistance", "0.00005"));
        inst.maximum_position_size = std::stod(j.value("maximumPositionSize", "0"));
        inst.maximum_order_units = std::stod(j.value("maximumOrderUnits", "100000000"));
        inst.margin_rate = std::stod(j.value("marginRate", "0.02"));
        return inst;
    }

    Price OandaClient::parse_price(const json& j) {
        Price price;
        price.instrument = j.value("instrument", "");
        price.time = rfc3339_to_time(j.value("time", ""));
        price.tradeable = j.value("tradeable", false);

        // Parse bid/ask buckets
        if (j.contains("bids") && !j["bids"].empty()) {
            price.bid = std::stod(j["bids"][0].value("price", "0"));
            for (const auto& bucket : j["bids"]) {
                PriceBucket pb;
                pb.price = std::stod(bucket.value("price", "0"));
                pb.liquidity = bucket.value("liquidity", 0);
                price.bids.push_back(pb);
            }
        }

        if (j.contains("asks") && !j["asks"].empty()) {
            price.ask = std::stod(j["asks"][0].value("price", "0"));
            for (const auto& bucket : j["asks"]) {
                PriceBucket pb;
                pb.price = std::stod(bucket.value("price", "0"));
                pb.liquidity = bucket.value("liquidity", 0);
                price.asks.push_back(pb);
            }
        }

        price.mid = (price.bid + price.ask) / 2.0;
        return price;
    }

    Candlestick OandaClient::parse_candlestick(const json& j) {
        Candlestick candle;
        candle.time = rfc3339_to_time(j.value("time", ""));
        candle.volume = j.value("volume", 0);
        candle.complete = j.value("complete", false);

        // Parse mid prices
        if (j.contains("mid")) {
            const auto& mid = j["mid"];
            candle.mid.o = std::stod(mid.value("o", "0"));
            candle.mid.h = std::stod(mid.value("h", "0"));
            candle.mid.l = std::stod(mid.value("l", "0"));
            candle.mid.c = std::stod(mid.value("c", "0"));
        }

        // Parse bid prices
        if (j.contains("bid")) {
            const auto& bid = j["bid"];
            candle.bid.o = std::stod(bid.value("o", "0"));
            candle.bid.h = std::stod(bid.value("h", "0"));
            candle.bid.l = std::stod(bid.value("l", "0"));
            candle.bid.c = std::stod(bid.value("c", "0"));
        }

        // Parse ask prices
        if (j.contains("ask")) {
            const auto& ask = j["ask"];
            candle.ask.o = std::stod(ask.value("o", "0"));
            candle.ask.h = std::stod(ask.value("h", "0"));
            candle.ask.l = std::stod(ask.value("l", "0"));
            candle.ask.c = std::stod(ask.value("c", "0"));
        }

        return candle;
    }

    Order OandaClient::parse_order(const json& j) {
        Order order;
        order.id = j.value("id", "");
        order.create_time = rfc3339_to_time(j.value("createTime", ""));

        std::string state_str = j.value("state", "PENDING");
        if (state_str == "FILLED") order.state = OrderState::FILLED;
        else if (state_str == "TRIGGERED") order.state = OrderState::TRIGGERED;
        else if (state_str == "CANCELLED") order.state = OrderState::CANCELLED;
        else order.state = OrderState::PENDING;

        std::string type_str = j.value("type", "MARKET");
        if (type_str == "MARKET") order.type = OrderType::MARKET;
        else if (type_str == "LIMIT") order.type = OrderType::LIMIT;
        else if (type_str == "STOP") order.type = OrderType::STOP;
        else if (type_str == "MARKET_IF_TOUCHED") order.type = OrderType::MARKET_IF_TOUCHED;
        else order.type = OrderType::MARKET;

        order.instrument = j.value("instrument", "");
        order.units = std::stod(j.value("units", "0"));

        if (j.contains("price")) {
            order.price = std::stod(j["price"].get<std::string>());
        }

        return order;
    }

    Order OandaClient::parse_order_from_fill(const json& j) {
        Order order;
        order.id = j.value("orderID", "");
        order.state = OrderState::FILLED;
        order.type = OrderType::MARKET;
        order.instrument = j.value("instrument", "");
        order.units = std::stod(j.value("units", "0"));
        order.filled_price = std::stod(j.value("price", "0"));
        order.filled_time = rfc3339_to_time(j.value("time", ""));
        order.trade_id = j.value("tradeOpened", j.value("id", ""));
        return order;
    }

    Order OandaClient::parse_order_from_create(const json& j) {
        Order order;
        order.id = j.value("id", "");
        order.create_time = rfc3339_to_time(j.value("time", ""));
        order.state = OrderState::PENDING;

        std::string type_str = j.value("type", "MARKET");
        if (type_str == "MARKET") order.type = OrderType::MARKET;
        else if (type_str == "LIMIT") order.type = OrderType::LIMIT;
        else if (type_str == "STOP") order.type = OrderType::STOP;
        else order.type = OrderType::MARKET;

        order.instrument = j.value("instrument", "");
        order.units = std::stod(j.value("units", "0"));

        if (j.contains("price")) {
            order.price = std::stod(j["price"].get<std::string>());
        }

        return order;
    }

    Trade OandaClient::parse_trade(const json& j) {
        Trade trade;
        trade.id = j.value("id", "");
        trade.instrument = j.value("instrument", "");
        trade.price = std::stod(j.value("price", "0"));
        trade.units = std::stod(j.value("currentUnits", "0"));
        trade.open_time = rfc3339_to_time(j.value("openTime", ""));

        std::string state_str = j.value("state", "OPEN");
        trade.state = (state_str == "OPEN") ? OrderState::FILLED : OrderState::CANCELLED;

        trade.initial_units = std::stod(j.value("initialUnits", "0"));
        trade.current_units = std::stod(j.value("currentUnits", "0"));
        trade.realized_pl = std::stod(j.value("realizedPL", "0"));
        trade.unrealized_pl = std::stod(j.value("unrealizedPL", "0"));

        if (j.contains("stopLossOrder") && j["stopLossOrder"].contains("price")) {
            trade.stop_loss_price = std::stod(j["stopLossOrder"]["price"].get<std::string>());
        }

        if (j.contains("takeProfitOrder") && j["takeProfitOrder"].contains("price")) {
            trade.take_profit_price = std::stod(j["takeProfitOrder"]["price"].get<std::string>());
        }

        if (j.contains("trailingStopLossOrder") && j["trailingStopLossOrder"].contains("distance")) {
            trade.trailing_stop_distance = std::stod(j["trailingStopLossOrder"]["distance"].get<std::string>());
        }

        return trade;
    }

    Position OandaClient::parse_position(const json& j) {
        Position pos;
        pos.instrument = j.value("instrument", "");

        if (j.contains("long")) {
            const auto& long_pos = j["long"];
            pos.long_units = std::stod(long_pos.value("units", "0"));
            pos.long_unrealized_pl = std::stod(long_pos.value("unrealizedPL", "0"));
        }

        if (j.contains("short")) {
            const auto& short_pos = j["short"];
            pos.short_units = std::stod(short_pos.value("units", "0"));
            pos.short_unrealized_pl = std::stod(short_pos.value("unrealizedPL", "0"));
        }

        pos.pl = std::stod(j.value("pl", "0"));
        pos.unrealized_pl = std::stod(j.value("unrealizedPL", "0"));
        pos.margin_used = std::stod(j.value("marginUsed", "0"));

        return pos;
    }

    // === BUILDERS ===

    json OandaClient::build_order_json(const OrderRequest& order) {
        json j;

        // Order type
        switch (order.type) {
            case OrderType::MARKET:
                j["type"] = "MARKET";
                break;
            case OrderType::LIMIT:
                j["type"] = "LIMIT";
                break;
            case OrderType::STOP:
                j["type"] = "STOP";
                break;
            case OrderType::MARKET_IF_TOUCHED:
                j["type"] = "MARKET_IF_TOUCHED";
                break;
            default:
                j["type"] = "MARKET";
        }

        // Basic fields
        j["instrument"] = order.instrument;
        j["units"] = std::to_string(static_cast<int>(order.units));

        // Time in force
        switch (order.time_in_force) {
            case TimeInForce::FOK:
                j["timeInForce"] = "FOK";
                break;
            case TimeInForce::IOC:
                j["timeInForce"] = "IOC";
                break;
            case TimeInForce::GTC:
                j["timeInForce"] = "GTC";
                break;
            case TimeInForce::GTD:
                j["timeInForce"] = "GTD";
                if (order.gtd_time) {
                    j["gtdTime"] = time_to_rfc3339(*order.gtd_time);
                }
                break;
            case TimeInForce::DAY:
                j["timeInForce"] = "DAY";
                break;
        }

        // Price (for LIMIT/STOP/MIT orders)
        if (order.price) {
            j["price"] = std::to_string(*order.price);
        }

        // Price bound (slippage protection)
        if (order.price_bound) {
            j["priceBound"] = std::to_string(*order.price_bound);
        }

        // Stop loss
        if (order.stop_loss_price) {
            j["stopLossOnFill"] = {
                {"price", std::to_string(*order.stop_loss_price)},
                {"timeInForce", "GTC"}
            };
        }

        // Take profit
        if (order.take_profit_price) {
            j["takeProfitOnFill"] = {
                {"price", std::to_string(*order.take_profit_price)},
                {"timeInForce", "GTC"}
            };
        }

        // Trailing stop
        if (order.trailing_stop_distance) {
            j["trailingStopLossOnFill"] = {
                {"distance", std::to_string(*order.trailing_stop_distance)},
                {"timeInForce", "GTC"}
            };
        }

        return j;
    }

    // === HELPERS ===

    std::string OandaClient::time_to_rfc3339(time_point tp) {
        auto t = std::chrono::system_clock::to_time_t(tp);
        std::tm tm = *std::gmtime(&t);

        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");

        // Add microseconds
        auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
            tp.time_since_epoch()
        ) % 1000000;
        oss << "." << std::setfill('0') << std::setw(6) << microseconds.count() << "Z";

        return oss.str();
    }

    time_point OandaClient::rfc3339_to_time(const std::string& rfc3339) {
        if (rfc3339.empty()) {
            return std::chrono::system_clock::now();
        }

        // Parse: 2023-01-15T10:30:45.123456Z
        std::tm tm = {};
        std::istringstream ss(rfc3339);
        ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");

        auto tp = std::chrono::system_clock::from_time_t(std::mktime(&tm));

        // Parse microseconds if present
        auto dot_pos = rfc3339.find('.');
        if (dot_pos != std::string::npos) {
            auto z_pos = rfc3339.find('Z', dot_pos);
            if (z_pos != std::string::npos) {
                std::string usec_str = rfc3339.substr(dot_pos + 1, z_pos - dot_pos - 1);
                int microseconds = std::stoi(usec_str);
                tp += std::chrono::microseconds(microseconds);
            }
        }

        return tp;
    }

} // namespace gel::broker
