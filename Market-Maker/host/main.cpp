// MMX — host/main.cpp
// Phase A orchestrator: in the absence of FPGA hardware, this binary wires
// the slow-path threads (param compiler, PCTA ingest, risk supervisor) to the
// software FPGA stub and the LOB simulator's event stream. Useful as a
// dry-run / integration smoke test.
//
// Phase B replaces FpgaStub with a real PCIe XDMA path.

#include <cstdio>
#include <cstdint>
#include <random>
#include <vector>

#include "host/ipc/fpga_stub.h"
#include "host/ipc/hmac.h"
#include "host/ipc/param_block.h"
#include "host/math/ewma.h"
#include "host/math/kappa_fit.h"
#include "host/threads/param_compiler.h"
#include "host/threads/pcta_ingest.h"
#include "host/threads/risk_supervisor.h"
#include "sim/lob_simulator.h"

int main(int argc, char** argv) {
    int n_ticks = (argc >= 2) ? std::atoi(argv[1]) : 50000;

    std::vector<std::uint8_t> key(32, 0xab);
    mmx::FpgaStub        fpga(key.data(), key.size());
    mmx::ParamCompiler   compiler(key);
    mmx::RegimeMailbox   mailbox;
    mmx::EwmaVariance    ewma(0.94);
    mmx::KappaEstimator  kappa;
    mmx::BreakerLimits   limits = mmx::default_limits();
    mmx::RiskSupervisor  sup(limits, key);

    // Drive a synthetic random-walk feed through everything.
    std::mt19937 rng(7);
    std::normal_distribution<double> step(0.0, 0.5);
    std::uniform_int_distribution<int> trade_dist(0, 9);
    std::uniform_int_distribution<int> side_dist(0, 1);
    double mid = 10000.0;
    std::uint64_t ts = 0;
    std::uint64_t last_refresh_ns = 0;

    int placed = 0, killed = 0;
    for (int i = 0; i < n_ticks; ++i) {
        mid += step(rng);
        if (mid < 1.0) mid = 1.0;
        ts += 100'000ULL;  // 100 µs ticks
        const int32_t mid_t = static_cast<int32_t>(mid);

        ewma.update(mid);
        if (trade_dist(rng) == 0) {
            kappa.record_arrival(1, ts);
        }

        if (ts - last_refresh_ns >= 100'000'000ULL || last_refresh_ns == 0) {
            mmx::ParamCompilerInputs in{};
            in.sigma2_per_sec = ewma.sigma2_per_sec(0.1);
            in.kappa          = std::max(1e-3, kappa.refit(ts));
            in.horizon_sec    = 600.0;
            in.base_gamma     = 0.1;
            in.regime_id      = mailbox.get();
            in.breaker_tripped= sup.tripped();
            in.max_inv        = 100;
            in.base_qty       = 1;
            in.requote_ticks  = 1;
            in.tick_size      = 1.0;
            in.now_ns         = ts;

            mmx::ParamBlock p{};
            if (compiler.compile(in, &p)) {
                if (fpga.load(p, ts)) ++placed;
            }
            last_refresh_ns = ts;
        }

        auto qo = fpga.tick(mid_t, /*q*/ 0, ts);
        if (qo.cancel_only) ++killed;

        // No fills emitted by stub — sup.tick still runs the watchdog.
        mmx::ParamBlock kill_block{};
        sup.tick(ts, mid_t, last_refresh_ns, &kill_block);
    }

    std::printf("MMX dry run — %d ticks, %d ParamBlocks loaded, %d cancel-only ticks\n",
                n_ticks, placed, killed);
    return 0;
}
