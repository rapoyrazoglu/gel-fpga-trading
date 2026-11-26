// include/gel/signals/signal_generator.hpp
#pragma once

#include "signal_types.hpp"
#include "gel/patterns/fair_value_gap.hpp"
#include "gel/data/ohlc.hpp"
#include <vector>
#include <deque>
#include <unordered_map>

namespace gel::signals {

    /**
     * SignalGenerator - Converts FVG patterns into trading signals
     *
     * Strategy:
     * 1. Detect FVG patterns (via FVGDetector)
     * 2. Wait for price to retest the FVG zone
     * 3. Generate BUY/SELL signal when retest occurs
     * 4. Calculate confidence based on FVG quality
     */
    class SignalGenerator {
    public:
        using FVG = gel::patterns::FairValueGapDetector::Gap;
        using OHLC = gel::data::OHLCBar;
        using time_point = std::chrono::system_clock::time_point;

        explicit SignalGenerator(SignalConfig config = SignalConfig{})
            : config_(config) {}

        /**
         * Process new OHLC bar and generate signals
         * @param bar Current OHLC bar
         * @param fvgs Active FVG patterns (from FVGDetector)
         * @return Vector of generated signals
         */
        std::vector<TradingSignal> process(const OHLC& bar, const std::vector<FVG>& fvgs) {
            std::vector<TradingSignal> signals;

            // Update active FVGs
            update_active_fvgs(fvgs, bar.timestamp);

            // Check for retest entries
            for (auto& [fvg_id, fvg_state] : active_fvgs_) {
                if (auto signal = check_for_entry(bar, fvg_state)) {
                    signals.push_back(*signal);
                    // Mark FVG as used
                    fvg_state.signal_generated = true;
                }
            }

            // Cleanup expired FVGs
            cleanup_expired_fvgs(bar.timestamp);

            return signals;
        }

        /**
         * Generate exit signal for an open position
         * (Usually handled by ExitManager, but can be called manually)
         */
        std::optional<TradingSignal> generate_exit_signal(
            const std::string& position_id,
            const OHLC& bar,
            const std::string& reason
        ) {
            TradingSignal signal;
            signal.type = SignalType::CLOSE_LONG; // TODO: determine from position
            signal.source = SignalSource::TIME_EXIT;
            signal.timestamp = bar.timestamp;
            signal.signal_price = bar.close;
            signal.confidence = 1.0;
            signal.pattern_id = position_id;
            signal.reason = reason;
            return signal;
        }

        // Statistics
        [[nodiscard]] size_t get_active_fvg_count() const { return active_fvgs_.size(); }
        [[nodiscard]] size_t get_total_signals_generated() const { return total_signals_generated_; }

    private:
        struct FVGState {
            FVG fvg;
            time_point detected_at;
            int bars_since_detection = 0;
            bool signal_generated = false;
            double confidence = 0.0;
        };

        SignalConfig config_;
        std::unordered_map<std::string, FVGState> active_fvgs_;
        size_t total_signals_generated_ = 0;

        /**
         * Update active FVGs with newly detected ones
         */
        void update_active_fvgs(const std::vector<FVG>& fvgs, time_point current_time) {
            for (const auto& fvg : fvgs) {
                std::string fvg_id = generate_fvg_id(fvg);

                // Add new FVG if not already tracked
                if (active_fvgs_.find(fvg_id) == active_fvgs_.end()) {
                    FVGState state;
                    state.fvg = fvg;
                    state.detected_at = current_time;
                    state.confidence = calculate_fvg_confidence(fvg);
                    active_fvgs_[fvg_id] = state;
                }
            }
        }

        /**
         * Check if current bar retests an FVG zone
         */
        std::optional<TradingSignal> check_for_entry(const OHLC& bar, FVGState& fvg_state) {
            // Skip if signal already generated
            if (fvg_state.signal_generated) {
                return std::nullopt;
            }

            // Increment bars counter
            fvg_state.bars_since_detection++;

            // Timeout check
            if (fvg_state.bars_since_detection > config_.max_bars_to_wait) {
                return std::nullopt;
            }

            const auto& fvg = fvg_state.fvg;
            const double tolerance = config_.retest_tolerance_pips * config_.pip_size;

            // Check for bullish FVG retest (BUY signal)
            if (fvg.is_bullish) {
                // Price should come down to FVG upper zone
                bool retest = bar.low <= (fvg.high + tolerance) &&
                              bar.low >= (fvg.low - tolerance);

                if (retest || !config_.wait_for_retest) {
                    return create_buy_signal(bar, fvg_state);
                }
            }
            // Check for bearish FVG retest (SELL signal)
            else {
                // Price should come up to FVG lower zone
                bool retest = bar.high >= (fvg.low - tolerance) &&
                              bar.high <= (fvg.high + tolerance);

                if (retest || !config_.wait_for_retest) {
                    return create_sell_signal(bar, fvg_state);
                }
            }

            return std::nullopt;
        }

