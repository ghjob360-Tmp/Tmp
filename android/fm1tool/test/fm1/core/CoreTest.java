// SPDX-License-Identifier: MPL-2.0
// The app's core on a desktop JVM:
//  A. the wire format, the package and the rescue's commands, byte for byte against fm1-base's / felucca-bsp's Python
//     (expected.txt, made from tools/fm1_install.py and tools/fm1_rescue.py);
//  B. the install's write against felucca-bsp's real update loader (bsp/tests/ldr_pipe.c) on a pipe, every message
//     through the USB-MIDI codec both ways;
//  C. the whole install: a modelled running firmware (step 1), the real loader (step 2), a modelled restart (step 3);
//  D. the UBOOT rescue against a modelled UBOOT device: the bulk-only transport parsed, a 1 MiB flash.
//   java -cp out fm1.core.CoreTest EXPECTED STOCK.fwsc PRISM.fwsc LOADER_EXE WL82LOADER
package fm1.core;

import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;

public class CoreTest {
    static int fails;

    static void check(boolean ok, String msg) {
        System.out.printf("%-86s %s%n", msg, ok ? "ok" : "FAIL");
        if (!ok) fails++;
    }

    static String hex(byte[] b) { return Pkg.hex(b); }

    public static void main(String[] a) throws Exception {
        Map<String, String> ex = new HashMap<>();
        for (String l : Files.readAllLines(Paths.get(a[0]))) {
            String[] p = l.split(" ", 2);
            ex.put(p[0], p[1].trim());
        }
        byte[] stock = Files.readAllBytes(Paths.get(a[1])), prism = Files.readAllBytes(Paths.get(a[2]));
        String loaderExe = a[3];
        byte[] wl82 = Files.readAllBytes(Paths.get(a[4]));

        /* ---- A ---- */
        byte[] flash = Pkg.flashImage(stock);
        check(flash.length == Integer.parseInt(ex.get("flash_len")) && hex(Pkg.sha256(flash)).equals(ex.get("flash_sha")),
              "A: the stock package's flash entry = fm1_rescue.py's (" + flash.length + " B)");
        check("V16".equals(Rescue.stockName(stock)), "A: the stock package recognised as V16 by its sha256");
        check(Pkg.productOf(stock).equals(ex.get("product_v16")), "A: product of the stock package: " + Pkg.productOf(stock));
        Pkg pp = new Pkg(prism);
        check(pp.product.equals(ex.get("product_prism")) && pp.hasLoader && hex(Pkg.sha256(pp.image)).equals(ex.get("logical_sha")),
              "A: PRISM-1's package: " + pp.product + ", its logical image = fm1_install.py's, the Felucca loader in it");
        check(hex(Rescue.cdbFor(Rescue.CMD_READ_FLASH, Rescue.cat(Rescue.be32(0x12345), new byte[] {2, 0}))).equals(ex.get("cdb_read")),
              "A: a READ_FLASH command block = fm1_rescue.py's");
        byte[] d = new byte[512];
        for (int i = 0; i < 512; i++) d[i] = (byte) i;
        int c = Wire.crc16(d, 0, 512, 0);
        byte[] args = Rescue.cat(Rescue.be32(Rescue.LOADER_ADDR + 512), new byte[] {2, 0, 0, (byte) c, (byte) (c >> 8)});
        check(hex(Rescue.cdbFor(Rescue.CMD_WRITE_MEMORY, args)).equals(ex.get("cdb_wmem")), "A: a WRITE_MEMORY command block (with its CRC) = fm1_rescue.py's");
        byte[] k = {0x12, 0x34};
        Rescue.crcCipher(k);
        check(hex(k).equals(ex.get("cipher_1234")), "A: JieLi's CrcDecode cipher = fm1_rescue.py's");
        byte[] r10 = new byte[10];
        for (int i = 0; i < 10; i++) r10[i] = (byte) i;
        check(hex(Wire.response(0x1000, r10, 0, 10, 3)).equals(ex.get("resp")), "A: a read's answer = fm1_install.py's");
        check(hex(Wire.frame(49, new byte[0])).equals(ex.get("frame49")), "A: an M-UPGRADE frame = fm1_install.py's");
        check(Arrays.equals(Rescue.checkLoader(wl82), wl82), "A: wl82loader.bin's hash");
        Random rnd = new Random(1);
        boolean round = true;
        for (int t = 0; t < 300 && round; t++) {
            byte[] m = new byte[2 + rnd.nextInt(700)];
            m[0] = (byte) 0xF0;
            m[m.length - 1] = (byte) 0xF7;
            for (int i = 1; i < m.length - 1; i++) m[i] = (byte) rnd.nextInt(128);
            byte[] pk = UsbMidiCodec.encodeSysex(m);
            UsbMidiCodec.Decoder dec = new UsbMidiCodec.Decoder();
            List<byte[]> got = new ArrayList<>();
            for (int o = 0; o < pk.length; o += 64) got.addAll(dec.feed(Arrays.copyOfRange(pk, o, Math.min(pk.length, o + 64)), Math.min(64, pk.length - o)));
            round = got.size() == 1 && Arrays.equals(got.get(0), m) && pk.length % 4 == 0;
        }
        check(round, "A: USB-MIDI packets: 300 SysEx messages (2..701 B) round trip in 64-byte transfers");

        /* ---- B ---- */
        Path tmp = Files.createTempDirectory("fm1app");
        Dev b = device(loaderExe, tmp, "b");
        LoaderMidi lm = new LoaderMidi(loaderExe, b.flash);
        Installer up = new Installer(lm);
        up.scale(0.02);
        up.dReply = 0;
        Installer.Device ota = up.find(Wire.Identity::loader);
        check(ota != null && ota.id.loader(), "B: the loader found by its handshake: " + (ota == null ? "-" : ota.id.text));
        List<String> steps = new ArrayList<>();
        String err = null;
        try {
            up.write(ota, b.image, (kk, v) -> steps.add(kk + " " + v));
        } catch (Fm1Exception e) {
            err = e.code + " " + e.getMessage();
        }
        int ex1 = lm.waitExit();
        check(err == null && ex1 == 0 && done(b), "B: the classic write by the real loader: flash written and checked (" + (err == null ? "no error" : err) + ")");
        check(lm.largest <= 1000, "B: no message to the device over 1000 bytes (largest " + lm.largest + ")");

        /* ---- C ---- */
        Dev cdev = device(loaderExe, tmp, "c");
        Story st = new Story(loaderExe, cdev);
        Installer u2 = new Installer(st);
        u2.scale(0.02);
        u2.dReply = 0;
        List<String> s2 = new ArrayList<>();
        String err2 = null, got = null;
        try {
            Installer.Device dv = u2.find(id -> !id.loader());
            Installer.Device o2 = u2.check(dv, cdev.image, (kk, v) -> s2.add(kk));
            u2.write(o2, cdev.image, (kk, v) -> s2.add(kk));
            got = u2.verify("FM-1_95010300", (kk, v) -> s2.add(kk));
        } catch (Fm1Exception e) {
            err2 = e.code + " " + e.getMessage();
        }
        check(err2 == null && "FM-1_95010300".equals(got) && done(cdev) && st.checkReads > 0,
              "C: the whole install: step 1 (" + st.checkReads + " reads), the loader, the write, the restart (" + (err2 == null ? got : err2) + ")");

        /* ---- D ---- */
        FakeUboot fu = new FakeUboot(flash);
        Rescue.Uboot ub = new Rescue.Uboot(new Rescue.Bot(fu));
        List<Object[]> pk = new ArrayList<>();
        pk.add(new Object[] {"V16", flash});
        final byte[][] saved = {null};
        boolean wrote = Rescue.rescue(ub, pk, wl82, false, bk -> { saved[0] = bk.clone(); return "mem"; }, s -> { }, () -> true, null);
        byte[] expect = Arrays.copyOf(flash, Rescue.FLASH_SIZE);
        System.arraycopy(fu.orig, flash.length, expect, flash.length, Rescue.FLASH_SIZE - flash.length);
        check(wrote && Arrays.equals(fu.flash, expect), "D: the rescue (confirmed): the app area is stock V16, the rest kept");
        check(saved[0] != null && Arrays.equals(saved[0], fu.orig), "D: the backup is the flash as it was, before any write");
        check(fu.headWrites == 0 && fu.outsideWrites == 0, "D: nothing written below 0x4000 or past the app area");
        check(fu.loaderBytes == wl82.length && fu.jumped, "D: the flash loader sent whole and started");
        FakeUboot f2 = new FakeUboot(flash);
        boolean w2 = Rescue.rescue(new Rescue.Uboot(new Rescue.Bot(f2)), pk, wl82, false, bk -> "mem", s -> { }, () -> false, null);
        check(!w2 && Arrays.equals(f2.flash, f2.orig), "D: declined at the question: nothing written");
        FakeUboot f3 = new FakeUboot(flash);
        f3.key = 0x1234;
        String e3 = null;
        try { Rescue.rescue(new Rescue.Uboot(new Rescue.Bot(f3)), pk, wl82, true, bk -> "mem", s -> { }, null, null); }
        catch (Fm1Exception e) { e3 = e.getMessage(); }
        check(e3 != null && Arrays.equals(f3.flash, f3.orig), "D: another chip key: refused, nothing written");
        FakeUboot f4 = new FakeUboot(flash);
        f4.flash[0x100] ^= 1;
        f4.orig[0x100] ^= 1;
        String e4 = null;
        try { Rescue.rescue(new Rescue.Uboot(new Rescue.Bot(f4)), pk, wl82, true, bk -> "mem", s -> { }, null, null); }
        catch (Fm1Exception e) { e4 = e.getMessage(); }
        check(e4 != null && Arrays.equals(f4.flash, f4.orig), "D: a head that is not the stock version's: refused, nothing written");

        /* ---- E: logue units packed as chain1/tools/logue/logue_pack.py packs them ---- */
        if (a.length > 5) {
            String dir = a[5];
            for (String n : new String[] {"waves.ntkdigunit", "pluck.nts1mkiiunit", "dmod.nts1mkiiunit"}) {
                LogueUnit u = LogueUnit.pack(Files.readAllBytes(Paths.get(dir, n)));
                byte[] want = Files.readAllBytes(Paths.get(dir, n.replaceAll("\\.[a-z0-9]+$", ".lgu")));
                check(Arrays.equals(u.image, want), "E: " + n + " packed byte for byte as logue_pack.py (" + u.describe() + ")");
            }
            boolean refused = false;
            try { LogueUnit.pack(stock); } catch (Fm1Exception e) { refused = true; }
            check(refused, "E: a firmware package is not taken for a unit");
            /* ---- F: the unit bank's protocol against a CHAIN-1 stand-in ---- */
            LogueUnit u = LogueUnit.pack(Files.readAllBytes(Paths.get(dir, "pluck.nts1mkiiunit")));
            FakeChain fc = new FakeChain();
            int[] pct = {0};
            int slot = Units.put(fc, u.image, -1, (d2, t) -> pct[0] = d2 * 100 / t);
            check(slot == 0 && Arrays.equals(fc.slots[0], u.image) && pct[0] == 100, "F: PUT: the FM-1 read the image whole into slot 1");
            List<Units.Entry> l = Units.list(fc);
            check(l.size() == 7 && l.get(0).used && l.get(0).module == 4 && l.get(0).name.equals("PLUCK") && !l.get(1).used,
                  "F: LIST: slot 1 PLUCK (osc), the rest free");
            Units.erase(fc, 0);
            check(fc.slots[0] == null, "F: ERASE");
            fc.full = true;
            String msg = null;
            try { Units.put(fc, u.image, -1, null); } catch (Fm1Exception e) { msg = e.getMessage(); }
            check(msg != null && msg.contains("滿"), "F: a full bank: refused with why (" + msg + ")");
        }
        System.out.println(fails == 0 ? "core tests passed" : fails + " failed");
        System.exit(fails == 0 ? 0 : 1);
    }

