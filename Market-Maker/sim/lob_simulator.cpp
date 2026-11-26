// MMX — sim/lob_simulator.cpp

#include "lob_simulator.h"

#include <algorithm>
#include <cmath>

#include "host/pricing/pricing_engine.h"

namespace mmx {

LobSimulator::LobSimulator(SimParams params,
                           std::vector<std::uint8_t> hmac_key)
    : params_(params),
      hmac_key_(std::move(hmac_key)),
      compiler_(hmac_key_),
      ewma_(0.94),
      pnl_(params_.tick_size) {}

namespace {
struct PendingFill {
    std::uint64_t ts_ns;
    int32_t       price_ticks;
    int8_t        side;       // +1 buy, -1 sell
    uint32_t      qty;
    int32_t       mid_at_fill;
};
}  // namespace

SimResult LobSimulator::run() {
    if (events_.empty()) return {};

    SimResult res;
    std::vector<double> pnl_path;
    pnl_path.reserve(events_.size());

    std::uint64_t last_refresh_ns = 0;
    std::uint64_t last_event_ns   = events_.front().ts_ns;
    double quote_age_accum_ms = 0.0;
    int    quote_age_samples  = 0;
    int    bid_quotes_seen    = 0;
    int    ask_quotes_seen    = 0;
    double inventory_int      = 0.0;

    int32_t  outstanding_bid = 0;
    int32_t  outstanding_ask = 0;
    std::uint64_t bid_placed_ns = 0;
    std::uint64_t ask_placed_ns = 0;
    bool   bid_active = false;
    bool   ask_active = false;

    std::vector<PendingFill> fills_log;

    // Mid history, so the maker can price off a stale mid (reaction latency).
    std::vector<int32_t> mid_hist;
    mid_hist.reserve(events_.size());

    // Order-flow-imbalance signal (causal EWMA of signed aggressor volume).
    double ofi = 0.0;

    auto place_quotes = [&](int32_t bid, int32_t ask, uint32_t qty,
                            const LobEvent& ev) {
        // Cancel old.
        if (bid_active) bid_q_.cancel();
        if (ask_active) ask_q_.cancel();

        // Join behind whatever's resting at our level. If our quote is
        // through-market (better than best on our side), we're at the front.
        const uint32_t bid_q_ahead =
            (bid >= ev.bid_ticks) ? 0u : ev.bid_qty;
        const uint32_t ask_q_ahead =
            (ask <= ev.ask_ticks) ? 0u : ev.ask_qty;
        bid_q_.place(bid, qty, bid_q_ahead);
        ask_q_.place(ask, qty, ask_q_ahead);
        outstanding_bid = bid;
        outstanding_ask = ask;
        bid_placed_ns   = ev.ts_ns;
        ask_placed_ns   = ev.ts_ns;
        bid_active = ask_active = true;
        ++bid_quotes_seen;
        ++ask_quotes_seen;
    };

    for (const auto& ev : events_) {
        const int32_t mid = (ev.bid_ticks + ev.ask_ticks) / 2;
        ewma_.update(static_cast<double>(mid));

        // The mid the maker actually prices off — delayed by its reaction
        // latency. PnL is always marked at the true `mid`; quoting off a stale
        // mid is what exposes the maker to adverse selection.
        mid_hist.push_back(mid);
        const int qi = static_cast<int>(mid_hist.size()) - 1
                       - params_.quote_latency_events;
        const int32_t quote_mid = (qi >= 0) ? mid_hist[static_cast<std::size_t>(qi)]
                                            : mid_hist.front();

        // κ: pretend each trade is a fresh arrival at distance |trade_price - mid|.
        if (ev.trade_qty > 0) {
            int dist = std::abs(ev.trade_price - mid);
            kappa_.record_arrival(dist, ev.ts_ns);
        }

        // Refresh the param block periodically.
        if (ev.ts_ns - last_refresh_ns >= params_.refresh_period_ns ||
            last_refresh_ns == 0) {
            const double dt_sec =
                static_cast<double>(params_.refresh_period_ns) / 1e9;
            const double sigma2 = ewma_.sigma2_per_sec(dt_sec);
            const double kappa  = std::max(1e-3, kappa_.refit(ev.ts_ns));

            ParamCompilerInputs in{};
            in.sigma2_per_sec = sigma2;
            in.kappa          = kappa;
            in.horizon_sec    = params_.horizon_sec;
            in.base_gamma     = params_.gamma;
            in.regime_id      = kRegimeRanging;
            in.breaker_tripped= false;
            in.max_inv        = params_.max_inv;
            in.base_qty       = params_.base_qty;
            in.requote_ticks  = params_.requote_ticks;
            in.tick_size      = 1.0;  // we work in ticks already
            in.now_ns         = ev.ts_ns;
            compiler_.compile(in, &last_block_);
            last_refresh_ns = ev.ts_ns;
        }

        // Compute fresh quote and (re)place if needed. Priced off the
        // latency-delayed mid (quote_mid), not the instantaneous mid.
        QuoteOut q = compute_quote(quote_mid, pnl_.inventory(), last_block_);

        // Order-flow-imbalance defence overlay (causal, uses only the OFI
        // accumulated from past trades). Lean quotes WITH recent flow and widen
        // the vulnerable side when flow turns toxic. Does not touch the FPGA
        // kernel — it is a slow-path adjustment of the posted quote.
        if (!q.cancel_only &&
            (params_.ofi_skew_k != 0.0 || params_.ofi_widen_k != 0.0)) {
            int32_t skew  = static_cast<int32_t>(std::lround(params_.ofi_skew_k * ofi));
            int32_t widen = static_cast<int32_t>(std::lround(
                                params_.ofi_widen_k * std::fabs(ofi)));
            const int32_t cap = params_.ofi_max_skew_ticks;
            if (skew >  cap) skew =  cap; if (skew < -cap) skew = -cap;
            if (widen > cap) widen = cap; if (widen < 0)   widen = 0;
            q.bid += skew - widen;
            q.ask += skew + widen;
        }
        bool need =
            !bid_active || !ask_active || q.cancel_only ||
            std::abs(q.bid - outstanding_bid) >
                static_cast<int32_t>(last_block_.requote_ticks) ||
            std::abs(q.ask - outstanding_ask) >
                static_cast<int32_t>(last_block_.requote_ticks);
        if (q.cancel_only) {
            if (bid_active) bid_q_.cancel();
            if (ask_active) ask_q_.cancel();
            bid_active = ask_active = false;
        } else if (need) {
            place_quotes(q.bid, q.ask, q.qty, ev);
        }

        // Update the causal order-flow-imbalance EWMA from this trade (used by
        // the overlay on the NEXT event, so it never peeks at the future).
        if (ev.trade_qty > 0) {
            const double signed_q =
                static_cast<double>(ev.trade_side) * static_cast<double>(ev.trade_qty);
            ofi = params_.ofi_lambda * ofi + (1.0 - params_.ofi_lambda) * signed_q;
        }

        // Apply trade event to the queue model.
        if (ev.trade_qty > 0) {
            if (ev.trade_side > 0 && ask_active &&
                ev.trade_price >= outstanding_ask) {
                // Buy aggressor crossing into ask side. Hits us if at our level.
                uint32_t eaten = ask_q_.consume_aggressive(ev.trade_qty);
                if (eaten > 0) {
                    Fill f{ev.ts_ns, /*side*/ -1, outstanding_ask, eaten,
                           params_.maker_fee_ticks};
                    pnl_.on_fill(f);
                    fills_log.push_back({ev.ts_ns, outstanding_ask, -1, eaten, mid});
                    res.fills_ask += static_cast<int>(eaten);
                    quote_age_accum_ms +=
                        static_cast<double>(ev.ts_ns - ask_placed_ns) / 1e6;
                    ++quote_age_samples;
                    if (!ask_q_.active()) ask_active = false;
                }
            } else if (ev.trade_side < 0 && bid_active &&
                       ev.trade_price <= outstanding_bid) {
                uint32_t eaten = bid_q_.consume_aggressive(ev.trade_qty);
                if (eaten > 0) {
                    Fill f{ev.ts_ns, /*side*/ +1, outstanding_bid, eaten,
                           params_.maker_fee_ticks};
                    pnl_.on_fill(f);
                    fills_log.push_back({ev.ts_ns, outstanding_bid, +1, eaten, mid});
                    res.fills_bid += static_cast<int>(eaten);
                    quote_age_accum_ms +=
                        static_cast<double>(ev.ts_ns - bid_placed_ns) / 1e6;
                    ++quote_age_samples;
                    if (!bid_q_.active()) bid_active = false;
                }
            }
        }

        // Inventory L1 integration.
        const std::uint64_t dt_ns = ev.ts_ns - last_event_ns;
        inventory_int += static_cast<double>(std::abs(pnl_.inventory())) *
                         (static_cast<double>(dt_ns) / 1e9);
        last_event_ns = ev.ts_ns;

        // PnL path for sharpe / drawdown.
        pnl_path.push_back(pnl_.total_pnl(mid));
    }

    // Aggregate metrics.
    res.fills    = static_cast<int>(fills_log.size());
    res.pnl_total = pnl_path.empty() ? 0.0 : pnl_path.back();
    res.fill_rate_bid = bid_quotes_seen
                          ? static_cast<double>(res.fills_bid) / bid_quotes_seen
                          : 0.0;
    res.fill_rate_ask = ask_quotes_seen
                          ? static_cast<double>(res.fills_ask) / ask_quotes_seen
                          : 0.0;
    res.inventory_l1_norm = inventory_int;
    res.avg_quote_age_ms = quote_age_samples
                             ? quote_age_accum_ms / quote_age_samples
                             : 0.0;

    // Sharpe / Sortino on PnL increments.
    if (pnl_path.size() > 2) {
        std::vector<double> rets;
        rets.reserve(pnl_path.size());
        for (std::size_t i = 1; i < pnl_path.size(); ++i) {
            rets.push_back(pnl_path[i] - pnl_path[i - 1]);
        }
        double mean = 0.0;
        for (double r : rets) mean += r;
        mean /= static_cast<double>(rets.size());
        double var = 0.0, neg_var = 0.0;
        std::size_t neg_n = 0;
        for (double r : rets) {
            var += (r - mean) * (r - mean);
            if (r < 0) { neg_var += r * r; ++neg_n; }
        }
        var /= static_cast<double>(rets.size());
        double sd = std::sqrt(var);
        res.sharpe  = sd > 1e-12 ? mean / sd : 0.0;
        if (neg_n > 0) {
            double dsd = std::sqrt(neg_var / static_cast<double>(neg_n));
            res.sortino = dsd > 1e-12 ? mean / dsd : 0.0;
        }
        // Max drawdown.
        double peak = pnl_path.front(), max_dd = 0.0;
        for (double v : pnl_path) {
            if (v > peak) peak = v;
            double dd = peak - v;
            if (dd > max_dd) max_dd = dd;
        }
        res.max_drawdown = max_dd;
    }

    // Toxic-flow share: fills where mid moved adversely by > threshold
    // within the toxic window after the fill.
    //
    // fills_log is produced during the forward replay above, so it is in
    // ascending ts_ns order, and events_ is likewise time-ordered. The start
    // of each fill's window (first event strictly after the fill) is therefore
    // monotonic, so we walk both with a single forward cursor instead of
    // re-scanning events_ from the start for every fill — O(N + F) instead of
    // O(F*N).
    if (!fills_log.empty()) {
        int toxic = 0;
        std::size_t win_start = 0;  // first event with ts_ns > current fill ts
        for (const auto& f : fills_log) {
            const std::uint64_t cutoff = f.ts_ns + params_.toxic_window_ns;
            // Advance the window-start cursor past events at/<= this fill's ts.
            // Monotonic across fills because fills_log is time-ordered.
            while (win_start < events_.size() &&
                   events_[win_start].ts_ns <= f.ts_ns) {
                ++win_start;
            }
            int32_t worst = f.mid_at_fill;
            for (std::size_t i = win_start; i < events_.size(); ++i) {
                const auto& ev = events_[i];
                if (ev.ts_ns > cutoff) break;
                int32_t mid = (ev.bid_ticks + ev.ask_ticks) / 2;
                if (f.side > 0 && mid < worst) worst = mid;  // bought, mid dropped
                if (f.side < 0 && mid > worst) worst = mid;  // sold, mid rose
            }
            int32_t move = (f.side > 0)
                             ? (f.mid_at_fill - worst)
                             : (worst - f.mid_at_fill);
            if (move > params_.toxic_threshold_ticks) ++toxic;
        }
        res.toxic_fill_share = static_cast<double>(toxic) / static_cast<double>(fills_log.size());
    }

    return res;
}

void LobSimulator::flatten_inventory(int32_t mid_ticks) {
    const int32_t inv = pnl_.inventory();
    if (inv != 0) {
        // Cross to flat at mid: sell if long, buy if short. Fee 0 — flattening
        // at mid only converts unrealised P&L to realised, it does not change
        // total P&L; it frees the inventory cap for the next session.
        Fill f{};
        f.ts_ns       = 0;
        f.side        = inv > 0 ? -1 : +1;
        f.price_ticks = mid_ticks;
        f.qty         = static_cast<uint32_t>(inv > 0 ? inv : -inv);
        f.fee_ticks   = 0;
        pnl_.on_fill(f);
    }
    bid_q_.cancel();
    ask_q_.cancel();
}

int32_t LobSimulator::inventory() const { return pnl_.inventory(); }

double LobSimulator::cumulative_pnl(int32_t mid_ticks) const {
    return pnl_.total_pnl(mid_ticks);
}

}  // namespace mmx
