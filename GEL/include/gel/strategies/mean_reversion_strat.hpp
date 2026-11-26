// include/gel/strategies/mean_reversion_strat.hpp
//
// Mean Reversion — statistical fade of the mid price.
//
// Tracks a rolling mean + standard deviation of the mid over a fixed
// window (Welford-free ring buffer so the estimate is exact, not EWMA).
// When the mid stretches beyond a z-score band the strategy fades it:
//
//     z = (mid - mean) / std
//
//     z >= +entry_z   -> the market is rich   -> go SHORT (SELL)
//     z <= -entry_z   -> the market is cheap   -> go LONG  (BUY)
//     |z| <= exit_z   -> mid reverted to mean  -> flatten the position
//
// A simple flat/long/short state machine keeps us from spamming an order
// on every tick: we only emit on a genuine state transition. Position is
// tracked from fills (Order.quantity is signed, copied verbatim into
// ExecReport.fill_quantity by the router).
//
// Header-only, std-only. C++20/23.
//
#pragma once

#include "connector/strategy.hpp"
#include <array>
#include <atomic>
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

    struct MeanReversionConfig {
        // Rolling window length (ticks) used for mean/std.
        std::size_t window       = 32;
        // Entry band in z-score units. Deck spec: 2 sigma.
        double  entry_z          = 2.0;
        // Exit band: flatten once the mid comes back inside this band.
        double  exit_z           = 0.25;
        // Contracts per entry.
        int64_t order_qty        = 100;
        // Floor on std (in ticks) so a flat market can't blow z up to inf.
        double  min_std_ticks    = 1.0;
        // Hard cap on |net inventory| the state machine will build.
        int64_t max_abs_position = 1000;
    };

    class MeanReversionStrategy final : public IStrategy {
    public:
        explicit MeanReversionStrategy(MeanReversionConfig cfg = MeanReversionConfig{})
            : cfg_(cfg) {}

        StrategyId  id()   const noexcept override {
            return static_cast<StrategyId>(4);
        }
        std::string name() const override { return "MeanReversion"; }

        void on_start(OrderSink& sink) override { sink_ = &sink; }
        void on_stop() override                 { sink_ = nullptr; }

        void on_tick(const Tick& t) override {
            if (!sink_ || !active_.load(std::memory_order_acquire)) return;

            const double mid = 0.5 * (static_cast<double>(t.bid_ticks) +
                                      static_cast<double>(t.ask_ticks));

            auto& st = state_[t.symbol_id];
            push(st, mid);

            // Need a full window before trusting the statistics.
            if (st.count < cfg_.window) return;

            double mean = 0.0, var = 0.0;
            stats(st, mean, var);
            // Floor the VARIANCE (not just the std): a perfectly constant window
            // yields var==0, and if min_std_ticks is configured to 0 the std
            // floor below would leave sd==0 and divide the z-score by zero.
            if (var < kMinVar) var = kMinVar;
            double sd = std::sqrt(var);
            if (sd < cfg_.min_std_ticks) sd = cfg_.min_std_ticks;

            const double z = (mid - mean) / sd;

            switch (st.pos) {
                case Pos::FLAT:
                    if (z >= cfg_.entry_z) {
                        // Rich -> fade short (unless already at the short cap).
                        if (inventory(t.symbol_id) > -cfg_.max_abs_position) {
                            emit(t, Side::SELL, OrdType::MARKET, t.bid_ticks);
                            st.pos = Pos::SHORT;
                        }
                    } else if (z <= -cfg_.entry_z) {
                        // Cheap -> fade long (unless already at the long cap).
                        if (inventory(t.symbol_id) < cfg_.max_abs_position) {
                            emit(t, Side::BUY, OrdType::MARKET, t.ask_ticks);
                            st.pos = Pos::LONG;
                        }
                    }
                    break;

                case Pos::LONG:
                    // Exit when mid climbs back to (or through) the mean.
                    if (z >= -cfg_.exit_z) {
                        emit(t, Side::SELL, OrdType::MARKET, t.bid_ticks);
                        st.pos = Pos::FLAT;
                    }
                    break;

                case Pos::SHORT:
                    if (z <= cfg_.exit_z) {
                        emit(t, Side::BUY, OrdType::MARKET, t.ask_ticks);
                        st.pos = Pos::FLAT;
                    }
                    break;
            }
        }

        void on_fill(const ExecReport& r) override {
            // Only real fills move inventory; skip non-trading terminal reports.
            const auto st = static_cast<connector::ExecStatus>(r.status);
            if (st == connector::ExecStatus::REJECTED ||
                st == connector::ExecStatus::CANCELED ||
                st == connector::ExecStatus::EXPIRED) return;
            // Signed fill quantity folds straight into inventory.
            inventory_[r.symbol_id] += r.fill_quantity;
        }

        void on_kill_switch(connector::RejectReason) override {
            active_.store(false, std::memory_order_release);
        }
        void on_resume() override {
            active_.store(true, std::memory_order_release);
        }

        const MeanReversionConfig& config() const { return cfg_; }
        int64_t inventory(SymbolId s) const {
            auto it = inventory_.find(s);
            return it == inventory_.end() ? 0 : it->second;
        }

    private:
        static constexpr std::size_t kMaxWindow = 512;
        // Absolute variance floor so the z-score divisor is never zero even if
        // both the window is flat and min_std_ticks is configured to 0.
        static constexpr double kMinVar = 1e-9;

        enum class Pos : uint8_t { FLAT, LONG, SHORT };

        struct SymState {
            std::array<double, kMaxWindow> buf{};
            std::size_t head  = 0;   // next write slot
            std::size_t count = 0;   // valid samples (<= window)
            Pos         pos   = Pos::FLAT;
        };

        void push(SymState& st, double mid) {
            const std::size_t w = window_capped();
            st.buf[st.head] = mid;
            st.head = (st.head + 1) % w;
            if (st.count < w) ++st.count;
        }

        void stats(const SymState& st, double& mean, double& var) const {
            const std::size_t n = st.count;
            double sum = 0.0;
            for (std::size_t i = 0; i < n; ++i) sum += st.buf[i];
            mean = sum / static_cast<double>(n);
            double acc = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                const double d = st.buf[i] - mean;
                acc += d * d;
            }
            var = (n > 1) ? acc / static_cast<double>(n - 1) : 0.0;
        }

        std::size_t window_capped() const {
            std::size_t w = cfg_.window;
            if (w == 0) w = 1;
            if (w > kMaxWindow) w = kMaxWindow;
            return w;
        }

        void emit(const Tick& t, Side side, OrdType ot, int64_t px_ticks) {
            Order o{};
            o.ts_ns           = now_ns();
            o.client_order_id = next_coid_.fetch_add(1, std::memory_order_relaxed);
            o.strategy_id     = static_cast<uint32_t>(id());
            o.symbol_id       = t.symbol_id;
            o.price_ticks     = (ot == OrdType::MARKET) ? -1 : px_ticks;
            o.quantity        = (side == Side::BUY ? cfg_.order_qty
                                                   : -cfg_.order_qty);
            o.side            = static_cast<uint8_t>(side);
            o.ord_type        = static_cast<uint8_t>(ot);
            o.tif             = static_cast<uint8_t>(TIF::IOC);
            (void)sink_->send(o);
        }

        static uint64_t now_ns() {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
        }

        MeanReversionConfig                    cfg_;
        OrderSink*                             sink_ = nullptr;
        std::atomic<bool>                      active_{true};
        std::atomic<uint64_t>                  next_coid_{1};
        std::unordered_map<SymbolId, SymState> state_;
        std::unordered_map<SymbolId, int64_t>  inventory_;
    };

} // namespace gel::strategies