    /* ------------------------------------------------------------- the real loader on a pipe --- */
    static final class Dev {
        Path flash;
        byte[] image;
    }

    static Dev device(String exe, Path tmp, String name) throws Exception {
        Dev d = new Dev();
        d.flash = tmp.resolve(name + ".nor");
        Path img = tmp.resolve(name + ".img");
        new ProcessBuilder(exe, "init", d.flash.toString(), img.toString()).inheritIO().start().waitFor();
        d.image = Files.readAllBytes(img);
        return d;
    }

    static boolean done(Dev d) throws IOException {
        byte[] nor = Files.readAllBytes(d.flash);
        return Arrays.equals(Arrays.copyOfRange(nor, 0x4000, 0x93000), Arrays.copyOfRange(d.image, 0x400 + 0x4000, 0x400 + 0x93000));
    }

    /** one port: the loader process; every message through the USB-MIDI codec, as the app's USB link does */
    static class LoaderMidi implements Midi {
        final Process p;
        final BlockingQueue<byte[]> q = new LinkedBlockingQueue<>();
        volatile boolean exited;
        int largest;

        LoaderMidi(String exe, Path flash) throws IOException {
            ProcessBuilder pb = new ProcessBuilder(exe, "run", flash.toString());
            pb.environment().put("LDR_PIPE_TRY_MS", "3000");
            pb.redirectError(ProcessBuilder.Redirect.DISCARD);
            p = pb.start();
            Thread t = new Thread(() -> {
                try (InputStream in = p.getInputStream()) {
                    ByteArrayOutputStream cur = new ByteArrayOutputStream();
                    UsbMidiCodec.Decoder dec = new UsbMidiCodec.Decoder();
                    for (int x; (x = in.read()) >= 0; ) {
                        if (x == 0xF0) cur.reset();
                        cur.write(x);
                        if (x == 0xF7) {
                            byte[] pk = UsbMidiCodec.encodeSysex(cur.toByteArray());
                            q.addAll(dec.feed(pk, pk.length));
                            cur.reset();
                        }
                    }
                } catch (IOException ignored) {
                }
                exited = true;
            });
            t.setDaemon(true);
            t.start();
        }

