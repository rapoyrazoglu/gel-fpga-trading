# Architecture

The system splits along one line: **what must happen on every market event**
and **what can wait a second**.

```
                    slow path (host CPU, per second)
        volatility estimate, arrival-rate fit, regime, risk supervisor
                              │
                              ▼  64-byte signed parameter block
   ┌──────────────────── fast path (FPGA, per event) ───────────────────┐
   │ ITCH in ─▶ mac_udp_rx ─▶ feed_parser ─▶ order_book ─▶ pricing      │
   │                                                          │         │
   │           OUCH out ◀─ order_encoder ◀─ quote_decider ◀───┘         │
   └────────────────────────────────────────────────────────────────────┘
```

The per-event work is a handful of integer operations, so it fits in a fixed
number of clock cycles. Everything expensive — square roots, regressions,
anything in floating point — runs on the host and reaches the chip as a few
precomputed integers.

## Three components

**connector** is the contract. It defines the structs that cross the boundary
between a strategy and the engine, the rings that carry them, and the
`IStrategy` interface. Strategies depend on connector. The engine depends on
connector. Neither depends on the other, which is what lets a strategy move
between in-process, cross-process and Python without changing the engine.

**GEL** is the engine. It owns one single-producer single-consumer ring per
strategy. `run_once()` drains each ring and takes every order through the same
steps: resolve the account, check risk, encode the wire frame, route to a
broker, and report back through the drop-copy. Risk state is kept per
strategy and per account, so one misbehaving strategy is cut off without
touching the others.

**Market-Maker (MMX)** is one strategy, taken all the way to hardware. Its
host side computes the Avellaneda–Stoikov parameters. Its FPGA side applies
them on each event.

## Why the boundary structs look the way they do

Every struct that crosses a boundary is trivially copyable, has a fixed size,
and is checked with `static_assert`. An `Order` is exactly one 64-byte cache
line. That gives three things at once: a ring slot never shares a cache line
with its neighbour, the same bytes can sit in a memory-mapped file read by
another process, and the FPGA can read the struct at fixed offsets.

Prices are integers scaled by 10^8. No floating point crosses a boundary.

## Why the rings are single-producer single-consumer

With exactly one writer and one reader, each side owns one index and only
reads the other's. No compare-and-swap loop, no lock. The writer publishes
with a release store and the reader observes with an acquire load, which is
exactly the guarantee needed: a slot's contents are visible before the index
that announces them.

The cost is one ring per strategy rather than one shared queue. Thirty-two
rings is cheap; contention on a shared queue is not.

## What the hardware build does not have

The engine's FPGA router is a host-side interface with no bitstream behind it.
The hardware result in this repository is MMX's data plane, driven through an
AXI-Lite register interface by a host program — not a NIC-to-NIC path. The
[README](../../README.md#measurement-and-scope) lists the limits.
