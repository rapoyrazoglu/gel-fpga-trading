#!/bin/bash
# GEL Trading System - Clean Build Artifacts
# Removes all build directories and artifacts

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}╔════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  GEL - Clean Build Artifacts           ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════╝${NC}"
echo ""

# Get script directory and project root
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$( cd "$SCRIPT_DIR/.." && pwd )"

cd "$PROJECT_ROOT"

echo -e "${YELLOW}Cleaning build artifacts from: $PROJECT_ROOT${NC}"
echo ""

# List what will be deleted
echo "Will delete:"
DELETED=0

if [ -d "build" ]; then
    echo "  - build/"
    ((DELETED++))
fi

if [ -d "cmake-build-debug" ]; then
    echo "  - cmake-build-debug/"
    ((DELETED++))
fi

if [ -d "cmake-build-release" ]; then
    echo "  - cmake-build-release/"
    ((DELETED++))
fi

if [ -d "Debug" ]; then
    echo "  - Debug/"
    ((DELETED++))
fi

if [ -d "Release" ]; then
    echo "  - Release/"
    ((DELETED++))
fi

if [ -d "out" ]; then
    echo "  - out/"
    ((DELETED++))
fi

# Find .o, .a, .so files
OBJ_FILES=$(find . -type f \( -name "*.o" -o -name "*.a" -o -name "*.so" -o -name "*.dylib" \) 2>/dev/null | wc -l)
if [ "$OBJ_FILES" -gt 0 ]; then
    echo "  - $OBJ_FILES object/library files (*.o, *.a, *.so, *.dylib)"
    ((DELETED++))
fi

if [ "$DELETED" -eq 0 ]; then
    echo -e "${GREEN}Nothing to clean - already clean!${NC}"
    exit 0
fi

echo ""
read -p "Proceed with deletion? (y/N): " -n 1 -r
echo

if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo -e "${YELLOW}Cancelled${NC}"
    exit 0
fi

# Delete directories
echo ""
echo -e "${YELLOW}Cleaning...${NC}"

rm -rf build cmake-build-debug cmake-build-release Debug Release out

# Delete object files
find . -type f \( -name "*.o" -o -name "*.a" -o -name "*.so" -o -name "*.dylib" \) -delete 2>/dev/null || true

echo -e "${GREEN}✓ All build artifacts deleted${NC}"

# Summary
echo ""
echo -e "${GREEN}╔════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║         Clean Complete!                ║${NC}"
echo -e "${GREEN}╚════════════════════════════════════════╝${NC}"
echo ""
echo "Next steps:"
echo -e "  ${BLUE}1.${NC} Rebuild:  ${GREEN}./build_scripts/build_unix.sh${NC}"
echo ""
