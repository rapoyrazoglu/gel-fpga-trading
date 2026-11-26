// MMX — host/risk/circuit_breaker.cpp

#include "circuit_breaker.h"

#include <cstdlib>

namespace mmx {

// Saturating elapsed-time helper: timestamps may arrive out of order or a
// clock may step backwards; an unguarded (now - start) on unsigned values
// would underflow to a huge number and trip the breaker spuriously.
static inline std::uint64_t elapsed_ns(std::uint64_t now, std::uint64_t start) {
    return now > start ? now - start : 0;
}

bool CircuitBreaker::evaluate(std::uint64_t now_ns,
                              double daily_pnl,
                              double rolling_5min_pnl,
                              int32_t inventory_lots,
                              std::uint64_t last_param_refresh_ns) {
    if (tripped_.load(std::memory_order_acquire)) return true;

    if (daily_pnl < -limits_.daily_loss_limit) {
        last_reason_ = "daily_loss_limit";
        tripped_.store(true, std::memory_order_release);
        return true;
    }
    if (rolling_5min_pnl < -limits_.intraday_loss_limit) {
        last_reason_ = "intraday_loss_limit";
        tripped_.store(true, std::memory_order_release);
        return true;
    }

    if (std::abs(inventory_lots) > limits_.max_inventory_lots) {
        if (inv_breach_started_ns_ == kNoBreach) {
            inv_breach_started_ns_ = now_ns;
        } else if (elapsed_ns(now_ns, inv_breach_started_ns_) >
                   limits_.inventory_breach_ns) {
            last_reason_ = "inventory_breach";
            tripped_.store(true, std::memory_order_release);
            return true;
        }
    } else {
        inv_breach_started_ns_ = kNoBreach;
    }

    if (last_param_refresh_ns != 0 &&
        elapsed_ns(now_ns, last_param_refresh_ns) > limits_.param_stale_ns) {
        last_reason_ = "param_stale";
        tripped_.store(true, std::memory_order_release);
        return true;
    }

    return false;
}

}  // namespace mmx
