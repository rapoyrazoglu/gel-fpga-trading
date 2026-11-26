// MMX — host/threads/risk_supervisor.cpp

#include "risk_supervisor.h"

#include <cstring>

#include "host/ipc/hmac.h"

namespace mmx {

bool RiskSupervisor::tick(std::uint64_t now_ns,
                          int32_t mid_ticks,
                          std::uint64_t last_param_refresh_ns,
                          ParamBlock* kill_block_out) {
    pnl_.snapshot(now_ns, mid_ticks);

    constexpr std::uint64_t kFiveMin = 5ULL * 60ULL * 1'000'000'000ULL;
    const double daily_pnl    = pnl_.total_pnl(mid_ticks);
    const double rolling_5min = pnl_.rolling_pnl(now_ns, kFiveMin, mid_ticks);

    const bool now_tripped = breaker_.evaluate(now_ns, daily_pnl, rolling_5min,
                                               pnl_.inventory(),
                                               last_param_refresh_ns);
    if (!now_tripped) {
        // Re-arm emission for the next independent trip episode (e.g. after an
        // operator breaker.reset()); dedup-while-tripped is preserved because
        // this branch is not entered while the breaker stays latched.
        kill_emitted_ = false;
        return false;
    }
    if (kill_emitted_ || kill_block_out == nullptr) return true;

    ParamBlock p{};
    p.kill_switch = 1;
    p.epoch_ns    = now_ns;
    sign_param_block(p, hmac_key_.data(), hmac_key_.size());
    *kill_block_out = p;
    kill_emitted_ = true;
    return true;
}

}  // namespace mmx
