// connector/strategy.hpp
//
// IStrategy — the SDK contract every strategy implements to attach to
// the engine.
//
// Lifecycle (per strategy):
//
//   1. ctor                            — strategy is constructed by the host
//   2. on_start(sink)                  — once, before any data flows;
//                                        sink is the only door for orders
//   3. on_tick / on_book / on_event    — hot-path market data callbacks
//      on_fill                         — execution feedback
//   4. on_kill_switch / on_resume      — risk circuit breaker
//   5. on_stop()                       — once, on shutdown
//
// A strategy NEVER calls a broker directly. It calls OrderSink::send()
// which pushes onto a ring buffer; the engine drains the ring, runs
// risk/format/route, and pushes ExecReports back via on_fill.
//
#pragma once

#include "wire.hpp"
#include <string>

namespace connector {

    // -------------------------------------------------------------------
    // OrderSink — the only exit door for a strategy.
    // -------------------------------------------------------------------
    // Implementations live in sinks.hpp / shm_client.hpp:
    //   - SpscOrderSink   : in-process lock-free ring (production)
    //   - DirectOrderSink : straight callback (in-process tests)
    //   - ShmOrderSink    : cross-process mmap'd ring (Python ↔ C++ engine)
    class OrderSink {
    public:
        virtual ~OrderSink() = default;

        // Returns false if the ring is full. Strategy must decide whether
        // to retry, drop, or assert. Never blocks.
        virtual bool send(const Order& o) noexcept = 0;
    };

    // -------------------------------------------------------------------
    // IStrategy
    // -------------------------------------------------------------------
    class IStrategy {
    public:
        virtual ~IStrategy() = default;

        // Identity --------------------------------------------------------
        // Static identity used by the engine for risk attribution and audit.
        virtual StrategyId  id()   const noexcept = 0;
        virtual std::string name() const = 0;

        // Lifecycle -------------------------------------------------------
        // The OrderSink reference is owned by the engine; it stays alive
        // for the entire lifetime of the strategy.
        virtual void on_start(OrderSink& sink) { (void)sink; }
        virtual void on_stop()                 {}

        // Reconciliation hook — engine calls this once, after on_start, with
        // the orders that were still live in the book for this strategy when
        // the engine took over (e.g. after a hot restart). Strategies that
        // rehydrate per-order state should override; default is a no-op.
        // Pointer + count instead of std::span to keep the SDK C++17.
        virtual void on_open_orders(const Order* /*orders*/,
                                    std::size_t /*count*/) {}

        // Hot path --------------------------------------------------------
        // Default no-ops so a strategy can subscribe only to what it needs.
        // PCTA cares about ticks + events; MM cares about book updates.
        virtual void on_tick (const Tick&          t) { (void)t; }
        virtual void on_book (const BookUpdate&    b) { (void)b; }
        virtual void on_event(const CalendarEvent& e) { (void)e; }
        virtual void on_fill (const ExecReport&    r) { (void)r; }

        // Risk hook -------------------------------------------------------
        // Engine calls this when its kill-switch trips for this strategy.
        // The strategy MUST stop emitting orders until on_resume().
        virtual void on_kill_switch(RejectReason r) { (void)r; }
        virtual void on_resume() {}
    };

} // namespace connector
