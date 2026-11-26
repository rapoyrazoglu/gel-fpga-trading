#!/usr/bin/env bash
# Offline regression — one command, no FPGA and no cloud needed.
# Prints a PASS/FAIL line per layer and exits non-zero on any failure.
#
#   connector + MMX : root CMake build + ctest (parity, shm IPC, acceptance)
#   GEL             : platform demos (2-strategy + 10-strategy fleet)
#   FPGA RTL        : GHDL testbenches, including the mutation ("L2") ones
#
# Requires: cmake, a C++23 compiler ($CXX, default clang++), ghdl for the RTL layer.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CXX="${CXX:-clang++}"
STD="-std=gnu++2b"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
pass=0; fail=0
ok()  { printf "  [PASS] %s\n" "$1"; pass=$((pass+1)); }
bad() { printf "  [FAIL] %s\n" "$1"; fail=$((fail+1)); }

echo "=============== OFFLINE REGRESSION ==============="

echo "[connector + MMX] cmake build + ctest"
if cmake -S "$ROOT" -B "$TMP/build" -DCMAKE_BUILD_TYPE=Release >"$TMP/cmake.log" 2>&1 \
   && cmake --build "$TMP/build" -j >>"$TMP/cmake.log" 2>&1 \
   && ctest --test-dir "$TMP/build" --output-on-failure >"$TMP/ctest.log" 2>&1; then
  ok "connector + MMX ($(grep -oE '[0-9]+ tests failed out of [0-9]+' "$TMP/ctest.log"))"
else
  bad "connector + MMX (see log below)"; tail -30 "$TMP/cmake.log" "$TMP/ctest.log" 2>/dev/null
fi

echo "[GEL] platform demos"
GI="-I $ROOT/GEL/include -I $ROOT/connector/include"
if $CXX $STD -O2 $GI "$ROOT/GEL/src/scripts/integrated_demo.cpp" -o "$TMP/intg" 2>"$TMP/gel.log" \
   && "$TMP/intg" | grep -q "GEL multi-account demo"; then ok "GEL integrated_demo"; else bad "GEL integrated_demo"; fi
if $CXX $STD -O2 $GI "$ROOT/GEL/src/scripts/fleet_demo.cpp" -o "$TMP/fleet" 2>>"$TMP/gel.log" \
   && "$TMP/fleet" | grep -q "10-STRATEGY PLATFORM"; then ok "GEL fleet_demo (10 strategies, one engine)"; else bad "GEL fleet_demo"; fi

echo "[FPGA RTL] GHDL testbenches"
if command -v ghdl >/dev/null 2>&1; then
  # Run inside the RTL dir with relative names: GHDL's library stores source
  # paths, so analysis and run must share the same working directory.
  read tpass tfail < <( cd "$ROOT/Market-Maker/fpga/rtl" && {
    W="$TMP/ghdl"; mkdir -p "$W"
    A() { ghdl -a --std=08 --workdir="$W" -fsynopsys "$1" >/dev/null 2>&1; }
    A mmx_pkg.vhd; A mmx_multi_pkg.vhd
    A sha256_core.vhd; A hmac_verify.vhd
    for f in $(ls *.vhd | grep -vE '^tb_|_pkg\.vhd$|sha256_core|hmac_verify|mmx_dataplane'); do A "$f"; done
    A param_block_loader.vhd; A mmx_dataplane.vhd
    for f in mutants/*.vhd; do A "$f"; done   # deliberately broken DUTs for the *_l2 benches
    for f in tb_*.vhd; do A "$f"; done
    tp=0; tf=0
    for tb in $(ls tb_*.vhd | sed 's/\.vhd//'); do
      # elab-run (not -r): works on every GHDL backend and never reuses a stale binary
      out=$(ghdl --elab-run --std=08 --workdir="$W" -fsynopsys -o "$W/$tb" "$tb" 2>&1)
      if echo "$out" | grep -qiE "assertion violation|severity (error|failure)|report note\): FAIL"; then tf=$((tf+1)); echo "FAILED: $tb" >&2
      elif echo "$out" | grep -qiE "report note\): PASS|PASS:"; then tp=$((tp+1))
      else tf=$((tf+1)); echo "NO VERDICT: $tb" >&2; fi
    done
    echo "$tp $tf"
  } )
  if [ "${tfail:-1}" -eq 0 ] && [ "${tpass:-0}" -ge 23 ]; then ok "GHDL testbenches ($tpass/$tpass)"; else bad "GHDL testbenches (${tpass:-0} PASS / ${tfail:-?} FAIL)"; fi
else
  # shortcut: missing ghdl is a skip, not a failure; CI installs it so the layer cannot silently vanish there.
  printf "  [SKIP] GHDL testbenches (ghdl not installed)\n"
fi

echo "=================================================="
printf "TOTAL: %d PASS / %d FAIL\n" "$pass" "$fail"
[ "$fail" -eq 0 ]
