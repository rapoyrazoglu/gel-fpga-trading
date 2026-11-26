# Test status

Measured on this tree on macOS, Apple clang, GHDL 6.0.0. CI repeats the first
three rows on Ubuntu.

| Layer | How to run | Result |
|---|---|---|
| connector | root `ctest` | 2 / 2 |
| Market-Maker host | root `ctest` | 13 / 13 |
| GEL engine demos | root `ctest` with `-DGEL_BUILD_ENGINE=ON` | 2 / 2 |
| RTL simulation | `tools/run_offline_regression.sh` | 23 / 23 |
| GEL `all_tests` | `cd GEL && ../build/GEL/all_tests` | 4 known failures |

## What each layer covers

**connector.** Order-encoding parity over 100,000 random orders, and a
10,000-message round trip through the cross-process ring.

**Market-Maker host.** The specification's acceptance tests: parameter block
size and offsets, HMAC round trip, pricing at zero inventory, skew sign, kill
switch, staleness, fixed-point drift, queue-position model, circuit breaker,
regime overrides, no floating point in synthesizable code, a walk-forward
check that the tuned configuration still works out of sample, and the
`IStrategy` adapter.

**RTL simulation.** Eleven module and pipeline testbenches plus twelve
mutation testbenches that must detect a deliberately broken module.

## Known failures in `all_tests`

`all_tests` is the engine's older strategy-research suite. It is built but not
registered with `ctest`, so it does not gate CI.

| Test | Reason |
|---|---|
| FVG: CSV Config Loading | Needs `ai_results/fvg_analysis_real.csv`, a data file not in the repository |
| Backtest: Basic Functionality | The fair-value-gap strategy opens no trades on the synthetic series |
| Backtest: Equity Curve Tracking | Follows from the above: no trades, empty curve |
| Strategy Parameters Match | EMA momentum profit factor is not above 1 on the synthetic series |

The last three are strategy-logic results on generated data, not engine
defects. They are listed in [CONTRIBUTING.md](../../CONTRIBUTING.md) as open
work. Run the binary from `GEL/`; it opens files by relative path.

## Not tested here

- Nothing runs on an FPGA in CI. The hardware numbers in the README were
  measured by hand on an AWS F2 instance.
- The parameter-block HMAC check is verified in simulation only.
- The rings have no ThreadSanitizer or model-checking run.
