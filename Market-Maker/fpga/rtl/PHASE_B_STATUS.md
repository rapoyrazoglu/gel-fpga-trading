# MMX Phase-B FPGA data plane — status

Date: 2026-06-29. Authored + **verified in simulation with GHDL on macOS** (no instance needed).

## What exists now (`fpga/rtl/`)

| File | Role |
|---|---|
| `mmx_pkg.vhd` | shared types/contract (records, subtypes, helpers) |
| `mac_udp_rx.vhd` | §4.1 Eth/IP/UDP header strip → payload AXIS |
| `feed_parser.vhd` | §4.2 ITCH-lite → `feed_msg_t` |
| `order_book.vhd` | §4.3 8-level dual book → mid/spread/imbalance |
| `inventory_counter.vhd` | §4.7 fill-driven signed inventory, saturating |
| `pricing_engine_beh.vhd` | §4.4 VHDL mirror of `compute_quote()` (parity) |
| `quote_decider.vhd` | §4.5/§3.5 cancel/replace gate (mirror `needs_replace`) |
| `order_encoder.vhd` | §4.6 OUCH-like TX frame builder |
| `param_block_loader.vhd` | §4.8 AXI-Lite param load + staleness + **real HMAC** kill |
| `sha256_core.vhd` | **FIPS 180-4 SHA-256** (1 round/cycle) — NEW |
| `hmac_verify.vhd` | **REAL HMAC-SHA256** verifier (replaced the placeholder) — NEW |
| `mmx_dataplane.vhd` | top: wires the full pipeline |
| `tb_mmx_dataplane.vhd` | full-pipeline self-checking testbench |
| `tb_sha256/tb_hmac/tb_param_load/tb_pricing_parity.vhd` | unit/parity testbenches — NEW |
| `pricing_vectors.txt` | 500 C++ `compute_quote` vectors (parity fixture) — NEW |
| `build_sim.tcl` | xsim + Vivado synth-check flow (for the instance) |

≈ 2,700 lines VHDL-2008.

## Offline verification (GHDL 6.0.0, `--std=08`) — all green

- **Analyze: all files OK** (one real bug found+fixed in `feed_parser` byte-concat).
- **Elaborate: OK** — full `mmx_dataplane` hierarchy links and type-checks.
- **Simulation results:**

| Testbench | Result |
|---|---|
| `tb_sha256` | **PASS** — `SHA256("abc")=ba7816bf…f20015ad` (FIPS vector) |
| `tb_hmac` | **PASS** — valid host tag accepted; tampered tag rejected |
| `tb_param_load` | **PASS** — host `sign_param_block` authenticated (err=00, kill=0); tampered → HMAC-fail kill |
| `tb_pricing_parity` | **PASS** — `pricing_engine_beh` == host `compute_quote()` **BIT-EXACT on 500 random vectors** |
| `tb_mmx_dataplane` | **PASS** — fake ITCH → header-strip → book → fill → price → **TX order frame** (bid=100<ask=110, q=10), now gated by the **real HMAC** authenticating a host-signed ParamBlock |

The HMAC core is proven against the host crypto: `hmac_verify` reproduces `host/ipc/hmac.cpp`
exactly (key 0xab*32, 48-byte block → tag `37439f6f…0c34c`), and the pricing RTL is proven
bit-identical to `host/pricing/pricing_engine.h`.

## Review verdicts (3 adversarial agents)

- **Interfaces: PASS** — all top-level port maps & record fields consistent.
- **Synthesizability: no blockers** — single clock, full sync resets, no inferred latches.
- **Spec/parity: cores bit-for-bit faithful** to the host source of truth.

## Remaining work (punch list)

1. **Instance: Vivado synth + place&route** for VU47P (`xcvu47p-fsvh2892-2-e`) → post-route
   timing + resources; GHDL proves logic/parity, not timing closure. Run `build_sim.tcl` (xsim)
   then `synth_check.tcl`.
2. Production HMAC **key** from eFuse/secure store (now a constant matching the host dev key).
3. For the real build, swap `pricing_engine_beh` for the HLS-exported `mmx_pricing_engine` IP
   (already C-synth'd + P&R'd: 2.919 ns, 4 DSP/1022 LUT) — same bit-exact source.
4. Minor hardening (review low items): pricing 64×64→34×32 multiply, order_encoder same-cycle
   corner, param_block_loader sticky stale-error, feed backpressure.
5. AFI build + load on silicon — gated on AWS creds/S3/IAM.

## Honest scope

The Phase-B data plane is **authored and simulation-verified end-to-end offline**, including a
real, host-parity HMAC-SHA256 authentication core and bit-exact pricing. What remains needs the
instance: Vivado synthesis/timing closure and the AWS AFI flow.
