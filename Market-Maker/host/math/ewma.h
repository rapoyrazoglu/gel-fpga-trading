// MMX — host/math/ewma.h
// EWMA log-return variance (spec §3.3, RiskMetrics λ=0.94).

#pragma once

#include <cmath>

namespace mmx {

class EwmaVariance {
  public:
    explicit EwmaVariance(double lambda = 0.94)
        : lambda_(lambda < 0.0 ? 0.0 : (lambda > 0.999999 ? 0.999999 : lambda)),
          sigma2_(0.0), prev_log_price_(0.0),
          have_prev_(false), seeded_(false) {}

    // Feed one mid-price sample. Variance is per-sample; spec §3.3 says to
    // divide by the sample interval in seconds to get σ²_per_sec.
    void update(double mid_price) {
        if (!(mid_price > 0.0)) return;  // also rejects NaN / non-positive
        double lp = std::log(mid_price);
        if (have_prev_) {
            double r = lp - prev_log_price_;
            if (!seeded_) {
                // Cold-start: seed with the first squared return so the
                // estimate is not biased toward zero for the first ~1/(1-λ)
                // samples after construction or reset.
                sigma2_ = r * r;
                seeded_ = true;
            } else {
                sigma2_ = lambda_ * sigma2_ + (1.0 - lambda_) * r * r;
            }
        }
        prev_log_price_ = lp;
        have_prev_ = true;
    }

    double sigma2_per_sample() const { return sigma2_; }
    double sigma2_per_sec(double sample_dt_sec) const {
        if (sample_dt_sec <= 0.0) return sigma2_;
        return sigma2_ / sample_dt_sec;
    }

    void reset() {
        sigma2_ = 0.0;
        prev_log_price_ = 0.0;
        have_prev_ = false;
        seeded_ = false;
    }

  private:
    double lambda_;
    double sigma2_;
    double prev_log_price_;
    bool   have_prev_;
    bool   seeded_;
};

}  // namespace mmx
