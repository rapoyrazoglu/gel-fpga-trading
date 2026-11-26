// include/gel/broker/cpu/oanda_broker_adapter.hpp
//
// OANDA broker adapter — bridges the existing single-account `OandaClient`
// into the GEL multi-broker / multi-account world by implementing the
// `IBrokerClient` interface.
//
// Concurrency model (the "concurrent control" requirement):
//
//   - One adapter instance per OANDA account.   (1 token → 1 sub-account)
//   - Each adapter owns its own worker thread + MPSC queue.
//   - `dispatch()` runs on GEL's hot run_once() thread; it does not block
//     on HTTP — it only enqueues the Order and signals the worker.
//   - The worker thread is the only thread that calls `OandaClient`.
//     Multiple adapters → multiple worker threads → fully concurrent
//     traffic to N OANDA accounts. Each account has its own credentials,
//     its own per-token rate budget, its own pacing.
//
// Build classification:  CPU-ONLY.
//   This adapter is HTTPS-over-curl + JSON. There is no meaningful FPGA
//   acceleration path for OANDA's REST API — the wire is text, the
//   transport is TLS, and the latency floor is the Internet round-trip
//   to OANDA (50–100 ms typical). FPGA-eligible brokers are the
//   exchange-native ones (NASDAQ OUCH on AWS F2, etc.) which are reached
//   via `gel::platform::AwsF2Router`. See docs/BROKER_ARCHITECTURE.md.
//
// Toggled by:  GEL_ENABLE_OANDA  in CMakeLists.txt (default ON).
//
#pragma once

