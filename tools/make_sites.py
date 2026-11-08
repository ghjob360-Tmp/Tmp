#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Make docs/ (GitHub Pages): an install site per firmware, with fm1-base's tools/make_site.py, from a copy of
fm1-base's web installer with these fixes applied (fm1-base, a submodule, is not changed):

 1. The web installer never uses the fast install: its 4 KiB answers are SysEx messages of ~4.7 KB, more than
    Android's MIDI framework takes in one message (MidiPortImpl: 1015 bytes) and more than some Windows MIDI drivers
    handle. Android Chrome crashed with them, taking the system's USB MIDI service (and so Android) with it, and
    Windows Chrome crashed. The classic protocol's answers are 512 bytes of data (~600 bytes of SysEx).
 2. A guard under that: no SysEx over SEND_MAX bytes ever reaches the browser's MIDI output (the send fails instead,
    and the install stops cleanly).
 3. A link's ports are closed when it is done with (the installer used to leave every port it tried open, then the
    FM-1 restarted into its update loader under them: a device going away under an open port is the case Chrome's
    MIDI back ends handle worst).
 4. The log is written in batches, once a frame, and keeps its last 400 lines (it took a line, a DOM text node and a
    forced layout per request: a few thousand of each in an install).

  python3 tools/make_sites.py            the fix tested (tools/web_install_test.mjs: node, cc), then every
                                         firmware's site, from its dist/ package, into docs/
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIRMWARES = [("chain1", "chain-1"), ("prism1", "prism-1"), ("toybox4", "toybox-4")]
SEND_MAX = 1000


def patch(text, old, new, what):
    if old not in text:
        raise SystemExit(f"make_sites: fm1-base's {what} changed: the fix no longer applies; look again")
    return text.replace(old, new, 1)


def patched_base(base, tmp):
    """a tree that make_site.py takes for fm1-base: its web/ and tools/ copied (and fixed), the rest linked"""
    t = Path(tmp) / "base"
    t.mkdir()
    for e in Path(base).resolve().iterdir():
        if e.name in ("web", "tools", "tests"):
            shutil.copytree(e, t / e.name, ignore=shutil.ignore_patterns("__pycache__", "node_modules"))
        elif e.name not in (".git",):
            os.symlink(e, t / e.name)
    ota = t / "web" / "fm1ota.js"
    s = ota.read_text(encoding="utf-8")
    s = patch(s, "// one MIDI in/out pair with a SysEx queue\nclass Link {",
              "// the largest SysEx handed to the browser's MIDI output (limitSend); Infinity: no limit (the tests)\n"
              "let SEND_MAX = Infinity;\n"
              "export function limitSend(n) { SEND_MAX = n; }\n\n"
              "// one MIDI in/out pair with a SysEx queue\nclass Link {", "fm1ota.js Link")
    s = patch(s, "  send(bytes) {\n    try { this.output.send(bytes); return true; } catch (_) { return false; }\n  }",
              "  send(bytes) {\n"
              "    if (bytes.length > SEND_MAX) return false;   // (never more than the OS's MIDI takes in one message)\n"
              "    try { this.output.send(bytes); return true; } catch (_) { return false; }\n  }", "fm1ota.js send")
    s = patch(s, "  close() { this.input.onmidimessage = null; }",
              "  close() {                                   // (and the ports: none left open under a restarting FM-1)\n"
              "    this.input.onmidimessage = null;\n"
              "    try { const p = this.input.close && this.input.close(); if (p && p.catch) p.catch(() => {}); } catch (_) {}\n"
              "    try { const p = this.output.close && this.output.close(); if (p && p.catch) p.catch(() => {}); } catch (_) {}\n"
              "  }", "fm1ota.js close")
    ota.write_text(s, encoding="utf-8")
    page = t / "web" / "installer.html"
    h = page.read_text(encoding="utf-8")
    h = patch(h, "const updater = (midi, lg) => { const up = new Updater(midi, lg);",
              "const updater = (midi, lg) => { const up = new Updater(midi, lg); up.classic = true; limitSend(%d);" % SEND_MAX,
              "installer.html updater")
    h = patch(h, """const log = (t) => {
  const el = $("log"), atEnd = el.scrollHeight - el.scrollTop - el.clientHeight < 24;
  el.append(t + "\\n");
  if (atEnd) el.scrollTop = el.scrollHeight;
};""", """const logq = [];
let logLines = 0, logFrame = 0;
const logFlush = () => {                 /* once a frame: the lines since, and only the last 400 kept */
  logFrame = 0;
  const el = $("log"), atEnd = el.scrollHeight - el.scrollTop - el.clientHeight < 24;
  el.append(logq.join("\\n") + "\\n");
  logLines += logq.length;
  logq.length = 0;
  if (logLines > 600) {
    const keep = el.textContent.split("\\n").slice(-401).join("\\n");
    el.textContent = keep;
    logLines = 400;
  }
  if (atEnd) el.scrollTop = el.scrollHeight;
};
const log = (t) => {
  logq.push(t);
  if (!logFrame) logFrame = requestAnimationFrame(logFlush);
};""", "installer.html log")
    page.write_text(h, encoding="utf-8")
    return t


def test(b):
    """the fixed installer against felucca-bsp's real update loader (tools/web_install_test.mjs): before any site"""
    shutil.copy(ROOT / "tools" / "web_install_test.mjs", b / "tests" / "web_install_test.mjs")
    subprocess.run(["node", "tests/web_install_test.mjs"], check=True, cwd=b)


def main():
    base = ROOT / "prism1" / "base"
    out = ROOT / "docs"
    with tempfile.TemporaryDirectory() as tmp:
        b = patched_base(base, tmp)
        test(b)
        for d, stem in FIRMWARES:
            pkgs = sorted((ROOT / d / "dist").glob(f"{stem}-*.fwsc"))
            if not pkgs:
                raise SystemExit(f"no package in {d}/dist")
            pkg = pkgs[-1]
            ver = re.fullmatch(rf"{re.escape(stem)}-(.+)\.fwsc", pkg.name)[1]
            if (out / d).exists():
                shutil.rmtree(out / d)
            subprocess.run([sys.executable, str(b / "tools" / "make_site.py"), str(ROOT / d / "app"), str(pkg), ver,
                            str(out / d)], check=True, cwd=ROOT / d)
            html = (out / d / "install" / "index.html").read_text(encoding="utf-8")
            for must in ("up.classic = true", "limitSend(%d)" % SEND_MAX, "SEND_MAX", "logFlush"):
                if must not in html:
                    raise SystemExit(f"{d}: the fix is not in the page ({must})")
    print("docs/: done")


if __name__ == "__main__":
    main()
