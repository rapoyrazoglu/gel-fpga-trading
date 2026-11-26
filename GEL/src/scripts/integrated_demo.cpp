// src/scripts/integrated_demo.cpp
//
// End-to-end demonstration of the GEL "lego" architecture (v2 multi-account).
//
//   1. GelEngine boots with no router; brokers attach via accounts.
//   2. Two simulated brokers/accounts: OANDA_SIM and IB_SIM.
//   3. PCTAStrategy + MarketMakerStrategy registered side-by-side.
//   4. AccountRouter policy:
//        PCTA, EURUSD     → OANDA_SIM (default)
//        MM,   EURUSD     → IB_SIM    (default)
//   5. Dynamic CapitalAllocator turned on — per (strategy, account)
//      caps reshuffle based on rolling Sharpe.
//   6. Synthetic tick stream + one FOMC event, then a per-cell scoreboard.
//
#include "gel/platform/gel_engine.hpp"
#include "gel/platform/broker_client.hpp"
#include "gel/strategies/pcta.hpp"
#include "gel/strategies/market_maker.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>

using namespace connector;
using namespace gel::platform;
using namespace gel::strategies;

// --------------------------------------------------------------------
// Stub PCTA policy: on every HIGH-impact event, open a long.
// Deniz's PPO+LLM lives elsewhere; this is a placeholder for the demo.
// --------------------------------------------------------------------
struct StubPctaPolicy : public IPctaPolicy {
    SymbolId target_symbol;
    explicit StubPctaPolicy(SymbolId s) : target_symbol(s) {}

    void on_event(const CalendarEvent& e, OrderSink& sink) override {
        if (e.impact != static_cast<uint8_t>(EventImpact::HIGH)) return;
        Order o{};
        o.ts_ns           = now_ns();
        o.client_order_id = ++coid_;
        o.strategy_id     = static_cast<uint32_t>(StrategyId::PCTA);
        o.symbol_id       = target_symbol;
        o.price_ticks     = -1;                                  // MARKET
        o.quantity        = 1000;
        o.side            = static_cast<uint8_t>(Side::BUY);
        o.ord_type        = static_cast<uint8_t>(OrdType::MARKET);
        o.tif             = static_cast<uint8_t>(TIF::DAY);
        sink.send(o);
    }
    uint64_t coid_{1000};
};

