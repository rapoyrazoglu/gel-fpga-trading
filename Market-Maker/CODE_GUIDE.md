# MMX — Code Map (defense guide: "where did you do X?")

Every major piece, with file:line so you can open it on the spot.

## "Where did you set up the registers?" (host <-> FPGA interface)
| What | File : line |
|---|---|
| Register MAP (addresses ADDR_S..ADDR_LATENCY) | `fpga/cl/cl_mmx_pricing_defines.vh:28-53` |
| Host WRITE -> FPGA regs (`case (write_addr)`) | `fpga/cl/cl_mmx_pricing.sv:258` (e.g. line 259 `ADDR_S : reg_s <= wdata; tick_strobe<=1`) |
| FPGA regs -> host READ (`case (araddr)`) | `fpga/cl/cl_mmx_pricing.sv:282` |
| Host side C driver (poke/peek over PCIe) | `fpga/cl/mmx_fpga_test_v3.c` — `wr()`/`rd()` = `fpga_pci_poke/peek` |

The registers are 32-bit words on the OCL AXI-Lite BAR: the host writes the tick + parameters,
the FPGA computes, the host reads bid/ask/qty/cancel + telemetry (spread_add, vol, hard_kill, latency).

## "Where's the pricing algorithm (the math)?"
| What | File : line |
|---|---|
| **compute_quote()** — Avellaneda-Stoikov, Q1.31 fixed-point (single source of truth) | `host/pricing/pricing_engine.h:41` |
| The formula (r = s - skew; bid = r - half_spread - bid_skew) | `pricing_engine.h:5-7`, body `:58-72` |
| HLS synthesizable top (`mmx_pricing_engine`) — same header | `fpga/hls/pricing_engine_hls.cpp` |
| On-silicon 4-stage pipeline (stage 1..4) | `fpga/cl/cl_mmx_pricing.sv:391` (S1 clamp) → `:408` (S2 mult) → `:412` (S3 skew/r) → `:416` (S4 bid/ask+sat) |
| VHDL mirror (for simulation) | `fpga/rtl/pricing_engine_beh.vhd` |

## "Where did you do OBI / volatility / circuit breaker?"
| Feature | On-silicon (SV) | VHDL reference module |
|---|---|---|
| Order-Book Imbalance skew | `cl_mmx_pricing.sv:318` | `fpga/rtl/obi_skew.vhd` |
| Volatility-adaptive spread (EWMA) | `cl_mmx_pricing.sv:330` | `fpga/rtl/vol_ewma.vhd` |
| Hardware circuit breaker (drop-circuit) | `cl_mmx_pricing.sv:350` | `fpga/rtl/circuit_breaker_hw.vhd` |
| On-chip latency counter | `cl_mmx_pricing.sv:367` | — |

## "Where's the full data plane?" (VHDL, simulation-verified)
`fpga/rtl/` : `mac_udp_rx.vhd` (NIC ingress) -> `feed_parser.vhd` (ITCH-lite) -> `order_book.vhd`
(8-level book) -> pricing -> `quote_decider.vhd` (cancel/replace) -> `order_encoder.vhd` /
`ouch_encoder.vhd` (OUCH out). Plus `inventory_counter.vhd`, `param_block_loader.vhd`,
top-level `mmx_dataplane.vhd`. Shared types/contract: `mmx_pkg.vhd`.

## "Where's the security / HMAC?"
| What | File |
|---|---|
| SHA-256 core (FIPS 180-4) | `fpga/rtl/sha256_core.vhd` |
| HMAC verifier (RFC 2104) | `fpga/rtl/hmac_verify.vhd` |
| Host signing (the reference) | `host/ipc/hmac.cpp`, `host/ipc/sha256.cpp` |
| ParamBlock 64-byte layout | `host/ipc/param_block.h` |

## "Where's the slow path (CPU strategy)?"
`host/threads/param_compiler.cpp` — every 100 ms computes the ParamBlock (inv_skew, half_spread)
from EWMA sigma^2 (`host/math/ewma.h`) + kappa arrival rate (`host/math/kappa_fit.cpp`) + regime.

## "How did you test / prove it?"
| Proof | File |
|---|---|
| Host<->FPGA pricing parity (500 vectors) | `fpga/rtl/tb_pricing_parity.vhd` |
| L2 "tester's test" (proves the test catches bugs) | `fpga/rtl/tb_pricing_parity_l2.vhd` + `pricing_engine_beh_buggy.vhd` |
| On-silicon parity (runs on the real FPGA) | `fpga/cl/mmx_fpga_test.c` / `mmx_fpga_test_v3.c` |
| Full data-plane sim | `fpga/rtl/tb_mmx_dataplane.vhd` |
| HMAC vs host (+ tamper reject) | `fpga/rtl/tb_hmac.vhd`, `tb_param_load.vhd` |
| Backtest / no-overfit | `sim/cv_harness.cpp`, `sim/lob_simulator.cpp` |
| Fake-exchange -> FPGA demo | `sim/fake_exchange_fpga_test.cpp` |

## Multi-asset + protocol (enhancements)
`fpga/rtl/pricing_engine_multi.vhd` (N parallel pricing lanes, `mmx_multi_pkg.vhd`),
`fpga/rtl/ouch_encoder.vhd` (Nasdaq-OUCH-style order frames), `fpga/rtl/hw_backtester.vhd`
(BRAM tick replay + PnL accumulate).

## One-line data flow to say out loud
"Host writes a tick + Avellaneda-Stoikov parameters to AXI-Lite registers over PCIe
(`cl_mmx_pricing.sv:258`); the 4-stage pricing pipeline (`:391`) plus OBI/vol/breaker compute a
quote in hardware; the host reads bid/ask back (`:282`) — verified bit-exact against the C++
`compute_quote` (`pricing_engine.h:41`) on the real VU47P."
