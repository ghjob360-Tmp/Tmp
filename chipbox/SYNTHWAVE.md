# Chipbox Synthwave 版：新增功能說明

這份是 Chipbox 擴充版的使用說明，原版的操作全部不變。
舊專案打開來聲音和原版**完全一樣**（有自動測試逐樣本比對過）。新設定的預設值都等於「原本的行為」。

> 狀態：`.fwsc` 和 `.rfm1` 都已編譯（放在 `dist/`），電腦版測試全部通過，**還沒在 FM-1 實機上跑過**。
> 實機請先用 USB 接著看 CPU：裝置會用 MIDI CC 100 回報 CPU %。詳見文末的「實機注意事項」。

---

## 1. 新音源（加入頻道時選）

| 音源 | 是什麼 | 專屬頁面 |
|---|---|---|
| **VA Synth** | 類比合成器：兩個振盪器（鋸齒／脈衝／三角／正弦）、低八度 sub、噪音、最多 7 個 supersaw 去諧副本、最多 4 音複音（和弦、pad） | OSC、MIX、VOICE |
| **SNES** | S-DSP 取樣器：4-bit BRR 壓縮、Gaussian 4 點內插、32 kHz 音高步進；跟真的晶片一樣，最多只能比根音高兩個八度 | SAMPLE |
| **Amiga** | Paula 取樣器：8-bit、無內插、音高量化成整數 period、A500 輸出濾波器，加上 LED 濾波器開關 | SAMPLE |

15 種取樣樂器：SAW、SQUARE、STRINGS、CHOIR、BRASS、E.PIANO、BASS、BELL、PAD、ORGAN、FLUTE、PLUCK、ORCHHIT、SYNBASS、MARIMBA。
這些取樣是第一次加入 SNES 或 Amiga 頻道時才在 RAM 裡合成出來的（約 14 KB），**不佔 flash**，也沒有用到任何第三方取樣檔。

## 2. 晶片畫面（CHIP）的新頁面

用 ALGORITHM 往後翻，原本的頁面後面會多出這些頁。所有旋律頻道都有，FM 晶片要先按 SEL 切回「頻道」焦點：

| 頁面 | KNOB 1 | KNOB 2 | KNOB 3 | KNOB 4 |
|---|---|---|---|---|
| **ARP+** | 八度數 1–4 | Gate（LEG＝連音，1/8–7/8） | USER 和弦第 2 音 | USER 和弦第 3 音 |
| **FILTER** | 類型 OFF/LP/BP/HP/LP24 | Cutoff | Resonance | 濾波器包絡量（±） |
| **SHAPE** | 濾波器包絡衰減 | Key tracking | Drive（失真） | Glide（滑音時間） |
| **LFO** | 目標：音高/Cutoff/音量/Pan/PWM | 速度（自由 0.1–20 Hz，或同步 4BAR–1/16） | 深度 | 波形 TRI/SAW/SQR/S&H/RAMP |
| **WIDE** | 去諧副本數 2–4（unison／ensemble） | 去諧量 | 立體聲寬度 | — |
| **SENDS** | Chorus 送出量 | Delay 送出量 | Reverb 送出量 | — |
| **STEPS** | 速度 1/16、**1/16T**、1/32、**1/8T**、1/8、1/32T、1/4T、1/4 | 長度 1–16 步 | 參數鎖定的目標 | 和弦用下方幾個頻道（+1～+3） |

**原本的 ARP 頁**也擴充了：
- SHAPE：22 種和弦，包括 OCT、MAJ、MIN、MAJ7、MIN7、SUS4、PWR、DIM、NOTE、MAJ3、MIN3、ADD9、MIN9、MAJ9、DOM7、7SUS、6TH、MIN6、M11、AUG、SUS2 和 USER（自己設定）。
- RATE：0–3 是舊的固定速度；從 1/4 開始**跟著 BPM 同步**（1/4、1/8、1/8T、1/16、1/16T、1/32、1/32T）。新增的頻道預設 1/16。
- DIR：UP、DOWN、UPDN、DNUP、RAND、CONV。

**三連音**有兩種做法：
- 在 STEPS 頁把頻道設成 1/16T、長度 12。這樣一小節剛好是 24 個 1/16 三連音。
- 在 STEP 畫面用 ratchet 3，讓單一步打三下。

## 3. STEP 畫面（SEQ）

**輕按 ENV** 可以切換旋鈕層。鍵盤下面的條會顯示目前四顆旋鈕各在做什麼：

| 層 | KNOB 1 | KNOB 2 | KNOB 3 | KNOB 4 |
|---|---|---|---|---|
| 1 | BPM | 音長 | 力度 | Ratchet（一步打 1–4 下） |
| 2 | Nudge（往後推 0–5 tick） | 機率 100%…25%、1:2、2:2 | Slide（滑進這一步） | 和弦 |
| 3 | 參數鎖定值 | 頻道速度 | 頻道步數 | 鎖定目標 |

- **和弦輸入**：先按住一個鍵，再按其他鍵。最低音當根音，自動配對最接近的和弦。
  - VA／SNES／Amiga 的 POLY 設大於 1 時，和弦直接在同一個頻道播。
  - 其他晶片會分配到下方 1–3 個頻道（自動設成 +2）。
