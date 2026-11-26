// MMX — sim/replay_main.cpp
// Tiny CLI wrapper around LobSimulator. Drives a synthetic random-walk LOB
// when no input file is given. Useful as a smoke-test target.

#include <cstdio>
#include <cstdint>
#include <random>
#include <vector>

#include "lob_simulator.h"

int main(int argc, char** argv) {
    int   ticks       = (argc >= 2) ? std::atoi(argv[1]) : 100000;
    int   seed        = (argc >= 3) ? std::atoi(argv[2]) : 42;
    int   start_price = 10000;

    std::mt19937 rng(seed);
    std::normal_distribution<double> step(0.0, 0.5);
    std::uniform_int_distribution<int> trade_dist(0, 9);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<uint32_t> qty_dist(1, 5);

    std::vector<mmx::LobEvent> events;
    events.reserve(ticks);

    double mid = start_price;
    std::uint64_t ts = 0;
    for (int i = 0; i < ticks; ++i) {
        mid += step(rng);
        if (mid < 1.0) mid = 1.0;
        int32_t bid_t = static_cast<int32_t>(mid - 0.5);
        int32_t ask_t = bid_t + 1;
        ts += 1'000'000ULL;  // 1 ms ticks

        mmx::LobEvent ev{};
        ev.ts_ns = ts;
        ev.bid_ticks = bid_t;
        ev.ask_ticks = ask_t;
        ev.bid_qty   = 10;
        ev.ask_qty   = 10;
        if (trade_dist(rng) == 0) {
            ev.trade_side  = side_dist(rng) ? +1 : -1;
            ev.trade_qty   = qty_dist(rng);
            ev.trade_price = ev.trade_side > 0 ? ask_t : bid_t;
        }
        events.push_back(ev);
    }

    std::vector<std::uint8_t> key(32, 0xab);
    mmx::SimParams sp;
    sp.requote_ticks   = 1;
    sp.refresh_period_ns = 100'000'000ULL;
    mmx::LobSimulator sim(sp, key);
    sim.load_events(std::move(events));
    auto r = sim.run();

    std::printf("MMX backtest — %d ticks, seed %d\n", ticks, seed);
    std::printf("  fills          = %d (bid=%d ask=%d)\n", r.fills, r.fills_bid, r.fills_ask);
    std::printf("  pnl_total      = %.4f\n", r.pnl_total);
    std::printf("  sharpe         = %.4f\n", r.sharpe);
    std::printf("  sortino        = %.4f\n", r.sortino);
    std::printf("  max_drawdown   = %.4f\n", r.max_drawdown);
    std::printf("  inv_l1_norm    = %.4f\n", r.inventory_l1_norm);
    std::printf("  avg_quote_age  = %.2f ms\n", r.avg_quote_age_ms);
    std::printf("  toxic_share    = %.4f\n", r.toxic_fill_share);
    return 0;
}
