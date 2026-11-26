// tests/shm_roundtrip_test.cpp
//
// Cross-process round-trip on the SHM rings.
//
// fork() into a parent + child pair. Parent acts as the engine
// (ShmServer): drains orders, sends ExecReports back. Child acts as the
// strategy (ShmClient): sends orders, polls reports.
//
// Validates that:
//   - mmap'd Order/ExecReport structs survive a write/read across
//     processes (POD layout + cacheline alignment intact)
//   - head/tail atomics see the producer's write from the consumer side
//   - account_id is preserved end-to-end (sanity for the new field)
//
#include "connector/shm_client.hpp"
#include "connector/wire.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using connector::ExecReport;
using connector::Order;
using connector::ShmClient;
using connector::ShmServer;

namespace {

constexpr int      N_ORDERS     = 10'000;
constexpr uint32_t TEST_ACCOUNT = 0xA1B2C3D4u;

std::string tmp_path(const char* tag) {
    std::string p = "/tmp/connector_shm_test_";
    p += std::to_string(::getpid());
    p += "_";
    p += tag;
    return p;
}

int run_child(const std::string& orders_path, const std::string& reports_path) {
    // Wait briefly for the parent to create the rings before we attach.
    // (If we attached too early, ShmClient::ctor would throw.)
    timespec ts{0, 50'000'000}; // 50 ms
    nanosleep(&ts, nullptr);

    ShmClient client(orders_path, reports_path);

    int sent = 0, recvd = 0;
    while (sent < N_ORDERS || recvd < N_ORDERS) {
        if (sent < N_ORDERS) {
            Order o{};
            o.ts_ns           = static_cast<uint64_t>(sent);
            o.client_order_id = static_cast<uint64_t>(sent);
            o.strategy_id     = 1;
            o.symbol_id       = 42;
            o.account_id      = TEST_ACCOUNT;
            o.quantity        = 100;
            o.side            = 0;
            o.ord_type        = 0;
            o.price_ticks     = -1;
            if (client.send_order(o)) ++sent;
        }
        ExecReport r{};
        while (client.poll_report(r)) {
            if (r.account_id != TEST_ACCOUNT) {
                std::fprintf(stderr, "child: account_id mismatch on report %d\n", recvd);
                return 1;
            }
            ++recvd;
        }
    }
    std::printf("child:  sent=%d  recvd=%d\n", sent, recvd);
    return (sent == N_ORDERS && recvd == N_ORDERS) ? 0 : 1;
}

int run_parent(pid_t child_pid,
               const std::string& orders_path, const std::string& reports_path) {
    ShmServer server(orders_path, reports_path);

    int relayed = 0;
    while (relayed < N_ORDERS) {
        Order o{};
        if (!server.poll_order(o)) continue;

        ExecReport r{};
        r.ts_ns             = o.ts_ns;
        r.client_order_id   = o.client_order_id;
        r.strategy_id       = o.strategy_id;
        r.symbol_id         = o.symbol_id;
        r.fill_price_ticks  = 1'070'000'000; // dummy
        r.fill_quantity     = o.quantity;
        r.status            = 3; // FILLED
        r.side              = o.side;
        r.account_id        = o.account_id; // <- preserves the routing tag
        while (!server.send_report(r)) {
            // strategy not draining yet; spin
        }
        ++relayed;
    }
    std::printf("parent: relayed=%d\n", relayed);

    int child_status = 0;
    waitpid(child_pid, &child_status, 0);
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        std::fprintf(stderr, "child exited with status %d\n", child_status);
        return 1;
    }
    return 0;
}

} // namespace

int main() {
    const std::string orders_path  = tmp_path("orders");
    const std::string reports_path = tmp_path("reports");

    // Clean up any leftovers from a prior aborted run.
    ::unlink(orders_path.c_str());
    ::unlink(reports_path.c_str());

    pid_t pid = ::fork();
    if (pid < 0) {
        std::perror("fork");
        return 1;
    }

    int rc = 0;
    if (pid == 0) {
        rc = run_child(orders_path, reports_path);
        std::_Exit(rc);
    } else {
        rc = run_parent(pid, orders_path, reports_path);
    }

    ::unlink(orders_path.c_str());
    ::unlink(reports_path.c_str());

    if (rc == 0) {
        std::printf("\n=== shm round-trip test: PASS ===\n\n");
    } else {
        std::printf("\n=== shm round-trip test: FAIL ===\n\n");
    }
    return rc;
}
