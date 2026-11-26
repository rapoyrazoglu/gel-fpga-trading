# MMX — Market Maker eXecution

> **Owner:** Remzi Atasagun Poyrazoğlu
> **Program:** BAU Computer Engineering — Capstone
> **Spec:** [`docs/MMX_SPECIFICATION.md`](docs/MMX_SPECIFICATION.md)
> **Sister system:** PCTA (calendar-driven RL signal source)
> **Host platform:** GEL via the neutral [`connector`](../connector/) SDK — see [`connector/README.md`](connector/README.md)

MMX is an inventory-aware, volatility-adaptive market maker built on the
Avellaneda–Stoikov framework, designed as a **split fast-path / slow-path**
system: every per-tick decision lives on an FPGA (target ≤ 2 µs tick-to-trade
on AWS F2 / Xilinx VU47P), every per-second decision lives on a host CPU.

MMX is a **Lego piece**, not a standalone trading system: it plugs into GEL
through `connector::IStrategy` (single boundary file:
`connector/mmx_strategy.hpp`) and snaps in or out on demand. GEL owns risk,
routing, drop-copy, and the NIC; MMX owns Avellaneda–Stoikov + the FPGA
pricing engine.

---

## Status

Both phases are done. **Phase A** is the software reference: every host
component, the simulator and the synthesizable HLS source. **Phase B** is the
VHDL data plane in [`fpga/rtl/`](fpga/rtl/), which has run on an AWS F2
instance: 20 cycles (80 ns) from market-data beat in to order beat out, timing
met at 250 MHz. The silicon build omits the parameter-block HMAC check, which
is verified in simulation. See the [repository README](../README.md) for the
measurements and their limits.

The table below is the Phase A component list.

| Component                              | Status                       |
| -------------------------------------- | ---------------------------- |
| `ParamBlock` (cache-line, alignof 64)  | ✅ implemented + tested      |
| HMAC-SHA256 sign / verify              | ✅ self-contained, RFC 4231  |
| Pricing engine (Q1.31 fixed-point)     | ✅ shared host + HLS source  |
| EWMA σ² estimator                      | ✅ RiskMetrics λ = 0.94      |
| κ arrival-rate fit (OLS + median)      | ✅ 60 s rolling window       |
| Param compiler (slow-path heart)       | ✅ §5.3 reference            |
| PCTA regime ingest + mailbox           | ✅ table per spec §5.4       |
| Risk supervisor + circuit breaker      | ✅ §5.5 trip rules           |
| LOB simulator + queue-position model   | ✅ §9.1 fill model           |
| FPGA stub (param loader + tick gate)   | ✅ software model of §4.7/§4.8|
| HLS C++ for `pricing_engine`           | ✅ `fpga/hls/`               |
| **connector plug-in adapter (IStrategy)** | ✅ `connector/mmx_strategy.hpp` |
| 11 of 12 spec §10 acceptance tests     | ✅ all green                 |
| connector adapter integration test     | ✅ optional, opt-in build    |
| §10 test #12 (HW-in-the-loop ≤ 2 µs)   | ✅ 80 ns compute path on F2  |
| RTL (`feed_parser`, `order_book`, …)   | ✅ `fpga/rtl/`, 23 testbenches|

Phase A's covenant: **the FPGA never sees `ln`, `exp`, `sqrt`, or non-constant
division; the CPU never makes a per-tick decision.** This is enforced by
acceptance test #11 — a static check that grep's the synth-critical paths.

---

## Build & test

Requires CMake ≥ 3.20 and a C++20 compiler (Clang 16+, GCC 12+, Apple Clang 17 OK).

```bash
cmake -S . -B build
cmake --build build -j
( cd build && ctest --output-on-failure )
```

Expected output:

```
Test project .../Market-Maker/build
 1/11 Test  #1: test_paramblock_size .............   Passed
 2/11 Test  #2: test_hmac_roundtrip ..............   Passed
 …
11/11 Test #11: test_no_floating_point_in_fpga ...   Passed
100% tests passed, 0 tests failed out of 11
```

### Smoke binaries

```bash
./build/mmx_replay  100000 42        # synthetic random-walk backtest
./build/mmx_host     50000           # full slow-path pipeline dry-run
```

