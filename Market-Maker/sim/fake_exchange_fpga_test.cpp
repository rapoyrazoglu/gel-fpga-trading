// MMX — sim/fake_exchange_fpga_test.cpp
//
// End-to-end "fake exchange -> FPGA -> outputs" harness.
//
//   [ FakeExchange ]  publishes a synthetic L2 tick/trade stream (random-walk
//        |            mid + aggressor trades + an injected volatility SHOCK).
//        v
//   [ FPGA fast path ] each tick is gated through the *software model* of the
//        |             FPGA: FpgaStub::tick() -> compute_quote(), the exact same
//        |             source that the Vitis HLS top (mmx_pricing_engine) synthesizes
//        v             to RTL. We call BOTH every tick and assert bit-exact parity.
//   [ outputs ]       per-tick quotes, a maker fill model + PnL, the slow-path
//                     ParamBlock (HMAC-signed) cadence, the risk supervisor /
//                     circuit breaker, host<->HLS parity, and fast-path latency.
//
// This is the project's Phase A: the FPGA logic executed as its bit-exact
// software model. On real F2/VU47P silicon the same compute_quote runs in the
// pricing kernel; here it runs on the CPU so we get real, inspectable outputs
// today. Build: linked against libmmx_sim + libmmx_core.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "host/ipc/fpga_stub.h"
#include "host/ipc/param_block.h"
#include "host/math/ewma.h"
#include "host/math/kappa_fit.h"
#include "host/pricing/pricing_engine.h"
#include "host/threads/param_compiler.h"
#include "host/threads/pcta_ingest.h"
#include "host/threads/risk_supervisor.h"

// The Vitis HLS synthesizable top-level. Same translation unit that v++
// synthesizes to RTL; linked here as plain C++ so we can prove that the FPGA
// kernel and the host reference produce identical bytes for every tick.
extern "C" void mmx_pricing_engine(const mmx::ParamBlock& p_in, int32_t s,
                                   int32_t q, int32_t& bid_out, int32_t& ask_out,
                                   uint32_t& qty_out, uint8_t& cancel_only_out);

namespace {

struct Tick {
    std::uint64_t ts_ns;
    int32_t       bid_t, ask_t;
    int8_t        trade_side;   // +1 buy aggressor, -1 sell aggressor, 0 none
    std::uint32_t trade_qty;
    int32_t       trade_price;
    bool          in_shock;
};

// ---- Component 1: the FAKE EXCHANGE (synthetic feed publisher) -------------
// Streams ticks one at a time, mirroring a live market-data handler. Mid is a
// mean-reverting diffusion; aggressor trades cross a VARIABLE DEPTH into the
// book (so a maker quoting δ ticks off mid actually gets filled when flow
// reaches its level — same fill mechanics as sim/cv_harness.cpp). A regime
// SHOCK in [shock_start, shock_start+shock_len) switches to one-directional
// INFORMED flow: deep crossings + drift that picks off the maker's stale bid
// (adverse selection — exactly what the FPGA fast path exists to fight).
class FakeExchange {
  public:
    FakeExchange(int n, unsigned seed, int shock_start, int shock_len)
        : n_(n), shock_start_(shock_start), shock_len_(shock_len), rng_(seed) {}

    bool next(Tick& t) {
        if (i_ >= n_) return false;
        const bool shock = (i_ >= shock_start_ && i_ < shock_start_ + shock_len_);

        std::normal_distribution<double>       gauss(0.0, 1.0);
        std::uniform_real_distribution<double> u01(0.0, 1.0);

        const double sd    = shock ? shock_sd_    : noise_sd_;
        const double drift = shock ? shock_drift_ : 0.0;
        mid_ += drift + sd * gauss(rng_) - mr_ * (mid_ - anchor_);
        if (mid_ < 1.0) mid_ = 1.0;
        ts_ += 100'000ULL;  // 100 µs between ticks

        t.ts_ns      = ts_;
        t.bid_t      = static_cast<int32_t>(mid_ - 0.5);
        t.ask_t      = t.bid_t + 1;
        t.trade_side = 0;
        t.trade_qty  = 0;
        t.trade_price= 0;
        t.in_shock   = shock;

        const double trade_p = shock ? shock_trade_p_ : noise_trade_p_;
        if (u01(rng_) < trade_p) {
            std::uniform_int_distribution<std::uint32_t> qd(1, 4);
            const int dist_max = shock ? shock_depth_ : noise_dist_max_;
            std::uniform_int_distribution<int> dd(1, dist_max);
            const int d = dd(rng_);
            // normal: balanced two-sided noise; shock: one-directional informed.
            const int s = shock ? shock_dir_ : (u01(rng_) < 0.5 ? +1 : -1);
            t.trade_side  = static_cast<int8_t>(s);
            t.trade_qty   = qd(rng_);
            t.trade_price = s > 0 ? t.ask_t + d : t.bid_t - d;  // crosses d deep
        }
        ++i_;
        return true;
    }