- **Slide**：TB-303 的做法。前一個音會延續到這一步，然後滑音過去。
- **參數鎖定（p-lock）**：每一步可以鎖一個值。鎖哪個參數由頻道的 LOCK 設定決定：CUT、RES、FENV、VOL、TONE、DEC、PAN、DLY、REV、CHO、DRIVE。
- **SEL** 複製這個頻道的這一頁；**按住 SEL** 貼上，可以貼到別的頻道。
- **EDIT** 清除一步的時候，那一步的 ratchet、和弦、鎖定也會一起清掉。
- 方塊上的小記號：上方的點是 ratchet；黃點是有機率；底下的鉤是 slide；十字是和弦；方塊是鎖定。
- **鼓的頻道**：用 SELECT 移動游標（上方的白線），ENV 切換層，可以設 ratchet、nudge、機率、速度、步數。

## 4. MIX 畫面（第四個分頁）

按 HOME 的順序是 RACK → CHIP → SONG → **MIX**。用 ALGORITHM 翻頁：

| 頁 | 內容 |
|---|---|
| GROOVE | Swing 50–75%；鍵盤音階鎖定（MAJOR、MINOR、DORIAN、PHRYG、MIXO、HARM、PENTA、PENTm、BLUES）和調性 |
| CHORUS | Juno 式 chorus：I、II、I+II；速度、深度 |
| DELAY | 跟 BPM 同步（1/16–1/2，含附點和三連音）；回授；STEREO 或 PING-PONG；音色 |
| REVERB | 大小、阻尼、**GATE**（80 年代 gated snare） |
| MASTER | 壓縮器、飽和、sidechain 回復時間和曲線（LINEAR／CURVE／HOLD） |

- FX 鍵盤上的 ECHO 和 VERB pad 現在共用同一組 delay 和 reverb。設定是預設值時，聲音跟原版一樣。
- 這些設定都會存在專案裡。

## 5. 其他

- **Redo**：按住 ENV 再按 LFO。單按 LFO 一樣是 undo。
- **音色庫**：在 CHIP 畫面**按住 SEL** 會跳出清單。
  - 有 12 個工廠預設：Supersaw Lead、Juno Pad、Pluck Bass、Brass Stab、Arp Pulse、PWM Strings、SNES Strings、SNES Choir、SNES E.Piano、Orch Hit、Amiga Bass、NES Arp Lead。
  - 另有 31 個 USER 槽：OCT+ 載入，SAVE 存入。
  - 存放在專案區後面那個原本沒用到的 4 KB flash 磁區，換專案也還在。
- **網頁版**：多了「Record WAV」按鈕，可以直接錄 44.1 kHz 的立體聲 WAV。
- 實機本來就能當 USB 音訊輸入，DAW 也可以直接錄。

## 6. 專案格式

- 專案版本從 5 升到 6，新資料加在原本資料的最後面。
- 舊專案可以正常讀取。
- 用新韌體存過的專案，**舊版 Chipbox 讀不進來**（會被當成無效檔），請先備份。

## 7. 實機注意事項（請先看）

1. **已編譯，但還沒有實機測試。** 現成的檔案在 `dist/`：
   - `chipbox-1.6.1-synthwave.fwsc`：FM-1 安裝包。用安裝頁面或 `tools/fm1_install.py` 燒進去。
   - `chipbox-1.6.1-synthwave.rfm1`：RemoteFM1 用。在 remotefm1.com 選「Open a file…」，或放進 App 的韌體資料夾。
   - 要自己重編：`.fwsc` 執行 `./build.sh`；`.rfm1` 執行 `rfm1/build_rfm1.sh`（說明見 `rfm1/README.md`）。也可以在 GitHub Actions 手動執行 `chipbox` workflow。
2. **CPU。** 這個「重」配置在電腦上大約是原版 demo 的 3.8 倍：
   - 4 音 Juno pad
   - supersaw lead
   - 濾波 bass
   - SNES strings
   - 開著 chorus、reverb、壓縮

   請在實機上監看 CC 100。爆音的話，降低 POLY、UNISON、WIDE 副本數，或少開幾個頻道的 FILTER。
3. **記憶體**：
   - 實際 build：app 415,236／581,564 bytes；RAM（.data＋.bss）87,968／98,304 bytes；pool 337,808／344,064 bytes。
4. **Brothers Link**：swing 只作用在自己內部的時脈；跟隨外部時脈時不套用 swing。

## 測試

```sh
cd chipbox
cc -O2 -DCHIPBOX_HOST -DCHIPBOX_TEST -Dmain=host_main -Ifirmware/src -Ifirmware/gen -o build/host/ext_check tests/ext_check.c -lm
build/host/ext_check          # 40 項檢查：音高、arp 時間、三連音、ratchet、機率、nudge、swing、和弦、slide、濾波、鎖定、send、gated reverb、存檔、預設音色
tests/shots.sh tests/stepx.txt build/shots/stepx.png   # 螢幕截圖（mix.txt、patch.txt、va_pages.txt 也可以）
```
