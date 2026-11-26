# Changelog

## Unreleased

Repository restructure. No behaviour change in the engine, the market maker or
the RTL.

**Added**
- `connector/` — the SDK the engine depends on. It was missing, so the engine
  could not be built from this repository before.
- Root `CMakeLists.txt`: one configure builds the SDK and the market maker and
  runs 15 tests; `-DGEL_BUILD_ENGINE=ON` adds the engine and its demo tests.
- Root CI workflow. The previous workflow sat under `GEL/.github`, where GitHub
  does not look, so it never ran.
- `docs/` organised as tutorial, how-to, explanation and reference.
- `CONTRIBUTING.md`, `SECURITY.md`, an issue template.
- Compile-time checks that the ring indices are lock-free atomics.

**Changed**
- The deliberately broken RTL used by the mutation testbenches moved to
  `Market-Maker/fpga/rtl/mutants/`.
- `tools/run_offline_regression.sh` builds everything from scratch in a
  temporary directory. The old script reported the GHDL testbenches as passing
  only when simulator binaries from an earlier run were lying in the tree.
- `deploy_dataplane.sh` takes the key path and host from `F2_KEY` and `F2_HOST`
  instead of hardcoded values.
- The host router targets AWS F2 (`aws_f2_router.hpp` replaces the F1 header).
- Figures moved to `docs/assets/`, the browser demo to `docs/demo/`.

**Fixed**
- The market maker builds warning-free under `-Wall -Wextra -Wpedantic
  -Wshadow -Wconversion` and can be added with `add_subdirectory`.

**Security**
- The cross-process ring file is created owner-only and is not opened through
  a symlink, and a file that is not a private regular file owned by the caller
  is refused. Ring files left by older builds must be deleted once. Indices
  read from the shared file are masked before use.
- The account-admin tool refuses cross-origin and non-JSON writes, validates
  field types, escapes every value it renders, and loads its one third-party
  script pinned with an integrity hash.
- The `gel` CLI keeps its pid and log files in a private per-user directory
  instead of `/tmp`.

**Removed**
- The rolling-release job that published engine binaries on every push.
