# MMX — Market Maker eXecution
## Full Technical Specification (Agent-Programmable Reference, C++ / FPGA-Hybrid)

> **Owner:** Remzi Atasagun Poyrazoğlu (BAU Computer Engineering, Capstone)
> **Sister system:** PCTA (calendar-driven RL signal source) — see PCTA_SPECIFICATION.md
> **Substrate:** GEL (Generic Exchange Layer) — runs on AWS F2 / Xilinx VU47P (UltraScale+)
> **Audience:** Coding agent. Be literal. Implementation language: **C++20** (host) + **VHDL / Vitis HLS C++** (FPGA).

---

## 0. One-paragraph summary

MMX is an **inventory-aware, volatility-adaptive market maker** built on the Avellaneda–Stoikov framework, designed to run as a **split fast-path / slow-path system**. The fast path lives on the FPGA: every tick it computes the reservation price `r = s − q·γ·σ²·(T−t)`, derives bid/ask from a pre-computed half-spread, decides cancel/replace, and emits an order packet — all in ≤ 2 µs tick-to-trade. The slow path lives on the host CPU (C++20): every ~100 ms it re-estimates volatility, re-fits arrival-rate κ, recomputes the half-spread term `(γ·σ²·(T−t)/2 + ln(1+γ/κ)/γ)`, applies regime overrides from PCTA, and DMA-pushes a refreshed parameter block to the FPGA. **Things that change per-tick run on FPGA. Things that change per-second run on CPU.** This split is non-negotiable and dictates every interface in this document.

---

## 1. Architecture overview

```
                         ┌────────────────────────────────────┐
                         │      EXCHANGE (UDP multicast)      │
                         └─────────────┬──────────────────────┘
                                       │  10G/25G SFP+
                                       ▼
   ┌──────────────────────── FPGA (VU47P, AWS F2) ────────────────────────┐
   │                                                                      │
   │   ┌──────────────┐   ┌──────────────┐   ┌──────────────┐            │
   │   │  MAC + UDP   │ → │  FAST/ITCH   │ → │  L2 ORDER    │            │
   │   │  (~40 ns)    │   │  PARSER      │   │  BOOK        │            │
   │   │              │   │  (~120 ns)   │   │  (~80 ns)    │            │
   │   └──────────────┘   └──────────────┘   └──────┬───────┘            │
   │                                                 │                    │
   │                                                 ▼                    │
   │                                        ┌────────────────┐            │
   │                                        │  PRICING       │            │
   │                                        │  ENGINE        │            │
   │                                        │  (~150 ns)     │            │
   │                                        └────────┬───────┘            │
   │                                                 ▼                    │
   │                                        ┌────────────────┐            │
   │                                        │  QUOTE/CANCEL  │            │
   │                                        │  DECIDER       │            │
   │                                        │  (~80 ns)      │            │
   │                                        └────────┬───────┘            │
   │                                                 ▼                    │
   │                                        ┌────────────────┐            │
   │                                        │  ORDER ENC     │            │
   │                                        │  + UDP TX      │            │
   │                                        │  (~100 ns)     │            │
   │                                        └────────────────┘            │
   │                                                                      │
   │   ┌──────────────────────────────────┐                              │
   │   │   PARAM BLOCK (BRAM, dual-port)  │◀── DMA ── from CPU            │
   │   │   γ·σ²·(T-t), δ/2, regime, qty   │                              │
   │   │   max_inv, kill_switch, hmac     │                              │
   │   └──────────────────────────────────┘                              │
   └──────────────────────────────────────────┬───────────────────────────┘
                                              │ PCIe Gen3 x8 (XDMA)
                                              ▼
   ┌──────────────────────────── HOST CPU (C++20) ────────────────────────────┐
   │                                                                            │
   │   Core 0: Fill listener      Core 1: Volatility estimator                  │
   │   Core 2: κ estimator        Core 3: Param compiler & DMA                  │
   │   Core 4: PCTA regime ingest Core 5: Risk supervisor                       │
   │   Core 6: Logger / journal   Core 7: Watchdog / heartbeat                  │
   │                                                                            │
   │   All inter-core: SPSC lock-free queues. Pinned threads. SCHED_FIFO 99.   │
   └────────────────────────────────────────────────────────────────────────────┘
```

