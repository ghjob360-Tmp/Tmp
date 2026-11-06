# KORG NTS-1 (mk1) 合成鏈路調查 → 移植到 M-VAVE FM-1 的設計

> 調查日期：2026-11-06。來源列在最後。標記：**[手冊]** = 原廠手冊參數表；**[評測]** = Sound On Sound 評測；**[SDK]** = logue SDK 文件；**[推測]** = 文件沒寫清楚，這是我的解讀。
> 本文只參考 NTS-1 的「訊號流程與參數架構」，FM-1 韌體（CHAIN-1）的 DSP 全部是自己寫的，沒有使用 Korg 的程式碼、音色或素材，名稱也不使用 Korg 的商標。

---

## 1. NTS-1 mk1 概要

- Nu:Tekt NTS-1 digital kit（2019）：DIY 組裝的單音（monophonic）數位合成器 / 效果器。[評測]
- 引擎和 prologue / minilogue xd 同一系列，可以用 **logue SDK** 載入自製 oscillator 與 mod/delay/reverb 效果。[SDK][評測]
- 控制：觸控 ribbon（彈奏 / 琶音）、TYPE 旋鈕、A / B 兩個旋鈕、OSC / FILTER / EG / MOD / DELAY / REVERB / ARP 按鈕。按住按鈕再轉 A/B = 該區段的第二組參數。[手冊]
- 立體聲 3.5 mm Audio In，可以只拿效果器處理外部聲音（不經過濾波器）。[評測]
- 使用者槽位：16 osc、16 mod、8 delay、8 reverb。[評測]
- 平台取樣率 48 kHz、render callback 至少 64 frames、pitch / cutoff / resonance 用整數傳給 user oscillator（cutoff、resonance 是 0x0000–0x1FFF）。[SDK]

## 2. 訊號流程

```
                ┌──────── OSC LFO（rate 0–30 Hz；depth 雙極：P=pitch ← 0 → S=shape）
                ▼
 ribbon/MIDI ─► OSC ──► FILTER ──► AMP(EG) ──► MOD FX ──► DELAY ──► REVERB ──► OUT (stereo)
  (ARP)        SAW       LP2/LP4     ADSR       CHORUS     STEREO     HALL
               TRI       BP2/BP4     AHR        ENSEMBLE   MONO       PLATE
               SQR       HP2/HP4     AR         PHASER     PINGPONG   SPACE
               VPM       OFF         AR LOOP    FLANGER    HIPASS     RISER
               USER                  OPEN       USER       TAPE       SUBMARINE
                           ▲            ▲                  USER       USER
               CUTOFF SWEEP┘   TREMOLO──┘
               (rate 0–30 Hz,  (rate 0–60 Hz,
                depth u/d)      depth 0–100)
 Audio In ─(Input route 全域設定：mod 前 / delay 前 / reverb 前 / master 前 / master 後)─► 效果鏈
```
- 效果順序 MOD → DELAY → REVERB，reverb 在最後。[評測]
- 三個效果開機時都是 bypass（OFF）。[評測]

## 3. 各區段參數

### OSC [手冊][評測]
| 參數 | 內容 |
|---|---|
| TYPE | SAW、TRI、SQR、VPM、USER（logue SDK）；另外有 WAVES（波表，兩組各 46 波，A↔B morph，附 sub / ring / bitcrush）[評測] |
| SHAPE（A）| 波形整形量（每種 osc 意義不同）|
| ALT（B，按住 OSC）| 替代參數，每種 osc 不同 |
| LFO RATE | F0.0–F30.0 Hz |
| LFO DEPTH | P100 … P001、--、S001 … S100（負向調音高、正向調 shape）|
- VPM 是 Korg 的 FM 類（相位調變）振盪器，NTS-1 只有一種 carrier/modulator 演算法。[評測]
- SAW/TRI/SQR/VPM 的 SHAPE / ALT 各自代表什麼，手冊摘錄沒寫清楚。[推測：VPM 的 SHAPE 是調變深度、ALT 是頻率比或 feedback]

