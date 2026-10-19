@echo off
setlocal EnableDelayedExpansion

REM Always run from the script directory.
cd /d "%~dp0"

REM Ensure Visual Studio environment is initialized if running from normal shell
if "%INCLUDE%"=="" (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VS_DIR=%%i"
    )
    if exist "!VS_DIR!\VC\Auxiliary\Build\vcvars64.bat" (
        echo [INFO] Initializing Visual Studio environment from !VS_DIR!...
        call "!VS_DIR!\VC\Auxiliary\Build\vcvars64.bat"
    )
)

REM ---------------------------------------------------------------------------
REM Configurable build options
REM   GENERATOR  - CMake generator (default: auto-detect)
REM   ARCH       - Architecture for multi-config generators (default: x64)
REM   CONFIGS    - Configurations to build, space-separated (default: Release Debug)
REM ---------------------------------------------------------------------------
if "%ARCH%"=="" set "ARCH=x64"
if "%CONFIGS%"=="" set "CONFIGS=Release Debug"

REM Configure CMake generator
if exist "build\CMakeCache.txt" (
    if not "%GENERATOR%"=="" (
        echo [INFO] Configuring with explicit generator: %GENERATOR%
        echo "%GENERATOR%" | findstr /I /C:"Visual Studio" >nul
        if not errorlevel 1 (
            cmake -S src -B build -G "%GENERATOR%" -A %ARCH%
        ) else (
            cmake -S src -B build -G "%GENERATOR%"
        )
    ) else (
        echo [INFO] Existing build cache found, updating configuration...
        cmake -S src -B build
    )
) else (
    if not "%GENERATOR%"=="" (
        echo [INFO] Configuring with explicit generator: %GENERATOR%
        echo "%GENERATOR%" | findstr /I /C:"Visual Studio" >nul
        if not errorlevel 1 (
            cmake -S src -B build -G "%GENERATOR%" -A %ARCH%
        ) else (
            cmake -S src -B build -G "%GENERATOR%"
        )
    ) else (
        echo [INFO] Configuring with default generator for architecture %ARCH%...
        cmake -S src -B build -A %ARCH%
    )
)
if errorlevel 1 goto :fail

for %%C in (%CONFIGS%) do (
    echo [INFO] Building: %%C
    cmake --build build --config %%C
    if errorlevel 1 goto :fail

    REM extract_tickets is EXCLUDE_FROM_ALL, so build it explicitly. It lands in
    REM build\tools\%%C\ rather than the shipped output directory.
    echo [INFO] Building tool extract_tickets for %%C
    cmake --build build --config %%C --target extract_tickets
    if errorlevel 1 goto :fail
)

echo.
echo ===========================================================================
echo [OK] Build completed successfully.
echo Output DLLs:
if exist "build\Release\SteamDaddy.dll" echo   [Release] build\Release\SteamDaddy.dll, dwmapi.dll, version.dll
if exist "build\Debug\SteamDaddy.dll"   echo   [Debug]   build\Debug\SteamDaddy.dll, dwmapi.dll, version.dll
echo ===========================================================================
exit /b 0

:fail
echo [ERROR] Build failed.
exit /b 1
