// SPDX-License-Identifier: MPL-2.0
// CHAIN-1's unit bank over USB-MIDI (its app.c, APP_USB_CMD; chain1/tools/fm1_units.py does the same): in the update's
// framing, LIST (0x70 -> 0x78), PUT (0x71: the FM-1 then reads the image from us as an update reads a package, our
// answers to its read requests, -> 0x79) and ERASE (0x72 -> 0x79).
package fm1.core;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

public final class Units {
    public static final int LIST = 0x70, PUT = 0x71, ERASE = 0x72, LIST_R = 0x78, RESULT = 0x79, SLOTS = 7;

    public static final class Entry {
        public final int slot, module, fmt;
        public final boolean used;
        public final String name;

        Entry(int slot, boolean used, int module, int fmt, String name) {
            this.slot = slot;
            this.used = used;
            this.module = module;
            this.fmt = fmt;
            this.name = name;
        }
    }

    public interface Progress {
        void at(int done, int total);
    }

    static String why(int rc) {
        switch (rc) {
            case 2: return "FM-1 的快閃寫入失敗";
            case 3: return "7 個位置都滿了，先刪掉一個";
            case 4: return "單元太大（一個位置 36 KB）";
            case 5: return "手機沒有及時回應";
            case 6: return "CHAIN-1 不能執行這個單元（不是給 NTS-1 的，或檔案壞了）";
            default: return "錯誤 " + rc;
        }
    }

    /** the frames (or read requests, answered from image) until cmd arrives -> its body */
    static byte[] await(Midi.Link link, int cmd, long ms, byte[] image, Progress pr) throws Fm1Exception {
        long end = System.currentTimeMillis() + ms;
        while (System.currentTimeMillis() < end) {
            byte[] p = link.read(500);
            if (p == null) {
                if (link.lost()) throw new Fm1Exception("lost", "FM-1 斷線了");
                continue;
            }
            long[] r = Wire.parseRequest(p);
            if (r != null && image != null) {
                long addr = r[1];
                int n = (int) r[2];
                if (n > Wire.MAXDATA || addr < 0 || addr + n > image.length)
                    throw new Fm1Exception("badreq", "FM-1 要求了單元以外的資料");
                link.send(Wire.response(addr, image, (int) addr, n, (int) r[0]));
                end = System.currentTimeMillis() + ms;
                if (pr != null) pr.at((int) (addr + n), image.length);
                continue;
            }
            byte[][] f = Wire.parseFrame(p);
            if (f != null && (f[0][0] & 0xFF) == cmd) return f[1];
        }
        throw new Fm1Exception("noanswer", "FM-1 沒有回應：它要在跑 CHAIN-1 1.0 以上");
    }

    public static List<Entry> list(Midi.Link link) throws Fm1Exception {
        link.drain();
        link.send(Wire.frame(LIST, new byte[0]));
        byte[] b = await(link, LIST_R, 5000, null, null);
        List<Entry> out = new ArrayList<>();
        for (int s = 0; s + 1 <= b.length / 23; s++) {
            int o = 23 * s, k = 0;
            while (k < 20 && b[o + 3 + k] != 0) k++;
            out.add(new Entry(s, b[o] != 0, b[o + 1] & 0xFF, b[o + 2] & 0xFF, new String(b, o + 3, k, StandardCharsets.US_ASCII)));
        }
        return out;
    }

    /** the image into slot (0..6, or -1: the first free one) -> the slot it went to */
    public static int put(Midi.Link link, byte[] image, int slot, Progress pr) throws Fm1Exception {
        byte[] body = new byte[9];
        long crc = LogueUnit.crc32(image);
        body[0] = (byte) (slot < 0 ? 0xFF : slot);
        LogueUnit.put32(body, 1, image.length);
        LogueUnit.put32(body, 5, crc);
        link.drain();
        link.send(Wire.frame(PUT, body));
        byte[] r = await(link, RESULT, 15000, image, pr);
        if (r.length < 2) throw new Fm1Exception("unit", "FM-1 的回應不對");
        if (r[0] != 0) throw new Fm1Exception("unit", "沒有安裝：" + why(r[0] & 0xFF));
        return r[1] & 0xFF;
    }

    public static void erase(Midi.Link link, int slot) throws Fm1Exception {
        link.drain();
        link.send(Wire.frame(ERASE, new byte[] {(byte) slot}));
        byte[] r = await(link, RESULT, 10000, null, null);
        if (r.length < 1 || r[0] != 0) throw new Fm1Exception("unit", "刪除失敗：" + why(r.length > 0 ? r[0] & 0xFF : 1));
    }
}
