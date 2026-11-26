# Run the regression

One command checks every layer that does not need an FPGA:

```sh
tools/run_offline_regression.sh
```

Expected output:

```
[connector + MMX] cmake build + ctest
  [PASS] connector + MMX (0 tests failed out of 15)
[GEL] platform demos
  [PASS] GEL integrated_demo
  [PASS] GEL fleet_demo (10 strategies, one engine)
[FPGA RTL] GHDL testbenches
  [PASS] GHDL testbenches (23/23)
TOTAL: 4 PASS / 0 FAIL
```

It needs CMake, a C++23 compiler and, for the last layer, GHDL. Without GHDL
that layer is reported as `SKIP`. Pick the compiler with `CXX=g++-14`.

Everything is built in a temporary directory that is removed afterwards. The
script never reuses binaries from an earlier run.

## Just the C++ tests

```sh
cmake -B build && cmake --build build -j && ctest --test-dir build
```

## Including the engine

The engine needs Boost, Eigen, curl, nlohmann_json, fmt and range-v3.

```sh
cmake -B build -DGEL_BUILD_ENGINE=ON -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build -j && ctest --test-dir build     # 17 tests
```

On Ubuntu the distribution packages work without vcpkg; the CI workflow lists
them.

## One testbench by hand

Run from `Market-Maker/fpga/rtl`, because two benches read
`pricing_vectors.txt` by relative path.

```sh
W=$(mktemp -d)
for f in mmx_pkg.vhd sha256_core.vhd tb_sha256.vhd; do
  ghdl -a --std=08 --workdir=$W -fsynopsys $f
done
ghdl --elab-run --std=08 --workdir=$W -fsynopsys -o $W/tb tb_sha256
```

A passing bench prints a `PASS` report note.
