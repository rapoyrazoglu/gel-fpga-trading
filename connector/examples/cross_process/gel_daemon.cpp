// examples/cross_process/gel_daemon.cpp
//
// Minimal stand-in for the real GEL engine. Demonstrates how the
// engine-process side of the connector::ShmServer works:
//
//   - Create a pair of mmap'd ring files
//   - Spin draining orders from the strategy
//   - For every order, push a "filled" ExecReport back
//
// In real GEL this is where risk + router + drop-copy would live.
//
// Run alongside a Python or C++ strategy process:
//
//     ./build/examples/cross_process/gel_daemon \
//         /tmp/orders.shm /tmp/reports.shm
//
#include "connector/shm_client.hpp"
#include "connector/wire.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

namespace {
std::atomic<bool> g_stop{false};

void handle_sigint(int) { g_stop.store(true); }

uint64_t now_ns() {
    using clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            clock::now().time_since_epoch()).count());
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <orders_path> <reports_path>\n", argv[0]);
        return 2;
    }
    const std::string orders_path  = argv[1];
    const std::string reports_path = argv[2];

    std::signal(SIGINT,  handle_sigint);
    std::signal(SIGTERM, handle_sigint);

    std::printf("gel_daemon: orders=%s reports=%s\n",
                orders_path.c_str(), reports_path.c_str());

    connector::ShmServer server(orders_path, reports_path);

    std::uint64_t exch_oid = 1;
    std::uint64_t filled   = 0;

    timespec idle{0, 1'000'000}; // 1 ms

    while (!g_stop.load()) {
        connector::Order o{};
        if (!server.poll_order(o)) {
            nanosleep(&idle, nullptr);
            continue;
        }

        connector::ExecReport r{};
        r.ts_ns             = now_ns();
        r.client_order_id   = o.client_order_id;
        r.exchange_order_id = exch_oid++;
        r.strategy_id       = o.strategy_id;
        r.symbol_id         = o.symbol_id;
        r.fill_price_ticks  = (o.price_ticks == -1) ? connector::to_ticks(1.0)
                                                    : o.price_ticks;
        r.fill_quantity     = o.quantity;
        r.status            = static_cast<uint8_t>(connector::ExecStatus::FILLED);
        r.side              = o.side;
        r.account_id        = o.account_id;

        while (!server.send_report(r) && !g_stop.load()) {
            // strategy isn't draining — back off briefly
            nanosleep(&idle, nullptr);
        }
        ++filled;

        if ((filled & 0xFFFF) == 0) {
            std::printf("gel_daemon: filled=%llu pending_orders=%zu\n",
                        static_cast<unsigned long long>(filled),
                        server.orders_pending());
        }
    }

    std::printf("gel_daemon: shutting down. filled=%llu\n",
                static_cast<unsigned long long>(filled));
    return 0;
}
