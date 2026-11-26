# Write your first strategy

By the end you will have built the repository, run a strategy that sends one
order and receives one fill, and seen where it plugs into the engine. You need
CMake 3.22 or newer and a C++20 compiler. Nothing else.

## 1. Build and run the skeleton

```sh
cmake -B build && cmake --build build -j
./build/connector/connector_cpp_strategy_skeleton
```

It prints:

```
skeleton: orders_sent=1 fills=1
```

The source is [`connector/examples/cpp_strategy_skeleton.cpp`](../../connector/examples/cpp_strategy_skeleton.cpp).
Keep it open for the next steps.

## 2. Read the three parts of a strategy

A strategy is a class that implements `connector::IStrategy`.

**It keeps the door it was given.** The engine calls `on_start` once and hands
over an `OrderSink`. That sink is the only way out.

```cpp
void on_start(OrderSink& sink) override { sink_ = &sink; }
```

**It reacts to market data.** Override only the callbacks you need: `on_tick`,
`on_book`, `on_event`. The skeleton waits for a high-impact FOMC event.

**It sends an order by filling a 64-byte struct.**

```cpp
Order o{};
o.client_order_id = ++next_coid_;
o.strategy_id     = static_cast<uint32_t>(id());
o.symbol_id       = symbol_;
o.quantity        = 1000;
o.side            = static_cast<uint8_t>(Side::BUY);
o.ord_type        = static_cast<uint8_t>(OrdType::MARKET);
o.price_ticks     = -1;                 // -1 means market
if (sink_->send(o)) fired_ = true;      // send never blocks; false means the ring is full
```

Fills come back through `on_fill`.

## 3. Change something

In `on_event`, turn the market order into a limit order one tick below the
last bid. Prices are integers: `price * 10^8`.

```cpp
o.ord_type    = static_cast<uint8_t>(OrdType::LIMIT);
o.price_ticks = to_ticks(1.0750) - 1;
```

Rebuild and run again. The output is the same, because the demo harness fills
every order. That harness is the next thing to replace.

## 4. Run it under the real engine

The skeleton uses `DirectOrderSink`, a test sink that calls back immediately.
Under the engine the same class gets a lock-free ring instead, and its orders
pass through risk checks, encoding and routing. The strategy code does not
change.

```cpp
GelEngine engine;
const auto sym = engine.symbols().register_symbol("EURUSD", Venue::OANDA, 100, 1);
engine.register_strategy(std::make_shared<MyStrategy>(sym, /*account=*/0));

engine.publish_tick(t);   // fans out to every registered strategy
engine.run_once();        // drains the rings: risk, encode, route, drop-copy
```

Each strategy needs its own `id()`, below 32. The engine uses it to keep risk
limits, PnL and audit separate per strategy.

A complete example with ten strategies on one engine is
[`GEL/src/scripts/fleet_demo.cpp`](../../GEL/src/scripts/fleet_demo.cpp). It
has no third-party dependencies; `tools/run_offline_regression.sh` builds and
runs it.

## Where next

- The fields of `Order` and the frame it becomes: [wire format](../reference/wire-format.md)
- What happens between `send` and the wire: [architecture](../explanation/architecture.md)
- A strategy in another process, or in Python: [`connector/README.md`](../../connector/README.md)
