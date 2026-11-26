// include/gel/strategies/market_maker.hpp
//
// Market Maker — Avellaneda-Stoikov inspired.
// Owner: Samet (deck slide 13).
//
// Quotes both sides of the book. Reservation price tracks the mid; spread
// widens with realized vol and skews against inventory.
//
//     r        = mid - q * γ * σ²        // reservation price
//     δ*       = γ * σ² + (2/γ) * ln(1 + γ/κ)
//     bid_px   = r - δ*/2
//     ask_px   = r + δ*/2
//
// Where:
//     mid = (bid + ask) / 2
//     q   = inventory (signed)
//     γ   = risk aversion
//     κ   = order arrival intensity
//     σ²  = realized variance over a rolling window
//
// This file ships an EWMA-based σ² estimator and an inventory-skewed
// quoter. The full A-S calibration (γ, κ tuning) is the "in-progress"
// work mentioned on slide 15.
//
#pragma once

#include "connector/strategy.hpp"
#include "gel/platform/telemetry.hpp"   // for now_ns()
#include <atomic>
#include <cmath>
#include <unordered_map>

namespace gel::strategies {

    using connector::IStrategy;
    using connector::OrderSink;
    using connector::Order;
    using connector::Tick;
    using connector::BookUpdate;
    using connector::ExecReport;
    using connector::StrategyId;
    using connector::Side;
    using connector::OrdType;
    using connector::TIF;
    using connector::SymbolId;

    struct MmConfig {
        double  gamma          = 0.1;     // risk aversion
        double  kappa          = 1.5;     // order arrival intensity
        double  sigma2_init    = 1e-7;    // initial realized variance
        double  ewma_alpha     = 0.05;    // σ² update rate
        int64_t base_quote_qty = 100;     // contracts per side
        int64_t max_abs_inventory = 5000; // hard inventory cap
        // Clamp on A-S half-spread output. The textbook formula's units
        // misbehave for FX-scale prices (e.g. EUR/USD ~ 1.07); without
        // a cap, the (2/γ)·ln(1+γ/κ) term can dominate and produce a
        // nonsense quote that the risk engine immediately rejects.
        // Defaults to 5 bps of mid — reasonable for liquid FX.
        double  max_half_spread_frac_of_mid = 0.0005;
        // Re-quote throttle. Without it the maker re-emits a fresh pair of
        // quotes on every single market tick (2 orders/tick). We only re-quote
        // when the mid has moved at least `requote_move_ticks` price_ticks
        // (one instrument "tick", 1 pip at the 1e-8 scale) OR at least
        // `requote_min_interval_ns` have elapsed since the last emit — whichever
        // comes first. The A-S quoting math itself is unchanged.
        int64_t  requote_move_ticks      = 10'000;      // 1 pip @ 1e-8 scale
        uint64_t requote_min_interval_ns = 250'000ULL;  // 0.25 ms staleness floor
    };

    class MarketMakerStrategy final : public IStrategy {
    public:
        explicit MarketMakerStrategy(MmConfig cfg = MmConfig{}) : cfg_(cfg) {}

        StrategyId  id()   const noexcept override { return StrategyId::MARKET_MAKER; }
        std::string name() const override          { return "MarketMaker"; }

        void on_start(OrderSink& sink) override { sink_ = &sink; }
        void on_stop() override                 { sink_ = nullptr; }

        void on_tick(const Tick& t) override {
            if (!sink_ || !active_.load(std::memory_order_acquire)) return;
            update_vol(t);
            if (!should_requote(t)) return;   // in-flight/movement throttle
            quote(t);
        }

        void on_book(const BookUpdate&) override {
            // L2-aware quoting goes here when we tune microstructure logic.
        }

        void on_fill(const ExecReport& r) override {
            // Order.quantity is signed: + for BUY, - for SELL. The router
            // copies it into ExecReport.fill_quantity unchanged, so adding
            // it directly to inventory does the right thing.
            inventory_[r.symbol_id] += r.fill_quantity;
        }

        void on_kill_switch(connector::RejectReason) override {
            active_.store(false, std::memory_order_release);
        }
        void on_resume() override {
            active_.store(true, std::memory_order_release);
        }

