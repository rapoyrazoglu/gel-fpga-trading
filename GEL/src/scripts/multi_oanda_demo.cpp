// src/scripts/multi_oanda_demo.cpp
//
// Smoke demo: GEL with TWO concurrent OANDA practice accounts.
//
// Each account is an OandaBrokerAdapter instance — one worker thread, one
// MPSC queue, isolated rate limit per token. The integrated demo uses the
// same wiring; the only thing that changes is the broker class.
//
// To actually hit OANDA the environment must define:
//
//     export OANDA_TOKEN_A=<practice api token, account 1>
//     export OANDA_ACCOUNT_A=<oanda account id>
//     export OANDA_TOKEN_B=<practice api token, account 2>
//     export OANDA_ACCOUNT_B=<oanda account id>
//
// Without those, the demo still compiles and runs but skips the live
// dispatch step and prints a message. This way the build target is
// always green; live verification is opt-in.
//
#include "gel/platform/gel_engine.hpp"
#include "gel/broker/cpu/oanda_broker_adapter.hpp"
#include "gel/strategies/pcta.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <chrono>

using namespace connector;
using namespace gel::platform;
using namespace gel::strategies;
using gel::broker::cpu::OandaBrokerAdapter;

namespace {
const char* envz(const char* k) { const char* v = std::getenv(k); return v ? v : ""; }
}

// Stub PCTA policy: emits a single market BUY when on_event fires,
// regardless of impact (so the smoke test always exercises the path).
struct SmokePolicy : public IPctaPolicy {
    SymbolId target_symbol;
    explicit SmokePolicy(SymbolId s) : target_symbol(s) {}
    void on_event(const CalendarEvent&, OrderSink& sink) override {
        Order o{};
        o.client_order_id = ++coid_;
        o.strategy_id     = static_cast<uint32_t>(StrategyId::PCTA);
        o.symbol_id       = target_symbol;
        o.quantity        = 100;
        o.side            = static_cast<uint8_t>(Side::BUY);
        o.ord_type        = static_cast<uint8_t>(OrdType::MARKET);
        o.tif             = static_cast<uint8_t>(TIF::DAY);
        o.price_ticks     = -1;
        // account_id stays 0 → AccountRouter will fill from policy.
        sink.send(o);
    }
    uint64_t coid_{1};
};

int main() {
    const std::string tok_a = envz("OANDA_TOKEN_A");
    const std::string acc_a = envz("OANDA_ACCOUNT_A");
    const std::string tok_b = envz("OANDA_TOKEN_B");
    const std::string acc_b = envz("OANDA_ACCOUNT_B");
    const bool live_mode = !tok_a.empty() && !acc_a.empty() && !tok_b.empty() && !acc_b.empty();

    if (!live_mode) {
        std::printf("\n=== multi-OANDA smoke demo ===\n");
        std::printf("OANDA_TOKEN_A/ACCOUNT_A and OANDA_TOKEN_B/ACCOUNT_B not set.\n");
        std::printf("Build verified compiled OK; live dispatch skipped.\n");
        std::printf("(set the env vars to actually fire orders against the practice API.)\n");
        std::printf("==============================\n\n");
        return 0;
    }

    auto engine_ptr = std::make_unique<GelEngine>();
    auto& engine = *engine_ptr;

    const auto eurusd = engine.symbols().register_symbol(
        "EURUSD", Venue::OANDA, /*tick=*/100, /*lot=*/1);

    // Build the two OANDA adapters. The injected lambda translates GEL's
    // SymbolId back into OANDA's "EUR_USD" instrument string.
    auto oanda_resolver = [eurusd](SymbolId id) -> std::string {
        return id == eurusd ? "EUR_USD" : "";
    };

    auto adapter_a = std::make_shared<OandaBrokerAdapter>(OandaBrokerAdapter::Config{
        .api_token       = tok_a,
        .account_id      = acc_a,
        .use_practice    = true,
        .timeout_seconds = 10,
        .resolve_symbol  = oanda_resolver,
        .display_name    = "OANDA_A",
    });
    auto adapter_b = std::make_shared<OandaBrokerAdapter>(OandaBrokerAdapter::Config{
        .api_token       = tok_b,
        .account_id      = acc_b,
        .use_practice    = true,
        .timeout_seconds = 10,
        .resolve_symbol  = oanda_resolver,
        .display_name    = "OANDA_B",
    });

    const auto acct_a = engine.register_account("OANDA_A", adapter_a, /*balance=*/1'000'00000000LL);
    [[maybe_unused]] const auto acct_b =
        engine.register_account("OANDA_B", adapter_b, /*balance=*/1'000'00000000LL);

    // Default routing: PCTA → A, anything else → B.
    engine.account_routing().set_default(StrategyId::PCTA, acct_a);

    engine.risk().set_limits(StrategyId::PCTA, RiskLimits{
        .max_abs_position    = 1'000,
        .max_order_qty       = 200,
        .fat_finger_band_pct = 0.10,
        .max_orders_per_sec  = 10,
    });

    auto pcta = std::make_shared<PCTAStrategy>(
        PctaConfig{}, std::make_unique<SmokePolicy>(eurusd));
    engine.register_strategy(pcta);

    // Fire one event → adapter A enqueues, worker dispatches.
    CalendarEvent e{};
    e.kind   = static_cast<uint8_t>(EventKind::FOMC);
    e.impact = static_cast<uint8_t>(EventImpact::HIGH);
    engine.publish_event(e);
    engine.run_once();

    // Wait for the OANDA worker thread to drain. In production GEL has its
    // own loop; here the demo is one-shot.
    std::this_thread::sleep_for(std::chrono::seconds(3));
    engine.run_once();   // drain any reject/fill that came back

    std::printf("\n=== multi-OANDA smoke demo (live) ===\n");
    std::printf("Adapter A pending: %zu   Adapter B pending: %zu\n",
                adapter_a->pending(), adapter_b->pending());
    std::printf("PCTA  filled / rej:  %lld / %lld\n",
                (long long)engine.drop().pnl(StrategyId::PCTA).filled_count,
                (long long)engine.drop().pnl(StrategyId::PCTA).rejected_count);
    engine.drop().for_each_cell([&](StrategyId s, AccountId a, const CellPnL& c) {
        const auto* acc = engine.accounts().get(a);
        std::printf("  strategy=%u  account=%-10s  fills=%lld  rej=%lld\n",
                    static_cast<uint32_t>(s),
                    acc ? acc->name : "?",
                    (long long)c.filled_count.load(),
                    (long long)c.rejected_count.load());
        (void)a;
    });
    std::printf("=====================================\n\n");

    engine.shutdown();
    return 0;
}
