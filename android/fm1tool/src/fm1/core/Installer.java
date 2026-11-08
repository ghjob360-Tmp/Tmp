// SPDX-License-Identifier: MPL-2.0
// Installing a package on an FM-1: fm1-base's tools/fm1_install.py Updater, step for step (the running firmware checks
// the package and starts the update loader, the loader reads and writes it, the FM-1 restarts into it), always the
// classic protocol (answers of 512 bytes of data: ~600-byte SysEx messages).
package fm1.core;

import java.util.Arrays;

public class Installer {
    public interface Step {
        /** kind: start, check, loader, write (percent), reboot, done, info */
        void step(String kind, String arg);
    }

    public interface Want {
        boolean ok(Wire.Identity id);
    }

    public static final class Device {
        public final Midi.Link link;
        public final Wire.Identity id;
        public final String port;

        Device(Midi.Link link, Wire.Identity id, String port) {
            this.link = link;
            this.id = id;
            this.port = port;
        }
    }

    final Midi midi;
    /** ms; the tests shorten them (scale) */
    long dStart = 2000, dReply = 10, dLoader = 3000, dReboot = 3000, dRetry = 1000, dPoll = 1000, dHs = 1000,
         dHsFull = 600, idleCheck = 8000, idleWrite = 180000, waitLoader = 120000, waitReboot = 120000;
    public volatile boolean cancel;

    public Installer(Midi midi) { this.midi = midi; }

    public void scale(double f) {
        dStart *= f; dReply *= f; dLoader *= f; dReboot *= f; dRetry *= f; dPoll = Math.max(20, (long) (dPoll * f));
        dHs *= f; dHsFull *= f; idleCheck *= f; waitLoader *= f; waitReboot *= f;
    }

