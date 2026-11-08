// SPDX-License-Identifier: MPL-2.0
// Putting M-VAVE's stock firmware back on an FM-1 through the chip's own update mode (the mask-ROM UBOOT, USB
// 4C4A:8057): felucca-bsp's tools/fm1_rescue.py, step for step, and with every one of its checks:
//   JieLi's flash loader (wl82loader.bin, checked against its hash) into RAM; the chip key 980F and the flash part
//   checked (856014 or B36014), or nothing is done; the whole flash read and saved first; the stock version whose
//   protected head [0, 0x4000) equals the FM-1's chosen (none: nothing written); only the 4 KiB sectors of the app
//   area that differ written (high to low), each read back; a final full read must equal the expected image.
package fm1.core;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

public class Rescue {
    public static final int VID = 0x4C4A, PID = 0x8057;
    public static final String LOADER_URL = "https://raw.githubusercontent.com/kagaimiq/jl-uboot-tool/"
        + "c32b28e7a04f873f526ba929cb1bae9f05442fe9/data/loaderblobs/usb/wl82loader.bin";
    public static final String LOADER_SHA256 = "d41da6126760c9d66660bcc0cac8d27d221806c5e369a8036921efe68dca5376";
    public static final String MVAVE = "https://yms-file-store.oss-cn-hongkong.aliyuncs.com/software/firmware/FM-1.fwsc";
    public static final String[][] STOCK = {   /* name, sha256, url */
        {"V16", "d017fb65e70aca97ee29d944470c3515a74581df5c0576bf70de917c7b2b61d5", MVAVE},
        {"V15", "db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a",
         "https://web.archive.org/web/20261009203224id_/" + MVAVE}};
    static final int LOADER_ADDR = 0x1C02000, LOADER_ARG = 0x0001;
    static final int FLASH_SIZE = 0x100000, APP_MAX = 0xE0000, WRITE_MIN = 0x4000, SECTOR = 0x1000, IO = 512;
    static final int CMD_WRITE_MEMORY = 0xFB06, CMD_JUMP = 0xFB08;
    static final int CMD_ERASE_SECTOR = 0xFB01, CMD_WRITE_FLASH = 0xFB04, CMD_READ_FLASH = 0xFD05;
    static final int CMD_READ_KEY = 0xFC09, CMD_GET_ONLINE_DEVICE = 0xFC0A, CMD_RUN_APP = 0xFC0C;
    static final byte[] MAGIC = Wire.b(0xC3, 0xCF, 0xC0, 0xE8, 0xCE, 0xD2, 0xB0, 0xAE, 0xC4, 0xE3, 0xA3, 0xAC, 0xD3, 0xF1,
                                       0xC1, 0xD6);

    /** the stock name of a package by its sha256, or null */
    public static String stockName(byte[] raw) {
        String h = Pkg.hex(Pkg.sha256(raw));
        for (String[] s : STOCK) if (s[1].equals(h)) return s[0];
        return null;
    }

    /* ------------------------------------------------- the USB mass-storage bulk-only transport --- */
    public interface Bulk {
        /** write all of d to the bulk OUT endpoint; throws on failure */
        void out(byte[] d, int timeout) throws Fm1Exception;

        /** read up to n bytes from the bulk IN endpoint */
        byte[] in(int n, int timeout) throws Fm1Exception;

        /** bulk-only mass storage reset, both halts cleared */
        void resetRecovery();
    }

    public static final class Bot {
        final Bulk bulk;
        int tag = 1;

        public Bot(Bulk bulk) { this.bulk = bulk; }

        static byte[] cbw(int tag, int length, boolean in, byte[] cdb) {
            byte[] c = new byte[31];
            c[0] = 'U'; c[1] = 'S'; c[2] = 'B'; c[3] = 'C';
            le(c, 4, tag);
            le(c, 8, length);
            c[12] = (byte) (in ? 0x80 : 0);
            c[13] = 0;
            c[14] = (byte) cdb.length;
            System.arraycopy(cdb, 0, c, 15, Math.min(16, cdb.length));
            return c;
        }

