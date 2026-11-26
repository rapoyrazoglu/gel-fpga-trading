// connector/sinks.hpp
//
// Concrete OrderSink implementations for in-process use.
//
//   SpscOrderSink   — production: ring-buffer-backed, lock-free.
//   DirectOrderSink — testing: pushes straight into a callback.
//
// Cross-process sink lives in shm_client.hpp.
//
#pragma once

#include "ring.hpp"
#include "strategy.hpp"
#include <functional>

namespace connector {

    using OrderRing = SpscRing<Order, DEFAULT_ORDER_RING_CAP>;

    // Ring-backed sink (production path).
    class SpscOrderSink final : public OrderSink {
    public:
        explicit SpscOrderSink(OrderRing& ring) noexcept : ring_(ring) {}

        bool send(const Order& o) noexcept override {
            return ring_.try_push(o);
        }

    private:
        OrderRing& ring_;
    };

    // Callback sink (in-process tests).
    class DirectOrderSink final : public OrderSink {
    public:
        using Callback = std::function<bool(const Order&)>;
        explicit DirectOrderSink(Callback cb) : cb_(std::move(cb)) {}

        bool send(const Order& o) noexcept override {
            try {
                return cb_(o);
            } catch (...) {
                return false;
            }
        }

    private:
        Callback cb_;
    };

} // namespace connector
