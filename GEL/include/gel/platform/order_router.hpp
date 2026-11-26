// include/gel/platform/order_router.hpp
//
// Block ⑥ in deck slide 6 — venue selection and dispatch.
//
// In v2 (multi-account) the Router is a thin dispatcher:
//
//     Order ──► Router ──► AccountRegistry.broker(account_id) ──► IBrokerClient
//
// Each broker (OANDA, IB, Binance, AWS F2 FPGA, SIM) implements
// IBrokerClient and lives behind an AccountState. The Router has no
// per-broker logic — it just looks up the right client and forwards.
//
// Backwards compat: a "default" broker is kept for orders that never
// got an account_id assigned (legacy demos, unit tests). The Account
// Router runs before this and fills account_id from policy in production.
//
#pragma once

#include "connector/wire.hpp"
#include "account_registry.hpp"
#include "broker_client.hpp"
#include "order_formatter.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>

namespace gel::platform {

    using connector::Order;
    using connector::ExecReport;
    using connector::ExecStatus;

    using ReportSink = std::function<void(const ExecReport&)>;

    // -------------------------------------------------------------------
    // Multi-broker router. Looks up Order.account_id → broker → dispatch.
    // -------------------------------------------------------------------
    class MultiBrokerRouter {
    public:
        explicit MultiBrokerRouter(AccountRegistry& accounts) : accounts_(accounts) {}

        // GEL passes a single sink — every broker forwards through it.
        void set_report_sink(ReportSink s) {
            sink_ = std::move(s);
            // Wire all currently-registered brokers (and any registered later).
            accounts_.for_each([this](AccountState& a) {
                if (auto* b = accounts_.broker(a.id); b) b->set_fill_callback(sink_);
            });
        }

        // Optional fallback broker for legacy/test paths.
        void set_default_broker(std::shared_ptr<IBrokerClient> b) {
            default_broker_ = std::move(b);
            if (default_broker_ && sink_) default_broker_->set_fill_callback(sink_);
        }

        void dispatch(const Order& o, const WireFrame& frame) {
            if (auto* b = accounts_.broker(o.account_id); b) {
                b->dispatch(o, frame);
                return;
            }
            if (default_broker_) {
                default_broker_->dispatch(o, frame);
                return;
            }
            // No broker → synthesize a REJECTED report.
            using namespace std::chrono;
            ExecReport r{};
            r.ts_ns           = static_cast<uint64_t>(
                                    duration_cast<nanoseconds>(
                                        system_clock::now().time_since_epoch()).count());
            r.client_order_id = o.client_order_id;
            r.strategy_id     = o.strategy_id;
            r.account_id      = o.account_id;
            r.symbol_id       = o.symbol_id;
            r.side            = o.side;
            r.status          = static_cast<uint8_t>(connector::ExecStatus::REJECTED);
            r.reject_reason   = static_cast<uint8_t>(connector::RejectReason::UNKNOWN_SYMBOL);
            if (sink_) sink_(r);
        }

    private:
        AccountRegistry&                accounts_;
        ReportSink                      sink_;
        std::shared_ptr<IBrokerClient>  default_broker_;
    };

    // -------------------------------------------------------------------
    // Backwards-compat aliases for the v1 single-router demos.
    // CpuRouter / FpgaRouter were just SimBrokerClient / FPGA backends —
    // they're kept here as helper factories so old call sites still work.
    // -------------------------------------------------------------------
    inline std::shared_ptr<IBrokerClient> make_cpu_router() {
        return std::make_shared<SimBrokerClient>("CPU");
    }

} // namespace gel::platform
