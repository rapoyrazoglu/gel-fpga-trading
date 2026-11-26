#!/bin/bash
# GEL Trading System - macOS Dependency Installation (Homebrew)
# Alternative to vcpkg - uses system package manager

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}╔════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  GEL - macOS Dependencies (Homebrew)   ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════╝${NC}"
echo ""

# Check if running on macOS
if [[ "$OSTYPE" != "darwin"* ]]; then
    echo -e "${RED}Error: This script is for macOS only${NC}"
    echo "For Linux, use install_deps_ubuntu.sh"
    exit 1
fi

# Check if Homebrew is installed
echo -e "${YELLOW}[1/3] Checking Homebrew...${NC}"
if ! command -v brew &> /dev/null; then
    echo -e "${RED}Homebrew not found${NC}"
    echo "Install Homebrew first:"
    echo '  /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"'
    exit 1
fi
echo -e "${GREEN}✓ Homebrew found${NC}"

# Update Homebrew
echo -e "${YELLOW}[2/3] Updating Homebrew...${NC}"
brew update
echo -e "${GREEN}✓ Homebrew updated${NC}"

# Install dependencies
echo -e "${YELLOW}[3/3] Installing dependencies...${NC}"
echo "This will install: cmake, boost, eigen, curl, nlohmann-json, fmt, range-v3"
echo ""

brew install \
    cmake \
    boost \
    eigen \
    curl \
    nlohmann-json \
    fmt \
    range-v3

echo -e "${GREEN}✓ All dependencies installed${NC}"

# Check Xcode Command Line Tools
echo ""
echo -e "${YELLOW}Checking Xcode Command Line Tools...${NC}"
if xcode-select -p &> /dev/null; then
    echo -e "${GREEN}✓ Xcode Command Line Tools installed${NC}"
else
    echo -e "${YELLOW}⚠ Xcode Command Line Tools not found${NC}"
    echo "Install with: xcode-select --install"
fi

# Summary
echo ""
echo -e "${GREEN}╔════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║     Dependencies Installed!            ║${NC}"
echo -e "${GREEN}╚════════════════════════════════════════╝${NC}"
echo ""
echo "Installed packages (via Homebrew):"
echo "  ✓ cmake"
echo "  ✓ boost"
echo "  ✓ eigen"
echo "  ✓ curl"
echo "  ✓ nlohmann-json"
echo "  ✓ fmt"
echo "  ✓ range-v3"
echo ""
echo "Next steps:"
echo -e "  ${BLUE}1.${NC} Build GEL:  ${GREEN}cd .. && ./build_scripts/build_unix.sh${NC}"
echo ""
echo "Note: CMake will use system-installed libraries (no vcpkg needed)"
