# gel-fpga-trading — root build.
#
#   cmake -B build && cmake --build build -j && ctest --test-dir build
#
# builds the dependency-free parts (connector SDK + Market-Maker) and runs
# their tests. The GEL engine needs Boost, Eigen, curl, nlohmann_json, fmt and
# range-v3, so it is opt-in:
#
#   cmake -B build -DGEL_BUILD_ENGINE=ON -DCMAKE_TOOLCHAIN_FILE=<vcpkg.cmake>
cmake_minimum_required(VERSION 3.22)
project(gel_fpga_trading CXX)

option(GEL_BUILD_ENGINE "Also build the GEL engine (needs third-party deps)" OFF)

enable_testing()

add_subdirectory(connector)

# MMX's IStrategy adapter is the piece that plugs the market maker into the
# engine, so the root build always exercises it.
set(MMX_BUILD_CONNECTOR_ADAPTER ON CACHE BOOL "" FORCE)
add_subdirectory(Market-Maker)

if(GEL_BUILD_ENGINE)
    add_subdirectory(GEL)
endif()
