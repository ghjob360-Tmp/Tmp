# Vital 的系統架構與互動模式（PRISM-1 1.0 的設計依據）

這份文件整理 Vital（Matt Tytel，GPL-3.0）的聲音架構與介面互動，依據的是它的原始碼（`src/synthesis`、`src/common/synth_parameters.cpp`、`src/interface`），以及使用手冊對各區塊的說明。每一節最後一欄寫 PRISM-1 怎麼對應。

## 1. 訊號流程

```
          ┌ OSC 1 ┐                       ┌ FILTER 1 ┐
 voice ─► ├ OSC 2 ┼─ 每個來源各自選去處：  ├ FILTER 2 ┤（串聯：F1 → F2；或並聯）─► ×AMP ENV ─┐
          ├ OSC 3 ┤   F1 / F2 / F1+F2 /    └──────────┘                                      │
          └ SMP   ┘   EFFECTS（略過濾波器）/ DIRECT OUT（連效果也略過）                       │
                                                                                               ▼
 全部 voice 加總 ─► 效果鏈（9 種，可拖曳排序、各自開關）─► 輸出音量 ─► 限幅
```

| Vital | PRISM-1 |
|---|---|
| 1–32 voices，可 2× oversampling | 8 voices，兩顆核心各算一半；不 oversample |
| 每個振盪器 / sampler 的「destination」：Filter 1、Filter 2、Filter 1+2、Effects、Direct Out | 相同（`OUT` 參數）|
| 濾波器串聯或並聯（filter 1 的「→ filter 2」開關）| `ROUTING`：PARALLEL / SERIAL |
| 音量律：ENV 1 × 力度（velocity tracking）再平方 | 相同 |

## 2. 振盪器（`synth_oscillator.cpp`）

| 參數 | Vital 的意思 | PRISM-1 |
|---|---|---|
| Wavetable / frame | 每張表最多 256 幀、每幀 2048 sample；frame position 在幀之間內插 | 32 張出廠表 × 32 幀，每幀 6 層 mip（256→8 sample）；幀之間內插 |
| Spectral morph | 12 種：vocode、formant scale、harmonic stretch、inharmonic stretch、smear、random amplitudes、low pass、high pass、phase disperse、shepard tone、skew，每個 voice 每個 block 用 FFT 重建 | 同 12 種，但**整張表一起套**（主迴圈分步重建）：FM-1 沒有每個 voice 做 FFT 的餘裕，所以 morph 量不能被 voice 調變 |
| Distortion（相位 warp）| sync、formant、quantize、bend、squeeze、pulse width、FM ← osc / sample、RM ← osc / sample | 全部 13 種，公式照 Vital |
| Unison | 1–16 voices、detune、detune power、blend（中央 vs 兩側）、stereo spread、frame spread、stack style（11 種：normal、center drop octave、octave、power chord、major、minor、harmonic series…）| 1–8 voices，其餘全部照做；另外有全域的 unison 預算，CPU 不夠時自動收 |
| Phase / random phase | 每個音從哪個相位開始、隨機程度 | 相同 |
| Level（平方）、pan、transpose、tune、keytrack | | 相同 |

Sampler（`sample_source.cpp`）：一個 sample，可 keytrack、loop、random start、自己的 level / pan / destination。PRISM-1 的出廠素材是開機時算出來的噪音和短 loop，白、粉紅、棕噪音直接即時產生。

## 3. 濾波器（`synthesis/filters`）

| Model | Styles | 備註 |
|---|---|---|
| Analog（Sallen-Key）| 12 dB、24 dB、notch pass、notch swap、dual notch band | BLEND：low → band → high 連續 |
| Dirty | 同上 | 會飽和的版本 |
| Ladder | 同上 | 4 極 |
| Digital（SVF）| 同上 | 乾淨 |
| Diode | low shelf、low cut | |
| Formant | AOIE、AIUO | X / Y 是母音平面，另有 formant transpose |
| Comb | low high / low band / band spread × comb / flange+ / flange− | cutoff 決定延遲，spread 決定頻帶 |
| Phaser | positive、negative | |

共同參數：cutoff（MIDI note 8–136）、resonance、drive、blend、keytrack、mix。PRISM-1 全部照做，兩個濾波器 × 8 voices。

## 4. 調變源

