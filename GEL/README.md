# GEL — execution engine

The C++23 engine of [gel-fpga-trading](../README.md). Strategies hand it
orders through the [`connector`](../connector/) SDK; it applies risk checks,
encodes the wire frame, routes to a broker and reports fills back.

```cpp
GelEngine engine;
engine.register_strategy(my_strategy);   // one lock-free ring per strategy
engine.publish_tick(t);                  // fan out market data
engine.run_once();                       // drain rings: risk, encode, route, drop-copy
```

## What is in the engine

All under [`include/gel/platform/`](include/gel/platform/):

| Header | Role |
|---|---|
| `gel_engine.hpp` | Orchestrator: registration, market-data fan-out, `run_once()` |
| `risk_engine.hpp` | Pre-trade checks per strategy and account: size, price band, position, rate, two kill switches |
| `symbol_table.hpp` | Ticker to dense symbol id |
| `account_registry.hpp`, `account_router.hpp` | Many strategies to many broker accounts |
| `order_formatter.hpp` | `Order` to the 56-byte wire frame |
| `order_router.hpp`, `broker_client.hpp` | Dispatch to a broker implementation |
| `drop_copy.hpp` | Per strategy and account PnL; delivers fills to strategies |
| `capital_allocator.hpp` | Optional periodic reallocation of position limits |
| `telemetry.hpp` | A timestamp per order per hop, with percentiles |
| `aws_f2_router.hpp` | Host-side interface for an FPGA broker. There is no bitstream behind it; the hardware result in this repository is the market maker's data plane |

The rest of `include/gel/` is the older strategy-research code: indicators,
pattern detection, a backtester and an OANDA REST client.

## Build

Dependencies: Boost, Eigen, curl, nlohmann_json, fmt, range-v3. From the
repository root:

```sh
cmake -B build -DGEL_BUILD_ENGINE=ON -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build -j && ctest --test-dir build
```

[BUILD.md](BUILD.md) covers platform setup and building this directory on its
own. The two demos need none of the dependencies and are built and run by
`tools/run_offline_regression.sh`:

- `src/scripts/integrated_demo.cpp` — two strategies, several accounts
- `src/scripts/fleet_demo.cpp` — ten strategies on one engine, each with its
  own risk and PnL

## Tests

The demos are registered with `ctest`. The older `all_tests` binary has four
known failures, listed in
[docs/reference/test-status.md](../docs/reference/test-status.md).

## Live trading

`live_trader` and the OANDA client talk to OANDA's practice REST API. Supply
your own practice-account credentials. This path is for experiments; it is
REST over HTTPS and has nothing to do with the latency figures in the root
README.
