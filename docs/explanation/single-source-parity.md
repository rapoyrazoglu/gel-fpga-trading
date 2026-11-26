# Single-source parity

The claim "the FPGA computes the same quotes as the reference" is only worth
something if it cannot quietly stop being true. This page explains how the
repository arranges that.

## One function, compiled twice

The pricing math lives in one header,
[`host/pricing/pricing_engine.h`](../../Market-Maker/host/pricing/pricing_engine.h).
The HLS top-level in
[`fpga/hls/pricing_engine_hls.cpp`](../../Market-Maker/fpga/hls/pricing_engine_hls.cpp)
includes that header and adds only interface pragmas, hidden behind a macro so
an ordinary compiler sees plain C++. There is no second copy of the formula to
drift.

That works only because the function is written to mean the same thing in
both worlds:

- **Integers only.** A test scans the synthesizable sources, with comments
  stripped, for the words `float` and `double` and fails the build if it finds
  one.
- **No overflow to interpret.** Sums are formed in 64 bits and saturated to 32.
  On a CPU, signed overflow is undefined behaviour; in hardware it wraps
  silently. Saturation makes both do the same defined thing.
- **Inventory is clamped before the multiply**, so a corrupt input cannot push
  the product out of range.
- **Rounding is explicit.** The skew is `(product + 2^30) >> 31`. A bare shift
  rounds toward minus infinity and biased the reservation price by half a tick.

## A hand-written mirror, held to the same vectors

HLS output closed timing for the kernel alone but not inside the full data
plane, so the data plane uses a hand-pipelined VHDL version,
[`pricing_engine_beh.vhd`](../../Market-Maker/fpga/rtl/pricing_engine_beh.vhd).
That *is* a second copy, so it is pinned down differently: the C++ function
generates 500 random input and output vectors, and a testbench replays them
through the VHDL and compares every bit.

The same 500 vectors were replayed on the device. All 500 matched.

## Tests of the tests

A parity bench that always prints PASS would look identical to a real one. So
each bench has a twin that runs against a deliberately broken module and must
detect it. The broken modules are in
[`fpga/rtl/mutants/`](../../Market-Maker/fpga/rtl/mutants/). See
[how to add one](../how-to/add-a-mutation-test.md).

## The same idea on the order path

The order encoder exists as a struct-based reference and as a byte-buffer
version shaped like the hardware kernel. A fuzz test encodes 100,000 random
orders both ways and requires identical 56-byte frames.

## What parity does not cover

Parity says the hardware computes what the reference computes. It says nothing
about whether the reference is a good strategy.
