# OP-1（原版）行為參考 → TOYBOX-4 1.0 的設計依據

> 主要來源：OP-1 官方使用手冊 v2.2（56 頁，[M]）：https://medias.audiofanzine.com/files/op-1-manual-v2-2-479248.pdf ，以及官方 original 版網頁說明 https://teenage.engineering/guides/op-1/original 。
> 補充來源：OP-1 field 手冊 fw 1.7.0（[F]，只用來補原版沒有的項目）、op1hacks keys.md（[H]）、OP-Forums tips digest（[T]，使用者回報，沒有官方背書）。
> 本文件只記錄「行為」，用來重新實作互動模式；TOYBOX-4 的圖形、名稱、聲音全部是原創的。

## 1. 全域結構
- 四種模式：Synth（藍）、Drum（綠）、Tape（橘）、Mixer（灰）。按鍵符號的顏色表示它屬於哪一類。[M 4.1]
- T1–T4：在 synth / drum 裡是 引擎 / 包絡 / FX / LFO；在 tape 裡是軌 1–4；在 mixer 裡是 混音 / EQ / master FX / master out。[M 5.1]
- 1–8 號鍵：在 synth / drum 裡是 8 個音色槽（8 個合成音色、8 個鼓組）；在 tape / mixer 裡是「tape tricks」。[M 5.7, 8.12]
- shift + 模式鍵：
  - synth / drum：把音色還原成上次存檔的版本。
  - tape：清除整條磁帶（要再按 T1–T4 確認）。
  - mixer：顯示訊號路徑圖。
- shift + T：只開某一個模組的瀏覽器（換引擎時保留包絡、FX、LFO）。shift + 1–8：預設音色瀏覽器。[M 5.2, 5.7]
- 存檔：按住音色鍵 5 秒。[M 5.8]
- help：
  - 點一下：顯示目前的模式和音色。
  - 按住 help + 某鍵：顯示那個鍵的功能。
  - 按住 help 彈奏：顯示音名。[M 11]
- tempo：
  - BPM 範圍 40–220（[T]，沒有官方背書）。
  - 同步模式：free / beat match / sync / PO sync。
  - 橘色旋鈕調節拍器音高，轉到底關掉。
  - 連按可 tap tempo。[M 10]
- 硬體：24 鍵、沒有力度、6 複音、單音色（同一時間只有一個合成音色在彈）。

## 2. 合成引擎（四色旋鈕：藍 / 綠 / 白 / 橘）[M 17.1]
| 引擎 | 藍 | 綠 | 白 | 橘 | 圖形 |
|---|---|---|---|---|---|
| Cluster | 波數 0–6 | 波形包絡 | spread | unitor | 四個時鐘指針，加重疊的彩色波 |
| Digital | wave shaper | 八度 | detune + ring mod | digitalness | 彩色點連成的移動多邊形 |
| String | tension | impulse decay | detune | impulse 種類 | 3D 弦振動瀑布圖 |
| Pulse | filter | amplitude | 第二個脈波 | mod | 有 H/T 游標的脈波串 |
| FM（4 個 operator）| FM 量 | 頻率 | 拓樸（演算法）| detune | 六角形 / 立方體的 operator 格子 |
| Phase | phase shift | 失真量 | phase filter | phase tilt | 兩張 sine 圖 |
| Dr Wave（頻域）| 波形類型與長度 | filter | phase | chorus | 一排斜的頻譜線條 |
| D-Synth | 包絡交叉淡化 | 波形 | 包絡 | cross mod | OSC1/OSC2 面板；shift 一排調 osc2 |
| Voltage | ampere mod | 感應 wave shaper | phase filter | voltage detune | 電極、火花、「180V」 |
| DNA [F] | filter | 波形編號 | 波形調整 | noise | 寫著「CPU ID」的十六進位格子 |
| Sampler | start | loop in | loop out | end | 波形和 4 個彩色記號；shift：reverse、loop in/out 微調、gain |
- 換引擎時會保留包絡、FX、LFO。[M 5.2]
- T2 包絡：ADSR，正在調的那一段會變成對應旋鈕的顏色。按住 shift 是 play mode：poly / mono / legato / unison、portamento。[M 5.3–5.4]
- T3 FX、T4 LFO：各一個槽；T 鍵切換開關，shift + T 開瀏覽器。[M 5.5–5.6]

## 3. LFO [M 15]
| LFO | 藍 | 綠 | 白 | 橘 |
|---|---|---|---|---|
| Element | 來源（傾斜、mic、包絡、電平）| 量 | 目的地模組 | 目的地參數 |
| Random | 量 | 速度 | 目的地 | 包絡 |
| Tremolo | 速度 | 音高量 ± | 音量量 ± | 包絡（shift + 橘：形狀 sine / saw / exp / square / blip）|
| Value | 速度 | 量 | 目的地模組 | 目的地參數（用顏色選）|
| MIDI | 把 4 個 CC 對應到目的地 | | | |
- 選目標的方式：先選模組（引擎 / 包絡 / FX），再選那個模組 4 個顏色參數中的一個（暗色 = shift 那一排的參數）。目標的圖形會在它自己的頁面上跟著動。
- 跟 tempo 同步時，LFO 的時鐘顯示數字；自由運行時顯示手的圖示。

