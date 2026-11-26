# GEL Trading System - Quick Build Guide

## 🚀 One-Command Build

### macOS
```bash
./build_scripts/build_unix.sh
```

### Linux
```bash
./build_scripts/build_unix.sh
```

### Windows
```batch
build_scripts\build_windows.bat
```

---

## 📦 First-Time Setup

### macOS

**Option 1: Using vcpkg (Recommended)**
```bash
./build_scripts/setup_vcpkg.sh
./build_scripts/build_unix.sh
```

**Option 2: Using Homebrew**
```bash
./build_scripts/install_deps_macos.sh
./build_scripts/build_unix.sh
```

### Linux (Ubuntu/Debian)

**Option 1: Using vcpkg**
```bash
./build_scripts/setup_vcpkg.sh
./build_scripts/build_unix.sh
```

**Option 2: Using apt**
```bash
./build_scripts/install_deps_ubuntu.sh
./build_scripts/build_unix.sh
```

### Windows

**Prerequisites:**
- Visual Studio 2022 with C++ workload
- vcpkg installed at `C:\vcpkg`

**Build:**
```batch
REM From "Developer Command Prompt for VS 2022"
build_scripts\build_windows.bat
```

---

## 📂 Build Output

After building, executables are in:
```
GEL/
└── build/
    ├── GEL              (main application)
    ├── all_tests        (test suite)
    ├── sma_test         (FPGA SMA test)
    ├── file_cleaner     (utility)
    └── create_sample_data (utility)
```

**Run tests:**
```bash
cd build && ./all_tests
```

---

## 🛠️ Useful Scripts

Located in `build_scripts/`:

| Script | Description |
|--------|-------------|
| `build_unix.sh` | Build on Unix/Linux/macOS |
| `build_windows.bat` | Build on Windows |
| `setup_vcpkg.sh` | Auto-install vcpkg + dependencies |
| `install_deps_macos.sh` | Install via Homebrew (macOS) |
| `install_deps_ubuntu.sh` | Install via apt (Ubuntu) |
| `clean_build.sh` | Clean all build artifacts |

---

## 📖 Detailed Documentation

For detailed build instructions, troubleshooting, and platform-specific notes, see:

**[build_scripts/README.md](build_scripts/README.md)**

---

## ⚙️ Build Options

### Debug Build (Default)
```bash
./build_scripts/build_unix.sh Debug
```

### Release Build (Optimized)
```bash
./build_scripts/build_unix.sh Release
```

### Enable Sanitizers (Debugging)
```bash
cd build
cmake .. -DENABLE_SANITIZERS=ON
make -j$(nproc)
```

### Enable Native CPU Optimization
```bash
cd build
cmake .. -DENABLE_NATIVE_OPTIMIZATION=ON
make -j$(nproc)
```

---

## 🧹 Clean Build

Remove all build artifacts:
```bash
./build_scripts/clean_build.sh
```

---

## ❓ Troubleshooting

### "vcpkg not found"
- Install: `./build_scripts/setup_vcpkg.sh`
- Or use system packages: `./build_scripts/install_deps_macos.sh` (macOS) or `./build_scripts/install_deps_ubuntu.sh` (Linux)

### "CMake configuration failed"
- Check compiler version: `g++ --version` or `clang++ --version`
- Need GCC 12+ or Clang 15+
- macOS: Install Xcode Command Line Tools: `xcode-select --install`

### "C++23 features not available"
- Upgrade compiler (see [build_scripts/README.md](build_scripts/README.md))

### Build still fails?
- See detailed troubleshooting: [build_scripts/README.md](build_scripts/README.md)

---

## 📌 Quick Reference

**Make scripts executable (first time only):**
```bash
chmod +x build_scripts/*.sh
```

**Build from any location:**
```bash
cd /path/to/GEL
./build_scripts/build_unix.sh
```

**Build location is always:** `GEL/build/`

---

**Last Updated**: 2025-01-10
**For detailed docs**: See [build_scripts/README.md](build_scripts/README.md)