### FILTER [手冊][評測]
| 參數 | 內容 |
|---|---|
| TYPE | LP2、LP4、BP2、BP4、HP2、HP4、OFF |
| CUTOFF（A）/ RESONANCE（B）| 不會自激振盪 [評測] |
| SWEEP RATE | F0.0–F30.0 |
| SWEEP DEPTH | u100 … u001、--、d000 … d100（往上 / 往下）|
- 評測描述 sweep 是「單一斜率，往上或往下」。[推測：每次觸發時以 rate 速度重複的鋸齒斜坡]

### EG（音量包絡）[手冊][評測]
| 參數 | 內容 |
|---|---|
| TYPE | ADSR、AHR、AR、AR LOOP、OPEN |
| ATTACK（A）/ RELEASE（B）| 只有這兩個可以調；ADSR 的 decay / sustain 固定，sustain 大約是峰值的一半 [評測] |
| TREMOLO RATE | F0.0–F60.0 Hz |
| TREMOLO DEPTH | d000–d100 |

### MOD / DELAY / REVERB [手冊][評測]
| 區段 | TYPE | A | B | 按住 |
|---|---|---|---|---|
| MOD | OFF、CHORUS、ENSEMBLE、PHASER、FLANGER | TIME（速度）| DEPTH | — |
| DELAY | OFF、STEREO、MONO、PING PONG、HIGH PASS、TAPE | TIME（最長約 500 ms）| DEPTH | MIX（dry ↔ wet）|
| REVERB | OFF、HALL、PLATE、SPACE、RISER、SUBMARINE | TIME | DEPTH | MIX |
- TAPE delay 會晃動並逐漸劣化。[評測]

### ARP [手冊][評測]
| 參數 | 內容 |
|---|---|
| 開關 | 短按切換開關，長按（約 0.5 秒）latch |
| TYPE（pattern，10 種）| UP、DOWN、UP-DOWN、DOWN-UP、CONVERGE、DIVERGE、CONV-DIV、DIV-CONV、RANDOM、STOCHASTIC |
| A | pattern 長度 1–24 步 |
| B | tempo 56.0–240.0 BPM（外部同步時改成步長 16 分音符到 64 分音符）|
| 和弦（按住 ARP + 按鈕）| OCTAVE、MAJ TRIAD、MAJ SUS、MAJ AUG、MIN TRIAD、MIN DIM |
| 八度 | ARP + OSC |

### 全域（按住 REVERB 開機）[手冊]
Input route（0 = mod 前 … 4 = master 後）、input trim、sync in/out 極性、tempo 範圍、MIDI channel、觸控校正等。

## 4. 移植到 FM-1：CHAIN-1 的設計

### 4.1 硬體差異與對應
| | NTS-1 mk1 | FM-1 / CHAIN-1 |
|---|---|---|
| 取樣率 | 48 kHz | 44.1 kHz |
| 演奏介面 | ribbon | 27 鍵（F3–G5）+ OCT−/OCT+ |
| 參數旋鈕 | TYPE + A + B（按住按鈕切換第二組）| PRESET = TYPE，K1 = A，K2 = B，K3/K4 = 第二組（不必按住）|
| 區段選擇 | 各區段一顆按鈕 | EDIT = OSC、SEL = FILTER、ENV = EG、FX = MOD→DELAY→REVERB 輪流、ARP = ARP；SELECT 編碼器也能換區段 |
| 顯示 | 7 段 LED | 240×240 彩色 LCD：區段、TYPE、4 個參數條、整條訊號鏈 |
| Audio In | 有 | 沒有（FM-1 無類比輸入；USB audio-out 由平台混音，不經過效果）|
| User oscillator / FX | logue SDK | 不支援（要擴充就直接改 C 原始碼）|

