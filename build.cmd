@echo off
setlocal enabledelayedexpansion

REM lvt build script — run from a VS Developer Command Prompt (x64)
REM Requires: VCPKG_ROOT set, cl.exe and ninja.exe on PATH
REM Usage: build.cmd [clean]

if "%VCPKG_ROOT%"=="" (
    echo ERROR: VCPKG_ROOT is not set.
    exit /b 1
)

where cl.exe >nul 2>&1 || (echo ERROR: cl.exe not found. Run from a VS Developer Command Prompt. & exit /b 1)
where ninja.exe >nul 2>&1 || (echo ERROR: ninja.exe not found. & exit /b 1)

if /i "%1"=="clean" (
    echo Cleaning build directory...
    if exist build rmdir /s /q build
)

REM --- .NET projects (must build before CMake) ---

echo.
echo === Building .NET projects ===

echo [1/3] LvtWpfTap (net48)...
dotnet build src\tap_wpf\LvtWpfTap.csproj -c Release -v:q --nologo
if errorlevel 1 (
    echo WARNING: LvtWpfTap build failed (WPF TAP will be unavailable)
)

echo [2/3] LvtAvaloniaTreeWalker (net8.0)...
dotnet restore src\plugin_avalonia\LvtAvaloniaTreeWalker\LvtAvaloniaTreeWalker.csproj -v:q --nologo
dotnet publish src\plugin_avalonia\LvtAvaloniaTreeWalker\LvtAvaloniaTreeWalker.csproj -c Release -v:q --nologo
if errorlevel 1 (
    echo WARNING: LvtAvaloniaTreeWalker build failed (Avalonia plugin will be unavailable)
)

echo [3/3] LvtWinFormsTap (net48)...
dotnet build src\tap_winforms\LvtWinFormsTap.csproj -c Release -v:q --nologo
if errorlevel 1 (
    echo WARNING: LvtWinFormsTap build failed (WinForms enrichment will be unavailable)
)

REM --- CMake configure + build ---

echo.
echo === Configuring CMake (x64) ===

if not exist build (
    cmake --preset default
    if errorlevel 1 (
        echo ERROR: CMake configure failed.
        exit /b 1
    )
)

echo.
echo === Building C++ targets ===

cmake --build build
if errorlevel 1 (
    echo.
    echo ERROR: C++ build failed.
    exit /b 1
)

echo.
echo === Build complete ===
echo Output: build\lvt.exe
echo         build\lvt_tap_x64.dll
echo Tests:  build\lvt_unit_tests.exe
echo         build\lvt_integration_tests.exe
