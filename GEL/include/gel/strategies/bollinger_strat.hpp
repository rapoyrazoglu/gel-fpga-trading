// include/gel/strategies/bollinger_strat.hpp
//
// Bollinger Band mean-reversion strategy (id 7).
//
// Maintains a rolling window of the mid price. Each tick computes the
// N-tick simple moving average (the "middle band") and the population
// standard deviation. The bands are:
//
//     middle = SMA(N)
//     upper  = middle + k * stddev
//     lower  = middle - k * stddev
//
// Trading logic (simple flat/long/short state machine):
//   - Flat  + mid <= lower band  -> go LONG  (buy)
//   - Flat  + mid >= upper band  -> go SHORT (sell)
//   - Long  + mid >= middle band -> exit (flatten)
//   - Short + mid <= middle band -> exit (flatten)
//
// Header-only, std-only. Orders are LIMIT at the current touch price with
// a fixed base size; exits flatten the tracked position.
//
#pragma once

#include "connector/strategy.hpp"

#include <array>
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

    struct BollingerConfig {
        int      window        = 20;    // SMA / stddev lookback in ticks
        double   num_std       = 2.0;   // band width in standard deviations
        int64_t  base_qty      = 100;   // contracts per entry
        int64_t  max_abs_pos   = 1000;  // hard position cap
    };

    class BollingerStrategy final : public IStrategy {
    public:
        explicit BollingerStrategy(BollingerConfig cfg = BollingerConfig{})
            : cfg_(cfg) {}

        StrategyId  id()   const noexcept override {
            return static_cast<StrategyId>(7);
        }
        std::string name() const override { return "Bollinger"; }

        void on_start(OrderSink& sink) override { sink_ = &sink; }
        void on_stop()                 override { sink_ = nullptr; }

        void on_tick(const Tick& t) override {
            if (!sink_ || !active_) return;

            const int64_t mid = (t.bid_ticks + t.ask_ticks) / 2;

            State& s = state_[t.symbol_id];
            push(s, mid);

            // Need a full window before the bands are meaningful.
            if (s.count < cfg_.window) return;

            double mean = 0.0, var = 0.0;
            stats(s, mean, var);
            const double sd    = std::sqrt(var);
            // Degenerate window (constant mids) -> zero-width bands. Trading off
            // sd==0 would fire on any touch of the mean; skip until the window
            // actually has dispersion.
            if (!(sd > 0.0)) return;
            const double upper = mean + cfg_.num_std * sd;
            const double lower = mean - cfg_.num_std * sd;

            const double m = static_cast<double>(mid);
            const int64_t pos = s.position;

            if (pos == 0) {
                // Entry: touch of a band opens a mean-reversion position.
                if (m <= lower && can_add(pos, Side::BUY)) {
                    emit(t, mid, Side::BUY, cfg_.base_qty);
                } else if (m >= upper && can_add(pos, Side::SELL)) {
                    emit(t, mid, Side::SELL, cfg_.base_qty);
                }
            } else if (pos > 0) {
                // Long: exit when price reverts up to the middle band.
                if (m >= mean) emit(t, mid, Side::SELL, pos);
            } else {
                // Short: exit when price reverts down to the middle band.
                if (m <= mean) emit(t, mid, Side::BUY, -pos);
            }
        }

        void on_fill(const ExecReport& r) override {
            // Only real fills move position; skip non-trading terminal reports.
            const auto es = static_cast<connector::ExecStatus>(r.status);
            if (es == connector::ExecStatus::REJECTED ||
                es == connector::ExecStatus::CANCELED ||
                es == connector::ExecStatus::EXPIRED) return;
            // fill_quantity is signed (+buy / -sell) — matches position sign.
            state_[r.symbol_id].position += r.fill_quantity;
        }

        void on_kill_switch(connector::RejectReason) override { active_ = false; }
        void on_resume()                             override { active_ = true;  }

        const BollingerConfig& config() const { return cfg_; }
        int64_t position(SymbolId s) const {
            auto it = state_.find(s);
            return it == state_.end() ? 0 : it->second.position;
        }

    private:
        static constexpr int kMaxWindow = 512;

        struct State {
            std::array<int64_t, kMaxWindow> buf{};
            int      head     = 0;   // next write slot
            int      count    = 0;   // valid samples (<= window)
            int64_t  sum      = 0;   // running sum of buffered values
            int64_t  position = 0;   // signed inventory
        };

        void push(State& s, int64_t v) {
            const int w = window();
            if (s.count == w) {
                s.sum -= s.buf[s.head];   // evict oldest
            } else {
                ++s.count;
            }
            s.buf[s.head] = v;
            s.sum += v;
            s.head = (s.head + 1) % w;
        }

        // Mean + population variance over the live window.
        void stats(const State& s, double& mean, double& var) const {
            // Guard the empty window: no samples means no dispersion and a
            // division by zero below. Report a flat, zero-variance band.
            if (s.count == 0) { mean = 0.0; var = 0.0; return; }
            mean = static_cast<double>(s.sum) / static_cast<double>(s.count);
            double acc = 0.0;
            for (int i = 0; i < s.count; ++i) {
                const double d = static_cast<double>(s.buf[i]) - mean;
                acc += d * d;
            }
            var = acc / static_cast<double>(s.count);
        }

        int window() const {
            int w = cfg_.window;
            if (w < 1) w = 1;
            if (w > kMaxWindow) w = kMaxWindow;
            return w;
        }

        bool can_add(int64_t pos, Side side) const {
            const int64_t next = pos + (side == Side::BUY ? cfg_.base_qty
                                                          : -cfg_.base_qty);
            return next <= cfg_.max_abs_pos && next >= -cfg_.max_abs_pos;
        }

        void emit(const Tick& t, int64_t price_ticks, Side side, int64_t qty) {
            Order o{};
            o.ts_ns           = t.ts_ns;
            o.client_order_id = next_coid_++;
            o.strategy_id     = static_cast<uint32_t>(id());
            o.symbol_id       = t.symbol_id;
            o.price_ticks     = price_ticks;
            o.quantity        = (side == Side::BUY ? qty : -qty);
            o.side            = static_cast<uint8_t>(side);
            o.ord_type        = static_cast<uint8_t>(OrdType::LIMIT);
            o.tif             = static_cast<uint8_t>(TIF::DAY);
            (void)sink_->send(o);
        }

        BollingerConfig                       cfg_;
        OrderSink*                            sink_      = nullptr;
        bool                                  active_    = true;
        uint64_t                              next_coid_ = 1;
        std::unordered_map<SymbolId, State>   state_;
    };

} // namespace gel::strategies
