// include/gel/platform/gel_engine.hpp
//
// The orchestrator. Wires the 7 blocks together and exposes the only
// API the rest of the program needs:
//
//     GelEngine engine;
//     engine.register_strategy(my_pcta);
//     engine.register_strategy(my_market_maker);
//     engine.publish_tick(t);            // → fanned out to subscribers
//     engine.publish_event(e);           // → only PCTA listens
//     engine.run_once();                 // drain rings, dispatch orders
//
// run_once() is what a backtest loop calls after every bar; in production
// a dedicated thread spins on it.
//
#pragma once

#include "connector/strategy.hpp"
#include "connector/sinks.hpp"
#include "connector/ring.hpp"
#include "risk_engine.hpp"
#include "symbol_table.hpp"
#include "account_registry.hpp"
#include "account_router.hpp"
#include "broker_client.hpp"
#include "capital_allocator.hpp"
#include "order_formatter.hpp"
#include "order_router.hpp"
#include "drop_copy.hpp"
#include "telemetry.hpp"
#include <array>
#include <memory>
#include <vector>

namespace gel::platform {

    using connector::IStrategy;
    using connector::Order;
    using connector::OrderRing;
    using connector::SpscOrderSink;
    using connector::Tick;
    using connector::BookUpdate;
    using connector::CalendarEvent;
    using connector::Hop;

    class GelEngine {
    public:
        GelEngine()
            : risk_(symbols_, accounts_), router_(accounts_)
        {
            router_.set_report_sink([this](const ExecReport& r) {
                risk_.on_fill(r);
                drop_copy_.on_report(r);
                telemetry_.stamp(r.client_order_id, r.strategy_id, Hop::DROP_COPY);
            });
            // Default broker for orders that arrive without account_id —
            // keeps v1 demos and unit tests working unchanged.
            router_.set_default_broker(make_cpu_router());
        }

        // Setup ----------------------------------------------------------
        SymbolTable&     symbols()  { return symbols_; }
        AccountRegistry& accounts() { return accounts_; }
        RiskEngine&      risk()     { return risk_;    }
        DropCopy&        drop()     { return drop_copy_; }
        Telemetry&       telemetry(){ return telemetry_; }
        MultiBrokerRouter& router()   { return router_; }
        AccountRouter&   account_routing() { return acct_router_; }

        // Opt-in capital allocator. Once enabled, run_once() will tick it.
        void enable_capital_allocator(CapitalAllocatorConfig cfg = {}) {
            allocator_ = std::make_unique<CapitalAllocator>(
                risk_, drop_copy_, accounts_, cfg);
        }
        CapitalAllocator* capital_allocator() { return allocator_.get(); }

        // Convenience: register a broker-backed account in one call.
        AccountId register_account(std::string_view name,
                                   std::shared_ptr<IBrokerClient> broker,
                                   int64_t initial_balance_ticks = 0) {
            const auto id = accounts_.register_account(name, broker, initial_balance_ticks);
            // New broker needs the same fill sink so reports flow back into GEL.
            if (broker) {
                broker->set_fill_callback([this](const ExecReport& r) {
                    risk_.on_fill(r);
                    drop_copy_.on_report(r);
                    telemetry_.stamp(r.client_order_id, r.strategy_id, Hop::DROP_COPY);
                });
            }
            return id;
        }

        // Register a strategy. The engine owns one ring buffer per slot
        // and hooks the strategy's on_fill into the drop-copy.
        // Returns true if the strategy was registered, false if it was dropped
        // (slot out of range or already occupied). Existing callers may ignore
        // the return value.
        bool register_strategy(std::shared_ptr<IStrategy> s) {
            const auto sid = s->id();
            const auto slot = static_cast<size_t>(sid);
            if (slot >= MAX_SLOTS || strategies_[slot]) {
                return false; // already registered or out of range
            }
            rings_[slot] = std::make_unique<OrderRing>();
            sinks_[slot] = std::make_unique<SpscOrderSink>(*rings_[slot]);
            strategies_[slot] = s;

            drop_copy_.set_strategy_callback(
                sid, [s](const ExecReport& r) { s->on_fill(r); });

            s->on_start(*sinks_[slot]);
            return true;
        }

        // Hot path — fan out market data to every registered strategy.
        void publish_tick(const Tick& t) {
            risk_.set_last_price(t.symbol_id, (t.bid_ticks + t.ask_ticks) / 2);
            for (auto& s : strategies_) if (s) s->on_tick(t);
        }
        void publish_book(const BookUpdate& b) {
            for (auto& s : strategies_) if (s) s->on_book(b);
        }
        void publish_event(const CalendarEvent& e) {
            for (auto& s : strategies_) if (s) s->on_event(e);
        }

        // Drain all strategy rings, push each Order through risk →
        // formatter → router → drop-copy. One pass per call.
        // Returns the number of orders processed.
        std::size_t run_once() {
            std::size_t processed = 0;
            for (size_t slot = 0; slot < MAX_SLOTS; ++slot) {
                if (!rings_[slot]) continue;
                Order o{};
                while (rings_[slot]->try_pop(o)) {
                    telemetry_.stamp(o.client_order_id, o.strategy_id, Hop::IPC_DEQUEUE);

                    // Step 0 — fill in account_id from policy if unset.
                    acct_router_.resolve(o);

                    const auto rej = risk_.check(o);
                    if (rej != connector::RejectReason::NONE) {
                        ExecReport r{};
                        r.ts_ns           = now_ns();
                        r.client_order_id = o.client_order_id;
                        r.strategy_id     = o.strategy_id;
                        r.symbol_id       = o.symbol_id;
                        r.side            = o.side;
                        r.status          = static_cast<uint8_t>(connector::ExecStatus::REJECTED);
                        r.reject_reason   = static_cast<uint8_t>(rej);
                        drop_copy_.on_report(r);
                        ++processed;
                        continue;
                    }
                    telemetry_.stamp(o.client_order_id, o.strategy_id, Hop::RISK_PASS);

                    const auto frame = OrderFormatter::encode(o);
                    telemetry_.stamp(o.client_order_id, o.strategy_id, Hop::ENCODE_DONE);

                    router_.dispatch(o, frame);
                    telemetry_.stamp(o.client_order_id, o.strategy_id, Hop::WIRE_OUT);

                    ++processed;
                }
            }
            // Periodic dynamic capital reallocation (no-op if not enabled).
            if (allocator_) allocator_->maybe_rebalance();
            return processed;
        }

        void shutdown() {
            for (auto& s : strategies_) if (s) s->on_stop();
        }

    private:
        static constexpr size_t MAX_SLOTS = 32; // mirrors RiskEngine slot count

        SymbolTable                                   symbols_;
        AccountRegistry                               accounts_;
        RiskEngine                                    risk_;
        DropCopy                                      drop_copy_;
        Telemetry                                     telemetry_;
        MultiBrokerRouter                             router_;
        AccountRouter                                 acct_router_;
        std::unique_ptr<CapitalAllocator>             allocator_;

        std::array<std::shared_ptr<IStrategy>, MAX_SLOTS> strategies_{};
        std::array<std::unique_ptr<OrderRing>,  MAX_SLOTS> rings_{};
        std::array<std::unique_ptr<SpscOrderSink>, MAX_SLOTS> sinks_{};
    };

} // namespace gel::platform
