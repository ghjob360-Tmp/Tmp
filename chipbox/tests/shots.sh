#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Run a host script and put its screenshots side by side: tests/shots.sh SCRIPT.txt OUT.png
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host build/shots
[ -x build/host/chipbox ] || cc -O2 -w -DCHIPBOX_HOST -DAPP_VER='"dev"' -Ifirmware/src -Ifirmware/gen -o build/host/chipbox firmware/src/chipbox.c
n=$(basename "$1" .txt)
rm -f build/shots/$n-*.ppm
HOST_FLASH= build/host/chipbox build/shots/$n "$1" > /dev/null
python3 - "$n" "$2" <<'PY'
import sys, glob
from PIL import Image
fs = sorted(glob.glob(f"build/shots/{sys.argv[1]}-*.ppm"), key=lambda f: int(f.rsplit('-', 1)[1][:-4]))
ims = [Image.open(f) for f in fs]
W = Image.new('RGB', (240 * len(ims), 240))
for i, im in enumerate(ims): W.paste(im, (240 * i, 0))
W.save(sys.argv[2]); print(sys.argv[2], len(ims), "screens")
PY