        int waitExit() throws InterruptedException {
            if (!p.waitFor(10, TimeUnit.SECONDS)) {
                p.destroy();
                return -1;
            }
            return p.exitValue();
        }

        public List<String> ports() { return exited ? Collections.emptyList() : Collections.singletonList("Felucca Update"); }

        public Link open(String port) {
            return new Link() {
                final UsbMidiCodec.Decoder dec = new UsbMidiCodec.Decoder();

                public boolean send(byte[] m) {
                    if (exited) return false;
                    largest = Math.max(largest, m.length);
                    byte[] pk = UsbMidiCodec.encodeSysex(m);
                    try {
                        for (byte[] x : dec.feed(pk, pk.length)) p.getOutputStream().write(x);
                        p.getOutputStream().flush();
                        return true;
                    } catch (IOException e) {
                        return false;
                    }
                }

                public byte[] read(long ms) {
                    try { return q.poll(Math.max(ms, 1), TimeUnit.MILLISECONDS); } catch (InterruptedException e) { return null; }
                }

                public void drain() { q.clear(); }

                public boolean lost() { return exited; }

                public void close() { }
            };
        }
    }

    /* ---------------------------------- a running firmware, then the real loader, then the restart --- */
    static byte[] identity(String text) {
        byte[] d = new byte[34];
        d[0] = 0;
        d[1] = 0x59;
        d[2] = 0x11;
        byte[] t = text.getBytes();
        System.arraycopy(t, 0, d, 6, t.length);
        return Wire.wrap(d);
    }