`mmx_replay` is a deliberately crude smoke test on a 1-tick-wide synthetic
LOB; it exercises the simulator end-to-end but will report **zero fills**
because the synthetic feed's trade rate is too low for κ to converge to a
narrow half-spread. To produce meaningful Sharpe/PnL numbers, feed it real
L2 ticks (LOBSTER, Dukascopy) — the simulator interface is in
[`sim/lob_simulator.h`](sim/lob_simulator.h).

### Building with the connector adapter

```bash
# auto-detects ../connector/ as a sibling checkout
cmake -S . -B build -DMMX_BUILD_CONNECTOR_ADAPTER=ON
cmake --build build -j
( cd build && ctest --output-on-failure )    # 12/12 green
```

If `connector` lives elsewhere, pass `-DCONNECTOR_INCLUDE_DIR=/path/to/connector/include`.
The adapter is `connector/mmx_strategy.hpp`; usage and trade-offs vs. GEL's
baseline market maker are documented in [`connector/README.md`](connector/README.md).

---

## Repository layout

```
Market-Maker/
├── docs/
│   └── MMX_SPECIFICATION.md         spec (single source of truth)
├── host/                            C++20 host
│   ├── ipc/                         param_block, hmac, sha256, fpga_stub
│   ├── math/                        ewma, kappa_fit
│   ├── pricing/                     pricing_engine.h (shared with FPGA)
│   ├── risk/                        pnl_tracker, circuit_breaker
│   ├── threads/                     param_compiler, pcta_ingest, risk_supervisor
│   ├── util/                        fixed_point, spsc_queue
│   └── main.cpp                     dry-run orchestrator
├── sim/                             backtester
│   ├── queue_model.{h,cpp}          single-level queue-position model
│   ├── lob_simulator.{h,cpp}        replay-driven sim → SimResult
│   └── replay_main.cpp              CLI wrapper
├── fpga/
│   ├── hls/pricing_engine_hls.cpp   Vitis HLS top (re-uses host header)
│   ├── rtl/                         VHDL data plane + testbenches + mutants/
│   ├── cl_dataplane/                AWS F2 custom-logic wrapper + drivers
│   └── cl/                          pricing-kernel-only wrapper
├── connector/                       Lego connector to GEL via neutral SDK
│   ├── mmx_strategy.hpp             connector::IStrategy adapter
│   └── README.md                    plug-in instructions + comparison table
├── tests/                           one file per spec §10 acceptance test
└── CMakeLists.txt
```

---

## Architecture in one diagram

```
   ┌──────────────────────── FPGA (Phase B) ────────────────────────┐
   │  feed_parser → order_book → pricing_engine → quote_decider →   │
   │                    ↑                                            │
   │           ParamBlock (64 B, BRAM, dual-port)                   │
   └──────────────────────────┬─────────────────────────────────────┘
                              │ PCIe Gen3 ×8 (XDMA) — Phase B
                              │
   ┌────────────────────── HOST (Phase A, this repo) ───────────────┐
   │  EwmaVariance ──► ParamCompiler ──► sign(HMAC) ──► FpgaStub    │
   │  KappaEstimator ──┘     ▲                                       │
   │  RegimeMailbox  ────────┘                                       │
   │                                                                  │
   │  Fills ──► PnlTracker ──► CircuitBreaker ──► RiskSupervisor ──┐ │
   │                                                                ▼ │
   │                                          kill_switch=1 block ─┘ │
   └────────────────────────────────────────────────────────────────┘
```

The shared header `host/pricing/pricing_engine.h` is the single source of
truth for the per-tick math: it is consumed by the host reference, the
simulator, the FPGA stub, and (with HLS pragmas enabled) the Vitis HLS
synthesis target.

---

## Acceptance tests (spec §10)

