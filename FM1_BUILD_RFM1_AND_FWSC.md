# 同一份程式碼編出兩種產物：RemoteFM1 用的 `.rfm1`（WebAssembly）＋ FM-1 實機可燒的 `.fwsc`

> 調查日期：2026-11-06。這份是 `MVAVE_FM1_CUSTOM_FIRMWARE.md` 的續篇，專講「怎麼編」。
> 來源是以下 repo 的文件與原始碼（2026-11-06 的 HEAD）。
> **2026-11-06 更新：§2–§4 的 fm1-base 流程已實測成功**（Linux x86-64，JieLi toolchain 20260730.1、AC79 SDK `d179b44`、emsdk 6.0.10）。實際範例和踩到的坑在 `fm1-hello/README.md`。補充幾點：
> - SDK 只需要 3 個打包檔，可以用 `--filter=blob:none --sparse` 只抓 `cpu/wl82/tools/{uboot.boot,cfg_tool.bin,cfg/eq_cfg_hw.bin}`（gitee 的完整 clone 很容易斷線）。
> - `words.tsv` 必須有一行 `# url: https://.../words/`，否則 build 會失敗。
> - fm1-base 的畫布上限是 240×144，要畫全螢幕得分塊 `cv_begin`/`cv_blit`。
> - `screenshot` 路徑是相對於 `app/` 的上一層；`firmware.json` 會由 build 改寫，要先 commit，`.rfm1` 的 source_url 才會準確。
> §5（FM1VST 路線）仍然只照文件整理，沒有實測。
> - `charlesvestal/fm1-base`（README、template/、tools/build.py）
> - `Quixotic7/RemoteFM1`（README、docs/NATIVE-AUTHORS.md）
> - `Quixotic7/RemoteFM1IOS` docs/RFM1-FORMAT.md
> - `Quixotic7/FM1VST`（branch `rfm1-target`：docs/RFM1.md、core-glue/common/README.md）
> - `Quixotic7/RemoteFM1FW`（docs/REMOTEFM1-METADATA.md）
> - `imijes/bass-something-2`（真實的 fm1-base 韌體範例，同時出 `.fwsc` 與 `.rfm1`）

---

## 0. 先搞懂名詞

| 名詞 | 是什麼 |
|---|---|
| **RemoteFM1** | Quixotic7 做的「給韌體用的韌體」。燒進 FM-1 後，FM-1 只負責把按鍵/旋鈕/MASTER 送給電腦，並顯示電腦傳回的 LED 與 240×240 畫面；真正的韌體跑在電腦、手機或瀏覽器上。網站 `https://remotefm1.com/`（FM-1 Player），也有 iOS（TestFlight + AUv3）、Android APK、Mac VST3/AU（FM1VST）。|
| **`.rfm1`** | 「韌體的 host build 編成 WebAssembly」的單一檔案：wasm module + `rfm1` 自訂 section（JSON manifest）+ 授權文字。RemoteFM1 的網站和 App 都吃這個。**不會改到 FM-1 本體**。|
| **`.fwsc`** | FM-1 原生安裝包（JieLi UFW 容器：app + update loader），用 USB-MIDI 燒進實機。|
| **fm1core / ABI 6** | FM1VST 定義的「韌體當成可載入模組」介面（`core-api/fm1core.h`）。`.rfm1` 就是 fm1core 編成 wasm 後，把函式包成平的 `rfm1_*` export。|
| **`.fm1web`** | remotefm1.com 的「native build」格式（作者用 RemoteFM1 的 reusable workflow 產生），跟 `.rfm1` 是不同檔案；見 §6。|
| **emulated** | remotefm1.com 也能用 fm1-emu（模擬 AC791N 晶片）直接跑任何 `.fwsc`，但比 native/rfm1 吃 CPU（約 20 倍）。原廠 M-VAVE 韌體不能在 host 跑。|

**結論：要「同時」產出兩種，最省事的路線是把韌體寫在 fm1-base 上**，因為 fm1-base 的 `tools/build.py` 一個參數就切換目標：

