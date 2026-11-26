// MMX — host/risk/circuit_breaker.h
// Risk supervisor's tripping logic (spec §5.5).

#pragma once

#include <atomic>
#include <cstdint>

namespace mmx {

struct BreakerLimits {
    double  daily_loss_limit;       // currency, positive number; trip if pnl < -this
    double  intraday_loss_limit;    // 5-min window
    int32_t max_inventory_lots;     // |q| > this for > inventory_breach_ns trips
    std::uint64_t inventory_breach_ns;  // typical 30s
    std::uint64_t param_stale_ns;       // refresh stalled > this trips
};

inline BreakerLimits default_limits() {
    return BreakerLimits{
        /*daily_loss_limit*/    1000.0,
        /*intraday_loss_limit*/  300.0,
        /*max_inventory_lots*/   100,
        /*inventory_breach_ns*/  30ULL * 1'000'000'000ULL,
        /*param_stale_ns*/        1ULL * 1'000'000'000ULL,
    };
}

class CircuitBreaker {
  public:
    explicit CircuitBreaker(BreakerLimits limits = default_limits())
        : limits_(limits), tripped_(false),
          inv_breach_started_ns_(kNoBreach) {}

    bool tripped() const { return tripped_.load(std::memory_order_acquire); }
    void reset() { tripped_.store(false, std::memory_order_release); inv_breach_started_ns_ = kNoBreach; }

    // Returns true if (now or already) tripped.
    bool evaluate(std::uint64_t now_ns,
                  double daily_pnl,
                  double rolling_5min_pnl,
                  int32_t inventory_lots,
                  std::uint64_t last_param_refresh_ns);

    const BreakerLimits& limits() const { return limits_; }

    // Last-tripped reason — for journaling. Empty if never tripped.
    const char* last_reason() const { return last_reason_; }

  private:
    // Sentinel meaning "not currently breaching". A real breach-start
    // timestamp of 0 (clocks/sims often start at t=0) is a valid, distinct
    // value, so 0 must not double as the cleared state.
    static constexpr std::uint64_t kNoBreach = UINT64_MAX;

    BreakerLimits        limits_;
    std::atomic<bool>    tripped_;
    std::uint64_t        inv_breach_started_ns_;
    const char*          last_reason_ = "";
};

}  // namespace mmx
