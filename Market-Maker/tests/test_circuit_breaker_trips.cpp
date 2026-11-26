// MMX — Acceptance test #9 (spec §10):
// Feed a synthetic loss > limit and confirm the supervisor writes a kill
// ParamBlock within 50 ms.

#include <vector>

#include "host/ipc/hmac.h"
#include "host/ipc/param_block.h"
#include "host/risk/circuit_breaker.h"
#include "host/risk/pnl_tracker.h"
#include "host/threads/risk_supervisor.h"
#include "test_helpers.h"

int main() {
    std::vector<std::uint8_t> key(32, 0xa5);
    mmx::BreakerLimits limits{
        /*daily_loss_limit*/    100.0,
        /*intraday_loss_limit*/  50.0,
        /*max_inventory_lots*/   100,
        /*inventory_breach_ns*/  30ULL * 1'000'000'000ULL,
        /*param_stale_ns*/        1ULL * 1'000'000'000ULL,
    };

    mmx::RiskSupervisor sup(limits, key);

    // We sold 200 lots @ 100; mid then runs to 110 → MTM loss = 200*10 = 2000.
    sup.on_fill({/*ts*/ 0, /*side*/ -1, /*price*/ 100, /*qty*/ 200, /*fee*/ 0});

    mmx::ParamBlock kill{};
    bool ok = false;
    const std::uint64_t t_evaluate_start = 0;
    std::uint64_t t_kill = 0;

    for (std::uint64_t t = 0; t <= 100'000'000ULL; t += 1'000'000ULL) {
        if (sup.tick(t, /*mid*/ 110, /*last_refresh*/ t, &kill)) {
            ok = true;
            t_kill = t;
            break;
        }
    }
    MMX_EXPECT(ok);
    MMX_EXPECT_EQ(kill.kill_switch, 1u);

    // Within 50 ms of the start of evaluation.
    MMX_EXPECT(t_kill - t_evaluate_start <= 50'000'000ULL);

    // Kill block must verify against our key.
    MMX_EXPECT(mmx::verify_param_block(kill, key.data(), key.size()));

    MMX_OK();
}
