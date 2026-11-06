# M-VAVE FM-1 Custom Firmware 開發調查筆記

> 調查日期：2026-11-06
> 用途：當作之後（人或 AI agent）開發 FM-1 自製韌體的參考底稿。
> 標記說明：**[確認]** = 多個來源一致或來自官方/專案本身文件；**[單一來源]** = 只有一個來源；**[未驗證]** = 來源本身說未在硬體驗證或互相矛盾。
> 本文所有數值都是二手整理，動手寫 flash 之前**務必回原始 repo 再核對一次**（尤其是位址、SHA-256、版本號）。

---

## 0. TL;DR（最重要的 10 件事）

1. FM-1 主晶片是 **JieLi（杰理）AC791N / WL82** SoC，CPU 架構 **pi32v2**（JieLi 自家 ISA，不是 ARM/RISC-V），**雙核**，開機後 boot loader 設 **360 MHz**。[確認]
2. 編譯器：**JieLi pi32v2 clang 4.0.1**（JieLi toolchain 2.5.x），官方 SDK 用 `-mcpu=r3` 目標。Linux x86-64 原生可跑；macOS 用 Docker (`linux/amd64`)；官方 FM1 Open 用 Windows 版。[確認]
3. 參考 SDK：**JieLi AC79 AIoT SDK** `AC79NN_SDK_V1.2.1_2023-12-13`（Apache-2.0，gitee `Jieli-Tech/fw-AC79_AIoT_SDK`）。社群韌體只拿它的 `uboot.boot`、`cfg_tool.bin`、`eq_cfg_hw.bin` 來打包。[確認]
4. 安裝包格式是 **`.fwsc`**（JieLi UFW 容器，內含 app + `ota.bin` update loader）。[確認]
5. 更新通道是 **USB-MIDI SysEx**（不用 HID），所以瀏覽器（Chrome/Edge 的 Web MIDI）就能刷。[確認]
6. Flash 佈局：bootloader `< 0x4000`（不要碰）／ **application `0x4000–0x93000`** ／使用者資料 `> 0x93000`。XIP 映射在 `0x02000000`。**單一 bank，沒有 A/B 回滾**。[確認]
7. 最快上手路線：**fork Felucca 或 Melodee**（GPL-3.0，C，已有 HAL/BSP、打包、安裝、救磚工具、WASM 模擬器、CI），或用 **M-VAVE 官方 FM1 Open SDK**（2026-10 釋出，app 層 GPLv3 + 引擎是 binary blob）。
8. 硬體層可直接用 **`hugelton/felucca-bsp`**（MPL-2.0）：ST7789 LCD、按鍵掃描、Q23 立體聲 audio DMA、USB device/host、OTA、CPU1、flash API。
9. 救磚有兩層：(a) app 自帶的 update loader（開機按住 **OCT−**）；(b) 晶片 ROM **UBOOT** 模式（USB `4c4a:8057`，用 `fm1_recover.py` 或 RP2040 做的 **FM-1-transporter**）。
10. 開發前一定先 **完整備份**（專案、音色、samples；project 檔不含 sample 音訊），寫入時不能斷電/斷 USB/電腦休眠。

---

## 1. 生態系現況（2026-11）

