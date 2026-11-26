# Contributing

Thanks for looking. Questions, bug reports and small fixes are all welcome.

## Before you open a pull request

```sh
tools/run_offline_regression.sh
```

It must end with `0 FAIL`. It needs CMake, a C++23 compiler and GHDL; see
[docs/how-to/run-the-regression.md](docs/how-to/run-the-regression.md).

- Keep a pull request to one change. Say what it does and how you checked it.
- New logic comes with a test. New RTL comes with a testbench **and** a
  mutation testbench: [how to add one](docs/how-to/add-a-mutation-test.md).
- The market maker builds with `-Wall -Wextra -Wpedantic -Wshadow
  -Wconversion` and no warnings. Keep it that way.
- If behaviour changes, update the page under `docs/` in the same pull request
  and add a line to `CHANGELOG.md`.

## Rules for the fast path

These are what keep the hardware and the reference identical.

- No `float` or `double` in `host/pricing/` or `fpga/hls/`. A test enforces it.
- No signed overflow: widen, then saturate.
- Structs that cross a process or hardware boundary keep their size and field
  offsets. If you must change one, change the `static_assert`s, the encoder,
  the RTL and the reference page together.
- A change to `compute_quote` needs regenerated parity vectors and a passing
  `tb_pricing_parity`.

## Changes written with an AI assistant

Fine, with the same bar as any other change:

- You can explain and defend every line.
- You built it and ran the regression yourself.
- The diff contains only what the change needs. No drive-by rewrites.
- Say in the pull request that an assistant was used.

A design that looks odd may be that way for a measured reason: several
bit-widths and pipeline stages here exist only because of timing. Ask before
replacing one.

## Good first tasks

Each is small, self-contained and has an obvious way to check it.

1. **Version the shared-memory ring.** Add a magic number and version to the
   file header in `shm_ring.hpp` and refuse to attach on mismatch.
2. **ThreadSanitizer job.** Add a CI job that runs the connector tests under
   `-fsanitize=thread`, with a two-thread stress test for `SpscRing`.
3. **One ring instead of two.** `Market-Maker/host/util/spsc_queue.h` and
   `connector/ring.hpp` are two implementations of the same idea. Pick one.
4. **Flat risk tables.** Replace the `unordered_map` lookups in
   `risk_engine.hpp` with preallocated storage. Measure before and after with
   the telemetry hops. Background:
   [hot-path notes](docs/explanation/hot-path-notes.md).
5. **Fix or retire the failing engine tests** listed in
   [test status](docs/reference/test-status.md).
6. **`find_package` support** for connector, so it can be installed and
   consumed outside this tree.

Larger directions — putting HMAC back on silicon, a multi-threaded engine —
are worth an issue first.

## Reporting a bug

Use the issue template. The exact commands from a clean clone are the most
useful thing you can include.
