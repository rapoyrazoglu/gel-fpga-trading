# connector — Strategy ↔ Engine SDK

The thin contract that lets trading strategies (PCTA, Market-Maker, …) plug
into the GEL execution engine like Lego pieces. Strategies depend on
`connector`; the engine consumes `connector`. Neither side knows about
the other directly.

## What lives here

| Layer | Header | Purpose |
|---|---|---|
| Wire types | `connector/wire.hpp` | `Order`, `ExecReport`, `Tick`, `BookUpdate`, `CalendarEvent` — POD structs, fixed-point prices, cacheline-aligned |
| Strategy contract | `connector/strategy.hpp` | `IStrategy` abstract base, `OrderSink` interface |
| In-process transport | `connector/ring.hpp`, `connector/sinks.hpp` | Lock-free SPSC ring + sinks for same-process strategies |
| Cross-process transport | `connector/shm_ring.hpp`, `connector/shm_client.hpp` | mmap-backed SPSC ring so a Python strategy in process A can submit orders to a C++ engine in process B |
| Account assignment | `connector/account_map.hpp` | N:M mapping of `StrategyId` ↔ `AccountId`, so multiple strategies can share an account or one strategy can own several |
| FPGA parity | `connector/order_router_native.hpp` | CPU-side shim of the HLS encoder, for bit-exact parity tests |
| Python bindings | `src/bindings.cpp` → `connector._connector_native` | pybind11 module exposing wire types, `PyStrategy`, `ShmClient` |

## How GEL consumes it

GEL's `CMakeLists.txt`:

```cmake
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../connector ${CMAKE_BINARY_DIR}/connector_build)
target_link_libraries(gel_engine PRIVATE connector::connector)
```

`#include "connector/wire.hpp"` and use `connector::Order`, etc.

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Builds the header-only `connector` library and (if pybind11 is available)
the `_connector_native` Python extension, dropped into `python/connector/`.

## Run cross-process demo

```sh
# Terminal A: engine daemon
./build/examples/cross_process/gel_daemon /tmp/orders.shm /tmp/reports.shm

# Terminal B: Python strategy
PYTHONPATH=python python3 examples/cross_process/pcta_proc.py \
    /tmp/orders.shm /tmp/reports.shm
```
