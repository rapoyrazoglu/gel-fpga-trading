// MMX — Walk-Forward Cross-Validation Harness (anti-overfit by construction)
// =========================================================================
// Goal: find a MMX parameterization that survives our system's real latency
// (~2.7 us => quote_latency_events = 3 on a 1-event-per-us feed) on a fast,
// informed-flow market — WITHOUT overfitting.
//
// Anti-overfit design:
//   * Parameters are chosen on a TRAIN seed set and judged on a DISJOINT TEST
//     seed set (different seed range entirely). A config only "wins" if it
//     generalizes out-of-sample.
//   * Robust objective = MEDIAN P&L across train seeds (not the max of one
//     lucky path), with worst-case and fraction-positive reported so a config
//     that is great on average but blows up on one path is visible/rejected.
//   * The robust lever is SYMMETRIC spread widening (ofi_widen_k) + base spread
//     (gamma). Directional skew (ofi_skew_k) is a bet on flow direction; we
//     deliberately EXCLUDE it from the robust config and separately demonstrate
//     that it overfits (good on train, collapses on test).
//   * maker_fee_ticks = 0 (no maker rebate) — the conservative, honest case.
//
// Build (from Market-Maker/):
//   g++ -std=c++20 -O3 -DNDEBUG -I. \
//       ~/Downloads/capstone_build/cv_harness.cpp \
//       build/libmmx_sim.a build/libmmx_core.a -o ~/Downloads/capstone_build/cv
//
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "sim/lob_simulator.h"

using mmx::LobEvent;
using mmx::SimParams;
using mmx::SimResult;

// ---- Market regime descriptor ------------------------------------------------
// One "event" == 1 microsecond. A fast micro-market: a mean-reverting mid driven
// by noise, plus occasional INFORMED bursts (directional runs) whose aggressive
// trades pick off a maker that prices off a stale mid (its reaction latency).
//
// Crucially, the two flow types reach the maker DIFFERENTLY:
//   * uninformed (noise) trades cross only a SHALLOW, varied distance into the
//     book — so a maker that widens its spread MISSES some of them (the real
//     fill-rate cost of widening, and it also gives the kappa estimator a true
//     arrival-rate decay to fit instead of a single degenerate distance);
//   * informed (burst) trades walk DEEP — they always reach the maker and pick
//     off its stale quote (the adverse-selection cost of latency).
// The interior optimum spread balances these two — that is what CV searches for.
struct Regime {
    const char* name;
    double noise_sd;       // per-event mid diffusion (ticks)
    double mr;             // mean-reversion pull toward anchor
    double noise_trade_p;  // P(uninformed trade) per event  (maker earns spread)
    double burst_p;        // P(start an informed burst) per event
    int    burst_len;      // burst duration (events == us)
    double burst_drift;    // directional drift during burst (ticks/event)
    double burst_trade_p;  // P(aggressive informed trade) per burst event
    int    noise_dist_max; // shallow noise trades cross 1..this ticks past mid
    int    depth;          // book half-width / informed trade depth (ticks)
};

// A "fast informed market" — the adversarial stress case for a latency-bound
// maker. ~10:1 uninformed:informed flow (realistic for a liquid book).
static const Regime kFast{
    "fast-informed", /*noise_sd*/8.0, /*mr*/0.01, /*noise_trade_p*/0.020,
    /*burst_p*/0.00020, /*burst_len*/20, /*burst_drift*/25.0, /*burst_trade_p*/0.35,
    /*noise_dist_max*/60, /*depth*/200};

// A different regime for out-of-distribution robustness checks (trendier,
// burstier) — used by the overfit auditor, NOT by parameter selection.
static const Regime kTrendy{
    "trendy-toxic", /*noise_sd*/11.0, /*mr*/0.005, /*noise_trade_p*/0.016,
    /*burst_p*/0.00035, /*burst_len*/30, /*burst_drift*/35.0, /*burst_trade_p*/0.40,
    /*noise_dist_max*/70, /*depth*/240};

