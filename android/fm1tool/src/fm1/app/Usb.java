// SPDX-License-Identifier: MPL-2.0
// USB on Android, straight to the FM-1's endpoints (android.hardware.usb), never Android's MIDI service:
//  - Midi: every attached USB device with a MIDI streaming interface (class 1, subclass 3) is a port; a link claims
//    that interface and moves USB-MIDI event packets on its bulk endpoints (fm1.core.UsbMidiCodec);
//  - UBOOT: the chip's update mode (4C4A:8057), a mass-storage interface whose bulk endpoints carry the rescue's
//    bulk-only transport (fm1.core.Rescue.Bot).
// Permission is asked for each device as it appears (the FM-1 is a new USB device in each of its modes); the asking
// thread waits for the person's answer.
package fm1.app;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.os.Build;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

import fm1.core.Fm1Exception;
import fm1.core.Midi;
import fm1.core.Rescue;
import fm1.core.UsbMidiCodec;

final class Usb {
    static final String ACTION = "fm1.app.USB_PERMISSION";
    final Context ctx;
    final UsbManager mgr;

    Usb(Context ctx) {
        this.ctx = ctx;
        this.mgr = (UsbManager) ctx.getSystemService(Context.USB_SERVICE);
    }

    /** permission for dev: asked if not held; waits for the answer (up to ms) */
    boolean permit(UsbDevice dev, long ms) {
        if (mgr.hasPermission(dev)) return true;
        final CountDownLatch done = new CountDownLatch(1);
        BroadcastReceiver r = new BroadcastReceiver() {
            @Override public void onReceive(Context c, Intent i) {
                if (ACTION.equals(i.getAction())) done.countDown();
            }
        };
        IntentFilter f = new IntentFilter(ACTION);
        if (Build.VERSION.SDK_INT >= 33) ctx.registerReceiver(r, f, Context.RECEIVER_NOT_EXPORTED);
        else ctx.registerReceiver(r, f);
        try {
            Intent i = new Intent(ACTION).setPackage(ctx.getPackageName());
            int fl = Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0;
            mgr.requestPermission(dev, PendingIntent.getBroadcast(ctx, 0, i, fl));
            done.await(ms, TimeUnit.MILLISECONDS);
        } catch (InterruptedException ignored) {
        } finally {
            try { ctx.unregisterReceiver(r); } catch (Exception ignored) { }
        }
        return mgr.hasPermission(dev);
    }

    static String name(UsbDevice d) {
        String p = Build.VERSION.SDK_INT >= 21 ? d.getProductName() : null;
        return (p == null ? "USB" : p) + String.format(" (%04x:%04x) #%d", d.getVendorId(), d.getProductId(), d.getDeviceId());
    }

    UsbDevice byName(String n) {
        for (UsbDevice d : mgr.getDeviceList().values()) if (name(d).equals(n)) return d;
        return null;
    }

    /* ---------------------------------------------------------------------------- MIDI --- */
    static UsbInterface midiInterface(UsbDevice d) {
        for (int i = 0; i < d.getInterfaceCount(); i++) {
            UsbInterface it = d.getInterface(i);
            if (it.getInterfaceClass() == UsbConstants.USB_CLASS_AUDIO && it.getInterfaceSubclass() == 3 && bulk(it, true) != null
                && bulk(it, false) != null) return it;
        }
        return null;
    }

    static UsbEndpoint bulk(UsbInterface it, boolean in) {
        for (int e = 0; e < it.getEndpointCount(); e++) {
            UsbEndpoint ep = it.getEndpoint(e);
            if (ep.getType() == UsbConstants.USB_ENDPOINT_XFER_BULK
                && (ep.getDirection() == UsbConstants.USB_DIR_IN) == in) return ep;
        }
        return null;
    }

    final Midi midi = new Ports();

    final class Ports implements Midi {
        @Override public List<String> ports() {
            List<String> o = new ArrayList<>();
            for (UsbDevice d : mgr.getDeviceList().values()) if (midiInterface(d) != null) o.add(name(d));
            return o;
        }

        @Override public Link open(String port) {
            UsbDevice d = byName(port);
            if (d == null || !permit(d, 90000)) return null;
            UsbInterface it = midiInterface(d);
            UsbDeviceConnection c = mgr.openDevice(d);
            if (it == null || c == null) return null;
            if (!c.claimInterface(it, true)) {
                c.close();
                return null;
            }
            return new MidiLink(d, c, it);
        }
    }

    final class MidiLink implements Midi.Link {
        final UsbDevice dev;
        final UsbDeviceConnection c;
        final UsbInterface it;
        final UsbEndpoint in, out;
        final BlockingQueue<byte[]> q = new LinkedBlockingQueue<>();
        volatile boolean closed;
        final Thread reader;

        MidiLink(UsbDevice dev, UsbDeviceConnection c, UsbInterface it) {
            this.dev = dev;
            this.c = c;
            this.it = it;
            this.in = bulk(it, true);
            this.out = bulk(it, false);
            reader = new Thread(() -> {
                UsbMidiCodec.Decoder dec = new UsbMidiCodec.Decoder();
                byte[] buf = new byte[Math.max(64, in.getMaxPacketSize())];
                while (!closed) {
                    int n = c.bulkTransfer(in, buf, buf.length, 100);
                    if (n > 0) q.addAll(dec.feed(buf, n));
                    else if (lost()) break;
                }
            }, "fm1-midi-in");
            reader.setDaemon(true);
            reader.start();
        }