```
tools/build.py APP_DIR                      → APP_DIR/build/NAME.fwsc            （實機）
tools/build.py APP_DIR --rfm1               → APP_DIR/build/rfm1/ID-dev+COMMIT.rfm1（RemoteFM1）
tools/build.py APP_DIR --release X.Y        → APP_DIR/build/NAME-X.Y.fwsc
tools/build.py APP_DIR --rfm1 --release X.Y → APP_DIR/build/NAME-X.Y.rfm1
tools/build.py APP_DIR --host               → 本機 host 模擬器（cc，不需 JieLi toolchain）
tools/build.py APP_DIR --web                → 瀏覽器模擬器 APP_DIR/build/emu/（emcc）
```

已經寫在 Felucca/Melodee 等其他平台上的韌體，則走 FM1VST 的 glue（§5）。

---

## 1. 架構：為什麼同一份 C 能跑兩邊

```
              ┌──────────── app.c / app.h / fm1.toml（你的程式，硬體無關）────────────┐
              │                                                                       │
     device/unity.c + plat_fm1.c                                      host/plat_host.c + host/rfm1.c
     felucca-bsp（ST7789、key scan、Q23 audio DMA、USB…）              （panel/LCD/LED/音訊/MIDI 都是記憶體）
              │  JieLi clang 4.0.1 (pi32v2)                                   │  emcc (Emscripten)
              ▼                                                               ▼
     NAME.bin + felucca-bsp loader → NAME.fwsc                      wasm → tools/rfm1_pack.py → NAME.rfm1
              │ fm1_install.py / web installer（USB-MIDI SysEx）               │ remotefm1.com「Open a file…」/ iOS / Android
              ▼                                                               ▼
           FM-1 實機                                                 RemoteFM1 host（FM-1 跑 RemoteFM1 當面板，或螢幕上的虛擬面板）
```

app 只透過 `plat.h` 跟平台溝通，所以編到哪邊都不用改 app 原始碼。

### app 的合約（`app.h`，以 template 為例）

```c
#include "plat.h"
#define APP_NAME "TEMPLATE"
#define APP_BLOCK 256u              /* 每個 audio block 的 frame 數：44.1 kHz 下 5.8 ms */
#define APP_TOUCH                   /* 要 key velocity/pressure（lib/touch.h）*/
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_BLE, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u, 0xDE000u}   /* 各存檔物件的 flash offset */

void app_init(void);                                         /* 開機：engine + 讀設定 */
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);/* audio IRQ：n 個立體聲 frame，24-bit */
void app_panic(void);                                        /* 立刻靜音（更新寫 flash 前）*/
void app_ui_init(void);                                      /* splash 後第一個畫面 */
void app_frame(void);                                        /* UI：讀輸入 + 重畫，約 60 fps */
void app_input_only(void);                                   /* frame 之間只讀輸入 */
void app_say(const char *a, const char *b);                  /* 畫面訊息 + 螢幕閱讀器 */
```

### 寫給兩邊都能跑的守則
- **不要讀 host 的時鐘**，時間一律來自平台的 tick（.rfm1 的 `clock_time_get` 只給 log 用，聲音/畫面不能依賴它）。
- **不要用檔案系統、執行緒、網路、亂數來源**（.rfm1 只允許 WASI 的 `fd_write/fd_read/fd_close/fd_seek/clock_time_get/proc_exit`）。
- 主迴圈要「每 tick 回傳」，不能是永不 return 的 `while(1)`（wasm 版不支援 coroutine，`fm1coro_new` 會 link 失敗）。fm1-base 的 app 天生就是 callback 式，沒有這問題。
- 浮點要可重現：build 會加 `-ffp-contract=off`（兩邊都加，聲音逐樣本一致）。
- 記憶體：.rfm1 是**固定大小**線性記憶體（預設 8 MiB，最多 32 MiB，`fm1.toml` 的 `rfm1_memory_mib`），不能 `memory.grow`。實機則是 app slot 581 KB flash、96 KB `.data+.bss`、336 KB pool。
- radio（BLE/WiFi）、USB host 在 .rfm1 裡不存在；`getenv` 回 NULL。
- wasm 指令集限制（iOS 用 wasm3 0.5.0 解譯）：**不能用** SIMD、threads/atomics、reference types、tail call、exception handling、memory64、`memory.init`/`data.drop`/`table.*`/passive data。build 已設好 flag，但你自己加的 `web_cflags` 別打開這些。

---

## 2. 環境準備

