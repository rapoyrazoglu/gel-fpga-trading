// include/gel/platform/broker_client.hpp
//
// IBrokerClient — abstract broker. Each broker (OANDA, IB, Binance,
// AWS F2 FPGA, simulation) implements this interface.
//
// One client instance per (broker, credential set). The same broker
// can host multiple AccountStates if it supports sub-accounts.
//
#pragma once

#include "connector/wire.hpp"
#include "order_formatter.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <string>

namespace gel::platform {

    using connector::Order;
    using connector::ExecReport;

    enum class BrokerKind : uint8_t {
        UNKNOWN  = 0,
        SIM      = 1,    // synthetic — used in tests + demo
        OANDA    = 2,
        IB       = 3,    // Interactive Brokers
        BINANCE  = 4,
        AWS_F2   = 5,    // FPGA path on AWS F2
    };

    using FillCallback = std::function<void(const ExecReport&)>;

    class IBrokerClient {
    public:
        virtual ~IBrokerClient() = default;
        virtual BrokerKind kind() const noexcept = 0;
        virtual std::string name() const = 0;

        // Hot path. Strategy_id, account_id and client_order_id are all
        // present in the WireFrame; the broker is free to read whichever
        // it needs to ack the right strategy.
        virtual void dispatch(const Order& o, const WireFrame& frame) = 0;

        // GEL registers a single fill callback per broker — DropCopy.
        virtual void set_fill_callback(FillCallback cb) = 0;
    };

    // ----------------------------------------------------------------------
    // SimBrokerClient — synthetic broker for tests + demo. Fills every
    // order at the request price/qty after a configurable latency. Real
    // broker clients (OANDAClient, IBClient, BinanceClient, F2Client) just
    // implement the same interface.
    // ----------------------------------------------------------------------
    class SimBrokerClient final : public IBrokerClient {
    public:
        explicit SimBrokerClient(std::string name = "SIM") : name_(std::move(name)) {}
        BrokerKind  kind() const noexcept override { return BrokerKind::SIM; }
        std::string name() const override          { return name_; }

        void set_fill_callback(FillCallback cb) override { cb_ = std::move(cb); }

        void dispatch(const Order& o, const WireFrame& /*frame*/) override {
            using namespace std::chrono;
            ExecReport r{};
            r.ts_ns             = static_cast<uint64_t>(
                                      duration_cast<nanoseconds>(
                                          system_clock::now().time_since_epoch()).count());
            r.client_order_id   = o.client_order_id;
            r.exchange_order_id = next_exch_id_.fetch_add(1, std::memory_order_relaxed);
            r.strategy_id       = o.strategy_id;
            r.symbol_id         = o.symbol_id;
            r.account_id        = o.account_id;
            r.fill_price_ticks  = o.price_ticks;
            r.fill_quantity     = o.quantity;
            r.status            = static_cast<uint8_t>(connector::ExecStatus::FILLED);
            r.side              = o.side;
            if (cb_) cb_(r);
        }

    private:
        std::string           name_;
        FillCallback          cb_;
        std::atomic<uint64_t> next_exch_id_{1};
    };

} // namespace gel::platform
