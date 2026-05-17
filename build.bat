@echo off
REM TriCord Build Script for Windows
REM Requires: devkitARM (via MSYS2/devkitPro), 3ds-curl, 3ds-mbedtls, 3ds-zlib, bannertool, makerom
setlocal enabledelayedexpansion

set "TOOLS_DIR=%~dp0tools"
if not exist "%TOOLS_DIR%" mkdir "%TOOLS_DIR%"

REM Check devkitARM
if not defined DEVKITARM (
    if exist "C:\devkitPro\devkitARM" (
        set "DEVKITPRO=C:\devkitPro"
        set "DEVKITARM=C:\devkitPro\devkitARM"
        set "PATH=%DEVKITARM%\bin;%DEVKITPRO%\tools\bin;%PATH%"
    ) else (
        echo ERROR: devkitARM not found.
        echo Install it from https://devkitpro.org/wiki/Getting_Started
        echo Or set DEVKITARM environment variable manually.
        exit /b 1
    )
)

echo === devkitARM: %DEVKITARM% ===

REM Check makerom
where makerom >nul 2>&1
if errorlevel 1 (
    echo === makerom not found in PATH ===
    if exist "%TOOLS_DIR%\makerom.exe" (
        set "PATH=%TOOLS_DIR%;%PATH%"
        echo === Using makerom from tools/ ===
    ) else (
        echo ERROR: makerom.exe not found.
        echo Download it from: https://github.com/3DSGuy/Project_CTR/releases
        echo Place makerom.exe in the tools\ folder.
        exit /b 1
    )
)
echo === makerom OK ===

REM Check bannertool
where bannertool >nul 2>&1
if errorlevel 1 (
    echo === bannertool not found in PATH ===
    if exist "%TOOLS_DIR%\bannertool.exe" (
        set "PATH=%TOOLS_DIR%;%PATH%"
        echo === Using bannertool from tools/ ===
    ) else (
        echo ERROR: bannertool.exe not found.
        echo Download it from: https://github.com/carstene1ns/3ds-bannertool/releases
        echo Place bannertool.exe in the tools\ folder.
        exit /b 1
    )
)
echo === bannertool OK ===

REM Build using MSYS2 make from devkitPro
echo === Building TriCord ===
make
if errorlevel 1 (
    echo.
    echo === BUILD FAILED ===
    exit /b 1
)

echo.
echo === Build complete! ===
if exist "TriCord.3dsx" (echo   TriCord.3dsx OK) else (echo   TriCord.3dsx MISSING)
if exist "TriCord.cia" (echo   TriCord.cia  OK) else (echo   TriCord.cia  MISSING)

endlocal