| 類別 | 名稱 | 維護者 | 授權 | 備註 |
|---|---|---|---|---|
| 官方原廠 | Stock V15 / V16 | M-VAVE | 閉源 | V16 於 2026-10-21 上架；多數社群工具「回原廠」是回 **V15** |
| 官方開源 | **FM1 Open V14** | M-VAVE (`github.com/m-vave/fm1_open`) | App: GPLv3 + FM1 SDK linking exception；引擎：binary SDK 另有條款 | 版本線跟 stock 分開，V14 ≠ 比 V16 舊 |
| 社群始祖 | **Felucca** | Leo Kuroshita (@kurogedelic) / Hügelton Instruments (`hugelton/Felucca`) | GPL-3.0-only | 多引擎合成器；stable 1.8.1 / beta 1.8.2（版本資訊各頁面不一致 [未驗證]） |
| 社群 BSP | **felucca-bsp** | hugelton | MPL-2.0 | 晶片層 + FM-1 / Galaxia 板級驅動 |
| 社群引擎 | Felucca-Engines | hugelton | MIT | 聲音引擎 |
| Felucca fork | **Melodee** | keremimo (`keremimo/melodee`) | GPL-3.0-only | 文件最完整（BUILDING.md、BOOT-SAFETY、救磚）|
| Felucca fork | Felucca [Salt] | ChanceTheMaker | GPL | |
| Felucca 衍生 | Hortator | DEADACTIVE（codeberg fork: `armin/hortator`）| GPL-3.0-only | 鼓機；移植 Mutable Instruments Grids/Plaits 等 |
| 其他 | BAUDGIRL FM-1 VA | BAUDGIRL | 閉源? | 需先在官方 V15 |
| 其他 | GLIDER (`ttreu/fm1-glider`)、fm1-x0x、groove-os、sloop-fm1、fm1-fomni、GR!MM、Doom port… | 各自 | 各異 | 清單：`https://fm1-editor.com/firmware/` |
| 逆向 | **FM-1-RE** | AL-255 (`AL-255/FM-1-RE`) | WTFPL | Ghidra 分析、OTA 協定、M-UPGRADE 反編譯 |
| 救磚硬體 | **FM-1-transporter** | kurogedelic | — | RP2040 當 USB 中介，ROM 模式讀寫 flash |
| CI | **zvenson/fm1-build** | zvenson | — | 可重用 GitHub Actions，build + attestation，Hub 顯示「Built from source」徽章 |

> 跨韌體切換：project/sample 格式**不互通**。Melodee 建議從其他 custom FW 切換時先裝 **TransitFM1**（0.1.3）過渡；不少韌體要求先回官方 V15。

---

## 2. 硬體

| 項目 | 內容 | 可信度 |
|---|---|---|
| SoC | JieLi AC791N / WL82（韌體裡的 `JL-BR22` 字串是繼承的 library 名，不是真正型號）| 確認 |
| CPU | pi32v2，雙核，360 MHz（由 bootloader 設定）| 確認 |
| CPU bug | 64-bit 乘法夾在 16-bit load 之間可能讓暫存器值錯誤 → 這類運算放到 out-of-line 函式（felucca-bsp `docs/CPU.md`）| 單一來源 |
| Flash | SPI NOR，XIP 映射 `0x02000000`；新批次有 **UC25HQ80**（JEDEC `0xB36014`），Felucca 1.8.2-beta 才支援 | 確認/單一來源 |
| SRAM 重點 | Cache RAM：7 個 4 KiB data-cache way `0x01F28000..0x01F2F000` 可回收；CPU1 entry mailbox `0x01C7FFF8`；CPU1 私有 4 KiB（3 KiB user stack + 1 KiB supervisor stack）| 單一來源（Melodee）|
| 螢幕 | ST7789 控制器，RGB565 傳輸 | 確認 |
| 輸入 | 16 顆白鍵當 step、12 個 mode 鍵、旋鈕/encoder、OCT−/OCT+；鍵盤可讀 velocity/pressure（預設關）| 確認 |
| 音訊 | 立體聲 Q23 audio DMA；44.1 kHz（Galaxia 板 44,118 Hz）；USB audio 16/24-bit | 確認 |
| 耳機偵測 | PB5 | 單一來源 |
| MIDI | DIN/TRS 輸入、USB-MIDI（device & host）、BLE MIDI | 確認 |
| USB | USB-C；device 模式 = composite（USB-MIDI + UAC audio，可加 serial）；host 模式可接 USB-MIDI（**不供電**，需 powered hub）| 確認 |
| 無線 | BLE 需要 flash 裡的 **RF trim** 資料，缺了 BT 版本會報錯；藍牙 radio program 放在 resource region | 確認 |
| 電源 | 電池供電；VDC14、SYSVDD rail 依 M-VAVE 設定（`fm1_sys.h`）| 單一來源 |
| 同晶片產品 | M-VAVE SMK-37 PRO、Galaxia（PD49 V03）也是 AC79 系列，fm1-build 也支援 SMK-37 | 確認 |

