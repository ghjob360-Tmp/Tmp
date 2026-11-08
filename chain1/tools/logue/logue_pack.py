#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""A logue SDK unit for the NTS-1 as CHAIN-1 stores it (an "LGU1" image, logue.h):

    logue_pack.py UNIT OUT.lgu      UNIT: a .ntkdigunit (mk1), a .nts1mkiiunit (mkII), or a mk1 payload.bin with its
                                    manifest.json beside it

mk1: the zip's payload.bin as it is, its manifest's name, module and parameters in the header. mkII: the ELF's
loadable segments laid out from address 0 (the dynamic section is inside them; CHAIN-1 links the unit itself).
Importable: pack(path) -> bytes."""
import io
import json
import pathlib
import struct
import sys
import zipfile
import zlib

MAGIC = b"LGU1"
MODULES = {"modfx": 1, "delfx": 2, "revfx": 3, "osc": 4}


def header(fmt, module, size, memsz, dyn, crc, name, params):
    h = MAGIC + struct.pack("<BBBBIIII", fmt, module, len(params), 0, size, memsz, dyn, crc)
    h += name.encode("ascii", "replace")[:19].ljust(20, b"\0") + bytes(20)
    for p in params[:6]:
        h += p[0].encode("ascii", "replace")[:19].ljust(20, b"\0")
        h += struct.pack("<hhB", int(p[1]), int(p[2]), 1 if p[3] == "%" else 0) + bytes(7)
    return h.ljust(256, b"\0")


def mk1(payload, manifest):
    hd = manifest["header"]
    if hd.get("platform") != "nutekt-digital":
        raise ValueError(f"for {hd.get('platform')}, not the NTS-1")
    module = MODULES.get(hd.get("module"))
    if not module:
        raise ValueError(f"module {hd.get('module')}")
    params = hd.get("params", [])[:6]
    return header(1, module, len(payload), 0, 0, zlib.crc32(payload), hd.get("name", "UNIT"), params) + payload


def mk2(elf):
    if elf[:4] != b"\x7fELF" or elf[4] != 1 or elf[18] != 40:
        raise ValueError("not an ARM ELF32")
    phoff, = struct.unpack_from("<I", elf, 28)
    phentsize, phnum = struct.unpack_from("<HH", elf, 42)
    loads, dyn = [], None
    for i in range(phnum):
        t, off, va, pa, fsz, msz, fl, al = struct.unpack_from("<8I", elf, phoff + i * phentsize)
        if t == 1:
            loads.append((off, va, fsz, msz))
        elif t == 2:
            dyn = va
    if not loads or dyn is None:
        raise ValueError("no loadable segments or no dynamic section")
    size = max(va + fsz for off, va, fsz, msz in loads)
    memsz = max(va + msz for off, va, fsz, msz in loads)
    img = bytearray(size)
    for off, va, fsz, msz in loads:
        img[va:va + fsz] = elf[off:off + fsz]
    img = bytes(img)
    return header(2, 0, len(img), memsz, dyn, zlib.crc32(img), "", []) + img


def pack(path):
    path = pathlib.Path(path)
    data = path.read_bytes()
    if data[:4] == b"\x7fELF":
        return mk2(data)
    if data[:2] == b"PK":
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            names = z.namelist()
            pay = next((n for n in names if n.endswith("payload.bin")), None)
            man = next((n for n in names if n.endswith("manifest.json")), None)
            if not pay or not man:
                raise ValueError("no payload.bin / manifest.json in the archive")
            return mk1(z.read(pay), json.loads(z.read(man)))
    man = path.with_name("manifest.json") if path.with_suffix(".json").exists() is False else path.with_suffix(".json")
    return mk1(data, json.loads(man.read_text()))


if __name__ == "__main__":
    out = pack(sys.argv[1])
    pathlib.Path(sys.argv[2]).write_bytes(out)
    print(f"{sys.argv[2]}: {len(out)} bytes")
