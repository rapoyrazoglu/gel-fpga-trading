cmake_minimum_required(VERSION 3.16)

project(connector
    VERSION 0.1.0
    DESCRIPTION "Strategy ↔ engine SDK shared by GEL, PCTA, and Market-Maker"
    LANGUAGES CXX
)

# -----------------------------------------------------------------------
# Header-only INTERFACE library — what every consumer (GEL, PCTA C++
# strategies, MM host) links against.
# -----------------------------------------------------------------------
add_library(connector INTERFACE)
add_library(connector::connector ALIAS connector)

target_include_directories(connector INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(connector INTERFACE cxx_std_17)

# Cross-process SHM relies on POSIX shm_open / mmap. Linux needs -lrt.
if(UNIX AND NOT APPLE)
    target_link_libraries(connector INTERFACE rt)
endif()

# -----------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------
option(CONNECTOR_BUILD_TESTS    "Build connector tests"    ON)
option(CONNECTOR_BUILD_EXAMPLES "Build connector examples" ON)
option(CONNECTOR_BUILD_PYTHON   "Build Python bindings (requires pybind11)" ON)

if(CONNECTOR_BUILD_TESTS)
    enable_testing()

    add_executable(connector_parity_test tests/parity_test.cpp)
    target_link_libraries(connector_parity_test PRIVATE connector::connector)
    add_test(NAME connector_parity COMMAND connector_parity_test)

    add_executable(connector_shm_roundtrip_test tests/shm_roundtrip_test.cpp)
    target_link_libraries(connector_shm_roundtrip_test PRIVATE connector::connector)
    add_test(NAME connector_shm_roundtrip COMMAND connector_shm_roundtrip_test)
endif()

# -----------------------------------------------------------------------
# Examples
# -----------------------------------------------------------------------
if(CONNECTOR_BUILD_EXAMPLES)
    add_executable(connector_cpp_strategy_skeleton
        examples/cpp_strategy_skeleton.cpp)
    target_link_libraries(connector_cpp_strategy_skeleton
        PRIVATE connector::connector)

    add_executable(connector_gel_daemon
        examples/cross_process/gel_daemon.cpp)
    target_link_libraries(connector_gel_daemon
        PRIVATE connector::connector)
    set_target_properties(connector_gel_daemon PROPERTIES
        OUTPUT_NAME gel_daemon
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples/cross_process)
endif()

# -----------------------------------------------------------------------
# Python bindings — gated on pybind11 being available.
# -----------------------------------------------------------------------
if(CONNECTOR_BUILD_PYTHON)
    find_package(pybind11 CONFIG QUIET)
    if(pybind11_FOUND)
        pybind11_add_module(_connector_native src/bindings.cpp)
        target_link_libraries(_connector_native PRIVATE connector::connector)

        # Drop the .so next to python/connector/ so `import connector`
        # resolves directly with PYTHONPATH=python.
        set_target_properties(_connector_native PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY
                ${CMAKE_CURRENT_SOURCE_DIR}/python/connector)

        message(STATUS "connector: Python bindings ENABLED")
    else()
        message(STATUS "connector: Python bindings SKIPPED — install pybind11 to enable")
    endif()
endif()

# -----------------------------------------------------------------------
# A note for downstream consumers (GEL, etc.):
#
#     add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../connector
#                      ${CMAKE_BINARY_DIR}/connector_build)
#     target_link_libraries(my_target PRIVATE connector::connector)
#
# The connector::connector alias is the only public entry point.
# -----------------------------------------------------------------------
