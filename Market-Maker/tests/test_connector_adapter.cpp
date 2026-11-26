// MMX — tests/test_connector_adapter.cpp
//
// End-to-end check that mmx::MmxStrategy honours the connector::IStrategy
// contract: orders flow only through OrderSink, fills update inventory,
// kill-switch silences the strategy on both the IStrategy AND FPGA paths,
// HIGH-impact CalendarEvents flip the regime, partial fills accumulate at
// the contract level (no sub-lot drift), wrong-symbol ticks are rejected,
// and a saturated sink raises the dropped-order counter.
//
// Compiled only when MMX_BUILD_CONNECTOR_ADAPTER=ON.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "connector/mmx_strategy.hpp"
#include "connector/strategy.hpp"
#include "connector/wire.hpp"
#include "host/ipc/hmac.h"
#include "test_helpers.h"

namespace {

class CapturingSink final : public connector::OrderSink {
  public:
    bool send(const connector::Order& o) noexcept override {
        orders.push_back(o);
        return true;
    }
    std::vector<connector::Order> orders;
};

}  // namespace

int main() {
    using connector::PRICE_TICK_PER_UNIT;
    using connector::SymbolId;
    using connector::Tick;

    constexpr SymbolId kEurUsd  = 1;
    constexpr int64_t  kPipConn = 1000;  // 1e-5 in 1e-8 ticks

    mmx::ConnectorAdapterConfig cfg{};
    cfg.conn_ticks_per_mmx_tick = kPipConn;
    cfg.lot_to_qty              = 100;
    cfg.base_qty_lots           = 1;
    cfg.max_inv_lots            = 100;
    cfg.requote_ticks           = 1;
    cfg.refresh_period_ns       = 1'000'000ULL;  // 1 ms — fast for the test
    cfg.base_gamma              = 0.5;
    cfg.horizon_sec             = 60.0;
    cfg.hmac_key.assign(32, 0xCC);

    mmx::MmxStrategy strat(cfg);
    CapturingSink   sink;

    // Sanity: identity / name.
    MMX_EXPECT_EQ(static_cast<uint32_t>(strat.id()),
                  static_cast<uint32_t>(connector::StrategyId::MARKET_MAKER));
    MMX_EXPECT(strat.name() == std::string("MMX"));

    // Without on_start, on_tick must be a no-op (sink_ is null).
    Tick t0{};
    t0.ts_ns     = 1;
    t0.symbol_id = kEurUsd;
    t0.bid_ticks = static_cast<int64_t>(1.07500 * PRICE_TICK_PER_UNIT);
    t0.ask_ticks = static_cast<int64_t>(1.07501 * PRICE_TICK_PER_UNIT);
    strat.on_tick(t0);
    MMX_EXPECT(sink.orders.empty());

    strat.on_start(sink);

    // Drive a synthetic random-walk feed through on_tick.
    std::mt19937 rng(0xCAFE);
    std::normal_distribution<double> step(0.0, 1e-5);
    double mid = 1.07500;
    std::uint64_t ts = 1;

    for (int i = 0; i < 2000; ++i) {
        mid += step(rng);
        Tick t{};
        t.ts_ns     = (ts += 100'000ULL);
        t.symbol_id = kEurUsd;
        t.seq       = static_cast<uint32_t>(i);
        const double half = 5e-6;
        t.bid_ticks = static_cast<int64_t>((mid - half) * PRICE_TICK_PER_UNIT);
        t.ask_ticks = static_cast<int64_t>((mid + half) * PRICE_TICK_PER_UNIT);
        strat.on_tick(t);
    }

    MMX_EXPECT(!sink.orders.empty());

    int n_buy = 0, n_sell = 0;
    for (const auto& o : sink.orders) {
        MMX_EXPECT_EQ(o.strategy_id,
                      static_cast<uint32_t>(connector::StrategyId::MARKET_MAKER));
        MMX_EXPECT_EQ(o.symbol_id, kEurUsd);
        // After G1: first order per side is POST_ONLY; subsequent are
        // REPLACE referencing the prior coid. CANCEL only appears on kills.
        MMX_EXPECT(o.ord_type == static_cast<uint8_t>(connector::OrdType::POST_ONLY) ||
                   o.ord_type == static_cast<uint8_t>(connector::OrdType::REPLACE));
        MMX_EXPECT(o.price_ticks > 0);
        // MMX is account-agnostic — the engine stamps account_id later.
        MMX_EXPECT_EQ(o.account_id, connector::ACCOUNT_INVALID);
        if (o.side == static_cast<uint8_t>(connector::Side::BUY))  ++n_buy;
        if (o.side == static_cast<uint8_t>(connector::Side::SELL)) ++n_sell;
    }
    MMX_EXPECT(n_buy  > 0);
    MMX_EXPECT(n_sell > 0);

    // ---- Inventory bookkeeping ------------------------------------------
    {
        connector::ExecReport r{};
        r.ts_ns          = ts;
        r.strategy_id    = static_cast<uint32_t>(strat.id());
        r.symbol_id      = kEurUsd;
        r.fill_price_ticks = static_cast<int64_t>(mid * PRICE_TICK_PER_UNIT);
        r.fill_quantity  = +200;  // bought 200 contracts → +2 lots
        r.status         = static_cast<uint8_t>(connector::ExecStatus::FILLED);
        strat.on_fill(r);
        MMX_EXPECT_EQ(strat.inventory_lots(), +2);

        r.fill_quantity  = -500;
        strat.on_fill(r);
        MMX_EXPECT_EQ(strat.inventory_lots(), -3);
    }

    // ---- Kill-switch silences the strategy ------------------------------
    // Kill itself may emit CANCELs for outstanding live orders (G1); the
    // ticks AFTER the kill must not produce any new ENTER / REPLACE.
    sink.orders.clear();
    strat.on_kill_switch(connector::RejectReason::KILL_SWITCH);
    const std::size_t orders_after_kill = sink.orders.size();
    for (const auto& o : sink.orders) {
        MMX_EXPECT_EQ(o.ord_type,
                      static_cast<uint8_t>(connector::OrdType::CANCEL));
    }

    Tick tk{};
    tk.ts_ns     = (ts += 100'000ULL);
    tk.symbol_id = kEurUsd;
    tk.bid_ticks = static_cast<int64_t>((mid - 5e-6) * PRICE_TICK_PER_UNIT);
    tk.ask_ticks = static_cast<int64_t>((mid + 5e-6) * PRICE_TICK_PER_UNIT);
    for (int i = 0; i < 50; ++i) {
        tk.ts_ns = (ts += 100'000ULL);
        strat.on_tick(tk);
    }
    MMX_EXPECT_EQ(sink.orders.size(), orders_after_kill);

    // ---- on_resume re-enables ------------------------------------------
    strat.on_resume();
    for (int i = 0; i < 100; ++i) {
        tk.ts_ns = (ts += 100'000ULL);
        strat.on_tick(tk);
    }
    MMX_EXPECT(strat.active());

    // ---- Regime override: event_imminent ⇒ ParamBlock kill_switch -------
    sink.orders.clear();
    strat.set_regime("event_imminent");
    for (int i = 0; i < 50; ++i) {
        tk.ts_ns = (ts += 1'000'000ULL);
        strat.on_tick(tk);
    }
    MMX_EXPECT_EQ(strat.active_param_block().kill_switch, 1u);
    MMX_EXPECT(sink.orders.empty());

    // ===================================================================
    // Audit-pass coverage — gaps G2, G3, G4, G5, G7 (docs/MMX_GAPS.md)
    // ===================================================================

    // ---- G3: GEL kill propagates into the FPGA stub --------------------
    {
        mmx::MmxStrategy s2(cfg);
        CapturingSink sink2;
        s2.on_start(sink2);

        Tick warm{};
        warm.symbol_id = kEurUsd;
        warm.bid_ticks = static_cast<int64_t>((mid - 5e-6) * PRICE_TICK_PER_UNIT);
        warm.ask_ticks = static_cast<int64_t>((mid + 5e-6) * PRICE_TICK_PER_UNIT);

        std::uint64_t ts2 = 1;
        for (int i = 0; i < 100; ++i) {
            warm.ts_ns = (ts2 += 100'000ULL);
            s2.on_tick(warm);
        }
        MMX_EXPECT_EQ(s2.active_param_block().kill_switch, 0u);

        s2.on_kill_switch(connector::RejectReason::KILL_SWITCH);
        MMX_EXPECT(!s2.active());
        MMX_EXPECT_EQ(s2.active_param_block().kill_switch, 1u);

        s2.on_resume();
        MMX_EXPECT(s2.active());
        for (int i = 0; i < 100; ++i) {
            warm.ts_ns = (ts2 += 100'000ULL);
            s2.on_tick(warm);
        }
        MMX_EXPECT_EQ(s2.active_param_block().kill_switch, 0u);
    }

    // ---- G4: wrong-symbol ticks are rejected ---------------------------
    {
        mmx::ConnectorAdapterConfig scoped = cfg;
        scoped.symbol_id = kEurUsd;
        mmx::MmxStrategy s3(scoped);
        CapturingSink sink3;
        s3.on_start(sink3);

        Tick wrong{};
        wrong.symbol_id = 99;  // not kEurUsd
        wrong.bid_ticks = static_cast<int64_t>((mid - 5e-6) * PRICE_TICK_PER_UNIT);
        wrong.ask_ticks = static_cast<int64_t>((mid + 5e-6) * PRICE_TICK_PER_UNIT);

        std::uint64_t ts3 = 1;
        for (int i = 0; i < 200; ++i) {
            wrong.ts_ns = (ts3 += 100'000ULL);
            s3.on_tick(wrong);
        }
        MMX_EXPECT(sink3.orders.empty());

        Tick right = wrong;
        right.symbol_id = kEurUsd;
        for (int i = 0; i < 200; ++i) {
            right.ts_ns = (ts3 += 100'000ULL);
            s3.on_tick(right);
        }
        MMX_EXPECT(!sink3.orders.empty());
    }

    // ---- G5: HIGH-impact CalendarEvent flips regime ⇒ kill block --------
    {
        mmx::MmxStrategy s4(cfg);
        CapturingSink sink4;
        s4.on_start(sink4);

        Tick t{};
        t.symbol_id = kEurUsd;
        t.bid_ticks = static_cast<int64_t>((mid - 5e-6) * PRICE_TICK_PER_UNIT);
        t.ask_ticks = static_cast<int64_t>((mid + 5e-6) * PRICE_TICK_PER_UNIT);

        std::uint64_t ts4 = 1;
        for (int i = 0; i < 50; ++i) {
            t.ts_ns = (ts4 += 100'000ULL);
            s4.on_tick(t);
        }
        MMX_EXPECT_EQ(s4.active_param_block().kill_switch, 0u);

        connector::CalendarEvent e{};
        e.event_ts_ns  = ts4;
        e.ingest_ts_ns = ts4;
        e.symbol_id    = 0;  // market-wide
        e.kind         = static_cast<uint8_t>(connector::EventKind::FOMC);
        e.impact       = static_cast<uint8_t>(connector::EventImpact::HIGH);
        s4.on_event(e);

        sink4.orders.clear();
        for (int i = 0; i < 50; ++i) {
            t.ts_ns = (ts4 += 1'000'000ULL);  // step past refresh window
            s4.on_tick(t);
        }
        MMX_EXPECT_EQ(s4.active_param_block().kill_switch, 1u);
        MMX_EXPECT(sink4.orders.empty());
    }

    // ---- G7: backpressure counter increments on a saturated sink --------
    {
        class FullSink final : public connector::OrderSink {
          public:
            bool send(const connector::Order&) noexcept override {
                ++rejected;
                return false;
            }
            std::uint64_t rejected = 0;
        };

        mmx::ConnectorAdapterConfig scoped = cfg;
        scoped.dropped_order_breaker_threshold = 0;  // disable auto-kill
        mmx::MmxStrategy s5(scoped);
        FullSink sink5;
        s5.on_start(sink5);

        Tick t{};
        t.symbol_id = kEurUsd;
        std::uint64_t ts5 = 1;
        double mid5 = mid;

        for (int i = 0; i < 200; ++i) {
            t.ts_ns = (ts5 += 100'000ULL);
            mid5 += 1e-5;
            t.bid_ticks = static_cast<int64_t>((mid5 - 5e-6) * PRICE_TICK_PER_UNIT);
            t.ask_ticks = static_cast<int64_t>((mid5 + 5e-6) * PRICE_TICK_PER_UNIT);
            s5.on_tick(t);
        }
        MMX_EXPECT(s5.dropped_orders() > 0);
        MMX_EXPECT_EQ(s5.dropped_orders(), sink5.rejected);
    }

    // ---- G2: sub-lot precision on partial fills ------------------------
    {
        mmx::MmxStrategy s6(cfg);
        CapturingSink sink6;
        s6.on_start(sink6);

        connector::ExecReport r{};
        r.symbol_id        = kEurUsd;
        r.fill_price_ticks = static_cast<int64_t>(mid * PRICE_TICK_PER_UNIT);
        r.status = static_cast<uint8_t>(connector::ExecStatus::PARTIAL);

        r.fill_quantity = 30; s6.on_fill(r);
        r.fill_quantity = 30; s6.on_fill(r);
        r.fill_quantity = 30; s6.on_fill(r);
        MMX_EXPECT_EQ(s6.inventory_contracts(), int64_t{90});
        MMX_EXPECT_EQ(s6.inventory_lots(),      0);

        r.fill_quantity = 30; s6.on_fill(r);
        MMX_EXPECT_EQ(s6.inventory_contracts(), int64_t{120});
        MMX_EXPECT_EQ(s6.inventory_lots(),      1);
    }

    // ---- G1: cancel/replace flow ----------------------------------------
    {
        mmx::ConnectorAdapterConfig scoped = cfg;
        scoped.symbol_id = kEurUsd;
        mmx::MmxStrategy s7(scoped);
        CapturingSink sink7;
        s7.on_start(sink7);

        // Drive a moving feed so each tick triggers a fresh quote.
        Tick t{};
        t.symbol_id = kEurUsd;
        std::uint64_t ts7  = 1;
        double mid7        = mid;

        for (int i = 0; i < 200; ++i) {
            t.ts_ns = (ts7 += 100'000ULL);
            mid7 += 1e-5;  // 1 pip per tick — guaranteed requote
            t.bid_ticks = static_cast<int64_t>((mid7 - 5e-6) * PRICE_TICK_PER_UNIT);
            t.ask_ticks = static_cast<int64_t>((mid7 + 5e-6) * PRICE_TICK_PER_UNIT);
            s7.on_tick(t);
        }

        // The first quote on each side must be POST_ONLY; subsequent ones
        // must be REPLACE referencing the prior coid on that side.
        bool saw_first_buy_post = false, saw_first_sell_post = false;
        int  replace_buys = 0, replace_sells = 0;
        std::uint64_t prev_buy_coid = 0, prev_sell_coid = 0;

        for (const auto& o : sink7.orders) {
            const auto op = static_cast<connector::OrdType>(o.ord_type);
            if (o.side == static_cast<uint8_t>(connector::Side::BUY)) {
                if (!saw_first_buy_post) {
                    MMX_EXPECT_EQ(o.ord_type,
                        static_cast<uint8_t>(connector::OrdType::POST_ONLY));
                    MMX_EXPECT_EQ(o.ref_order_id, std::uint64_t{0});
                    saw_first_buy_post = true;
                } else {
                    MMX_EXPECT_EQ(o.ord_type,
                        static_cast<uint8_t>(connector::OrdType::REPLACE));
                    MMX_EXPECT_EQ(o.ref_order_id, prev_buy_coid);
                    ++replace_buys;
                }
                prev_buy_coid = o.client_order_id;
                (void)op;
            } else {
                if (!saw_first_sell_post) {
                    MMX_EXPECT_EQ(o.ord_type,
                        static_cast<uint8_t>(connector::OrdType::POST_ONLY));
                    MMX_EXPECT_EQ(o.ref_order_id, std::uint64_t{0});
                    saw_first_sell_post = true;
                } else {
                    MMX_EXPECT_EQ(o.ord_type,
                        static_cast<uint8_t>(connector::OrdType::REPLACE));
                    MMX_EXPECT_EQ(o.ref_order_id, prev_sell_coid);
                    ++replace_sells;
                }
                prev_sell_coid = o.client_order_id;
            }
        }
        MMX_EXPECT(saw_first_buy_post);
        MMX_EXPECT(saw_first_sell_post);
        MMX_EXPECT(replace_buys  > 0);
        MMX_EXPECT(replace_sells > 0);
        MMX_EXPECT_EQ(s7.outstanding_bid_coid(), prev_buy_coid);
        MMX_EXPECT_EQ(s7.outstanding_ask_coid(), prev_sell_coid);

        // Terminal fill on the bid retires its coid; the next requote
        // emits a fresh POST_ONLY (not a REPLACE referencing a dead coid).
        connector::ExecReport done{};
        done.symbol_id        = kEurUsd;
        done.client_order_id  = s7.outstanding_bid_coid();
        done.side             = static_cast<uint8_t>(connector::Side::BUY);
        done.fill_price_ticks = static_cast<int64_t>(mid7 * PRICE_TICK_PER_UNIT);
        done.fill_quantity    = scoped.lot_to_qty;  // full lot
        done.status           = static_cast<uint8_t>(connector::ExecStatus::FILLED);
        s7.on_fill(done);
        MMX_EXPECT_EQ(s7.outstanding_bid_coid(), std::uint64_t{0});

        std::size_t before = sink7.orders.size();
        for (int i = 0; i < 5; ++i) {
            t.ts_ns = (ts7 += 100'000ULL);
            mid7 += 1e-5;
            t.bid_ticks = static_cast<int64_t>((mid7 - 5e-6) * PRICE_TICK_PER_UNIT);
            t.ask_ticks = static_cast<int64_t>((mid7 + 5e-6) * PRICE_TICK_PER_UNIT);
            s7.on_tick(t);
        }
        // Find the next BUY-side emit after `before` and confirm it's a
        // fresh POST_ONLY.
        bool checked = false;
        for (std::size_t i = before; i < sink7.orders.size(); ++i) {
            const auto& o = sink7.orders[i];
            if (o.side == static_cast<uint8_t>(connector::Side::BUY)) {
                MMX_EXPECT_EQ(o.ord_type,
                    static_cast<uint8_t>(connector::OrdType::POST_ONLY));
                MMX_EXPECT_EQ(o.ref_order_id, std::uint64_t{0});
                checked = true;
                break;
            }
        }
        MMX_EXPECT(checked);

        // on_kill_switch must emit explicit CANCELs for any live side.
        const std::uint64_t live_ask = s7.outstanding_ask_coid();
        MMX_EXPECT(live_ask != 0);
        before = sink7.orders.size();
        s7.on_kill_switch(connector::RejectReason::KILL_SWITCH);
        bool saw_cancel_ask = false;
        for (std::size_t i = before; i < sink7.orders.size(); ++i) {
            const auto& o = sink7.orders[i];
            if (o.side == static_cast<uint8_t>(connector::Side::SELL) &&
                o.ord_type == static_cast<uint8_t>(connector::OrdType::CANCEL)) {
                MMX_EXPECT_EQ(o.ref_order_id, live_ask);
                saw_cancel_ask = true;
            }
        }
        MMX_EXPECT(saw_cancel_ask);
        MMX_EXPECT_EQ(s7.outstanding_ask_coid(), std::uint64_t{0});
    }

    // ---- G10: client_order_id is namespaced by start-time --------------
    {
        mmx::ConnectorAdapterConfig scoped = cfg;
        scoped.symbol_id = kEurUsd;
        mmx::MmxStrategy s10(scoped);
        CapturingSink sink10;
        s10.on_start(sink10);

        Tick t{};
        t.symbol_id = kEurUsd;
        t.ts_ns     = 1;
        t.bid_ticks = static_cast<int64_t>((mid - 5e-6) * PRICE_TICK_PER_UNIT);
        t.ask_ticks = static_cast<int64_t>((mid + 5e-6) * PRICE_TICK_PER_UNIT);
        for (int i = 0; i < 10; ++i) {
            t.ts_ns = static_cast<std::uint64_t>(i + 1) * 100'000ULL + 1;
            s10.on_tick(t);
        }
        MMX_EXPECT(!sink10.orders.empty());

        const std::uint64_t prefix = sink10.orders.front().client_order_id >> 32;
        const auto now_secs = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        // Prefix should be wall-clock seconds at construction; allow a 5-min
        // window for test-host clock skew.
        MMX_EXPECT(prefix >  1'000'000'000ULL);            // post-2001 epoch
        MMX_EXPECT(static_cast<long long>(prefix) <= now_secs);
        MMX_EXPECT(static_cast<long long>(prefix) >= now_secs - 300);

        // Counter monotonic and within a single session prefix.
        std::uint64_t prev_low = 0;
        for (const auto& o : sink10.orders) {
            MMX_EXPECT_EQ(o.client_order_id >> 32, prefix);
            const std::uint64_t low = o.client_order_id & 0xffffffffULL;
            MMX_EXPECT(low > prev_low);
            prev_low = low;
        }
    }

    // ---- G9: telemetry stamps on accepted orders ------------------------
    {
        struct CapturingTelemetry final : public mmx::ITelemetrySink {
            void push(const connector::TelemetryRecord& r) noexcept override {
                records.push_back(r);
            }
            std::vector<connector::TelemetryRecord> records;
        };

        mmx::ConnectorAdapterConfig scoped = cfg;
        scoped.symbol_id = kEurUsd;
        CapturingTelemetry tel;
        scoped.telemetry = &tel;

        mmx::MmxStrategy s9(scoped);
        CapturingSink sink9;
        s9.on_start(sink9);

        Tick t{};
        t.symbol_id = kEurUsd;
        std::uint64_t ts9 = 1;
        double mid9       = mid;
        for (int i = 0; i < 200; ++i) {
            t.ts_ns = (ts9 += 100'000ULL);
            mid9 += 1e-5;
            t.bid_ticks = static_cast<int64_t>((mid9 - 5e-6) * PRICE_TICK_PER_UNIT);
            t.ask_ticks = static_cast<int64_t>((mid9 + 5e-6) * PRICE_TICK_PER_UNIT);
            s9.on_tick(t);
        }

        MMX_EXPECT(!tel.records.empty());
        // Every accepted Order produces exactly one record.
        MMX_EXPECT_EQ(tel.records.size(), sink9.orders.size());

        for (std::size_t i = 0; i < tel.records.size(); ++i) {
            const auto& r = tel.records[i];
            const auto& o = sink9.orders[i];
            MMX_EXPECT_EQ(r.hop,
                static_cast<uint8_t>(connector::Hop::STRATEGY_EMIT));
            MMX_EXPECT_EQ(r.strategy_id,
                static_cast<uint32_t>(connector::StrategyId::MARKET_MAKER));
            MMX_EXPECT_EQ(r.client_order_id, o.client_order_id);
            MMX_EXPECT_EQ(r.ts_ns,           o.ts_ns);
        }
    }

    // ---- G11: HMAC key provisioning from a file -------------------------
    {
        // Build a unique temp path so concurrent runs don't collide.
        char tmpl[] = "/tmp/mmx_hmac_XXXXXX";
        int fd = ::mkstemp(tmpl);
        MMX_EXPECT(fd >= 0);
        ::close(fd);
        const std::string path = tmpl;

        // Wrong size first.
        {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            for (int i = 0; i < 16; ++i) f.put(static_cast<char>(i));
        }
        ::chmod(path.c_str(), 0600);
        bool threw = false;
        try { (void)mmx::load_hmac_key(path); } catch (const std::exception&) { threw = true; }
        MMX_EXPECT(threw);

        // World-readable file rejected.
        {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            for (int i = 0; i < 32; ++i) f.put(static_cast<char>(0xA0 + i));
        }
        ::chmod(path.c_str(), 0644);
        threw = false;
        try { (void)mmx::load_hmac_key(path); } catch (const std::exception&) { threw = true; }
        MMX_EXPECT(threw);

        // Correct size + 0600 succeeds and round-trips.
        ::chmod(path.c_str(), 0600);
        auto key = mmx::load_hmac_key(path);
        MMX_EXPECT_EQ(key.size(), std::size_t{32});
        for (std::size_t i = 0; i < 32; ++i) {
            MMX_EXPECT_EQ(key[i], static_cast<std::uint8_t>(0xA0 + i));
        }
        ::unlink(path.c_str());
    }

    // ---- G12: restart reconciliation via on_open_orders -----------------
    {
        mmx::ConnectorAdapterConfig scoped = cfg;
        scoped.symbol_id = kEurUsd;
        mmx::MmxStrategy s12(scoped);
        CapturingSink sink12;
        s12.on_start(sink12);

        // Engine replays the live order book: one bid + one ask.
        connector::Order live[2]{};
        live[0].ts_ns           = 1;
        live[0].client_order_id = 0xFEEDFACEDEADBEEFULL;
        live[0].strategy_id     =
            static_cast<uint32_t>(connector::StrategyId::MARKET_MAKER);
        live[0].symbol_id       = kEurUsd;
        live[0].price_ticks     =
            static_cast<int64_t>((mid - 5e-6) * PRICE_TICK_PER_UNIT);
        live[0].quantity        = 100;  // 100 contracts buy
        live[0].side            = static_cast<uint8_t>(connector::Side::BUY);
        live[0].ord_type        = static_cast<uint8_t>(connector::OrdType::POST_ONLY);

        live[1].ts_ns           = 1;
        live[1].client_order_id = 0xFEEDFACEDEADBEF0ULL;
        live[1].strategy_id     = live[0].strategy_id;
        live[1].symbol_id       = kEurUsd;
        live[1].price_ticks     =
            static_cast<int64_t>((mid + 5e-6) * PRICE_TICK_PER_UNIT);
        live[1].quantity        = -100;
        live[1].side            = static_cast<uint8_t>(connector::Side::SELL);
        live[1].ord_type        = static_cast<uint8_t>(connector::OrdType::POST_ONLY);

        s12.on_open_orders(live, 2);

        MMX_EXPECT_EQ(s12.outstanding_bid_coid(), live[0].client_order_id);
        MMX_EXPECT_EQ(s12.outstanding_ask_coid(), live[1].client_order_id);

        // First requote on each side must now be REPLACE referencing the
        // replayed coid — no orphan POST_ONLY that would duplicate the book.
        Tick t{};
        t.symbol_id = kEurUsd;
        std::uint64_t ts12 = 2;
        double mid12 = mid + 1e-4;  // move enough to trip requote_ticks
        for (int i = 0; i < 50; ++i) {
            t.ts_ns = (ts12 += 100'000ULL);
            mid12 += 1e-5;
            t.bid_ticks = static_cast<int64_t>((mid12 - 5e-6) * PRICE_TICK_PER_UNIT);
            t.ask_ticks = static_cast<int64_t>((mid12 + 5e-6) * PRICE_TICK_PER_UNIT);
            s12.on_tick(t);
        }
        bool first_buy_seen = false, first_sell_seen = false;
        for (const auto& o : sink12.orders) {
            if (o.ord_type == static_cast<uint8_t>(connector::OrdType::CANCEL))
                continue;
            if (o.side == static_cast<uint8_t>(connector::Side::BUY) && !first_buy_seen) {
                MMX_EXPECT_EQ(o.ord_type,
                    static_cast<uint8_t>(connector::OrdType::REPLACE));
                MMX_EXPECT_EQ(o.ref_order_id, live[0].client_order_id);
                first_buy_seen = true;
            }
            if (o.side == static_cast<uint8_t>(connector::Side::SELL) && !first_sell_seen) {
                MMX_EXPECT_EQ(o.ord_type,
                    static_cast<uint8_t>(connector::OrdType::REPLACE));
                MMX_EXPECT_EQ(o.ref_order_id, live[1].client_order_id);
                first_sell_seen = true;
            }
        }
        MMX_EXPECT(first_buy_seen);
        MMX_EXPECT(first_sell_seen);
    }

    // ---- G8: queue-position estimate from L2 ----------------------------
    {
        mmx::ConnectorAdapterConfig scoped = cfg;
        scoped.symbol_id = kEurUsd;
        mmx::MmxStrategy s8(scoped);
        CapturingSink sink8;
        s8.on_start(sink8);

        // Drive enough ticks so the strategy places quotes on both sides.
        Tick t{};
        t.symbol_id = kEurUsd;
        std::uint64_t ts8  = 1;
        double mid8        = 1.07500;
        for (int i = 0; i < 200; ++i) {
            t.ts_ns = (ts8 += 100'000ULL);
            mid8 += 1e-5;
            t.bid_ticks = static_cast<int64_t>((mid8 - 5e-6) * PRICE_TICK_PER_UNIT);
            t.ask_ticks = static_cast<int64_t>((mid8 + 5e-6) * PRICE_TICK_PER_UNIT);
            s8.on_tick(t);
        }
        MMX_EXPECT(s8.outstanding_bid_coid() != 0);
        MMX_EXPECT(s8.outstanding_ask_coid() != 0);

        // Before any matching on_book, queue_ahead is unknown ⇒ -1.
        MMX_EXPECT_EQ(s8.queue_ahead_bid(), int64_t{-1});
        MMX_EXPECT_EQ(s8.queue_ahead_ask(), int64_t{-1});

        // Use the prices from the most recent buy + sell orders MMX emitted.
        std::int64_t our_bid_px = 0, our_ask_px = 0;
        std::int64_t our_bid_qty = 0, our_ask_qty = 0;
        for (auto it = sink8.orders.rbegin(); it != sink8.orders.rend(); ++it) {
            const auto& o = *it;
            if (o.ord_type == static_cast<uint8_t>(connector::OrdType::CANCEL))
                continue;
            if (our_bid_px == 0 &&
                o.side == static_cast<uint8_t>(connector::Side::BUY)) {
                our_bid_px  = o.price_ticks;
                our_bid_qty = o.quantity;
            }
            if (our_ask_px == 0 &&
                o.side == static_cast<uint8_t>(connector::Side::SELL)) {
                our_ask_px  = o.price_ticks;
                our_ask_qty = -o.quantity;
            }
            if (our_bid_px && our_ask_px) break;
        }
        MMX_EXPECT(our_bid_px != 0 && our_ask_px != 0);

        // Synthetic L2 with extra depth at our levels.
        connector::BookUpdate bu{};
        bu.ts_ns     = ts8 + 1'000'000ULL;
        bu.symbol_id = kEurUsd;
        bu.seq       = 1;  // non-zero so on_book accepts it
        bu.bids[0].price_ticks = our_bid_px;
        bu.bids[0].quantity    = our_bid_qty + 400;   // 400 contracts ahead
        bu.asks[0].price_ticks = our_ask_px;
        bu.asks[0].quantity    = our_ask_qty + 250;   // 250 contracts ahead
        s8.on_book(bu);
        MMX_EXPECT_EQ(s8.queue_ahead_bid(), int64_t{400});
        MMX_EXPECT_EQ(s8.queue_ahead_ask(), int64_t{250});

        // Cancels-ahead shrink the queue.
        bu.seq                 = 2;
        bu.bids[0].quantity    = our_bid_qty + 150;   // 250 cancels-ahead
        s8.on_book(bu);
        MMX_EXPECT_EQ(s8.queue_ahead_bid(), int64_t{150});

        // Total volume below our resting size ⇒ queue_ahead clamps to 0.
        bu.seq                 = 3;
        bu.bids[0].quantity    = our_bid_qty / 2;
        s8.on_book(bu);
        MMX_EXPECT_EQ(s8.queue_ahead_bid(), int64_t{0});
    }

    // ---- G14: max_quote_age forces a re-emit even on a stable price -----
    {
        mmx::ConnectorAdapterConfig scoped = cfg;
        scoped.symbol_id        = kEurUsd;
        scoped.max_quote_age_ns = 5'000'000ULL;     // 5 ms — short for the test
        scoped.requote_ticks    = 100;              // huge — pure age trigger
        mmx::MmxStrategy s14(scoped);
        CapturingSink sink14;
        s14.on_start(sink14);

        // Identical tick repeated; without G14 only the first would emit.
        Tick t{};
        t.symbol_id = kEurUsd;
        t.bid_ticks = static_cast<int64_t>((mid - 5e-6) * PRICE_TICK_PER_UNIT);
        t.ask_ticks = static_cast<int64_t>((mid + 5e-6) * PRICE_TICK_PER_UNIT);

        std::uint64_t ts14 = 1;
        // First batch: ticks within the age window. Only initial pair fires.
        for (int i = 0; i < 5; ++i) {
            t.ts_ns = (ts14 += 100'000ULL);  // +0.1 ms
            s14.on_tick(t);
        }
        const std::size_t after_burst = sink14.orders.size();
        MMX_EXPECT(after_burst >= 2);  // initial bid + ask

        // Now jump well past max_quote_age_ns and feed the same tick again.
        // Age force should re-emit on both sides.
        ts14 += 10'000'000ULL;  // +10 ms — above 5 ms threshold
        t.ts_ns = ts14;
        s14.on_tick(t);
        MMX_EXPECT(sink14.orders.size() > after_burst);
    }

    // ---- G13: toxic fills derate effective base_qty ---------------------
    {
        mmx::ConnectorAdapterConfig scoped = cfg;
        scoped.symbol_id             = kEurUsd;
        scoped.base_qty_lots         = 4;
        scoped.toxic_window_ns       = 1'000'000ULL;    // 1 ms
        scoped.toxic_lookback_ns     = 1'000'000'000ULL;
        scoped.toxic_threshold_ticks = 1;
        scoped.toxic_share_threshold = 0.30;
        scoped.toxic_qty_reduction   = 0.5;
        mmx::MmxStrategy s13(scoped);
        CapturingSink sink13;
        s13.on_start(sink13);

        // Inject three buy fills at price 100 (in MMX ticks).
        const int64_t fill_price_conn = int64_t{100} * scoped.conn_ticks_per_mmx_tick;
        connector::ExecReport r{};
        r.symbol_id        = kEurUsd;
        r.fill_price_ticks = fill_price_conn;
        r.side             = static_cast<uint8_t>(connector::Side::BUY);
        r.status           = static_cast<uint8_t>(connector::ExecStatus::FILLED);
        for (int i = 0; i < 3; ++i) {
            r.ts_ns        = static_cast<std::uint64_t>(i + 1) * 100'000ULL;
            r.fill_quantity= scoped.lot_to_qty;
            s13.on_fill(r);
        }
        MMX_EXPECT_EQ(s13.toxic_share(), 0.0);
        MMX_EXPECT_EQ(s13.qty_effective(), uint32_t{4});

        // Drive ticks well past toxic_window_ns with the mid 5 ticks BELOW
        // the fill price — adverse for buys → toxic.
        Tick t{};
        t.symbol_id = kEurUsd;
        const int32_t adverse_mid_mmx = 95;
        t.bid_ticks = (int64_t{adverse_mid_mmx} - 1) * scoped.conn_ticks_per_mmx_tick;
        t.ask_ticks = (int64_t{adverse_mid_mmx} + 1) * scoped.conn_ticks_per_mmx_tick;
        // First tick processes the pending fills.
        t.ts_ns = 10'000'000ULL;
        s13.on_tick(t);

        MMX_EXPECT(s13.toxic_share() > 0.5);
        MMX_EXPECT(s13.qty_effective() < uint32_t{4});
        MMX_EXPECT_EQ(s13.qty_effective(), uint32_t{2});  // 4 * 0.5 = 2
    }

    MMX_OK();
}
