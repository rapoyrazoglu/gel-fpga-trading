// src/scripts/fleet_demo.cpp
//
// GEL strategy-agnostic PLATFORM demo: TEN independent Layer-2 strategies
// (PCTA + Market Making + 8 technical strategies) plugged into ONE shared
// GelEngine, each with its own risk cell, PnL cell, and audit trail.
//
// This is the concrete proof of the platform thesis from the final report:
// many strategies competing for capital on one FPGA-accelerated execution
// stack, exactly as pro shops (Optiver, Flow Traders, Renaissance) operate.
//
//   register_strategy(s)  -> publish_tick/book/event fans out to all 10
//   run_once()            -> drain rings -> per-strategy risk -> route -> encode
//   drop().pnl(id)        -> independent PnL per strategy
//   CapitalAllocator      -> rolling-Sharpe softmax rebalance across the fleet
//
#include "gel/platform/gel_engine.hpp"
#include "gel/platform/broker_client.hpp"
#include "gel/strategies/pcta.hpp"
#include "gel/strategies/market_maker.hpp"
#include "gel/strategies/momentum_strat.hpp"
#include "gel/strategies/mean_reversion_strat.hpp"
#include "gel/strategies/breakout_strat.hpp"
#include "gel/strategies/rsi_strat.hpp"
#include "gel/strategies/bollinger_strat.hpp"
#include "gel/strategies/ofi_strat.hpp"
#include "gel/strategies/vwap_revert_strat.hpp"
#include "gel/strategies/macd_strat.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <vector>

using namespace connector;
using namespace gel::platform;
using namespace gel::strategies;

// Stub PCTA policy: open a long on every HIGH-impact event (Deniz's PPO+LLM
// agent runs cross-process in production; this keeps the demo self-contained).
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
        o.price_ticks     = -1;
        o.quantity        = 1000;
        o.side            = static_cast<uint8_t>(Side::BUY);
        o.ord_type        = static_cast<uint8_t>(OrdType::MARKET);
        o.tif             = static_cast<uint8_t>(TIF::DAY);
        sink.send(o);
    }
    uint64_t coid_{1000};
};