        public synchronized byte[] xfer(byte[] cdb, byte[] dataOut, int nIn, int timeout) throws Fm1Exception {
            int t = tag;
            tag = tag + 1;
            int length = dataOut != null ? dataOut.length : nIn;
            bulk.out(cbw(t, length, nIn > 0, cdb), timeout);
            byte[] got = new byte[0];
            if (dataOut != null && dataOut.length > 0) bulk.out(dataOut, timeout);
            else if (nIn > 0) got = bulk.in(nIn, timeout);
            byte[] csw = bulk.in(13, timeout);
            if (csw.length != 13 || csw[0] != 'U' || csw[1] != 'S' || csw[2] != 'B' || csw[3] != 'S' || (int) Pkg.le32(csw, 4) != t)
                throw new Fm1Exception("rescue", "狀態區塊不對（" + Pkg.hex(csw) + "）");
            if (csw[12] != 0) throw new Fm1Exception("rescue", "指令失敗（狀態 " + csw[12] + "）");
            return got;
        }
    }

    static void le(byte[] b, int o, int v) {
        for (int i = 0; i < 4; i++) b[o + i] = (byte) (v >>> (8 * i));
    }

    static byte[] be32(int v) { return new byte[] {(byte) (v >>> 24), (byte) (v >>> 16), (byte) (v >>> 8), (byte) v}; }

    static byte[] cat(byte[]... parts) {
        int n = 0;
        for (byte[] p : parts) n += p.length;
        byte[] o = new byte[n];
        int k = 0;
        for (byte[] p : parts) {
            System.arraycopy(p, 0, o, k, p.length);
            k += p.length;
        }
        return o;
    }

    static byte[] cdbFor(int cmd, byte[] args) {
        byte[] c = new byte[16];
        Arrays.fill(c, (byte) 0xFF);
        c[0] = (byte) (cmd >> 8);
        c[1] = (byte) cmd;
        System.arraycopy(args, 0, c, 2, Math.min(14, args.length));
        return c;
    }

    static void crcCipher(byte[] buf) {        // JieLi's CrcDecode cipher, key 0xFFFFFFFF (in place)
        int crc = Wire.crc16(new byte[] {(byte) 0xFF, (byte) 0xFF}, 0, 2, 0xFFFF);
        for (int i = 0; i < buf.length; i++) {
            crc = Wire.crc16(MAGIC, i % 16, 1, crc);
            buf[i] ^= (byte) (crc & 0xFF);
        }
    }

    /* --------------------------------------------------------------------- the UBOOT device --- */
    public static final class Uboot {
        final Bot bot;

        public Uboot(Bot bot) { this.bot = bot; }

        public String[] inquiry() throws Fm1Exception {
            byte[] r = bot.xfer(new byte[] {0x12, 0, 0, 0, 36, 0}, null, 36, 5000);
            if (r.length < 32) throw new Fm1Exception("rescue", "INQUIRY 回應太短");
            return new String[] {new String(r, 8, 8, StandardCharsets.ISO_8859_1).trim(),
                                 new String(r, 16, 16, StandardCharsets.ISO_8859_1).trim()};
        }

        byte[] cmd(int cmd, byte[] args) throws Fm1Exception {
            byte[] r = bot.xfer(cdbFor(cmd, args), null, 16, 5000);
            if (r.length < 2 || ((r[0] & 0xFF) << 8 | (r[1] & 0xFF)) != cmd)
                throw new Fm1Exception("rescue", String.format("指令 %04X 的回應不對", cmd));
            return Arrays.copyOfRange(r, 2, r.length);
        }

        void writeBlock(int cmd, int addr, byte[] data, int off, int n) throws Fm1Exception {
            byte[] d = Arrays.copyOfRange(data, off, off + n);
            int c = Wire.crc16(d, 0, n, 0);
            byte[] args = cat(be32(addr), new byte[] {(byte) (n >> 8), (byte) n, 0, (byte) c, (byte) (c >> 8)});
            bot.xfer(cdbFor(cmd, args), d, 0, 5000);
        }

