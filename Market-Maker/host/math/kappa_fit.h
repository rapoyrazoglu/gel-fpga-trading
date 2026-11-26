// MMX — host/math/kappa_fit.h
// Arrival-rate κ estimator (spec §3.4, Cont–Kukanov–Stoikov).
//
// 1. Bin same-side arrivals by distance δ (in ticks) from mid, per side, in a
//    rolling 60-second window.
// 2. λ(δ) = arrivals_in_bin / window_sec.
// 3. Fit λ(δ) = A · exp(−κ · δ) by OLS on log λ.
// 4. 5-update median filter for smoothing.

#pragma once

#include <array>
#include <cstdint>
#include <deque>

namespace mmx {

class KappaEstimator {
  public:
    static constexpr int    kMaxBin       = 16;       // ticks from mid
    static constexpr double kWindowSec    = 60.0;
    static constexpr int    kMedianN      = 5;

    KappaEstimator();

    // Record one arrival on either side, expressed as integer tick distance
    // from the prevailing mid (>= 0).  ts_ns: monotonic event timestamp.
    void record_arrival(int distance_ticks, std::uint64_t ts_ns);

    // Recompute κ and emit a smoothed estimate. Returns the latest median.
    // Caller should invoke this once per refresh cycle (~1 s, spec §3.4).
    double refit(std::uint64_t now_ns);

    // Latest smoothed κ. 0 means "not enough data yet" — caller must guard.
    double kappa() const { return last_kappa_; }

  private:
    struct Event { std::uint64_t ts_ns; int bin; };
    std::deque<Event> events_;
    std::deque<double> recent_kappa_;
    double last_kappa_;

    void evict_old(std::uint64_t now_ns);
};

}  // namespace mmx
