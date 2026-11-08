// SPDX-License-Identifier: MPL-2.0
// The FM-1 update protocol's wire format, as fm1-base's tools/fm1_install.py has it (felucca-bsp chip/ota.c): the 7-bit
// packing, the M-UPGRADE frames, the read requests and their answers, the identity handshake.
package fm1.core;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.zip.CRC32;

public final class Wire {
    private Wire() {}

    public static final byte[] HS_QUERY = b(0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7);
    public static final byte[] HS_FULL = b(0xF0, 0x00, 0x32, 0x45, 0x08, 0x00, 0x00, 0x40, 0x00, 0x7E, 0x01, 0xF7);
    public static final byte[] UPGRADE = b(0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7);
    public static final long FINISH_CHECK = 0xE0000000L, FINISH_WRITE = 0xF0000000L;
    public static final int MAXDATA = 512;
    public static final int FAST_CHUNK = 4096;
    public static final long TABLE = 0xD0000000L, TABLE_END = 0xE0000000L;

    static byte[] b(int... v) {
        byte[] o = new byte[v.length];
        for (int i = 0; i < v.length; i++) o[i] = (byte) v[i];
        return o;
    }

    public static byte[] pack7(byte[] data) {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        long acc = 0;
        int nb = 0;
        for (byte x : data) {
            acc |= (long) (x & 0xFF) << nb;
            nb += 8;
            while (nb >= 7) {
                o.write((int) (acc & 0x7F));
                acc >>= 7;
                nb -= 7;
            }
        }
        if (nb > 0) o.write((int) (acc & 0x7F));
        return o.toByteArray();
    }

    public static byte[] unpack7(byte[] s, int from, int to) {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        long acc = 0;
        int nb = 0;
        for (int i = from; i < to; i++) {
            acc |= (long) (s[i] & 0x7F) << nb;
            nb += 7;
            while (nb >= 8) {
                o.write((int) (acc & 0xFF));
                acc >>= 8;
                nb -= 8;
            }
        }
        return o.toByteArray();
    }

    static boolean isSysex(byte[] p) {
        return p != null && p.length >= 2 && (p[0] & 0xFF) == 0xF0 && (p[p.length - 1] & 0xFF) == 0xF7;
    }

    static byte[] wrap(byte[] body) {
        byte[] p = pack7(body);
        byte[] o = new byte[p.length + 2];
        o[0] = (byte) 0xF0;
        System.arraycopy(p, 0, o, 1, p.length);
        o[o.length - 1] = (byte) 0xF7;
        return o;
    }

    static int sum(byte[] a, int from, int to) {
        int s = 0;
        for (int i = from; i < to; i++) s += a[i] & 0xFF;
        return s;
    }

    /** an M-UPGRADE command: F0 pack7(00 59 cmd len16 00 payload ~sum) F7 */
    public static byte[] frame(int cmd, byte[] payload) {
        int n = payload.length;
        byte[] body = new byte[6 + n + 1];
        body[0] = 0;
        body[1] = 0x59;
        body[2] = (byte) cmd;
        body[3] = (byte) (n & 0xFF);
        body[4] = (byte) (n >> 8);
        body[5] = 0;
        System.arraycopy(payload, 0, body, 6, n);
        body[6 + n] = (byte) (~sum(payload, 0, n) & 0xFF);
        return wrap(body);
    }

    /** -> {cmd, body...} of any frame in the update protocol's framing (checksum checked), or null */
    public static byte[][] parseFrame(byte[] p) {
        if (!isSysex(p)) return null;
        byte[] u = unpack7(p, 1, p.length - 1);
        if (u.length < 7 || u[0] != 0 || u[1] != 0x59) return null;
        int n = (u[3] & 0xFF) | (u[4] & 0xFF) << 8 | (u[5] & 0xFF) << 16;
        if (u.length != 7 + n || (~sum(u, 6, 6 + n) & 0xFF) != (u[6 + n] & 0xFF)) return null;
        byte[] body = new byte[n];
        System.arraycopy(u, 6, body, 0, n);
        return new byte[][] {new byte[] {u[2]}, body};
    }

