// MMX — host/math/kappa_fit.cpp

#include "kappa_fit.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace mmx {

KappaEstimator::KappaEstimator() : last_kappa_(0.0) {}

void KappaEstimator::record_arrival(int distance_ticks, std::uint64_t ts_ns) {
    if (distance_ticks < 0) distance_ticks = 0;
    if (distance_ticks >= kMaxBin) distance_ticks = kMaxBin - 1;
    events_.push_back({ts_ns, distance_ticks});
}

void KappaEstimator::evict_old(std::uint64_t now_ns) {
    const std::uint64_t window_ns =
        static_cast<std::uint64_t>(kWindowSec * 1e9);
    while (!events_.empty() && events_.front().ts_ns + window_ns < now_ns) {
        events_.pop_front();
    }
}

double KappaEstimator::refit(std::uint64_t now_ns) {
    evict_old(now_ns);

    std::array<int, kMaxBin> counts{};
    counts.fill(0);
    for (const auto& e : events_) {
        ++counts[static_cast<std::size_t>(e.bin)];
    }

    // Weighted OLS on (delta, log lambda) for non-empty bins, lambda = count /
    // window. Arrivals are Poisson, so the variance of log(count) scales as
    // 1/count; weighting each bin by its count is the (first-order) correct
    // heteroscedastic fit and stops a single far-out tick (count = 1) from
    // dominating the slope, which an unweighted fit would let happen.
    int    n      = 0;
    double sum_w  = 0.0;
    double sum_wx = 0.0;
    double sum_wy = 0.0;
    double sum_wxy = 0.0;
    double sum_wxx = 0.0;
    for (std::size_t d = 0; d < static_cast<std::size_t>(kMaxBin); ++d) {
        if (counts[d] == 0) continue;
        const double w      = static_cast<double>(counts[d]);  // Poisson weight
        const double lambda = w / kWindowSec;
        const double y      = std::log(lambda);
        const double x      = static_cast<double>(d);
        sum_w   += w;
        sum_wx  += w * x;
        sum_wy  += w * y;
        sum_wxy += w * x * y;
        sum_wxx += w * x * x;
        ++n;
    }

    double new_kappa = last_kappa_;
    if (n >= 2) {
        const double denom = sum_w * sum_wxx - sum_wx * sum_wx;
        if (denom > 1e-12) {
            // slope of log λ vs δ. λ = A·exp(−κ·δ) ⇒ slope = −κ.
            const double slope = (sum_w * sum_wxy - sum_wx * sum_wy) / denom;
            new_kappa = -slope;
            if (new_kappa < 1e-6) new_kappa = 1e-6;  // guard
        }
    }

    recent_kappa_.push_back(new_kappa);
    while (static_cast<int>(recent_kappa_.size()) > kMedianN) {
        recent_kappa_.pop_front();
    }

    std::vector<double> tmp(recent_kappa_.begin(), recent_kappa_.end());
    if (tmp.empty()) return last_kappa_;
    std::sort(tmp.begin(), tmp.end());
    last_kappa_ = tmp[tmp.size() / 2];
    return last_kappa_;
}

}  // namespace mmx
