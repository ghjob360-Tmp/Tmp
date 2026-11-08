// SPDX-License-Identifier: MPL-2.0
// An FM-1 package (.fwsc): its identity, the logical image the device reads during an update, and (for the UBOOT
// rescue) its raw flash entry, as fm1-base's tools/fm1_install.py and felucca-bsp's tools/fm1_rescue.py read them.
package fm1.core;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;

public final class Pkg {
    public static final int BLOCKS = 20, BLK = 0x30, KEEP = 0x2F;
    static final byte[] LOADER_MARK = "FELUCCA-LOADER-1".getBytes(StandardCharsets.US_ASCII);

    public final byte[] raw, image, sha256;
    public final String product;
    public final boolean hasLoader;

    public Pkg(byte[] raw) throws Fm1Exception {
        if (raw.length < BLOCKS * BLK) throw new Fm1Exception("badpkg", "這不是 FM-1 韌體檔（太短）");
        this.raw = raw;
        this.product = productOf(raw);
        if (!product.matches("[^_]+_\\d+")) throw new Fm1Exception("badpkg", "這不是 FM-1 韌體檔（識別碼 " + product + "）");
        this.image = logicalImage(raw);
        this.sha256 = sha256(raw);
        this.hasLoader = indexOf(raw, LOADER_MARK) >= 0;
        if (!hasLoader && !isStock(product))
            throw new Fm1Exception("badpkg", "這個檔案沒有 Felucca 更新載入器，也不是 M-VAVE 原廠韌體");
    }

    /** the package identity ("FM-1_95010300"): one marker byte after each of the first 20 blocks */
    public static String productOf(byte[] raw) {
        StringBuilder s = new StringBuilder();
        for (int i = 0; i < BLOCKS; i++) {
            int m = raw[i * BLK + KEEP] & 0xFF;
            if (m != 0x7D) s.append((char) ((m - i - 1) & 0xFF));
        }
        return s.toString();
    }

    /** the image the device reads during an update: the package without the 20 marker bytes */
    public static byte[] logicalImage(byte[] raw) {
        ByteArrayOutputStream o = new ByteArrayOutputStream(raw.length);
        for (int i = 0; i < BLOCKS; i++) o.write(raw, i * BLK, KEEP);
        o.write(raw, BLOCKS * BLK, raw.length - BLOCKS * BLK);
        return o.toByteArray();
    }

    /** M-VAVE's FM-1 firmware, any version (FM-1_0 and two digits), not m-bootloader's installer (FM-1_099) */
    public static boolean isStock(String product) {
        return product.matches("FM-1_0\\d\\d") && !product.equals("FM-1_099");
    }

    public static String modelOf(String text) {
        String m = text.split("_")[0];
        return m.startsWith("ota-") ? m.substring(4) : m;
    }

    static int indexOf(byte[] a, byte[] n) {
        outer:
        for (int i = 0; i + n.length <= a.length; i++) {
            for (int k = 0; k < n.length; k++) if (a[i + k] != n[k]) continue outer;
            return i;
        }
        return -1;
    }

    public static byte[] sha256(byte[] d) {
        try {
            return MessageDigest.getInstance("SHA-256").digest(d);
        } catch (Exception e) {
            throw new RuntimeException(e);
        }
    }

    public static String hex(byte[] d) {
        StringBuilder s = new StringBuilder();
        for (byte x : d) s.append(String.format("%02x", x & 0xFF));
        return s.toString();
    }

    /* ---- the rescue's view: the package's type-0 entry, raw flash [0, the end of its app area) ---- */
    static void enc(byte[] buf, int off, int n) {      // JieLi's ENC stream cipher, key 0xFFFF (in place)
        int key = 0xFFFF;
        for (int i = off; i < off + n; i++) {
            buf[i] ^= (byte) (key & 0xFF);
            key = ((key << 1) ^ ((key & 0x8000) != 0 ? 0x1021 : 0)) & 0xFFFF;
        }
    }

    public static byte[] flashImage(byte[] raw) throws Fm1Exception {
        byte[] logical = logicalImage(raw);
        byte[] hdr = new byte[0x40];
        System.arraycopy(logical, 0, hdr, 0, 0x40);
        enc(hdr, 0, 0x40);
        int n = (hdr[8] & 0xFF) | (hdr[9] & 0xFF) << 8;
        String chip = new String(hdr, 16, 6, StandardCharsets.ISO_8859_1);
        if (!chip.equals("AC791N") || n <= 0 || n >= 16) throw new Fm1Exception("badpkg", "這不是 FM-1 韌體檔（檔頭）");
        for (int i = 0; i < n; i++) {
            byte[] e = new byte[0x50];
            System.arraycopy(logical, 0x40 + i * 0x50, e, 0, 0x50);
            enc(e, 0, 0x50);
            int typ = (e[0] & 0xFF) | (e[1] & 0xFF) << 8;
            int dcrc = (e[4] & 0xFF) | (e[5] & 0xFF) << 8;
            long off = le32(e, 8), size = le32(e, 12);
            if (typ == 0) {
                if (off + size > logical.length) throw new Fm1Exception("badpkg", "韌體檔的快閃區塊不完整");
                byte[] data = new byte[(int) size];
                System.arraycopy(logical, (int) off, data, 0, (int) size);
                if (Wire.crc16(data, 0, data.length, 0) != dcrc) throw new Fm1Exception("badpkg", "韌體檔的快閃區塊 CRC 不符");
                return data;
            }
        }
        throw new Fm1Exception("badpkg", "韌體檔沒有快閃區塊");
    }

    static long le32(byte[] b, int o) {
        return (b[o] & 0xFFL) | (b[o + 1] & 0xFFL) << 8 | (b[o + 2] & 0xFFL) << 16 | (b[o + 3] & 0xFFL) << 24;
    }
}