    static byte[] request(long addr, int len) {
        byte[] u = new byte[15];
        u[1] = 0x59;
        u[2] = 0x30;
        u[3] = 8;
        for (int i = 0; i < 4; i++) u[7 + i] = (byte) (addr >>> (8 * i));
        u[11] = (byte) len;
        u[12] = (byte) (len >> 8);
        u[14] = (byte) (~Wire.sum(u, 6, 14) & 0xFF);
        return Wire.wrap(u);
    }

    static class Story implements Midi {
        final String exe;
        final Dev dev;
        int phase;                                     // 0 running, 1 the loader, 2 restarted
        LoaderMidi loader;
        int checkReads;

        Story(String exe, Dev dev) { this.exe = exe; this.dev = dev; }

        public List<String> ports() {
            if (phase == 1 && loader.exited) phase = 2;
            return Collections.singletonList(phase == 1 ? "Felucca Update" : "FM-1");
        }

        public Link open(String port) {
            if (phase == 1) return loader.open(port);
            final String name = phase == 0 ? "FM-1_015" : "FM-1_95010300";
            final BlockingQueue<byte[]> q = new LinkedBlockingQueue<>();
            return new Link() {
                long next = 0;
                boolean gone;

                public boolean send(byte[] m) {
                    if (gone) return false;
                    if (Arrays.equals(m, Wire.HS_QUERY)) q.add(identity(name));
                    else if (phase == 0 && Arrays.equals(m, Wire.UPGRADE)) q.add(request(next, 512));
                    else if (phase == 0) {
                        long[] r = parseAnswer(m);
                        if (r != null && r[0] == Wire.FINISH_CHECK) {   // the check done: off to the loader
                            gone = true;
                            try {
                                loader = new LoaderMidi(exe, dev.flash);
                            } catch (IOException e) {
                                throw new UncheckedIOException(e);
                            }
                            phase = 1;
                        } else if (r != null) {
                            checkReads++;
                            next += 512;
                            q.add(next >= 4096 ? request(Wire.FINISH_CHECK, 8) : request(next, 512));
                        }
                    }
                    return true;
                }

                public byte[] read(long ms) {
                    try { return q.poll(Math.max(ms, 1), TimeUnit.MILLISECONDS); } catch (InterruptedException e) { return null; }
                }

                public void drain() { q.clear(); }

                public boolean lost() { return gone; }

                public void close() { }
            };
        }