**沒找到的**：完整 GPIO pin map（felucca-bsp 明確說只有實機驗證過才加）、codec 型號、拆機照、JTAG/UART pad 位置。

---

## 3. Flash / 記憶體佈局

```
Flash (offset)
0x000000 ┬───────────────────────────────┐
         │ Bootloader (uboot.boot 等)     │  ← 絕對不要寫
0x004000 ├───────────────────────────────┤
         │ Application (+ update loader) │  ← 自製韌體 & 救磚工具只寫這段
         │ (Melodee: 0x89000 舊 Prophet bank 會被 code 覆蓋) │
0x093000 ├───────────────────────────────┤
         │ Saved data: settings/presets/ │  ← 使用者資料；factory bank、cold asset、
         │ projects/samples/RF trim...   │     字型、BT radio program 等 resource
         └───────────────────────────────┘
XIP 映射基址：0x02000000
```

- Melodee 把 5 個開機搬到 SRAM 的 section 用 LZMA 壓縮：`.ram_text`、`.dsp_text`、`.dsp_tables`、`.data`、`.audio_text`，在 SRAM code / CPU1 執行前展開。
- 音訊工作記憶體：一般 build 至少要 139 KiB；resident profile 64 KiB。voice 預算 16（CPU1 上線的 resident profile 18）。
- 官方 FM1 Open：8 個 sample slot 共用約 **19.55 秒**新 sample 空間；DX7 library 128 音色（A–D 各 32）。

---

## 4. 開發路線選擇

### 路線 A：fork Felucca / Melodee（推薦，自由度最高）
- 全部是 C，有 HAL、UI、打包、安裝器、救磚、測試、WASM 模擬器、CI 範本。
- GPL-3.0-only → 衍生作品必須開源同授權。
- 適合：想做全新的合成器/鼓機/sequencer。

### 路線 B：只用 felucca-bsp 從零寫
- `chip/`（暫存器、clock、IRQ、flash、USB、UART、timer、boot entry、`ota.c`、`libc.c`）+ `boards/fm1/`。
- `examples/hwtest/` 有安全的硬體測試映像，可當 hello world 起點。
- 編譯：`-Ichip -Iboards/fm1`；loader 可從同一棵樹建出，跟 Felucca 內建的 loader 相同（marker `FELUCCA-LOADER-1` 是 loader 合約的一部分，別亂改）。
- MPL-2.0 → 你的 app 可以用別的授權（只改到 BSP 檔案要公開那些檔）。

### 路線 C：官方 FM1 Open SDK
- 好處：用原廠 DX7 引擎、跟官方 Workbench 整合（`https://open.m-vave.com/fm1-workbench`）、有瀏覽器預覽。
- 壞處：引擎是 binary SDK；建置只支援 **Windows x64 + PowerShell**；app 是 GPLv3（含 SDK linking exception）。
- 適合：只想改 sequencer/UI/參數，不想碰 DSP 與底層。

### 路線 D：純逆向改 stock 韌體
- 只有 AL-255/FM-1-RE 在做，`main` branch **沒有**可用的映像建構器（實驗碼在 `with-custom-firmware` branch），作者明說更新協定不是已證實的救援機制。**不建議**當開發路線，但它的文件是理解協定與容器格式的最佳資料。

---

## 5. 開發環境建置

### 5.1 Linux x86-64（社群韌體，最順）

