# PRISM-1: licensing

- `app/` (PRISM-1's own code: `app.c`, `app.h`, `synth.c`, `synth.h`, `wt.c`, `wt.h`, the tests): **GPL-3.0-only** (`LICENSE`).
  The engine follows **Vital** by Matt Tytel (https://github.com/mtytel/vital, GPL-3.0): the oscillator phase
  warps and their windows (`src/synthesis/producers/synth_oscillator.cpp`), the envelope's power curve
  (`src/synthesis/framework/futils.h` powerScale), the squared ENV1 x velocity amplitude
  (`src/synthesis/modules/synth_voice_handler.cpp`), the filter blend and the effects' order. They are written again
  here for floats and one core; no file of Vital's is included. "Vital" is Vital Audio's name; PRISM-1 is not made
  or endorsed by Vital Audio.
- `base/` (git submodule): **fm1-base** by Charles Vestal, MPL-2.0, with **felucca-bsp** by Leo Kuroshita
  (Hügelton Instruments) in `base/bsp/`, MPL-2.0. MPL-2.0 files may be part of a GPL work and stay MPL-2.0 file by
  file (MPL-2.0 section 3.3).
- The firmware package (`.fwsc`) also carries three files from JieLi's AC79 SDK (Apache-2.0) that the build takes
  from `$AC79_SDK`; they are not in this tree.
- "M-VAVE" and "FM-1" belong to their owners; PRISM-1 is not made or endorsed by them.
