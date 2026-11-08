# SPDX-License-Identifier: MPL-2.0
"""FM-1 救援工具（Windows 視窗版）。

把卡在晶片更新模式（USB 上顯示為 "WL82 UBOOT 1.00"，4C4A:8057）的 M-VAVE FM-1 寫回 M-VAVE 原廠韌體。
救援本身完全由 felucca-bsp 的 fm1_rescue.py 做（同一資料夾的複本，原封不動）：確認晶片與快閃型號、
先把整顆快閃備份成檔案、只寫程式區裡不同的 sector、開機區和裝置資料從不碰、寫完逐段讀回比對。
這個檔案只是它的視窗外殼：下載並核對原廠韌體、找裝置、顯示進度、寫入前問一次。

命令列版在 Windows 上不能直接跑（它用 os.geteuid 檢查 sudo，Windows 沒有這個函式），所以這裡直接呼叫
它的 open_device / rescue。Windows 會把這個裝置當隨身碟接管：要先用 Zadig 把它的驅動換成 WinUSB（一次）。
"""
import hashlib
import os
import queue
import threading
import time
import tkinter as tk
import traceback
import urllib.request
import webbrowser
from tkinter import filedialog, messagebox, scrolledtext

import fm1_rescue as R

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(os.path.expanduser("~"), "fm1-rescue")
MVAVE = "https://yms-file-store.oss-cn-hongkong.aliyuncs.com/software/firmware/FM-1.fwsc"
SOURCES = {"V16": MVAVE, "V15": "https://web.archive.org/web/20261009203224id_/" + MVAVE}
ZADIG = "https://zadig.akeo.ie/"


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


