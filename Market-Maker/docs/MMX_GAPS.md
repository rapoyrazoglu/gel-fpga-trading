# MMX — Known gaps and follow-ups

> Audit date: 2026-05-09
> Audited against: spec `docs/MMX_SPECIFICATION.md`, `connector/include/connector/*.hpp`,
> and `Market-Maker/{host,sim,gel}/`.

This document is the *honest list* of what MMX does **not** yet do. Each gap
has a severity (🔴 critical, 🟠 important, 🟢 nice-to-have), an owner side
(MMX, connector SDK, or GEL engine), and a fix sketch.

---

## 🔴 Critical — fix before live deployment

### G1. Cancel / replace semantics — **fixed** (Pair 1)
- **Resolution:** option (1) shipped. `connector::OrdType` now exposes
  `CANCEL=5` and `REPLACE=6`; `connector::Order` carries a new `uint64_t
  ref_order_id` field at offset 52 (the existing 48-byte OUCH ENTER ORDER
  read window is untouched, so connector's parity test still passes).
- **MMX behaviour:** adapter tracks `outstanding_bid_coid_` and
  `outstanding_ask_coid_`. The first quote on each side is `POST_ONLY`;
  subsequent requotes are `REPLACE` referencing the prior coid. On any
  terminal `ExecStatus` (FILLED / CANCELED / EXPIRED / REJECTED) the side's
  coid is retired so the next requote starts fresh. `on_kill_switch` emits
  explicit `CANCEL` orders for any live side.
- **Tests:** `test_connector_adapter` G1 section verifies first-POST_ONLY,
  subsequent-REPLACE-with-correct-ref, retire-on-fill, and kill→CANCEL.

### G2. Sub-lot quantization on partial fills
- **Where:** `gel/mmx_strategy.hpp::on_fill`:
  `lots = static_cast<int32_t>(r.fill_quantity / cfg_.lot_to_qty);`
- **Effect:** with `lot_to_qty=100` and a 30-contract partial fill, integer
  division → 0 lots. Inventory drifts silently from reality.
- **Fix (MMX-local):** track `inventory_contracts_` as `int64` and derive
  `inventory_lots()` from it. Pricing engine still receives an `int32` lot count
  but the running sum is exact.
- **Status:** **fixed** in this audit pass.

### G3. GEL kill does not propagate to the FPGA path
- **Where:** `on_kill_switch` only flips `active_=false`. FPGA stub still holds
  the last-loaded ParamBlock; if `active_` flips back via `on_resume()` while
  GEL hasn't reset its own kill state, FPGA quotes resume immediately.
- **Effect:** double-kill protection isn't symmetric. GEL kill must lock FPGA
  too.
- **Fix (MMX-local):** on `on_kill_switch`, build a `kill_switch=1` ParamBlock,
  HMAC-sign it, push to `FpgaStub`. On `on_resume`, reset internal breaker +
  `kill_emitted_=false`; the next `maybe_refresh()` will load a clean block.
- **Status:** **fixed** in this audit pass.

### G4. Multi-symbol state collisions
- **Where:** `inventory_lots_` and `last_quote_` are scalar; one MmxStrategy
  instance receives `Tick` callbacks for *every* symbol the engine fans in.
- **Effect:** if registered for >1 symbol, state of last symbol's tick
  overwrites the prior one's outstanding quotes; inventory adds across symbols.
- **Fix (MMX-local):** require single-symbol per instance via
  `cfg.symbol_id` and reject mismatched ticks. Multi-instrument MMX is
  achieved by registering N strategy instances. Per-symbol σ²/κ/ParamBlock are
  the natural unit anyway.
- **Status:** **fixed** in this audit pass.

---

## 🟠 Important — should land before the connector adapter migration

### G5. `on_event` is a no-op
- **Where:** `MmxStrategy::on_event` defaults to the `IStrategy` base no-op.
- **Effect:** GEL fires HIGH-impact `CalendarEvent`s through this hook (FOMC,
  NFP, CPI). MMX only flips into `event_imminent` regime if PCTA pushes a
  `set_regime("event_imminent")` — and PCTA's LLM loop has hundreds of ms of
  latency. We have a faster, deterministic path right under our nose.
- **Fix (MMX-local):** in `on_event`, if `impact == HIGH`, flip the
  `RegimeMailbox` to `kRegimeEventImminent` immediately; clear the override
  N seconds after the event ts (configurable, default 60 s).
- **Status:** **fixed** in this audit pass.

### G6. κ underflow with tick-only feeds
- **Where:** `KappaEstimator::refit` returns `1e-3` when the rolling window
  is empty. `(1/γ) · ln(1 + γ/κ)` with κ=1e-3 → half_spread blows up to
  hundreds of ticks → quotes never inside the market → 0 fills.
- **Effect:** `mmx_replay` reports zero fills on synthetic feeds; reproducible
  in `mmx_host`.
- **Fix (MMX-local):** add `cfg.kappa_floor` (default 1.0 — typical for tier-1
  FX ECN). Document that operators should override per-instrument from a
  calibrated value; rolling-window estimate refines it but cannot underflow it.
- **Status:** **fixed** in this audit pass.

### G7. Sink backpressure is silent
- **Where:** `push_order` discards the bool from `sink_->send(o)`.
- **Effect:** when the SPSC ring is saturated, MMX silently drops orders. The
  next dashboard glance shows "no fills"; no signal to the operator.
- **Fix (MMX-local):** maintain `dropped_orders_` counter; expose via
  `dropped_orders()` accessor; if `dropped_orders_ > N` within window, trip the
  internal breaker.
- **Status:** **fixed** in this audit pass.

### G8. `on_book` queue-position feedback — **fixed** (Pair 1)
- **Resolution:** observability-level G8 shipped. The adapter now tracks
  per-side `LevelTrack { price_mmx, our_qty, queue_ahead, queue_ahead_valid }`.
  On placement (`POST_ONLY` or `REPLACE`) the slot is recorded and
  `queue_ahead_valid=false`. Each subsequent `on_book` matching our price
  refreshes `queue_ahead = max(0, level_qty − our_qty)` (assumes the L2 snapshot
  includes our resting qty in the level total). On `CANCEL` the slot resets.
- **Public API:** `queue_ahead_bid()` / `queue_ahead_ask()` return the
  estimate or `-1` if unknown / no live quote on that side.
- **Closed-loop usage:** not yet — the strategy reports the estimate but
  doesn't yet adjust `base_qty` or `requote_ticks` from it. That's part of
  Pair 4 (G13 toxicity feedback).
- **Tests:** `test_connector_adapter` G8 section drives synthetic `BookUpdate`s
  and verifies the queue_ahead value tracks visible-volume changes (incl.
  cancels-ahead and clamp-to-zero).

---

## 🟢 Nice-to-have — quality-of-life and observability

### G9. Telemetry stamping — **fixed** (Pair 2)
- **Resolution:** `mmx::ITelemetrySink` interface added in
  `connector/mmx_strategy.hpp`; `ConnectorAdapterConfig::telemetry` is an
  optional pointer (default `nullptr` = telemetry off, no overhead). On
  every successful `OrderSink::send`, MMX stamps a `STRATEGY_EMIT`
  `connector::TelemetryRecord` carrying `(ts_ns, client_order_id, strategy_id)`.
  Failed sends are not stamped — they're tracked in `dropped_orders()`.
- **Tests:** G9 section verifies one record per accepted Order with matching
  fields and the correct `Hop`.

### G10. `client_order_id` collisions across restarts — **fixed** (Pair 2)
- **Resolution:** `next_coid_` is initialised to
  `(seconds_since_unix_epoch << 32) | 1` in the constructor. The high 32 bits
  are the session prefix (lasts until 2106); the low 32 bits are the within-
  session counter (4 billion orders per session). Two restarts in the same
  second still collide; collocated processes should also stagger session ids.
- **Tests:** G10 section asserts the coid prefix is the current wall-clock
  second (with a 5-minute window for test-host skew) and that all coids in
  one session share the same prefix with a strictly monotonic counter.

### G11. HMAC key provisioning — **fixed** (Pair 3)
- **Resolution:** `mmx::load_hmac_key(const std::string& path)` reads a
  32-byte binary file. POSIX mode check rejects any group/world permission
  bits (file must be 0600). Throws `std::runtime_error` on size mismatch,
  open failure, or perm violation. Production layout: `/etc/mmx/hmac.key`
  managed by deployment tooling, never checked in.
- **Tests:** G11 section verifies (a) wrong size rejected, (b) world-readable
  rejected, (c) correct 0600 file round-trips bytes exactly.

### G12. Restart reconciliation — **fixed** (Pair 3)
- **Resolution:** `connector::IStrategy::on_open_orders(const Order*, size_t)`
  added with a default no-op (every existing strategy stays compatible).
  MMX overrides it to rehydrate `outstanding_*_coid_`, `LevelTrack`, and
  to bump `next_coid_` past any replayed coid in the same session prefix.
  Subsequent requotes emit `REPLACE` referencing the replayed coid — the
  book never sees a duplicate POST_ONLY for an order it already knows about.
- **Tests:** G12 section replays one live bid + one live ask; verifies the
  next requote per side is REPLACE pointing at the replayed coid.

### G13. Toxicity feedback — **fixed** (Pair 4)
- **Resolution:** every `on_fill` queues a `PendingFill { ts_ns, side,
  mid_at_fill_mmx }`. Each subsequent `on_tick` evaluates fills whose
  `toxic_window_ns` has elapsed: if the live mid moved against our position
  by more than `toxic_threshold_ticks`, the fill is tagged toxic. A rolling
  `toxic_lookback_ns` deque holds (ts, was_toxic) pairs; when the toxic
  share crosses `toxic_share_threshold`, `effective_base_qty()` scales
  `base_qty_lots` by `toxic_qty_reduction` (clamped ≥ 1). The compiled
  ParamBlock picks the derated qty up on the next refresh.
- **Public API:** `toxic_share()`, `qty_effective()` for telemetry/dashboards.
- **Tests:** G13 section feeds 3 buy fills, then a tick with adverse mid;
  asserts share > 0.5 and effective qty halves from 4 → 2.

### G14. Host-side `MAX_QUOTE_AGE_NS` — **fixed** (Pair 4)
- **Resolution:** per-side `last_emit_bid_ns_` / `last_emit_ask_ns_` updated
  on every accepted Order. `emit_quotes` checks
  `(now − last_emit) > cfg.max_quote_age_ns` for either side; if so, the
  re-emit fires even when the requote-tick threshold is not crossed. Default
  50 ms matches spec §3.5.
- **Tests:** G14 section sets `requote_ticks=100` (effectively disabling
  price-based requotes), drives identical ticks within the age window
  (only initial pair fires), then jumps past the window and fires the same
  tick again — re-emit observed.

---

## Cross-reference table

| ID | Severity | Owner | Status |
|----|----------|-------|--------|
| G1 | 🔴 | connector SDK | **fixed** (Pair 1) |
| G2 | 🔴 | MMX | **fixed** in audit pass |
| G3 | 🔴 | MMX | **fixed** in audit pass |
| G4 | 🔴 | MMX | **fixed** in audit pass |
| G5 | 🟠 | MMX | **fixed** in audit pass |
| G6 | 🟠 | MMX | **fixed** in audit pass |
| G7 | 🟠 | MMX | **fixed** in audit pass |
| G8 | 🟠 | MMX | **fixed** (Pair 1) |
| G9 | 🟢 | MMX | **fixed** (Pair 2) |
| G10 | 🟢 | MMX | **fixed** (Pair 2) |
| G11 | 🟢 | MMX + ops | **fixed** (Pair 3) |
| G12 | 🟢 | connector + MMX | **fixed** (Pair 3) |
| G13 | 🟢 | MMX | **fixed** (Pair 4) |
| G14 | 🟢 | MMX | **fixed** (Pair 4) |

---

## What "implement" still means after this audit

All 14 audit-pass gaps are now closed. Phase A acceptance tests #1–#11 from
spec §10 plus the connector-adapter integration test (with sub-tests for
G1, G2, G3, G4, G5, G7, G8, G9, G10, G11, G12, G13, G14) all pass under
`ctest` — 12/12 green.

Live-deployment work that remains is operational, not strategic:

- Calibrate `kappa_floor` per instrument from a tier-1 ECN sample.
- Cut a real HMAC key with `head -c 32 /dev/urandom > /etc/mmx/hmac.key && chmod 600 …`.
- Wire a `mmx::ITelemetrySink` implementation that forwards into the engine's
  Prometheus pipeline.
- Replace `FpgaStub` with the PCIe XDMA driver in Phase B.
- Implement OUCH wire encoding for `OrdType::CANCEL` / `OrdType::REPLACE`
  (the SDK exposes the operations; the wire encoder in
  `connector/order_router_native.hpp` currently only emits ENTER ORDER).