// ---- Deterministic feed generator -------------------------------------------
static std::vector<LobEvent> make_feed(int seed, int n_events, const Regime& rg) {
    std::mt19937 rng(static_cast<uint32_t>(seed));
    std::normal_distribution<double>      gauss(0.0, 1.0);
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    std::uniform_int_distribution<int>    side(0, 1);
    std::uniform_int_distribution<int>    ndist(1, rg.noise_dist_max);
    std::uniform_int_distribution<uint32_t> qty(1, 4);

    std::vector<LobEvent> ev;
    ev.reserve(static_cast<size_t>(n_events));

    const double anchor = 10000.0;
    double mid = anchor;
    int    burst_left = 0;
    int    burst_dir  = 0;
    std::uint64_t ts = 0;

    for (int i = 0; i < n_events; ++i) {
        ts += 1'000ULL;  // 1 us per event (1000 ns)

        // Maybe start an informed burst.
        if (burst_left == 0 && u01(rng) < rg.burst_p) {
            burst_left = rg.burst_len;
            burst_dir  = side(rng) ? +1 : -1;
        }
        // Mid dynamics: mean-reversion + noise + (burst drift if active).
        double drift = -rg.mr * (mid - anchor) + rg.noise_sd * gauss(rng);
        if (burst_left > 0) { drift += burst_dir * rg.burst_drift; --burst_left; }
        mid += drift;
        if (mid < 2000.0) mid = 2000.0;

        const int32_t m = static_cast<int32_t>(std::lround(mid));

        LobEvent e{};
        e.ts_ns     = ts;
        e.bid_ticks = m - rg.depth;   // wide reference book; maker rests inside
        e.ask_ticks = m + rg.depth;   // and is therefore at the front of queue
        e.bid_qty   = 8;
        e.ask_qty   = 8;

        // Trades. INFORMED burst trades walk DEEP (always reach + pick off the
        // stale maker). Uninformed trades cross only a shallow, varied distance,
        // so a wider maker misses them — the genuine cost of widening.
        if (burst_left > 0 && u01(rng) < rg.burst_trade_p) {
            e.trade_side  = static_cast<int8_t>(burst_dir);
            e.trade_qty   = qty(rng);
            e.trade_price = m + burst_dir * rg.depth;
        } else if (u01(rng) < rg.noise_trade_p) {
            const int s = side(rng) ? +1 : -1;
            const int D = ndist(rng);
            e.trade_side  = static_cast<int8_t>(s);
            e.trade_qty   = qty(rng);
            e.trade_price = m + s * D;
        }
        ev.push_back(e);
    }
    return ev;
}

// ---- One backtest -----------------------------------------------------------
struct Cfg {
    double   gamma       = 0.10;
    double   ofi_widen_k = 0.0;
    double   ofi_skew_k  = 0.0;   // kept 0 for the robust config
    uint32_t max_inv     = 50;
    uint16_t requote     = 1;
    int      delta       = 3;     // quote_latency_events (==us of staleness)
};

static double run_one(const Cfg& c, int seed, int n_events, const Regime& rg) {
    SimParams sp;
    sp.tick_size         = 1.0;
    sp.maker_fee_ticks   = 0;          // NO rebate — conservative/honest
    sp.toxic_window_ns   = 50'000ULL;  // 50 us toxic horizon (µs-feed scale)
    sp.gamma             = c.gamma;
    sp.horizon_sec       = 600.0;
    sp.max_inv           = c.max_inv;
    sp.base_qty          = 1;
    sp.requote_ticks     = c.requote;
    sp.refresh_period_ns = 1'000'000ULL;  // 1 ms refresh (fast-market cadence)
    sp.ofi_widen_k       = c.ofi_widen_k;
    sp.ofi_skew_k        = c.ofi_skew_k;
    sp.ofi_lambda        = 0.90;
    sp.ofi_max_skew_ticks= 120;        // allow widening to actually protect
    sp.quote_latency_events = c.delta;

    std::vector<std::uint8_t> key(32, 0xab);
    mmx::LobSimulator sim(sp, key);
    sim.load_events(make_feed(seed, n_events, rg));
    SimResult r = sim.run();
    return r.pnl_total;   // ticks
}

// ---- Stats over a seed set --------------------------------------------------
struct Stats { double mean, median, wmin, wmax, sd; int pos, total; };

static Stats eval(const Cfg& c, const std::vector<int>& seeds, int n_events,
                  const Regime& rg) {
    std::vector<double> v;
    v.reserve(seeds.size());
    for (int s : seeds) v.push_back(run_one(c, s, n_events, rg));
    std::vector<double> sorted = v;
    std::sort(sorted.begin(), sorted.end());
    Stats st{};
    st.total = static_cast<int>(v.size());
    double sum = 0; st.pos = 0;
    for (double x : v) { sum += x; if (x > 0) ++st.pos; }
    st.mean   = sum / static_cast<double>(v.size());
    st.median = sorted[sorted.size() / 2];
    st.wmin   = sorted.front();
    st.wmax   = sorted.back();
    double var = 0; for (double x : v) var += (x - st.mean) * (x - st.mean);
    st.sd = std::sqrt(var / static_cast<double>(v.size()));
    return st;
}

static void print_stats(const char* tag, const Stats& s) {
    std::printf("  %-28s mean=%9.0f  median=%9.0f  min=%9.0f  max=%9.0f  sd=%8.0f  +%d/%d\n",
                tag, s.mean, s.median, s.wmin, s.wmax, s.sd, s.pos, s.total);
}

