// include/gel/strategies/vwap_revert_strat.hpp
//
// VWAP Reversion — mean-reversion around a running session VWAP proxy.
//
// Ticks carry no trade size, so we volume-weight each mid by a constant
// synthetic volume. That reduces the "VWAP" to an EWMA of mid, which is
// exactly the running-average behaviour we want for a session anchor:
//
//     vwap_t = (Σ v·mid) / (Σ v)          with v = const
//
// To keep the anchor from drifting to the very first print forever, we
// decay the accumulators geometrically each tick (a leaky VWAP). The
// half-life is configurable; a long half-life ≈ a true session VWAP.
//
// Signal (per symbol, flat/long/short state machine):
//     dev = (mid - vwap) / vwap                         // fractional gap
//     dev < -enter_frac  and flat  -> go LONG  (buy below VWAP)
//     dev > +enter_frac  and flat  -> go SHORT (sell above VWAP)
//     |dev| < exit_frac  and !flat -> revert to flat (close at VWAP)
//
// Header-only, std-only, C++20/23. One MARKET order per state transition,
// so it does not spam every tick.
//
#pragma once

#include "connector/strategy.hpp"
#include <chrono>
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

    struct VwapConfig {
        // Warm-up: don't trade until the VWAP has seen this many ticks.
        uint32_t warmup_ticks = 20;
        // Enter when |mid/vwap - 1| exceeds this (fractional, e.g. 0.0008 = 8 bps).
        double   enter_frac   = 0.0008;
        // Exit (revert to flat) once the gap shrinks back inside this band.
        double   exit_frac    = 0.0002;
        // Leaky-VWAP decay applied to the accumulators each tick. 1.0 = pure
        // session VWAP (never forgets); <1.0 tilts toward recent prints.
        double   decay        = 0.9995;
        // Synthetic per-tick volume used as the VWAP weight.
        double   synth_volume = 1.0;
        // Contracts per entry.
        int64_t  order_qty    = 100;
        // Hard cap on absolute inventory the state machine will build.
        int64_t  max_abs_inventory = 100;
    };

    // Simple flat/long/short state machine so we act on transitions only.
    enum class VwapState : uint8_t { FLAT = 0, LONG = 1, SHORT = 2 };

    class VwapReversionStrategy final : public IStrategy {
    public:
        explicit VwapReversionStrategy(VwapConfig cfg = VwapConfig{}) : cfg_(cfg) {}

        StrategyId  id()   const noexcept override {
            return static_cast<StrategyId>(9);
        }
        std::string name() const override { return "VwapReversion"; }

        void on_start(OrderSink& sink) override { sink_ = &sink; }
        void on_stop() override                 { sink_ = nullptr; }

        void on_tick(const Tick& t) override {
            if (!sink_ || !active_) return;

            const double bid = connector::from_ticks(t.bid_ticks);
            const double ask = connector::from_ticks(t.ask_ticks);
            if (bid <= 0.0 || ask <= 0.0) return;      // guard bad prints
            const double mid = 0.5 * (bid + ask);

            Book& b = book_[t.symbol_id];

            // Update leaky session VWAP proxy.
            b.num = b.num * cfg_.decay + cfg_.synth_volume * mid;
            b.den = b.den * cfg_.decay + cfg_.synth_volume;
            ++b.count;
            if (b.den <= 0.0) return;
            const double vwap = b.num / b.den;
            if (vwap <= 0.0) return;

            if (b.count < cfg_.warmup_ticks) return;   // warm-up

            const double dev = (mid - vwap) / vwap;

            switch (b.state) {
            case VwapState::FLAT:
                if (dev < -cfg_.enter_frac &&
                    b.inventory < cfg_.max_abs_inventory) {
                    // Mid is below VWAP -> expect reversion up -> go LONG.
                    emit_market(t, Side::BUY, cfg_.order_qty);
                    b.state = VwapState::LONG;
                } else if (dev > cfg_.enter_frac &&
                           b.inventory > -cfg_.max_abs_inventory) {
                    // Mid is above VWAP -> expect reversion down -> go SHORT.
                    emit_market(t, Side::SELL, cfg_.order_qty);
                    b.state = VwapState::SHORT;
                }
                break;

            case VwapState::LONG:
                // Reverted back to VWAP -> close the long.
                if (dev >= -cfg_.exit_frac) {
                    emit_market(t, Side::SELL, cfg_.order_qty);
                    b.state = VwapState::FLAT;
                }
                break;

            case VwapState::SHORT:
                // Reverted back to VWAP -> close the short.
                if (dev <= cfg_.exit_frac) {
                    emit_market(t, Side::BUY, cfg_.order_qty);
                    b.state = VwapState::FLAT;
                }
                break;
            }
        }

        void on_fill(const ExecReport& r) override {
            // Only real fills move inventory; skip non-trading terminal reports.
            const auto es = static_cast<connector::ExecStatus>(r.status);
            if (es == connector::ExecStatus::REJECTED ||
                es == connector::ExecStatus::CANCELED ||
                es == connector::ExecStatus::EXPIRED) return;
            // fill_quantity is signed (+buy / -sell); accumulate directly.
            book_[r.symbol_id].inventory += r.fill_quantity;
        }

        void on_kill_switch(connector::RejectReason) override { active_ = false; }
        void on_resume() override                             { active_ = true; }

        // Introspection helpers (tests / telemetry).
        const VwapConfig& config() const noexcept { return cfg_; }
        int64_t inventory(SymbolId s) const {
            auto it = book_.find(s);
            return it == book_.end() ? 0 : it->second.inventory;
        }
        double vwap(SymbolId s) const {
            auto it = book_.find(s);
            if (it == book_.end() || it->second.den <= 0.0) return 0.0;
            return it->second.num / it->second.den;
        }

    private:
        struct Book {
            double    num       = 0.0;   // Σ v·mid  (decayed)
            double    den       = 0.0;   // Σ v      (decayed)
            uint32_t  count     = 0;
            int64_t   inventory = 0;
            VwapState state     = VwapState::FLAT;
        };

        void emit_market(const Tick& t, Side side, int64_t qty) {
            Order o{};
            o.ts_ns           = now_ns();
            o.client_order_id = ++next_coid_;
            o.strategy_id     = static_cast<uint32_t>(id());
            o.symbol_id       = t.symbol_id;
            o.price_ticks     = -1;      // MARKET sentinel
            o.quantity        = (side == Side::BUY ? qty : -qty);
            o.side            = static_cast<uint8_t>(side);
            o.ord_type        = static_cast<uint8_t>(OrdType::MARKET);
            o.tif             = static_cast<uint8_t>(TIF::IOC);
            (void)sink_->send(o);
        }

        static uint64_t now_ns() {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
        }

        VwapConfig                        cfg_;
        OrderSink*                        sink_       = nullptr;
        bool                              active_     = true;
        uint64_t                          next_coid_  = 0;
        std::unordered_map<SymbolId, Book> book_;
    };

} // namespace gel::strategies
