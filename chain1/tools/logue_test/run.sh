#!/bin/sh
# the interpreter against qemu-arm (-cpu cortex-m4) on t_ops.c at several optimisation levels: run.sh [levels]
set -e
cd "$(dirname "$0")"
gcc -O2 -DPLAT_HOST -I../../app/logue -I../../base/include -o emu emu.c ../../app/logue/arm.c -lm
W=${TMPDIR:-/tmp}/armtest; mkdir -p $W
for O in ${*:--O0 -O1 -O2 -Os -O3}; do
  F="-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -mfp16-format=ieee"
  arm-none-eabi-gcc $F -O2 -fomit-frame-pointer -c start.c -o $W/start.o
  arm-none-eabi-gcc $F $O -g0 --specs=nano.specs -nostartfiles -T link.ld $W/start.o t_ops.c -lm -lc -lgcc -lnosys \
    -o $W/t$O.elf
  qemu-arm -cpu cortex-m4 $W/t$O.elf > $W/ref$O.bin
  ./emu $W/t$O.elf > $W/emu$O.bin
  if cmp -s $W/ref$O.bin $W/emu$O.bin; then echo "$O: same ($(stat -c%s $W/ref$O.bin) bytes)"; else
    echo "$O: DIFFERENT"; cmp $W/ref$O.bin $W/emu$O.bin | head -1; exit 1; fi
done
