// SPDX-License-Identifier: MPL-2.0
// USB MIDI 1.0 event packets (4 bytes: cable/CIN, then 3 MIDI bytes) for SysEx, both ways: what the app sends straight
// to the FM-1's bulk OUT endpoint and reads from its bulk IN, without Android's MIDI service.
package fm1.core;

import java.io.ByteArrayOutputStream;
import java.util.ArrayList;
import java.util.List;

public final class UsbMidiCodec {
    /** one SysEx message (F0 .. F7) as USB-MIDI event packets on cable 0 */
    public static byte[] encodeSysex(byte[] m) {
        ByteArrayOutputStream o = new ByteArrayOutputStream((m.length / 3 + 2) * 4);
        int i = 0, n = m.length;
        while (n - i > 3) {                            // CIN 4: start or continue, three bytes
            o.write(0x04);
            o.write(m, i, 3);
            i += 3;
        }
        int left = n - i;                              // CIN 5 / 6 / 7: the end with one / two / three bytes
        o.write(left == 1 ? 0x05 : left == 2 ? 0x06 : 0x07);
        for (int k = 0; k < 3; k++) o.write(k < left ? m[i + k] & 0xFF : 0);
        return o.toByteArray();
    }

    /** a decoder of the IN stream: whole SysEx messages out (other messages dropped) */
    public static final class Decoder {
        final ByteArrayOutputStream cur = new ByteArrayOutputStream();
        boolean in;

        public List<byte[]> feed(byte[] buf, int len) {
            List<byte[]> out = new ArrayList<>();
            for (int p = 0; p + 4 <= len; p += 4) {
                int cin = buf[p] & 0x0F;
                int n = cin == 0x4 || cin == 0x7 ? 3 : cin == 0x6 ? 2 : cin == 0x5 ? 1 : 0;
                if (n == 0) {                          // not SysEx (or an empty packet: zero padding)
                    continue;
                }
                for (int k = 0; k < n; k++) {
                    int b = buf[p + 1 + k] & 0xFF;
                    if (b == 0xF0) {
                        cur.reset();
                        in = true;
                    }
                    if (!in) continue;
                    cur.write(b);
                    if (b == 0xF7) {
                        out.add(cur.toByteArray());
                        cur.reset();
                        in = false;
                    } else if (cur.size() > 70000) {   // (runaway: drop it)
                        cur.reset();
                        in = false;
                    }
                }
            }
            return out;
        }
    }
}
