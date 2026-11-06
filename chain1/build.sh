#!/bin/sh
# Build CHAIN-1 with fm1-base (base/, a submodule; felucca-bsp in base/bsp/).
#   ./build.sh [--release X.Y]         app/build/chain-1[-X.Y].fwsc        (FM-1; JIELI_TOOLCHAIN, AC79_SDK)
#   ./build.sh --rfm1 [--release X.Y]  app/build/rfm1/... / chain-1-X.Y.rfm1 (RemoteFM1; emcc)
#   ./build.sh --host | --web          the host simulator / the browser emulator
cd "$(dirname "$0")" || exit 1
exec python3 base/tools/build.py app "$@"
