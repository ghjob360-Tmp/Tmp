// SPDX-License-Identifier: MPL-2.0
// The install sites' web installer as tools/make_sites.py fixes it (the classic protocol, no SysEx over 1000 bytes)
// against felucca-bsp's real update loader (bsp/tests/ldr_pipe.c) on a fake WebMIDI, as fm1-base's
// tests/installer_fast_test.mjs does. Run by make_sites.py from inside its patched copy of fm1-base:
//   node tests/web_install_test.mjs
import { spawn, execFileSync } from "child_process";
import fs from "fs";
import os from "os";
import path from "path";
import { Updater, unpack7, limitSend } from "../web/fm1ota.js";

let fails = 0;
const check = (ok, msg) => { console.log(`${msg.padEnd(78)} ${ok ? "ok" : "FAIL"}`); if (!ok) fails++; };
const APP_LO = 0x4000, APP_HI = 0x93000, FL_OFF = 0x400;
const TMP = fs.mkdtempSync(path.join(os.tmpdir(), "fm1-web-"));
const exe = path.join(TMP, "ldr");
execFileSync(process.env.CC || "cc", ["-std=c11", "-D_DEFAULT_SOURCE", "-O1", "-w", "-Ibsp/chip",
  "-DLDR_FAST_CHUNK=4096u", "-DOTA_FRAME_MAX=4200u", "bsp/tests/ldr_pipe.c", "-o", exe]);

function device(name) {
  const flash = path.join(TMP, name + ".nor"), img = path.join(TMP, name + ".img");
  execFileSync(exe, ["init", flash, img]);
  return { flash, image: new Uint8Array(fs.readFileSync(img)) };
}
const done = (flash, image) => {
  const nor = fs.readFileSync(flash);
  return Buffer.compare(nor.subarray(APP_LO, APP_HI), Buffer.from(image.subarray(FL_OFF + APP_LO, FL_OFF + APP_HI))) === 0;
};

// the FM-1 in its update loader on a fake WebMIDI port; every message the page sends is measured
function loader(flash) {
  const proc = spawn(exe, ["run", flash], { stdio: ["pipe", "pipe", "pipe"], env: { ...process.env, LDR_PIPE_TRY_MS: "3000" } });
  const L = { proc, sent: [], reqs: 0, exited: null, closed: 0 };
  const input = { name: "Felucca Update", state: "connected", open: async () => {}, close: async () => { L.closed++; }, onmidimessage: null };
  const output = { name: "Felucca Update", state: "connected", open: async () => {}, close: async () => { L.closed++; },
    send: (b) => { if (output.state !== "connected") throw new Error("gone"); L.sent.push(b.length); proc.stdin.write(Buffer.from(b)); } };
  proc.on("exit", (c) => { L.exited = c; input.state = output.state = "disconnected"; });
  proc.stdin.on("error", () => {});
  let buf = [];
  proc.stdout.on("data", (d) => {
    for (const x of d) {
      if (x === 0xF0) buf = [];
      buf.push(x);
      if (x === 0xF7 && buf[0] === 0xF0) {
        const u = unpack7(buf.slice(1, -1));
        if (u.length === 15 && u[2] === 0x30) L.reqs++;
        input.onmidimessage && input.onmidimessage({ data: Uint8Array.from(buf), target: input });
        buf = [];
      }
    }
  });
  L.access = { inputs: new Map([["i0", input]]), outputs: new Map([["o0", output]]) };
  L.wait = () => new Promise((r) => { if (L.exited !== null) r(L.exited); else { const t = setTimeout(() => { proc.kill(); r(null); }, 5000); proc.on("exit", (c) => { clearTimeout(t); r(c); }); } });
  return L;
}

{                                        // the page's settings: the whole install, no message over 1000 bytes
  limitSend(1000);
  const { flash, image } = device("web");
  const L = loader(flash);
  const up = new Updater(L.access);
  up.classic = true;
  up.pause = 0;
  let ok = false, code = null;
  try { ok = await up.resume(image, () => {}); } catch (e) { code = e.code || String(e); }
  const ex = await L.wait();
  const big = Math.max(...L.sent);
  check(ok && ex === 0 && done(flash, image), `the classic install, written and checked (${L.reqs} reads; ${code || "no error"})`);
  check(big <= 1000, `no message to the device over 1000 bytes (the largest: ${big})`);
  check(L.closed > 0, "the link's ports closed when done");
}
{                                        // the guard under it: a fast install's 4 KiB answers are refused, not sent
  limitSend(1000);
  const { flash, image } = device("guard");
  const L = loader(flash);
  const up = new Updater(L.access);
  up.classic = false;
  up.pause = 0;
  let ok = false;
  try { ok = await up.resume(image, () => {}); } catch (_) { ok = false; }
  L.proc.kill();
  await L.wait();
  check(!ok && Math.max(...L.sent) <= 1000, `the fast install under the limit: stopped, nothing over 1000 sent (largest ${Math.max(...L.sent)})`);
}
fs.rmSync(TMP, { recursive: true, force: true });
if (fails) { console.log(`${fails} failed`); process.exit(1); }
console.log("web install tests passed");
