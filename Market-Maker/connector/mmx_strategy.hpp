// MMX — connector/mmx_strategy.hpp
//
// **The Lego connector** between MMX and GEL via the neutral `connector` SDK.
//
// MMX is a self-contained market-maker module. GEL is the host platform that
// owns market data ingest, risk, routing, drop-copy, and FPGA NIC. The
// `connector` SDK is the neutral contract that lets either side plug in
// without knowing about the other.
//
// Lifecycle (matches connector::IStrategy):
//
//   on_start(sink)   — capture the OrderSink (only exit door for orders)
//   on_tick(t)       — every tick: σ² update, periodic param refresh, quote
//   on_book(b)       — optional L2 awareness for κ estimation
//   on_event(e)      — calendar fan-in (HIGH impact ⇒ event_imminent regime)
//   on_fill(r)       — inventory bookkeeping (in contracts, not lots)
//   on_kill_switch() — engine trips us; cancel + lock the FPGA stub
//   on_resume()      — clear-to-quote (resets the in-strategy breaker too)
//
// Every Order goes through `connector::OrderSink::send(...)` — MMX is
// structurally incapable of bypassing the engine's risk gate.
//
// Single-symbol per instance: see `cfg.symbol_id`. Register N strategy
// instances for N instruments — σ² / κ / ParamBlock are inherently per-symbol.
//
// `account_id` is left zero on Order; the engine stamps it via
// `connector::StrategyAccountMap::stamp_primary` (account_map.hpp). MMX is
// account-agnostic; it doesn't read `account_id` off ExecReports either.
//
// Build:
//   cmake -S . -B build -DMMX_BUILD_CONNECTOR_ADAPTER=ON
//                       [-DCONNECTOR_INCLUDE_DIR=/path/to/connector/include]
//   When MMX is added via add_subdirectory() from a parent (GEL) CMakeLists,
//   the parent should set CONNECTOR_INCLUDE_DIR before adding us.
//
// Known follow-ups (see docs/MMX_GAPS.md): cancel/replace OrdType, queue-
// position feedback, telemetry stamps, restart reconciliation, HMAC key
// provisioning, host-side MAX_QUOTE_AGE_NS.
//

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <string_view>
#include <utility>
#include <vector>

// connector — the neutral SDK boundary.
#include "connector/strategy.hpp"
#include "connector/wire.hpp"

#include "host/ipc/fpga_stub.h"
#include "host/ipc/hmac.h"
#include "host/ipc/param_block.h"
#include "host/math/ewma.h"
#include "host/math/kappa_fit.h"
#include "host/pricing/pricing_engine.h"
#include "host/risk/circuit_breaker.h"
#include "host/risk/pnl_tracker.h"
#include "host/threads/param_compiler.h"
#include "host/threads/pcta_ingest.h"

namespace mmx {

// G9 — opt-in telemetry sink. Engine wires up an implementation that drains
// records into Prometheus / a binary log; tests can inspect them directly.
// MMX stamps records at `connector::Hop::STRATEGY_EMIT` for every Order that
// the OrderSink accepts. Stays a separate interface from `connector::OrderSink`
// so a strategy can opt in/out without changing its registration call.
class ITelemetrySink {
  public:
    virtual ~ITelemetrySink() = default;
    virtual void push(const connector::TelemetryRecord& r) noexcept = 0;
};

struct ConnectorAdapterConfig {
    // Single-symbol gate. SYMBOL_INVALID (= 0) means "accept any" — useful
    // for tests, never set this in production.
    connector::SymbolId symbol_id = connector::SYMBOL_INVALID;

    // Tick-size translation. connector prices are int64 ticks at
    // PRICE_SCALE = 8 (1 unit = 10^8 connector ticks). MMX's pricing engine
    // works on int32 ticks at an instrument-defined granularity. For EUR/USD
    // with a 1-pip tick (10^-5 in the underlying) this is 1000 connector
    // ticks per MMX tick.
    int64_t  conn_ticks_per_mmx_tick = 1000;

    // 1 MMX lot ↔ this many connector contract units (signed-quantity scale).
    int64_t  lot_to_qty            = 100;

    // Avellaneda–Stoikov knobs.
    double   base_gamma            = 0.1;
    double   horizon_sec           = 600.0;

    // κ floor — protects against under-calibrated arrival rate (G6).
    // Without L2, KappaEstimator stays at 1e-3 and (1/γ)·ln(1+γ/κ) blows up.
    // 1.0 is sane for tier-1 FX ECN; calibrate per-instrument in production.
    double   kappa_floor           = 1.0;

