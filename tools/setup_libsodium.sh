#!/bin/bash
# Build libsodium for 3DS (devkitARM cross-compilation)
# This script downloads, configures, compiles and installs libsodium
# into the devkitPro portlibs directory for 3DS.

set -e

SODIUM_VERSION="1.0.20"
SODIUM_URL="https://download.libsodium.org/libsodium/releases/libsodium-${SODIUM_VERSION}.tar.gz"

# Setup devkitPro env
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITARM="${DEVKITARM:-${DEVKITPRO}/devkitARM}"
export PATH="${DEVKITARM}/bin:${DEVKITPRO}/tools/bin:${PATH}"

PORTLIBS="${DEVKITPRO}/portlibs/3ds"

# Check if already installed
if [ -f "${PORTLIBS}/lib/libsodium.a" ]; then
    echo "=== libsodium already installed in portlibs ==="
    exit 0
fi

echo "=== Building libsodium ${SODIUM_VERSION} for 3DS ==="

WORK_DIR="$(mktemp -d)"
cd "${WORK_DIR}"

echo "--- Downloading libsodium-${SODIUM_VERSION}..."
curl -L -o libsodium.tar.gz "${SODIUM_URL}"
tar xzf libsodium.tar.gz
cd "libsodium-${SODIUM_VERSION}"

echo "--- Configuring for ARM (3DS)..."
./configure \
    --host=arm-none-eabi \
    --prefix="${PORTLIBS}" \
    --disable-shared \
    --enable-static \
    --disable-ssp \
    --disable-asm \
    CFLAGS="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -O2 -ffunction-sections -fdata-sections"

echo "--- Compiling..."
make -j$(nproc 2>/dev/null || echo 2)

echo "--- Installing to ${PORTLIBS}..."
make install

echo "=== libsodium ${SODIUM_VERSION} installed successfully ==="

# Cleanup
cd /
rm -rf "${WORK_DIR}"