int main() {
    // --- 1. Engine setup ------------------------------------------------
    // Heap-allocated: telemetry + ring buffers are ~megabytes, stack would overflow.
    auto engine_ptr = std::make_unique<GelEngine>();
    auto& engine = *engine_ptr;

    const auto eurusd = engine.symbols().register_symbol(
        "EURUSD", Venue::OANDA, /*tick=*/100, /*lot=*/1);

    // --- 2. Accounts (two simulated brokers) ---------------------------
    auto oanda_broker = std::make_shared<SimBrokerClient>("OANDA_SIM");
    auto ib_broker    = std::make_shared<SimBrokerClient>("IB_SIM");
    const auto oanda  = engine.register_account("OANDA_SIM", oanda_broker, /*balance=*/10'000'00000000LL);
    const auto ib     = engine.register_account("IB_SIM",    ib_broker,    /*balance=*/20'000'00000000LL);

    // --- 3. Default routing: which (strategy, symbol) goes where -------
    engine.account_routing().set_default(StrategyId::PCTA,         oanda);
    engine.account_routing().set_default(StrategyId::MARKET_MAKER, ib);

    // --- 4. Risk limits -------------------------------------------------
    // Strategy defaults — apply unless overridden per-account.
    engine.risk().set_limits(StrategyId::PCTA, RiskLimits{
        .max_abs_position    = 50'000,
        .max_order_qty       = 5'000,
        .fat_finger_band_pct = 0.10,
        .max_orders_per_sec  = 1'000,
    });
    engine.risk().set_limits(StrategyId::MARKET_MAKER, RiskLimits{
        .max_abs_position    = 10'000,
        .max_order_qty       = 1'000,
        .fat_finger_band_pct = 0.10,
        .max_orders_per_sec  = 100'000,
    });
    // Per-(strategy, account) override example: tighter cap on IB until
    // we trust the credentials.
    engine.risk().set_limits(StrategyId::MARKET_MAKER, ib, RiskLimits{
        .max_abs_position    = 5'000,
        .max_order_qty       = 500,
        .fat_finger_band_pct = 0.05,
        .max_orders_per_sec  = 100'000,
    });

    // --- 5. Dynamic capital allocator -----------------------------------
    engine.enable_capital_allocator(CapitalAllocatorConfig{
        .interval_secs    = 1,
        .total_cap_pool   = 100'000,
        .min_cap_per_cell = 1'000,
        .temperature      = 1.0,
        .rolling_window   = 10,
    });

    // --- 6. Strategy registration --------------------------------------
    auto pcta = std::make_shared<PCTAStrategy>(
        PctaConfig{}, std::make_unique<StubPctaPolicy>(eurusd));
    auto mm   = std::make_shared<MarketMakerStrategy>(MmConfig{
        .gamma          = 0.1,
        .kappa          = 1.5,
        .sigma2_init    = 1e-7,
        .ewma_alpha     = 0.05,
        .base_quote_qty = 100,
        .max_abs_inventory = 5'000,
    });
    engine.register_strategy(pcta);
    engine.register_strategy(mm);

    // --- 4. Synthetic tick + event stream ------------------------------
    std::mt19937_64 rng{42};
    std::normal_distribution<double> noise{0.0, 0.00005};

    double mid = 1.0750;
    const int N = 50'000;

    auto t0 = std::chrono::steady_clock::now();

    for (int i = 0; i < N; ++i) {
        mid += noise(rng);
        Tick t{};
        t.ts_ns      = now_ns();
        t.symbol_id  = eurusd;
        t.seq        = static_cast<uint32_t>(i);
        const double half = 0.00005;
        t.bid_ticks  = to_ticks(mid - half);
        t.ask_ticks  = to_ticks(mid + half);

        engine.publish_tick(t);

        // Inject one FOMC event mid-run.
        if (i == N / 2) {
            CalendarEvent e{};
            e.event_ts_ns    = t.ts_ns;
            e.ingest_ts_ns   = t.ts_ns;
            e.symbol_id      = 0;             // market-wide
            e.kind           = static_cast<uint8_t>(EventKind::FOMC);
            e.impact         = static_cast<uint8_t>(EventImpact::HIGH);
            e.consensus_x1e6 = 5'250'000;     // 5.25 %
            e.actual_x1e6    = 5'500'000;     // 5.50 %
            e.previous_x1e6  = 5'250'000;
            engine.publish_event(e);
        }

        engine.run_once();
    }

    auto t1 = std::chrono::steady_clock::now();
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    // --- Report --------------------------------------------------------
    const auto pcta_pnl = engine.drop().pnl(StrategyId::PCTA);
    const auto mm_pnl   = engine.drop().pnl(StrategyId::MARKET_MAKER);

    const auto p50 = engine.telemetry().percentile_ns(
        Hop::IPC_DEQUEUE, Hop::WIRE_OUT, 0.50);
    const auto p99 = engine.telemetry().percentile_ns(
        Hop::IPC_DEQUEUE, Hop::WIRE_OUT, 0.99);

    std::printf("\n=== GEL multi-account demo ===\n");
    std::printf("Ticks:               %d in %lld ms\n", N, (long long)elapsed_ms);
    std::printf("\nPer-strategy aggregates:\n");
    std::printf("  PCTA  filled / rej:  %lld / %lld\n",
                (long long)pcta_pnl.filled_count, (long long)pcta_pnl.rejected_count);
    std::printf("  MM    filled / rej:  %lld / %lld\n",
                (long long)mm_pnl.filled_count, (long long)mm_pnl.rejected_count);

    std::printf("\nPer-cell scoreboard (strategy × account):\n");
    engine.drop().for_each_cell([&](StrategyId s, AccountId a, const CellPnL& c) {
        const auto* acc = engine.accounts().get(a);
        const char* sname = (s == StrategyId::PCTA) ? "PCTA" : "MM  ";
        std::printf("  %s @ %-10s  fills=%-6lld  rej=%-6lld  pnl_ticks=%lld\n",
                    sname, acc ? acc->name : "?",
                    (long long)c.filled_count.load(std::memory_order_relaxed),
                    (long long)c.rejected_count.load(std::memory_order_relaxed),
                    (long long)c.realized_pnl_ticks.load(std::memory_order_relaxed));
    });

    std::printf("\nMM inventory @ EURUSD (across all accounts): %lld\n",
                (long long)mm->inventory(eurusd));
    std::printf("Tick-to-wire p50:    %llu ns\n", (unsigned long long)p50);
    std::printf("Tick-to-wire p99:    %llu ns\n", (unsigned long long)p99);
    std::printf("==============================\n\n");

    engine.shutdown();
    return 0;
}
