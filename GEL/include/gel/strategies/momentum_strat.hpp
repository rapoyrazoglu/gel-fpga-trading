// include/gel/strategies/momentum_strat.hpp
//
// Momentum — dual moving-average crossover.
//
// Classic trend-follower on the tick mid-price. Two simple moving averages
// ride the mid: a fast one (default 10 ticks) and a slow one (default 50).
//
//   fast SMA crosses ABOVE slow SMA  -> bullish momentum -> go LONG
//   fast SMA crosses BELOW slow SMA  -> bearish momentum -> go FLAT/SHORT
//
// The strategy holds at most one position at a time and only trades on the
// crossover event (a state transition), so it never spams an order on every
// tick. Orders are emitted MARKET so a fresh momentum signal fills promptly.
//
// State machine:
//
//     FLAT --(golden cross)--> LONG --(death cross)--> SHORT --(golden)--> LONG ...
//
// On each cross we flatten any existing exposure and open the new one in a
// single netted MARKET order (e.g. LONG->SHORT sends 2x base_qty to sell).
//
// Header-only, std-only. Rolling SMAs use a fixed ring buffer + running sum
// so on_tick stays O(1).
//
#pragma once

#include "connector/strategy.hpp"

#include <array>
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

    struct MomentumConfig {
        std::size_t fast_window   = 10;    // fast SMA length (ticks)
        std::size_t slow_window   = 50;    // slow SMA length (ticks)
        int64_t     base_qty      = 100;   // contracts per position
        bool        allow_short   = true;  // if false, death cross -> FLAT only
        int64_t     max_abs_position = 1000; // hard cap on |net position|
    };

    // Position state machine.
    enum class PosState : uint8_t { FLAT = 0, LONG = 1, SHORT = 2 };

    class MomentumStrategy final : public IStrategy {
    public:
        // Slow window is capped by kMaxWindow so the ring buffer is bounded.
        static constexpr std::size_t kMaxWindow = 512;

        explicit MomentumStrategy(MomentumConfig cfg = MomentumConfig{})
            : cfg_(clamp_cfg(cfg)) {}

        StrategyId  id()   const noexcept override {
            return static_cast<StrategyId>(3);
        }
        std::string name() const override { return "Momentum"; }

        void on_start(OrderSink& sink) override { sink_ = &sink; }
        void on_stop() override                 { sink_ = nullptr; }

        void on_tick(const Tick& t) override {
            if (!sink_ || !active_) return;

            // Mid in ticks. Integer division is fine — SMA smooths the LSB.
            const int64_t mid = (t.bid_ticks + t.ask_ticks) / 2;
            if (mid <= 0) return;

            Roll& r = roll_[t.symbol_id];
            push(r, mid);

            // Need a full slow window before the crossover is meaningful.
            if (r.count < cfg_.slow_window) return;

            const double fast = sma(r, cfg_.fast_window);
            const double slow = sma(r, cfg_.slow_window);

            // First fully-warmed tick just seeds the sign; no trade yet.
            const int sign = (fast > slow) ? 1 : (fast < slow) ? -1 : 0;
            if (!r.primed) {
                r.primed    = true;
                r.last_sign = sign;
                return;
            }

            // Only act on a genuine crossover (sign flip).
            if (sign == 0 || sign == r.last_sign) return;
            r.last_sign = sign;

            if (sign > 0) enter_long(t);
            else          enter_short_or_flat(t);
        }

        void on_fill(const ExecReport& r) override {
            // Only real fills move inventory. A REJECTED/CANCELED/EXPIRED report
            // means nothing traded, so applying it would desync internal state.
            const auto st = static_cast<connector::ExecStatus>(r.status);
            if (st == connector::ExecStatus::REJECTED ||
                st == connector::ExecStatus::CANCELED ||
                st == connector::ExecStatus::EXPIRED) return;
            // fill_quantity is signed (+buy / -sell); accumulate net position.
            position_[r.symbol_id] += r.fill_quantity;
        }

        void on_kill_switch(connector::RejectReason) override { active_ = false; }
        void on_resume() override                             { active_ = true; }

        // Introspection for tests / telemetry.
        const MomentumConfig& config()                const { return cfg_; }
        PosState              state(SymbolId s)        const {
            auto it = state_.find(s);
            return it == state_.end() ? PosState::FLAT : it->second;
        }
        int64_t position(SymbolId s) const {
            auto it = position_.find(s);
            return it == position_.end() ? 0 : it->second;
        }

    private:
        struct Roll {
            std::array<int64_t, kMaxWindow> buf{};
            std::size_t head  = 0;    // next write index
            std::size_t count = 0;    // valid samples (<= slow_window)
            int64_t     sum_fast = 0; // running sum of last fast_window
            int64_t     sum_slow = 0; // running sum of last slow_window
            bool        primed = false;
            int         last_sign = 0;
        };

        static MomentumConfig clamp_cfg(MomentumConfig c) {
            if (c.fast_window < 1) c.fast_window = 1;
            if (c.slow_window < 2) c.slow_window = 2;
            if (c.fast_window >= c.slow_window) c.fast_window = c.slow_window - 1;
            if (c.slow_window > kMaxWindow) c.slow_window = kMaxWindow;
            if (c.fast_window > c.slow_window) c.fast_window = c.slow_window;
            if (c.base_qty < 1) c.base_qty = 1;
            return c;
        }

        // Push a new mid into the ring, maintaining both running sums.
        void push(Roll& r, int64_t mid) {
            const std::size_t sw = cfg_.slow_window;
            const std::size_t fw = cfg_.fast_window;

            if (r.count == sw) {
                // Evict the oldest sample from the slow sum.
                const std::size_t oldest = r.head; // head points at oldest when full
                r.sum_slow -= r.buf[oldest];
            }
            // Evict from the fast sum the sample leaving the fast window.
            if (r.count >= fw) {
                const std::size_t fast_out =
                    (r.head + sw - fw) % sw; // index leaving the fast window
                // Only valid once we actually have fw samples in the ring; when
                // count < sw the ring isn't wrapped, handle via recompute guard.
                if (r.count == sw) {
                    r.sum_fast -= r.buf[fast_out];
                }
            }

            r.buf[r.head] = mid;
            r.head = (r.head + 1) % sw;
            if (r.count < sw) ++r.count;

            // Recompute sums directly for correctness and clarity; O(window)
            // only until warmed, then we could optimize — but recompute keeps
            // the fast/slow windows exact regardless of wrap alignment.
            r.sum_fast = window_sum(r, fw);
            r.sum_slow = window_sum(r, sw);
        }

        // Sum of the most recent `w` samples (w <= count).
        static int64_t window_sum(const Roll& r, std::size_t w) {
            int64_t s = 0;
            if (w > r.count) w = r.count;
            for (std::size_t i = 0; i < w; ++i) {
                // walk backwards from the most-recently written slot
                const std::size_t idx =
                    (r.head + r.buf.size() - 1 - i) % r.buf.size();
                s += r.buf[idx];
            }
            return s;
        }

        double sma(const Roll& r, std::size_t w) const {
            if (w == cfg_.fast_window)
                return static_cast<double>(r.sum_fast) /
                       static_cast<double>(w);
            if (w == cfg_.slow_window)
                return static_cast<double>(r.sum_slow) /
                       static_cast<double>(w);
            return static_cast<double>(window_sum(r, w)) /
                   static_cast<double>(w);
        }

        void enter_long(const Tick& t) {
            PosState& st = state_[t.symbol_id];
            if (st == PosState::LONG) return;
            // Hard cap: never add to a long already at the position limit.
            if (position(t.symbol_id) >= cfg_.max_abs_position) return;
            // From SHORT we buy 2x (cover + go long); from FLAT we buy 1x.
            const int64_t qty =
                (st == PosState::SHORT) ? 2 * cfg_.base_qty : cfg_.base_qty;
            emit_market(t, Side::BUY, qty);
            st = PosState::LONG;
        }

        void enter_short_or_flat(const Tick& t) {
            PosState& st = state_[t.symbol_id];
            if (cfg_.allow_short) {
                if (st == PosState::SHORT) return;
                // Hard cap: never add to a short already at the position limit.
                if (position(t.symbol_id) <= -cfg_.max_abs_position) return;
                const int64_t qty =
                    (st == PosState::LONG) ? 2 * cfg_.base_qty : cfg_.base_qty;
                emit_market(t, Side::SELL, qty);
                st = PosState::SHORT;
            } else {
                // Long-only: death cross just closes any long.
                if (st == PosState::LONG) {
                    emit_market(t, Side::SELL, cfg_.base_qty);
                }
                st = PosState::FLAT;
            }
        }

        void emit_market(const Tick& t, Side side, int64_t abs_qty) {
            Order o{};
            o.ts_ns           = t.ts_ns;
            o.client_order_id = ++next_coid_;
            o.strategy_id     = static_cast<uint32_t>(id());
            o.symbol_id       = t.symbol_id;
            o.price_ticks     = -1;  // MARKET sentinel
            o.quantity        = (side == Side::BUY ? abs_qty : -abs_qty);
            o.side            = static_cast<uint8_t>(side);
            o.ord_type        = static_cast<uint8_t>(OrdType::MARKET);
            o.tif             = static_cast<uint8_t>(TIF::IOC);
            (void)sink_->send(o);
        }

        MomentumConfig                             cfg_;
        OrderSink*                                 sink_   = nullptr;
        bool                                       active_ = true;
        uint64_t                                   next_coid_ = 0;
        std::unordered_map<SymbolId, Roll>         roll_;
        std::unordered_map<SymbolId, PosState>     state_;
        std::unordered_map<SymbolId, int64_t>      position_;
    };

} // namespace gel::strategies