| 項目 | 給 `.fwsc` | 給 `.rfm1` | 說明 |
|---|---|---|---|
| OS | Linux x86-64 原生；macOS/Windows 需 Docker（`linux/amd64`）| 任何有 emcc 的 OS | build.py 偵測到非 Linux x86-64 會自動用 Docker 跑 JieLi 工具 |
| JieLi toolchain | ✅ clang 4.0.1 for pi32v2 | ✗ | `base/tools/get_toolchain.sh` → `~/.jieli/toolchain`，設 `JIELI_TOOLCHAIN` |
| JieLi AC79 SDK | ✅（打包需要 3 個檔；BLE/WiFi 需要其 library）| ✗ | tag `AC79NN_SDK_V1.2.1_2023-12-13`，設 `AC79_SDK`；CI 驗 commit `d179b4484759423312073f5fbb232501aa491047` |
| Emscripten (`emcc`) | ✗ | ✅ | fm1-base template CI 用 **emsdk 6.0.10**（emsdk commit `a2b92777574c2feda07994cd4f1079a3dfc151f8`）；FM1VST 用 6.0.13 |
| Python 3 + Pillow（含 **raqm**）| ✅ | ✅ | 字型/圖示在 build 時產生；Linux `apt install libfribidi0`，macOS `brew install fribidi` |
| Node.js | 選用 | 選用 | `tests/rfm1_test.mjs` 用 |

```sh
# 一次裝好（Linux x86-64）
git clone --recursive https://github.com/charlesvestal/fm1-base      # 內含 felucca-bsp 於 bsp/
cd fm1-base
tools/get_toolchain.sh                                              # → ~/.jieli/toolchain
export JIELI_TOOLCHAIN=~/.jieli/toolchain
git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
    https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
export AC79_SDK=~/fw-AC79_AIoT_SDK
pip3 install Pillow                                                 # 並確認 raqm：
python3 -c "from PIL import features; assert features.check('raqm')"

git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
~/emsdk/emsdk install 6.0.10 && ~/emsdk/emsdk activate 6.0.10
source ~/emsdk/emsdk_env.sh && emcc --version
```

---

## 3. 建立你的韌體專案（fm1-base 樣板）

官方建議的 repo 版型（release workflow 也假設這個版型）：

```
my-fw/
├── app/                 ← 從 fm1-base/template/ 複製過來
│   ├── app.c  app.h  fm1.toml  words.tsv
│   ├── tests/basic.txt  tests/screens.txt
│   └── docs/MANUAL.md   site/landing.md
├── base/                ← git submodule: charlesvestal/fm1-base（裡面 base/bsp = felucca-bsp）
├── build.sh             ← 呼叫 base/tools/build.py app "$@"
├── tests/run_tests.sh
├── RemoteFM1Metadata.json   ← 選用：RemoteFM1 卡片/皮膚/面板標籤
├── firmware.json        ← build 自動產生，要 commit（給 Firmware Hub）
└── .github/workflows/release.yml   ← 從 template/.github/workflows/release.yml 搬過來
```

```sh
mkdir my-fw && cd my-fw && git init
git submodule add https://github.com/charlesvestal/fm1-base base
git submodule update --init --recursive
cp -r base/template app
cat > build.sh <<'EOF'
#!/bin/sh
exec python3 base/tools/build.py app "$@"
EOF
chmod +x build.sh
```

### `app/fm1.toml` 必改欄位

```toml
name = "MYFW"                 # 產物檔名（小寫）：myfw.fwsc / myfw-0.1.rfm1；.rfm1 的 id = 小寫 name
id_prefix = "FM-1_77"         # 安裝識別碼，每個韌體要獨一無二：dev = FM-1_7700，release = FM-1_77XXYYZZ
usb_manufacturer = "me"
usb_product = "MYFW FM-1"     # 名稱含 "FM-1"，安裝器才找得到 port
o2 = ["dsp/*.c"]              # DSP 熱點用 -O2 單獨編
ble = false                   # 先關掉 BLE：省 flash/RAM，也不需要 SDK 的 BT library
repo = "https://github.com/me/my-fw"   # .rfm1 manifest 的 source_url = 這個 repo @ commit → 編之前要先 commit + push
licence = "MPL-2.0"           # SPDX；.rfm1 manifest 的 license
authors = ["Me"]              # .rfm1 manifest 的 authors
rfm1_memory_mib = 8           # .rfm1 固定記憶體（1..32）
# release build 會檢查下列都有填：
author = "Me"
description = "..."
tags = ["synth"]              # 只能用 docs/FIRMWARE-JSON.md 列的值
install = "https://me.github.io/my-fw/install/"
site = "https://me.github.io/my-fw/"
tagline = "..."
screenshot = "app/docs/img/screen.png"   # ≥240×240 的正方形 PNG
```

