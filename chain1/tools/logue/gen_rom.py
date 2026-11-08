#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Writes logue_rom.c: the firmware tables a logue SDK unit for the NTS-1 reads (lookup tables, band-limited and
user wavetables), laid out at the addresses the NTS-1 mk1's SDK links units against (dummy-*/ld/*_api.syms), and
the names an NTS-1 mkII unit asks for by symbol.

    gen_rom.py LOGUE_SDK_DIR OUT.c

The tables' values are the logue SDK's own (websim/dsp tables, BSD 3-Clause, (c) KORG): read here as data (numbers
parsed out of the C source text), never compiled or run. midi_to_hz_lut_f is scaled by 48000 / 44100: units turn a
note into a phase step with it and 1 / 48000 (the NTS-1's rate), CHAIN-1 runs them at 44.1 kHz, so the notes stay in
tune."""
import pathlib
import re
import struct
import sys

SDK = pathlib.Path(sys.argv[1])
OUT = pathlib.Path(sys.argv[2])
DSP = SDK / "websim" / "dsp"
LD = SDK / "platform" / "nutekt-digital"
RATE = 48000.0 / 44100.0


def strip_comments(t):
    t = re.sub(r"/\*.*?\*/", "", t, flags=re.S)
    return re.sub(r"//[^\n]*", "", t)


def arrays(path):
    """name -> list of values (numbers or identifiers) for every `name[...] ... = { ... };` in a C file"""
    t = strip_comments(path.read_text())
    out = {}
    for m in re.finditer(r"([A-Za-z_]\w*)\s*\[[^\]]*\][^=;{]*=\s*\{(.*?)\}\s*;", t, flags=re.S):
        vals = [v.strip() for v in m.group(2).split(",") if v.strip()]
        out[m.group(1)] = vals
    return out


def num(v):
    return float(v.rstrip("fF"))


def syms(path):
    s = {}
    for line in path.read_text().splitlines():
        m = re.match(r"\s*(\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*;", line)
        if m:
            s[m.group(1)] = int(m.group(2), 16)
    return s


tables = {}
for f in sorted(DSP.glob("*.c")):
    tables.update(arrays(f))

FLOATS = ["midi_to_hz_lut_f", "sqrtm2log_lut_f", "tanpi_lut_f", "log_lut_f", "bitres_lut_f", "wt_par_lut_f",
          "wt_sqr_lut_f", "wt_saw_lut_f", "wt_sine_lut_f", "schetzen_lut_f", "cubicsat_lut_f", "pow2_lut_f"]
BYTES = ["wt_par_notes", "wt_sqr_notes", "wt_saw_notes"]
WAVES = ["wavesA", "wavesB", "wavesC", "wavesD", "wavesE", "wavesF"]
API_VERSION = (1 << 16) | (1 << 8)          # USER_API_VERSION 1.1-0
PLATFORM = 3 << 8                           # k_user_target_nutektdigital


def build(symfile, base, funcs):
    s = syms(symfile)
    end = max(s.values()) + 0x40
    for name in WAVES:
        if name in s:
            end = max(end, s[name] + 4 * len(tables[name]) + 4 * 129 * len(tables[name]))
    img = bytearray(end - base)
    used = []

    def put(addr, data, name):
        o = addr - base
        for a, b, n in used:
            if o < b and a < o + len(data):
                raise SystemExit(f"{name} overlaps {n}")
        used.append((o, o + len(data), name))
        img[o:o + len(data)] = data

    for name, addr in s.items():
        if name in FLOATS:
            vals = [num(v) for v in tables[name]]
            if name == "midi_to_hz_lut_f":
                vals = [v * RATE for v in vals]
            put(addr, struct.pack(f"<{len(vals)}f", *vals), name)
        elif name in BYTES:
            put(addr, bytes(int(v) for v in tables[name]), name)
        elif name.endswith("_api_version"):
            put(addr, struct.pack("<I", API_VERSION), name)
        elif name.endswith("_api_platform"):
            put(addr, struct.pack("<I", PLATFORM), name)
    # the user wavetables: the pointer array at its address, its waves right after it (in the gap the SDK leaves)
    for name in WAVES:
        if name not in s:
            continue
        refs = tables[name]
        data_at = s[name] + 4 * len(refs)
        ptrs, blob = [], b""
        for i, r in enumerate(refs):
            ptrs.append(data_at + 4 * 129 * i)
            vals = [num(v) for v in tables[r]]
            if len(vals) != 129:
                raise SystemExit(f"{r}: {len(vals)} values")
            blob += struct.pack("<129f", *vals)
        put(s[name], struct.pack(f"<{len(ptrs)}I", *ptrs) + blob, name)
    trap = [s[f] for f in funcs]
    return s, img, min(trap), max(trap) + 4


osc_syms, osc_img, osc_tlo, osc_thi = build(LD / "dummy-osc" / "ld" / "osc_api.syms", 0x0800F000,
                                            ["_osc_mcu_hash", "_osc_bl_saw_idx", "_osc_bl_sqr_idx",
                                             "_osc_bl_par_idx", "_osc_rand", "_osc_white"])
fx_syms, fx_img, fx_tlo, fx_thi = build(LD / "dummy-delfx" / "ld" / "main_api.syms", 0x0807B000,
                                        ["_fx_mcu_hash", "_fx_rand", "_fx_white", "_fx_get_bpm", "_fx_get_bpmf"])
for n, a in osc_syms.items():
    if n.startswith("_osc_") and not osc_tlo <= a < osc_thi:
        raise SystemExit(n)


def words(img):
    img = img + bytes(-len(img) % 4)
    w = struct.unpack(f"<{len(img) // 4}I", img)
    lines = []
    for i in range(0, len(w), 8):
        lines.append("    " + ", ".join(f"{x}u" for x in w[i:i + 8]) + ",")   # (decimal: hex here reads as registers)
    return "\n".join(lines), len(w)


def symtab(sy, mk2names):
    rows = []
    for n, a in sorted(sy.items()):
        if n.startswith("_osc_") or n.startswith("_fx_"):
            rows.append((n[1:], a))                 # mkII's names for the functions: osc_rand, fx_get_bpm ...
        rows.append((n, a))
    return rows


osc_w, osc_n = words(osc_img)
fx_w, fx_n = words(fx_img)
src = f"""/* SPDX-License-Identifier: BSD-3-Clause AND MPL-2.0 */
/* Generated by tools/gen_rom.py from the logue SDK (its websim/dsp tables: BSD 3-Clause, Copyright (c) 2018-2023 KORG
 * INC.; its licence: chain1/LICENSES/logue-sdk.txt). Do not edit. The NTS-1's firmware tables as a unit sees them: the
 * oscillator API's at 0x0800F000, the effects API's at 0x0807B000 (mk1 units are linked to these addresses; mkII
 * units ask for the same names, logue_rom_syms). midi_to_hz_lut_f is scaled by 48000 / 44100 (CHAIN-1 runs units at
 * 44.1 kHz). */
#include <stdint.h>
#include "logue_rom.h"

const uint32_t logue_rom_osc[{osc_n}] __attribute__((aligned(4))) = {{
{osc_w}
}};
const uint32_t logue_rom_fx[{fx_n}] __attribute__((aligned(4))) = {{
{fx_w}
}};

const logue_sym_t logue_rom_osc_syms[] = {{
""" + "\n".join(f'    {{"{n}", 0x{a:08x}u}},' for n, a in symtab(osc_syms, True)) + """
    {0, 0}
};
const logue_sym_t logue_rom_fx_syms[] = {
""" + "\n".join(f'    {{"{n}", 0x{a:08x}u}},' for n, a in symtab(fx_syms, True)) + "\n    {0, 0}\n};\n"
OUT.write_text(src)

hdr = f"""/* SPDX-License-Identifier: MPL-2.0 */
/* Generated by tools/gen_rom.py: the NTS-1's firmware tables (logue_rom.c) and its API's call addresses */
#pragma once
#include <stdint.h>

typedef struct {{
    const char *name;
    uint32_t addr;
}} logue_sym_t;

#define LOGUE_ROM_OSC_BASE 0x0800F000u
#define LOGUE_ROM_OSC_WORDS {osc_n}u
#define LOGUE_ROM_FX_BASE 0x0807B000u
#define LOGUE_ROM_FX_WORDS {fx_n}u
extern const uint32_t logue_rom_osc[{osc_n}];
extern const uint32_t logue_rom_fx[{fx_n}];
extern const logue_sym_t logue_rom_osc_syms[];
extern const logue_sym_t logue_rom_fx_syms[];

/* the calls into the firmware (a unit's branch to one of these runs ours) */
""" + "\n".join(f"#define LOGUE_{n.upper().lstrip('_')} 0x{a:08x}u" for n, a in sorted({**osc_syms, **fx_syms}.items())
                if n.startswith("_")) + f"""
#define LOGUE_OSC_TRAP_LO 0x{osc_tlo:08x}u
#define LOGUE_OSC_TRAP_HI 0x{osc_thi:08x}u
#define LOGUE_FX_TRAP_LO 0x{fx_tlo:08x}u
#define LOGUE_FX_TRAP_HI 0x{fx_thi:08x}u
"""
OUT.with_suffix(".h").write_text(hdr)
print(f"{OUT}: osc {osc_n * 4} bytes, fx {fx_n * 4} bytes")
