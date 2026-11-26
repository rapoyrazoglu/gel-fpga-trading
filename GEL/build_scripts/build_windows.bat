@echo off
REM GEL Trading System - Windows Build Script
REM This script automates the build process on Windows using vcpkg and MSVC

setlocal enabledelayedexpansion

echo ========================================
echo   GEL Trading System - Build Script
echo ========================================
echo.

REM Check if running on Windows
if not "%OS%"=="Windows_NT" (
    echo Error: This script is for Windows only
    echo For Unix/Linux/macOS, use build_unix.sh
    exit /b 1
)

REM Check for CMake
echo [1/5] Checking CMake...
where cmake >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo Error: CMake not found
    echo Install: https://cmake.org/download/
    exit /b 1
)
cmake --version | findstr /R "version" >nul
echo   OK - CMake found

REM Check for MSVC (Visual Studio)
echo [2/5] Checking MSVC...
where cl >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo Error: MSVC compiler not found
    echo Install Visual Studio 2022 with C++ workload
    echo Or run from "Developer Command Prompt for VS 2022"
    exit /b 1
)
echo   OK - MSVC found

REM Check for vcpkg
echo [3/5] Checking vcpkg...
if defined VCPKG_ROOT (
    echo   OK - vcpkg found at: %VCPKG_ROOT%
) else if exist "C:\vcpkg" (
    set VCPKG_ROOT=C:\vcpkg
    echo   OK - vcpkg found at: C:\vcpkg
) else (
    echo Warning: vcpkg not found
    echo Install vcpkg:
    echo   git clone https://github.com/Microsoft/vcpkg.git C:\vcpkg
    echo   C:\vcpkg\bootstrap-vcpkg.bat
    echo   C:\vcpkg\vcpkg install boost-headers eigen3 curl nlohmann-json fmt range-v3
    exit /b 1
)

REM Get build type (default: Debug)
set BUILD_TYPE=%1
if "%BUILD_TYPE%"=="" set BUILD_TYPE=Debug
if not "%BUILD_TYPE%"=="Debug" if not "%BUILD_TYPE%"=="Release" (
    echo Error: Build type must be Debug or Release
    echo Usage: build_windows.bat [Debug^|Release]
    exit /b 1
)
echo [4/5] Build type: %BUILD_TYPE%

REM Get script directory and project root
set SCRIPT_DIR=%~dp0
set PROJECT_ROOT=%SCRIPT_DIR%..

REM Clean old builds
echo [5/5] Cleaning old build artifacts...
cd /d "%PROJECT_ROOT%"
if exist build rmdir /s /q build
if exist cmake-build-debug rmdir /s /q cmake-build-debug
if exist cmake-build-release rmdir /s /q cmake-build-release
if exist Debug rmdir /s /q Debug
if exist Release rmdir /s /q Release
echo   OK - Cleaned

REM Configure and build
echo.
echo Building GEL Trading System...
mkdir build
cd build

REM Configure with CMake
echo   - Running CMake configuration...
cmake "%PROJECT_ROOT%" -DCMAKE_BUILD_TYPE=%BUILD_TYPE% -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake"
if %ERRORLEVEL% neq 0 (
    echo.
    echo Error: CMake configuration failed
    echo See errors above. Common issues:
    echo   1. Missing dependencies - install via vcpkg
    echo   2. Wrong compiler - run from VS Developer Command Prompt
    exit /b 1
)
echo   OK - CMake configuration successful

REM Build
echo   - Compiling...
cmake --build . --config %BUILD_TYPE% --parallel
if %ERRORLEVEL% neq 0 (
    echo.
    echo Error: Build failed
    echo Check compilation errors above
    exit /b 1
)

REM Summary
echo.
echo ========================================
echo       BUILD SUCCESSFUL!
echo ========================================
echo.
echo Build location: %PROJECT_ROOT%\build\
echo.
echo Executables built:
if exist %BUILD_TYPE%\GEL.exe echo   - GEL.exe
if exist %BUILD_TYPE%\all_tests.exe echo   - all_tests.exe
if exist %BUILD_TYPE%\live_trader.exe echo   - live_trader.exe
if exist %BUILD_TYPE%\cpp_backtest.exe echo   - cpp_backtest.exe
if exist %BUILD_TYPE%\cpp_ema_backtest.exe echo   - cpp_ema_backtest.exe
if exist %BUILD_TYPE%\sma_test.exe echo   - sma_test.exe
if exist %BUILD_TYPE%\file_cleaner.exe echo   - file_cleaner.exe
if exist %BUILD_TYPE%\create_sample_data.exe echo   - create_sample_data.exe

echo.
echo Next steps:
echo   1. Run tests:    cd build\%BUILD_TYPE% ^&^& all_tests.exe
echo   2. Run GEL:      cd build\%BUILD_TYPE% ^&^& GEL.exe
echo.
echo Live Trading:
echo   1. Set OANDA credentials:
echo      set OANDA_API_TOKEN=your-token
echo      set OANDA_ACCOUNT_ID=your-account-id
echo   2. Run: build\%BUILD_TYPE%\live_trader.exe
echo.

endlocal