int main() {
    auto engine_ptr = std::make_unique<GelEngine>();
    auto& engine = *engine_ptr;

    const auto sym = engine.symbols().register_symbol("EURUSD", Venue::OANDA, 100, 1);

    auto broker = std::make_shared<SimBrokerClient>("SIM");
    const auto acct = engine.register_account("SIM", broker, 1'000'000'00000000LL);

    // ---- Build the 10-strategy fleet ---------------------------------------
    struct Entry { StrategyId id; std::shared_ptr<IStrategy> strat; const char* name; };
    std::vector<Entry> fleet;

    auto pcta = std::make_shared<PCTAStrategy>(
        PctaConfig{}, std::make_unique<StubPctaPolicy>(sym));
    auto mm = std::make_shared<MarketMakerStrategy>(MmConfig{
        .gamma = 0.1, .kappa = 1.5, .sigma2_init = 1e-7, .ewma_alpha = 0.05,
        .base_quote_qty = 100, .max_abs_inventory = 5'000});

    fleet.push_back({StrategyId::PCTA,          pcta, "PCTA (bandit+LLM)"});
    fleet.push_back({StrategyId::MARKET_MAKER,  mm,   "Market Making (A-S)"});
    fleet.push_back({static_cast<StrategyId>(3),  std::make_shared<MomentumStrategy>(),           "Momentum (dual-SMA)"});
    fleet.push_back({static_cast<StrategyId>(4),  std::make_shared<MeanReversionStrategy>(),      "Mean Reversion (z-score)"});
    fleet.push_back({static_cast<StrategyId>(5),  std::make_shared<BreakoutStrategy>(),           "Breakout (Donchian)"});
    fleet.push_back({static_cast<StrategyId>(6),  std::make_shared<RsiStrategy>(),                "RSI(14)"});
    fleet.push_back({static_cast<StrategyId>(7),  std::make_shared<BollingerStrategy>(),          "Bollinger(20,2)"});
    fleet.push_back({static_cast<StrategyId>(8),  std::make_shared<OrderFlowImbalanceStrategy>(), "Order-Flow Imbalance"});
    fleet.push_back({static_cast<StrategyId>(9),  std::make_shared<VwapReversionStrategy>(),      "VWAP Reversion"});
    fleet.push_back({static_cast<StrategyId>(10), std::make_shared<MacdStrategy>(),               "MACD(12,26,9)"});

    // Route every strategy to the shared account, give each an independent risk
    // cell, and register it. This is the entire "plug in a strategy" ceremony.
    for (auto& e : fleet) {
        engine.account_routing().set_default(e.id, acct);
        engine.risk().set_limits(e.id, RiskLimits{
            .max_abs_position    = 100'000'000,
            .max_order_qty       = 1'000'000,
            .fat_finger_band_pct = 0.90,
            .max_orders_per_sec  = 100'000'000,
        });
        engine.register_strategy(e.strat);
    }

    engine.enable_capital_allocator(CapitalAllocatorConfig{
        .interval_secs = 1, .total_cap_pool = 1'000'000,
        .min_cap_per_cell = 1'000, .temperature = 1.0, .rolling_window = 20});

    // ---- Synthetic feed: ticks + top-of-book + one FOMC event --------------
    std::mt19937_64 rng{42};
    std::normal_distribution<double> noise{0.0, 0.00005};
    double mid = 1.0750;
    const int N = 50'000;

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        mid += noise(rng);
        const double half = 0.00005;

        Tick t{};
        t.ts_ns     = now_ns();
        t.symbol_id = sym;
        t.seq       = static_cast<uint32_t>(i);
        t.bid_ticks = to_ticks(mid - half);
        t.ask_ticks = to_ticks(mid + half);
        engine.publish_tick(t);

        // Top-of-book snapshot with an oscillating queue imbalance so the
        // order-flow strategy has a live signal to trade.
        BookUpdate b{};
        b.ts_ns     = t.ts_ns;
        b.symbol_id = sym;
        b.seq       = static_cast<uint32_t>(i);
        const int64_t swing = static_cast<int64_t>(60.0 * std::sin(i * 0.01));
        b.bids[0] = BookLevel{t.bid_ticks, 100 + swing};
        b.asks[0] = BookLevel{t.ask_ticks, 100 - swing};
        for (int k = 1; k < BOOK_DEPTH; ++k) {
            b.bids[k] = BookLevel{t.bid_ticks - k * 100, 50};
            b.asks[k] = BookLevel{t.ask_ticks + k * 100, 50};
        }
        engine.publish_book(b);

        if (i == N / 2) {
            CalendarEvent e{};
            e.event_ts_ns  = t.ts_ns;
            e.ingest_ts_ns = t.ts_ns;
            e.symbol_id    = 0;
            e.kind         = static_cast<uint8_t>(EventKind::FOMC);
            e.impact       = static_cast<uint8_t>(EventImpact::HIGH);
            engine.publish_event(e);
        }

        engine.run_once();
    }
    auto t1 = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    // ---- Scoreboard: independent PnL per strategy --------------------------
    std::printf("\n================ GEL 10-STRATEGY PLATFORM DEMO ================\n");
    std::printf("Ticks: %d in %lld ms   |   strategies sharing ONE engine: %zu\n\n",
                N, (long long)ms, fleet.size());
    std::printf("  %-4s %-24s %10s %8s %16s\n", "id", "strategy", "fills", "rej", "pnl_ticks");
    std::printf("  ------------------------------------------------------------------\n");
    long long total_fills = 0;
    for (auto& e : fleet) {
        const auto p = engine.drop().pnl(e.id);
        total_fills += (long long)p.filled_count;
        std::printf("  %-4u %-24s %10lld %8lld %16lld\n",
                    static_cast<uint32_t>(e.id), e.name,
                    (long long)p.filled_count, (long long)p.rejected_count,
                    (long long)p.realized_pnl_ticks);
    }
    std::printf("  ------------------------------------------------------------------\n");

    const auto p50 = engine.telemetry().percentile_ns(Hop::IPC_DEQUEUE, Hop::WIRE_OUT, 0.50);
    const auto p99 = engine.telemetry().percentile_ns(Hop::IPC_DEQUEUE, Hop::WIRE_OUT, 0.99);
    std::printf("  total fills across fleet: %lld\n", total_fills);
    std::printf("  host tick-to-wire  p50: %llu ns   p99: %llu ns\n",
                (unsigned long long)p50, (unsigned long long)p99);
    std::printf("===============================================================\n\n");

    engine.shutdown();
    return 0;
}
