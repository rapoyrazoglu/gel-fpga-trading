# GEL Trading System - PowerShell Build Script
# Usage: .\build.ps1 [-BuildType Debug|Release] [-Clean] [-RunTests]

param(
    [ValidateSet("Debug", "Release")]
    [string]$BuildType = "Debug",
    [switch]$Clean,
    [switch]$RunTests
)

$ErrorActionPreference = "Stop"

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  GEL Trading System - Build Script" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Get project root (parent of build_scripts)
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir

# Add common CMake paths to PATH
$cmakePaths = @(
    "C:\Program Files\CMake\bin",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin",
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin",
    "C:\Program Files\JetBrains\CLion 2025.1.6\bin\cmake\win\x64\bin",
    "$env:USERPROFILE\AppData\Local\Programs\CMake\bin"
)

foreach ($path in $cmakePaths) {
    if (Test-Path $path) {
        $env:PATH = "$path;$env:PATH"
        break
    }
}

Write-Host "[1/6] Checking CMake..." -ForegroundColor Yellow
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    Write-Host "Error: CMake not found" -ForegroundColor Red
    Write-Host "Install from: https://cmake.org/download/" -ForegroundColor Yellow
    Write-Host "Searched paths:" -ForegroundColor Yellow
    foreach ($path in $cmakePaths) {
        Write-Host "  - $path" -ForegroundColor Gray
    }
    exit 1
}
$cmakeVersion = cmake --version | Select-Object -First 1
Write-Host "  OK - $cmakeVersion" -ForegroundColor Green

Write-Host "[2/6] Checking vcpkg..." -ForegroundColor Yellow
$vcpkgRoot = $env:VCPKG_ROOT
if (-not $vcpkgRoot) {
    if (Test-Path "C:\vcpkg") {
        $vcpkgRoot = "C:\vcpkg"
    } elseif (Test-Path "$env:USERPROFILE\vcpkg") {
        $vcpkgRoot = "$env:USERPROFILE\vcpkg"
    }
}

if (-not $vcpkgRoot -or -not (Test-Path "$vcpkgRoot\scripts\buildsystems\vcpkg.cmake")) {
    Write-Host "Error: vcpkg not found" -ForegroundColor Red
    Write-Host "Set VCPKG_ROOT environment variable or install vcpkg to C:\vcpkg" -ForegroundColor Yellow
    exit 1
}
Write-Host "  OK - vcpkg found at: $vcpkgRoot" -ForegroundColor Green

Write-Host "[3/6] Build type: $BuildType" -ForegroundColor Yellow

# Clean if requested
if ($Clean) {
    Write-Host "[4/6] Cleaning old build artifacts..." -ForegroundColor Yellow
    $buildDir = Join-Path $ProjectRoot "build"
    if (Test-Path $buildDir) {
        Remove-Item -Recurse -Force $buildDir
        Write-Host "  OK - Cleaned build directory" -ForegroundColor Green
    } else {
        Write-Host "  OK - Nothing to clean" -ForegroundColor Green
    }
} else {
    Write-Host "[4/6] Skipping clean (use -Clean to force)" -ForegroundColor Yellow
}

# Create build directory
$buildDir = Join-Path $ProjectRoot "build"
if (-not (Test-Path $buildDir)) {
    New-Item -ItemType Directory -Path $buildDir | Out-Null
}

# Configure with CMake
Write-Host "[5/6] Running CMake configuration..." -ForegroundColor Yellow
Push-Location $buildDir

try {
    $cmakeArgs = @(
        $ProjectRoot,
        "-DCMAKE_BUILD_TYPE=$BuildType",
        "-DCMAKE_TOOLCHAIN_FILE=$vcpkgRoot\scripts\buildsystems\vcpkg.cmake"
    )

    # Try to use Ninja if available, otherwise default generator
    $ninja = Get-Command ninja -ErrorAction SilentlyContinue
    if ($ninja) {
        $cmakeArgs += "-G", "Ninja"
        Write-Host "  Using Ninja generator" -ForegroundColor Gray
    } else {
        Write-Host "  Using default generator (Visual Studio)" -ForegroundColor Gray
    }

    & cmake @cmakeArgs

    if ($LASTEXITCODE -ne 0) {
        Write-Host "Error: CMake configuration failed" -ForegroundColor Red
        exit 1
    }
    Write-Host "  OK - CMake configuration successful" -ForegroundColor Green

    # Build
    Write-Host "[6/6] Compiling..." -ForegroundColor Yellow
    $buildArgs = @("--build", ".", "--config", $BuildType, "--parallel")
    & cmake @buildArgs

    if ($LASTEXITCODE -ne 0) {
        Write-Host "Error: Build failed" -ForegroundColor Red
        exit 1
    }
} finally {
    Pop-Location
}

# Summary
Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host "       BUILD SUCCESSFUL!" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host ""
Write-Host "Build location: $buildDir" -ForegroundColor Cyan
Write-Host ""
Write-Host "Executables built:" -ForegroundColor Cyan

$exePaths = @(
    "$buildDir\$BuildType\all_tests.exe",
    "$buildDir\$BuildType\GEL.exe",
    "$buildDir\$BuildType\live_trader.exe",
    "$buildDir\$BuildType\cpp_backtest.exe",
    "$buildDir\all_tests.exe",
    "$buildDir\GEL.exe",
    "$buildDir\live_trader.exe",
    "$buildDir\cpp_backtest.exe"
)

foreach ($exe in $exePaths) {
    if (Test-Path $exe) {
        Write-Host "  - $exe" -ForegroundColor White
    }
}

# Run tests if requested
if ($RunTests) {
    Write-Host ""
    Write-Host "Running tests..." -ForegroundColor Yellow

    $testExe = $null
    foreach ($path in @("$buildDir\$BuildType\all_tests.exe", "$buildDir\all_tests.exe")) {
        if (Test-Path $path) {
            $testExe = $path
            break
        }
    }

    if ($testExe) {
        & $testExe
        if ($LASTEXITCODE -ne 0) {
            Write-Host "Some tests failed!" -ForegroundColor Red
            exit $LASTEXITCODE
        }
        Write-Host "All tests passed!" -ForegroundColor Green
    } else {
        Write-Host "Warning: all_tests.exe not found" -ForegroundColor Yellow
    }
}

Write-Host ""
Write-Host "Next steps:" -ForegroundColor Cyan
Write-Host "  Run tests:     .\build_scripts\build.ps1 -RunTests" -ForegroundColor White
Write-Host "  Release build: .\build_scripts\build.ps1 -BuildType Release" -ForegroundColor White
Write-Host "  Clean build:   .\build_scripts\build.ps1 -Clean" -ForegroundColor White
Write-Host ""
Write-Host "Live Trading:" -ForegroundColor Cyan
Write-Host "  1. Set OANDA credentials:" -ForegroundColor White
Write-Host "     `$env:OANDA_API_TOKEN = 'your-token'" -ForegroundColor Gray
Write-Host "     `$env:OANDA_ACCOUNT_ID = 'your-account-id'" -ForegroundColor Gray
Write-Host "  2. Run live trader:" -ForegroundColor White
Write-Host "     .\build\$BuildType\live_trader.exe" -ForegroundColor Gray
Write-Host ""
