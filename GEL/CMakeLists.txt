cmake_minimum_required(VERSION 3.22)  # 3.31 çok yüksek, 3.22 daha uyumlu

# =============================================================================
# vcpkg toolchain - Platform-agnostic configuration
# =============================================================================
# Priority order:
# 1. VCPKG_ROOT environment variable (recommended)
# 2. Project-local vcpkg submodule
# 3. Platform-specific default paths
if(DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
    message(STATUS "Using vcpkg from VCPKG_ROOT: $ENV{VCPKG_ROOT}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
    message(STATUS "Using project-local vcpkg")
elseif(WIN32)
    set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
    message(STATUS "Using Windows default vcpkg path")
elseif(APPLE)
    if(EXISTS "/opt/homebrew/share/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "/opt/homebrew/share/vcpkg/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "Vcpkg toolchain file")
    elseif(EXISTS "/usr/local/share/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "/usr/local/share/vcpkg/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "Vcpkg toolchain file")
    endif()
    message(STATUS "Using macOS vcpkg path")
else()
    # Linux default paths
    if(EXISTS "/opt/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "/opt/vcpkg/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "Vcpkg toolchain file")
    elseif(EXISTS "$ENV{HOME}/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "$ENV{HOME}/vcpkg/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "Vcpkg toolchain file")
    endif()
    message(STATUS "Using Linux vcpkg path")
endif()

project(GEL VERSION 0.1.0 LANGUAGES CXX)

# =============================================================================
# Connector (sibling repo) — strategy ↔ engine SDK.
# =============================================================================
# All wire types, IStrategy, ring buffers, Python bindings, and the SHM
# transport live in ../connector. GEL link against connector::connector.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../connector/CMakeLists.txt")
    message(FATAL_ERROR
        "connector/ not found next to GEL/ — it ships in the same repository.")
endif()
# Already defined when GEL is built from the repo-root CMakeLists.
if(NOT TARGET connector)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../connector
                     ${CMAKE_BINARY_DIR}/connector_build)
endif()

# C++23 desteği
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Platform detection
if(UNIX AND NOT APPLE)
    message(STATUS "Platform: Linux")
elseif(APPLE)
    message(STATUS "Platform: macOS")
elseif(WIN32)
    message(STATUS "Platform: Windows")
endif()

# Compiler-specific flags
if(MSVC)
    # Visual Studio için
    add_compile_options(/W4 /permissive- /utf-8)
    set(CMAKE_CXX_FLAGS_DEBUG "/Od /Zi")
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG")
else()
    # GCC/Clang için
    add_compile_options(-Wall -Wextra -Wpedantic)

    # Platform-specific sanitizer configuration
    if(APPLE)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            # macOS + GCC: sanitizers not supported (libasan not available)
            set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
            set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
        else()
            # macOS + Apple Clang: address sanitizer works
            set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address")
            set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
        endif()
    else()
        # Linux: full sanitizers supported
        set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined")
        set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native")
    endif()
endif()

# Experimental C++ features (opsiyonel)
option(GEL_ENABLE_EXPERIMENTAL "Enable experimental C++ features" ON)
if(GEL_ENABLE_EXPERIMENTAL)
    add_compile_definitions(GEL_ENABLE_EXPERIMENTAL)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options(-std=c++2b)
    endif()
endif()

# =============================================================================
# Build-target switches: CPU brokers vs FPGA brokers
# =============================================================================
#
# GEL has two broker tiers:
#
#   CPU brokers (HTTPS / REST / WebSocket / FIX-over-TCP)
#     - OANDA, Interactive Brokers, Binance, etc.
#     - Latency floor: tens of ms (Internet round-trip).
#     - No useful FPGA acceleration path — wire is text/JSON.
#     - Toggle:   -DGEL_ENABLE_OANDA=ON   (default)
#     - Sources:  include/gel/broker/cpu/*.hpp
#
#   FPGA brokers (binary exchange-native protocols, AWS F2)
#     - NASDAQ OUCH, ITCH, custom binary feeds.
#     - Latency floor: sub-microsecond on UltraScale+.
#     - Requires AWS FPGA SDK (only available inside the AWS dev AMI /
#       Vitis container).
#     - Toggle:   -DGEL_ENABLE_FPGA=ON    (default OFF)
#     - Defines:  GEL_AWS_F2
#     - Sources:  include/gel/platform/aws_f2_router.hpp
#
# A typical capstone-on-laptop build uses GEL_ENABLE_OANDA=ON / GEL_ENABLE_FPGA=OFF.
# Production on AWS F2 uses both ON.
#
# See docs/BROKER_ARCHITECTURE.md for the full split.
# =============================================================================
option(GEL_ENABLE_OANDA "Build the CPU OANDA broker (REST over curl)" ON)
option(GEL_ENABLE_FPGA  "Build the FPGA broker path (AWS F2, requires aws-fpga SDK)" OFF)

if(GEL_ENABLE_OANDA)
    add_compile_definitions(GEL_ENABLE_OANDA)
    message(STATUS "GEL: CPU OANDA broker        ENABLED")
else()
    message(STATUS "GEL: CPU OANDA broker        DISABLED")
endif()

if(GEL_ENABLE_FPGA)
    add_compile_definitions(GEL_AWS_F2 GEL_ENABLE_FPGA)
    # Path to the AWS FPGA SDK (override on command line if non-default).
    set(GEL_AWS_FPGA_SDK_DIR "$ENV{AWS_FPGA_REPO_DIR}/sdk/userspace"
        CACHE PATH "Path to the AWS FPGA SDK userspace tree")
    if(EXISTS "${GEL_AWS_FPGA_SDK_DIR}/include/fpga_pci.h")
        include_directories(${GEL_AWS_FPGA_SDK_DIR}/include)
        link_directories(${GEL_AWS_FPGA_SDK_DIR}/lib)
        message(STATUS "GEL: FPGA broker (AWS F2)    ENABLED at ${GEL_AWS_FPGA_SDK_DIR}")
    else()
        message(WARNING
            "GEL_ENABLE_FPGA=ON but aws-fpga SDK not found. "
            "Set AWS_FPGA_REPO_DIR or pass -DGEL_AWS_FPGA_SDK_DIR=<path>.")
    endif()
else()
    message(STATUS "GEL: FPGA broker (AWS F2)    DISABLED")
endif()

# Bağımlılıkları bul
find_package(Boost REQUIRED)
find_package(Eigen3 REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(fmt REQUIRED)
find_package(range-v3 REQUIRED)
# Note: Catch2 removed - using custom test framework (test_reporter.hpp + logger.hpp)

# Include dizinleri
include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/fpga/hls/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../connector/include   # connector SDK
)

# === EXECUTABLE 1: Ana uygulama (main.cpp) ===
add_executable(GEL
        src/main.cpp
)

target_link_libraries(GEL PRIVATE
        Boost::boost
        Eigen3::Eigen
        CURL::libcurl
        nlohmann_json::nlohmann_json
        fmt::fmt
        range-v3::range-v3
)

# === EXECUTABLE 2: All Tests ===
# Pattern implementation sources
set(PATTERN_SOURCES
        src/patterns/fair_value_gap.cpp
        src/patterns/order_flow.cpp
)

# Broker implementation sources
set(BROKER_SOURCES
        src/broker/oanda_client.cpp
)

add_executable(all_tests
        src/tests/all_tests.cpp
        ${PATTERN_SOURCES}
        ${BROKER_SOURCES}
)

target_include_directories(all_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/fpga/hls/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/include/gel/testing
)

target_link_libraries(all_tests PRIVATE
        fmt::fmt
        Boost::boost
        nlohmann_json::nlohmann_json
        CURL::libcurl
)

# === EXECUTABLE 3: SMA Test ===
# Note: Requires sma_hls.hpp header (FPGA HLS implementation)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fpga/hls/test/sma_test.cpp" AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fpga/hls/include/sma_hls.hpp")
    add_executable(sma_test
            fpga/hls/test/sma_test.cpp
    )

    target_include_directories(sma_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/fpga/hls/include
    )

    target_link_libraries(sma_test PRIVATE
            fmt::fmt
    )
else()
    message(STATUS "SMA test skipped - requires fpga/hls/test/sma_test.cpp and fpga/hls/include/sma_hls.hpp")
endif()

# === EXECUTABLE 4: File Cleaner ===
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/scripts/file_cleaner.cpp")
    add_executable(file_cleaner
            src/scripts/file_cleaner.cpp
    )

    target_include_directories(file_cleaner PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/fpga/hls/include
    )

    target_link_libraries(file_cleaner PRIVATE
            fmt::fmt
    )
else()
    message(STATUS "file cleaner skipped - create src/scripts/file_cleaner.cpp to enable")
endif()

# === EXECUTABLE 5: Create Sample Data ===
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/scripts/create_sample_data.cpp")
    add_executable(create_sample_data
            src/scripts/create_sample_data.cpp
    )

    target_include_directories(create_sample_data PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/fpga/hls/include
    )

    target_link_libraries(create_sample_data PRIVATE
            fmt::fmt
    )
else()
    message(STATUS "create_sample_data skipped - create src/scripts/create_sample_data.cpp to enable")
endif()

# === EXECUTABLE 5.5: C++ Real Data Backtest ===
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/scripts/cpp_backtest_real.cpp")
    add_executable(cpp_backtest
            src/scripts/cpp_backtest_real.cpp
    )

    target_include_directories(cpp_backtest PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    # Release optimizations for backtest performance
    if(MSVC)
        target_compile_options(cpp_backtest PRIVATE /O2)
    else()
        target_compile_options(cpp_backtest PRIVATE -O3)
    endif()
endif()

# === EXECUTABLE 5.6: C++ EMA Momentum Backtest ===
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/scripts/cpp_ema_backtest.cpp")
    add_executable(cpp_ema_backtest
            src/scripts/cpp_ema_backtest.cpp
    )

    target_include_directories(cpp_ema_backtest PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    if(MSVC)
        target_compile_options(cpp_ema_backtest PRIVATE /O2)
    else()
        target_compile_options(cpp_ema_backtest PRIVATE -O3)
    endif()
endif()

# === EXECUTABLE 5.7: Live Trader (Interactive Strategy Selection) ===
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/scripts/live_trader.cpp")
    add_executable(live_trader
            src/scripts/live_trader.cpp
    )

    target_include_directories(live_trader PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(live_trader PRIVATE
            CURL::libcurl
            nlohmann_json::nlohmann_json
    )

    if(MSVC)
        target_compile_options(live_trader PRIVATE /O2)
    else()
        target_compile_options(live_trader PRIVATE -O3)
    endif()
endif()

# === EXECUTABLE 6b: Multi-OANDA smoke demo (CPU broker, multi-account) ===
if(GEL_ENABLE_OANDA AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/scripts/multi_oanda_demo.cpp")
    add_executable(multi_oanda_demo
            src/scripts/multi_oanda_demo.cpp
            ${BROKER_SOURCES}
    )
    target_include_directories(multi_oanda_demo PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../connector/include
    )
    target_link_libraries(multi_oanda_demo PRIVATE
            CURL::libcurl
            nlohmann_json::nlohmann_json
    )
    if(MSVC)
        target_compile_options(multi_oanda_demo PRIVATE /O2)
    else()
        target_compile_options(multi_oanda_demo PRIVATE -O3)
    endif()
endif()

# === EXECUTABLE 6: Live Trading (OANDA API) ===
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/live_trading_main.cpp")
    add_executable(live_trading
            src/live_trading_main.cpp
            ${PATTERN_SOURCES}
            ${BROKER_SOURCES}
    )

    target_include_directories(live_trading PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/fpga/hls/include
    )

    target_link_libraries(live_trading PRIVATE
            Boost::boost
            CURL::libcurl
            nlohmann_json::nlohmann_json
            fmt::fmt
    )
else()
    message(STATUS "live_trading skipped - create src/live_trading_main.cpp to enable")
endif()

# === EXECUTABLE 7: Ana uygulama 2 (src/*.cpp) ===
# Sadece src klasöründe başka .cpp dosyaları varsa
file(GLOB_RECURSE APP_SOURCES "src/*.cpp")
# main.cpp ve test dosyalarını çıkar

list(FILTER APP_SOURCES EXCLUDE REGEX ".*tests/.*\\.cpp$")
list(FILTER APP_SOURCES EXCLUDE REGEX ".*scripts/.*\\.cpp$")
list(FILTER APP_SOURCES EXCLUDE REGEX ".*live_trading_main\\.cpp$")

if(APP_SOURCES)
    add_executable(gel_app ${APP_SOURCES})

    target_link_libraries(gel_app PRIVATE
            Boost::boost
            Eigen3::Eigen
            CURL::libcurl
            nlohmann_json::nlohmann_json
            fmt::fmt
            range-v3::range-v3
    )
endif()

# === EXECUTABLE 8: GEL Integrated Demo (PCTA + MM end-to-end) ===
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/scripts/integrated_demo.cpp")
    add_executable(gel_integrated_demo
            src/scripts/integrated_demo.cpp
    )

    target_include_directories(gel_integrated_demo PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    if(MSVC)
        target_compile_options(gel_integrated_demo PRIVATE /O2)
    else()
        target_compile_options(gel_integrated_demo PRIVATE -O3)
    endif()
endif()

# === EXECUTABLE 8b: GEL 10-Strategy Fleet Demo (strategy-agnostic platform) ===
# PCTA + Market Making + 8 technical IStrategy plug-ins on ONE GelEngine, each
# with an independent risk cell and PnL cell. Header-only; no external deps.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/scripts/fleet_demo.cpp")
    add_executable(gel_fleet_demo
            src/scripts/fleet_demo.cpp
    )

    target_include_directories(gel_fleet_demo PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../connector/include
    )

    if(MSVC)
        target_compile_options(gel_fleet_demo PRIVATE /O2)
    else()
        target_compile_options(gel_fleet_demo PRIVATE -O3)
    endif()
endif()

# === Parity test + Python bindings now live in ../connector ===
# - parity test:        ../connector/build/connector_parity_test
# - python module:      ../connector/python/connector  (built as
#                       _connector_native if pybind11 is present)
# These targets propagate from add_subdirectory(../connector …) above.

# === Demo smoke tests (header-only, no market data needed) ===
enable_testing()
if(TARGET gel_integrated_demo)
    add_test(NAME gel_integrated_demo COMMAND gel_integrated_demo)
    set_tests_properties(gel_integrated_demo PROPERTIES
        PASS_REGULAR_EXPRESSION "GEL multi-account demo")
endif()
if(TARGET gel_fleet_demo)
    add_test(NAME gel_fleet_demo COMMAND gel_fleet_demo)
    set_tests_properties(gel_fleet_demo PROPERTIES
        PASS_REGULAR_EXPRESSION "10-STRATEGY PLATFORM")
endif()

# === TESTLER ===
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
else()
    message(STATUS "Tests directory skipped - create tests/CMakeLists.txt to enable tests")
endif()