**Latency budget (tick-to-trade):**
```
MAC + UDP parse        ~40 ns
Feed parser (FAST)    ~120 ns
Order book update      ~80 ns
Pricing engine        ~150 ns
Quote decider          ~80 ns
Order encode + TX     ~100 ns
─────────────────────────────
Total FPGA              570 ns  (sub-microsecond on chip)
+ Wire (NIC → exchange) ~1 µs
─────────────────────────────
End-to-end             ~1.5 µs   (target: ≤ 2 µs)
```

---

## 2. The split — what runs where, and why

> **The single most important rule in this document.** If you misclassify a computation, you destroy either latency or flexibility.

| Computation | Frequency of change | Where | Why |
|---|---|---|---|
| Mid price `s` | every tick | **FPGA** | Comes from order book, must be live |
| Inventory `q` | every fill | **FPGA** (counter) | Fills arrive on FPGA RX |
| Reservation price `r = s − q·γ·σ²·(T−t)` | every tick | **FPGA** | Pure mul-add, fixed-point, 1 cycle |
| Bid/ask = `r ± δ/2` | every tick | **FPGA** | δ/2 already in BRAM as scalar |
| Cancel/replace decision | every tick | **FPGA** | Threshold compare, 1 cycle |
| Order packet build + TX | when triggered | **FPGA** | Wire-format encoder |
| Volatility σ estimation (EWMA / GARCH) | ~100 ms | **CPU** | Floating-point, time-windowed |
| Arrival-rate κ fit | ~1 s | **CPU** | MLE on order arrivals; offline-style |
| Half-spread term `δ/2 = γσ²(T-t)/2 + ln(1+γ/κ)/γ` | ~100 ms | **CPU** | Contains `ln()`, expensive on FPGA, varies slowly |
| γ (risk aversion) | minutes | **CPU** | Operator/regime-driven |
| `T − t` time-to-horizon | seconds | **CPU** | Wall-clock based |
| Regime override (from PCTA) | minutes | **CPU** | LLM/RL latency is 100s of ms anyway |
| Risk circuit breaker (daily PnL, max-DD) | continuous | **CPU** | Reads journal, asserts kill_switch |
| Logging / journaling | continuous | **CPU** | I/O bound |

**The covenant:** the FPGA **never** computes `ln`, `exp`, `sqrt`, or division by a non-constant. The CPU **never** makes a per-tick decision. Violating either side breaks the system.

---

## 3. Mathematical core

### 3.1 Avellaneda–Stoikov (FPGA evaluates these every tick)

```
Reservation price:   r(s, q, t) = s − q · γ · σ² · (T − t)
Optimal half-spread: δ/2        = ½ · γ · σ² · (T − t)  +  (1/γ) · ln(1 + γ/κ)
Quotes:              bid = r − δ/2
                     ask = r + δ/2
```

Symbols:
- `s` mid price (from order book), `q` signed inventory in lots,
- `γ` risk aversion (e.g. 0.05–0.5), `σ²` realized variance per second,
- `κ` order arrival intensity (per side), `T − t` time-to-horizon (sec).

### 3.2 What the host pre-computes

The host produces a **single packed parameter block** every refresh (~100 ms) and DMAs it into FPGA BRAM. Per tick the FPGA only needs `s` and `q` to derive quotes.

```cpp
// Exactly 64 bytes — fits one cache line, one DMA burst.
struct alignas(64) ParamBlock {
    int32_t  inv_skew_q31;   // = γ · σ² · (T-t)  in Q1.31 fixed-point
    int32_t  half_spread;    // = δ/2 in price ticks
    uint32_t max_inv;        // |q| cap; FPGA pulls quotes if exceeded
    uint32_t base_qty;       // default order size
    uint8_t  regime_id;      // 0=normal, 1=trend_up, 2=trend_down,
                             // 3=high_vol, 4=event_imminent
    uint8_t  kill_switch;    // 1 = cancel all quotes, do nothing
    int16_t  bid_skew_ticks; // regime-driven asymmetry, signed
    int16_t  ask_skew_ticks;
    uint16_t requote_ticks;  // |Δprice| threshold to retrigger replace
    uint64_t epoch_ns;       // for staleness check by FPGA
    uint8_t  hmac[16];       // CPU signs the block; FPGA verifies
    uint8_t  pad[18];
};
static_assert(sizeof(ParamBlock) == 64);
```

