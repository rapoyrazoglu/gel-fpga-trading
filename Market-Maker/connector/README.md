# MMX ↔ connector plug-in

MMX plugs into GEL through the **`connector`** SDK — the neutral contract
shared by GEL, PCTA, and MMX. This directory holds the only seam.

`mmx::MmxStrategy` (in [`mmx_strategy.hpp`](mmx_strategy.hpp)) implements
`connector::IStrategy` and is a drop-in replacement for GEL's baseline
`gel::strategies::MarketMakerStrategy` (which itself is now an in-tree
`connector::IStrategy`).

## What MMX gives you over the baseline

| Property                    | `gel::strategies::MarketMakerStrategy`     | `mmx::MmxStrategy`                                |
| --------------------------- | ------------------------------------------ | ------------------------------------------------- |
| Pricing math representation | `double`, recomputed every tick            | Q1.31 fixed-point, recomputed every ~100 ms       |
| FPGA path                   | none                                       | shared `compute_quote(...)` source, HLS-ready     |
| ParamBlock + HMAC           | n/a                                        | 64 B cache-line, RFC 4231 HMAC-SHA256             |
| Volatility                  | EWMA on `double`                           | EWMA on log-returns + GARCH upgrade slot          |
| κ arrival-rate              | constant from config                       | OLS fit on rolling 60 s window, median-smoothed   |
| Inventory                   | int64 lots, no sub-lot precision           | int64 contracts (sub-lot exact)                   |
| Risk supervisor             | none in-strategy                           | daily / 5-min PnL + staleness watchdog            |
| Regime overrides            | none                                       | PCTA mailbox + on_event(HIGH) auto-flip           |
| Symbol guard                | implicit                                   | explicit `cfg.symbol_id` filter                   |
| Backpressure                | silent drop                                | `dropped_orders()` counter + auto-kill on flood   |
| Kill propagation            | strategy flag only                         | strategy flag **and** FPGA stub                   |
| Tick-to-trade target        | host-CPU bound                             | ≤ 2 µs on FPGA (Phase B)                          |
| Acceptance tests            | implicit                                   | spec §10 #1–#11 + connector adapter integration   |

## Wiring it up

### Option A — GEL adds MMX as a subdirectory

In `GEL/CMakeLists.txt`:

```cmake
option(GEL_USE_MMX "Use MMX as the market-maker strategy" ON)
if(GEL_USE_MMX)
    set(MMX_BUILD_CONNECTOR_ADAPTER ON CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_SOURCE_DIR}/../Market-Maker mmx)
endif()
```

In whatever GEL source registers strategies:

```cpp
#include "connector/mmx_strategy.hpp"

mmx::ConnectorAdapterConfig cfg;
cfg.symbol_id               = eurusd_symbol_id;
cfg.conn_ticks_per_mmx_tick = 1000;        // 1 pip on EUR/USD
cfg.lot_to_qty              = 100;
cfg.base_qty_lots           = 1;
cfg.max_inv_lots            = 100;
cfg.requote_ticks           = 1;
cfg.refresh_period_ns       = 100'000'000ULL;
cfg.kappa_floor             = 5.0;          // calibrate per-instrument
cfg.hmac_key                = load_hmac_key();   // operator-provisioned

auto mm = std::make_shared<mmx::MmxStrategy>(cfg);
engine.register_strategy(mm);
```

Engine then stamps `account_id` via `connector::StrategyAccountMap::stamp_primary`
on every Order MMX emits — MMX itself stays account-agnostic.

### Option B — standalone build to develop / test the adapter

```bash
cmake -S . -B build -DMMX_BUILD_CONNECTOR_ADAPTER=ON \
                    -DCONNECTOR_INCLUDE_DIR=/path/to/connector/include
cmake --build build -j
( cd build && ctest -R test_connector_adapter --output-on-failure )
```

Sibling-checkout layout (`capstone/{connector,Market-Maker}`) auto-detects the
include path; `CONNECTOR_INCLUDE_DIR` only needs to be passed when connector
lives somewhere unusual.

## Wire-format translation

`connector::Order` and `connector::ExecReport` use **int64 ticks at scale 10⁻⁸**
(`PRICE_SCALE = 8`) and **signed int64 quantity in contracts**. MMX's pricing
engine works on **int32 ticks at an instrument-defined granularity** and
**int32 lots**. The adapter handles both conversions through two config knobs:

| Config field                | Meaning                                      | EUR/USD example |
| --------------------------- | -------------------------------------------- | --------------- |
| `conn_ticks_per_mmx_tick`   | how many connector ticks make one MMX tick   | `1000` (1 pip = 10⁻⁵ = 1000 × 10⁻⁸) |
| `lot_to_qty`                | how many connector contract units in one MMX lot | `100`         |

**Inventory** is tracked at the contract level (int64), so partial fills below
`lot_to_qty` accumulate exactly. Pricing-engine input (`q` lots) is derived
by integer division at use-time. Get either knob wrong and prices arrive at
the exchange off by orders of magnitude — the adapter's HMAC won't catch
that, the engine's fat-finger gate will.

## Lifecycle that the engine drives

```
on_start(sink)            ─►  capture sink, raise active flag
on_tick(t)                ─►  σ²/κ update; refresh ParamBlock if 100 ms elapsed;
                              compute_quote() via FpgaStub; emit POST_ONLY pair
on_book(b)                ─►  feed L2 levels into κ estimator
on_event(e)               ─►  HIGH-impact ⇒ flip regime to event_imminent
on_fill(r)                ─►  inventory_contracts_ += signed qty; PnL bookkeeping
on_kill_switch(reason)    ─►  active=false, push kill ParamBlock to FPGA stub
on_resume()               ─►  active=true, reset breaker, clear FPGA kill on next refresh
on_stop()                 ─►  drop sink, active=false
```

Plus one slow-path fan-in from PCTA:

```cpp
strat.set_regime("event_imminent");   // or set_regime(uint8_t id)
```

## Safety properties this preserves

1. **Strategy never touches the wire.** Every order goes through
   `OrderSink::send` → engine's risk gate → router. If the engine trips us,
   we go silent.
2. **HMAC over every ParamBlock.** The FPGA stub auto-asserts kill on bad MAC
   or staleness > 500 ms (spec §7.3).
3. **Inventory saturates, never wraps.** `compute_quote(...)` enforces
   `|q| ≤ max_inv` and produces `cancel_only=1` past the cap.
4. **Kill is symmetric.** Engine `on_kill_switch` flips the active flag *and*
   pushes a `kill_switch=1` ParamBlock into the FPGA stub. The internal
   risk supervisor's `kill_switch=1` ParamBlock has the same effect. Either
   path makes the strategy emit nothing.
5. **Wrong-symbol ticks never reach pricing math** (`cfg.symbol_id` filter).
6. **Sub-lot fills cannot drift** (inventory in contracts, not lots).
7. **HIGH-impact events trigger a deterministic regime flip** even if PCTA's
   mailbox hasn't fired yet.

## What still lives in MMX's repo (not in connector or GEL)

- The fixed-point pricing engine (`host/pricing/pricing_engine.h`) — single
  source of truth, also feeds the Vitis HLS top.
- The slow-path math — `EwmaVariance`, `KappaEstimator`, `ParamCompiler`.
- The ParamBlock + HMAC scheme.
- The simulator with queue-position fill model.
- Acceptance tests #1–#11 from spec §10.

connector stays neutral; GEL stays clean of MM-specific math; MMX owns the
strategy. Three layers, one boundary file each.