> `id_prefix` 的例子：template `FM-1_11`、RemoteFM1 `FM-1_52`、Bass Something 2 `FM-1_62`、LOOM-8 `FM-1_10`。挑一個沒人用的。

---

## 4. 編譯流程

### 4.1 開發迴圈（最快，不碰任何硬體）

```sh
./build.sh --host                                   # 本機 cc 編 host 模擬器
app/build/host/myfw_host app/tests/basic.txt app/build/host   # 跑腳本測試（按鍵、MIDI、存檔重開、聲音、螢幕閱讀器）
./build.sh --web                                    # 瀏覽器模擬器 → app/build/emu/
(cd app/build/emu && python3 -m http.server 8000)   # http://localhost:8000
```

### 4.2 產出 RemoteFM1 用的 `.rfm1`

```sh
git commit -am "..." && git push          # manifest 的 source_url 指向 repo@commit，所以先 commit/push
./build.sh --rfm1                         # → app/build/rfm1/myfw-dev+<commit>.rfm1
./build.sh --rfm1 --release 0.1           # → app/build/myfw-0.1.rfm1
node base/tests/rfm1_test.mjs app/build/myfw-0.1.rfm1   # 用 host app 的方式驅動檔案做測試
```

build.py 內部做的事（`host/rfm1.c` + `tools/rfm1_pack.py`）：
1. 用 emcc 把 host 模擬器（`plat_host.c` + 你的 app）編成 wasm32，固定記憶體、`-ffp-contract=off`、關掉不允許的 wasm feature。
2. 匯出 `rfm1_get / rfm1_layout / rfm1_init / rfm1_tick / rfm1_frame / rfm1_idle / rfm1_audio / rfm1_midi_in / rfm1_midi_out_take / rfm1_flash* / rfm1_alloc / rfm1_free / rfm1_options / rfm1_boot_options / ...`。
3. 塞入 `rfm1` custom section（manifest JSON）、`rfm1.credits`、`rfm1.license.<n>`。
4. 依格式檢查 module（import 白名單、export 簽名、記憶體、指令集），不合格就不寫檔。

manifest 長這樣（由 fm1.toml 自動填）：
```json
{ "rfm1": 1, "abi": 6, "id": "myfw", "name": "MYFW", "version": "0.1",
  "license": "MPL-2.0", "source_url": "https://github.com/me/my-fw@<commit>",
  "authors": ["Me"], "built_by": "...", "memory_mib": 8 }
```

**在 RemoteFM1 上跑：**
- 網站：開 `https://remotefm1.com/` → 韌體瀏覽器「**Open a file…**」或直接把 `.rfm1` 拖到頁面。會存在瀏覽器 IndexedDB 的「Your firmware」。About 頁顯示授權、作者、原始碼 commit。
- 用實體 FM-1 當面板：FM-1 先燒 RemoteFM1（`https://remotefm1.com/installer/`），然後網站上選「**Play on my FM-1**」（Chrome/Edge，USB 或 BLE MIDI）。
- iOS：把 `.rfm1` 用 Files/AirDrop 開啟並選 RemoteFM1，或放到「檔案 › 我的 iPhone › RemoteFM1 › Firmware」。Android：APK 同理。
- 加皮膚（選用）：`scripts/rfm1_skin.py add fw.rfm1 -o out.rfm1 --background bg.png [--landscape wide.png] [--theme t.json] [--dim 0.2]`（RemoteFM1IOS repo；會改變檔案 SHA-256，程式碼不變）。

### 4.3 產出 FM-1 實機可燒的 `.fwsc`

```sh
./build.sh                       # → app/build/myfw.fwsc（dev，identity FM-1_7700）+ app/build/myfw.bin
./build.sh --release 0.1         # → app/build/myfw-0.1.fwsc（identity FM-1_77000100）
./build.sh --no-ble              # 有開 ble 時，另出一個不含藍牙的 myfw-usb.fwsc
FM1B_CPU2=0 ./build.sh           # 單核（環境變數可覆寫 fm1.toml 的不少選項，見 build.py 開頭說明）
```