The FPGA per-tick math reduces to **3 multiply-adds and 2 sign-extended subtractions**:

```
r      = s − ((int64_t)q * inv_skew_q31) >> 31
bid    = r − half_spread − bid_skew_ticks
ask    = r + half_spread + ask_skew_ticks
```

One DSP slice per multiply, all integer. Fits in a 4 ns cycle.

### 3.3 Volatility estimator (CPU)

Default: **EWMA on log-returns**, λ = 0.94 (RiskMetrics).

```cpp
double sigma2_ewma(double prev, double r_t, double lambda = 0.94) {
    return lambda * prev + (1.0 - lambda) * r_t * r_t;
}
```

Optional upgrade: **GARCH(1,1)**. Daily recalibration of (ω, α, β) via MLE on the previous trading day. The intraday update is a single line.

If you sample at 100 ms, `σ²_per_sec = σ²_per_sample / 0.1`.

### 3.4 Arrival-rate κ estimator (CPU)

Following Cont–Kukanov–Stoikov:

1. Bin order arrivals by distance `δ` from mid (in ticks) per side, over a 60-second rolling window.
2. For each bin, count arrivals per second → empirical `λ(δ)`.
3. Fit `λ(δ) = A · exp(−κ · δ)` by ordinary least-squares on `log λ`.
4. Smooth with a 5-update median filter to suppress jitter.

### 3.5 Cancel/replace policy (FPGA)

Every tick the FPGA holds the *currently outstanding* `(bid_old, ask_old, qty_old)` in BRAM. After computing `(bid, ask)`:

```
need_replace =
      |bid − bid_old|  > requote_ticks   ||
      |ask − ask_old|  > requote_ticks   ||
      now − last_quote > MAX_QUOTE_AGE_NS ||
      |q|              > max_inv          ||
      kill_switch
```

`requote_ticks` lives in `ParamBlock`. `MAX_QUOTE_AGE_NS` is a synthesis-time constant (e.g. 50 ms).

---

## 4. FPGA modules (VHDL / Vitis HLS)

All modules pipelined at 250 MHz (4 ns clock). Each consumes/produces an AXI-Stream of records.

### 4.1 `mac_udp_rx` — line-rate ingress
- 10G/25G MAC IP (Xilinx) → UDP demux → AXI-Stream of payloads.
- No CPU involvement; jumbo frames disabled (latency over throughput).

### 4.2 `feed_parser` — wire format → normalized
- Supports FAST/ITCH/PITCH (pick one for capstone — ITCH is cheapest to implement).
- Output record: `{ ts_ns, ticker_id, side, price_ticks, size, msg_type }`.
- State machine + lookup tables in BRAM.

### 4.3 `order_book` — top-N levels
- N = 8 levels per side (sufficient for mid-price; tunable).
- Storage: 2 × N × {price, qty} in distributed RAM for single-cycle access.
- On each delta: shift levels, update mid `s = (best_bid + best_ask) / 2`.
- Output: `{ ts_ns, mid_ticks, spread_ticks, imbalance_q15 }`.

### 4.4 `pricing_engine` — the math
- Inputs: `s` (from book), `q` (from inventory counter), `ParamBlock` (from BRAM).
- 1-cycle pipeline: multiply (`q × inv_skew`), shift, subtract, add half_spread + skew.
- Output: `{ bid_ticks, ask_ticks, qty }`.
- HLS C++ sketch:

