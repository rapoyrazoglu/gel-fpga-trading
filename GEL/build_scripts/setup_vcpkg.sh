#!/bin/bash
# GEL Trading System - vcpkg Setup Script
# This script automatically installs vcpkg and all required dependencies

set -e  # Exit on error

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}╔════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  GEL - vcpkg Setup Script              ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════╝${NC}"
echo ""

# Check if vcpkg already exists
if [ -d "$HOME/vcpkg" ]; then
    echo -e "${YELLOW}vcpkg already exists at $HOME/vcpkg${NC}"
    read -p "Do you want to reinstall? (y/N): " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo -e "${BLUE}Using existing vcpkg installation${NC}"
        export VCPKG_ROOT="$HOME/vcpkg"
        cd "$HOME/vcpkg"
        git pull
    else
        rm -rf "$HOME/vcpkg"
    fi
fi

# Install vcpkg if not exists
if [ ! -d "$HOME/vcpkg" ]; then
    echo -e "${YELLOW}[1/4] Cloning vcpkg...${NC}"
    cd ~
    git clone https://github.com/Microsoft/vcpkg.git
    echo -e "${GREEN}✓ vcpkg cloned${NC}"

    echo -e "${YELLOW}[2/4] Bootstrapping vcpkg...${NC}"
    cd vcpkg
    ./bootstrap-vcpkg.sh
    echo -e "${GREEN}✓ vcpkg bootstrapped${NC}"
else
    cd "$HOME/vcpkg"
fi

# Export VCPKG_ROOT
export VCPKG_ROOT="$HOME/vcpkg"

# Install dependencies
echo -e "${YELLOW}[3/4] Installing dependencies (this may take 10-15 minutes)...${NC}"
echo "Dependencies: boost-headers, eigen3, curl, nlohmann-json, fmt, range-v3"

./vcpkg install \
    boost-headers \
    eigen3 \
    curl \
    nlohmann-json \
    fmt \
    range-v3

echo -e "${GREEN}✓ All dependencies installed${NC}"

# Setup environment variable
echo -e "${YELLOW}[4/4] Setting up environment variable...${NC}"

SHELL_RC=""
if [ -n "$ZSH_VERSION" ]; then
    SHELL_RC="$HOME/.zshrc"
elif [ -n "$BASH_VERSION" ]; then
    SHELL_RC="$HOME/.bashrc"
fi

if [ -n "$SHELL_RC" ]; then
    if ! grep -q "VCPKG_ROOT" "$SHELL_RC"; then
        echo "" >> "$SHELL_RC"
        echo "# vcpkg for GEL Trading System" >> "$SHELL_RC"
        echo "export VCPKG_ROOT=\$HOME/vcpkg" >> "$SHELL_RC"
        echo -e "${GREEN}✓ Added VCPKG_ROOT to $SHELL_RC${NC}"
        echo -e "${YELLOW}  Run: source $SHELL_RC${NC}"
    else
        echo -e "${GREEN}✓ VCPKG_ROOT already in $SHELL_RC${NC}"
    fi
fi

# Summary
echo ""
echo -e "${GREEN}╔════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║     vcpkg Setup Complete!              ║${NC}"
echo -e "${GREEN}╚════════════════════════════════════════╝${NC}"
echo ""
echo "vcpkg location: $HOME/vcpkg"
echo "Installed packages:"
echo "  ✓ boost-headers"
echo "  ✓ eigen3"
echo "  ✓ curl"
echo "  ✓ nlohmann-json"
echo "  ✓ fmt"
echo "  ✓ range-v3"
echo ""
echo "Next steps:"
echo -e "  ${BLUE}1.${NC} Reload shell:  ${GREEN}source $SHELL_RC${NC}"
echo -e "  ${BLUE}2.${NC} Build GEL:     ${GREEN}cd .. && ./build_scripts/build_unix.sh${NC}"
echo ""
