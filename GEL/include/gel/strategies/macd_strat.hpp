// include/gel/strategies/macd_strat.hpp
//
// MACD Strategy — trend-following momentum via moving-average convergence
// divergence.
//
//     ema_fast = EMA(mid, 12)
//     ema_slow = EMA(mid, 26)
//     macd     = ema_fast - ema_slow
//     signal   = EMA(macd, 9)
//
// Trading rule (one position at a time, simple flat/long/short FSM):
//     macd crosses ABOVE signal  -> go LONG  (flat->long, or flip short->long)
//     macd crosses BELOW signal  -> go SHORT (flat->short, or flip long->short)
//
// Header-only, std-only. Prices are int64 ticks on the wire; we convert to
// double for the EMA math and back to ticks when emitting.
//
#pragma once

#include "connector/strategy.hpp"
#include <string>
#include <cstdint>
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

    struct MacdConfig {
        int     fast_period   = 12;    // fast EMA span
        int     slow_period   = 26;    // slow EMA span
        int     signal_period = 9;     // EMA span of the MACD line
        int64_t order_qty     = 100;   // contracts per entry/flip leg
        // Warm-up: don't trade until we've seen at least this many ticks so
        // the EMAs are meaningfully seeded. slow_period is a sane minimum.
        int     warmup_ticks  = 26;
        // Hard cap on |net inventory| the state machine will build.
        int64_t max_abs_position = 1000;
    };

    class MacdStrategy final : public IStrategy {
    public:
        explicit MacdStrategy(MacdConfig cfg = MacdConfig{})
            : cfg_(cfg),
              a_fast_(2.0 / (cfg.fast_period   + 1.0)),
              a_slow_(2.0 / (cfg.slow_period   + 1.0)),
              a_sig_ (2.0 / (cfg.signal_period + 1.0)) {}

        StrategyId  id()   const noexcept override { return static_cast<StrategyId>(10); }
        std::string name() const override          { return "MACD"; }

        void on_start(OrderSink& sink) override { sink_ = &sink; }
        void on_stop() override                 { sink_ = nullptr; }

        void on_tick(const Tick& t) override {
            if (!sink_ || !active_) return;

            State& s = state_[t.symbol_id];

            // mid in double units (ticks -> price). Division by 2 in tick space
            // is fine too, but doubles keep the EMA arithmetic uniform.
            const double mid = 0.5 * (static_cast<double>(t.bid_ticks) +
                                      static_cast<double>(t.ask_ticks));

            if (!s.seeded) {
                s.ema_fast = mid;
                s.ema_slow = mid;
                s.macd     = 0.0;
                s.signal   = 0.0;
                s.seeded   = true;
                s.ticks    = 1;
                s.prev_hist = 0.0;
                s.have_prev_hist = false;
                return;
            }

            s.ema_fast = a_fast_ * mid + (1.0 - a_fast_) * s.ema_fast;
            s.ema_slow = a_slow_ * mid + (1.0 - a_slow_) * s.ema_slow;
            s.macd     = s.ema_fast - s.ema_slow;
            s.signal   = a_sig_ * s.macd + (1.0 - a_sig_) * s.signal;
            ++s.ticks;

            const double hist = s.macd - s.signal;   // MACD histogram

            // Need a previous histogram value to detect a crossing, and enough
            // warm-up so the EMAs aren't dominated by the seed.
            if (!s.have_prev_hist || s.ticks < cfg_.warmup_ticks) {
                s.prev_hist = hist;
                s.have_prev_hist = true;
                return;
            }

            const bool cross_up   = s.prev_hist <= 0.0 && hist > 0.0;
            const bool cross_down = s.prev_hist >= 0.0 && hist < 0.0;
            s.prev_hist = hist;

            // One position at a time. On a bullish cross we want net LONG; if
            // currently short, flip (2x qty: cover + reverse). Same for bearish.
            if (cross_up && s.pos <= 0 &&
                inventory(t.symbol_id) < cfg_.max_abs_position) {
                const int64_t qty = (s.pos < 0) ? cfg_.order_qty * 2 : cfg_.order_qty;
                emit(t, Side::BUY, qty);
                s.pos = +1;
            } else if (cross_down && s.pos >= 0 &&
                       inventory(t.symbol_id) > -cfg_.max_abs_position) {
                const int64_t qty = (s.pos > 0) ? cfg_.order_qty * 2 : cfg_.order_qty;
                emit(t, Side::SELL, qty);
                s.pos = -1;
            }
        }

        void on_fill(const ExecReport& r) override {
            // Only real fills move inventory; skip non-trading terminal reports.
            const auto es = static_cast<connector::ExecStatus>(r.status);
            if (es == connector::ExecStatus::REJECTED ||
                es == connector::ExecStatus::CANCELED ||
                es == connector::ExecStatus::EXPIRED) return;
            // Order.quantity is signed (+buy/-sell) and copied verbatim into
            // fill_quantity, so accumulating tracks realized inventory.
            inventory_[r.symbol_id] += r.fill_quantity;
        }

        void on_kill_switch(connector::RejectReason) override { active_ = false; }
        void on_resume() override                             { active_ = true; }

        const MacdConfig& config() const { return cfg_; }
        int64_t inventory(SymbolId s) const {
            auto it = inventory_.find(s);
            return it == inventory_.end() ? 0 : it->second;
        }
        int position(SymbolId s) const {
            auto it = state_.find(s);
            return it == state_.end() ? 0 : it->second.pos;
        }

    private:
        struct State {
            double ema_fast   = 0.0;
            double ema_slow   = 0.0;
            double macd       = 0.0;
            double signal     = 0.0;
            double prev_hist  = 0.0;
            bool   have_prev_hist = false;
            bool   seeded     = false;
            int    ticks      = 0;
            int    pos        = 0;   // -1 short, 0 flat, +1 long
        };

        void emit(const Tick& t, Side side, int64_t qty) {
            Order o{};
            o.ts_ns           = t.ts_ns;
            o.client_order_id = ++next_coid_;
            o.strategy_id     = static_cast<uint32_t>(id());
            o.symbol_id       = t.symbol_id;
            o.price_ticks     = -1;   // MARKET
            o.quantity        = (side == Side::BUY ? qty : -qty);
            o.side            = static_cast<uint8_t>(side);
            o.ord_type        = static_cast<uint8_t>(OrdType::MARKET);
            o.tif             = static_cast<uint8_t>(TIF::IOC);
            (void)sink_->send(o);
        }

        MacdConfig                             cfg_;
        double                                 a_fast_;
        double                                 a_slow_;
        double                                 a_sig_;
        OrderSink*                             sink_       = nullptr;
        bool                                   active_     = true;
        uint64_t                               next_coid_  = 0;
        std::unordered_map<SymbolId, State>    state_;
        std::unordered_map<SymbolId, int64_t>  inventory_;
    };

} // namespace gel::strategies
