// src/patterns/fair_value_gap.cpp
#include "gel/patterns/fair_value_gap.hpp"
#include "gel/analysis/ai_results_parser.hpp"
#include <algorithm>
#include <sstream>

namespace gel::patterns {

void FairValueGapDetector::add_ohlc(const data::OHLCBar& ohlc) {
    // 1. Add OHLC to buffer
    ohlc_buffer_.push_back(ohlc);

    // 2. Maintain window size (max 3 candles)
    while (ohlc_buffer_.size() > WINDOW_SIZE) {
        ohlc_buffer_.pop_front();
    }

    // 3. Detect gap if we have 3 candles
    if (ohlc_buffer_.size() == WINDOW_SIZE) {
        auto gap = detect_gap();
        if (gap.has_value()) {
            active_gaps_.push_back(gap.value());

            // 4. Maintain max active gaps (FIFO)
            while (active_gaps_.size() > MAX_ACTIVE_GAPS) {
                active_gaps_.pop_front();
            }
        }
    }

    // 5. Mark as initialized
    is_initialized_ = true;
}

void FairValueGapDetector::update_price(const data::Tick& tick) {
    // Calculate mid price
    current_price_ = (tick.bid + tick.ask) / 2.0;

    // Remove filled gaps
    cleanup_filled_gaps();
}

std::optional<FairValueGapDetector::Gap> FairValueGapDetector::detect_gap() const {
    // Prerequisite: Must have 3 candles
    if (ohlc_buffer_.size() != WINDOW_SIZE) {
        return std::nullopt;
    }

    // c2 = oldest (left/index 0), c1 = middle (index 1), c0 = newest (right/index 2)
    const auto& c2 = ohlc_buffer_[0];
    const auto& c1 = ohlc_buffer_[1];
    const auto& c0 = ohlc_buffer_[2];

    // === Bullish FVG: c2.high < c0.low ===
    // Price had upward displacement, leaving gap below
    // Gap zone: [c2.high, c0.low]
    // Trade: Price returns to gap → BUY (discount zone)
    if (c2.high < c0.low) {
        double gap_size = c0.low - c2.high;

        // Filter by minimum gap size
        if (gap_size >= config_.min_gap_size) {
            Gap gap;
            gap.low = c2.high;           // Lower bound
            gap.high = c0.low;           // Upper bound
            gap.time_detected = c0.timestamp;
            gap.is_bullish = true;
            gap.confidence = calculate_confidence(gap);
            return gap;
        }
    }

    // === Bearish FVG: c2.low > c0.high ===
    // Price had downward displacement, leaving gap above
    // Gap zone: [c0.high, c2.low]
    // Trade: Price returns to gap → SELL (premium zone)
    if (c2.low > c0.high) {
        double gap_size = c2.low - c0.high;

        if (gap_size >= config_.min_gap_size) {
            Gap gap;
            gap.low = c0.high;           // Lower bound
            gap.high = c2.low;           // Upper bound
            gap.time_detected = c0.timestamp;
            gap.is_bullish = false;
            gap.confidence = calculate_confidence(gap);
            return gap;
        }
    }

    // No gap detected
    return std::nullopt;
}

PatternResult FairValueGapDetector::detect() {
    PatternResult result;
    result.pattern_name = name();

    // Check prerequisites
    if (!is_ready()) {
        return result; // NONE signal (default)
    }

    if (active_gaps_.empty()) {
        return result; // No gaps to trade
    }

    if (!current_price_.has_value()) {
        return result; // No current price
    }

    double price = current_price_.value();

    // Check each active gap (FIFO priority - oldest first)
    for (const auto& gap : active_gaps_) {
        // Is price inside gap zone?
        if (gap.contains(price)) {
            // Generate signal based on gap type
            if (gap.is_bullish) {
                // Bullish FVG: Price returned to discount zone → BUY
                result.signal = Signal::BUY;
            } else {
                // Bearish FVG: Price returned to premium zone → SELL
                result.signal = Signal::SELL;
            }

            // Populate result metadata
            result.confidence = gap.confidence;
            result.win_rate = config_.win_rate;
            result.expected_profit_pips = config_.avg_profit_pips;

            // JSON-like metadata string
            std::ostringstream meta;
            meta << "{\"gap_low\":" << gap.low
                 << ",\"gap_high\":" << gap.high
                 << ",\"gap_size_pips\":" << gap.size_pips()
                 << ",\"is_bullish\":" << (gap.is_bullish ? "true" : "false") << "}";
            result.metadata = meta.str();

            // Take first valid gap (FIFO)
            break;
        }
    }

    return result;
}

double FairValueGapDetector::calculate_confidence(const Gap& gap) const {
    // Base confidence
    double confidence = 0.5;

    // === Factor 1: Gap Size (0-20 pips normalization) ===
    // Larger gaps = higher confidence (up to +0.3)
    double gap_pips = gap.size_pips();
    double size_score = std::min(gap_pips / 20.0, 1.0);
    confidence += size_score * 0.3;

    // === Factor 2: Relative to Min Gap Size ===
    // Gap much larger than minimum = higher confidence (up to +0.2)
    double min_gap_pips = config_.min_gap_size / 0.0001;
    if (min_gap_pips > 0) {
        double relative_size = gap_pips / min_gap_pips;
        double relative_score = std::min(relative_size / 3.0, 1.0);
        confidence += relative_score * 0.2;
    }

    // Cap at 1.0
    return std::min(confidence, 1.0);
}

void FairValueGapDetector::cleanup_filled_gaps() {
    if (!current_price_.has_value()) {
        return; // No price to check against
    }

    double price = current_price_.value();

    // Remove gaps that are filled
    active_gaps_.erase(
        std::remove_if(active_gaps_.begin(), active_gaps_.end(),
            [price](const Gap& gap) {
                return gap.is_filled(price);
            }),
        active_gaps_.end()
    );
}

void FairValueGapDetector::load_config_from_csv(const std::string& csv_path) {
    // Parse CSV using AIResultsParser
    auto result = gel::analysis::AIResultsParser::parse_fvg_csv(csv_path);

    // Load configuration
    config_.min_gap_size = result.min_gap_size;
    config_.avg_fill_time_minutes = result.avg_fill_time_minutes;
    config_.win_rate = result.win_rate;
    config_.avg_profit_pips = result.avg_profit_pips;
    config_.confidence_threshold = 0.70; // Fixed threshold

    // Mark as initialized
    is_initialized_ = true;
}

} // namespace gel::patterns
