// include/gel/strategies/rsi_strat.hpp
//
// RSI(14) mean-reversion strategy.
// Owner: research (StrategyId 6).
//
// Signal:
//   - Compute Wilder's RSI(14) on mid-price returns.
//   - RSI < 30  -> oversold  -> go LONG.
//   - RSI > 70  -> overbought -> go SHORT.
//   - Flatten the position when RSI crosses back through 50.
//
// Wilder's smoothing:
//   avg_gain = ((n-1) * avg_gain + gain) / n
//   avg_loss = ((n-1) * avg_loss + loss) / n
//   RS  = avg_gain / avg_loss
//   RSI = 100 - 100 / (1 + RS)
//
// The first `period` diffs are used to seed the averages with a simple mean;
// only after that does the strategy start emitting orders. State is kept per
// symbol_id, and a flat/long/short state machine prevents per-tick order spam.
//
#pragma once

#include "connector/strategy.hpp"
#include "gel/platform/telemetry.hpp"   // for now_ns()
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace gel::strategies {

    using connector::IStrategy;
    using connector::OrderSink;
    using connector::Order;
    using connector::Tick;
    using connector::ExecReport;
    using connector::StrategyId;
    using connector::Side;
    using connector::OrdType;
    using connector::TIF;
    using connector::SymbolId;

    struct RsiConfig {
        int     period          = 14;     // Wilder lookback
        double  lower           = 30.0;   // oversold threshold -> long
        double  upper           = 70.0;   // overbought threshold -> short
        double  midline         = 50.0;   // exit line
        int64_t order_qty       = 100;    // contracts per entry
        int64_t max_abs_position = 100;   // hard position cap (one clip)
    };

    class RsiStrategy final : public IStrategy {
    public:
        enum class Pos : int8_t { FLAT = 0, LONG = 1, SHORT = -1 };

        explicit RsiStrategy(RsiConfig cfg = RsiConfig{}) : cfg_(cfg) {}

        StrategyId  id()   const noexcept override { return static_cast<StrategyId>(6); }
        std::string name() const override          { return "RSI"; }

        void on_start(OrderSink& sink) override { sink_ = &sink; }
        void on_stop() override                 { sink_ = nullptr; }

        void on_tick(const Tick& t) override {
            if (!sink_ || !active_.load(std::memory_order_acquire)) return;

            const double mid = 0.5 * (connector::from_ticks(t.bid_ticks) +
                                       connector::from_ticks(t.ask_ticks));
            if (!(mid > 0.0)) return;

            St& s = state_[t.symbol_id];

            if (!s.have_last) {
                s.have_last = true;
                s.last_mid  = mid;
                return;
            }

            const double diff = mid - s.last_mid;
            s.last_mid = mid;
            const double gain = diff > 0.0 ?  diff : 0.0;
            const double loss = diff < 0.0 ? -diff : 0.0;

            const int n = cfg_.period;
            if (s.seen < n) {
                // Seed phase: accumulate a simple average of the first n diffs.
                s.avg_gain += gain;
                s.avg_loss += loss;
                if (++s.seen == n) {
                    s.avg_gain /= n;
                    s.avg_loss /= n;
                    s.warm = true;
                }
                return;
            }

            // Wilder smoothing.
            s.avg_gain = ((n - 1) * s.avg_gain + gain) / n;
            s.avg_loss = ((n - 1) * s.avg_loss + loss) / n;

            const double rsi = compute_rsi(s.avg_gain, s.avg_loss);
            const double prev = s.last_rsi;
            const bool have_prev = s.have_rsi;
            s.last_rsi = rsi;
            s.have_rsi = true;

            // ---- State machine -------------------------------------------
            switch (s.pos) {
                case Pos::FLAT:
                    if (rsi < cfg_.lower) {
                        enter(t, Side::BUY,  s);   // oversold -> long
                        s.pos = Pos::LONG;
                    } else if (rsi > cfg_.upper) {
                        enter(t, Side::SELL, s);   // overbought -> short
                        s.pos = Pos::SHORT;
                    }
                    break;

                case Pos::LONG:
                    // Exit long when RSI crosses up through the midline.
                    if (have_prev && prev < cfg_.midline && rsi >= cfg_.midline) {
                        flatten(t, s);
                        s.pos = Pos::FLAT;
                    }
                    break;

                case Pos::SHORT:
                    // Exit short when RSI crosses down through the midline.
                    if (have_prev && prev > cfg_.midline && rsi <= cfg_.midline) {
                        flatten(t, s);
                        s.pos = Pos::FLAT;
                    }
                    break;
            }
        }

        void on_fill(const ExecReport& r) override {
            // Only real fills move position; skip non-trading terminal reports.
            const auto es = static_cast<connector::ExecStatus>(r.status);
            if (es == connector::ExecStatus::REJECTED ||
                es == connector::ExecStatus::CANCELED ||
                es == connector::ExecStatus::EXPIRED) return;
            // Signed fill_quantity (+ buy / - sell) folds straight into position.
            state_[r.symbol_id].position += r.fill_quantity;
        }

        void on_kill_switch(connector::RejectReason) override {
            active_.store(false, std::memory_order_release);
        }
        void on_resume() override {
            active_.store(true, std::memory_order_release);
        }

        // Introspection (tests / telemetry).
        int64_t position(SymbolId s) const {
            auto it = state_.find(s);
            return it == state_.end() ? 0 : it->second.position;
        }
        double rsi(SymbolId s) const {
            auto it = state_.find(s);
            return (it == state_.end() || !it->second.have_rsi) ? 50.0
                                                                : it->second.last_rsi;
        }
        const RsiConfig& config() const { return cfg_; }

    private:
        struct St {
            bool    have_last = false;
            bool    warm      = false;
            bool    have_rsi  = false;
            int     seen      = 0;
            double  last_mid  = 0.0;
            double  avg_gain  = 0.0;
            double  avg_loss  = 0.0;
            double  last_rsi  = 50.0;
            int64_t position  = 0;
            Pos     pos       = Pos::FLAT;
        };

        static double compute_rsi(double avg_gain, double avg_loss) {
            if (avg_loss <= 0.0) return avg_gain > 0.0 ? 100.0 : 50.0;
            const double rs = avg_gain / avg_loss;
            return 100.0 - 100.0 / (1.0 + rs);
        }

        // Open a fresh clip on the given side (position-capped).
        void enter(const Tick& t, Side side, const St& s) {
            int64_t qty = cfg_.order_qty;
            const int64_t room = cfg_.max_abs_position -
                                 (s.position < 0 ? -s.position : s.position);
            if (room <= 0) return;
            if (qty > room) qty = room;
            emit(t, side, qty, OrdType::MARKET);
        }

        // Close whatever position we currently hold with an offsetting MARKET.
        void flatten(const Tick& t, const St& s) {
            if (s.position == 0) return;
            const Side side = s.position > 0 ? Side::SELL : Side::BUY;
            const int64_t qty = s.position > 0 ? s.position : -s.position;
            emit(t, side, qty, OrdType::MARKET);
        }

        void emit(const Tick& t, Side side, int64_t qty, OrdType type) {
            if (qty <= 0) return;
            Order o{};
            o.ts_ns           = gel::platform::now_ns();
            o.client_order_id = next_coid_.fetch_add(1, std::memory_order_relaxed);
            o.strategy_id     = static_cast<uint32_t>(id());
            o.symbol_id       = t.symbol_id;
            o.price_ticks     = -1;                     // MARKET
            o.quantity        = (side == Side::BUY ? qty : -qty);
            o.side            = static_cast<uint8_t>(side);
            o.ord_type        = static_cast<uint8_t>(type);
            o.tif             = static_cast<uint8_t>(TIF::IOC);
            (void)sink_->send(o);
        }

        RsiConfig                        cfg_;
        OrderSink*                       sink_ = nullptr;
        std::atomic<bool>                active_{true};
        std::atomic<uint64_t>            next_coid_{1};
        std::unordered_map<SymbolId, St> state_;
    };

} // namespace gel::strategies