```cpp
// Vitis HLS — synthesizable
#pragma HLS pipeline II=1
struct QuoteOut { int32_t bid; int32_t ask; uint32_t qty; };

QuoteOut compute_quote(int32_t s, int32_t q, const ParamBlock& p) {
#pragma HLS inline
    int64_t skew = (int64_t)q * p.inv_skew_q31;
    int32_t r    = s - (int32_t)(skew >> 31);
    int32_t hs   = p.half_spread;
    QuoteOut out;
    out.bid = r - hs - p.bid_skew_ticks;
    out.ask = r + hs + p.ask_skew_ticks;
    out.qty = p.base_qty;
    return out;
}
```

### 4.5 `quote_decider` — cancel/replace gate
- Holds `(bid_old, ask_old, last_quote_ns)` in registers.
- Implements the policy in §3.5.
- Output: optionally `{ cancel_old, place_new(bid, ask, qty) }` actions.

### 4.6 `order_encoder` — wire format out
- Two modes: cancel-replace combined (preferred where exchange supports), or cancel-then-place.
- Pre-built header template in BRAM; only price/qty fields are mutated → single-cycle build.

### 4.7 `inventory_counter` — fill-driven
- RX path also receives our own fill confirmations.
- Increments / decrements `q` atomically (single writer). **Saturates** at `±max_inv`.
- Mirrors `q` to a CPU-readable AXI register every fill (no polling required).

### 4.8 `param_block_loader` — DMA receiver
- AXI-Lite slave: CPU writes 64 bytes + HMAC.
- On valid HMAC + non-stale `epoch_ns` (within 500 ms), atomic-swaps the active `ParamBlock`.
- On HMAC fail or staleness > 500 ms: assert `kill_switch` automatically.

---

## 5. Host-side modules (C++20)

### 5.1 Threading model
- One thread per logical role, pinned to a dedicated core, isolated via `isolcpus`.
- Inter-thread: **SPSC lock-free ring buffers** (e.g. `boost::lockfree::spsc_queue` or hand-rolled).
- IRQ steering: PCIe interrupts pinned to Core 0 (fill listener).

### 5.2 Files / classes

```
mmx/
├── host/
│   ├── main.cpp
│   ├── threads/
│   │   ├── fill_listener.{h,cpp}      // Core 0
│   │   ├── vol_estimator.{h,cpp}      // Core 1
│   │   ├── kappa_estimator.{h,cpp}    // Core 2
│   │   ├── param_compiler.{h,cpp}     // Core 3
│   │   ├── pcta_ingest.{h,cpp}        // Core 4
│   │   ├── risk_supervisor.{h,cpp}    // Core 5
│   │   ├── logger.{h,cpp}             // Core 6
│   │   └── watchdog.{h,cpp}           // Core 7
│   ├── ipc/
│   │   ├── pcie_dma.{h,cpp}           // XDMA wrapper
│   │   ├── param_block.h              // shared with FPGA
│   │   └── hmac.{h,cpp}
│   ├── math/
│   │   ├── ewma.h
│   │   ├── garch.{h,cpp}              // optional
│   │   └── kappa_fit.{h,cpp}
│   ├── risk/
│   │   ├── pnl_tracker.{h,cpp}
│   │   └── circuit_breaker.{h,cpp}
│   ├── util/
│   │   ├── spsc_queue.h
│   │   ├── pin.{h,cpp}
│   │   └── tsc_clock.h
│   └── config.toml
└── fpga/
    ├── hls/
    │   ├── pricing_engine.cpp
    │   └── quote_decider.cpp
    └── rtl/
        ├── order_book.vhd
        ├── feed_parser.vhd
        ├── inventory_counter.vhd
        ├── param_block_loader.vhd
        └── order_encoder.vhd
```

### 5.3 Param-compiler thread (the heart of the slow path)