```sh
# 1) JieLi toolchain（clang 4.0.1 for pi32v2，從 pkgman.jieliapp.com 下載）
git clone https://github.com/keremimo/melodee.git && cd melodee
tools/get_toolchain.sh            # 裝到 ~/.jieli/toolchain

# 2) JieLi AC79 SDK（只用來拿打包用的 boot 檔）
git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
    https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK

# 3) Python 依賴（字型/圖示在 build 時 rasterize，lzma 壓 SRAM section）
pip3 install -r tools/requirements-build.txt

# 4) 可覆寫的環境變數
export JIELI_TOOLCHAIN=~/.jieli/toolchain
export AC79_SDK=~/fw-AC79_AIoT_SDK

# 5) Build
./build.sh                       # → build/melodee.bin, build/loader/ota.bin, build/melodee.fwsc
./build.sh --release 0.11.1      # → identity FM-1_90111, build/melodee-0.11.1.fwsc
./build.sh --nightly             # → identity FM-1_900
```

打包需要的 SDK 檔：`cpu/wl82/tools/uboot.boot`、`cfg_tool.bin`、`cfg/eq_cfg_hw.bin`。

### 5.2 macOS
- Docker Desktop，每個 JieLi 工具在 `linux/amd64` 的 `debian:bookworm-slim` container 跑（Apple Silicon 走 Rosetta）。原始碼要放 Docker 可分享的路徑（`/Users/...`）。
- `brew install fribidi`（Pillow Raqm 用）。

### 5.3 Windows（官方 FM1 Open）

```powershell
# 需要 PowerShell、curl.exe、tar.exe；JieLi Windows toolchain 2.5.3（預設 C:\JL\pi32）
$env:FM1_PI32_TOOLCHAIN = 'C:\JL\pi32'
./sdk/Build.ps1 -FirmwareOnly -Toolchain 'C:\JL\pi32'   # 只出韌體
.\tools\Build-Web.ps1 -LocalPreview                     # 本地網頁預覽（第一次會抓 WASI SDK 27，可設 FM1_WASI_SDK）
.\build.bat --package                                   # 發佈包，需設 FM1_SOURCE_URL（HTTPS 原始碼壓縮檔，不能是 GitHub tree 頁）
```
- Build 會用 `sdk/manifest.json` 驗 22 個 toolchain 檔的 SHA-256；`pi32/bin/clang.exe` 應為
  `d4bc97cf421c1ccc373dd8924ce88720ff7457b3f1478d15df4aeda6ae923bce`。舊的 V14 kit 綁 2.5.2，會拒絕 2.5.3。
- 產物：`build/dx7/generated/FM1_OFFICIAL_OPEN.fwsc`、`build/preview/index.html`、`build/official-web/FM1-Official-Open-Source.html`。

### 5.4 Melodee build 選項（環境變數，參考用）

| 變數 | 效果 |
|---|---|
| `MELODEE_DUAL_CORE=1` | 目標 build **必須**為 1 |
| `MELODEE_CORE1_FALLBACK=0` | CPU1 失敗時改成 watchdog reset（診斷用）|
| `MELODEE_BLE=0` | 不含 BLE MIDI |
| `MELODEE_CACHE_RAM=0` | 不回收 cache RAM |
| `MELODEE_COMPRESS_AUDIO=0` | 不 LZMA 壓 SRAM section |
| `MELODEE_USB_HOST=0` | 不含 USB MIDI host |
| `MELODEE_HW_EQ=1` | 啟用 AC79 硬體 EQ |
| `MELODEE_RAM_AUDIO=1` | nightly / 手冊網站用 |

---

## 6. 專案結構（以 Felucca 系為範本）

```
firmware/
  src/        # 應用：UI、sequencer、引擎排程
  hal/        # 硬體抽象（或改用 felucca-bsp 的 chip/ + boards/fm1/）
  loader/     # update loader（RAM image，打進 .fwsc 當 ota.bin）
tools/        # get_toolchain.sh、package maker (fm1pkg.py)、fm1_install.py、fm1_recover.py、publish
web/          # Web installer / editor（Web MIDI）、web/sim 模擬器（firmware 編成 WASM）
assets/       # 字型（如 Rubik）、icon atlas、samples（注意授權，Hortator 用 CC0）
tests/        # run_tests.sh：含 sound golden hash、UI screenshot、cost budget
docs/         # BOOT-SAFETY、RECOVERY、BLE_MIDI、FLASH...
.github/workflows/release.yml   # 呼叫 zvenson/fm1-build@v1
build.sh
```

