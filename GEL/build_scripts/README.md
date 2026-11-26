# GEL Trading System - Build Guide

Complete build instructions for all platforms (Unix/Linux/macOS/Windows).

---

## 🚀 Quick Start

### macOS (Recommended)
```bash
# Option 1: Using automated script
cd /path/to/GEL
./build_scripts/build_unix.sh

# Option 2: Install dependencies first (Homebrew)
./build_scripts/install_deps_macos.sh
./build_scripts/build_unix.sh
```

### Linux (Ubuntu/Debian)
```bash
# Option 1: Using automated script
cd /path/to/GEL
./build_scripts/build_unix.sh

# Option 2: Install dependencies first (apt)
./build_scripts/install_deps_ubuntu.sh
./build_scripts/build_unix.sh
```

### Windows
```batch
REM From Developer Command Prompt for VS 2022
cd C:\path\to\GEL
build_scripts\build_windows.bat
```

---

# Building GEL on Unix/Linux/macOS

This guide explains how to build the GEL Trading System on Unix-based systems (Linux, macOS) using CMake.

## Prerequisites

### C++ Compiler

You need a modern C++ compiler with C++23 support:

- **GCC 12+** (recommended: GCC 13)
- **Clang 15+** (recommended: Clang 16+)
- **Apple Clang 14.0.3+** (Xcode 14.3+, macOS only)

Check your compiler version:
```bash
g++ --version
# or
clang++ --version
```

### CMake

- **Minimum version**: 3.22
- **Recommended**: 3.27+

```bash
cmake --version
```

---

## Option 1: Using vcpkg (Recommended)

vcpkg is a cross-platform C++ package manager that automatically handles dependencies.

### Step 1: Install vcpkg

```bash
# Clone vcpkg to your home directory
cd ~
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg

# Bootstrap vcpkg (builds the vcpkg executable)
./bootstrap-vcpkg.sh

# Optional: Add vcpkg to PATH
echo 'export PATH="$HOME/vcpkg:$PATH"' >> ~/.bashrc
source ~/.bashrc
```

### Step 2: Install Dependencies

```bash
# Install all required libraries
~/vcpkg/vcpkg install \
    boost-headers \
    eigen3 \
    curl \
    nlohmann-json \
    fmt \
    range-v3
```

This will take 5-15 minutes depending on your system.

### Step 3: Set Environment Variable

```bash
# Set VCPKG_ROOT (required for CMake to find vcpkg)
export VCPKG_ROOT=~/vcpkg

# Make it permanent (add to ~/.bashrc or ~/.zshrc)
echo 'export VCPKG_ROOT=~/vcpkg' >> ~/.bashrc
source ~/.bashrc
```

### Step 4: Build GEL

```bash
cd GEL

# Clean old build artifacts (if switching from Windows)
rm -rf cmake-build-debug

# Create build directory
mkdir build && cd build

# Configure with CMake
cmake .. -DCMAKE_BUILD_TYPE=Debug

# Build all targets
make -j$(nproc)

# Or build specific targets:
# make all_tests
# make GEL
```

### Step 5: Run Tests

```bash
# From build/ directory
./all_tests

# Run main application
./GEL
```

---

## Option 2: Using System Package Manager

If you prefer not to use vcpkg, you can install dependencies via your system's package manager.

### Ubuntu/Debian

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    g++-12 \
    libboost-all-dev \
    libeigen3-dev \
    libcurl4-openssl-dev \
    nlohmann-json3-dev \
    libfmt-dev \
    librange-v3-dev

# Set GCC 12 as default (if multiple versions installed)
sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-12 100
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-12 100
```

### macOS (Homebrew)

```bash
# Install Homebrew if not already installed
# /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

brew install cmake boost eigen curl nlohmann-json fmt range-v3
```

### Arch Linux

```bash
sudo pacman -S base-devel cmake boost eigen curl nlohmann-json fmt range-v3
```

### Fedora/RHEL

```bash
sudo dnf install gcc-c++ cmake boost-devel eigen3-devel libcurl-devel \
    json-devel fmt-devel range-v3-devel
```

### Build Without vcpkg

```bash
cd GEL

# Clean
rm -rf cmake-build-debug build

# Create build directory
mkdir build && cd build

