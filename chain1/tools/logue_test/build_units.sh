#!/bin/sh
# Builds logue SDK units from source, with the SDK's compiler flags, for the tests: mk1 units as payload.bin (what a
# .ntkdigunit carries), mkII units as their ELF. The results only ever run inside the interpreter.
#   build_units.sh LOGUE_SDK CMSIS_INCLUDE OUT_DIR
set -e
SDK=$1; CMSIS=$2; OUT=$3; mkdir -p $OUT
CC=arm-none-eabi-gcc; CXX=arm-none-eabi-g++
# --- mk1 (nutekt-digital) ---
P=$SDK/platform/nutekt-digital
F1="-mcpu=cortex-m4 -mthumb -mno-thumb-interwork -DTHUMB_NO_INTERWORKING -DTHUMB_PRESENT -g -Os -mlittle-endian
    -mfloat-abi=hard -mfpu=fpv4-sp-d16 -fsingle-precision-constant -fcheck-new -DSTM32F446xE -DCORTEX_USE_FPU=TRUE
    -DARM_MATH_CM4 -D__FPU_PRESENT -I$P/inc -I$P/inc/dsp -I$P/inc/utils -I$CMSIS"
mk1() {   # name unitdir ldscript syms sources...
  n=$1; d=$2; ld=$3; sy=$4; shift 4; objs=""
  for s in $d/tpl/_unit.c "$@"; do
    o=$OUT/mk1_${n}_$(basename $s).o
    case $s in *.c) $CC $F1 -std=c11 -mstructure-size-boundary=8 -I$d -I$d/inc -c $s -o $o ;;
               *) $CXX $F1 -std=c++11 -fno-rtti -fno-exceptions -fno-non-call-exceptions -I$d -I$d/inc -c $s -o $o ;; esac
    objs="$objs $o"
  done
  $CC $F1 -nostartfiles -Wl,--no-warn-mismatch,--library-path=$d/ld,--script=$d/ld/$ld -Xlinker --just-symbols=$d/ld/$sy \
      $objs -lm -o $OUT/mk1_$n.elf
  arm-none-eabi-objcopy -O binary $OUT/mk1_$n.elf $OUT/mk1_$n.bin
  cp $d/manifest.json $OUT/mk1_$n.json
  echo "mk1 $n: $(stat -c%s $OUT/mk1_$n.bin) bytes"
}
mk1 waves $P/waves userosc.ld osc_api.syms $P/waves/waves.cpp
mk1 dosc $P/dummy-osc userosc.ld osc_api.syms
mk1 dmod $P/dummy-modfx usermodfx.ld main_api.syms
mk1 ddel $P/dummy-delfx userdelfx.ld main_api.syms
mk1 drev $P/dummy-revfx userrevfx.ld main_api.syms
# --- mkII ---
Q=$SDK/platform/nts-1_mkii
F2="-mcpu=cortex-m7 -mthumb -mno-thumb-interwork -DTHUMB_NO_INTERWORKING -DTHUMB_PRESENT -g -Os -mlittle-endian
    -mfloat-abi=hard -mfpu=fpv4-sp-d16 -fsingle-precision-constant -fcheck-new -DSTM32H725xE -DCORTEX_USE_FPU=TRUE
    -DARM_MATH_CM7 -D__FPU_PRESENT -fPIC -I$Q/common -I$Q/common/utils -I$Q/common/dsp -I$Q/../common -I$CMSIS"
mk2() {   # name unitdir sources...
  n=$1; d=$2; shift 2; objs=""
  for s in $Q/common/_unit_base.c "$@"; do
    o=$OUT/mk2_${n}_$(basename $s).o
    case $s in *.c) $CC $F2 -std=c11 -fno-exceptions -I$d -c $s -o $o ;;
               *) $CXX $F2 -std=c++11 -fno-use-cxa-atexit -fno-rtti -fno-exceptions -fno-non-call-exceptions -I$d -c $s -o $o ;; esac
    objs="$objs $o"
  done
  $CC $F2 -nostartfiles -Wl,-z,max-page-size=128,--no-warn-mismatch,--library-path=$Q/ld,--script=$Q/ld/unit.ld \
      -shared --entry=0 -specs=nano.specs -specs=nosys.specs $objs -lm -o $OUT/mk2_$n.elf
  echo "mk2 $n: $(stat -c%s $OUT/mk2_$n.elf) bytes"
}
mk2 waves $Q/waves $Q/waves/header.c $Q/waves/unit.cc
mk2 pluck $Q/pluck $Q/pluck/header.c $Q/pluck/unit.cc
mk2 dosc $Q/dummy-osc $Q/dummy-osc/header.c $Q/dummy-osc/unit.cc
mk2 dmod $Q/dummy-modfx $Q/dummy-modfx/header.c $Q/dummy-modfx/unit.cc
mk2 ddel $Q/dummy-delfx $Q/dummy-delfx/header.c $Q/dummy-delfx/unit.cc
mk2 drev $Q/dummy-revfx $Q/dummy-revfx/header.c $Q/dummy-revfx/unit.cc