        @Override public boolean send(byte[] sysex) {
            if (closed) return false;
            byte[] pk = UsbMidiCodec.encodeSysex(sysex);
            int mp = Math.max(4, out.getMaxPacketSize() / 4 * 4);
            for (int o = 0; o < pk.length; ) {
                int n = Math.min(mp, pk.length - o);
                byte[] chunk = new byte[n];
                System.arraycopy(pk, o, chunk, 0, n);
                int w = c.bulkTransfer(out, chunk, n, 2000);
                if (w < 0) {
                    if (lost()) return false;
                    continue;                          // (a timeout: the same chunk again)
                }
                o += w;
            }
            return true;
        }

        @Override public byte[] read(long ms) {
            try { return q.poll(Math.max(1, ms), TimeUnit.MILLISECONDS); } catch (InterruptedException e) { return null; }
        }

        @Override public void drain() { q.clear(); }

        @Override public boolean lost() {
            for (UsbDevice d : mgr.getDeviceList().values()) if (d.getDeviceId() == dev.getDeviceId()) return false;
            return true;
        }

        @Override public void close() {
            closed = true;
            try { reader.join(500); } catch (InterruptedException ignored) { }
            try { c.releaseInterface(it); } catch (Exception ignored) { }
            c.close();
        }
    }

    /* --------------------------------------------------------------------------- UBOOT --- */
    final class Uboot implements Rescue.Bulk {
        final UsbDeviceConnection c;
        final UsbInterface it;
        final UsbEndpoint in, out;

        Uboot(UsbDevice d) throws Fm1Exception {
            UsbInterface f = null;
            for (int i = 0; i < d.getInterfaceCount() && f == null; i++)
                if (bulk(d.getInterface(i), true) != null && bulk(d.getInterface(i), false) != null) f = d.getInterface(i);
            if (f == null) throw new Fm1Exception("rescue", "UBOOT 裝置沒有 bulk 端點");
            c = mgr.openDevice(d);
            if (c == null) throw new Fm1Exception("rescue", "打不開 UBOOT 裝置");
            if (!c.claimInterface(f, true)) {
                c.close();
                throw new Fm1Exception("rescue", "無法取得 UBOOT 裝置的介面");
            }
            it = f;
            in = bulk(f, true);
            out = bulk(f, false);
        }

        @Override public void out(byte[] d, int timeout) throws Fm1Exception {
            for (int o = 0; o < d.length; ) {
                int n = Math.min(16384, d.length - o);
                byte[] chunk = new byte[n];
                System.arraycopy(d, o, chunk, 0, n);
                int w = c.bulkTransfer(out, chunk, n, timeout);
                if (w <= 0) throw new Fm1Exception("rescue", "USB 寫入失敗");
                o += w;
            }
        }

        @Override public byte[] in(int n, int timeout) throws Fm1Exception {
            byte[] buf = new byte[Math.max(n, 13)];
            int got = 0;
            while (got < n) {
                byte[] part = new byte[Math.min(16384, buf.length - got)];
                int r = c.bulkTransfer(in, part, part.length, timeout);
                if (r <= 0) throw new Fm1Exception("rescue", "USB 讀取失敗");
                System.arraycopy(part, 0, buf, got, r);
                got += r;
                if (r % in.getMaxPacketSize() != 0) break;   // a short packet ends the transfer
            }
            byte[] o = new byte[got];
            System.arraycopy(buf, 0, o, 0, got);
            return o;
        }

        @Override public void resetRecovery() {
            c.controlTransfer(0x21, 0xFF, 0, it.getId(), null, 0, 2000);
            for (UsbEndpoint ep : new UsbEndpoint[] {in, out})
                c.controlTransfer(0x02, 1, 0, ep.getAddress(), null, 0, 1000);   // CLEAR_FEATURE(ENDPOINT_HALT)
        }

        void close() {
            try { c.releaseInterface(it); } catch (Exception ignored) { }
            c.close();
        }
    }

    /** wait for the FM-1 in UBOOT (up to ms), permission asked, the device opened and answering INQUIRY */
    Uboot waitUboot(long ms, Rescue.Say say) throws Fm1Exception {
        long end = System.currentTimeMillis() + ms;
        while (System.currentTimeMillis() < end) {
            for (UsbDevice d : mgr.getDeviceList().values()) {
                if (d.getVendorId() != Rescue.VID || d.getProductId() != Rescue.PID) continue;
                if (!permit(d, 90000)) throw new Fm1Exception("rescue", "沒有取得 USB 權限（請在詢問時按「允許」）");
                Uboot u = new Uboot(d);
                Rescue.Bot bot = new Rescue.Bot(u);
                try {
                    new Rescue.Uboot(bot).inquiry();
                } catch (Fm1Exception e) {
                    u.resetRecovery();
                }
                return u;
            }
            try { Thread.sleep(200); } catch (InterruptedException e) { break; }
        }
        throw new Fm1Exception("rescue", "沒看到 UBOOT 模式的 FM-1（USB 4C4A:8057）");
    }
}