    static void sleep(long ms) {
        try { Thread.sleep(ms); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
    }

    /** a handshake -> the identity answered, null (no answer), or LOST (the send failed) */
    static final Wire.Identity LOST = new Wire.Identity("", 0);

    Wire.Identity ask(Midi.Link link, byte[] q, long ms) {
        if (!link.send(q)) return LOST;
        long end = System.currentTimeMillis() + ms;
        for (long left; (left = end - System.currentTimeMillis()) > 0; ) {
            byte[] p = link.read(left);
            if (p == null) break;
            Wire.Identity id = Wire.parseIdentity(p);
            if (id != null) return id;
        }
        return null;
    }

    Wire.Identity handshake(Midi.Link link, int tries) {
        link.drain();
        for (int t = 0; t < tries; t++) {
            Wire.Identity id = ask(link, Wire.HS_QUERY, dHs);
            if (id == LOST) return null;
            if (id == null) continue;
            if (id.loader() || !id.text.matches("[^_]+_\\d{3}")) return id;
            Wire.Identity full = ask(link, Wire.HS_FULL, dHsFull);
            return full != null && full != LOST && full.model.equals(id.model) && full.text.startsWith(id.text) ? full : id;
        }
        return null;
    }

    /** the first port that answers the handshake (and want) */
    public Device find(Want want) {
        for (String port : midi.ports()) {
            Midi.Link link = midi.open(port);
            if (link == null) continue;
            Wire.Identity id = handshake(link, 2);
            if (id != null && (want == null || want.ok(id))) return new Device(link, id, port);
            link.close();
        }
        return null;
    }

    Device waitFor(Want want, long ms) throws Fm1Exception {
        long end = System.currentTimeMillis() + ms;
        while (System.currentTimeMillis() < end) {
            if (cancel) throw new Fm1Exception("stopped", "已取消");
            Device d = find(want);
            if (d != null) return d;
            sleep(dRetry);
        }
        return null;
    }

    interface Progress {
        void read(int served, long addr, long n);
    }

    /** answer read requests until the device asks for finish -> {served, finished(1/0), lost(1/0)} */
    int[] serve(Midi.Link link, byte[] image, long finish, long idle, Progress progress, boolean loader) throws Fm1Exception {
        int served = 0;
        long last = System.currentTimeMillis();
        for (;;) {
            if (cancel) throw new Fm1Exception("stopped", "已取消");
            byte[] pkt = link.read(dPoll);
            if (pkt == null) {
                if (link.lost()) return new int[] {served, 0, 1};
                if (System.currentTimeMillis() - last > idle) return new int[] {served, 0, 0};
                continue;
            }
            long[] r = Wire.parseRequest(pkt);
            if (r == null) continue;
            int fl = (int) r[0];
            long addr = r[1], n = r[2];
            last = System.currentTimeMillis();
            if (addr == Wire.FINISH_CHECK || addr == Wire.FINISH_WRITE) {
                link.send(Wire.response(addr, Wire.SUCCESS, 0, Wire.SUCCESS.length, 0));
                if (addr == finish) return new int[] {served, 1, 0};
                continue;
            }
            byte[] data;
            int off;
            if (loader && addr >= Wire.TABLE && addr < Wire.TABLE_END) {     // (only a fast install asks it: not ours)
                int toff = (int) (addr - Wire.TABLE), k = (int) (n / 6);
                if (n == 0 || n % 6 != 0 || n > Wire.FAST_CHUNK || toff + (long) k * Wire.FAST_CHUNK > image.length)
                    throw new Fm1Exception("badreq", "裝置要求了韌體檔以外的表");
                data = Wire.sectorTable(image, toff, k);
                off = 0;
            } else {
                if (n > (loader ? Wire.FAST_CHUNK : Wire.MAXDATA) || addr + n > image.length)
                    throw new Fm1Exception("badreq", String.format("裝置要求了韌體檔以外的資料 (%x+%d)", addr, n));
                data = image;
                off = (int) addr;
            }
            sleep(dReply);
            if (!link.send(Wire.response(addr, data, off, (int) n, fl))) return new int[] {served, 0, 1};
            served++;
            progress.read(served, addr, n);
        }
    }

    /** step 1: the running firmware checks the package and starts the update loader */
    Device check(Device dev, byte[] image, Step step) throws Fm1Exception {
        step.step("start", dev.id.text);
        dev.link.send(Wire.UPGRADE);
        sleep(dStart);
        int[] s = serve(dev.link, image, Wire.FINISH_CHECK, idleCheck, (k, a, n) -> step.step("check", "" + k), false);
        dev.link.close();
        if (s[1] == 0)
            throw new Fm1Exception(s[2] != 0 ? "lost" : "stopped", "FM-1 在第 " + s[0] + " 次讀取後" +
                                   (s[2] != 0 ? "斷線" : "沒有回應") + "；還沒寫入任何東西");
        step.step("loader", "");
        sleep(dLoader);
        Device ota = waitFor(Wire.Identity::loader, waitLoader);
        if (ota == null)
            throw new Fm1Exception("noloader", "更新載入器沒有出現。拔插 USB 線後再按一次安裝：FM-1 會停在更新模式直到寫完");
        return ota;
    }

    /** step 2: the loader reads and writes the package (classic), then restarts */
    void write(Device ota, byte[] image, Step step) throws Fm1Exception {
        step.step("write", "0");
        final int[] pct = {0};
        ota.link.send(Wire.UPGRADE);
        sleep(dStart);
        int[] s = serve(ota.link, image, Wire.FINISH_WRITE, idleWrite, (k, addr, n) -> {
            int p = (int) Math.min(99, (addr + n) * 100 / image.length);
            if (p != pct[0]) {
                pct[0] = p;
                step.step("write", "" + p);
            }
        }, true);
        ota.link.close();
        if (s[1] == 0)
            throw new Fm1Exception(s[2] != 0 ? "lost" : "stopped", "更新載入器在第 " + s[0] + " 次讀取後" +
                                   (s[2] != 0 ? "斷線" : "沒有回應") + "。拔插 USB 線後再按一次安裝，就會接著寫完");
        step.step("write", "100");
    }

    String verify(String product, Step step) throws Fm1Exception {
        step.step("reboot", "");
        sleep(dReboot);
        Device back = waitFor(id -> !id.loader(), waitReboot);
        if (back == null) throw new Fm1Exception("noreturn", "FM-1 沒有重新出現：請重開 FM-1 看看");
        back.link.close();
        if (!back.id.text.equals(product))
            throw new Fm1Exception("mismatch", "寫完了，但 FM-1 回報的是 " + back.id.text + "（應該是 " + product + "）");
        step.step("done", back.id.text);
        return back.id.text;
    }

    static byte[] query(Midi.Link link, int cmd, byte[] payload, long hs) {
        link.drain();
        if (!link.send(Wire.frame(cmd, payload))) return null;
        long end = System.currentTimeMillis() + hs * 3 / 2;
        for (long left; (left = end - System.currentTimeMillis()) > 0; ) {
            byte[] p = link.read(left);
            if (p == null) break;
            if (!Wire.isSysex(p)) continue;
            byte[] u = Wire.unpack7(p, 1, p.length - 1);
            if (u.length >= 7 && u[0] == 0 && u[1] == 0x59 && (u[2] & 0xFF) == cmd && (Wire.sum(u, 6, u.length) & 0xFF) == 0xFF) {
                int n = (u[3] & 0xFF) | (u[4] & 0xFF) << 8;
                return Arrays.copyOfRange(u, 6, 6 + n);
            }
        }
        return null;
    }

    /** M-VAVE's Open firmware: allow the update of M-VAVE's own firmware (only that) */
    void arm(Device dev, Pkg pkg) throws Fm1Exception {
        if (!Pkg.isStock(pkg.product)) {
            dev.link.close();
            throw new Fm1Exception("open", "FM-1 上是 M-VAVE 的 " + dev.id.text + " 韌體：請先裝回原廠韌體（M-VAVE 的 FM-1.fwsc），再裝這個");
        }
        byte[] cap = query(dev.link, 49, new byte[0], dHs);
        if (cap == null || cap.length != 47 || cap[0] != 'F' || cap[1] != 'M' || cap[2] != 'C' || cap[3] != '1' || cap[6] != 0) {
            dev.link.close();
            throw new Fm1Exception("stopped", dev.id.text + " 沒有接受更新；還沒寫入任何東西");
        }
        int len = pkg.image.length;
        byte[] a = new byte[2 + 4 + 32];
        a[0] = 1;
        a[1] = cap[5];
        for (int i = 0; i < 4; i++) a[2 + i] = (byte) (len >>> (8 * i));
        System.arraycopy(pkg.sha256, 0, a, 6, 32);
        byte[] ack = query(dev.link, 50, a, dHs);
        if (ack == null || ack.length != 1 || ack[0] != 0) {
            dev.link.close();
            throw new Fm1Exception("stopped", dev.id.text + " 拒絕了這個韌體檔；還沒寫入任何東西");
        }
    }

    /** the whole install on dev (from find): a device already in update mode only needs the write */
    public String install(Device dev, Pkg pkg, Step step) throws Fm1Exception {
        if (!Pkg.modelOf(dev.id.text).equals(Pkg.modelOf(pkg.product)) && !dev.id.open()) {
            dev.link.close();
            throw new Fm1Exception("model", "裝置是 " + dev.id.text + "，這個韌體檔是給 " + pkg.product + " 的");
        }
        if (dev.id.open()) arm(dev, pkg);
        Device ota = dev.id.loader() ? dev : check(dev, pkg.image, step);
        write(ota, pkg.image, step);
        return verify(pkg.product, step);
    }
}
