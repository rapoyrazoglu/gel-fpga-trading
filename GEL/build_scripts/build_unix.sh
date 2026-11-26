#!/bin/bash
# GEL Trading System - Unix/Linux Build Script
# This script automates the build process on Unix-based systems

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}╔════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  GEL Trading System - Build Script    ║${NC}"
echo -e "${BLUE}╔════════════════════════════════════════╗${NC}"
echo ""

# Check if running on Unix-like system
if [[ "$OSTYPE" != "linux-gnu"* && "$OSTYPE" != "darwin"* ]]; then
    echo -e "${RED}Error: This script is for Unix/Linux/macOS only${NC}"
    echo "For Windows, use CLion or Visual Studio"
    exit 1
fi

# Check CMake
echo -e "${YELLOW}[1/6] Checking CMake...${NC}"
if ! command -v cmake &> /dev/null; then
    echo -e "${RED}Error: CMake not found${NC}"
    echo "Install: sudo apt-get install cmake (Ubuntu) or brew install cmake (macOS)"
    exit 1
fi
CMAKE_VERSION=$(cmake --version | head -n1 | grep -oE '[0-9]+\.[0-9]+' | head -1)
echo -e "${GREEN}✓ CMake $CMAKE_VERSION found${NC}"

# Check C++ compiler
echo -e "${YELLOW}[2/6] Checking C++ compiler...${NC}"

COMPILER=""

# macOS: Find highest available Homebrew GCC version for full C++23 support
if [[ "$OSTYPE" == "darwin"* ]]; then
    for gcc_version in 15 14 13 12; do
        if [ -f "/opt/homebrew/bin/g++-${gcc_version}" ]; then
            COMPILER="/opt/homebrew/bin/g++-${gcc_version}"
            COMPILER_VERSION=$($COMPILER --version | head -n1)
            echo -e "${GREEN}✓ $COMPILER_VERSION (Homebrew GCC-${gcc_version} - Full C++23 support)${NC}"
            break
        fi
    done
fi

# Fallback to system compilers if no Homebrew GCC found
if [ -z "$COMPILER" ]; then
    if command -v g++ &> /dev/null; then
        COMPILER="g++"
        COMPILER_VERSION=$(g++ --version | head -n1)
        echo -e "${GREEN}✓ $COMPILER_VERSION${NC}"
    elif command -v clang++ &> /dev/null; then
        COMPILER="clang++"
        COMPILER_VERSION=$(clang++ --version | head -n1)
        echo -e "${YELLOW}⚠ Using Apple Clang (limited C++23 support)${NC}"
        echo -e "${YELLOW}  For full C++23 support, install GCC: brew install gcc${NC}"
    else
        echo -e "${RED}Error: No C++ compiler found${NC}"
        echo "Install: sudo apt-get install g++ (Ubuntu) or xcode-select --install (macOS)"
        exit 1
    fi
fi

# Check vcpkg (optional)
echo -e "${YELLOW}[3/6] Checking vcpkg...${NC}"
if [ -n "$VCPKG_ROOT" ]; then
    echo -e "${GREEN}✓ vcpkg found at: $VCPKG_ROOT${NC}"
elif [ -d "$HOME/vcpkg" ]; then
    export VCPKG_ROOT="$HOME/vcpkg"
    echo -e "${GREEN}✓ vcpkg found at: $HOME/vcpkg${NC}"
    echo -e "${YELLOW}  Tip: Add 'export VCPKG_ROOT=~/vcpkg' to ~/.bashrc${NC}"
else
    echo -e "${YELLOW}⚠ vcpkg not found (will try system packages)${NC}"
    echo "  To install vcpkg:"
    echo "    git clone https://github.com/Microsoft/vcpkg.git ~/vcpkg"
    echo "    ~/vcpkg/bootstrap-vcpkg.sh"
    echo "    ~/vcpkg/vcpkg install boost-headers eigen3 curl nlohmann-json fmt range-v3"
fi

# Get build type (default: Debug)
BUILD_TYPE="${1:-Debug}"
if [[ "$BUILD_TYPE" != "Debug" && "$BUILD_TYPE" != "Release" ]]; then
    echo -e "${RED}Error: Build type must be 'Debug' or 'Release'${NC}"
    echo "Usage: ./build_unix.sh [Debug|Release]"
    exit 1
fi
echo -e "${YELLOW}[4/6] Build type: $BUILD_TYPE${NC}"

# Get script directory and project root
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$( cd "$SCRIPT_DIR/.." && pwd )"

# Clean old builds
echo -e "${YELLOW}[5/6] Cleaning old build artifacts...${NC}"
cd "$PROJECT_ROOT"
rm -rf build cmake-build-debug cmake-build-release
echo -e "${GREEN}✓ Cleaned${NC}"

# Configure and build
echo -e "${YELLOW}[6/6] Building GEL Trading System...${NC}"
mkdir -p build
cd build

# Configure with CMake
echo -e "${BLUE}  → Running CMake configuration...${NC}"
if cmake "$PROJECT_ROOT" -DCMAKE_BUILD_TYPE=$BUILD_TYPE -DCMAKE_CXX_COMPILER=$COMPILER; then
    echo -e "${GREEN}✓ CMake configuration successful${NC}"
else
    echo -e "${RED}✗ CMake configuration failed${NC}"
    echo "See errors above. Common issues:"
    echo "  1. Missing dependencies - install via vcpkg or system package manager"
    echo "  2. Wrong compiler version - need GCC 12+ or Clang 15+"
    echo "  3. See BUILD_UNIX.md for detailed instructions"
    exit 1
fi

# Build
echo -e "${BLUE}  → Compiling...${NC}"
NCORES=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
if make -j$NCORES; then
    echo -e "${GREEN}✓ Build successful${NC}"
else
    echo -e "${RED}✗ Build failed${NC}"
    echo "Check compilation errors above"
    exit 1
fi

# Summary
echo ""
echo -e "${GREEN}╔════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║         BUILD SUCCESSFUL!              ║${NC}"
echo -e "${GREEN}╚════════════════════════════════════════╝${NC}"
echo ""
echo "Executables built:"
for exe in GEL all_tests sma_test file_cleaner create_sample_data; do
    if [ -f "$exe" ]; then
        echo -e "  ${GREEN}✓${NC} ./$exe"
    fi
done

echo ""
echo "Build location: $PROJECT_ROOT/build/"
echo ""
echo "Next steps:"
echo -e "  ${BLUE}1.${NC} Run tests:    ${GREEN}cd $PROJECT_ROOT/build && ./all_tests${NC}"
echo -e "  ${BLUE}2.${NC} Run GEL:      ${GREEN}cd $PROJECT_ROOT/build && ./GEL${NC}"
echo -e "  ${BLUE}3.${NC} View reports: ${GREEN}cat $PROJECT_ROOT/build/test_results/test_results.md${NC}"
echo ""
echo "For more info, see build_scripts/README.md"