class App:
    def __init__(self, root):
        self.root = root
        self.q = queue.Queue()
        self.busy = False
        self.pkgs = {}                   # name -> path, checked
        root.title("FM-1 救援工具（寫回 M-VAVE 原廠韌體）")
        root.geometry("760x620")
        f = tk.Frame(root, padx=12, pady=10)
        f.pack(fill="both", expand=True)
        tk.Label(f, text="FM-1 救援工具", font=("Microsoft JhengHei UI", 16, "bold")).pack(anchor="w")
        tk.Label(f, justify="left", wraplength=720, text=(
            "給開機就當機、或停在晶片更新模式（電腦顯示「WL82 UBOOT 1.00」、FM-1 螢幕全黑）的 FM-1。"
            "它會先備份整顆快閃，再寫回 M-VAVE 的原廠韌體；之後就能重新安裝任何韌體。")).pack(anchor="w", pady=(2, 8))

        def step(title, text, buttons):
            box = tk.LabelFrame(f, text=title, padx=8, pady=6)
            box.pack(fill="x", pady=4)
            tk.Label(box, text=text, justify="left", wraplength=560).pack(side="left", anchor="w")
            bf = tk.Frame(box)
            bf.pack(side="right")
            out = []
            for label, cmd in buttons:
                b = tk.Button(bf, text=label, command=cmd, width=16)
                b.pack(pady=1)
                out.append(b)
            return out

        self.b_fw = step("1. 原廠韌體", "從 M-VAVE 下載 V16（和 V15 的存檔），用雜湊值核對；\n或選你自己下載的 FM-1.fwsc。",
                         [("下載並核對", self.download), ("選擇檔案…", self.pick)])
        step("2. 驅動（只要做一次）",
             "FM-1 停在更新模式（螢幕黑、電腦顯示 WL82 UBOOT）時，開 Zadig：\n"
             "Options → List All Devices → 選「WL82 UBOOT」(USB ID 4C4A 8057)\n"
             "→ 右邊選 WinUSB → Replace Driver。只影響這個模式，FM-1 平常的 MIDI 不受影響。",
             [("下載 Zadig", lambda: webbrowser.open(ZADIG)), ("檢查裝置", self.probe)])
        self.b_go = step("3. 救援", "先「檢查並備份」（不寫入任何東西）；沒問題再「寫回原廠韌體」，\n"
                                      "寫入前會再問一次。過程中不要拔線。",
                         [("檢查並備份", lambda: self.run(False)), ("寫回原廠韌體", lambda: self.run(True))])
        self.status = tk.Label(f, text="", anchor="w", justify="left", wraplength=720, fg="#a33",
                               font=("Microsoft JhengHei UI", 11, "bold"))
        self.status.pack(fill="x", pady=(6, 2))
        self.log = scrolledtext.ScrolledText(f, height=14, font=("Consolas", 9))
        self.log.pack(fill="both", expand=True)
        os.makedirs(WORK, exist_ok=True)
        self.say(f"工作資料夾（原廠韌體、備份都放這裡）：{WORK}")
        for n in ("V16", "V15"):         # copies from an earlier run
            p = os.path.join(WORK, f"FM-1-{n}.fwsc")
            if os.path.exists(p) and R.STOCK.get(sha256(p)) == n:
                self.pkgs[n] = p
                self.say(f"已有原廠韌體 {n}：{p}")
        root.after(100, self.pump)

    # ---------------------------------------------------------------- log / threads
    def say(self, *a, **_k):
        self.q.put(("log", " ".join(str(x) for x in a)))

    def set_status(self, t):
        self.q.put(("status", t))

    def pump(self):
        try:
            while True:
                k, v = self.q.get_nowait()
                if k == "log":
                    self.log.insert("end", v + "\n")
                    self.log.see("end")
                elif k == "status":
                    self.status.config(text=v)
                elif k == "ask":
                    v[1].append(messagebox.askyesno("最後確認", v[0], icon="warning"))
                    v[2].set()
        except queue.Empty:
            pass
        self.root.after(100, self.pump)

    def ask_main(self, text):            # from the worker: a dialog on the window's thread, its answer back
        ans, ev = [], threading.Event()
        self.q.put(("ask", (text, ans, ev)))
        ev.wait()
        return bool(ans and ans[0])

    def background(self, fn):
        if self.busy:
            messagebox.showinfo("請稍候", "上一個動作還在進行。")
            return
        self.busy = True

        def wrap():
            try:
                fn()
            except R.Fail as e:
                self.say(f"\n停止：{e}")
                self.set_status("停止了（見下方訊息）。沒有完成的寫入，FM-1 會回到更新模式，可以再試。")
            except Exception:
                self.say("\n發生錯誤：\n" + traceback.format_exc())
                self.set_status("發生錯誤：請把下方整段訊息貼出來。")
            finally:
                self.busy = False
        threading.Thread(target=wrap, daemon=True).start()

    # ---------------------------------------------------------------- 1. firmware
    def download(self):
        def go():
            for n, url in SOURCES.items():
                if n in self.pkgs:
                    continue
                dst = os.path.join(WORK, f"FM-1-{n}.fwsc")
                self.say(f"下載原廠韌體 {n} …")
                try:
                    urllib.request.urlretrieve(url, dst + ".part")
                except Exception as e:
                    self.say(f"  {n} 下載失敗（{e}）")
                    continue
                if R.STOCK.get(sha256(dst + ".part")) != n:
                    os.remove(dst + ".part")
                    self.say(f"  {n}：檔案內容不對（雜湊值不符），丟棄")
                    continue
                os.replace(dst + ".part", dst)
                self.pkgs[n] = dst
                self.say(f"  {n}：已核對 ✓")
            self.set_status("原廠韌體就緒：" + "、".join(sorted(self.pkgs)) if self.pkgs else "沒有拿到原廠韌體。")
        self.background(go)

    def pick(self):
        for p in filedialog.askopenfilenames(title="選擇 M-VAVE 的 FM-1.fwsc", filetypes=[("FM-1 firmware", "*.fwsc")]):
            n = R.STOCK.get(sha256(p))
            if n:
                self.pkgs[n] = p
                self.say(f"原廠韌體 {n}：{p} ✓")
            else:
                self.say(f"{p}：不是 M-VAVE 的原廠 FM-1 韌體（只接受 V15 / V16 的原檔），不採用")

    def packages(self):
        if not self.pkgs:
            raise R.Fail("還沒有原廠韌體：先做第 1 步")
        out = []
        for n, p in sorted(self.pkgs.items(), reverse=True):
            image = R.package_flash_image(open(p, "rb").read())
            if len(image) % R.SECTOR or not R.WRITE_MIN < len(image) <= R.APP_MAX:
                raise R.Fail(f"{p}: unexpected flash entry size")
            out.append((n, image))
        return out

    # ---------------------------------------------------------------- 2. device
    def usb(self):
        try:
            import usb.core  # noqa: F401
            import usb.util  # noqa: F401
            import usb
        except ImportError:
            raise R.Fail("少了 pyusb：用「啟動救援工具.bat」開這個工具（它會自動安裝），或執行 pip install pyusb libusb-package")
        return usb

    def probe(self):
        def go():
            usb = self.usb()
            import usb.core
            be = R.backend(usb)
            dev = usb.core.find(idVendor=R.VID, idProduct=R.PID, backend=be)
            if dev is None:
                self.set_status("沒看到 WL82 UBOOT：確認 FM-1 在更新模式（螢幕黑）、線是資料線、Zadig 已換成 WinUSB。")
                self.say("找不到 4C4A:8057。若裝置管理員裡看得到「WL82 UBOOT」卻找不到，通常是還沒用 Zadig 換成 WinUSB。")
                return
            try:
                bot = R.Bot(dev, usb)
                v, p = R.Uboot(bot).inquiry()
                self.say(f"找到：{v} {p}")
                self.set_status("裝置正常，可以進行第 3 步。")
                usb.util.dispose_resources(dev)
            except Exception as e:
                self.say(f"看得到裝置但打不開（{e}）。")
                self.set_status("打不開裝置：用 Zadig 把「WL82 UBOOT」的驅動換成 WinUSB，再按「檢查裝置」。")
        self.background(go)

    # ---------------------------------------------------------------- 3. rescue
    def run(self, write):
        if write and not messagebox.askyesno("寫回原廠韌體",
                                             "會先備份整顆快閃，再把 FM-1 的程式區寫回 M-VAVE 原廠韌體。\n"
                                             "FM-1 上現在的自製韌體會被取代（之後可以重新安裝）。\n\n繼續嗎？"):
            return

        def go():
            usb = self.usb()
            import usb.core
            import usb.util
            packages = self.packages()
            loader = R.get_loader(None)  # (JieLi's flash loader, downloaded and checked against its hash)
            self.set_status("等待 FM-1 的更新模式 …（最多 2 分鐘）")
            for attempt in range(1, 6):
                bot = R.open_device(usb, 120)
                try:
                    # (as the command line's --ask: the check and the backup, then the question, the loader kept
                    # busy meanwhile by fm1_rescue's keep_alive: it resets the chip after ~3 s without a command)
                    confirm = (lambda: self.ask_main("檢查和備份都完成了。\n\n現在寫回原廠韌體嗎？")) if write else None
                    wrote = R.rescue(R.Uboot(bot), packages, loader, False, WORK, say=self.say, confirm=confirm)
                    if wrote:
                        self.set_status("完成！FM-1 會重新開機回到原廠韌體。之後可以重新安裝你要的韌體。")
                    else:
                        self.set_status(f"檢查完成，備份在 {WORK}。沒有寫入任何東西"
                                        + ("（你選了不寫入）。" if write else "；要救援請按「寫回原廠韌體」。"))
                    return
                except (R.Fail, usb.core.USBError) as e:
                    self.say(f"\n第 {attempt} 次：{e}")
                    if isinstance(e, R.Fail) and ("not writing" in str(e) or "not the FM-1" in str(e)):
                        raise
                finally:
                    try:
                        usb.util.dispose_resources(bot.dev)
                    except Exception:
                        pass
            raise R.Fail("試了 5 次都沒成功；請把整段訊息貼出來")
        self.background(go)


if __name__ == "__main__":
    root = tk.Tk()
    App(root)
    root.mainloop()