    /** the answer to a read request */
    public static byte[] response(long addr, byte[] data, int off, int n, int fl) {
        byte[] body = new byte[7 + 4 + 3 + n + 1];
        body[0] = 0;
        body[1] = 0x59;
        body[2] = 0x30;
        body[3] = (byte) ((n + 8) & 0xFF);
        body[4] = (byte) (((n + 8) >> 8) & 0xFF);
        body[5] = 0;
        body[6] = (byte) fl;
        for (int i = 0; i < 4; i++) body[7 + i] = (byte) (addr >>> (8 * i));
        body[11] = (byte) (n & 0xFF);
        body[12] = (byte) ((n >> 8) & 0xFF);
        body[13] = 0;
        System.arraycopy(data, off, body, 14, n);
        body[14 + n] = (byte) (~sum(body, 6, 14 + n) & 0xFF);
        return wrap(body);
    }

    public static final byte[] SUCCESS = "success\0".getBytes(StandardCharsets.US_ASCII);

    /** a read request -> {flags, addr, len}, or null */
    public static long[] parseRequest(byte[] p) {
        if (!isSysex(p)) return null;
        byte[] u = unpack7(p, 1, p.length - 1);
        if (u.length != 15 || u[0] != 0 || u[1] != 0x59 || u[2] != 0x30) return null;
        if ((~sum(u, 6, 14) & 0xFF) != (u[14] & 0xFF)) return null;
        long addr = (u[7] & 0xFFL) | (u[8] & 0xFFL) << 8 | (u[9] & 0xFFL) << 16 | (u[10] & 0xFFL) << 24;
        long len = (u[11] & 0xFFL) | (u[12] & 0xFFL) << 8 | (u[13] & 0xFFL) << 16;
        return new long[] {u[6] & 0xFF, addr, len};
    }

    /** the device's identity */
    public static final class Identity {
        public final String text, model;
        public final int flash;

        Identity(String text, int flash) {
            this.text = text;
            this.flash = flash;
            int u = text.indexOf('_');
            this.model = u < 0 ? text : text.substring(0, u);
        }

        public boolean loader() { return model.toLowerCase().startsWith("ota-"); }

        public boolean open() { return text.matches("FM1_(OFFICIAL_OPEN|COMMUNITY_\\d+)"); }

        @Override public String toString() { return text; }
    }

    public static Identity parseIdentity(byte[] p) {
        if (!isSysex(p)) return null;
        byte[] d = unpack7(p, 1, p.length - 1);
        if (d.length != 34 || d[0] != 0 || d[1] != 0x59 || d[2] != 0x11) return null;
        int end = 6;
        while (end < 33 && d[end] != 0) end++;
        String txt = new String(d, 6, end - 6, StandardCharsets.ISO_8859_1);
        int flash = txt.length() < 24 ? (d[30] & 0xFF) << 16 | (d[31] & 0xFF) << 8 | (d[32] & 0xFF) : 0;
        return new Identity(txt, flash);
    }

    /** the UFW's CRC16 (XMODEM: poly 0x1021, init c) */
    public static int crc16(byte[] data, int off, int n, int c) {
        for (int i = off; i < off + n; i++) {
            c ^= (data[i] & 0xFF) << 8;
            for (int k = 0; k < 8; k++) c = (c & 0x8000) != 0 ? ((c << 1) ^ 0x1021) & 0xFFFF : (c << 1) & 0xFFFF;
        }
        return c;
    }

    /** the fast install's sector table (kept for completeness; this app installs classic only) */
    public static byte[] sectorTable(byte[] image, int off, int n) {
        byte[] o = new byte[n * 6];
        for (int k = 0; k < n; k++) {
            int a = off + k * FAST_CHUNK, len = Math.max(0, Math.min(FAST_CHUNK, image.length - a));
            int c16 = crc16(image, a, len, 0);
            CRC32 c = new CRC32();
            c.update(image, a, len);
            long c32 = c.getValue();
            o[k * 6] = (byte) c16;
            o[k * 6 + 1] = (byte) (c16 >> 8);
            for (int i = 0; i < 4; i++) o[k * 6 + 2 + i] = (byte) (c32 >>> (8 * i));
        }
        return o;
    }
}
