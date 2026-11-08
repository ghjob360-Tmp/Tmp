// SPDX-License-Identifier: MPL-2.0
// FM-1 Tool: install a firmware package (.fwsc) over USB; put logue SDK units on a CHAIN-1 FM-1; rescue an FM-1 stuck in
// UBOOT by writing M-VAVE's stock firmware back. Material 3 (res/layout/activity_main.xml); the work runs on a thread.
package fm1.app;

import android.content.ContentValues;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.MediaStore;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import androidx.appcompat.app.AppCompatActivity;

import com.google.android.material.bottomnavigation.BottomNavigationView;
import com.google.android.material.button.MaterialButton;
import com.google.android.material.chip.Chip;
import com.google.android.material.chip.ChipDrawable;
import com.google.android.material.chip.ChipGroup;
import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import com.google.android.material.progressindicator.LinearProgressIndicator;
import com.google.android.material.snackbar.Snackbar;

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
import fm1.core.LogueUnit;
import fm1.core.Pkg;
import fm1.core.Rescue;
import fm1.core.Units;

public class MainActivity extends AppCompatActivity {
    static final int PICK_FW = 1, PICK_UNIT = 2;
    Usb usb;
    TextView fwFile, unitName, unitDetail, status, log;
    LinearProgressIndicator bar;
    MaterialButton fwPick, fwInstall, stockInstall, unitPick, unitSend, bankRead, rescueCheck, rescueGo, cancel, logToggle;
    LinearLayout bankList;
    ChipGroup slotChips;
    View[] pages;
    Pkg pkg;
    LogueUnit unit;
    volatile Installer running;
    volatile boolean busy, stop;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        usb = new Usb(this);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        setContentView(R.layout.activity_main);
        com.google.android.material.appbar.MaterialToolbar tb = findViewById(R.id.toolbar);
        tb.setSubtitle(version());
        fwFile = findViewById(R.id.fw_file);
        unitName = findViewById(R.id.unit_name);
        unitDetail = findViewById(R.id.unit_detail);
        status = findViewById(R.id.status);
        log = findViewById(R.id.log);
        bar = findViewById(R.id.progress);
        fwPick = findViewById(R.id.fw_pick);
        fwInstall = findViewById(R.id.fw_install);
        stockInstall = findViewById(R.id.stock_install);
        unitPick = findViewById(R.id.unit_pick);
        unitSend = findViewById(R.id.unit_send);
        bankRead = findViewById(R.id.bank_read);
        rescueCheck = findViewById(R.id.rescue_check);
        rescueGo = findViewById(R.id.rescue_go);
        cancel = findViewById(R.id.cancel);
        logToggle = findViewById(R.id.log_toggle);
        bankList = findViewById(R.id.bank_list);
        slotChips = findViewById(R.id.slot_chips);
        pages = new View[] {findViewById(R.id.page_install), findViewById(R.id.page_units), findViewById(R.id.page_rescue)};

        for (int i = -1; i < Units.SLOTS; i++) {           /* the slot: automatic, or 1..7 */
            Chip c = new Chip(this);
            c.setChipDrawable(ChipDrawable.createFromAttributes(this, null, 0,
                                                                com.google.android.material.R.style.Widget_Material3_Chip_Filter));
            c.setCheckable(true);
            c.setText(i < 0 ? getString(R.string.slot_auto) : String.valueOf(i + 1));
            c.setTag(i);
            c.setId(View.generateViewId());
            slotChips.addView(c);
            if (i < 0) c.setChecked(true);
        }

