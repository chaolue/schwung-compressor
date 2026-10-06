#!/usr/bin/env bash
# Build the Compressor module for Schwung on Ableton Move (ARM64).
#
# Uses Docker for cross-compilation unless CROSS_PREFIX is set (or we are
# already inside the container). Output: dist/compressor/ and
# dist/compressor-module.tar.gz
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
MODULE_ID=compressor
IMAGE_NAME="schwung-compressor-builder"

if [ -z "${CROSS_PREFIX:-}" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Compressor build (via Docker) ==="
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$SCRIPT_DIR"
    fi
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh
    exit 0
fi

CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
cd "$REPO_ROOT"

echo "=== Building Compressor (${CROSS_PREFIX}gcc) ==="
mkdir -p build

# The plugin serves its own chain_params / ui_hierarchy, generated from
# module.json so the two cannot drift (see scripts/gen_contract.py).
if ! command -v python3 >/dev/null 2>&1; then
    echo "ERROR: python3 is required to generate the contract header" >&2
    exit 1
fi
python3 scripts/gen_contract.py src/module.json build/contract_gen.h

"${CROSS_PREFIX}gcc" -std=c11 -D_DEFAULT_SOURCE \
    -O2 -shared -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -fomit-frame-pointer -DNDEBUG \
    -Wall -Wextra \
    src/dsp/compressor.c \
    -Isrc/dsp -Ibuild \
    -o "build/${MODULE_ID}.so" \
    -lm

echo "Packaging..."
rm -rf "dist/${MODULE_ID}"
mkdir -p "dist/${MODULE_ID}"
# cat rather than cp: avoids ExtFS deallocation issues on Docker mounts
cat src/module.json    > "dist/${MODULE_ID}/module.json"
cat src/help.json      > "dist/${MODULE_ID}/help.json"
cat src/canvas.js      > "dist/${MODULE_ID}/canvas.js"
cat "build/${MODULE_ID}.so" > "dist/${MODULE_ID}/${MODULE_ID}.so"
chmod +x "dist/${MODULE_ID}/${MODULE_ID}.so"

tar -czf "dist/${MODULE_ID}-module.tar.gz" -C dist "${MODULE_ID}"

echo ""
echo "=== Build complete ==="
echo "Module:  dist/${MODULE_ID}/"
echo "Tarball: dist/${MODULE_ID}-module.tar.gz"
echo "Install: ./scripts/install.sh"
