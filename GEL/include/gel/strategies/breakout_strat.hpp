// include/gel/strategies/breakout_strat.hpp
//
// Breakout — Donchian channel breakout trend follower.
//
// Tracks the rolling max/min of the mid price over a fixed lookback window
// (default 50 ticks). When the mid breaks ABOVE the prior window high we go
// long; when it breaks BELOW the prior window low we go short. Position is
// closed by a symmetric trailing stop that ratchets in the direction of the
// trade — a long exits when price falls a fixed fraction below the best mid
// seen since entry, a short exits when price rallies above the lowest mid
// seen since entry.
//
// State machine per symbol: FLAT -> LONG / SHORT -> FLAT. We only emit an
// order on a transition, never every tick, so the strategy does not spam the
// ring when price wanders inside the channel.
//
// Header-only, std-only, C++20/23.
//
#pragma once

#include "connector/strategy.hpp"
#include <array>
#include <atomic>
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

    struct BreakoutConfig {
        // Donchian lookback in ticks. The current mid is compared against the
        // extreme of the PRIOR `window` mids (excluding the current one).
        std::size_t window          = 50;
        // Trailing-stop distance, expressed as a fraction of the entry mid.
        // e.g. 0.0020 = 20 bps. A long is closed once mid drops this far
        // below the peak mid since entry.
        double      trail_frac      = 0.0020;
        // Contracts per position (absolute). Sign is applied from side.
        int64_t     order_qty       = 100;
        // Minimum ticks a channel must span before a breakout is actionable,
        // guards against firing on a flat/degenerate window at warm-up.
        int64_t     min_channel_ticks = 1;
        // Hard cap on |net inventory| the state machine will build.
        int64_t     max_abs_position  = 1000;
    };

    class BreakoutStrategy final : public IStrategy {
    public:
        explicit BreakoutStrategy(BreakoutConfig cfg = BreakoutConfig{})
            : cfg_(cfg) {}

        StrategyId  id()   const noexcept override {
            return static_cast<StrategyId>(5);
        }
        std::string name() const override { return "Breakout"; }

        void on_start(OrderSink& sink) override { sink_ = &sink; }
        void on_stop()                 override { sink_ = nullptr; }

        void on_tick(const Tick& t) override {
            if (!sink_ || !active_.load(std::memory_order_acquire)) return;

            const int64_t mid = (t.bid_ticks + t.ask_ticks) / 2;
            SymState& s = state_[t.symbol_id];

            // Warm-up: need a full prior window before any breakout is valid.
            if (s.count < cfg_.window) {
                // Establish running extremes over the accumulating window.
                if (s.count == 0) { s.hi = mid; s.lo = mid; }
                else {
                    if (mid > s.hi) s.hi = mid;
                    if (mid < s.lo) s.lo = mid;
                }
                push(s, mid);
                return;
            }

            // Extremes of the PRIOR window (state built from previous ticks).
            const int64_t prior_hi = s.hi;
            const int64_t prior_lo = s.lo;
            const bool channel_ok  = (prior_hi - prior_lo) >= cfg_.min_channel_ticks;

            switch (s.pos) {
                case Pos::FLAT: {
                    if (channel_ok && mid > prior_hi &&
                        inventory(t.symbol_id) < cfg_.max_abs_position) {
                        enter(t.symbol_id, mid, Side::BUY);
                        s.pos       = Pos::LONG;
                        s.extreme   = mid;   // peak mid since entry
                    } else if (channel_ok && mid < prior_lo &&
                               inventory(t.symbol_id) > -cfg_.max_abs_position) {
                        enter(t.symbol_id, mid, Side::SELL);
                        s.pos       = Pos::SHORT;
                        s.extreme   = mid;   // trough mid since entry
                    }
                    break;
                }
                case Pos::LONG: {
                    if (mid > s.extreme) s.extreme = mid;
                    const int64_t stop = trail_stop(s.extreme, /*below=*/true);
                    if (mid <= stop) {
                        // Close long by selling.
                        enter(t.symbol_id, mid, Side::SELL);
                        s.pos = Pos::FLAT;
                    }
                    break;
                }
                case Pos::SHORT: {
                    if (mid < s.extreme) s.extreme = mid;
                    const int64_t stop = trail_stop(s.extreme, /*below=*/false);
                    if (mid >= stop) {
                        // Close short by buying.
                        enter(t.symbol_id, mid, Side::BUY);
                        s.pos = Pos::FLAT;
                    }
                    break;
                }
            }

            // Roll the window AFTER the decision so `prior_*` truly excluded
            // the current tick.
            push(s, mid);
        }

        void on_fill(const ExecReport& r) override {
            // Only real fills move inventory; skip non-trading terminal reports.
            const auto st = static_cast<connector::ExecStatus>(r.status);
            if (st == connector::ExecStatus::REJECTED ||
                st == connector::ExecStatus::CANCELED ||
                st == connector::ExecStatus::EXPIRED) return;
            // Order.quantity is signed (+buy/-sell); router copies it into
            // fill_quantity unchanged, so this tracks net position.
            inventory_[r.symbol_id] += r.fill_quantity;
        }

        void on_kill_switch(connector::RejectReason) override {
            active_.store(false, std::memory_order_release);
        }
        void on_resume() override {
            active_.store(true, std::memory_order_release);
        }

        int64_t inventory(SymbolId s) const {
            auto it = inventory_.find(s);
            return it == inventory_.end() ? 0 : it->second;
        }
        const BreakoutConfig& config() const { return cfg_; }

    private:
        enum class Pos : uint8_t { FLAT = 0, LONG = 1, SHORT = 2 };

        struct SymState {
            // Ring buffer of the last `window` mids so we can recompute the
            // rolling extremes as ticks age out.
            std::array<int64_t, 512> buf{};
            std::size_t head    = 0;      // next write slot
            std::size_t count   = 0;      // filled slots (<= window)
            int64_t     hi      = 0;      // max over current stored window
            int64_t     lo      = 0;      // min over current stored window
            Pos         pos     = Pos::FLAT;
            int64_t     extreme = 0;      // peak/trough mid since entry
        };

        int64_t trail_stop(int64_t extreme, bool below) const {
            // Distance in ticks from the running extreme.
            const int64_t dist =
                static_cast<int64_t>(static_cast<double>(extreme) * cfg_.trail_frac);
            return below ? (extreme - dist) : (extreme + dist);
        }

        void push(SymState& s, int64_t mid) {
            const std::size_t cap =
                (cfg_.window < s.buf.size()) ? cfg_.window : s.buf.size();
            s.buf[s.head] = mid;
            s.head = (s.head + 1) % cap;
            if (s.count < cap) ++s.count;

            // Recompute rolling extremes over the live window. Window is small
            // and bounded (<=512) so a linear scan is cheap and exact.
            int64_t hi = s.buf[0], lo = s.buf[0];
            for (std::size_t i = 0; i < s.count; ++i) {
                const int64_t v = s.buf[i];
                if (v > hi) hi = v;
                if (v < lo) lo = v;
            }
            s.hi = hi;
            s.lo = lo;
        }

        void enter(SymbolId sym, int64_t mid, Side side) {
            Order o{};
            o.ts_ns           = 0;   // engine stamps authoritative time
            o.client_order_id = next_coid_.fetch_add(1, std::memory_order_relaxed);
            o.strategy_id     = static_cast<uint32_t>(id());
            o.symbol_id       = sym;
            o.price_ticks     = mid;  // marketable limit at mid
            o.quantity        = (side == Side::BUY ? cfg_.order_qty
                                                   : -cfg_.order_qty);
            o.side            = static_cast<uint8_t>(side);
            o.ord_type        = static_cast<uint8_t>(OrdType::LIMIT);
            o.tif             = static_cast<uint8_t>(TIF::IOC);
            (void)sink_->send(o);
        }

        BreakoutConfig                         cfg_;
        OrderSink*                             sink_ = nullptr;
        std::atomic<bool>                      active_{true};
        std::atomic<uint64_t>                  next_coid_{1};
        std::unordered_map<SymbolId, SymState> state_;
        std::unordered_map<SymbolId, int64_t>  inventory_;
    };

} // namespace gel::strategies