```cpp
// param_compiler.cpp — every 100 ms
void ParamCompiler::run() {
    pin_to_core(3);
    set_realtime_priority(99);

    while (running_.load(std::memory_order_acquire)) {
        wait_until_next_tick(100ms);

        double sigma2 = vol_q_.snapshot();           // EWMA σ² per sec
        double kappa  = kappa_q_.snapshot();         // arrival rate
        double Tt     = horizon_seconds_remaining();
        double gamma  = current_gamma_.load();       // regime-adjusted

        double inv_skew = gamma * sigma2 * Tt;
        double half_sp  = 0.5 * gamma * sigma2 * Tt
                        + std::log1p(gamma / kappa) / gamma;

        ParamBlock p{};
        p.inv_skew_q31    = to_q31(inv_skew);
        p.half_spread     = ticks_from_price(half_sp);
        p.max_inv         = current_max_inv_;
        p.base_qty        = current_base_qty_;
        p.regime_id       = pcta_regime_.load();
        p.kill_switch     = circuit_breaker_.tripped() ? 1 : 0;
        p.bid_skew_ticks  = regime_table_[p.regime_id].bid_skew;
        p.ask_skew_ticks  = regime_table_[p.regime_id].ask_skew;
        p.requote_ticks   = current_requote_threshold_;
        p.epoch_ns        = now_ns();
        sign_hmac(p);

        if (validate_bounds(p)) {
            dma_.write_param_block(p);
        } else {
            logger_.error("param block out of bounds, skipped");
        }
    }
}
```

### 5.4 PCTA regime ingest

PCTA emits a regime hint over a UNIX domain socket or shared-memory mailbox. Ingest is non-blocking; the latest value wins. Mapping:

```
PCTA regime          → (bid_skew, ask_skew) in ticks   γ multiplier
─────────────────────────────────────────────────────────────────────
"ranging"            → (0, 0)                          1.0
"trend_up"           → (-1, +2)                        1.2
"trend_down"         → (-2, +1)                        1.2
"high_vol"           → (-1, +1)                        2.5
"event_imminent"     → kill_switch = 1                 —
```

PCTA latency is hundreds of ms, which is fine — regime overrides are a slow-path concern.

### 5.5 Risk supervisor

- Computes daily PnL from the journal in real time.
- Trips `circuit_breaker_` if any of:
  - daily PnL < `−DAILY_LOSS_LIMIT`
  - rolling-5-min PnL < `−INTRADAY_LIMIT`
  - inventory `|q| > max_inv` for > 30 s
  - param refresh stalled > 1 s (watchdog signal)
- When tripped: writes a `kill_switch=1` ParamBlock immediately, then alerts.

---

## 6. Fixed-point conventions

Use **Q1.31** for the skew coefficient and **integer ticks** for prices. Reasons: deterministic on FPGA, no rounding drift, no DSP for floating-point.

| Quantity | Format | Range | Notes |
|---|---|---|---|
| Price | `int32_t` ticks | full | tick size from instrument config |
| Inventory `q` | `int32_t` | ±2³¹ | saturates at `max_inv` |
| `inv_skew = γσ²(T−t)` | Q1.31 → `int32_t` | ±1.0 | host clamps |
| `half_spread` | `int32_t` ticks | full | host computes |

The host **always clamps** computed values to representable ranges before DMA; the FPGA verifies the HMAC before applying.

---

## 7. IPC: host ↔ FPGA

### 7.1 Channels
- **C2F params:** AXI-Lite write of 64-byte `ParamBlock` (atomic via shadow register + commit bit).
- **F2C events:** PCIe interrupt + ring buffer in host memory (DMA write):
  - fill events `{ts_ns, side, price, qty}`,
  - quote-out events (for journaling),
  - error / HMAC-fail events.
- **C2F kill:** AXI-Lite single-bit register `force_kill` for emergency.

### 7.2 HMAC
- 16-byte truncated HMAC-SHA256 over the first 48 bytes of `ParamBlock` with a 256-bit key burned in at synthesis.
- Defends against bad pointer writes and stack-corruption-driven garbage params.

### 7.3 Staleness
- FPGA tracks `now_ns − epoch_ns`. If > 500 ms → auto-kill.
- Host watchdog must beat at ≤ 100 ms; if it doesn't, supervisor trips.

---

## 8. Build / deploy

### 8.1 Toolchain
- Host: GCC 12 / Clang 16, `-O3 -march=native -fno-omit-frame-pointer -flto`.
- FPGA: Vivado/Vitis 2025.2 (FPGA Developer AMI v1.19.1), Vitis HLS for §4.4 and §4.5; rest in VHDL.
- AWS F2 AFI build: AGFI registration against the F2 shell.
  TODO: confirm the F2 shell version - "Shell 1.4+" was the F1 figure. The F2
  shell differs materially (2 user clocks vs F1’s 8, HBM instead of DDR4).