    double mid() const { return mid_; }

  private:
    int           n_, i_ = 0;
    int           shock_start_, shock_len_;
    std::mt19937  rng_;
    double        mid_ = 10000.0, anchor_ = 10000.0;
    std::uint64_t ts_  = 0;
    double        noise_sd_       = 6.0;    // ticks/sample, normal regime
    double        shock_sd_       = 14.0;   // volatility spike during shock
    double        mr_             = 0.01;   // mean reversion toward anchor
    double        noise_trade_p_  = 0.04;   // P(trade) per tick, two-sided
    double        shock_trade_p_  = 0.30;   // informed flow is dense
    double        shock_drift_    = -6.0;   // informed sells -> price runs down
    int           shock_dir_      = -1;     // -1 = sell aggressor hits our bid
    int           noise_dist_max_ = 60;     // noise trades cross 1..60 deep
    int           shock_depth_    = 220;    // informed trades walk deep
};

struct Row {
    std::uint64_t ts_ns;
    int32_t  mid_t;
    uint8_t  regime;
    int32_t  inv;
    int32_t  bid, ask, half_spread;
    uint8_t  cancel_only;
    int8_t   fill;          // +1 we bought, -1 we sold, 0 none
    double   pnl;
    bool     in_shock;
    double   tick_latency_ns;
};

double pct(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    auto idx = static_cast<std::size_t>(p * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + idx, v.end());
    return v[idx];
}

}  // namespace