| #  | File                                        | What it pins down                                              |
| -- | ------------------------------------------- | -------------------------------------------------------------- |
| 1  | `test_paramblock_size`                      | `sizeof == 64`, `alignof == 64`, field offsets                 |
| 2  | `test_hmac_roundtrip`                       | RFC 4231 vectors + 10 000 random sign/verify, tamper detection |
| 3  | `test_pricing_invariant_q0`                 | q=0 gives symmetric quotes around s with half-spread δ/2       |
| 4  | `test_pricing_skew_sign`                    | q>0 ⇒ bid+ask < 2s; q<0 ⇒ bid+ask > 2s; monotone in \|q\|      |
| 5  | `test_kill_switch_pulls`                    | kill_switch=1 ⇒ FPGA stub emits cancel-only across all q       |
| 6  | `test_paramblock_staleness`                 | epoch_ns 600 ms old ⇒ FPGA auto-asserts kill on load           |
| 7  | `test_fixed_point_no_drift`                 | 10⁶ random samples within ±1 tick of double-precision ref      |
| 8  | `test_simulator_queue_pos`                  | Queue-position 5 + 4 lots aggressive flow ⇒ no fill            |
| 9  | `test_circuit_breaker_trips`                | Loss > limit ⇒ signed kill block emitted within 50 ms          |
| 10 | `test_pcta_regime_overrides`                | `event_imminent` regime ⇒ next compile sets kill_switch=1      |
| 11 | `test_no_floating_point_in_fpga`            | No `float` / `double` survives in synthesizable code           |
| 12 | (HW-in-the-loop ≤ 2 µs)                     | run by hand on AWS F2: 20 cycles, 80 ns compute path            |

---

## Design notes

**Why one shared `pricing_engine.h` for host + FPGA?** Two implementations
drift; one cannot. The host reference, the simulator's pricing path, the
FPGA stub used by acceptance tests, and the Vitis HLS top function all call
the same `mmx::compute_quote(…)`. A future ECO that touches the math touches
exactly one file.

**Why HMAC over the first 48 bytes?** The 64-byte `ParamBlock` is laid out as
32 B fields + 16 B reserved + 16 B HMAC tag. The signed range is exactly
`[0, 48)`, which is why `kParamBlockSignedBytes == 48`. (The spec snippet in
`docs/MMX_SPECIFICATION.md §3.2` summed to 66 bytes — a typo. The corrected
layout matches §7.2 verbatim and is asserted at compile time.)

**Why Q1.31 instead of float on the host?** Eliminates the host-vs-FPGA
representation gap. The CPU clamps to representable bounds before signing;
the FPGA never has to second-guess.

**Why is the skew not symmetric at half-ticks?** The skew is
`(q * inv_skew + 2^30) >> 31`: round to nearest, with exact halves rounded
toward plus infinity. So `q = 5, inv_skew = 0.5` gives a skew of 3 ticks while
`q = −5` gives −2. Host and FPGA do the same thing, and test #4 does not assert
strict symmetry across the sign.

---

## Not done

| Item                                          | Note                |
| --------------------------------------------- | ------------------- |
| HMAC check in the silicon build               | left out to close timing; passes in simulation |
| Real PCIe driver replacing `FpgaStub`         | the on-chip design is driven over AXI-Lite today |
| NIC-to-NIC path with a 25G MAC                | market data is host-injected |
| GARCH(1,1) volatility upgrade                 | drop-in for `EwmaVariance` |
| LOBSTER / Dukascopy ingest                    | `sim/replay_main.cpp` |
| Real-money deployment hooks                   | **explicitly out of scope** |

--------------------------------------------- | ------------------- |
| `feed_parser`, `order_book`, `quote_decider`, `order_encoder`, `inventory_counter`, `param_block_loader`, `mac_udp_rx` | VHDL — `fpga/rtl/` (Vivado 2023.2) |
| Vitis HLS synthesis of `pricing_engine_hls.cpp` | `v++ -c --mode hls --part xcvu47p-fsvh2892-2-e` |
| Real PCIe XDMA driver replacing `FpgaStub`    | Phase B host glue   |
| HW-in-the-loop ≤ 2 µs latency test (#12)      | AWS F2 / VU47P bench |
| GARCH(1,1) volatility upgrade                 | drop-in for `EwmaVariance` |
| LOBSTER / Dukascopy ingest                    | `sim/replay_main.cpp` |
| Real-money deployment hooks                   | **explicitly out of scope** |

---

## Citations (per spec §14)

- Avellaneda & Stoikov 2008 — *High-frequency Trading in a Limit Order Book*
- Guéant 2017 — *The Financial Mathematics of Market Liquidity*
- Cartea, Jaimungal & Penalva 2015 — *Algorithmic and High-Frequency Trading*
- Cont, Kukanov & Stoikov 2014 — *The Price Impact of Order Book Events*
- Easley & O'Hara 1987 — *Price, Trade Size, and Information in Securities Markets*
- Stoikov 2018 — *The Micro-Price*

---

*If anything in this README contradicts `docs/MMX_SPECIFICATION.md`, the spec
wins; fix this README.*