### 8.2 OS tuning
- Kernel: 5.15 LTS, `tickless`, `nohz_full=1-7`, `isolcpus=1-7`.
- Hugepages (1 GB) for DMA buffers.
- IRQ pinning per §5.1.
- C-states disabled, P-state set to performance, turbo off (jitter > speed).

### 8.3 Repo layout (top-level)

```
mmx/
├── host/      (C++20 — see §5.2)
├── fpga/      (RTL + HLS — see §5.2)
├── sim/       (offline simulator, fill model, replay)
├── tests/     (unit + integration + leakage)
├── tools/     (param compiler offline, journal viewer, risk dashboard)
├── docs/
│   └── MMX_SPECIFICATION.md  (this file)
└── CMakeLists.txt
```

---

## 9. Backtest / simulation

### 9.1 The problem
A retrospective backtest of a market maker is **always too optimistic** unless queue position and adverse selection are modeled. The simulator must:

1. Replay tick-level L2 data (Dukascopy / TickData / LOBSTER).
2. For each of our quotes, simulate **fill probability**:
   - Same-side flow within `[t, t+ε]` and price ≤ our level → check queue position.
   - Queue-position model: cumulative same-price volume from book joined at quote time minus subsequent same-side cancels.
3. Apply **toxic-flow asymmetry**: when adverse moves precede our fill, mark fill toxic and add slippage penalty.
4. Account for maker rebates and taker fees per venue tier.

### 9.2 Simulator interface

```cpp
class MMXSimulator {
public:
    void load_lob(std::string_view path);
    void set_strategy(std::unique_ptr<MMXStrategy>);
    SimResult run(SimParams);
};

struct SimResult {
    double pnl_total;
    double sharpe;
    double sortino;
    double max_drawdown;
    int    fills;
    double fill_rate_bid, fill_rate_ask;
    double inventory_l1_norm;     // Σ |q| dt
    double avg_quote_age_ms;
    double toxic_fill_share;
};
```

### 9.3 Reference targets (sanity)

For a single liquid instrument (e.g. EUR/USD on a tier-1 ECN), 1 day, base_qty 1 lot:

```
Trade count       : 500–5000
Sharpe (gross)    : 2.0–5.0
PnL/trade (gross) : 0.1–0.3 ticks
Max |q| excursion : ≤ 5–10 % of turnover
Win rate          : 55–65 %
```

If results are wildly outside these bounds, the simulator probably has queue-position or fee modeling wrong.

---

## 10. Acceptance tests

Mandatory test suite (the agent must implement all of these):

1. **`test_paramblock_size`** — `sizeof(ParamBlock) == 64`, `alignof == 64`.
2. **`test_hmac_roundtrip`** — sign + verify 10 000 random blocks; zero failures.
3. **`test_pricing_invariant_q0`** — when `q == 0`, FPGA-mode pricing returns symmetric quotes around `s` with half-spread `δ/2` exactly.
4. **`test_pricing_skew_sign`** — `q > 0` produces `bid+ask < 2s` (skewed down to encourage selling); `q < 0` produces the opposite.
5. **`test_kill_switch_pulls`** — set `kill_switch = 1`; FPGA stub must emit cancel-only and never a place.
6. **`test_paramblock_staleness`** — supply `epoch_ns` 600 ms old; FPGA stub trips kill automatically.
7. **`test_fixed_point_no_drift`** — replay 10⁶ ticks; integer pricing matches double-precision reference within ±1 tick.
8. **`test_simulator_queue_pos`** — synthetic LOB where our order is at queue position 5; with 4 lots of same-side flow ahead, we must not fill.
9. **`test_circuit_breaker_trips`** — feed a synthetic loss > limit; supervisor writes kill block within 50 ms.
10. **`test_pcta_regime_overrides`** — push `event_imminent`; param compiler outputs `kill_switch = 1` in the next refresh.
11. **`test_no_floating_point_in_fpga`** — static check (grep / synth report) that no `float` / `double` survives in synthesizable HLS.
12. **`test_end_to_end_latency`** — hardware-in-the-loop: synthetic UDP tick → measured order TX in ≤ 2 µs (p99).

