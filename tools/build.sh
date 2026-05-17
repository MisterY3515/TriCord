#!/bin/bash
# TriCord Build Script for Linux/macOS
# Requires: devkitARM, 3ds-curl, 3ds-mbedtls, 3ds-zlib, 3ds-libopus, 3ds-libsodium, bannertool, makerom
set -e

TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$TOOLS_DIR")"

cd "$ROOT_DIR"

# Check devkitARM
if [ -z "$DEVKITARM" ]; then
    if [ -d "/opt/devkitpro/devkitARM" ]; then
        export DEVKITPRO=/opt/devkitpro
        export DEVKITARM=$DEVKITPRO/devkitARM
        export PATH=$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH
    else
        echo "ERROR: devkitARM not found. Install it from https://devkitpro.org/wiki/Getting_Started"
        exit 1
    fi
fi

echo "=== devkitARM: $DEVKITARM ==="

export PATH="$TOOLS_DIR:$PATH"

# Check/install makerom
if ! command -v makerom &> /dev/null; then
    echo "=== Installing makerom ==="
    ARCH=$(uname -m)
    if [ "$ARCH" = "x86_64" ]; then
        wget -q https://github.com/3DSGuy/Project_CTR/releases/download/makerom-v0.18.3/makerom-v0.18.3-ubuntu_x86_64.zip -O "$TOOLS_DIR/makerom.zip"
        unzip -o "$TOOLS_DIR/makerom.zip" -d "$TOOLS_DIR"
        chmod +x "$TOOLS_DIR/makerom"
    else
        echo "ERROR: Unsupported architecture $ARCH for makerom auto-install. Install manually."
        exit 1
    fi
fi
echo "=== makerom: $(which makerom) ==="

# Check/install bannertool
if ! command -v bannertool &> /dev/null; then
    echo "=== Building bannertool from source ==="
    wget -q https://github.com/carstene1ns/3ds-bannertool/archive/refs/heads/master.zip -O "$TOOLS_DIR/bannertool-src.zip"
    unzip -o "$TOOLS_DIR/bannertool-src.zip" -d "$TOOLS_DIR"
    BTDIR=$(find "$TOOLS_DIR" -maxdepth 1 -type d -name '*bannertool*' | head -1)
    (cd "$BTDIR" && make)
    BTBIN=$(find "$BTDIR" -name "bannertool" -type f -executable | head -1)
    if [ -z "$BTBIN" ]; then
        BTBIN=$(find "$BTDIR" -name "bannertool" -type f | head -1)
    fi
    cp "$BTBIN" "$TOOLS_DIR/bannertool"
    chmod +x "$TOOLS_DIR/bannertool"
fi
echo "=== bannertool: $(which bannertool) ==="

# Build
echo "=== Building TriCord ==="
make

echo ""
echo "=== Build complete! ==="
[ -f "TriCord.3dsx" ] && echo "  TriCord.3dsx OK" || echo "  TriCord.3dsx MISSING"
[ -f "TriCord.cia" ] && echo "  TriCord.cia  OK" || echo "  TriCord.cia  MISSING"