build.py 內部做的事：
1. 用 JieLi clang（`-mcpu=r3 -mfprev1 -ffp-contract=off`）編 unity build（平台 + app）與 `o2` 單元。
2. 檢查：MMIO 位址、不准呼叫晶片 ROM、64-bit 乘法夾在 load 間的 CPU bug（felucca-bsp `docs/CPU.md`）、stack 深度。
3. 連結 → `myfw.bin`；加上 felucca-bsp 的 update loader；用 SDK 的 `uboot.boot`/`cfg_tool.bin`/`eq_cfg_hw.bin` 打包成 `.fwsc`。
4. 寫 `firmware.json`（給 Firmware Hub / RemoteFM1 / M-Flasher）。

**燒錄：**
```sh
python3 base/tools/fm1_install.py app/build/myfw-0.1.fwsc   # 需 pip3 install mido python-rtmidi
```
或用 `base/tools/make_site.py app app/build/myfw-0.1.fwsc 0.1 OUT_DIR` 產生自己的 web installer（Chrome/Edge，Web MIDI 只在 localhost/HTTPS 可用）。

> 燒錄前提醒（詳見第一份文件 §10）：先備份、充飽電、不要斷線；手邊準備好救磚工具（`MvaveFM1Unbricker`、FM-1-transporter）。fm1-base 韌體內建 boot guard（連續兩次開機失敗 → safe mode、USB 開著）、crash 畫面、watchdog、開機按住 REC 進更新模式。

### 4.4 一次出兩種（release）

```sh
./build.sh --release 0.1 && ./build.sh --rfm1 --release 0.1
ls app/build/myfw-0.1.fwsc app/build/myfw-0.1.rfm1 app/build/firmware.json
```

---

## 5. 已經是別的平台（Felucca / Melodee / Dinghy…）的韌體怎麼出 `.rfm1`

用 **FM1VST** 的 glue。FM1VST 把各家韌體的 HAL 對接到 `fm1core.h`（ABI 6），再用 `fm1_add_rfm1` 編成 wasm。

### 5.1 FM1VST 已經支援你的韌體（family 已有 glue）

```sh
git clone -b rfm1-target https://github.com/Quixotic7/FM1VST.git && cd FM1VST
# 把你的韌體放在 glue 預期的 submodule 路徑，例如 cores/Felucca
cmake -S . -B build-rfm1 -DFM1_RFM1_ONLY=ON -DFM1_PUBLIC_BUILD=ON -DFM1_ONLY_CORES=<id>
cmake --build build-rfm1 --target rfm1_<id> rfm1_run
# → build-rfm1/rfm1/<id>-<version>.rfm1
python3 tools/rfm1_check.py build-rfm1/rfm1/<id>-<version>.rfm1   # 格式檢查（只用標準函式庫）
build-rfm1/rfm1_run build-rfm1/rfm1/<id>-<version>.rfm1          # wasm3 開機、按 D4 F#4 A4 和弦 5 秒，報 RMS/LCD/速度
ctest --test-dir build-rfm1 -R rfm1_
```
- 不想改 glue 就試：`-DFM1_RFM1_EXTRA_CORES=<id>`（或 `all`）。
- `fm1_add_rfm1(<id> AUTHORS "..." [MEMORY_MIB n] [STACK_KIB n] [LICENSE_FILES ...] [SOURCE_URL ...] [DEFINES/FLAGS/INCLUDES ...])`，沒加 `-DFM1_RFM1=ON` 時這行什麼都不做。
- 已有 glue 的 family：`felucca_web`（Felucca、GHOULBOX、JIANT、DUELL、Mini-Tone、FMini-1、Skiff、PIGEON…）、`fm1base`、`plat`（X0X 系）、`dinghy`、`sloop`、`choralroot`、`melodee`、`tonitor`、`speak37`。
- **限制**：主迴圈永不 return 的平台（Dinghy、Melodee、Tonitor）原本靠 `coro.c`（ucontext）→ wasm link 失敗（`undefined symbol: fm1coro_new`），需要改寫成「每 tick 回傳」的 glue（S1Vave 用的 `s1vave` branch 有 wasm 內 coroutine 的做法）。