### 4.2 各區段實作（`dsp.c`，float，編譯 -O2）
| 區段 | 實作 | 和 NTS-1 不同的地方 |
|---|---|---|
| OSC SAW | PolyBLEP 鋸齒；SHAPE = hard sync（slave 頻率比 1–4）；ALT = sub osc（低八度方波）音量 | SHAPE/ALT 的意義是我自己定義的 |
| OSC TRI | 三角波 → sin wavefolder；SHAPE = fold 量；ALT = sub | 同上 |
| OSC SQR | PolyBLEP 脈波；SHAPE = 脈寬 50 %→95 %；ALT = sub | 同上 |
| OSC VPM | 2-op 相位調變（sine 調 sine）；SHAPE = 調變指數 0–6；ALT = 頻率比 {0.5, 1, 1.5, 2, 3, 4, 5, 7} 加一點 feedback | 原廠演算法未公開 |
| OSC LFO | 三角波 0–30 Hz；depth 雙極：負 = 音高（最多 ±7 半音）、正 = shape | |
| FILTER | TPT state-variable filter（Zavalishin），4-pole = 兩級串接；輸入過 tanh；cutoff 20 Hz–18 kHz 指數對應；resonance 上限不會自激 | |
| SWEEP | 每次 note-on 重新開始、以 rate 速度重複的斜坡；depth 往上 / 往下，最多 ±4 個八度 | 斜坡的形狀是推測 |
| EG | ADSR（decay 0.3 秒、sustain 0.5）、AHR、AR（一次性）、AR LOOP（按住時循環）、OPEN（常開）；attack 1 ms–5 s、release 1 ms–8 s | |
| TREMOLO | sine 0–60 Hz | |
| MOD | CHORUS（2 tap、L/R 正交）、ENSEMBLE（3 tap、相位差 120°、加快速顫動）、PHASER（6 級一階全通 + feedback）、FLANGER（短延遲 + feedback）| |
| DELAY | 最長 500 ms 立體聲（int16 緩衝放在 pool）；STEREO / MONO / PING PONG / HIGH PASS（feedback 路徑串高通）/ TAPE（wow & flutter、低通、飽和）；DEPTH = feedback | |
| REVERB | 4 線 FDN（Hadamard 矩陣）+ 4 個輸入擴散 allpass；HALL / PLATE / SPACE 的線長、阻尼不同；RISER = 每次 note-on 濕訊號逐漸增強並往上掃高通；SUBMARINE = 濕訊號重低通並慢速調變線長 | RISER / SUBMARINE 是近似，原廠演算法未公開 |
| ARP | 10 種 pattern、1–24 步、56–240 BPM 十六分音符、6 種和弦、1–3 個八度、50 % gate；按住鍵盤上的音當根音；長按 ARP 鍵 latch | NTS-1 用 ribbon 位置當根音 |

### 4.3 預算
- 單音、每個 sample 大約 250 次浮點運算；在 360 MHz 單精度 FPU 上預期負擔很低。**實機 CPU 負載沒量過**（目前只有 host 模擬器數據）。
- RAM：delay 2 × 22,050 × int16 ≈ 86 KB、reverb 約 40 KB、mod 約 16 KB，都放在 `.pool`（上限 336 KB）。

## 5. 來源
- NTS-1 原廠手冊參數表（ManualsLib 摘錄）：https://www.manualslib.fr/manual/146060/Korg-Nu-Tekt-Nts-1-Digital-Kit.html?page=2
- Sound On Sound 評測：https://www.soundonsound.com/reviews/korg-nts-1
- logue SDK（NTS-1 `userosc.h`）：https://korginc.github.io/logue-sdk/ref/nutekt-digital/v1.1-0/html/userosc_8h_source.html
- Perfect Circuit 商品頁（reverb 列表）：https://www.perfectcircuit.com/korg-nts-1.html
- NTS-1 mkII 手冊（交叉比對用）：https://www.cavallimusica.com/media/productattach/n/t/nts-1_digital_kit_mkii_om_e1.pdf