        long[] parseAnswer(byte[] m) {                 // -> {addr} of an answer, data checked against the image
            byte[] u = Wire.unpack7(m, 1, m.length - 1);
            if (u.length < 15 || u[2] != 0x30) return null;
            long addr = Pkg.le32(u, 7);
            int n = (u[11] & 0xFF) | (u[12] & 0xFF) << 8;
            if (addr == Wire.FINISH_CHECK) return new long[] {addr};
            for (int i = 0; i < n; i++) if (u[14 + i] != dev.image[(int) addr + i]) return null;
            return new long[] {addr};
        }
    }

    /* ------------------------------------------------------------------- a modelled UBOOT --- */
    static class FakeUboot implements Rescue.Bulk {
        final byte[] flash = new byte[Rescue.FLASH_SIZE], orig;
        int key = 0x980F, loaderBytes, headWrites, outsideWrites;
        boolean jumped;
        final int appEnd;
        byte[] pendingCbw;
        byte[] pendingIn;
        int csTag;
        final ArrayDeque<byte[]> outQ = new ArrayDeque<>();

        FakeUboot(byte[] stockFlash) {
            appEnd = stockFlash.length;
            Random r = new Random(7);
            System.arraycopy(stockFlash, 0, flash, 0, 0x4000);              // the head: stock's
            for (int i = 0x4000; i < flash.length; i++) flash[i] = (byte) r.nextInt(256);   // a firmware, then data
            System.arraycopy(stockFlash, 0x20000, flash, 0x20000, 0x10000); // some sectors already equal
            orig = flash.clone();
        }