        public void startLoader(byte[] blob) throws Fm1Exception {
            for (int off = 0; off < blob.length; off += IO)
                writeBlock(CMD_WRITE_MEMORY, LOADER_ADDR + off, blob, off, Math.min(IO, blob.length - off));
            cmd(CMD_JUMP, cat(be32(LOADER_ADDR), new byte[] {(byte) (LOADER_ARG >> 8), (byte) LOADER_ARG}));
        }

        /** {key, flash type, flash id} */
        public int[] chip() throws Fm1Exception {
            byte[] p = cmd(CMD_READ_KEY, be32(0x00AC6900));
            byte[] k = {p[5], p[4]};
            crcCipher(k);
            int key = (k[0] & 0xFF) | (k[1] & 0xFF) << 8;
            p = cmd(CMD_GET_ONLINE_DEVICE, new byte[0]);
            return new int[] {key, p[0] & 0xFF, (int) Pkg.le32(p, 2)};
        }

        public byte[] read(int addr, int n, Progress pr) throws Fm1Exception {
            byte[] out = new byte[n];
            for (int got = 0; got < n; ) {
                int k = Math.min(IO, n - got);
                byte[] args = cat(be32(addr + got), new byte[] {(byte) (k >> 8), (byte) k});
                byte[] r = bot.xfer(cdbFor(CMD_READ_FLASH, args), null, k, 5000);
                if (r.length != k) throw new Fm1Exception("rescue", "讀取快閃時長度不符");
                System.arraycopy(r, 0, out, got, k);
                got += k;
                if (pr != null && (got % 0x10000 == 0 || got == n)) pr.at(got, n);
            }
            return out;
        }

        public void writeSector(int addr, byte[] image, int end) throws Fm1Exception {
            if (addr < WRITE_MIN || addr >= end || end > APP_MAX || addr % SECTOR != 0)
                throw new Fm1Exception("rescue", String.format("拒絕寫入 %x", addr));
            cmd(CMD_ERASE_SECTOR, be32(addr));
            for (int off = 0; off < SECTOR; off += IO) writeBlock(CMD_WRITE_FLASH, addr + off, image, addr + off, IO);
            byte[] back = read(addr, SECTOR, null);
            if (!Arrays.equals(back, Arrays.copyOfRange(image, addr, addr + SECTOR)))
                throw new Fm1Exception("rescue", String.format("sector %07x 讀回比對不符", addr));
        }

        public void runApp() {
            try { cmd(CMD_RUN_APP, be32(1)); } catch (Exception ignored) { /* it may reset before it answers */ }
        }
    }

    public interface Progress { void at(int done, int total); }

    public interface Say { void say(String s); }

    public interface Confirm { boolean ok(); }

    public interface Saver { String save(byte[] backup) throws Exception; }

    /** the stock versions given: {name, flash image}; checks the loader blob's hash */
    public static byte[] checkLoader(byte[] blob) throws Fm1Exception {
        if (!Pkg.hex(Pkg.sha256(blob)).equals(LOADER_SHA256))
            throw new Fm1Exception("rescue", "wl82loader.bin 的內容不對（雜湊值不符）");
        return blob;
    }

