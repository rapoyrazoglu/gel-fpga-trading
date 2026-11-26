#!/bin/bash
# GEL Trading System - Ubuntu/Debian Dependency Installation
# Alternative to vcpkg - uses apt package manager

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}╔════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  GEL - Ubuntu Dependencies (apt)       ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════╝${NC}"
echo ""

# Check if running on Linux
if [[ "$OSTYPE" != "linux-gnu"* ]]; then
    echo -e "${RED}Error: This script is for Linux only${NC}"
    echo "For macOS, use install_deps_macos.sh"
    exit 1
fi

# Check if apt is available
if ! command -v apt-get &> /dev/null; then
    echo -e "${RED}Error: apt-get not found${NC}"
    echo "This script is for Ubuntu/Debian-based systems"
    echo "For other distros:"
    echo "  - Fedora/RHEL: Use dnf install"
    echo "  - Arch: Use pacman -S"
    exit 1
fi

# Update package list
echo -e "${YELLOW}[1/3] Updating package list...${NC}"
sudo apt-get update
echo -e "${GREEN}✓ Package list updated${NC}"

# Install build tools
echo -e "${YELLOW}[2/3] Installing build tools...${NC}"
sudo apt-get install -y \
    build-essential \
    cmake \
    g++-12

echo -e "${GREEN}✓ Build tools installed${NC}"

# Install dependencies
echo -e "${YELLOW}[3/3] Installing dependencies...${NC}"
echo "This will install: libboost-all-dev, libeigen3-dev, libcurl4-openssl-dev,"
echo "                   nlohmann-json3-dev, libfmt-dev, librange-v3-dev"
echo ""

sudo apt-get install -y \
    libboost-all-dev \
    libeigen3-dev \
    libcurl4-openssl-dev \
    nlohmann-json3-dev \
    libfmt-dev \
    librange-v3-dev

echo -e "${GREEN}✓ All dependencies installed${NC}"

# Set default compiler
echo ""
echo -e "${YELLOW}Setting GCC 12 as default compiler...${NC}"
sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-12 100 2>/dev/null || true
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-12 100 2>/dev/null || true
echo -e "${GREEN}✓ Compiler configured${NC}"

# Verify installation
echo ""
echo -e "${YELLOW}Verifying installation...${NC}"
echo -n "CMake: "
cmake --version | head -1
echo -n "GCC: "
g++ --version | head -1

# Summary
echo ""
echo -e "${GREEN}╔════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║     Dependencies Installed!            ║${NC}"
echo -e "${GREEN}╚════════════════════════════════════════╝${NC}"
echo ""
echo "Installed packages (via apt):"
echo "  ✓ build-essential"
echo "  ✓ cmake"
echo "  ✓ g++-12"
echo "  ✓ libboost-all-dev"
echo "  ✓ libeigen3-dev"
echo "  ✓ libcurl4-openssl-dev"
echo "  ✓ nlohmann-json3-dev"
echo "  ✓ libfmt-dev"
echo "  ✓ librange-v3-dev"
echo ""
echo "Next steps:"
echo -e "  ${BLUE}1.${NC} Build GEL:  ${GREEN}cd .. && ./build_scripts/build_unix.sh${NC}"
echo ""
echo "Note: CMake will use system-installed libraries (no vcpkg needed)"