---

## 11. Known pitfalls (do not repeat these)

1. **`ln(1 + γ/κ)` on FPGA.** Don't. Pre-compute on CPU. The instinct to "just put it all on the chip" is wrong here — κ moves on second-scale, the term is constant from the FPGA's POV.
2. **Floating-point on FPGA.** Synthesizes, but inflates resource use 5–10× and ruins timing. Q1.31 is more than enough precision for prices in ticks.
3. **Quote refresh storm.** Without `requote_ticks` threshold, every micro-jitter in `s` triggers a cancel-replace pair. Exchange will rate-limit you — and *fee* you for excessive messaging.
4. **Adverse selection in backtests.** A naive backtest that assumes 100 % fill at quoted price will report Sharpe 8+. Reality: 1.5–4. Implement queue-position modeling or your numbers are a fantasy.
5. **Dividend-adjusted close (equity case).** If you ever extend to equities, never use dividend-adjusted close — it deletes the very signal you'd care about (see PCTA spec §12.4).
6. **PCTA latency assumption.** PCTA runs in Python with LLM in the loop — its regime updates can take seconds. Treat its output as a slow-path advisory, never a per-tick gate.
7. **Inventory counter wraparound.** `q` must saturate, not wrap. A wrap from `+max_inv` to `−max_inv` would invert your skew and turn the MM into a momentum chaser.
8. **HMAC key in source.** The HMAC key gets baked into the bitstream; never check it into git. Use a `key.local.h` excluded by `.gitignore` and enforce in CI.
9. **Logging on the fast path.** Don't `printf` from the param compiler — log via SPSC queue to the logger thread. A blocking write can stall a 100 ms refresh into a 50 ms one and trigger your own staleness kill.
10. **Single-instrument hard-coding.** Even if capstone scope is one instrument, parameterize ticker / tick-size from config. Multi-instrument extension should be a config change, not a refactor.

---

## 12. Out of scope

- Cross-venue MM (multiple exchanges, latency arbitrage).
- Options market making (Greeks, hedging legs).
- Order-routing optimization across maker/taker tiers.
- Self-trade prevention (delegated to exchange where supported).
- Real-money deployment hooks.

---

## 13. Build order (suggested)

1. `host/ipc/param_block.h` + `host/ipc/hmac.cpp` → tests #1, #2.
2. `fpga/hls/pricing_engine.cpp` (HLS + C++ unit test) → tests #3, #4.
3. `fpga/rtl/order_book.vhd` with synthetic L2 stimulus.
4. `fpga/rtl/param_block_loader.vhd` → tests #5, #6.
5. `host/threads/vol_estimator.cpp`, `kappa_estimator.cpp`, `param_compiler.cpp` (in-process, no DMA yet) — driving the HLS module from a C++ harness.
6. `sim/` — replay-driven simulator with queue-position model → tests #7, #8.
7. `host/risk/circuit_breaker.cpp` + `risk_supervisor.cpp` → test #9.
8. `host/threads/pcta_ingest.cpp` → test #10.
9. `fpga/rtl/feed_parser.vhd` + `mac_udp_rx` (or use Xilinx IPs) → test #12 with HW-in-the-loop.
10. End-to-end on AWS F2.

---

## 14. Reference citations the agent can use

- Avellaneda & Stoikov 2008 — *High-frequency Trading in a Limit Order Book*.
- Guéant 2017 — *The Financial Mathematics of Market Liquidity*.
- Cartea, Jaimungal & Penalva 2015 — *Algorithmic and High-Frequency Trading*.
- Cont, Kukanov & Stoikov 2014 — *The Price Impact of Order Book Events*.
- Easley & O'Hara 1987 — *Price, Trade Size, and Information in Securities Markets*.
- Stoikov 2018 — *The Micro-Price* (alternative mid).

---

**End of spec.** If anything here contradicts the codebase, the codebase is wrong; fix the codebase.
