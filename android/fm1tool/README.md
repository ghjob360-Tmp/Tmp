# FM-1 Tool (Android) 2.0

M-VAVE FM-1 的手機工具，Material 3 介面（Android 12 以上會跟著桌布換顏色）。它直接用 Android 的 USB host API 跟 FM-1 的 USB 端點溝通，不經過 Android 的 MIDI 服務，也不經過 Chrome。

下載：`dist/fm1tool-2.0.apk`。需要 Android 8 以上，手機要支援 USB OTG。用的是同一把簽章金鑰，可以直接覆蓋安裝 1.0。

## 三個分頁

**安裝**
- 選一個 `.fwsc`，裝到 FM-1。流程和 fm1-base 的 `tools/fm1_install.py` 一樣。
- 「下載並安裝 V16」：下載 M-VAVE 原廠 FM-1.fwsc，核對雜湊值後安裝。

**單元**（CHAIN-1 1.0 以上）
- 選一個 NTS-1 的 logue SDK 單元：`.ntkdigunit`（NTS-1 mk1）或 `.nts1mkiiunit`（NTS-1 mkII），可以是振盪器、MOD、DELAY 或 REVERB。
- 選一個位置（自動，或 1–7），按「傳送到 FM-1」。
- 「讀取」會列出 FM-1 上的 7 個位置，每個都可以刪除。
- 打包方式和 `chain1/tools/logue/logue_pack.py` 一模一樣（測試逐位元組比對）。傳送協定和 `chain1/tools/fm1_units.py` 一樣。

**救援**
- FM-1 開不了機、電腦看到 WL82 UBOOT 時用：
  - 先把整顆快閃備份到「下載/FM-1」。
  - 確認開機區是原廠 V15 或 V16。
  - 只改寫應用程式區，最後整顆讀回比對。

## 建置

用 Gradle 8.7 以上（Android Gradle Plugin 8.7、Material Components 1.12），還需要 Android SDK 的 platform 34：

```
echo "sdk.dir=<Android SDK>" > local.properties
gradle assembleRelease          # build/outputs/apk/release/fm1tool-release.apk
```

`fm1tool.keystore`（密碼 fm1tool）是公開的簽章金鑰。它只是讓新版能覆蓋安裝，不代表來源可信。

## 測試

核心邏輯（`src/fm1/core`，純 Java）的桌面測試在 `test/fm1/core/CoreTest.java`：
- 和 Python 工具逐位元組比對。
- 用真正的更新載入器跑完整安裝。
- 模擬 UBOOT 救援。
- 單元打包和 logue_pack.py 一致。
- 單元的 LIST、PUT、ERASE 協定對著模擬的 CHAIN-1 跑。
