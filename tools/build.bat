@echo off
REM TriCord Build Script for Windows
REM Requires: devkitARM (via MSYS2/devkitPro), 3ds-curl, 3ds-mbedtls, 3ds-zlib, bannertool, makerom
setlocal enabledelayedexpansion

set "TOOLS_DIR=%~dp0"
if "%TOOLS_DIR:~-1%"=="\" set "TOOLS_DIR=%TOOLS_DIR:~0,-1%"

set "ROOT_DIR=%TOOLS_DIR%\.."
cd /d "%ROOT_DIR%"

set "PATH=%TOOLS_DIR%;%PATH%"

REM Override DEVKITARM and DEVKITPRO if they are set to linux paths like /opt/...
echo "%DEVKITARM%" | findstr /i /c:"/opt/devkitpro" >nul
if not errorlevel 1 (
    set "DEVKITARM="
    set "DEVKITPRO="
)

REM Check devkitARM
if not defined DEVKITARM (
    if exist "C:\devkitPro\devkitARM" (
        set "DEVKITPRO=C:\devkitPro"
        set "DEVKITARM=C:\devkitPro\devkitARM"
    ) else (
        echo ERROR: devkitARM not found in C:\devkitPro\devkitARM.
        echo Install it from https://devkitpro.org/wiki/Getting_Started
        pause
        exit /b 1
    )
)

set "PATH=!DEVKITARM!\bin;!DEVKITPRO!\tools\bin;!DEVKITPRO!\msys2\usr\bin;%PATH%"
echo === devkitARM: %DEVKITARM% ===

REM Install dependencies automatically via pacman if missing
echo === Checking/Installing library dependencies (pacman) ===
pacman -S --needed --noconfirm 3ds-curl 3ds-mbedtls 3ds-zlib 3ds-pkg-config 3ds-libopus
if errorlevel 1 (
    echo WARNING: pacman failed to install dependencies. You may need to run this manually in MSYS2:
    echo pacman -S 3ds-curl 3ds-mbedtls 3ds-zlib 3ds-pkg-config 3ds-libopus
)

REM Build libsodium from source (not available in pacman)
echo === Checking/Building libsodium ===
bash "%TOOLS_DIR%\setup_libsodium.sh"
if errorlevel 1 (
    echo WARNING: libsodium build failed. You may need to run tools/setup_libsodium.sh manually in MSYS2.
)

REM Check makerom
where makerom >nul 2>&1
if errorlevel 1 (
    echo ERROR: makerom.exe not found.
    echo Download it from: https://github.com/3DSGuy/Project_CTR/releases
    echo Place makerom.exe in the tools\ folder.
    pause
    exit /b 1
)
echo === makerom OK ===

REM Check bannertool
where bannertool >nul 2>&1
if errorlevel 1 (
    echo ERROR: bannertool.exe not found.
    echo Download it from: https://github.com/carstene1ns/3ds-bannertool/releases
    echo Place bannertool.exe in the tools\ folder.
    pause
    exit /b 1
)
echo === bannertool OK ===

REM Check make
where make >nul 2>&1
if errorlevel 1 (
    echo ERROR: make.exe not found.
    echo Ensure MSYS2 is installed in devkitPro.
    pause
    exit /b 1
)

REM Build using MSYS2 make from devkitPro
echo === Building TriCord ===
make
if errorlevel 1 (
    echo.
    echo === BUILD FAILED ===
    pause
    exit /b 1
)

echo.
echo === Build complete! ===
if exist "TriCord.3dsx" (echo   TriCord.3dsx OK) else (echo   TriCord.3dsx MISSING)
if exist "TriCord.cia" (echo   TriCord.cia  OK) else (echo   TriCord.cia  MISSING)

pause
endlocal
