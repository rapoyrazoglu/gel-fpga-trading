# MMX — top-level CMake
#
# Phase A targets:
#   mmx_core          — static library: ipc, math, pricing, risk, threads, util
#   mmx_sim           — static library: LOB simulator + queue model
#   mmx_replay        — CLI backtester (sim/replay_main.cpp)
#   mmx_host          — Phase A integration smoke test (host/main.cpp)
#   tests/*           — one executable per spec §10 acceptance test
#
# Optional connector plug-in target (Lego-piece integration with GEL via the
# neutral connector SDK):
#   mmx_connector_adapter  — interface library exposing connector/mmx_strategy.hpp
#   test_connector_adapter — IStrategy ↔ MMX integration test
#   Enable with: -DMMX_BUILD_CONNECTOR_ADAPTER=ON
#                -DCONNECTOR_INCLUDE_DIR=/path/to/connector/include
#   When MMX is added via add_subdirectory() from a parent (GEL) CMakeLists,
#   the parent should set CONNECTOR_INCLUDE_DIR before adding us.
#
# Phase B (FPGA synthesis) is out of scope for this CMakeLists — the HLS
# source `fpga/hls/pricing_engine_hls.cpp` is built as a plain C++ object
# under MMX_HLS_BUILD=0 so it stays compilable in CI.

cmake_minimum_required(VERSION 3.20)
project(mmx CXX)

option(MMX_BUILD_CONNECTOR_ADAPTER "Build the connector::IStrategy adapter" OFF)
set(CONNECTOR_INCLUDE_DIR "" CACHE PATH
    "Path to connector/include (only required if MMX_BUILD_CONNECTOR_ADAPTER=ON)")

# Auto-detect connector when MMX is checked out as a sibling.
if(MMX_BUILD_CONNECTOR_ADAPTER AND NOT CONNECTOR_INCLUDE_DIR)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../connector/include/connector/strategy.hpp")
        set(CONNECTOR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../connector/include"
            CACHE PATH "" FORCE)
        message(STATUS "MMX: auto-detected connector at ${CONNECTOR_INCLUDE_DIR}")
    endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion
    -Wno-sign-conversion           # tick math intentionally mixes signed/unsigned
    -Wno-unused-parameter
    -fno-omit-frame-pointer
)

# Headers reference paths like "host/ipc/param_block.h" — root is the include path.
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# ---- Core library ---------------------------------------------------------

add_library(mmx_core STATIC
    host/ipc/sha256.cpp
    host/ipc/hmac.cpp
    host/math/kappa_fit.cpp
    host/risk/pnl_tracker.cpp
    host/risk/circuit_breaker.cpp
    host/threads/pcta_ingest.cpp
    host/threads/param_compiler.cpp
    host/threads/risk_supervisor.cpp
    fpga/hls/pricing_engine_hls.cpp     # plain-C++ buildable; HLS pragmas off
)

# ---- Simulator library ----------------------------------------------------

add_library(mmx_sim STATIC
    sim/queue_model.cpp
    sim/lob_simulator.cpp
)
target_link_libraries(mmx_sim PUBLIC mmx_core)

# ---- Replay CLI -----------------------------------------------------------

add_executable(mmx_replay sim/replay_main.cpp)
target_link_libraries(mmx_replay PRIVATE mmx_sim)

# ---- Walk-forward cross-validation harness (anti-overfit study) -----------

add_executable(mmx_cv sim/cv_harness.cpp)
target_link_libraries(mmx_cv PRIVATE mmx_sim)

# ---- Host integration smoke test ------------------------------------------

add_executable(mmx_host host/main.cpp)
target_link_libraries(mmx_host PRIVATE mmx_sim)

# ---- Acceptance tests -----------------------------------------------------

enable_testing()

set(_acceptance_tests
    test_paramblock_size
    test_hmac_roundtrip
    test_pricing_invariant_q0
    test_pricing_skew_sign
    test_kill_switch_pulls
    test_paramblock_staleness
    test_fixed_point_no_drift
    test_simulator_queue_pos
    test_circuit_breaker_trips
    test_pcta_regime_overrides
)

foreach(t IN LISTS _acceptance_tests)
    add_executable(${t} tests/${t}.cpp)
    target_link_libraries(${t} PRIVATE mmx_sim)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# Anti-overfit guard: the walk-forward cross-validation must select a strategy
# configuration on the TRAIN seeds that still generalizes on a DISJOINT TEST
# seed set. This test FAILS if MMX ever overfits (i.e. if a chosen config does
# not stay profitable out-of-sample). Reduced event count keeps it CI-fast.
add_test(NAME test_mmx_no_overfit COMMAND mmx_cv 120000)
set_tests_properties(test_mmx_no_overfit PROPERTIES
    PASS_REGULAR_EXPRESSION "OUT-OF-SAMPLE: ROBUST / NOT OVERFIT")

# Test #11 takes file paths as arguments — built once, run with the synth-
# critical sources.
add_executable(test_no_floating_point_in_fpga
    tests/test_no_floating_point_in_fpga.cpp)
add_test(
    NAME test_no_floating_point_in_fpga
    COMMAND test_no_floating_point_in_fpga
        ${CMAKE_CURRENT_SOURCE_DIR}/host/pricing/pricing_engine.h
        ${CMAKE_CURRENT_SOURCE_DIR}/fpga/hls/pricing_engine_hls.cpp
)

# ---- Optional connector adapter ------------------------------------------

if(MMX_BUILD_CONNECTOR_ADAPTER)
    if(NOT CONNECTOR_INCLUDE_DIR OR
       NOT EXISTS "${CONNECTOR_INCLUDE_DIR}/connector/strategy.hpp")
        message(FATAL_ERROR
            "MMX_BUILD_CONNECTOR_ADAPTER=ON but "
            "CONNECTOR_INCLUDE_DIR='${CONNECTOR_INCLUDE_DIR}' does not contain "
            "connector/strategy.hpp. Point it at connector/include.")
    endif()
    message(STATUS
        "MMX: building connector adapter against ${CONNECTOR_INCLUDE_DIR}")

    add_library(mmx_connector_adapter INTERFACE)
    target_include_directories(mmx_connector_adapter INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CONNECTOR_INCLUDE_DIR}
    )
    target_link_libraries(mmx_connector_adapter INTERFACE mmx_core)

    add_executable(test_connector_adapter tests/test_connector_adapter.cpp)
    target_link_libraries(test_connector_adapter PRIVATE mmx_connector_adapter)
    add_test(NAME test_connector_adapter COMMAND test_connector_adapter)
endif()

# ---- Convenience: `make check` runs everything under CTest. ---------------

set(_check_targets ${_acceptance_tests} test_no_floating_point_in_fpga mmx_replay mmx_host mmx_cv)
if(MMX_BUILD_CONNECTOR_ADAPTER)
    list(APPEND _check_targets test_connector_adapter)
endif()

add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS ${_check_targets}
)