felucca-bsp 主要 header：

| 檔案 | 功能 |
|---|---|
| `boards/fm1/fm1_lcd.h` | ST7789 init + RGB565 傳輸 |
| `boards/fm1/fm1_input.h` | 按鍵掃描（velocity/pressure 預設關）|
| `boards/fm1/fm1_audio.h` | 立體聲 Q23 audio DMA、buffer 檢查、stop/restart、underrun |
| `boards/fm1/fm1_jack.h` | 耳機偵測（PB5）|
| `chip/fm1_sys.h` | reboot、core reset、電源 rail |
| `chip/fm1_cpu1.*` | 第二核（beta）|
| `chip/fm1_flash_ext.h`、`chip/fm1_cache_ram.h` | 擴充 flash、cache RAM（beta）|
| `chip/fm1_perf.h` | cycle / stall counter |
| `chip/fm1_guard.h` | stack/write/fetch guard → fault report |
| `chip/fm1_debug.h` | 暫存器存取 |
| `chip/usb.c`、`chip/usb_host.c` | USB device（MIDI/serial/audio-in，`FELUCCA_UAC_OUT` 開 audio-out）、USB host |
| `chip/ota.c` | 更新 session，由 app 提供 hook |
| `docs/BOARD_IO.md`、`FLASH.md`、`CPU.md`、`USB_HOST.md` | 一定要先讀 |

設計重點（從各專案歸納）：
- 音訊 callback 有時間預算；Hortator 做法：單一 audio block 超過 95% 時間預算就砍一個 voice（低優先先砍）。
- DSP 熱路徑放 SRAM（`.dsp_text`/`.audio_text`），不要從 XIP flash 跑。
- 雙核：CPU1 透過 mailbox 啟動，要有單核 fallback。
- 寫 flash 時要保存中斷狀態、與 CPU1 協調（見 `FLASH.md`）。
- 聲音引擎可移植來源：Dexed/msfa（stock 的 FM 引擎就是由此衍生）、Mutable Instruments（Plaits/Grids/Streams）、DaisySP。注意授權相容性。

---

## 7. `.fwsc` 封裝格式（JieLi UFW）

來源：FM-1-RE `docs/io/11-ota-protocol.md`（以 stock V13/V14 為樣本）。

- 前 **960 (0x3C0)** bytes 是交錯的 UFW header：20 個 0x30-byte block，每塊 0x2F 資料 + 1 marker byte → 去 marker 後 940 bytes logical header。logical image = stripped header + `file[960:]`。
- Header 以 HDRKEY `0xFFFF` 解密，內含 hdrcrc、listcrc、imgsize、numents，以及 8 個 entry：
  `flash.bin`、`info.log`、`USR`、`isd_config.ini`、`ota.bin`、`script.ver`、`blimit.bin`、`tail.bin`。
- `script.ver` 用 SFC 加密（chipkey `0x980F`），解出 `AC791N-v0.01-cfg_tool-v0.10`。
- `ota.bin`（update loader）：外層 header `[hdrcrc:2][datacrc:2][doff][dlen][attr][rsvd][last]` + 名稱 `usb_hid_ota.bin`；內層 boot header 含 `imgsize`、`loadaddr=0x1C0A800`；payload 是共用字典的多段 **LZ4 block**。
- 所有 CRC = **jl_crc16 = CRC-16/CCITT-FALSE**。
- 工具：`kagaimiq/jl-misctools`（解 JieLi 容器、key 檔、UI 資源）、FM-1-RE `scripts/extract_ota_loader.py`、felucca-bsp `tools/fm1pkg.py`（**實務上直接用這個打包**，別自己手刻）。

---

## 8. 更新協定（USB-MIDI SysEx）