        fwPick.setOnClickListener(v -> pick(PICK_FW));
        fwInstall.setOnClickListener(v -> { if (pkg != null) work(() -> doInstall(pkg)); });
        stockInstall.setOnClickListener(v -> work(() -> {
            String[] s = Rescue.STOCK[0];
            Pkg p = new Pkg(fetch("FM-1-" + s[0] + ".fwsc", s[2], s[1]));
            say("原廠韌體 " + s[0] + "：" + p.product);
            doInstall(p);
        }));
        unitPick.setOnClickListener(v -> pick(PICK_UNIT));
        unitSend.setOnClickListener(v -> { if (unit != null) work(this::doSendUnit); });
        bankRead.setOnClickListener(v -> work(this::doReadBank));
        rescueCheck.setOnClickListener(v -> startRescue(false));
        rescueGo.setOnClickListener(v -> startRescue(true));
        cancel.setOnClickListener(v -> {
            stop = true;
            Installer r = running;
            if (r != null) r.cancel = true;
        });
        logToggle.setOnClickListener(v -> log.setVisibility(log.getVisibility() == View.VISIBLE ? View.GONE : View.VISIBLE));
        BottomNavigationView nav = findViewById(R.id.nav);
        nav.setOnItemSelectedListener(item -> {
            int id = item.getItemId(), k = id == R.id.nav_units ? 1 : id == R.id.nav_rescue ? 2 : 0;
            for (int i = 0; i < pages.length; i++) pages[i].setVisibility(i == k ? View.VISIBLE : View.GONE);
            return true;
        });
        say("FM-1 Tool " + version());
    }

    String version() {
        try { return getPackageManager().getPackageInfo(getPackageName(), 0).versionName; } catch (Exception e) { return ""; }
    }

    /* ---------------------------------------------------------------------- log & state --- */
    void say(String s) {
        runOnUiThread(() -> log.append(s + "\n"));
    }

    void state(String s) { runOnUiThread(() -> status.setText(s)); }

    void progress(int done, int total) {
        runOnUiThread(() -> {
            if (total <= 0) {
                if (!bar.isIndeterminate()) {
                    bar.setVisibility(View.INVISIBLE);
                    bar.setIndeterminate(true);
                    bar.setVisibility(View.VISIBLE);
                }
            } else {
                if (bar.isIndeterminate()) {
                    bar.setVisibility(View.INVISIBLE);
                    bar.setIndeterminate(false);
                    bar.setVisibility(View.VISIBLE);
                }
                bar.setProgressCompat((int) ((long) done * 100 / total), true);
            }
        });
    }

    void setBusy(boolean b) {
        busy = b;
        runOnUiThread(() -> {
            for (MaterialButton x : new MaterialButton[] {fwPick, stockInstall, unitPick, bankRead, rescueCheck, rescueGo})
                x.setEnabled(!b);
            fwInstall.setEnabled(!b && pkg != null);
            unitSend.setEnabled(!b && unit != null);
            cancel.setEnabled(b);
            if (!b && bar.isIndeterminate()) {
                bar.setVisibility(View.INVISIBLE);
                bar.setIndeterminate(false);
                bar.setProgressCompat(0, false);
                bar.setVisibility(View.VISIBLE);
            }
        });
    }

    interface Job { void run() throws Exception; }

    void work(Job j) {
        if (busy) return;
        stop = false;
        setBusy(true);
        new Thread(() -> {
            try {
                j.run();
            } catch (Fm1Exception e) {
                state("失敗");
                say("✗ " + e.getMessage());
                snack(e.getMessage());
            } catch (Exception e) {
                state("失敗");
                say("✗ " + e);
                snack(String.valueOf(e.getMessage()));
            } finally {
                running = null;
                setBusy(false);
            }
        }, "fm1-work").start();
    }

    void snack(String s) {
        runOnUiThread(() -> Snackbar.make(findViewById(R.id.nav), s, Snackbar.LENGTH_LONG).setAnchorView(R.id.nav).show());
    }

    /** a yes/no asked on the UI thread; the caller (a worker) waits for the answer */
    boolean ask(String title, String msg, String yes) {
        CountDownLatch done = new CountDownLatch(1);
        boolean[] ok = {false};
        runOnUiThread(() -> new MaterialAlertDialogBuilder(this).setTitle(title).setMessage(msg).setCancelable(false)
            .setPositiveButton(yes, (d, w) -> { ok[0] = true; done.countDown(); })
            .setNegativeButton("不要", (d, w) -> done.countDown()).show());
        try { done.await(); } catch (InterruptedException ignored) { }
        return ok[0];
    }

    /* --------------------------------------------------------------------------- files --- */
    void pick(int what) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, what);
    }

    @Override protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null || data.getData() == null) return;
        Uri u = data.getData();
        work(() -> {
            String name = displayName(u);
            byte[] raw;
            try (InputStream in = getContentResolver().openInputStream(u)) { raw = readAll(in, 4 << 20); }
            if (req == PICK_FW) {
                pkg = null;
                Pkg p = new Pkg(raw);
                pkg = p;
                String st = Rescue.stockName(raw);
                String desc = name + "\n" + p.product + (st != null ? "（M-VAVE 原廠 " + st + "）" : "") + " · " + raw.length / 1024 + " KB";
                runOnUiThread(() -> fwFile.setText(desc));
                say("已選韌體：" + name + " (" + p.product + ")");
            } else {
                unit = null;
                LogueUnit lu = LogueUnit.pack(raw);
                unit = lu;
                runOnUiThread(() -> {
                    unitName.setText(lu.name.isEmpty() ? name : lu.name);
                    String ps = lu.params.isEmpty() ? "" : "\n參數：" + String.join("、", lu.params);
                    unitDetail.setText(lu.describe() + " · " + lu.image.length / 1024.0f + " KB" + ps);
                    unitDetail.setVisibility(View.VISIBLE);
                });
                say("已選單元：" + name + "（" + lu.describe() + "）");
            }
            state(getString(R.string.status_idle));
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
            if (o.size() > max) throw new Fm1Exception("big", "檔案太大");
        }
        return o.toByteArray();
    }

    /** a download kept in the app's files, checked against its sha256 */
    byte[] fetch(String name, String url, String sha) throws Exception {
        File f = new File(getFilesDir(), name);
        if (f.exists()) {
            byte[] d = Files.readAllBytes(f.toPath());
            if (Pkg.hex(Pkg.sha256(d)).equals(sha)) return d;
        }
        say("下載 " + name + " …");
        state("下載中…");
        progress(0, 0);
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

    /** a file in Downloads/FM-1 (Android 10+), else the app's own folder -> where */
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
    Installer.Device findFm1(Installer in, long ms) throws Exception {
        state("尋找 FM-1…");
        progress(0, 0);
        say("尋找 FM-1（第一次會詢問 USB 權限，請按「允許」）");
        long end = System.currentTimeMillis() + ms;
        Installer.Device dev = null;
        while (dev == null && System.currentTimeMillis() < end) {
            if (in.cancel || stop) throw new Fm1Exception("stopped", "已取消");
            dev = in.find(null);
            if (dev == null) Thread.sleep(1000);
        }
        if (dev == null)
            throw new Fm1Exception("nodev", "找不到 FM-1。確認 OTG 線、FM-1 已開機；開不了機的 FM-1 請用「救援」");
        say("找到：" + dev.id.text + (dev.id.loader() ? "（更新模式）" : ""));
        return dev;
    }

    void doInstall(Pkg p) throws Exception {
        Installer in = new Installer(usb.midi);
        running = in;
        Installer.Device dev = findFm1(in, 60000);
        if (!ask("安裝", "把 " + p.product + " 安裝到 " + dev.id.text + "？\n\n安裝中請不要拔線、不要關螢幕。", "安裝")) {
            dev.link.close();
            say("沒有安裝");
            state(getString(R.string.status_idle));
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
                default: break;
            }
        });
        progress(100, 100);
        state("完成：" + got);
        say("✓ 完成，FM-1 現在是 " + got);
        snack("安裝完成：" + got);
    }

    /* --------------------------------------------------------------------------- units --- */
    Installer.Device findChain(Installer in) throws Exception {
        Installer.Device dev = findFm1(in, 20000);
        if (dev.id.loader()) {
            dev.link.close();
            throw new Fm1Exception("mode", "FM-1 在更新模式：先讓它開機到 CHAIN-1");
        }
        if (!dev.id.text.contains("_94")) {
            dev.link.close();
            throw new Fm1Exception("notchain", "FM-1 上是 " + dev.id.text + "，不是 CHAIN-1：logue 單元要裝在 CHAIN-1 1.0 以上");
        }
        return dev;
    }

    int chosenSlot() {
        int id = slotChips.getCheckedChipId();
        View c = id == View.NO_ID ? null : slotChips.findViewById(id);
        return c == null ? -1 : (int) c.getTag();
    }

    void doSendUnit() throws Exception {
        LogueUnit lu = unit;
        Installer in = new Installer(usb.midi);
        running = in;
        Installer.Device dev = findChain(in);
        try {
            state("傳送 " + lu.name + "…");
            say("傳送 " + lu.describe() + "（" + lu.image.length + " bytes）");
            progress(0, lu.image.length);
            int slot = Units.put(dev.link, lu.image, chosenSlot(), this::progress);
            state("完成：位置 " + (slot + 1));
            say("✓ " + lu.name + " 裝進了位置 " + (slot + 1) + "。在 CHAIN-1 的對應區段轉 PRESET（TYPE）就能選到它");
            snack(lu.name + " 已裝進位置 " + (slot + 1));
            showBank(Units.list(dev.link));
        } finally {
            dev.link.close();
        }
    }

    void doReadBank() throws Exception {
        Installer in = new Installer(usb.midi);
        running = in;
        Installer.Device dev = findChain(in);
        try {
            showBank(Units.list(dev.link));
            state(getString(R.string.status_idle));
        } finally {
            dev.link.close();
        }
    }

    void showBank(List<Units.Entry> list) {
        runOnUiThread(() -> {
            bankList.removeAllViews();
            float dp = getResources().getDisplayMetrics().density;
            for (Units.Entry e : list) {
                LinearLayout row = new LinearLayout(this);
                row.setOrientation(LinearLayout.HORIZONTAL);
                row.setGravity(Gravity.CENTER_VERTICAL);
                row.setPadding(0, (int) (6 * dp), 0, (int) (6 * dp));
                TextView n = new TextView(this);
                n.setText(String.valueOf(e.slot + 1));
                n.setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_TitleMedium);
                n.setMinWidth((int) (28 * dp));
                row.addView(n);
                TextView t = new TextView(this);
                t.setText(e.used ? e.name + "\n" + (e.module >= 1 && e.module <= 4 ? LogueUnit.MODULE[e.module] : "?")
                                   + " · " + (e.fmt == 2 ? "NTS-1 mkII" : "NTS-1") : "（空）");
                t.setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_BodyMedium);
                row.addView(t, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
                if (e.used) {
                    MaterialButton del = new MaterialButton(this, null, com.google.android.material.R.attr.materialIconButtonStyle);
                    del.setIconResource(R.drawable.ic_delete);
                    del.setContentDescription("刪除");
                    del.setOnClickListener(v -> new MaterialAlertDialogBuilder(this)
                        .setTitle("刪除單元").setMessage("從 FM-1 的位置 " + (e.slot + 1) + " 刪除 " + e.name + "？\n用到它的音色會改回內建的效果／振盪器。")
                        .setNegativeButton("不要", null)
                        .setPositiveButton("刪除", (d, w) -> work(() -> {
                            Installer in = new Installer(usb.midi);
                            running = in;
                            Installer.Device dev = findChain(in);
                            try {
                                Units.erase(dev.link, e.slot);
                                say("刪除了位置 " + (e.slot + 1) + " 的 " + e.name);
                                showBank(Units.list(dev.link));
                                state(getString(R.string.status_idle));
                            } finally {
                                dev.link.close();
                            }
                        })).show());
                    row.addView(del);
                }
                bankList.addView(row);
            }
        });
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
                if (wrote) say("✓ 完成。FM-1 應該會以原廠韌體開機；之後就能用「安裝」裝自訂韌體");
            } finally {
                u.close();
            }
        });
    }
}
