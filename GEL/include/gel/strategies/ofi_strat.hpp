// include/gel/strategies/ofi_strat.hpp
//
// Order-Flow Imbalance (OFI) — top-of-book pressure taker.
//
// Reads the top-of-book bid/ask sizes from each BookUpdate and computes a
// normalized queue imbalance:
//
//     imbalance = (bidQ - askQ) / (bidQ + askQ)      in [-1, +1]
//
// A strongly positive imbalance means resting bid liquidity dwarfs the ask —
// buyers are lined up, price tends to tick up — so we go LONG. A strongly
// negative imbalance means the ask queue dominates — go SHORT. When the
// imbalance decays back inside a neutral band we flatten.
//
// The signal is smoothed with an EWMA so a single lumpy quote doesn't flip
// the book. A simple flat/long/short state machine keeps us from re-sending
// an order on every book update (we only act on state transitions).
//
// on_tick keeps a rolling mid so entries reference the current market and
// so the strategy still has a sane price if a book callback is sparse.
//
// Header-only, std-only, C++20/23.
//
#pragma once

#include "connector/strategy.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace gel::strategies {

    using connector::IStrategy;
    using connector::OrderSink;
    using connector::Order;
    using connector::Tick;
    using connector::BookUpdate;
    using connector::BookLevel;
    using connector::ExecReport;
    using connector::StrategyId;
    using connector::Side;
    using connector::OrdType;
    using connector::TIF;
    using connector::SymbolId;

    struct OfiConfig {
        // Enter a directional position once the smoothed imbalance crosses
        // +/- enter_threshold; unwind once it falls back inside exit_threshold.
        double  enter_threshold = 0.35;
        double  exit_threshold  = 0.10;
        double  ewma_alpha      = 0.20;   // imbalance smoothing rate
        int64_t base_qty        = 100;    // contracts per entry
        int64_t max_abs_position = 1000;  // hard position cap
        int64_t min_top_qty      = 1;     // ignore books with empty top level
    };

    // Per-symbol rolling state.
    struct OfiState {
        double   ewma_imb  = 0.0;   // smoothed imbalance
        bool     seeded    = false; // has ewma been initialized?
        int64_t  mid_ticks = 0;     // last known mid (from ticks or book)
        bool     have_mid  = false;
        int8_t   pos_state  = 0;    // -1 short, 0 flat, +1 long
        int64_t  position   = 0;    // signed net position from fills
    };

    class OrderFlowImbalanceStrategy final : public IStrategy {
    public:
        explicit OrderFlowImbalanceStrategy(OfiConfig cfg = OfiConfig{})
            : cfg_(cfg) {}

        StrategyId  id()   const noexcept override {
            return static_cast<StrategyId>(8);
        }
        std::string name() const override { return "OrderFlowImbalance"; }

        void on_start(OrderSink& sink) override { sink_ = &sink; }
        void on_stop() override                 { sink_ = nullptr; }

        // Keep a rolling mid so entries reference the live market.
        void on_tick(const Tick& t) override {
            if (!sink_) return;
            auto& st = state_[t.symbol_id];
            st.mid_ticks = (t.bid_ticks + t.ask_ticks) / 2;
            st.have_mid  = true;
        }

        // The heart of the strategy: order-flow imbalance from top-of-book.
        void on_book(const BookUpdate& b) override {
            if (!sink_ || !active_.load(std::memory_order_acquire)) return;

            const BookLevel& top_bid = b.bids[0];
            const BookLevel& top_ask = b.asks[0];
            const int64_t bidQ = top_bid.quantity;
            const int64_t askQ = top_ask.quantity;

            // Ignore degenerate / empty books.
            if (bidQ < cfg_.min_top_qty || askQ < cfg_.min_top_qty) return;

            auto& st = state_[b.symbol_id];

            // Refresh mid from the book top if we have valid prices.
            if (top_bid.price_ticks > 0 && top_ask.price_ticks > 0) {
                st.mid_ticks = (top_bid.price_ticks + top_ask.price_ticks) / 2;
                st.have_mid  = true;
            }
            if (!st.have_mid) return;

            const double denom = static_cast<double>(bidQ + askQ);
            const double imb    = static_cast<double>(bidQ - askQ) / denom;

            if (!st.seeded) { st.ewma_imb = imb; st.seeded = true; }
            else st.ewma_imb = (1.0 - cfg_.ewma_alpha) * st.ewma_imb
                             + cfg_.ewma_alpha * imb;

            step_state_machine(b.symbol_id, st);
        }

        // Track net position from execution feedback.
        void on_fill(const ExecReport& r) override {
            // Only real fills move position; skip non-trading terminal reports.
            const auto es = static_cast<connector::ExecStatus>(r.status);
            if (es == connector::ExecStatus::REJECTED ||
                es == connector::ExecStatus::CANCELED ||
                es == connector::ExecStatus::EXPIRED) return;
            state_[r.symbol_id].position += r.fill_quantity;
        }

        void on_kill_switch(connector::RejectReason) override {
            active_.store(false, std::memory_order_release);
        }
        void on_resume() override {
            active_.store(true, std::memory_order_release);
        }

        const OfiConfig& config()              const { return cfg_; }
        int64_t position(SymbolId s)           const {
            auto it = state_.find(s);
            return it == state_.end() ? 0 : it->second.position;
        }
        double imbalance(SymbolId s)           const {
            auto it = state_.find(s);
            return it == state_.end() ? 0.0 : it->second.ewma_imb;
        }

    private:
        // flat/long/short transitions — act only on a state change so we don't
        // spam an order on every book update.
        void step_state_machine(SymbolId sym, OfiState& st) {
            const double s = st.ewma_imb;

            switch (st.pos_state) {
            case 0: // FLAT — look for a strong signal either way.
                if (s >= cfg_.enter_threshold &&
                    st.position < cfg_.max_abs_position) {
                    emit_market(sym, st, Side::BUY, cfg_.base_qty);
                    st.pos_state = +1;
                } else if (s <= -cfg_.enter_threshold &&
                           st.position > -cfg_.max_abs_position) {
                    emit_market(sym, st, Side::SELL, cfg_.base_qty);
                    st.pos_state = -1;
                }
                break;

            case +1: // LONG — exit when imbalance decays; flip on strong reverse.
                if (s <= -cfg_.enter_threshold) {
                    emit_market(sym, st, Side::SELL, cfg_.base_qty);
                    st.pos_state = 0; // flatten this leg; next book may go short
                } else if (s < cfg_.exit_threshold) {
                    emit_market(sym, st, Side::SELL, cfg_.base_qty);
                    st.pos_state = 0;
                }
                break;

            case -1: // SHORT — mirror of the long branch.
                if (s >= cfg_.enter_threshold) {
                    emit_market(sym, st, Side::BUY, cfg_.base_qty);
                    st.pos_state = 0;
                } else if (s > -cfg_.exit_threshold) {
                    emit_market(sym, st, Side::BUY, cfg_.base_qty);
                    st.pos_state = 0;
                }
                break;
            default: break;
            }
        }

        void emit_market(SymbolId sym, const OfiState& st,
                         Side side, int64_t qty) {
            Order o{};
            o.ts_ns           = now_ns();
            o.client_order_id = next_coid_.fetch_add(1, std::memory_order_relaxed);
            o.strategy_id     = static_cast<uint32_t>(id());
            o.symbol_id       = sym;
            o.price_ticks     = -1; // MARKET
            o.quantity        = (side == Side::BUY ? qty : -qty);
            o.side            = static_cast<uint8_t>(side);
            o.ord_type        = static_cast<uint8_t>(OrdType::MARKET);
            o.tif             = static_cast<uint8_t>(TIF::IOC);
            (void)st;
            (void)sink_->send(o);
        }

        static uint64_t now_ns() {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
        }

        OfiConfig                            cfg_;
        OrderSink*                           sink_ = nullptr;
        std::atomic<bool>                    active_{true};
        std::atomic<uint64_t>                next_coid_{1};
        std::unordered_map<SymbolId, OfiState> state_;
    };

} // namespace gel::strategies