| 模式 | VID:PID | 列舉名稱 |
|---|---|---|
| 正常 | `4c4a:c755` | "FM-1 Midi"（USB-MIDI + UAC）|
| Stock OTA loader | `4d4a:4155` | "ota-FM-1" |
| Melodee loader | — | "Melodee Update" |
| ROM UBOOT | `4c4a:8057` | inquiry `WL82 / UBOOT1.00` |

- 訊息 `F0 … F7`，內容是 **LSB-first 連續 8→7 bit 編碼**。
- Handshake：`F0 00 32 45 00 00 00 40 7F F7` → 回 34-byte ID block（header `00 32 45 58`），型號 = `_` 前文字，例如 `FM-1_009`、`FM-1_90111`；後綴十進位為版本號。
- Upgrade 指令：`F0 22 24 35 7F F7`。
- **Device 主動拉資料**：送 read request，host 回應。
  ```
  00 59 30 | body_length:u24le | flash_type:u8 | address:u32le | requested_length:u24le | data | checksum:u8
  checksum = ~sum(flash_type..data) & 0xFF
  必須滿足 body_length + 7 == 解碼後封包長度（body_length = len(data)+8）
  ```
- 流程：Step1 驗證 → 寫 boot record（JieLi `UPDATA_PARM`）→ soft reset 進 loader → Step2 再 handshake → 讀 header、entry list（由高往低）、bulk data（由低往高）。
- 終止訊號：讀 `0xE0000000` len 8 = 驗證完成；讀 `0xF0000000` len 8 = loader 完成。host 皆回 `success\0`。
- 時序：每個 upgrade 指令後等 2000 ms；每次接收 timeout 8000 ms；無 keepalive；`0xE0000000` 回覆後保持連線 3000 ms 再等重新列舉。
- 同版本/no-op 更新會被 verifier 拒絕（部分 gate 尚未完全解開，見 FM-1-RE `TODO_aug2.md`）。
- 實作參考：`tools/fm1_install.py`（Felucca/Melodee/felucca-bsp，需 `pip3 install mido python-rtmidi`）、FM-1-RE `tools/fm1_ota.py`（Linux，硬體驗證中）。
- Normal-mode 另有 `syscmd`（`[00 59][cmd][len:3 LE][payload][~sum]`，transport 0=USB-MIDI，1=BT），跟 OTA 分開。

> 注意：自己寫 installer 的話，最好直接重用現成的 `fm1_install.py`，而不是重新實作協定。

---

## 9. 刷機、安裝、測試

```sh
# 查目前裝置資訊
python3 tools/fm1_install.py --info
# 安裝
python3 tools/fm1_install.py build/melodee.fwsc
# 裝置卡在 update loader（顯示為 "Melodee Update"）
python3 tools/fm1_install.py /path/to/working-firmware.fwsc --yes
```

- Web installer（Chrome/Edge，需 Web MIDI；Safari 不行）：Felucca `hugelton.github.io/Felucca/`、Melodee `keremimo.github.io/melodee/webapp/installer/`。
- 本地測試 web installer：Web MIDI 只在 localhost/HTTPS 下可用 → `python3 -m http.server 8000`。
- 官方工具：Windows **M-UPGRADE**、官方 **FM1 Workbench**（可回 stock V15、從相容的第三方韌體遷移、有 force-flash）、Firmware Hub（有「Back to official」= V15）。
- 先在模擬器跑：Hortator/Felucca 的 web sim 把真正韌體編成 WASM；另有非官方 fm1-emu（MIDI 輸入 Chrome/Edge/Firefox）。
- 測試：`npm ci --prefix web && tests/run_tests.sh`（`SOUND=0` 跳過聲音測試、`GOLDEN_UPDATE=1` 更新 golden hash）。

---

## 10. 救磚（一定要先讀）

風險本質：**單一 bank**、寫入時斷電 = app 區毀損；stock 韌體甚至可能在開機後覆寫 custom FW 的資料。