### 5.2 新平台：自己寫 glue（`core-glue/common/README.md` 的「Adding a core」）
1. 韌體當成 pinned、不修改的 submodule：`cores/<Name>`。
2. `core-glue/<family>/core.cmake`：
   ```cmake
   fm1_add_core(<id> ${CMAKE_CURRENT_LIST_DIR}
                SOURCE ${CMAKE_SOURCE_DIR}/cores/<Name>
                UNIT core_<id>.c DESC core_<id>_desc.c
                FAMILY <family> LICENSE GPL-3.0-only
                GEN ui_fonts.h          # 或 NO_GEN
                VERSION_FILE ... VERSION_REGEX ...)
   fm1_add_rfm1(<id> AUTHORS "Your Name")
   ```
3. `core_<id>.c`：include `version.h`、`exit.h`、韌體原始碼（含其 host HAL）、`exit_end.h`，再實作 `fm1core_<p>_*` 進入點：tick/frame/idle 開頭把 panel 輸入拷進 HAL，init/frame/idle 結束時輸出 LED 與 LCD。可直接抄 `core-glue/felucca/core_felucca.c`。
4. `core_<id>_desc.c`：descriptor（id、family、license、version、source_url、14 個按鈕名稱）。
5. `conformance_<id>.txt`：flash round-trip、聲音前奏、reboot 測試。
6. 輔助：`nor.{h,c}`（1 MiB NOR 映像：4 KiB erase、256-byte page program、erase 期間靜音 45 ms）、`fifo.{h,c}`（任意 block 大小轉成 ABI 的 128 frame int16 立體聲）、`ticks.h`（模擬的 ms/us/24 MHz 時鐘）、`panel.h`、`sysex.h`。

Host 的呼叫順序（寫 glue 時要符合）：`options` → `boot_options` → `panel->ready = 2` → `init`；之後每個裝置 ms：`tick(ms)`，再 `frame()`（第 0 ms 與每 15 ms）或 `idle()`，再對每個到期的 block 呼叫 `audio(stereo, 128)`（44100 Hz）。

### 5.3 或者乾脆搬到 fm1-base
fm1-base 附了一個 Claude skill `skill/felucca-to-fm1-base/`，專門把 Felucca 系韌體搬到 fm1-base（盤點→移植→逐 byte 比對模擬器輸出→實機），**不會擅自改授權**：
```sh
npx degit --force charlesvestal/fm1-base/skill/felucca-to-fm1-base .claude/skills/felucca-to-fm1-base
```
搬完就能用 §4 的一行指令同時出兩種產物。

---

## 6. CI：tag 一推，GitHub 自動出兩種並簽章

fm1-base template 的 `.github/workflows/release.yml`（搬到 repo 根目錄，改 `# <- yours` 那幾行：`pkg=myfw`、標題、concurrency group）會：
1. `prepare`：建立 draft release（帶 `-` 後綴的 tag 為 prerelease；已公開的 release 不會重建）。
2. `firmware`：呼叫 `zvenson/fm1-build/.github/workflows/build.yml@v1`（sdk: true），在裡面：
   - `git submodule update --init --recursive`、裝 `libfribidi0`、確認 SDK commit。
   - 裝 emsdk 6.0.10。
   - `./build.sh --release $version` → 複製 `myfw-X.Y.fwsc`、`myfw-X.Y-app.bin`
   - `./build.sh --rfm1 --release $version` → 複製 `myfw-X.Y.rfm1`
   - 複製 `firmware.json`
   - 跑 `tests/run_tests.sh`（失敗就不發佈）
   - 對全部產物做 build provenance attestation。
3. `verify`：`sha256sum --check`，對每個 `.fwsc`/`-app.bin`/`.rfm1`/`firmware.json` 跑 `gh attestation verify ... --signer-workflow zvenson/fm1-build/.github/workflows/build.yml`，通過才把 draft 發佈。

```sh
git tag v0.1 && git push origin v0.1
```
Firmware Hub 會因 attestation 顯示「Built from source」徽章。

**選用：remotefm1.com 的 native build（`.fm1web`）** —— 在 release workflow 再加一個 job：
```yaml
  native:
    needs: build
    uses: Quixotic7/RemoteFM1/.github/workflows/fm1-native.yml@native-v1
    permissions: { contents: write, id-token: write, attestations: write }
    with: { id: myfw, family: fm1base }      # family 見 §5.1
```
然後到 RemoteFM1 開 issue 申請列入；網站每晚同步，卡片會標「native · built by the author」。

