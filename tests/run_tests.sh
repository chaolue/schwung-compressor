#!/usr/bin/env bash
# Offline tests: DSP behaviour, the served contract, help text and the canvas
# drawers. Native build -- no Move, no cross-compiler, no Docker needed.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE"
mkdir -p build

CC="${CC:-cc}"
python3 scripts/gen_contract.py src/module.json build/contract_gen.h

echo "==> DSP"
"$CC" -std=c11 -D_DEFAULT_SOURCE -O2 -g -Wall -Wextra -Werror \
    -Isrc/dsp -Ibuild tests/test_compressor.c -o build/test_compressor -lm
./build/test_compressor

echo "==> served contract == module.json"
./build/test_compressor --dump-contract > build/served_contract.txt
python3 tests/check_contract.py build/served_contract.txt

echo "==> help.json fits the screen"
python3 tests/check_help.py

echo "==> canvas.js"
./build/test_compressor --dump-curve > build/curve.json
node tests/test_canvas.mjs build/curve.json

echo "==> all tests passed"