| 層級 | 進入方式 | 工具 | 適用 |
|---|---|---|---|
| 1. App 內回原廠 | 正常開機 | Web installer「Return to official V15」、Workbench、Hub | 韌體正常 |
| 2. Update loader | 開機時**單獨按住 OCT−**；key light 閃 3 下 | `fm1_install.py <fw>.fwsc --yes`（已驗證 sector 會跳過）| Melodee ≥1.3.5 的 boot protection；寫入中斷後也會自動進 |
| 3. ROM UBOOT | 開機時 1 次長閃；USB `4c4a:8057` | `fm1_recover.py`（pyusb + libusb；Windows 用 Zadig 只對 `4c4a:8057` 裝 WinUSB）、felucca-bsp `fm1_rescue`(macOS)、Melodee release 附的各平台 unbricker | 黑畫面、app 毀損 |
| 4. 外部中介 | RP2040 接 FM-1 的 USB D+/D-/GND（**不接 VBUS**）| FM-1-transporter `fm1t.py`（需 `wl82loader.bin`，來自 jl-uboot-tool）| 主機 USB 進不了 UBOOT 時 |

`fm1_recover.py` 用法：
```sh
python3 tools/fm1_recover.py --info                    # 只辨識 ROM UBOOT
python3 tools/fm1_recover.py working.fwsc --check      # 離線驗證包與 RAM loader
python3 tools/fm1_recover.py working.fwsc              # 載 RAM helper + 完整備份，不寫入
python3 tools/fm1_recover.py working.fwsc --write      # 再備份 → 只寫變動的 4 KiB sector (0x4000–0x93000) → 全片驗證
```
- RAM helper `wl82loader.bin`：30,720 bytes，SHA-256 `d9de9f1c81a3bb52998e5bced678f0ca851223014a7d43965b03f0719a44eb55`。
- 成功會印 `RESTORE VERIFIED`；保留 `full-flash-before.bin`、`backup.json`、`retained-ram.bin`。
- 官方 V15 救援包須與原始 `FM-1.fwsc` SHA-256 相符。
- 已知壞包（勿重刷）：Melodee 1.3.x SHA-256 `e0cfb18c63ff1fdfd4c0ed9af49bb02ef191ec84413d3dfca3424482eb1b9d24`。

FM-1-transporter：
```sh
# 韌體（RP2040）
git submodule update --init
cmake -S . -B build -DPICO_SDK_PATH=$HOME/pico-sdk -DPICO_BOARD=seeed_xiao_rp2040
make -C build -j8 fm1_transporter
# 接線 XIAO RP2040: D6(GP0)=D+, D7(GP1)=D-, GND=GND
python3 tools/fm1t.py --loader wl82loader.bin info
python3 tools/fm1t.py --loader wl82loader.bin dump backup.bin
python3 tools/fm1t.py --loader wl82loader.bin write --package FM-1_vNN.fwsc --ref backup.bin --write
# 完全沒韌體時：fm1t.py rekey 再開機
```

**開發守則**
1. 新硬體先 `dump` 整片 flash 留底（含 `> 0x93000` 的 RF trim / factory data，這是每台獨有的）。
2. 只寫 `0x4000–0x93000`。
3. 先跑模擬器 → `examples/hwtest` → 自己的 build。
4. 保留 boot protection（OCT− 進 loader）的機制，不要在 app 裡拿掉。
5. 寫入時：充飽電、關閉其他 MIDI app、電腦不休眠。

---

## 11. 發佈

- 用 `zvenson/fm1-build` reusable workflow：

```yaml
# .github/workflows/release.yml
name: release
on:
  push:
    tags: ['v*']
jobs:
  build:
    uses: zvenson/fm1-build/.github/workflows/build.yml@v1
    permissions:
      contents: write
      id-token: write
      attestations: write
    with:
      build: ./build.sh --release "${GITHUB_REF_NAME#v}"
      packages: build/*.fwsc
      # test: sh tests/run_tests.sh
```
- 驗證：`gh attestation verify x.fwsc --repo <owner>/<repo> --signer-workflow zvenson/fm1-build/.github/workflows/build.yml`
- Attestation 只證明 bytes 來自該原始碼與 builder，**不代表硬體安全**。
- 授權：Felucca 系 GPL-3.0-only → 衍生需同授權；官方 FM1 Open 發佈要保留 SDK terms 與 `THIRD_PARTY.md`。JieLi SDK 是 Apache-2.0 但不要 vendor 進 repo（用腳本抓並驗 hash）。
- Identity 字串（`FM-1_xxxxx`）要和官方/其他韌體區隔，安裝器靠它判斷版本。

