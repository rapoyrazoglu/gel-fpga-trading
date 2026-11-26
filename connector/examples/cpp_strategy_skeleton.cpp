// examples/cpp_strategy_skeleton.cpp
//
// What a brand-new C++ strategy looks like when it plugs into the
// connector SDK. This file is a *template* — copy it, rename the class,
// fill in the on_xxx callbacks.
//
// The strategy itself depends only on the connector. To actually run
// it under GEL, you'd link against gel::engine and register it via the
// engine's API; this skeleton uses an in-process DirectOrderSink so it
// can demonstrate the pattern without pulling in the engine.
//
#include "connector/sinks.hpp"
#include "connector/strategy.hpp"
#include "connector/wire.hpp"

#include <atomic>
#include <cstdio>

using namespace connector;

// ---------------------------------------------------------------------
// MyStrategy — a stub that fires a single market BUY on the first FOMC
// HIGH-impact event it sees, and tallies fills as they come back.
// ---------------------------------------------------------------------
class MyStrategy final : public IStrategy {
public:
    MyStrategy(SymbolId sid, AccountId aid)
        : symbol_(sid), account_(aid) {}

    StrategyId  id()   const noexcept override { return StrategyId::PCTA; }
    std::string name() const override          { return "skeleton"; }

    void on_start(OrderSink& sink) override { sink_ = &sink; }

    void on_tick(const Tick& t) override {
        // Real strategies update microstructure state here. Keep this
        // path branch-free — it runs millions of times a second.
        (void)t;
    }

    void on_event(const CalendarEvent& e) override {
        if (fired_)                                  return;
        if (e.kind   != static_cast<uint8_t>(EventKind::FOMC))    return;
        if (e.impact != static_cast<uint8_t>(EventImpact::HIGH))  return;

        Order o{};
        o.ts_ns           = e.ingest_ts_ns;
        o.client_order_id = ++next_coid_;
        o.strategy_id     = static_cast<uint32_t>(id());
        o.account_id      = account_;
        o.symbol_id       = symbol_;
        o.quantity        = 1000;
        o.side            = static_cast<uint8_t>(Side::BUY);
        o.ord_type        = static_cast<uint8_t>(OrdType::MARKET);
        o.tif             = static_cast<uint8_t>(TIF::DAY);
        o.price_ticks     = -1;

        if (sink_ && sink_->send(o)) fired_ = true;
    }

    void on_fill(const ExecReport& r) override {
        if (r.status == static_cast<uint8_t>(ExecStatus::FILLED)) ++fills_;
    }

    int fills() const noexcept { return fills_; }

private:
    SymbolId   symbol_;
    AccountId  account_;
    OrderSink* sink_       = nullptr;
    bool       fired_      = false;
    uint64_t   next_coid_  = 1000;
    int        fills_      = 0;
};

// ---------------------------------------------------------------------
// Demo harness: a DirectOrderSink that "fills" every order immediately,
// so we can drive the strategy end-to-end without an engine.
// ---------------------------------------------------------------------
int main() {
    MyStrategy strat(/*sid=*/1, /*account=*/0x10);

    int filled_count = 0;
    DirectOrderSink sink([&](const Order& o) {
        ExecReport r{};
        r.client_order_id  = o.client_order_id;
        r.strategy_id      = o.strategy_id;
        r.symbol_id        = o.symbol_id;
        r.fill_quantity    = o.quantity;
        r.fill_price_ticks = to_ticks(1.075);
        r.status           = static_cast<uint8_t>(ExecStatus::FILLED);
        r.side             = o.side;
        r.account_id       = o.account_id;
        strat.on_fill(r);
        ++filled_count;
        return true;
    });

    strat.on_start(sink);

    // Synthetic stream: 1000 ticks, FOMC event in the middle.
    for (uint32_t i = 0; i < 1000; ++i) {
        Tick t{};
        t.symbol_id = 1;
        t.seq       = i;
        t.bid_ticks = to_ticks(1.0750);
        t.ask_ticks = to_ticks(1.0750) + 5;
        strat.on_tick(t);

        if (i == 500) {
            CalendarEvent e{};
            e.event_ts_ns  = 0;
            e.ingest_ts_ns = 0;
            e.symbol_id    = 0;
            e.kind         = static_cast<uint8_t>(EventKind::FOMC);
            e.impact       = static_cast<uint8_t>(EventImpact::HIGH);
            strat.on_event(e);
        }
    }

    std::printf("skeleton: orders_sent=%d fills=%d\n",
                filled_count, strat.fills());
    return strat.fills() == 1 ? 0 : 1;
}
