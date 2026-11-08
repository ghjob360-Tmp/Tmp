// SPDX-License-Identifier: MPL-2.0
// FM-1 Tool: pick a firmware package (.fwsc) and install it over USB, or rescue an FM-1 stuck in UBOOT by writing
// M-VAVE's stock firmware back. One screen, built in code (no layout resources); the work runs on a background thread.
package fm1.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ContentValues;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.MediaStore;
import android.provider.OpenableColumns;
import android.text.method.ScrollingMovementMethod;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.CountDownLatch;

import fm1.core.Fm1Exception;
import fm1.core.Installer;
import fm1.core.Pkg;
import fm1.core.Rescue;

public class MainActivity extends Activity {
    static final int PICK = 1;
    Usb usb;
    TextView fileView, status, log;
    ProgressBar bar;
    Button pick, install, stock, rescue, check, stop;
    ScrollView logScroll;
    Pkg pkg;
    String pkgName;
    volatile Installer running;
    volatile boolean busy;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        usb = new Usb(this);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        int pad = dp(16);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);

        root.addView(title("安裝韌體"));
        root.addView(note("FM-1 開機（或在更新模式），用 USB-C OTG 線接到手機。"));
        pick = button("選擇韌體檔 (.fwsc)", v -> pickFile());
        root.addView(pick);
        fileView = note("還沒選檔案");
        root.addView(fileView);
        install = button("安裝到 FM-1", v -> startInstall());
        install.setEnabled(false);
        root.addView(install);
        stock = button("下載並安裝 M-VAVE 原廠韌體 (V16)", v -> startStockInstall());
        root.addView(stock);

        root.addView(title("UBOOT 救援（還原原廠韌體）"));
        root.addView(note("FM-1 開不了機時：關機，按住 REC 開機（或照救援說明進入 UBOOT），電腦/手機會看到 WL82 UBOOT。" +
                          "這裡會先備份整顆快閃，確認開機區是原廠 V15/V16 後，只改寫應用程式區。"));
        check = button("只檢查並備份（不寫入）", v -> startRescue(false));
        root.addView(check);
        rescue = button("檢查、備份並寫回原廠韌體", v -> startRescue(true));
        root.addView(rescue);

        stop = button("取消", v -> {
            Installer r = running;
            if (r != null) r.cancel = true;
        });
        stop.setEnabled(false);
        root.addView(stop);
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(100);
        root.addView(bar);
        status = note("");
        status.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(status);

        log = new TextView(this);
        log.setTypeface(Typeface.MONOSPACE);
        log.setTextSize(12);
        log.setTextIsSelectable(true);
        logScroll = new ScrollView(this);
        logScroll.addView(log);
        root.addView(logScroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));

        setContentView(root);
        say("FM-1 Tool " + version());
    }

    String version() {
        try { return getPackageManager().getPackageInfo(getPackageName(), 0).versionName; } catch (Exception e) { return ""; }
    }

    int dp(int v) { return (int) (v * getResources().getDisplayMetrics().density + 0.5f); }

    TextView title(String s) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(20);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setPadding(0, dp(12), 0, dp(4));
        return t;
    }

    TextView note(String s) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(14);
        return t;
    }

    Button button(String s, View.OnClickListener l) {
        Button x = new Button(this);
        x.setText(s);
        x.setAllCaps(false);
        x.setOnClickListener(l);
        return x;
    }

    /* ---------------------------------------------------------------------- log & state --- */
    void say(String s) {
        runOnUiThread(() -> {
            log.append(s + "\n");
            logScroll.post(() -> logScroll.fullScroll(View.FOCUS_DOWN));
        });
    }

    void state(String s) { runOnUiThread(() -> status.setText(s)); }

    void progress(int done, int total) {
        runOnUiThread(() -> {
            bar.setIndeterminate(total <= 0);
            if (total > 0) bar.setProgress((int) ((long) done * 100 / total));
        });
    }

    void setBusy(boolean b) {
        busy = b;
        runOnUiThread(() -> {
            pick.setEnabled(!b);
            install.setEnabled(!b && pkg != null);
            stock.setEnabled(!b);
            check.setEnabled(!b);
            rescue.setEnabled(!b);
            stop.setEnabled(b);
            if (!b) bar.setIndeterminate(false);
        });
    }

    interface Job { void run() throws Exception; }

    void work(Job j) {
        if (busy) return;
        setBusy(true);
        new Thread(() -> {
            try {
                j.run();
            } catch (Fm1Exception e) {
                state("失敗");
                say("✗ " + e.getMessage());
            } catch (Exception e) {
                state("失敗");
                say("✗ " + e);
            } finally {
                running = null;
                setBusy(false);
            }
        }, "fm1-work").start();
    }

    /** a yes/no asked on the UI thread; the caller (a worker) waits for the answer */
    boolean ask(String title, String msg, String yes) {
        CountDownLatch done = new CountDownLatch(1);
        boolean[] ok = {false};
        runOnUiThread(() -> new AlertDialog.Builder(this).setTitle(title).setMessage(msg).setCancelable(false)
            .setPositiveButton(yes, (d, w) -> { ok[0] = true; done.countDown(); })
            .setNegativeButton("不要", (d, w) -> done.countDown()).show());
        try { done.await(); } catch (InterruptedException ignored) { }
        return ok[0];
    }

    /* --------------------------------------------------------------------------- files --- */
    void pickFile() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, PICK);
    }

    @Override protected void onActivityResult(int req, int res, Intent data) {
        if (req != PICK || res != RESULT_OK || data == null || data.getData() == null) return;
        Uri u = data.getData();
        work(() -> {
            String name = displayName(u);
            byte[] raw;
            try (InputStream in = getContentResolver().openInputStream(u)) { raw = readAll(in, 4 << 20); }
            pkg = null;
            Pkg p = new Pkg(raw);
            pkg = p;
            pkgName = name;
            String st = Rescue.stockName(raw);
            String desc = name + "\n→ " + p.product + (st != null ? "（M-VAVE 原廠 " + st + "）" : "") + "，" + raw.length / 1024 + " KB";
            runOnUiThread(() -> fileView.setText(desc));
            say("已選：" + name + " (" + p.product + ")");
            state("");
        });
    }

    String displayName(Uri u) {
        try (Cursor c = getContentResolver().query(u, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (Exception ignored) { }
        return u.getLastPathSegment();
    }

    static byte[] readAll(InputStream in, int max) throws Exception {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        byte[] b = new byte[65536];
        for (int n; (n = in.read(b)) > 0; ) {
            o.write(b, 0, n);
            if (o.size() > max) throw new Fm1Exception("big", "檔案太大，不是 FM-1 韌體");
        }
        return o.toByteArray();
    }

    /** a download kept in the app's cache, checked against its sha256 */
    byte[] fetch(String name, String url, String sha) throws Exception {
        File f = new File(getFilesDir(), name);
        if (f.exists()) {
            byte[] d = Files.readAllBytes(f.toPath());
            if (Pkg.hex(Pkg.sha256(d)).equals(sha)) return d;
        }
        say("下載 " + name + " …");
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(20000);
        c.setReadTimeout(60000);
        c.setInstanceFollowRedirects(true);
        byte[] d;
        try (InputStream in = c.getInputStream()) { d = readAll(in, 4 << 20); } finally { c.disconnect(); }
        if (!Pkg.hex(Pkg.sha256(d)).equals(sha)) throw new Fm1Exception("download", name + " 的內容不對（雜湊值不符）");
        try (OutputStream o = new FileOutputStream(f)) { o.write(d); }
        return d;
    }

    /** a file in Downloads (Android 10+), else the app's own folder -> where */
    String saveDownload(String name, byte[] d) throws Exception {
        if (Build.VERSION.SDK_INT >= 29) {
            ContentValues v = new ContentValues();
            v.put(MediaStore.MediaColumns.DISPLAY_NAME, name);
            v.put(MediaStore.MediaColumns.MIME_TYPE, "application/octet-stream");
            v.put(MediaStore.MediaColumns.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS + "/FM-1");
            Uri u = getContentResolver().insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, v);
            if (u == null) throw new Exception("無法建立檔案");
            try (OutputStream o = getContentResolver().openOutputStream(u)) { o.write(d); }
            return "下載/FM-1/" + name;
        }
        File dir = getExternalFilesDir(null);
        if (dir == null) dir = getFilesDir();
        File f = new File(dir, name);
        try (OutputStream o = new FileOutputStream(f)) { o.write(d); }
        return f.getAbsolutePath();
    }

    /* ------------------------------------------------------------------------- install --- */
    void startInstall() {
        Pkg p = pkg;
        if (p == null) return;
        work(() -> doInstall(p));
    }

    void startStockInstall() {
        work(() -> {
            String[] s = Rescue.STOCK[0];
            Pkg p = new Pkg(fetch("FM-1-" + s[0] + ".fwsc", s[2], s[1]));
            say("原廠韌體 " + s[0] + "：" + p.product);
            doInstall(p);
        });
    }

    void doInstall(Pkg p) throws Exception {
        Installer in = new Installer(usb.midi);
        running = in;
        state("尋找 FM-1…");
        progress(0, 0);
        say("尋找 FM-1（第一次會詢問 USB 權限，請按「允許」）");
        Installer.Device dev = null;
        long end = System.currentTimeMillis() + 60000;
        while (dev == null && System.currentTimeMillis() < end) {
            if (in.cancel) throw new Fm1Exception("stopped", "已取消");
            dev = in.find(null);
            if (dev == null) Thread.sleep(1000);
        }
        if (dev == null)
            throw new Fm1Exception("nodev", "找不到 FM-1。確認 OTG 線、FM-1 已開機；開不了機的 FM-1 請用下面的 UBOOT 救援");
        say("找到：" + dev.id.text + (dev.id.loader() ? "（更新模式）" : ""));
        if (!ask("安裝", "把 " + p.product + " 安裝到 " + dev.id.text + "？\n\n安裝中請不要拔線、不要關螢幕。", "安裝")) {
            dev.link.close();
            say("沒有安裝");
            state("");
            return;
        }
        String got = in.install(dev, p, (kind, arg) -> {
            switch (kind) {
                case "start": state("FM-1 檢查韌體檔…"); say("開始：" + arg); break;
                case "check": progress(0, 0); break;
                case "loader": state("等待更新載入器…"); say("更新載入器啟動中（可能再詢問一次 USB 權限）"); break;
                case "write":
                    int pc = Integer.parseInt(arg);
                    state("寫入 " + pc + "%");
                    progress(pc, 100);
                    if (pc % 10 == 0) say("寫入 " + pc + "%");
                    break;
                case "reboot": state("FM-1 重新開機…"); progress(0, 0); say("寫完，等 FM-1 重新開機（可能再詢問一次 USB 權限）"); break;
                case "done": break;
                default: say(arg);
            }
        });
        progress(100, 100);
        state("完成：" + got);
        say("✓ 完成，FM-1 現在是 " + got);
    }

    /* -------------------------------------------------------------------------- rescue --- */
    void startRescue(boolean write) {
        work(() -> {
            state("準備原廠韌體…");
            progress(0, 0);
            byte[] loader = Rescue.checkLoader(fetch("wl82loader.bin", Rescue.LOADER_URL, Rescue.LOADER_SHA256));
            List<Object[]> pk = new ArrayList<>();
            for (String[] s : Rescue.STOCK) {
                try {
                    pk.add(new Object[] {s[0], Pkg.flashImage(fetch("FM-1-" + s[0] + ".fwsc", s[2], s[1]))});
                } catch (Exception e) {
                    say("（" + s[0] + " 無法取得：" + e.getMessage() + "）");
                }
            }
            if (pk.isEmpty()) throw new Fm1Exception("download", "無法下載原廠韌體（需要網路）");
            state("等待 UBOOT 模式的 FM-1…");
            say("等待 WL82 UBOOT（4C4A:8057）… 請現在讓 FM-1 進入 UBOOT 並接上");
            Usb.Uboot u = usb.waitUboot(120000, this::say);
            try {
                state(write ? "救援中…" : "檢查中…");
                String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
                boolean wrote = Rescue.rescue(new Rescue.Uboot(new Rescue.Bot(u)), pk, loader, false,
                    bk -> saveDownload("fm1-backup-" + stamp + ".bin", bk), this::say,
                    write ? () -> ask("寫回原廠韌體", "備份已存好，開機區是原廠的。\n要把應用程式區寫回原廠韌體嗎？\n\n" +
                                       "請在幾秒內決定；寫入中不要拔線。", "寫入") : null,
                    this::progress);
                progress(100, 100);
                state(wrote ? "完成：已寫回原廠韌體" : "檢查完成（沒有寫入）");
                if (wrote) say("✓ 完成。FM-1 應該會以原廠韌體開機；之後就能用上面的「安裝」裝自訂韌體");
            } finally {
                u.close();
            }
        });
    }
}