int main(int argc, char** argv) {
    const int      n_ticks = (argc >= 2) ? std::atoi(argv[1]) : 200000;
    const unsigned seed    = (argc >= 3) ? static_cast<unsigned>(std::atoi(argv[2])) : 7;
    const int      shock_start = static_cast<int>(n_ticks * 0.60);
    const int      shock_len   = static_cast<int>(n_ticks * 0.08);

    // Shared HMAC key (slow-path CPU signs ParamBlocks, FPGA verifies).
    std::vector<std::uint8_t> key(32, 0xab);

    mmx::FpgaStub       fpga(key.data(), key.size());
    mmx::ParamCompiler  compiler(key);
    mmx::RegimeMailbox  mailbox;
    mmx::EwmaVariance   ewma(0.94);
    mmx::KappaEstimator kappa;
    mmx::RiskSupervisor sup(mmx::default_limits(), key);

    FakeExchange exch(n_ticks, seed, shock_start, shock_len);

    // Engine state.
    int32_t inv = 0;                 // signed inventory (lots)
    double  cash = 0.0;              // in tick*lots (buy: -, sell: +)
    std::uint64_t last_refresh_ns = 0;
    mmx::OutstandingQuote resting{0, 0, 0};
    bool resting_valid = false;

    // Counters / metrics.
    long loads = 0, quotes = 0, cancels = 0, replaces = 0;
    long fills_bid = 0, fills_ask = 0;
    long parity_checks = 0, parity_mismatch = 0;
    long breaker_trips = 0;
    int32_t max_abs_inv = 0;
    double sum_hs_normal = 0, sum_hs_shock = 0;
    long   n_hs_normal = 0, n_hs_shock = 0;
    std::vector<double> lat;
    lat.reserve(n_ticks);
    std::vector<Row> rows;
    rows.reserve(n_ticks);

    Tick tk;
    while (exch.next(tk)) {
        const int32_t mid_t = static_cast<int32_t>(exch.mid());

        // --- slow-path estimators (per tick) ---
        ewma.update(exch.mid());
        if (tk.trade_side != 0) kappa.record_arrival(1, tk.ts_ns);

        // PCTA-style regime hint: high-vol during the shock, ranging otherwise.
        mailbox.set(tk.in_shock ? mmx::kRegimeHighVol : mmx::kRegimeRanging);

        // --- slow-path param compile + HMAC-signed load every 100 ms ---
        if (tk.ts_ns - last_refresh_ns >= 100'000'000ULL || last_refresh_ns == 0) {
            mmx::ParamCompilerInputs in{};
            in.sigma2_per_sec  = ewma.sigma2_per_sec(0.1);
            in.kappa           = std::max(1e-3, kappa.refit(tk.ts_ns));
            in.horizon_sec     = 600.0;
            in.base_gamma      = 0.1;
            in.regime_id       = mailbox.get();
            in.breaker_tripped = sup.tripped();
            in.max_inv         = 100;
            in.base_qty        = 1;
            in.requote_ticks   = 1;
            in.tick_size       = 1.0;
            in.now_ns          = tk.ts_ns;

            mmx::ParamBlock p{};
            if (compiler.compile(in, &p) && fpga.load(p, tk.ts_ns)) ++loads;
            last_refresh_ns = tk.ts_ns;
        }

        // --- FPGA FAST PATH: tick enters, quote action comes out ---
        const auto t0 = std::chrono::high_resolution_clock::now();
        mmx::QuoteOut qo = fpga.tick(mid_t, inv, tk.ts_ns);
        const auto t1 = std::chrono::high_resolution_clock::now();
        const double ns =
            std::chrono::duration<double, std::nano>(t1 - t0).count();
        lat.push_back(ns);

        // --- host <-> HLS bit-exact parity (the project's signature claim) ---
        if (fpga.active_valid()) {
            mmx::QuoteOut ref = mmx::compute_quote(mid_t, inv, fpga.active());
            int32_t hb, ha; uint32_t hq; uint8_t hc;
            mmx_pricing_engine(fpga.active(), mid_t, inv, hb, ha, hq, hc);
            ++parity_checks;
            if (hb != ref.bid || ha != ref.ask || hq != ref.qty ||
                hc != ref.cancel_only)
                ++parity_mismatch;
        }

        // --- act on the quote: cancel / place / replace ---
        int8_t fill = 0;
        if (qo.cancel_only) {
            if (resting_valid) ++cancels;
            resting_valid = false;
        } else {
            ++quotes;
            const bool replace = !resting_valid ||
                mmx::needs_replace(qo, resting, tk.ts_ns,
                                   /*requote_ticks*/ 1,
                                   /*max_quote_age_ns*/ 50'000'000ULL,
                                   inv, /*max_inv*/ 100, /*kill*/ 0);
            if (replace) {
                if (resting_valid) ++replaces;
                resting = {qo.bid, qo.ask, tk.ts_ns};
                resting_valid = true;
            }

            // --- maker fill model: did the published trade cross our quote? ---
            if (tk.trade_side < 0 && tk.trade_price <= resting.bid_old) {
                // sell aggressor hit our bid -> we BUY
                const int32_t lots = static_cast<int32_t>(qo.qty);
                inv  += lots;
                cash -= static_cast<double>(resting.bid_old) * lots;
                ++fills_bid; fill = +1;
            } else if (tk.trade_side > 0 && tk.trade_price >= resting.ask_old) {
                // buy aggressor lifted our ask -> we SELL
                const int32_t lots = static_cast<int32_t>(qo.qty);
                inv  -= lots;
                cash += static_cast<double>(resting.ask_old) * lots;
                ++fills_ask; fill = -1;
            }
        }

        max_abs_inv = std::max(max_abs_inv, std::abs(inv));

        // half-spread adaptation: bucket by regime
        if (fpga.active_valid()) {
            const int32_t hs = fpga.active().half_spread;
            if (tk.in_shock) { sum_hs_shock += hs; ++n_hs_shock; }
            else             { sum_hs_normal += hs; ++n_hs_normal; }
        }

        // --- risk supervisor watchdog ---
        mmx::ParamBlock kill_block{};
        const bool was_tripped = sup.tripped();
        sup.tick(tk.ts_ns, mid_t, last_refresh_ns, &kill_block);
        if (!was_tripped && sup.tripped()) ++breaker_trips;

        const double pnl = cash + static_cast<double>(inv) * mid_t;
        rows.push_back(Row{tk.ts_ns, mid_t, mailbox.get(), inv, qo.bid, qo.ask,
                           fpga.active_valid() ? fpga.active().half_spread : 0,
                           qo.cancel_only, fill, pnl, tk.in_shock, ns});
    }

    // ---- report ----------------------------------------------------------
    auto print_row = [](const Row& r) {
        std::printf("  t=%8.1fms  mid=%6d  reg=%u  inv=%+4d  FPGA[bid=%6d ask=%6d "
                    "hs=%2d]  %s  %s  pnl=%+.0f\n",
                    r.ts_ns / 1e6, r.mid_t, r.regime, r.inv, r.bid, r.ask,
                    r.half_spread, r.cancel_only ? "CANCEL" : "quote ",
                    r.fill > 0 ? "BUY " : (r.fill < 0 ? "SELL" : "    "), r.pnl);
    };

    std::printf("================ MMX  fake-exchange -> FPGA  end-to-end ================\n");
    std::printf("feed: %d ticks @100us, seed=%u   vol SHOCK on ticks [%d,%d)\n",
                n_ticks, seed, shock_start, shock_start + shock_len);
    std::printf("FPGA path: FpgaStub::tick -> compute_quote (== Vitis HLS top mmx_pricing_engine)\n\n");

    std::printf("--- first ticks (normal regime) ---\n");
    for (int i = 0; i < 6 && i < (int)rows.size(); ++i) print_row(rows[i]);
    std::printf("--- ticks at volatility SHOCK onset (regime 3 = high-vol) ---\n");
    for (int i = shock_start; i < shock_start + 6 && i < (int)rows.size(); ++i)
        print_row(rows[i]);

    const double avg_hs_normal = n_hs_normal ? sum_hs_normal / n_hs_normal : 0;
    const double avg_hs_shock  = n_hs_shock  ? sum_hs_shock  / n_hs_shock  : 0;
    const double final_pnl = rows.empty() ? 0.0 : rows.back().pnl;

    long cancel_all_ticks = 0;
    for (const auto& r : rows) if (r.cancel_only) ++cancel_all_ticks;

    std::printf("\n--- aggregate ---\n");
    std::printf("  ParamBlocks loaded (HMAC-signed)   = %ld\n", loads);
    std::printf("  quotes placed / cancels / replaces = %ld / %ld / %ld\n",
                quotes, cancels, replaces);
    std::printf("  FPGA cancel-all ticks (inv/staleness guard) = %ld\n",
                cancel_all_ticks);
    std::printf("  fills  bid(buy)=%ld  ask(sell)=%ld  total=%ld\n",
                fills_bid, fills_ask, fills_bid + fills_ask);
    std::printf("  final inventory = %+d lots   max |inventory| = %d (cap 100)\n",
                rows.empty() ? 0 : rows.back().inv, max_abs_inv);
    std::printf("  final marked PnL = %+.0f tick*lots\n", final_pnl);
    std::printf("  circuit-breaker trips = %ld\n", breaker_trips);

    std::printf("\n--- adaptive half-spread (Avellaneda-Stoikov, kappa-driven) ---\n");
    std::printf("  avg half-spread  normal regime = %.2f ticks\n", avg_hs_normal);
    std::printf("  avg half-spread  SHOCK  regime = %.2f ticks   (%.2fx of normal)\n",
                avg_hs_shock, avg_hs_normal > 0 ? avg_hs_shock / avg_hs_normal : 0);
    std::printf("  (denser informed flow raises kappa -> tighter base spread; the\n");
    std::printf("   defensive response is inventory skew + cancel-all on cap breach)\n");

    std::printf("\n--- host <-> FPGA(HLS) bit-exact parity ---\n");
    std::printf("  ticks checked = %ld   mismatches = %ld   -> %s\n",
                parity_checks, parity_mismatch,
                parity_mismatch == 0 ? "BIT-EXACT PARITY OK" : "PARITY FAILURE");

    std::vector<double> lcopy = lat;
    std::printf("\n--- FPGA fast-path latency (software model, CPU) ---\n");
    std::printf("  ticks=%zu  p50=%.0f ns  p99=%.0f ns  max=%.0f ns\n",
                lat.size(), pct(lcopy, 0.50), pct(lcopy, 0.99), pct(lcopy, 1.0));
    std::printf("  (real F2/VU47P target: <=2 us tick-to-trade in hardware)\n");

    // ---- write full per-tick CSV ----
    const char* csv = (argc >= 4) ? argv[3] : "fpga_demo_run.csv";
    if (FILE* f = std::fopen(csv, "w")) {
        std::fprintf(f, "ts_ns,mid_t,regime,inv,fpga_bid,fpga_ask,half_spread,"
                        "cancel_only,fill,pnl,in_shock,tick_latency_ns\n");
        for (const auto& r : rows)
            std::fprintf(f, "%llu,%d,%u,%d,%d,%d,%d,%u,%d,%.1f,%d,%.1f\n",
                         (unsigned long long)r.ts_ns, r.mid_t, r.regime, r.inv,
                         r.bid, r.ask, r.half_spread, r.cancel_only, r.fill,
                         r.pnl, r.in_shock ? 1 : 0, r.tick_latency_ns);
        std::fclose(f);
        std::printf("\nfull per-tick log written: %s  (%zu rows)\n", csv, rows.size());
    }
    return parity_mismatch == 0 ? 0 : 1;
}
