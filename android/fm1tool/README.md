# FM-1 Tool (Android)

裝韌體到 M-VAVE FM-1，或把卡在 UBOOT 的 FM-1 救回原廠韌體。直接用 Android 的 USB host API 跟 FM-1 的 USB 端點溝通
（不經過 Android 的 MIDI 服務，也不經過 Chrome）。

下載：`dist/fm1tool-1.0.apk`（Android 8 以上、手機要支援 USB OTG）。

## 安裝韌體
1. 用 USB-C OTG 線把開機的 FM-1 接到手機。
2. 「選擇韌體檔 (.fwsc)」→「安裝到 FM-1」。詢問 USB 權限時按「允許」（FM-1 換到更新模式、再重開時，各會是一個新的
   USB 裝置，可能再問一次）。
3. 寫入中不要拔線；螢幕會保持開著。中斷了就拔插再按一次安裝，FM-1 會停在更新模式等著接著寫。

「下載並安裝 M-VAVE 原廠韌體 (V16)」：下載原廠 FM-1.fwsc（檢查雜湊值）後用同樣流程安裝。

## UBOOT 救援
FM-1 開不了機、電腦看到「WL82 UBOOT 1.00」時用：
- 「只檢查並備份」：讀出整顆 1 MiB 快閃存到「下載/FM-1/fm1-backup-*.bin」，檢查開機區是否為原廠 V15/V16，不寫入。
- 「檢查、備份並寫回原廠韌體」：同上，確認後只改寫應用程式區裡跟原廠不同的 sector（由高到低），每個讀回確認，
  最後整顆讀回比對，再讓 FM-1 重開。開機區（前 16 KiB）永遠不寫。

需要網路：第一次會下載 wl82loader.bin（jl-uboot-tool，固定版本與雜湊）和原廠 V16/V15。
流程與 fm1-base 的 `tools/fm1_install.py`、felucca 的 `fm1_rescue.py` 相同。

## 建置
只用 Android SDK 命令列工具（不需 Gradle）：
```
ANDROID_SDK=<sdk 路徑> sh build.sh
```
需要 `platforms;android-34`、`build-tools;34.0.0`、JDK 17+。`fm1tool.keystore`（密碼 fm1tool）是公開的簽章金鑰，
只是讓更新版能覆蓋安裝，不代表來源可信。

核心邏輯（`src/fm1/core`，純 Java）的桌面測試：`test/fm1/core/CoreTest.java`（與 Python 工具逐位元組比對、
跟真正的更新載入器跑完整安裝、模擬 UBOOT 救援）。