# Configure (CMake will use system libraries)
cmake .. -DCMAKE_BUILD_TYPE=Debug

# Build
make -j$(nproc)

# Test
./all_tests
```

---

## Build Options

### Debug Build (Default)

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

### Release Build (Optimized)

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Enable Sanitizers (Debugging Memory Issues)

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
make -j$(nproc)

# Run with sanitizers
ASAN_OPTIONS=detect_leaks=1 ./all_tests
```

### Enable Native CPU Optimization

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_NATIVE_OPTIMIZATION=ON
make -j$(nproc)
```

**Warning**: `-march=native` optimizes for your CPU but binaries won't be portable to other CPUs.

### Specify Compiler

```bash
# Use GCC 13
cmake .. -DCMAKE_CXX_COMPILER=g++-13

# Use Clang 16
cmake .. -DCMAKE_CXX_COMPILER=clang++-16
```

---

## Troubleshooting

### Issue 1: "Could not find Boost"

**Solution**: Install Boost or set `Boost_ROOT`:
```bash
# With vcpkg
~/vcpkg/vcpkg install boost-headers

# Or set manually
cmake .. -DBoost_ROOT=/path/to/boost
```

### Issue 2: "C++23 features not available"

**Solution**: Upgrade your compiler:
```bash
# Ubuntu
sudo apt-get install g++-13

# macOS (requires Xcode 14.3+)
xcode-select --install
```

### Issue 3: "vcpkg not found"

**Solution**: Set `VCPKG_ROOT` or skip vcpkg:
```bash
# Set environment variable
export VCPKG_ROOT=~/vcpkg

# Or use system packages (see Option 2 above)
```

### Issue 4: Linking errors with sanitizers

**Solution**: Disable sanitizers (they don't work on all systems):
```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=OFF
```

### Issue 5: Unknown pragma warnings (HLS pragmas)

**Solution**: Already fixed in CMakeLists.txt with `-Wno-unknown-pragmas`

---

## Project Structure

After building, you'll have these executables:

```
build/
├── GEL                  # Main application
├── all_tests            # Test suite (14 tests)
├── sma_test             # FPGA SMA test
├── file_cleaner         # Utility script
└── create_sample_data   # Utility script
```

Run them from the `build/` directory:
```bash
./all_tests      # Run all 14 tests
./GEL            # Run main application
```

---

## Performance Tips

### 1. Parallel Compilation

Use all CPU cores:
```bash
make -j$(nproc)          # Linux
make -j$(sysctl -n hw.ncpu)  # macOS
```

### 2. ccache (Optional)

Speed up recompilation:
```bash
# Install ccache
sudo apt-get install ccache  # Ubuntu
brew install ccache          # macOS

# Use it
cmake .. -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
```

### 3. Ninja (Alternative to Make)

Faster build system:
```bash
# Install ninja
sudo apt-get install ninja-build  # Ubuntu
brew install ninja                # macOS

# Use it
cmake .. -G Ninja
ninja -j$(nproc)
```

---

## Verification Checklist

After building, verify everything works:

- [ ] CMake configuration succeeds (no errors)
- [ ] All executables build successfully
- [ ] Tests pass: `./all_tests` shows "12/12 (100%)" or "14/14 (100%)"
- [ ] No compilation warnings (or only minor ones)
- [ ] Main application runs: `./GEL`

---

## Clean Build

If you encounter issues, try a clean rebuild:

```bash
cd GEL

# Remove all build artifacts
rm -rf build cmake-build-debug

# Fresh build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

---

## Next Steps

After successful build:

1. **Run tests**: `./all_tests`
2. **Check test reports**: `cat test_results/test_results.md`
3. **Read documentation**: See `README.md`, `TEST_INTERPRETATION_GUIDE.md`
4. **Develop**: Modify code, rebuild with `make`, rerun tests

---

## Need Help?

- **vcpkg documentation**: https://github.com/microsoft/vcpkg
- **CMake documentation**: https://cmake.org/documentation/
- **GEL project issues**: Check `README.md` and `GEL_PROJECT_HANDBOOK.md`

---

**Last Updated**: 2025-01-10
**Tested On**: Ubuntu 22.04, macOS 14.0, Arch Linux