    // ParamBlock constants.
    uint32_t max_inv_lots          = 100;
    uint32_t base_qty_lots         = 1;
    uint16_t requote_ticks         = 1;

    // Slow-path cadence: recompile ParamBlock and DMA refresh.
    std::uint64_t refresh_period_ns = 100'000'000ULL;

    // After a HIGH-impact CalendarEvent, hold `event_imminent` regime for
    // this many ns before reverting (G5).
    std::uint64_t event_imminent_window_ns = 60'000'000'000ULL;  // 60 s

    // If we drop this many sink sends inside one refresh window, trip the
    // internal breaker (G7). 0 disables the auto-kill behaviour.
    uint32_t dropped_order_breaker_threshold = 1024;

    // G13 — toxicity feedback. After a fill, wait `toxic_window_ns` and
    // measure the mid move. If the mid moved by > `toxic_threshold_ticks`
    // *against* our position (down for a buy fill, up for a sell), tag the
    // fill toxic. When the rolling share over `toxic_lookback_ns` exceeds
    // `toxic_share_threshold`, scale `base_qty` down by `toxic_qty_reduction`
    // to derisk further inventory accumulation.
    std::uint64_t toxic_window_ns        = 5'000'000'000ULL;   // 5 s
    std::uint64_t toxic_lookback_ns      = 60'000'000'000ULL;  // 60 s
    int32_t       toxic_threshold_ticks  = 1;
    double        toxic_share_threshold  = 0.30;               // 30 %
    double        toxic_qty_reduction    = 0.50;               // halve qty

    // G14 — force replace once an outstanding side ages past this. Spec §3.5
    // calls for 50 ms on the FPGA; the host mirrors it so the simulator and
    // dry-run paths can't sit on a stale quote forever.
    std::uint64_t max_quote_age_ns = 50'000'000ULL;  // 50 ms

    // 32-byte HMAC key. Provisioned by the operator; matches the burned-in
    // FPGA key. If empty, a deterministic dev key is used (insecure — only
    // for unit tests).
    std::vector<std::uint8_t> hmac_key;

    // G9 — optional telemetry sink. Set to non-null to receive a
    // STRATEGY_EMIT TelemetryRecord per accepted Order.
    ITelemetrySink* telemetry = nullptr;

    // Risk supervisor limits (passed straight to mmx::CircuitBreaker).
    BreakerLimits breaker = default_limits();
};

class MmxStrategy final : public connector::IStrategy {
  public:
    explicit MmxStrategy(ConnectorAdapterConfig cfg = {})
        : cfg_(normalise(std::move(cfg))),
          ewma_(0.94),
          compiler_(cfg_.hmac_key),
          fpga_(cfg_.hmac_key.data(), cfg_.hmac_key.size()),
          breaker_(cfg_.breaker) {
        // G10 — namespace client_order_ids by start-time so a hot restart
        // can't collide with engine-tracked outstanding orders from a
        // previous session. Layout: [secs_since_epoch : 32][counter : 32].
        next_coid_.store(initial_coid(), std::memory_order_relaxed);
    }

    // ---- IStrategy identity ------------------------------------------------
    connector::StrategyId id() const noexcept override {
        return connector::StrategyId::MARKET_MAKER;
    }
    std::string name() const override { return "MMX"; }

    // ---- IStrategy lifecycle ----------------------------------------------
    void on_start(connector::OrderSink& sink) override {
        sink_ = &sink;
        active_.store(true, std::memory_order_release);
    }
    void on_stop() override {
        sink_ = nullptr;
        active_.store(false, std::memory_order_release);
    }

    // ---- Hot path ----------------------------------------------------------
    void on_tick(const connector::Tick& t) override {
        if (sink_ == nullptr) return;
        if (!active_.load(std::memory_order_acquire)) return;
        if (!matches_symbol(t.symbol_id)) return;

        const std::uint64_t now_ns =
            t.ts_ns ? t.ts_ns : monotonic_now_ns();

        // Roll the event_imminent override back to ranging once the window
        // has passed (G5).
        if (event_clear_at_ns_ != 0 && now_ns >= event_clear_at_ns_) {
            regime_.set(kRegimeRanging);
            event_clear_at_ns_ = 0;
        }

        const int32_t mid_mmx = mid_in_mmx_ticks(t);
        ewma_.update(static_cast<double>(mid_mmx));

        // G13 — evaluate any pending fills whose toxicity window has elapsed.
        evaluate_pending_toxicity(now_ns, mid_mmx);

        maybe_refresh(now_ns, mid_mmx);

        // Risk supervisor watchdog (kill on staleness / loss / inventory).
        ParamBlock kill{};
        if (sup_tick(now_ns, mid_mmx, last_refresh_ns_, &kill)) {
            fpga_.load(kill, now_ns);
        }

        emit_quotes(t, now_ns, mid_mmx);
    }