    /** everything in one session (the FM-1 leaves UBOOT as soon as it is let go); -> true when written */
    public static boolean rescue(Uboot dev, List<Object[]> packages, byte[] loader, boolean write, Saver saver, Say say,
                                 Confirm confirm, Progress pr) throws Fm1Exception {
        String[] iq = dev.inquiry();
        say.say("裝置：" + iq[0] + " " + iq[1]);
        if (!iq[0].equals("WL82")) throw new Fm1Exception("rescue", "這不是 WL82 UBOOT（" + iq[0] + "）");
        dev.startLoader(loader);
        int[] c = dev.chip();
        say.say(String.format("晶片金鑰 %04X，快閃類型 %d，型號 %06X", c[0], c[1], c[2]));
        if (c[0] != 0x980F || c[1] != 3 || (c[2] != 0x856014 && c[2] != 0xB36014))
            throw new Fm1Exception("rescue", "這不是 FM-1 的晶片和快閃（應為金鑰 980F、類型 3、型號 856014 或 B36014）；沒有寫入任何東西");
        say.say("讀取整顆快閃（備份）…");
        byte[] now = dev.read(0, FLASH_SIZE, pr);
        String path;
        try {
            path = saver.save(now);
        } catch (Exception e) {
            throw new Fm1Exception("rescue", "備份存檔失敗（" + e.getMessage() + "）；沒有寫入任何東西");
        }
        say.say("備份已存：" + path);
        Object[] match = null;
        for (Object[] p : packages) {
            byte[] im = (byte[]) p[1];
            if (Arrays.equals(Arrays.copyOf(im, WRITE_MIN), Arrays.copyOf(now, WRITE_MIN))) {
                match = p;
                break;
            }
        }
        if (match == null)
            throw new Fm1Exception("rescue", "FM-1 受保護的開機區跟提供的原廠版本都不一樣：不寫入。請把備份檔留著");
        String name = (String) match[0];
        byte[] image = (byte[]) match[1];
        int end = image.length;
        say.say("FM-1 的開機區是 " + name + " 的");
        List<Integer> diff = new ArrayList<>();
        for (int a = WRITE_MIN; a < end; a += SECTOR)
            if (!Arrays.equals(Arrays.copyOfRange(image, a, a + SECTOR), Arrays.copyOfRange(now, a, a + SECTOR))) diff.add(a);
        say.say(diff.size() + " / " + end / SECTOR + " 個 sector 跟 " + name + " 不同");
        if (!write && confirm != null) write = keepAlive(dev, confirm);
        if (!write) {
            say.say("檢查通過。沒有寫入任何東西。");
            return false;
        }
        // high to low: a firmware's start-up code (low in the app area) is the last thing replaced if cut short
        for (int i = diff.size() - 1, k = 0; i >= 0; i--, k++) {
            int a = diff.get(i);
            for (int attempt = 0; ; attempt++) {
                try {
                    dev.writeSector(a, image, end);
                    break;
                } catch (Fm1Exception e) {
                    if (attempt == 2) throw new Fm1Exception("rescue", e.getMessage() + "；FM-1 仍在更新模式：再執行一次");
                }
            }
            if (pr != null) pr.at(k + 1, diff.size());
        }
        say.say("最後整顆讀回比對…");
        byte[] expect = Arrays.copyOf(image, FLASH_SIZE);
        System.arraycopy(now, end, expect, end, FLASH_SIZE - end);
        byte[] fin = dev.read(0, FLASH_SIZE, pr);
        if (!Arrays.equals(fin, expect)) {
            StringBuilder bad = new StringBuilder();
            for (int a = 0; a < FLASH_SIZE; a += SECTOR)
                if (!Arrays.equals(Arrays.copyOfRange(fin, a, a + SECTOR), Arrays.copyOfRange(expect, a, a + SECTOR)))
                    bad.append(String.format(" %07x", a));
            throw new Fm1Exception("rescue", "最後讀回在這些 sector 不符：" + bad + "；請再寫一次");
        }
        say.say("快閃現在是 " + name + "。FM-1 重新開機…");
        dev.runApp();
        return true;
    }

    /** ask while the loader is kept busy (it resets the chip after ~3 s without a command) */
    static boolean keepAlive(Uboot dev, Confirm confirm) throws Fm1Exception {
        final Exception[] lost = {null};
        final boolean[] stop = {false};
        Thread t = new Thread(() -> {
            while (true) {
                synchronized (stop) {
                    try { stop.wait(1000); } catch (InterruptedException e) { return; }
                    if (stop[0]) return;
                }
                try { dev.cmd(CMD_GET_ONLINE_DEVICE, new byte[0]); } catch (Exception e) { lost[0] = e; return; }
            }
        });
        t.setDaemon(true);
        t.start();
        boolean answer;
        try {
            answer = confirm.ok();
        } finally {
            synchronized (stop) {
                stop[0] = true;
                stop.notifyAll();
            }
            try { t.join(); } catch (InterruptedException ignored) { }
        }
        if (lost[0] != null) throw new Fm1Exception("rescue", "等待時 FM-1 斷線了（" + lost[0].getMessage() + "）；再執行一次");
        return answer;
    }
}