---

## 12. 建議的第一個開發計畫（給接手的 agent）

1. 在 Linux x86-64 container：裝 toolchain + AC79 SDK，build Melodee 或 Felucca 原樣，確認 `.fwsc` 產出、tests 通過。
2. 跑 web simulator，確認改動能在 WASM 裡看見。
3. 在 felucca-bsp 的 `examples/hwtest` 上做最小改動（例：LCD 顯示字串 + 按鍵發 MIDI note），用 `fm1pkg.py` 打包。
4. 準備好 UBOOT 救援工具（`fm1_recover.py --info` 確認能辨識）**之後**才刷第一次實機。
5. 再往上加：音訊 callback（Q23 DMA）、簡單振盪器 → voice 管理 → sequencer → UI。
6. 加 CI（fm1-build）與 golden-sound 測試。

---

## 13. 尚未解決 / 需要再查

- 完整 GPIO pin map、codec 型號、debug UART/JTAG pad（沒有公開資料）。
- Felucca 最新版本號各頁不一致（1.0.4 / 1.6 / 1.8.1）。
- 官方 FM1 Open 的 app API（audio callback、UI、key）細節沒在 README 摘要中，需要直接讀 `app/` 與 `app/instrument.h`。
- Stock OTA verifier 的 no-op/notify gate 尚未完全逆向。
- 新批次 flash（UC25HQ80）的相容性還在驗證（Felucca issue #320）。

---

## 14. 參考來源

- 官方
  - Synthtopia：M-VAVE Officially Opens FM-1 Firmware — https://synthtopia.com/content/2026/10/20/m-vave-officially-opens-fm-1-firmware-to-developers-with-new-workbench-sdk
  - FM1 Open repo — https://github.com/m-vave/fm1_open （release V14：/releases/tag/V14）
  - FM1 Workbench — https://open.m-vave.com/fm1-workbench
  - 論壇 — https://forum.m-vave.com/topic/4
- 社群韌體 / BSP
  - Felucca — https://github.com/hugelton/Felucca
  - felucca-bsp — https://github.com/hugelton/felucca-bsp
  - Melodee（含 BUILDING.md）— https://github.com/keremimo/melodee 、https://github.com/keremimo/melodee/wiki
  - Hortator — https://codeberg.org/armin/hortator
  - GLIDER — https://github.com/ttreu/fm1-glider
  - Felucca [Salt] — https://github.com/ChanceTheMaker/Felucca
  - 韌體清單 — https://fm1-editor.com/firmware/
  - BAUDGIRL VA（Sonic State）— https://sonicstate.com/news/2026/09/29/free-custom-firmware-for-mvave-fm-1-/
- 逆向 / 工具
  - FM-1-RE — https://github.com/AL-255/FM-1-RE （協定：`docs/io/11-ota-protocol.md`）
  - FM-1-transporter — https://github.com/kurogedelic/FM-1-transporter
  - zvenson/fm1-build — https://github.com/zvenson/fm1-build
  - JieLi AC79 SDK — https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK
  - kagaimiq jl-misctools / jl-uboot-tool（GitHub）
- 報導
  - zeli.app 逆向摘要 — https://zeli.app/story/49994065
  - Neoteo：stock V16 與 Open V14 為何分開 — https://www.neoteo.com/en/m-vave-fm-1-firmware-why-stock-v16-and-open-v14-are-separate
  - Synth Anatomy 產品介紹 — https://synthanatomy.com/2026/07/m-vave-fm-1-a-budget-friendly-dx-7-style-desktop-fm-polysynth.html
