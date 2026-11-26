// src/patterns/order_flow.cpp
#include "gel/patterns/order_flow.hpp"
#include "gel/analysis/ai_results_parser.hpp"
#include <algorithm>
#include <sstream>
#include <cmath>

namespace gel::patterns {

void OrderFlowAnalyzer::add_tick(const data::Tick& tick) {
    // 1. Classify tick type (BUY/SELL/NEUTRAL)
    TickType type = classify_tick(tick);

    // 2. Add to buffer with classification
    ClassifiedTick ct;
    ct.type = type;
    ct.time = tick.timestamp;
    ct.price = (tick.bid + tick.ask) / 2.0;
    tick_buffer_.push_back(ct);

    // 3. Cleanup old ticks (outside rolling window)
    cleanup_old_ticks(tick.timestamp);

    // 4. Recalculate imbalance
    current_imbalance_ = calculate_imbalance();

    // 5. Update previous tick for next classification
    prev_tick_ = tick;
    is_initialized_ = true;
}

OrderFlowAnalyzer::TickType OrderFlowAnalyzer::classify_tick(const data::Tick& tick) const {
    // First tick → no comparison possible
    if (!prev_tick_.has_value()) {
        return TickType::NEUTRAL;
    }

    // Calculate mid prices
    double curr_mid = (tick.bid + tick.ask) / 2.0;
    double prev_mid = (prev_tick_->bid + prev_tick_->ask) / 2.0;

    const double epsilon = 0.00001; // 0.1 pip threshold

    // === Uptick → Aggressive BUY ===
    if (curr_mid > prev_mid + epsilon) {
        return TickType::BUY;
    }

    // === Downtick → Aggressive SELL ===
    if (curr_mid < prev_mid - epsilon) {
        return TickType::SELL;
    }

    // === No change → Use bid/ask proximity ===
    // Closer to ask = more likely BUY, closer to bid = more likely SELL
    double bid_dist = std::abs(curr_mid - tick.bid);
    double ask_dist = std::abs(curr_mid - tick.ask);

    if (ask_dist < bid_dist) {
        return TickType::BUY; // Closer to ask
    } else if (bid_dist < ask_dist) {
        return TickType::SELL; // Closer to bid
    }

    // Exactly in the middle → neutral
    return TickType::NEUTRAL;
}

OrderFlowAnalyzer::Imbalance OrderFlowAnalyzer::calculate_imbalance() const {
    Imbalance imb;

    // Count BUY and SELL ticks in current buffer
    for (const auto& ct : tick_buffer_) {
        if (ct.type == TickType::BUY) {
            imb.buy_count++;
        } else if (ct.type == TickType::SELL) {
            imb.sell_count++;
        }
        // NEUTRAL ticks are ignored in ratio calculation
    }

    // Calculate buy/sell ratio
    if (imb.sell_count > 0) {
        imb.ratio = static_cast<double>(imb.buy_count) / imb.sell_count;
    } else {
        // Edge case: No sells
        imb.ratio = (imb.buy_count > 0) ? 999.0 : 1.0;
    }

    // Calculate confidence score
    imb.confidence = calculate_confidence(imb);

    return imb;
}

PatternResult OrderFlowAnalyzer::detect() {
    PatternResult result;
    result.pattern_name = name();

    // Check prerequisites
    if (!is_ready()) {
        return result; // NONE signal
    }

    // === Strong BUY Imbalance ===
    if (current_imbalance_.is_strong_buy(config_.buy_threshold)) {
        result.signal = Signal::BUY;
        result.confidence = current_imbalance_.confidence;
        result.win_rate = config_.win_rate;
        result.expected_profit_pips = config_.avg_profit_pips;

        // Metadata JSON
        std::ostringstream meta;
        meta << "{\"buy_count\":" << current_imbalance_.buy_count
             << ",\"sell_count\":" << current_imbalance_.sell_count
             << ",\"ratio\":" << current_imbalance_.ratio
             << ",\"total_ticks\":" << current_imbalance_.total_count() << "}";
        result.metadata = meta.str();
    }
    // === Strong SELL Imbalance ===
    else if (current_imbalance_.is_strong_sell(config_.buy_threshold)) {
        result.signal = Signal::SELL;
        result.confidence = current_imbalance_.confidence;
        result.win_rate = config_.win_rate;
        result.expected_profit_pips = config_.avg_profit_pips;

        std::ostringstream meta;
        meta << "{\"buy_count\":" << current_imbalance_.buy_count
             << ",\"sell_count\":" << current_imbalance_.sell_count
             << ",\"ratio\":" << current_imbalance_.ratio
             << ",\"total_ticks\":" << current_imbalance_.total_count() << "}";
        result.metadata = meta.str();
    }

    return result;
}

double OrderFlowAnalyzer::calculate_confidence(const Imbalance& imbalance) const {
    double base = 0.5;

    // === Factor 1: Sample Size ===
    // More ticks in window = higher confidence (up to +0.3 at 100 ticks)
    int total = imbalance.total_count();
    double sample_score = std::min(total / 100.0, 0.3);

    // === Factor 2: Ratio Extremity ===
    // Further from 1.0 (neutral) = higher confidence (up to +0.2)
    double ratio_deviation = std::abs(imbalance.ratio - 1.0);
    double ratio_score = std::min(ratio_deviation / 2.0, 0.2);

    return std::min(base + sample_score + ratio_score, 1.0);
}

void OrderFlowAnalyzer::cleanup_old_ticks(timestamp current_time) {
    // Calculate cutoff time (window_duration ago)
    auto cutoff = current_time - config_.window_duration;

    // Remove ticks older than cutoff
    tick_buffer_.erase(
        std::remove_if(tick_buffer_.begin(), tick_buffer_.end(),
            [cutoff](const ClassifiedTick& ct) {
                return ct.time < cutoff;
            }),
        tick_buffer_.end()
    );
}

void OrderFlowAnalyzer::load_config_from_csv(const std::string& csv_path) {
    // Parse CSV using AIResultsParser
    auto result = gel::analysis::AIResultsParser::parse_order_flow_csv(csv_path);

    // Load configuration
    config_.window_duration = std::chrono::seconds(result.optimal_window_seconds);
    config_.buy_threshold = result.buy_threshold;
    config_.sell_threshold = result.sell_threshold;
    config_.win_rate = result.win_rate;
    config_.avg_profit_pips = result.avg_profit_pips;
    config_.confidence_threshold = 0.70;

    // Mark as initialized
    is_initialized_ = true;
}

} // namespace gel::patterns