        byte[] status(int tag, int st) {
            byte[] c = new byte[13];
            c[0] = 'U'; c[1] = 'S'; c[2] = 'B'; c[3] = 'S';
            Rescue.le(c, 4, tag);
            c[12] = (byte) st;
            return c;
        }

        public void out(byte[] d, int timeout) throws Fm1Exception {
            if (pendingCbw == null) {
                if (d.length != 31 || d[0] != 'U' || d[3] != 'C') throw new Fm1Exception("t", "not a CBW");
                pendingCbw = d;
                csTag = (int) Pkg.le32(d, 4);
                int len = (int) Pkg.le32(d, 8);
                boolean in = (d[12] & 0x80) != 0;
                if (in || len == 0) run(null);
                return;
            }
            run(d);
        }

        void run(byte[] data) {
            byte[] cdb = Arrays.copyOfRange(pendingCbw, 15, 31);
            pendingCbw = null;
            int cmd = (cdb[0] & 0xFF) << 8 | (cdb[1] & 0xFF);
            int addr = (cdb[2] & 0xFF) << 24 | (cdb[3] & 0xFF) << 16 | (cdb[4] & 0xFF) << 8 | (cdb[5] & 0xFF);
            int n = (cdb[6] & 0xFF) << 8 | (cdb[7] & 0xFF);
            byte[] reply = null;
            int st = 0;
            if ((cdb[0] & 0xFF) == 0x12) {
                reply = new byte[36];
                System.arraycopy("WL82    ".getBytes(), 0, reply, 8, 8);
                System.arraycopy("UBOOT1.00       ".getBytes(), 0, reply, 16, 16);
            } else if (cmd == Rescue.CMD_WRITE_MEMORY) {
                loaderBytes += data.length;
                if (Wire.crc16(data, 0, data.length, 0) != ((cdb[9] & 0xFF) | (cdb[10] & 0xFF) << 8)) st = 1;
            } else if (cmd == Rescue.CMD_JUMP) {
                jumped = true;
                reply = ans(cmd, new byte[0]);
            } else if (cmd == Rescue.CMD_READ_KEY) {
                byte[] kk = {(byte) key, (byte) (key >> 8)};
                Rescue.crcCipher(kk);                 // (the same XOR stream both ways)
                byte[] p = new byte[14];
                p[5] = kk[0];
                p[4] = kk[1];
                reply = ans(cmd, p);
            } else if (cmd == Rescue.CMD_GET_ONLINE_DEVICE) {
                byte[] p = new byte[14];
                p[0] = 3;
                Rescue.le(p, 2, 0x856014);
                reply = ans(cmd, p);
            } else if (cmd == Rescue.CMD_READ_FLASH) {
                reply = Arrays.copyOfRange(flash, addr, addr + n);
            } else if (cmd == Rescue.CMD_ERASE_SECTOR) {
                if (addr < 0x4000) headWrites++;
                if (addr >= appEnd) outsideWrites++;
                Arrays.fill(flash, addr, addr + 0x1000, (byte) 0xFF);
                reply = ans(cmd, new byte[0]);
            } else if (cmd == Rescue.CMD_WRITE_FLASH) {
                if (addr < 0x4000) headWrites++;
                if (addr >= appEnd) outsideWrites++;
                if (Wire.crc16(data, 0, data.length, 0) != ((cdb[9] & 0xFF) | (cdb[10] & 0xFF) << 8)) st = 1;
                else for (int i = 0; i < data.length; i++) flash[addr + i] &= data[i];   // (NOR: 1 -> 0 only)
            } else if (cmd == Rescue.CMD_RUN_APP) {
                reply = ans(cmd, new byte[0]);
            }
            if (reply != null) outQ.add(reply);
            outQ.add(status(csTag, st));
        }