        const MmConfig& config()                     const { return cfg_; }
        int64_t         inventory(SymbolId s)        const {
            auto it = inventory_.find(s);
            return it == inventory_.end() ? 0 : it->second;
        }

    private:
        // Per-symbol last-emitted-quote bookkeeping for the re-quote throttle.
        struct QuoteGuard {
            int64_t  last_mid_ticks = 0;
            uint64_t last_emit_ns   = 0;
            bool     have           = false;
        };

        // Decide whether the market moved / enough time passed to re-quote.
        // Also records the decision so the next tick compares against this one.
        bool should_requote(const Tick& t) {
            const int64_t mid_ticks = (t.bid_ticks + t.ask_ticks) / 2;
            const uint64_t now = gel::platform::now_ns();
            QuoteGuard& g = quote_guard_[t.symbol_id];

            if (g.have) {
                int64_t d = mid_ticks - g.last_mid_ticks;
                if (d < 0) d = -d;
                const bool moved = d >= cfg_.requote_move_ticks;
                const bool stale = (now - g.last_emit_ns) >= cfg_.requote_min_interval_ns;
                if (!moved && !stale) return false;   // resting quotes still good
            }

            g.last_mid_ticks = mid_ticks;
            g.last_emit_ns   = now;
            g.have           = true;
            return true;
        }

        // EWMA realized variance on log returns of mid.
        void update_vol(const Tick& t) {
            const double mid = 0.5 * (connector::from_ticks(t.bid_ticks) +
                                       connector::from_ticks(t.ask_ticks));
            auto& last = last_mid_[t.symbol_id];
            if (last > 0.0) {
                const double r = std::log(mid / last);
                auto& s2 = sigma2_[t.symbol_id];
                if (s2 == 0.0) s2 = cfg_.sigma2_init;
                s2 = (1.0 - cfg_.ewma_alpha) * s2 + cfg_.ewma_alpha * r * r;
            }
            last = mid;
        }

        // Compute reservation price and optimal half-spread, push two LIMITs.
        void quote(const Tick& t) {
            const double mid    = 0.5 * (connector::from_ticks(t.bid_ticks) +
                                          connector::from_ticks(t.ask_ticks));
            const double sigma2 = sigma2_[t.symbol_id];
            const int64_t q     = inventory_[t.symbol_id];

            const double resv     = mid - q * cfg_.gamma * sigma2;
            double half_spd       = 0.5 * (cfg_.gamma * sigma2 +
                                            (2.0 / cfg_.gamma) *
                                                std::log(1.0 + cfg_.gamma / cfg_.kappa));
            const double cap = mid * cfg_.max_half_spread_frac_of_mid;
            if (half_spd > cap) half_spd = cap;

            // Hard inventory guard — skip the side that would worsen the position.
            const bool skip_buy  = q >=  cfg_.max_abs_inventory;
            const bool skip_sell = q <= -cfg_.max_abs_inventory;

            if (!skip_buy)  emit(t, resv - half_spd, Side::BUY);
            if (!skip_sell) emit(t, resv + half_spd, Side::SELL);
        }

        void emit(const Tick& t, double price, Side side) {
            Order o{};
            o.ts_ns           = gel::platform::now_ns();
            o.client_order_id = next_coid_.fetch_add(1, std::memory_order_relaxed);
            o.strategy_id     = static_cast<uint32_t>(id());
            o.symbol_id       = t.symbol_id;
            o.price_ticks     = connector::to_ticks(price);
            o.quantity        = (side == Side::BUY ? cfg_.base_quote_qty
                                                   : -cfg_.base_quote_qty);
            o.side            = static_cast<uint8_t>(side);
            o.ord_type        = static_cast<uint8_t>(OrdType::LIMIT);
            o.tif             = static_cast<uint8_t>(TIF::DAY);
            (void)sink_->send(o);
        }

        MmConfig                            cfg_;
        OrderSink*                          sink_   = nullptr;
        std::atomic<bool>                   active_{true};
        std::atomic<uint64_t>                next_coid_{1};
        std::unordered_map<SymbolId, double>  last_mid_;
        std::unordered_map<SymbolId, double>  sigma2_;
        std::unordered_map<SymbolId, int64_t> inventory_;
        std::unordered_map<SymbolId, QuoteGuard> quote_guard_;
    };

} // namespace gel::strategies