    void on_book(const connector::BookUpdate& b) override {
        if (b.seq == 0) return;
        if (!matches_symbol(b.symbol_id)) return;

        // Feed L2 traffic into κ. Each refreshed level acts as an arrival
        // signal at distance `|level.price − mid|` from mid.
        const int64_t mid_conn =
            (b.bids[0].price_ticks + b.asks[0].price_ticks) / 2;
        const std::uint64_t ts_ns =
            b.ts_ns ? b.ts_ns : monotonic_now_ns();
        for (const auto& lvl : b.bids) {
            if (lvl.price_ticks == 0) break;
            int dist = static_cast<int>(
                (mid_conn - lvl.price_ticks) / cfg_.conn_ticks_per_mmx_tick);
            if (dist < 0) dist = -dist;
            kappa_.record_arrival(dist, ts_ns);
        }
        for (const auto& lvl : b.asks) {
            if (lvl.price_ticks == 0) break;
            int dist = static_cast<int>(
                (lvl.price_ticks - mid_conn) / cfg_.conn_ticks_per_mmx_tick);
            if (dist < 0) dist = -dist;
            kappa_.record_arrival(dist, ts_ns);
        }

        // G8 — queue-position estimate from L2 levels at our outstanding
        // prices. We assume the engine reports total visible volume at each
        // level (including our own resting qty), so:
        //     queue_ahead = max(0, level_qty - our_qty_at_level)
        // If our price isn't in the top-N snapshot, queue_ahead stays as
        // last known until the next matching update.
        update_queue_position(b);
    }

    void on_event(const connector::CalendarEvent& e) override {
        // Symbol 0 = market-wide event, applies to every instance.
        if (e.symbol_id != 0 && !matches_symbol(e.symbol_id)) return;

        // HIGH-impact ⇒ deterministic regime flip on the fast path. PCTA's
        // LLM-driven mailbox is still respected; this only ensures we don't
        // wait hundreds of ms for a regime hint we already see in the wire.
        if (e.impact == static_cast<uint8_t>(connector::EventImpact::HIGH)) {
            regime_.set(kRegimeEventImminent);
            const std::uint64_t now =
                e.ingest_ts_ns ? e.ingest_ts_ns : monotonic_now_ns();
            event_clear_at_ns_ = now + cfg_.event_imminent_window_ns;
        }
    }

    void on_fill(const connector::ExecReport& r) override {
        if (!matches_symbol(r.symbol_id)) return;

        // Inventory in *contracts* (G2) — partial fills below `lot_to_qty`
        // accumulate exactly. Pricing engine sees an int32 lot count derived
        // by integer division at use-time.
        // G2 — the FPGA saturates inventory at ±max_inv (in contracts); the
        // host must clamp identically or the two models diverge.
        const int64_t inv_cap =
            static_cast<int64_t>(cfg_.max_inv_lots) * cfg_.lot_to_qty;
        inventory_contracts_ = std::clamp(
            inventory_contracts_ + r.fill_quantity, -inv_cap, +inv_cap);

        const int32_t price_mmx = static_cast<int32_t>(
            r.fill_price_ticks / cfg_.conn_ticks_per_mmx_tick);
        const int64_t signed_qty = r.fill_quantity;

        Fill f{};
        f.ts_ns       = r.ts_ns;
        f.side        = static_cast<int8_t>(signed_qty >= 0 ? +1 : -1);
        f.price_ticks = price_mmx;
        f.qty         = static_cast<uint32_t>(
            signed_qty >= 0 ? signed_qty : -signed_qty);
        f.fee_ticks   = 0;
        pnl_.on_fill(f);

        // G13 — schedule a toxicity check at fill_ts + toxic_window_ns.
        pending_fills_.push_back(PendingFill{
            r.ts_ns, f.side, price_mmx});

        // G1 — retire the per-side outstanding coid on terminal exec status,
        // so the next requote uses ENTER (not REPLACE referencing a dead id).
        const auto status = static_cast<connector::ExecStatus>(r.status);
        const bool terminal =
            status == connector::ExecStatus::FILLED   ||
            status == connector::ExecStatus::CANCELED ||
            status == connector::ExecStatus::EXPIRED  ||
            status == connector::ExecStatus::REJECTED;
        if (terminal) {
            const auto side = static_cast<connector::Side>(r.side);
            if (side == connector::Side::BUY  && r.client_order_id == outstanding_bid_coid_)
                outstanding_bid_coid_ = 0;
            if (side == connector::Side::SELL && r.client_order_id == outstanding_ask_coid_)
                outstanding_ask_coid_ = 0;
        }
    }