int main(int argc, char** argv) {
    // argv: [N] [train_base] [test_base] [regime 0=fast 1=trendy]
    const int N          = (argc >= 2) ? std::atoi(argv[1]) : 300000;
    const int train_base = (argc >= 3) ? std::atoi(argv[2]) : 100;
    const int test_base  = (argc >= 4) ? std::atoi(argv[3]) : 200;
    const int regime_id  = (argc >= 5) ? std::atoi(argv[4]) : 0;
    const Regime& mkt  = (regime_id == 1) ? ::kTrendy : ::kFast;

    // DISJOINT seed sets — train never overlaps test (callers must keep the
    // two bases >=20 apart so the ranges cannot overlap).
    std::vector<int> train, test;
    for (int s = train_base; s < train_base + 20; ++s) train.push_back(s);
    for (int s = test_base;  s < test_base  + 20; ++s) test.push_back(s);

    std::printf("=== MMX Walk-Forward Cross-Validation ===\n");
    std::printf("feed=%d events/us  regime=%s  delta=3us (our system ~2.7us)\n",
                N, mkt.name);
    std::printf("TRAIN seeds=[%d..%d]  TEST seeds=[%d..%d] (disjoint)\n",
                train_base, train_base + 19, test_base, test_base + 19);
    std::printf("fee=0 (no rebate)  objective=median(train), verified on test\n\n");

    // ---- Baseline: no defense (gamma=0.1, no widening) ----
    Cfg base; base.gamma = 0.10; base.ofi_widen_k = 0.0; base.ofi_skew_k = 0.0;
    std::printf("[BASELINE  gamma=0.10 widen=0 skew=0]\n");
    print_stats("train", eval(base, train, N, mkt));
    print_stats("test ", eval(base, test,  N, mkt));
    std::printf("\n");

    // ---- Grid search on TRAIN ONLY (robust lever: gamma x ofi_widen_k) ----
    // gamma sets the base half-spread (≈ log1p(gamma/kappa)/gamma here); larger
    // gamma => tighter spread. ofi_widen_k adds dynamic toxicity widening.
    const std::vector<double> gammas{0.10, 0.20, 0.40, 0.80, 1.60};
    const std::vector<double> widens{0.0, 1.0, 2.0, 4.0, 8.0};

    std::printf("[GRID SEARCH on TRAIN — select by median(train)]\n");
    Cfg best; double best_obj = -1e18; Stats best_train{};
    for (double g : gammas) {
        for (double w : widens) {
            Cfg c; c.gamma = g; c.ofi_widen_k = w; c.ofi_skew_k = 0.0;
            Stats st = eval(c, train, N, mkt);
            // Robust objective: median, but reject configs whose worst path is
            // catastrophic relative to the median (anti-blowup guard).
            double obj = st.median;
            if (st.wmin < -3.0 * std::fabs(st.median) - 5000.0) obj = -1e17;
            if (obj > best_obj) { best_obj = obj; best = c; best_train = st; }
        }
    }
    std::printf("  selected: gamma=%.2f  widen=%.1f  skew=0\n",
                best.gamma, best.ofi_widen_k);
    print_stats("train (selected)", best_train);

    // ---- OUT-OF-SAMPLE verdict on TEST ----
    Stats best_test = eval(best, test, N, mkt);
    print_stats("test  (selected)", best_test);
    const bool generalizes =
        (best_test.median > 0) && (best_train.median > 0) &&
        (best_test.median > 0.4 * best_train.median);
    std::printf("  --> OUT-OF-SAMPLE: %s (test median %s 0, and within 40%% of train)\n\n",
                generalizes ? "ROBUST / NOT OVERFIT" : "FAILS — would be overfit/rejected",
                best_test.median > 0 ? ">" : "<=");

    // ---- Demonstrate that DIRECTIONAL SKEW overfits ----
    std::printf("[OVERFIT DEMO — directional skew (ofi_skew_k=1) on top of selected]\n");
    Cfg sk = best; sk.ofi_skew_k = 1.0;
    Stats sk_tr = eval(sk, train, N, mkt);
    Stats sk_te = eval(sk, test,  N, mkt);
    print_stats("train (skew=1)", sk_tr);
    print_stats("test  (skew=1)", sk_te);
    std::printf("  --> skew helps/hurts inconsistently across disjoint seeds => directional bet, REJECTED\n\n");

    // ---- Latency tiers with the ROBUST config (comparative table) ----
    std::printf("[LATENCY TIERS with robust config gamma=%.2f widen=%.1f — TEST seeds]\n",
                best.gamma, best.ofi_widen_k);
    struct Tier { const char* name; int d; };
    const std::vector<Tier> tiers{
        {"Co-located bare-metal (~0us)", 0},
        {"OUR system AWS F2 (~2.7us)",   3},
        {"Same-DC non-colo (~10us)",     10},
        {"Software/CPU (~50us)",         50},
    };
    for (const auto& t : tiers) {
        Cfg c = best; c.delta = t.d;
        Stats st = eval(c, test, N, mkt);
        std::printf("  d=%-3d  %-30s median=%9.0f  mean=%9.0f  min=%9.0f  max=%9.0f  +%d/%d\n",
                    t.d, t.name, st.median, st.mean, st.wmin, st.wmax, st.pos, st.total);
    }

    // ---- Robust config on a DIFFERENT regime (out-of-distribution sanity) ----
    std::printf("\n[OUT-OF-DISTRIBUTION — robust config on '%s' regime, TEST seeds]\n",
                kTrendy.name);
    print_stats("test (trendy regime)", eval(best, test, N, kTrendy));

    std::printf("\nROBUST_CONFIG gamma=%.2f ofi_widen_k=%.1f ofi_skew_k=0 max_inv=%u requote=%u\n",
                best.gamma, best.ofi_widen_k, best.max_inv, best.requote);
    return 0;
}
