// MMX — host/risk/pnl_tracker.cpp

#include "pnl_tracker.h"

#include <algorithm>
#include <cstdlib>

namespace mmx {

void PnlTracker::on_fill(const Fill& f) {
    const int32_t qty = static_cast<int32_t>(f.qty);
    const int32_t signed_qty = (f.side >= 0) ? qty : -qty;
    const double price = static_cast<double>(f.price_ticks);

    // Zero-qty fill: a clean no-op on inventory/avg-cost. We still book any
    // reported fee so behaviour matches the rest of on_fill, but we never let
    // it route through the crossing/realise branch (where correctness only
    // accidentally holds because closing == 0).
    if (signed_qty == 0) {
        realised_pnl_ticks_ -= static_cast<double>(f.fee_ticks);
        return;
    }

    // Standard avg-cost bookkeeping with crossing-zero handling.
    if (inventory_ == 0 || (inventory_ > 0) == (signed_qty > 0)) {
        // Same direction or starting flat: update average cost.
        const double new_inv = inventory_ + signed_qty;
        if (new_inv != 0) {
            avg_cost_ticks_ =
                (avg_cost_ticks_ * inventory_ + price * signed_qty) / new_inv;
        }
        inventory_ = static_cast<int32_t>(new_inv);
    } else {
        // Opposite direction: realise PnL on min(|inv|,|qty|) lots.
        const int32_t closing = std::abs(signed_qty) <= std::abs(inventory_)
                                    ? std::abs(signed_qty)
                                    : std::abs(inventory_);
        const double pnl_per_lot =
            (inventory_ > 0) ? (price - avg_cost_ticks_)
                             : (avg_cost_ticks_ - price);
        realised_pnl_ticks_ += pnl_per_lot * closing;

        const int32_t new_inv = inventory_ + signed_qty;
        if ((inventory_ > 0) == (new_inv > 0) || new_inv == 0) {
            inventory_ = new_inv;
            if (new_inv == 0) avg_cost_ticks_ = 0.0;
        } else {
            // Crossed through zero: residual takes the new side at fill price.
            inventory_      = new_inv;
            avg_cost_ticks_ = price;
        }
    }

    realised_pnl_ticks_ -= static_cast<double>(f.fee_ticks);
}

double PnlTracker::total_pnl(int32_t mid_ticks) const {
    const double mtm = static_cast<double>(inventory_) *
                       (static_cast<double>(mid_ticks) - avg_cost_ticks_);
    return (realised_pnl_ticks_ + mtm) * tick_value_;
}

void PnlTracker::snapshot(std::uint64_t ts_ns, int32_t mid_ticks) {
    snaps_.push_back({ts_ns, total_pnl(mid_ticks)});
    // Keep the deque bounded — we only need ~5 minutes back.
    constexpr std::uint64_t kKeepNs = 10ULL * 60ULL * 1'000'000'000ULL;
    while (!snaps_.empty() && snaps_.front().ts_ns + kKeepNs < ts_ns) {
        snaps_.pop_front();
    }
}

double PnlTracker::rolling_pnl(std::uint64_t ts_ns, std::uint64_t window_ns,
                               int32_t mid_ticks) const {
    if (snaps_.empty()) return total_pnl(mid_ticks);
    // Find the earliest snap inside the window.
    const std::uint64_t cutoff =
        ts_ns > window_ns ? ts_ns - window_ns : 0;
    // snaps_ is sorted ascending by ts_ns (snapshot() only push_back's the
    // current now_ns and pop_front's the oldest), so binary-search the window
    // edge instead of scanning — O(log N) on the per-tick risk hot path.
    const auto it = std::lower_bound(
        snaps_.begin(), snaps_.end(), cutoff,
        [](const Snap& s, std::uint64_t c) { return s.ts_ns < c; });
    const double base = (it != snaps_.end()) ? it->pnl : snaps_.front().pnl;
    return total_pnl(mid_ticks) - base;
}

}  // namespace mmx
