// MMX — host/threads/risk_supervisor.h
// Glue: PnL tracker + circuit breaker + last-refresh watchdog → emit
// kill_switch=1 ParamBlock the moment any limit trips.

#pragma once

#include <cstdint>
#include <vector>

#include "host/ipc/param_block.h"
#include "host/risk/circuit_breaker.h"
#include "host/risk/pnl_tracker.h"

namespace mmx {

class RiskSupervisor {
  public:
    RiskSupervisor(BreakerLimits limits, std::vector<std::uint8_t> hmac_key)
        : breaker_(limits), hmac_key_(std::move(hmac_key)) {}

    // Inject a fill (e.g., from F2C event ring).
    void on_fill(const Fill& f) { pnl_.on_fill(f); }

    // Periodic tick (call from supervisor thread). Returns true and fills
    // *kill_block if a kill ParamBlock must be DMA'd to the FPGA right now.
    bool tick(std::uint64_t now_ns,
              int32_t mid_ticks,
              std::uint64_t last_param_refresh_ns,
              ParamBlock* kill_block_out);

    bool tripped() const { return breaker_.tripped(); }
    const char* last_reason() const { return breaker_.last_reason(); }
    PnlTracker& pnl() { return pnl_; }
    CircuitBreaker& breaker() { return breaker_; }

  private:
    PnlTracker     pnl_;
    CircuitBreaker breaker_;
    std::vector<std::uint8_t> hmac_key_;
    bool kill_emitted_ = false;
};

}  // namespace mmx
