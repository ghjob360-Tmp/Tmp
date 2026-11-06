#!/bin/sh
# Build HELLO with fm1-base (base/, a submodule; felucca-bsp in base/bsp/).
#   ./build.sh                     app/build/hello.fwsc            (FM-1; JIELI_TOOLCHAIN, AC79_SDK)
#   ./build.sh --release 0.1       app/build/hello-0.1.fwsc
#   ./build.sh --rfm1 [--release]  app/build/rfm1/hello-dev+COMMIT.rfm1 / app/build/hello-X.Y.rfm1 (emcc)
#   ./build.sh --host              app/build/host/hello_host       (cc only)
#   ./build.sh --web               app/build/emu/                  (emcc)
cd "$(dirname "$0")" || exit 1
exec python3 base/tools/build.py app "$@"