        /**
         * Create BUY signal from bullish FVG
         */
        TradingSignal create_buy_signal(const OHLC& bar, const FVGState& fvg_state) {
            const auto& fvg = fvg_state.fvg;

            TradingSignal signal;
            signal.type = SignalType::BUY;
            signal.source = SignalSource::FVG;
            signal.timestamp = bar.timestamp;
            signal.signal_price = bar.close;

            // Entry at FVG midpoint
            double fvg_mid = (fvg.high + fvg.low) / 2.0;
            signal.entry_price = fvg_mid;

            // Stop loss below FVG
            signal.stop_loss = fvg.low - (config_.stop_loss_pips * config_.pip_size);

            // Take profit above entry
            signal.take_profit = fvg_mid + (config_.take_profit_pips * config_.pip_size);

            signal.confidence = fvg_state.confidence;
            signal.pattern_id = generate_fvg_id(fvg);
            signal.reason = "Bullish FVG retest - BUY";

            total_signals_generated_++;
            return signal;
        }

        /**
         * Create SELL signal from bearish FVG
         */
        TradingSignal create_sell_signal(const OHLC& bar, const FVGState& fvg_state) {
            const auto& fvg = fvg_state.fvg;

            TradingSignal signal;
            signal.type = SignalType::SELL;
            signal.source = SignalSource::FVG;
            signal.timestamp = bar.timestamp;
            signal.signal_price = bar.close;

            // Entry at FVG midpoint
            double fvg_mid = (fvg.high + fvg.low) / 2.0;
            signal.entry_price = fvg_mid;

            // Stop loss above FVG
            signal.stop_loss = fvg.high + (config_.stop_loss_pips * config_.pip_size);

            // Take profit below entry
            signal.take_profit = fvg_mid - (config_.take_profit_pips * config_.pip_size);

            signal.confidence = fvg_state.confidence;
            signal.pattern_id = generate_fvg_id(fvg);
            signal.reason = "Bearish FVG retest - SELL";

            total_signals_generated_++;
            return signal;
        }

        /**
         * Calculate confidence score for FVG quality
         * Higher score = better quality pattern
         */
        double calculate_fvg_confidence(const FVG& fvg) {
            double confidence = 0.5; // Base confidence

            // Factor 1: Gap size (prefer medium-sized gaps)
            double gap_pips = fvg.size_pips() / config_.pip_size;
            if (gap_pips >= config_.min_fvg_size_pips && gap_pips <= config_.max_fvg_size_pips) {
                confidence += 0.2;

                // Optimal range: 8-20 pips
                if (gap_pips >= 8.0 && gap_pips <= 20.0) {
                    confidence += 0.1;
                }
            }

            // Factor 2: Candle strength (TODO: add volume/momentum checks)
            // For now, larger gaps = stronger momentum
            if (gap_pips > 15.0) {
                confidence += 0.1;
            }

            // Factor 3: Clean gap (no overlap)
            // TODO: check if gap is fully formed without wicks

            // Clamp to [0.0, 1.0]
            return std::clamp(confidence, 0.0, 1.0);
        }

        /**
         * Generate unique ID for FVG
         */
        std::string generate_fvg_id(const FVG& fvg) const {
            auto ts = std::chrono::system_clock::to_time_t(fvg.time_detected);
            return std::string(fvg.is_bullish ? "BULL_" : "BEAR_") + std::to_string(ts);
        }

        /**
         * Remove FVGs that expired without generating signals
         */
        void cleanup_expired_fvgs(time_point current_time) {
            std::vector<std::string> to_remove;

            for (auto& [id, state] : active_fvgs_) {
                if (state.bars_since_detection > config_.max_bars_to_wait ||
                    state.signal_generated) {
                    to_remove.push_back(id);
                }
            }

            for (const auto& id : to_remove) {
                active_fvgs_.erase(id);
            }
        }
    };

} // namespace gel::signals