        byte[] ans(int cmd, byte[] p) {
            byte[] r = new byte[16];
            r[0] = (byte) (cmd >> 8);
            r[1] = (byte) cmd;
            System.arraycopy(p, 0, r, 2, Math.min(14, p.length));
            return r;
        }

        public byte[] in(int n, int timeout) throws Fm1Exception {
            byte[] r = outQ.poll();
            if (r == null) throw new Fm1Exception("t", "nothing to read");
            return r;
        }

        public void resetRecovery() { }
    }

    /** CHAIN-1's app_usb_cmd as the FM-1 runs it: PUT reads the image 512 bytes at a time, then answers */
    static final class FakeChain implements Midi.Link {
        final java.util.ArrayDeque<byte[]> outQ = new java.util.ArrayDeque<>();
        final byte[][] slots = new byte[7][];
        byte[][] names = new byte[7][];
        boolean full;
        byte[] rx;
        int rxSlot, rxLen, rxOff;

        static byte[] request(long addr, int n) {
            byte[] q = new byte[8];
            for (int i = 0; i < 4; i++) q[1 + i] = (byte) (addr >>> (8 * i));
            q[5] = (byte) n;
            q[6] = (byte) (n >> 8);
            return Wire.frame(0x30, q);
        }

        void next() {
            if (rxOff < rxLen) {
                outQ.add(request(rxOff, Math.min(512, rxLen - rxOff)));
                return;
            }
            slots[rxSlot] = rx;
            rx = null;
            outQ.add(Wire.frame(Units.RESULT, new byte[] {0, (byte) rxSlot}));
        }

        public boolean send(byte[] m) {
            long[] r = Wire.parseRequest(m);
            byte[][] f = Wire.parseFrame(m);
            if (f != null && (f[0][0] & 0xFF) == 0x30 && rx != null) {   /* a read's answer: fl addr len data */
                byte[] b = f[1];
                long addr = (b[1] & 0xFF) | (b[2] & 0xFF) << 8 | (b[3] & 0xFF) << 16 | (long) (b[4] & 0xFF) << 24;
                int n = (b[5] & 0xFF) | (b[6] & 0xFF) << 8;
                if (addr == rxOff) {
                    System.arraycopy(b, 8, rx, rxOff, n);
                    rxOff += n;
                }
                next();
                return true;
            }
            if (f == null) return true;
            int cmd = f[0][0] & 0xFF;
            byte[] b = f[1];
            if (cmd == Units.LIST) {
                byte[] o = new byte[7 * 23];
                for (int s = 0; s < 7; s++)
                    if (slots[s] != null) {
                        o[23 * s] = 1;
                        o[23 * s + 1] = slots[s][4] == 2 ? (byte) 4 : slots[s][5];
                        o[23 * s + 2] = slots[s][4];
                        byte[] nm = "PLUCK".getBytes();
                        System.arraycopy(nm, 0, o, 23 * s + 3, nm.length);
                    }
                outQ.add(Wire.frame(Units.LIST_R, o));
            } else if (cmd == Units.ERASE) {
                slots[b[0]] = null;
                outQ.add(Wire.frame(Units.RESULT, new byte[] {0, b[0]}));
            } else if (cmd == Units.PUT) {
                int s = b[0] & 0xFF;
                if (s == 0xFF) for (s = 0; s < 7 && slots[s] != null; s++) { }
                if (full || s >= 7) {
                    outQ.add(Wire.frame(Units.RESULT, new byte[] {3, 0}));
                    return true;
                }
                rxSlot = s;
                rxLen = (b[1] & 0xFF) | (b[2] & 0xFF) << 8 | (b[3] & 0xFF) << 16;
                rx = new byte[rxLen];
                rxOff = 0;
                next();
            }
            return true;
        }

        public byte[] read(long ms) { return outQ.poll(); }
        public void drain() { }
        public boolean lost() { return false; }
        public void close() { }
    }
}