## 4. 效果 [M 17.2]（同一套效果可以當音色 FX，也可以當立體聲 master FX；shift = 微調）
| FX | 藍 | 綠 | 白 | 橘 | 圖形 |
|---|---|---|---|---|---|
| Delay | size | speed | feedback | mix | 雷達同心圓 |
| Grid（3D plate reverb）| X | Y | Z feedback | mix | 3D 方塊 |
| Nitro（雙共振濾波器）| freq | follow | reso | freq 2 | 儀表板 |
| Phone（lo-fi）| tone | phonic | baud | telematic | 分解的電話 |
| Punch（低通 + 壓縮）| freq | punch | rounds | power | 拳擊手 |
| Spring | tone | turns | damping | mix | 彈簧 |
| CWO（移調 delay）| freq | delay | feedback | sideband | 乳牛 |
| Mother [F]（reverb）| distance | gate | color | mix | 走廊裡的人 |
| Terminal [F]（bitcrush）| rate | bits | model | mix | 終端機 |

## 5. 鼓 [M 6]
- 鼓取樣器：一個樣本最長 12 秒；24 個鍵各有自己的切片（in / out）。
- 每鍵參數：
  - 主：藍 pitch、綠 in、白 out、橘 play mode（播到底 / loop / 播一次）
  - shift：方向、in 微調、out 微調、level
- 複製切片：按住來源鍵 + lift，再按住目標鍵 + drop。
- 動態包絡：attack level / mid / release / timing。
- D-Box：雙振盪器鼓合成器（pitch / 波形 / 包絡 / cross mod；shift 一排調 osc2，橘色變 filter）。

## 6. 取樣 [M 12]
- 選一個 sampler 引擎 → shift + mic 選來源（line、mic、FM 收音機、out-to-in 重錄自己的輸出）→ 按住任一鍵就開始錄。合成取樣器最長 6 秒，鼓取樣器最長 12 秒。
- 也可以把 lift 起來的磁帶段落 drop 進 sampler。

## 7. Sequencer [M 7]
- synth 和 drum 各有自己的 sequencer 記憶和類型；sequencer 存的是音符，不是音訊；彈鍵盤會移調（C = 原音高）。
- 跟 tempo 同步；在 1/16 時，16 步 = 磁帶上 1 小節。

| Sequencer | 藍 | 綠 | 白 | 橘 | shift | 行為 |
|---|---|---|---|---|---|---|
| Endless（128 步）| 拍子（含三連音）| swing | trigger pattern | hold | crank / rotate / 方向 | shift + 鍵輸入一步；shift + > 長音 / 休止；shift + < 刪除；大數字顯示步數 |
| Pattern（16 步）| 游標 | swing | 長度 | start / hold | 清除 / rotate / 播放區段 / 方向 | 格子；live 模式時有掃描線 |
| Tombola | 速度與方向 | 重力 | 形狀（打開六角形）| 質量 / 彈性 | crank | 音符掉進旋轉的六角形，撞到牆就發聲 |
| Finger（32 步）| 游標 | swing | 長度 | hold | 清除 / 模式（join / replace / fill）| 每個白鍵一個 pattern，按住就播 |
| Sketch | 畫 X | 畫 Y | 游標 X | 游標 Y | 清除 / 速度 / 格線 / start | 畫線，高度 = 音高 |
| Arpeggio | 拍子 | trigger 模式 | trigger pattern | hold | 音長、type（八度上、上下、+7 +9 −5、2 4 5 7 9）| 畫在五線譜上 |
| Hold [F] | 分割點 | mono / poly | transpose | hold | | latch |

## 8. 磁帶 [M 8]
- 4 軌，正常速度下 6 分鐘；錄音一律 overdub。
- 白色旋鈕 = 變速（shift = 固定檔位）；shift + play = 倒轉；藍色 = scrub。
- 編輯：
  - lift = 剪下，drop = 在磁頭處貼上，split = 在磁頭處切開，shift + split = 合併相鄰段落。
  - shift + lift = 四軌一起剪；搭配 loop in / out 可以剪一個區段。
- tape tricks（1–8 號鍵）：
  - 1 loop in、2 loop out、3 loop on / off
  - 4 break（停下但 loop 在背景照樣計時）
  - 5 reverse、6 chop（跟 tempo 鎖定的重複）
  - 7 / 8 memo M1 / M2（按住並轉旋鈕記住參數，再按一下叫回）
- beat match 時顯示小節記號；shift + < / > 跳一小節。
- shift + T：靜音那一軌。原版沒有 undo。

## 9. 混音 [M 9]
- T1：4 軌 level（藍綠白橘），shift = pan。
- T2：3 段 EQ（低 / 中 / 高）+ 量（最左 = CLEAN）。
- T3：master FX（同一套效果，立體聲）。
- T4：master out（L / R 電平、drive 壓縮 / 限幅、release）。
- shift + mixer：訊號路徑圖（synth / drum → FX → tape 1–4 → mixer → EQ → master FX → drive → out）。

## 10. 讓 OP-1 好玩的互動細節
- 顏色 = 旋鈕：所有畫面都用旋鈕的顏色畫參數，動到的元素會變色或亮起。
- shift 是一整層：微調，或第二排參數（在 LFO 的目標選擇裡用暗色表示）。
- 不用存檔提示：狀態在斷電後保留；shift + 模式鍵可以還原。
- 具象的視覺隱喻：
  - 捲盤和磁頭、唱盤、拳擊手、乳牛、儀表板、電話、彈簧、3D 方塊、雷達
  - 會轉的六角形和彈跳的球、彈鍵盤的卡通人物、巨大的步數數字、五線譜
- help 泡泡疊在畫面上；按住 help 彈奏時顯示音名。
