# FM-1 救援工具（Windows 視窗版）

給開機就當機、或停在晶片更新模式的 M-VAVE FM-1：螢幕全黑、電腦顯示「WL82 UBOOT 1.00」（USB ID `4C4A:8057`）。它會先把整顆快閃備份成檔案，再把 FM-1 的程式區寫回 **M-VAVE 原廠韌體**；之後就能用一般的安裝方式重新裝任何韌體。

## 用法
1. 安裝 [Python 3](https://www.python.org/downloads/)，安裝畫面第一頁勾選 **Add python.exe to PATH**。
2. 解壓縮這個資料夾，雙擊 **啟動救援工具.bat**。第一次會自動安裝 pyusb 和 libusb，接著開出視窗。
3. 視窗裡依序做：
   1. **原廠韌體**：按「下載並核對」。會從 M-VAVE 下載 V16，並從 Internet Archive 下載 V15，用雜湊值核對，只接受原檔。也可以選你自己下載的 FM-1.fwsc。
   2. **驅動（只要做一次）**：FM-1 在更新模式時，開 [Zadig](https://zadig.akeo.ie/)：
      - Options → List All Devices
      - 選「WL82 UBOOT」（USB ID 4C4A 8057）
      - 驅動選 **WinUSB** → Replace Driver
      - 回到工具按「檢查裝置」，確認找得到。
      - 這只改這個模式的驅動，FM-1 平常當 MIDI 裝置時是另一個 USB ID，不受影響。
   3. **救援**：先「檢查並備份」，不會寫入；沒問題再按「寫回原廠韌體」，寫入前會再問一次。過程中不要拔線。
4. 原廠韌體、快閃備份都放在 `%USERPROFILE%\fm1-rescue`。

## 它是什麼
- `fm1_rescue.py`：felucca-bsp 的救援程式（commit `78cf51b`，MPL-2.0），**原封不動**的複本。救援本身都由它做：
  - 確認晶片金鑰 980F 和快閃型號（P25Q80 或 UC25HQ80）
  - 先備份整顆快閃
  - 依 FM-1 現有的開機區挑對應的原廠版本
  - 只寫程式區裡不同的 sector，開機區和裝置資料從不寫入
  - 寫完逐段讀回比對
- `fm1_rescue_gui.pyw`：它的視窗外殼。命令列版在 Windows 不能直接執行，因為它用 `os.geteuid` 檢查 sudo，Windows 沒有這個函式。所以這裡直接呼叫它的 `open_device` 和 `rescue`，流程跟命令列的 `--ask` 相同：先檢查、備份，再問要不要寫入；問的時候，救援程式會讓載入程式保持連線。

**狀態**：救援程式本身在 macOS 上驗證過。這個 Windows 視窗版只在 Linux 上用假的裝置測過流程（檢查、確認後寫入、確認時取消），還沒在 Windows 和真的 FM-1 上跑過。出錯時請把視窗下方整段訊息貼出來。與 M-VAVE 無關；使用風險自負。