| 來源 | Vital | PRISM-1 |
|---|---|---|
| Envelope | 6 個 DAHDSR；時間是 value⁴ 秒（最長約 32 s）；attack / decay / release 各自有 power 曲線；ENV 1 固定接音量 | 相同 |
| LFO | 8 個；形狀是可編輯的點加每段曲線；sync 模式：trigger、sync（全部 voice 同相）、envelope（跑一次）、sustain envelope（停在 loop 點等放開）、loop point、loop hold；速度 free（Hz）或拍子；fade in、delay、smooth、phase | 8 個；9 種預設形狀加一個可畫的 16 點形狀（每段有曲線）；6 種模式全做；fade / delay / smooth / phase / amount |
| Random LFO | 4 個：perlin、sample & hold、sine interpolate、lorenz attractor；free 或 trigger | 相同 |
| Macro | 4 個 | 相同 |
| 其他 | velocity、aftertouch、note、note in octave、mod wheel、pitch wheel、random（每個音一個亂數）、lift、slide、stereo | velocity、pressure（FM-1 的鍵有壓力）、note、octave note、mod wheel、pitch bend、random |

## 5. 調變矩陣（`modulation_connection_processor`）

每條連線：source → destination、amount（±，以目的參數的整個範圍為單位）、bipolar（把 0..1 的來源變成 −1..1）、power（對來源套曲線）、stereo、bypass、還可以畫 remap 曲線。Vital 最多 64 條。

PRISM-1：32 條，有 amount、bipolar、power、bypass，不做 stereo 和 remap。

**目的地**：幾乎所有連續參數都能被調變，包括效果的參數。Vital 用「最新的 voice」的調變值去推全域的效果；PRISM-1 也一樣。

## 6. 效果（`synthesis/effects`、`modules/*_module`）

預設順序：chorus → compressor → delay → distortion → equalizer → filter → flanger → phaser → reverb。介面上可以拖曳排序，每個都有開關。

| 效果 | 主要參數 |
|---|---|
| Chorus | voices、frequency / tempo、depth、delay 1 / 2、feedback、filter cutoff / spread、mix |
| Compressor | 多頻段（OTT 式，上壓加下壓）：low / band / high 的 gain 和 threshold、attack、release、mix |
| Delay | mono、stereo、ping pong、mid ping pong；時間（free 或拍子）、feedback、filter、mix |
| Distortion | soft clip、hard clip、linear fold、sine fold、bit crush、downsample；drive；濾波器放前或放後 |
| EQ | low（shelf / high pass）、band（peak / notch）、high（shelf / low pass），各有 cutoff / gain / resonance |
| Filter | 跟 voice 的濾波器同一套 model |
| Flanger | rate / tempo、feedback、center、depth、mix |
| Phaser | rate / tempo、feedback、center、depth、blend、mix |
| Reverb | size、decay、pre low / high cut、high shelf、chorus、delay、mix |

PRISM-1：9 種全做，順序可重排。延遲線用 int16 存在固定的 arena 裡，reverb 縮成較少的 FDN 線。

## 7. 介面的互動模式

| Vital | PRISM-1 在 FM-1 上 |
|---|---|
| 把調變源（ENV / LFO / RANDOM / MACRO）的圖示**拖到任一旋鈕**上，就建立連線 | 在來源的頁面按 **ARP**「撿起」它（燈會閃、畫面出現紫色標示），再轉任一旋鈕：轉的是調變量 |
| 旋鈕外圈的**紫色弧**顯示調變範圍；播放時有一個點顯示目前的值 | 每顆旋鈕外圈的紫弧，加上白色刻度標出最新 voice 聽到的值 |
| Wavetable 以 **3D 堆疊**顯示，播放的幀在前面，warp 的結果即時畫出 | 同樣的堆疊（每隔一幀畫一條），播放幀用青綠色、已套 warp；按住 GLO 改看頻譜 |
| 濾波器畫**頻率響應**，可拖曳 cutoff / resonance | 響應曲線加 cutoff 的調變範圍、voice 正在用的曲線（白）|
| 包絡 / LFO 是**可拖曳的曲線**，播放點即時跑 | 曲線加播放點；LFO 的 DRAW 列逐點改高度和曲率 |
| Matrix 分頁列出所有連線 | MATRIX 頁：7 列一頁，source、目的地、amount 條、來源即時值 |
| 效果分頁：一欄可排序的效果 | 效果鏈一列 9 格；SELECT 選效果，GLO+SELECT 移動位置 |
| Preset 瀏覽器 | PRESET 編碼器：出廠音色，接著是使用者槽（快閃，46 個）|