**選用：`RemoteFM1Metadata.json`**（repo 根目錄）—— 設定 RemoteFM1 上的卡片（tag ≤40 字、what ≤600 字、category: Synth/Groovebox/Chords/Drums/Other、site、editor、port、正方形 image ≤1 MiB）、皮膚、面板按鈕/旋鈕標籤、要建置哪個 release（`target`）。Quixotic7 會替大多數 FM-1 韌體從你的 release 建 `.rfm1` 放到 RemoteFM1FW，所以你甚至可以不自己出 `.rfm1`。

---

## 7. 常見坑

| 症狀 | 原因 / 解法 |
|---|---|
| `.rfm1` link 失敗 `undefined symbol: fm1coro_new` | 韌體主迴圈不 return；改成 tick 式 glue（§5.1）|
| `.rfm1` 被 host 拒絕 | import 了 WASI 白名單外的東西、用了 SIMD/threads/`memory.grow`、或 manifest 與 descriptor 的 id/name/version 不一致 |
| link 說 data 放不下 | 提高 `rfm1_memory_mib`（fm1-base）或 `MEMORY_MIB`（FM1VST），上限 32 |
| 字型產生失敗 | Pillow 沒有 raqm → 裝 fribidi；macOS SIP 會清掉 `DYLD_*`，FM1VST 用 `cmake -E env DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib` |
| `.rfm1` 的 source_url 指到不存在的 commit | 編之前沒 commit/push |
| release build 拒絕 | fm1.toml 的 author/description/tags/install/tagline/repo/licence/screenshot 沒填齊 |
| 兩邊聲音不一樣 | 有程式讀了 host 時鐘、或有浮點 contract；只用平台 tick，保持 `-ffp-contract=off` |
| 實機安裝被 RemoteFM1 installer 拒絕 | 映像超出 app 區（0x4000–0x93000），例如 M-VAVE V16、Baud Girl 的包 |
| 實機 USB 版不需要 BT 卻吃掉一堆 flash | `ble = false` 或 `--no-ble`；BLE（JieLi stack）約 +124 KB flash / +45 KB RAM；或用 `ble_stack = "own"`（約 +41–46 KB）|
| 授權 | 要連結 JieLi 的 BLE/WiFi binary library 必須是 MPL 之類可相容的授權，GPL 韌體不行（RemoteFM1 因此從 GPL 改 MPL-2.0）。`.rfm1` 也是 binary，GPL 韌體要附原始碼連結（manifest 的 source_url 與 glue_url）|

---

## 8. 給接手 agent 的最短清單

1. Linux x86-64：裝 JieLi toolchain、AC79 SDK、emsdk 6.0.10、Pillow+raqm（§2）。
2. 用 fm1-base template 開 repo（§3），改 `fm1.toml` 的 `name`/`id_prefix`/`usb_product`/`repo`/`licence`/`authors`。
3. `./build.sh --host` + `app/tests/basic.txt` 跑通 → `./build.sh --web` 用瀏覽器看。
4. commit + push → `./build.sh --rfm1` → 拖到 remotefm1.com 測試。
5. `./build.sh` → `fm1_install.py` 燒實機（先備份、備好救磚）。
6. 搬 release.yml，`git tag v0.1 && git push --tags` 讓 CI 出簽章過的 `.fwsc` + `.rfm1` + `firmware.json`。

## 參考連結
- RemoteFM1：https://github.com/Quixotic7/RemoteFM1 ・ https://remotefm1.com/ ・ docs/NATIVE-AUTHORS.md
- `.rfm1` 格式：https://github.com/Quixotic7/RemoteFM1IOS/blob/main/docs/RFM1-FORMAT.md
- FM1VST（rfm1-target branch：docs/RFM1.md、core-glue/common/README.md）：https://github.com/Quixotic7/FM1VST/tree/rfm1-target
- RemoteFM1FW（`.rfm1` 目錄、RemoteFM1Metadata.json 規格）：https://github.com/Quixotic7/RemoteFM1FW
- fm1-base：https://github.com/charlesvestal/fm1-base （template/、tools/build.py、docs/FIRMWARE-JSON.md）
- 範例：https://github.com/imijes/bass-something-2 （fm1-base 韌體，同時出 `.fwsc` 與 `.rfm1`）
- 共用 CI：https://github.com/zvenson/fm1-build
- felucca-bsp：https://github.com/hugelton/felucca-bsp
