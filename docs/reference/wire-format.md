# Wire format

Defined in [`connector/include/connector/wire.hpp`](../../connector/include/connector/wire.hpp)
and [`order_router_native.hpp`](../../connector/include/connector/order_router_native.hpp).
Sizes and layout properties are enforced with `static_assert`.

## Conventions

- Host structs are little-endian, packed, and aligned to their own size.
- Prices are `int64` ticks: `price x 10^8`. `-1` means a market order.
- Timestamps are `uint64` nanoseconds.
- `0` is the invalid value for `SymbolId` and `AccountId`.

## `Order` — strategy to engine, 64 bytes

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 8 | `ts_ns` | |
| 8 | 8 | `client_order_id` | For REPLACE, the new id |
| 16 | 4 | `strategy_id` | Below 32 |
| 20 | 4 | `symbol_id` | |
| 24 | 8 | `price_ticks` | `-1` for market and cancel |
| 32 | 8 | `quantity` | Signed |
| 40 | 1 | `side` | 0 buy, 1 sell |
| 41 | 1 | `ord_type` | 0 market, 1 limit, 2 IOC, 3 FOK, 4 post-only, 5 cancel, 6 replace |
| 42 | 1 | `tif` | 0 day, 1 IOC, 2 FOK, 3 GTC |
| 43 | 1 | `flags` | bit 0 hidden, bit 1 iceberg, bit 2 reduce-only |
| 44 | 4 | `risk_budget_bps` | |
| 48 | 4 | `account_id` | Host routing only; not sent on the wire |
| 52 | 8 | `ref_order_id` | Cancel and replace: the order acted on |
| 60 | 4 | reserved | |

## `ExecReport` — engine to strategy, 64 bytes

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 8 | `ts_ns` | |
| 8 | 8 | `client_order_id` | |
| 16 | 8 | `exchange_order_id` | |
| 24 | 4 | `strategy_id` | |
| 28 | 4 | `symbol_id` | |
| 32 | 8 | `fill_price_ticks` | |
| 40 | 8 | `fill_quantity` | |
| 48 | 1 | `status` | 0 accepted, 1 rejected, 2 partial, 3 filled, 4 canceled, 5 expired |
| 49 | 1 | `reject_reason` | 1 fat finger, 2 position limit, 3 throttle, 4 unknown symbol, 5 kill switch, 6 bad price, 7 bad quantity, 8 stale |
| 50 | 1 | `side` | |
| 51 | 4 | `account_id` | |
| 55 | 9 | reserved | |

## Other structs

| Struct | Size | Purpose |
|---|---|---|
| `Tick` | 32 | Top of book: bid, ask, per-symbol sequence number |
| `BookUpdate` | 192 | Five price levels per side |
| `CalendarEvent` | 64 | Scheduled event with consensus, actual and previous values |
| `TelemetryRecord` | 64 | One timestamp per order per processing hop |

## Order frame on the wire — 56 bytes, big-endian

An OUCH-style layout. It is fixed and deterministic so hardware and software
encoders can be compared byte for byte; it is not a certified OUCH
implementation.

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Message type: `O` enter, `X` cancel, `U` replace |
| 1 | 1 | `side` |
| 2 | 1 | `ord_type` |
| 3 | 1 | `tif` |
| 4 | 8 | `client_order_id` |
| 12 | 4 | `strategy_id` |
| 16 | 4 | `symbol_id` |
| 20 | 8 | `price_ticks` |
| 28 | 8 | `quantity` |
| 36 | 8 | `ts_ns` |
| 44 | 1 | `flags` |
| 45 | 3 | reserved, zero |
| 48 | 8 | `ref_order_id`, zero for enter |

## Ring capacities

| Ring | Slots | Payload |
|---|---|---|
| Order ring, per strategy | 65,536 | `Order` |
| Event ring, per strategy | 16,384 | `ExecReport` |

One slot is kept empty to tell full from empty, so usable capacity is one less.
The cross-process ring file is a 64-byte head index, a 64-byte tail index, then
the slots. It has no magic number or version field.