    // G12 — engine replays the live order book for this strategy on hot
    // restart. We rehydrate `outstanding_*_coid_` and `LevelTrack` from the
    // open orders so the next requote emits a REPLACE (referencing the
    // existing coid) instead of a fresh POST_ONLY that creates a duplicate.
    void on_open_orders(const connector::Order* orders,
                        std::size_t count) override {
        for (std::size_t i = 0; i < count; ++i) {
            const auto& o = orders[i];
            if (!matches_symbol(o.symbol_id)) continue;
            const auto side = static_cast<connector::Side>(o.side);
            const int32_t px_mmx = static_cast<int32_t>(
                o.price_ticks / cfg_.conn_ticks_per_mmx_tick);
            const int64_t abs_qty = o.quantity >= 0 ? o.quantity : -o.quantity;

            LevelTrack& lvl = (side == connector::Side::BUY) ? bid_lvl_
                                                              : ask_lvl_;
            lvl.valid             = true;
            lvl.price_mmx         = px_mmx;
            lvl.our_qty           = abs_qty;
            lvl.queue_ahead       = 0;
            lvl.queue_ahead_valid = false;

            if (side == connector::Side::BUY)
                outstanding_bid_coid_ = o.client_order_id;
            else
                outstanding_ask_coid_ = o.client_order_id;

            // Bump our session's next_coid past any replayed coid in the
            // same session prefix so we never re-issue a known id.
            std::uint64_t cur = next_coid_.load(std::memory_order_relaxed);
            while (cur <= o.client_order_id) {
                if (next_coid_.compare_exchange_weak(
                        cur, o.client_order_id + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) break;
            }
        }
    }

    void on_kill_switch(connector::RejectReason r) override {
        active_.store(false, std::memory_order_release);
        last_kill_reason_ = r;
        // G3 — propagate the kill into the FPGA path so a future spurious
        // resume cannot re-quote against a stale ParamBlock.
        propagate_kill_to_fpga(monotonic_now_ns());
        // G1 — emit explicit CANCELs for any outstanding live quotes so the
        // engine doesn't keep our orders in book past a kill.
        emit_cancels_for_outstanding();
    }

    void on_resume() override {
        active_.store(true, std::memory_order_release);
        // G3 — clear the in-strategy breaker so the next maybe_refresh()
        // installs a clean (kill_switch=0) ParamBlock.
        breaker_.reset();
        kill_emitted_          = false;
        dropped_orders_window_ = 0;
    }

    // ---- PCTA fan-in (called from the host's PCTA ingest thread) -----------
    void set_regime(uint8_t regime_id) { regime_.set(regime_id); }
    void set_regime(std::string_view name) {
        regime_.set(parse_regime_name(name));
    }

    // ---- Introspection (for tests / dashboards) ---------------------------
    int64_t            inventory_contracts() const { return inventory_contracts_; }
    int32_t            inventory_lots()      const {
        return static_cast<int32_t>(inventory_contracts_ / cfg_.lot_to_qty);
    }
    const ParamBlock&  active_param_block() const { return fpga_.active(); }
    bool               active() const { return active_.load(std::memory_order_acquire); }
    std::uint64_t      dropped_orders() const { return dropped_orders_total_; }
    bool               breaker_tripped() const { return breaker_.tripped(); }

    // G1 — outstanding coid per side (0 = no live quote on that side).
    std::uint64_t      outstanding_bid_coid() const { return outstanding_bid_coid_; }
    std::uint64_t      outstanding_ask_coid() const { return outstanding_ask_coid_; }

    // G8 — queue-position estimate per side, in contracts ahead of us.
    // Returns -1 when unknown (no L2 update has matched our level yet, or
    // we have no outstanding quote on that side).
    int64_t            queue_ahead_bid() const {
        return (bid_lvl_.valid && bid_lvl_.queue_ahead_valid)
                   ? bid_lvl_.queue_ahead : int64_t{-1};
    }
    int64_t            queue_ahead_ask() const {
        return (ask_lvl_.valid && ask_lvl_.queue_ahead_valid)
                   ? ask_lvl_.queue_ahead : int64_t{-1};
    }

    // G13 — rolling toxic-fill share over `cfg.toxic_lookback_ns`.
    double             toxic_share()  const { return current_toxic_share(); }
    uint32_t           qty_effective() const { return effective_base_qty(); }

    // G14 — last successful emit timestamp per side (0 = never emitted).
    std::uint64_t      last_emit_bid_ns() const { return last_emit_bid_ns_; }
    std::uint64_t      last_emit_ask_ns() const { return last_emit_ask_ns_; }

  private:
    static ConnectorAdapterConfig normalise(ConnectorAdapterConfig cfg) {
        if (cfg.hmac_key.empty()) {
            // Deterministic dev key — DO NOT ship. Must match the FPGA
            // param_block_loader.vhd HMAC_KEY (0xAB*32) and host/main.cpp, or
            // the FPGA rejects every ParamBlock on silicon. Production: eFuse.
            cfg.hmac_key.assign(32, static_cast<std::uint8_t>(0xAB));
        }
        if (cfg.conn_ticks_per_mmx_tick <= 0) cfg.conn_ticks_per_mmx_tick = 1;
        if (cfg.lot_to_qty <= 0)              cfg.lot_to_qty              = 1;
        if (cfg.kappa_floor < 1e-6)           cfg.kappa_floor             = 1e-6;
        return cfg;
    }

    bool matches_symbol(connector::SymbolId sid) const {
        return cfg_.symbol_id == connector::SYMBOL_INVALID ||
               cfg_.symbol_id == sid;
    }

    static std::uint64_t monotonic_now_ns() {
        using namespace std::chrono;
        return static_cast<std::uint64_t>(
            duration_cast<nanoseconds>(
                steady_clock::now().time_since_epoch()).count());
    }

    // G10 — first coid for a new strategy session. Encodes wall-clock
    // seconds since UNIX epoch in the high 32 bits; counter starts at 1
    // in the low 32 bits.
    static std::uint64_t initial_coid() {
        using namespace std::chrono;
        const auto secs = duration_cast<seconds>(
            system_clock::now().time_since_epoch()).count();
        return (static_cast<std::uint64_t>(secs) << 32) | std::uint64_t{1};
    }

    int32_t mid_in_mmx_ticks(const connector::Tick& t) const {
        const int64_t mid_conn = (t.bid_ticks + t.ask_ticks) / 2;
        return static_cast<int32_t>(mid_conn / cfg_.conn_ticks_per_mmx_tick);
    }

    void maybe_refresh(std::uint64_t now_ns, int32_t /*mid_mmx*/) {
        if (last_refresh_ns_ != 0 &&
            now_ns - last_refresh_ns_ < cfg_.refresh_period_ns) return;

        const double dt_sec =
            static_cast<double>(cfg_.refresh_period_ns) / 1e9;
        const double kappa_raw = kappa_.refit(now_ns);
        // G6 — clamp κ to the operator-supplied floor.
        const double kappa_eff =
            (kappa_raw < cfg_.kappa_floor) ? cfg_.kappa_floor : kappa_raw;

        ParamCompilerInputs in{};
        in.sigma2_per_sec = ewma_.sigma2_per_sec(dt_sec);
        in.kappa          = kappa_eff;
        in.horizon_sec    = cfg_.horizon_sec;
        in.base_gamma     = cfg_.base_gamma;
        in.regime_id      = regime_.get();
        in.breaker_tripped= breaker_.tripped();
        in.max_inv        = cfg_.max_inv_lots;
        // G13 — derate qty when recent fills look toxic.
        in.base_qty       = effective_base_qty();
        in.requote_ticks  = cfg_.requote_ticks;
        in.tick_size      = 1.0;
        in.now_ns         = now_ns;

        ParamBlock p{};
        if (compiler_.compile(in, &p)) {
            fpga_.load(p, now_ns);
        }
        last_refresh_ns_       = now_ns;
        dropped_orders_window_ = 0;
    }

    bool sup_tick(std::uint64_t now_ns, int32_t mid_mmx,
                  std::uint64_t last_refresh, ParamBlock* kill_out) {
        pnl_.snapshot(now_ns, mid_mmx);
        constexpr std::uint64_t kFiveMin =
            5ULL * 60ULL * 1'000'000'000ULL;
        const double daily   = pnl_.total_pnl(mid_mmx);
        const double rolling = pnl_.rolling_pnl(now_ns, kFiveMin, mid_mmx);
        const bool now_tripped = breaker_.evaluate(now_ns, daily, rolling,
                                                   inventory_lots(),
                                                   last_refresh);
        if (!now_tripped || kill_emitted_) return false;
        ParamBlock p{};
        p.kill_switch = 1;
        p.epoch_ns    = now_ns;
        sign_param_block(p, cfg_.hmac_key.data(), cfg_.hmac_key.size());
        *kill_out = p;
        kill_emitted_ = true;
        return true;
    }

    void propagate_kill_to_fpga(std::uint64_t now_ns) {
        ParamBlock p{};
        p.kill_switch = 1;
        p.epoch_ns    = now_ns;
        sign_param_block(p, cfg_.hmac_key.data(), cfg_.hmac_key.size());
        fpga_.load(p, now_ns);
        kill_emitted_ = true;
    }

    void emit_quotes(const connector::Tick& t, std::uint64_t now_ns,
                     int32_t mid_mmx) {
        const auto qo = fpga_.tick(mid_mmx, inventory_lots(), now_ns);
        if (qo.cancel_only) return;

        // G14 — force replace if any side has aged past max_quote_age_ns.
        const bool age_force =
            (last_emit_bid_ns_ != 0 &&
             now_ns - last_emit_bid_ns_ > cfg_.max_quote_age_ns) ||
            (last_emit_ask_ns_ != 0 &&
             now_ns - last_emit_ask_ns_ > cfg_.max_quote_age_ns);

        const auto& last = last_quote_;
        if (last.valid && !age_force &&
            std::abs(qo.bid - last.bid) <= last.requote_ticks &&
            std::abs(qo.ask - last.ask) <= last.requote_ticks) {
            return;
        }
        last_quote_ = LastQuote{
            qo.bid, qo.ask,
            static_cast<int32_t>(active_param_block().requote_ticks),
            true};

        // Convert MMX ticks back to connector int64 ticks.
        const int64_t bid_conn =
            static_cast<int64_t>(qo.bid) * cfg_.conn_ticks_per_mmx_tick;
        const int64_t ask_conn =
            static_cast<int64_t>(qo.ask) * cfg_.conn_ticks_per_mmx_tick;
        const int64_t qty_conn =
            static_cast<int64_t>(qo.qty) * cfg_.lot_to_qty;

        const int32_t lots = inventory_lots();
        const bool skip_buy  =
            lots >=  static_cast<int32_t>(cfg_.max_inv_lots);
        const bool skip_sell =
            lots <= -static_cast<int32_t>(cfg_.max_inv_lots);

        // G1 — REPLACE if we already have a live quote on that side, else
        // ENTER (POST_ONLY). Coid bookkeeping happens inside push_order.
        if (!skip_buy) {
            const bool replace = outstanding_bid_coid_ != 0;
            push_order(t, bid_conn,  qty_conn, connector::Side::BUY,
                       replace ? connector::OrdType::REPLACE
                               : connector::OrdType::POST_ONLY);
        }
        if (!skip_sell) {
            const bool replace = outstanding_ask_coid_ != 0;
            push_order(t, ask_conn, -qty_conn, connector::Side::SELL,
                       replace ? connector::OrdType::REPLACE
                               : connector::OrdType::POST_ONLY);
        }
    }

    void push_order(const connector::Tick& t, int64_t price_ticks,
                    int64_t signed_qty, connector::Side side,
                    connector::OrdType op = connector::OrdType::POST_ONLY) {
        connector::Order o{};
        o.ts_ns           = t.ts_ns ? t.ts_ns : monotonic_now_ns();
        const std::uint64_t fresh_coid =
            next_coid_.fetch_add(1, std::memory_order_relaxed);
        o.client_order_id = fresh_coid;
        o.strategy_id     = static_cast<uint32_t>(id());
        o.symbol_id       = t.symbol_id;
        o.price_ticks     = price_ticks;
        o.quantity        = signed_qty;
        o.side            = static_cast<uint8_t>(side);
        o.ord_type        = static_cast<uint8_t>(op);
        o.tif             = static_cast<uint8_t>(connector::TIF::DAY);
        o.account_id      = connector::ACCOUNT_INVALID;
        // G1 — REPLACE/CANCEL carry a reference to the prior coid.
        if (op == connector::OrdType::REPLACE ||
            op == connector::OrdType::CANCEL) {
            o.ref_order_id = (side == connector::Side::BUY)
                               ? outstanding_bid_coid_
                               : outstanding_ask_coid_;
        }

        if (sink_->send(o)) {
            // Update outstanding coid bookkeeping (only on successful send).
            // CANCEL retires the side; REPLACE/POST_ONLY take over the slot.
            LevelTrack& lvl = (side == connector::Side::BUY) ? bid_lvl_
                                                              : ask_lvl_;
            if (op == connector::OrdType::CANCEL) {
                if (side == connector::Side::BUY)  outstanding_bid_coid_ = 0;
                else                               outstanding_ask_coid_ = 0;
                lvl = LevelTrack{};
            } else {
                if (side == connector::Side::BUY)  outstanding_bid_coid_ = fresh_coid;
                else                               outstanding_ask_coid_ = fresh_coid;
                // G8 — record where we joined; queue_ahead will refresh on
                // the next on_book that includes our level.
                lvl.valid             = true;
                lvl.price_mmx         = static_cast<int32_t>(
                    price_ticks / cfg_.conn_ticks_per_mmx_tick);
                lvl.our_qty           = signed_qty >= 0 ? signed_qty : -signed_qty;
                lvl.queue_ahead       = 0;
                lvl.queue_ahead_valid = false;
            }
            // G9 — stamp STRATEGY_EMIT for every accepted Order.
            if (cfg_.telemetry) {
                connector::TelemetryRecord rec{};
                rec.ts_ns           = o.ts_ns;
                rec.client_order_id = o.client_order_id;
                rec.strategy_id     = o.strategy_id;
                rec.hop = static_cast<uint8_t>(connector::Hop::STRATEGY_EMIT);
                cfg_.telemetry->push(rec);
            }
            // G14 — record last emit time per side for age-forced replace.
            if (op != connector::OrdType::CANCEL) {
                if (side == connector::Side::BUY)  last_emit_bid_ns_ = o.ts_ns;
                else                                last_emit_ask_ns_ = o.ts_ns;
            }
            return;
        }

        // G7 — sink saturated. Track and, beyond a threshold inside one
        // refresh window, trip the internal breaker.
        ++dropped_orders_total_;
        ++dropped_orders_window_;
        if (cfg_.dropped_order_breaker_threshold > 0 &&
            dropped_orders_window_ >= cfg_.dropped_order_breaker_threshold) {
            propagate_kill_to_fpga(o.ts_ns);
        }
    }

    // G13 — examine each pending fill; if its toxicity window has elapsed
    // without us evaluating it yet, score and roll into the rolling window.
    void evaluate_pending_toxicity(std::uint64_t now_ns, int32_t mid_mmx) {
        while (!pending_fills_.empty()) {
            const auto& pf = pending_fills_.front();
            if (now_ns - pf.ts_ns < cfg_.toxic_window_ns) break;
            // Adverse mid move: down for buys, up for sells.
            const int32_t move = mid_mmx - pf.mid_at_fill_mmx;
            const bool toxic = (pf.side > 0)
                                 ? (-move > cfg_.toxic_threshold_ticks)
                                 : ( move > cfg_.toxic_threshold_ticks);
            recent_evaluated_.push_back({now_ns, toxic});
            pending_fills_.pop_front();
        }
        // Evict beyond the lookback window.
        while (!recent_evaluated_.empty() &&
               recent_evaluated_.front().first + cfg_.toxic_lookback_ns < now_ns) {
            recent_evaluated_.pop_front();
        }
    }

    double current_toxic_share() const {
        if (recent_evaluated_.empty()) return 0.0;
        std::size_t toxic = 0;
        for (const auto& e : recent_evaluated_) if (e.second) ++toxic;
        return static_cast<double>(toxic) /
               static_cast<double>(recent_evaluated_.size());
    }

    uint32_t effective_base_qty() const {
        const double share = current_toxic_share();
        if (share <= cfg_.toxic_share_threshold) return cfg_.base_qty_lots;
        const double scaled = static_cast<double>(cfg_.base_qty_lots)
                            * cfg_.toxic_qty_reduction;
        if (scaled < 1.0) return 1u;
        return static_cast<uint32_t>(scaled);
    }

    // G8 — for each side that has a live quote, look up our resting price
    // in the L2 snapshot and update our queue_ahead estimate.
    void update_queue_position(const connector::BookUpdate& b) {
        auto find_qty = [&](const connector::BookLevel* levels,
                            int32_t target_mmx) -> int64_t {
            for (int i = 0; i < connector::BOOK_DEPTH; ++i) {
                const auto& l = levels[i];
                if (l.price_ticks == 0 && l.quantity == 0) break;
                int32_t px_mmx = static_cast<int32_t>(
                    l.price_ticks / cfg_.conn_ticks_per_mmx_tick);
                if (px_mmx == target_mmx) return l.quantity;
            }
            return -1;  // not in top-N
        };
        if (bid_lvl_.valid) {
            int64_t total = find_qty(b.bids, bid_lvl_.price_mmx);
            if (total >= 0) {
                bid_lvl_.queue_ahead =
                    total > bid_lvl_.our_qty ? (total - bid_lvl_.our_qty)
                                              : int64_t{0};
                bid_lvl_.queue_ahead_valid = true;
            }
        }
        if (ask_lvl_.valid) {
            int64_t total = find_qty(b.asks, ask_lvl_.price_mmx);
            if (total >= 0) {
                ask_lvl_.queue_ahead =
                    total > ask_lvl_.our_qty ? (total - ask_lvl_.our_qty)
                                              : int64_t{0};
                ask_lvl_.queue_ahead_valid = true;
            }
        }
    }

    void emit_cancels_for_outstanding() {
        if (sink_ == nullptr) return;
        // Synthetic Tick — only ts_ns and symbol_id are read by push_order.
        connector::Tick t{};
        t.ts_ns     = monotonic_now_ns();
        t.symbol_id = cfg_.symbol_id;
        if (outstanding_bid_coid_ != 0) {
            push_order(t, /*price*/ -1, /*qty*/ 0,
                       connector::Side::BUY, connector::OrdType::CANCEL);
        }
        if (outstanding_ask_coid_ != 0) {
            push_order(t, /*price*/ -1, /*qty*/ 0,
                       connector::Side::SELL, connector::OrdType::CANCEL);
        }
    }

    struct LastQuote {
        int32_t bid;
        int32_t ask;
        int32_t requote_ticks;
        bool    valid;
    };

    // G8 — per-side queue-position track. `queue_ahead_valid=false` means
    // we've placed but no L2 snapshot has matched our level yet.
    struct LevelTrack {
        bool    valid             = false;
        int32_t price_mmx         = 0;
        int64_t our_qty           = 0;     // contracts at level
        int64_t queue_ahead       = 0;     // contracts ahead of us
        bool    queue_ahead_valid = false;
    };

    // G13 — pending-evaluation fill record.
    struct PendingFill {
        std::uint64_t ts_ns;
        int8_t        side;             // +1 buy, -1 sell
        int32_t       mid_at_fill_mmx;
    };

    ConnectorAdapterConfig    cfg_;
    connector::OrderSink*     sink_ = nullptr;
    EwmaVariance              ewma_;
    KappaEstimator            kappa_;
    ParamCompiler             compiler_;
    FpgaStub                  fpga_;
    RegimeMailbox             regime_;
    PnlTracker                pnl_;
    CircuitBreaker            breaker_;

    std::atomic<bool>         active_{true};
    std::atomic<uint64_t>     next_coid_{1};
    std::uint64_t             last_refresh_ns_     = 0;
    std::uint64_t             event_clear_at_ns_   = 0;
    int64_t                   inventory_contracts_ = 0;
    LastQuote                 last_quote_{0, 0, 0, false};
    bool                      kill_emitted_        = false;
    connector::RejectReason   last_kill_reason_    = connector::RejectReason::NONE;
    std::uint64_t             dropped_orders_total_  = 0;
    std::uint32_t             dropped_orders_window_ = 0;
    // G1 — live order-id per side; 0 = no outstanding quote on that side.
    std::uint64_t             outstanding_bid_coid_ = 0;
    std::uint64_t             outstanding_ask_coid_ = 0;
    // G8 — queue-position bookkeeping per side.
    LevelTrack                bid_lvl_{};
    LevelTrack                ask_lvl_{};
    // G13 — toxicity tracking.
    std::deque<PendingFill>                        pending_fills_{};
    std::deque<std::pair<std::uint64_t, bool>>     recent_evaluated_{};
    // G14 — last successful per-side emit timestamps.
    std::uint64_t             last_emit_bid_ns_ = 0;
    std::uint64_t             last_emit_ask_ns_ = 0;
};

}  // namespace mmx
