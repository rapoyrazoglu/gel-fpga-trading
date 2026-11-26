// connector/shm_client.hpp
//
// Strategy-side wrapper around a pair of SHM rings.
//
// A strategy that runs in its own process (most commonly a Python PCTA
// agent talking to a C++ engine daemon) opens two ring files:
//
//     orders_path   — strategy → engine. Strategy is the producer.
//     reports_path  — engine   → strategy. Strategy is the consumer.
//
// The engine side does the mirror image: it creates both files
// (`/*create=*/true`) and acts as orders' consumer + reports' producer.
//
// Both sides drive the rings with try_push / try_pop directly; this
// wrapper just holds them together with a little ergonomic glue.
//
#pragma once

#include "shm_ring.hpp"
#include "strategy.hpp"
#include "wire.hpp"

#include <string>

namespace connector {

    // -------------------------------------------------------------------
    // Strategy-process side.
    // -------------------------------------------------------------------
    // Constructs by attaching (create=false) to ring files the engine
    // already created. Use ShmServer (below) on the engine side.
    class ShmClient {
    public:
        ShmClient(const std::string& orders_path,
                  const std::string& reports_path)
            : orders_(orders_path,  /*create=*/false)
            , reports_(reports_path, /*create=*/false) {}

        // Push an order to the engine. Returns false if the ring is full.
        bool send_order(const Order& o) noexcept { return orders_.try_push(o); }

        // Pull the next ExecReport, if any. Returns false if no message.
        bool poll_report(ExecReport& out) noexcept { return reports_.try_pop(out); }

        std::size_t orders_pending()  const noexcept { return orders_.size_approx(); }
        std::size_t reports_pending() const noexcept { return reports_.size_approx(); }

    private:
        OrderShmRing  orders_;
        ReportShmRing reports_;
    };

    // -------------------------------------------------------------------
    // Engine-process side.
    // -------------------------------------------------------------------
    // Creates the ring files (truncate + zero header). Drains incoming
    // orders from each strategy, writes ExecReports back.
    class ShmServer {
    public:
        ShmServer(const std::string& orders_path,
                  const std::string& reports_path)
            : orders_(orders_path,  /*create=*/true)
            , reports_(reports_path, /*create=*/true) {}

        // Pull the next pending Order, if any. Engine drains in a loop.
        bool poll_order(Order& out) noexcept { return orders_.try_pop(out); }

        // Send an ExecReport back to the strategy. Returns false if the
        // strategy isn't keeping up — engine should consider that a fault.
        bool send_report(const ExecReport& r) noexcept { return reports_.try_push(r); }

        std::size_t orders_pending()  const noexcept { return orders_.size_approx(); }
        std::size_t reports_pending() const noexcept { return reports_.size_approx(); }

    private:
        OrderShmRing  orders_;
        ReportShmRing reports_;
    };

    // -------------------------------------------------------------------
    // OrderSink that pushes through a ShmClient — lets a C++ strategy
    // running cross-process satisfy the IStrategy contract just like
    // an in-process one.
    // -------------------------------------------------------------------
    class ShmOrderSink final : public OrderSink {
    public:
        explicit ShmOrderSink(ShmClient& client) noexcept : client_(client) {}

        bool send(const Order& o) noexcept override { return client_.send_order(o); }

    private:
        ShmClient& client_;
    };

} // namespace connector