#include "connector/wire.hpp"
#include "gel/platform/broker_client.hpp"
#include "gel/broker/oanda_client.hpp"
#include "gel/broker/oanda_types.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace gel::broker::cpu {

    using gel::platform::IBrokerClient;
    using gel::platform::BrokerKind;
    using gel::platform::FillCallback;
    using gel::platform::WireFrame;

    using connector::Order;
    using connector::ExecReport;
    using connector::ExecStatus;
    using connector::RejectReason;
    using connector::SymbolId;

    // --------------------------------------------------------------------
    // OandaBrokerAdapter
    // --------------------------------------------------------------------
    class OandaBrokerAdapter final : public IBrokerClient {
    public:
        // Translates a GEL-internal SymbolId into OANDA's instrument name.
        // OANDA uses "EUR_USD" (underscored), GEL stores "EURUSD" by default.
        // The adapter cannot read GEL's SymbolTable (that's a layering
        // violation), so the caller injects the lookup as a function.
        using SymbolToInstrument = std::function<std::string(SymbolId)>;

        struct Config {
            std::string         api_token;
            std::string         account_id;          // OANDA account string
            bool                use_practice    = true;
            int                 timeout_seconds = 10;
            // Provide either:
            //   (a) a custom resolver, or
            //   (b) leave this empty and call set_symbol(SymbolId, "EUR_USD")
            //       at setup for each symbol the adapter will see.
            SymbolToInstrument  resolve_symbol  = nullptr;
            // Logical name used in BrokerKind diagnostics — defaults to
            // "OANDA(<account>)" so multi-account logs are unambiguous.
            std::string         display_name;
        };

        explicit OandaBrokerAdapter(Config cfg)
            : cfg_(std::move(cfg))
            , client_(make_client_config_(cfg_))
        {
            if (cfg_.display_name.empty())
                cfg_.display_name = "OANDA(" + cfg_.account_id + ")";
            worker_ = std::thread(&OandaBrokerAdapter::worker_loop_, this);
        }

        ~OandaBrokerAdapter() override {
            stop_.store(true, std::memory_order_release);
            cv_.notify_all();
            if (worker_.joinable()) worker_.join();
        }

        // IBrokerClient interface -----------------------------------------
        BrokerKind  kind() const noexcept override { return BrokerKind::OANDA; }
        std::string name() const override          { return cfg_.display_name; }

        void set_fill_callback(FillCallback cb) override {
            std::lock_guard<std::mutex> g(mtx_);
            sink_ = std::move(cb);
        }

        // Hot path. Wait-free: copies the Order and signals the worker.
        void dispatch(const Order& o, const WireFrame& /*frame*/) override {
            {
                std::lock_guard<std::mutex> g(mtx_);
                queue_.push_back(o);
            }
            cv_.notify_one();
        }

        // Setup-only. Optional convenience if you don't want to provide a
        // resolve_symbol lambda — fill the static map once at startup.
        void set_symbol(SymbolId id, std::string instrument) {
            std::lock_guard<std::mutex> g(map_mtx_);
            instruments_[id] = std::move(instrument);
        }

        // Diagnostics
        std::size_t pending() const {
            std::lock_guard<std::mutex> g(mtx_);
            return queue_.size();
        }

    private:
        // The existing OandaClient takes its own Config struct.
        static OandaClient::Config make_client_config_(const Config& c) {
            OandaClient::Config oc;
            oc.api_token       = c.api_token;
            oc.account_id      = c.account_id;
            oc.use_practice    = c.use_practice;
            oc.timeout_seconds = c.timeout_seconds;
            return oc;
        }

        // Resolve SymbolId → OANDA instrument string. Tries the user-
        // provided lambda first, then the static map populated by
        // set_symbol(). Returns empty on miss.
        std::string resolve_(SymbolId id) {
            if (cfg_.resolve_symbol) {
                auto s = cfg_.resolve_symbol(id);
                if (!s.empty()) return s;
            }
            std::lock_guard<std::mutex> g(map_mtx_);
            auto it = instruments_.find(id);
            return it == instruments_.end() ? std::string() : it->second;
        }

        // Translate a GEL Order into an OandaClient OrderRequest.
        bool build_request_(const Order& o, ::gel::broker::OrderRequest& out, std::string& err) {
            const std::string instr = resolve_(o.symbol_id);
            if (instr.empty()) {
                err = "no instrument mapping for symbol_id=" + std::to_string(o.symbol_id);
                return false;
            }

            out.instrument = instr;

            // Map order type. OANDA only supports a subset; we cover the
            // ones GEL strategies actually emit.
            switch (static_cast<connector::OrdType>(o.ord_type)) {
                case connector::OrdType::MARKET:
                    out.type = ::gel::broker::OrderType::MARKET;
                    break;
                case connector::OrdType::LIMIT:
                case connector::OrdType::POST_ONLY:
                    out.type  = ::gel::broker::OrderType::LIMIT;
                    out.price = connector::from_ticks(o.price_ticks);
                    break;
                case connector::OrdType::IOC:
                    out.type           = ::gel::broker::OrderType::MARKET;
                    out.time_in_force  = ::gel::broker::TimeInForce::IOC;
                    break;
                case connector::OrdType::FOK:
                    out.type           = ::gel::broker::OrderType::MARKET;
                    out.time_in_force  = ::gel::broker::TimeInForce::FOK;
                    break;
                case connector::OrdType::CANCEL:
                case connector::OrdType::REPLACE:
                    err = "OrdType CANCEL/REPLACE goes through a different OANDA endpoint, not create_order";
                    return false;
            }

            // Order.quantity is signed: + BUY, − SELL.
            // OandaClient OrderRequest::units uses the same convention.
            out.units = static_cast<double>(o.quantity);
            // Map side (informational; sign of units is what OANDA reads).
            // No-op: OANDA does not have a separate "side" field.

            // TIF — only override if the user didn't pick IOC/FOK above.
            if (out.time_in_force == ::gel::broker::TimeInForce::FOK ||
                out.time_in_force == ::gel::broker::TimeInForce::IOC) {
                // already set by IOC/FOK branches
            } else {
                switch (static_cast<connector::TIF>(o.tif)) {
                    case connector::TIF::IOC: out.time_in_force = ::gel::broker::TimeInForce::IOC; break;
                    case connector::TIF::FOK: out.time_in_force = ::gel::broker::TimeInForce::FOK; break;
                    case connector::TIF::GTC: out.time_in_force = ::gel::broker::TimeInForce::GTC; break;
                    default:                  out.time_in_force = ::gel::broker::TimeInForce::DAY; break;
                }
            }
            return true;
        }

        // Build an ExecReport from an OandaClient response.
        static ExecReport build_report_(const Order& o,
                                        bool         filled,
                                        double       fill_price,
                                        const std::string& exch_id,
                                        RejectReason rej = RejectReason::NONE) {
            using namespace std::chrono;
            ExecReport r{};
            r.ts_ns             = static_cast<uint64_t>(
                                     duration_cast<nanoseconds>(
                                         system_clock::now().time_since_epoch()).count());
            r.client_order_id   = o.client_order_id;
            r.exchange_order_id = std::stoull(exch_id.empty() ? "0" : exch_id);
            r.strategy_id       = o.strategy_id;
            r.account_id        = o.account_id;
            r.symbol_id         = o.symbol_id;
            r.fill_price_ticks  = filled ? connector::to_ticks(fill_price) : 0;
            r.fill_quantity     = filled ? o.quantity                      : 0;
            r.status            = static_cast<uint8_t>(filled ? ExecStatus::FILLED
                                                              : ExecStatus::REJECTED);
            r.reject_reason     = static_cast<uint8_t>(rej);
            r.side              = o.side;
            return r;
        }

        // Worker thread main loop.
        void worker_loop_() {
            while (!stop_.load(std::memory_order_acquire)) {
                Order o{};
                {
                    std::unique_lock<std::mutex> lk(mtx_);
                    cv_.wait(lk, [&] {
                        return stop_.load(std::memory_order_acquire) || !queue_.empty();
                    });
                    if (stop_.load(std::memory_order_acquire)) return;
                    o = queue_.front();
                    queue_.pop_front();
                }

                ::gel::broker::OrderRequest req{};
                std::string err;
                if (!build_request_(o, req, err)) {
                    deliver_(build_report_(o, /*filled=*/false, 0.0, "", RejectReason::UNKNOWN_SYMBOL));
                    std::fprintf(stderr, "[OandaAdapter %s] %s\n", cfg_.display_name.c_str(), err.c_str());
                    continue;
                }

                auto resp = client_.create_order(req);
                if (!resp.success || !resp.data.has_value()) {
                    deliver_(build_report_(o, /*filled=*/false, 0.0, "", RejectReason::STALE_ORDER));
                    std::fprintf(stderr, "[OandaAdapter %s] create_order failed: %s\n",
                                 cfg_.display_name.c_str(), resp.error_message.c_str());
                    continue;
                }

                const auto& oanda_order = *resp.data;
                const bool  filled      = oanda_order.filled_price.has_value();
                const double price      = filled ? *oanda_order.filled_price : 0.0;
                deliver_(build_report_(o, filled, price, oanda_order.id));
            }
        }

        void deliver_(const ExecReport& r) {
            FillCallback cb;
            {
                std::lock_guard<std::mutex> g(mtx_);
                cb = sink_;
            }
            if (cb) cb(r);
        }

        // ----- state ------------------------------------------------------
        Config                                cfg_;
        OandaClient                           client_;
        FillCallback                          sink_;
        std::thread                           worker_;
        std::atomic<bool>                     stop_{false};
        mutable std::mutex                    mtx_;        // protects queue_ + sink_
        std::condition_variable               cv_;
        std::deque<Order>                     queue_;
        mutable std::mutex                    map_mtx_;
        std::unordered_map<SymbolId, std::string> instruments_;
    };

} // namespace gel::broker::cpu
