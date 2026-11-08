#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""logue SDK units on a CHAIN-1 FM-1, over USB-MIDI:

    fm1_units.py list [--port NAME]
    fm1_units.py put UNIT [--slot 1..7] [--port NAME]    UNIT: .ntkdigunit (NTS-1), .nts1mkiiunit (NTS-1 mkII), .lgu
    fm1_units.py erase SLOT [--port NAME]

The unit is packed here (logue/logue_pack.py) and sent in the update's framing (CHAIN-1's app.c, APP_USB_CMD): a PUT
command, then the FM-1 reads the image from us as an update reads a package (its read requests, our answers), writes
it into the slot and says how it went. Needs mido with python-rtmidi; fm1-base's tools (base/tools) beside it."""
import argparse
import os
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [os.path.join(HERE, "..", "base", "tools"), os.path.join(HERE, "logue")]
import fm1_install as I  # noqa: E402
import logue_pack  # noqa: E402

UC_LIST, UC_PUT, UC_ERASE, UC_LIST_R, UC_RESULT = 0x70, 0x71, 0x72, 0x78, 0x79
MODULES = {1: "MOD", 2: "DELAY", 3: "REVERB", 4: "OSC"}
WHY = {1: "?", 2: "the flash refused", 3: "the bank is full (erase a slot first)", 4: "too big for a slot (36 KB)",
       5: "this computer stopped answering", 6: "not a unit CHAIN-1 can run (not for the NTS-1, or damaged)"}


def find(port):
    up = I.Updater(I.MidoBackend(), port)
    dev = up.find(lambda ident: not ident.loader())
    if not dev:
        sys.exit("no FM-1 found (is it on, with CHAIN-1, and plugged in?)")
    if "_94" not in dev.id.text:
        print(f"warning: the FM-1 runs {dev.id.text}, not CHAIN-1 (FM-1_94...)", file=sys.stderr)
    return dev


def wait_frame(link, cmd, secs, image=None, progress=None):
    """our answers to the FM-1's read requests, until its frame cmd comes -> its body"""
    end = time.time() + secs
    while time.time() < end:
        pkt = link.read(0.5)
        if pkt is None:
            if link.lost:
                sys.exit("the FM-1 went away")
            continue
        req = I.parse_request(pkt)
        if req and image is not None:
            fl, addr, n = req
            if addr + n > len(image) or n > I.MAXDATA:
                sys.exit(f"the FM-1 asked for bytes outside the unit ({addr:x}+{n})")
            link.send(I.response(addr, image[addr:addr + n], fl))
            end = time.time() + secs
            if progress:
                progress(addr + n, len(image))
            continue
        f = I.parse_frame(pkt)
        if f and f[0] == cmd:
            return f[1]
    sys.exit("no answer from the FM-1 (is CHAIN-1 1.0 or later on it?)")


def cmd_list(link):
    link.drain()
    link.send(I.frame(UC_LIST))
    body = wait_frame(link, UC_LIST_R, 5)
    for s in range(len(body) // 23):
        e = body[23 * s:23 * s + 23]
        if e[0]:
            name = e[3:23].split(b"\0")[0].decode("ascii", "replace")
            print(f"{s + 1}  {MODULES.get(e[1], '?'):7s} {'mkII' if e[2] == 2 else 'mk1 ':4s}  {name}")
        else:
            print(f"{s + 1}  --")


def cmd_put(link, path, slot):
    image = logue_pack.pack(path)
    link.drain()
    link.send(I.frame(UC_PUT, bytes([slot - 1 if slot else 0xFF]) + len(image).to_bytes(4, "little")
                      + zlib.crc32(image).to_bytes(4, "little")))

    def progress(done, total):
        print(f"\r{done * 100 // total:3d} %", end="", flush=True)

    body = wait_frame(link, UC_RESULT, 15, image, progress)
    print()
    if body[0]:
        sys.exit(f"not installed: {WHY.get(body[0], body[0])}")
    print(f"installed in slot {body[1] + 1}")


def cmd_erase(link, slot):
    link.drain()
    link.send(I.frame(UC_ERASE, bytes([slot - 1])))
    body = wait_frame(link, UC_RESULT, 10)
    print("erased" if not body[0] else f"not erased: {WHY.get(body[0], body[0])}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", choices=["list", "put", "erase"])
    ap.add_argument("arg", nargs="?")
    ap.add_argument("--slot", type=int, choices=range(1, 8))
    ap.add_argument("--port")
    a = ap.parse_args()
    dev = find(a.port)
    try:
        if a.what == "list":
            cmd_list(dev.link)
        elif a.what == "put":
            if not a.arg:
                ap.error("put UNIT")
            cmd_put(dev.link, a.arg, a.slot)
            cmd_list(dev.link)
        else:
            if not a.arg or not a.arg.isdigit() or not 1 <= int(a.arg) <= 7:
                ap.error("erase SLOT (1..7)")
            cmd_erase(dev.link, int(a.arg))
    finally:
        dev.link.close()


if __name__ == "__main__":
    main()
