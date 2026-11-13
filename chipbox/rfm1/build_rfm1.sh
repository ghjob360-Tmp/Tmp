#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Chipbox as an .rfm1 (RemoteFM1): FM1VST's rfm1-target branch with this glue (rfm1/) and this tree as cores/Chipbox.
#   rfm1/build_rfm1.sh [FM1VST_DIR]      needs emcc (Emscripten 6.0.13), cmake, ninja, python3 with Pillow and fonttools
# Out: build/rfm1/chipbox-<version>.rfm1, checked by FM1VST's rfm1_check.py and run in wasm3 by rfm1_run.
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
V=${1:-build/FM1VST}
[ -d "$V/core-glue" ] || git clone --depth 1 -b rfm1-target https://github.com/Quixotic7/FM1VST.git "$V"
ln -sfn "$ROOT/rfm1" "$V/core-glue/chipbox"
ln -sfn "$ROOT" "$V/cores/Chipbox"
cmake -S "$V" -B "$V/build-rfm1" -G Ninja -DFM1_RFM1_ONLY=ON -DFM1_PUBLIC_BUILD=ON -DFM1_ONLY_CORES=chipbox
cmake --build "$V/build-rfm1" --target rfm1_chipbox rfm1_run
mkdir -p build/rfm1
for f in "$V"/build-rfm1/rfm1/chipbox-*.rfm1; do
    python3 "$V/tools/rfm1_check.py" "$f"
    "$V/build-rfm1/rfm1_run" "$f"
    cp "$f" build/rfm1/
done
ls -l build/rfm1
