# 引き継ぎ元の詳細仕様（取得できた本文）

参照: 音量閾値ピッチ制御設計 / 6ab0a72c-b8fc-83e8-befd-2abd55c6e60c

read_thread の1メッセージ20,000文字上限により94節途中で取得が終了。取得できた本文を要約せず保存。実装上の確定事項・追加のユーザー必須方針・検証結果は VOICE_QUALITY_DYNAMICS_DESIGN.md を参照。

# VoxMorph Voice Quality Dynamics 再設計・実装指示

## 0. この指示の目的

VoxMorphに現在実装されている「Adaptive Voice Dynamics」を再設計し、ユーザーが明示的に調整できる以下の統合システムへ置き換える。

1. **Parametric / Multiband EQ**
2. **各EQ Bandに対するDynamic EQ / Compression**
3. **入力音量に応じたDynamic Pitch**
4. 上記を統合した**グラフ型VOICE QUALITY UI**

今回の最大の方針は、

> 「自動で声を解析していい感じに補正する機能」ではなく、  
> 一般的なEQ・コンプレッサーと同じように、ユーザーがThresholdやGain等を明示的に設定するシステムにする。

ことである。

現在のAdaptive Voice Dynamicsの

- 数秒間の通常声学習
- Vocal Effort推定
- 自動Tilt補正
- 自動Body/Presence補正
- 1つのAmountだけで全処理を決定

という設計思想は今回の目的と異なるため、廃止する。

ただし現在実装済みの、

- 入力時刻基準のControl Ring
- PSOLAのレイテンシを考慮した制御信号同期
- 検出用音声と変換出力音声を分離する設計

は有用なので再利用する。

---

# 1. 最重要：別機能を絶対に混同しないこと

今回実装するDynamicsと、既存の**大声時のF0/Formant Tracking Error回避機能**は完全に別機能として扱うこと。

以下を混同しない。

## A. Tracking Protection

目的：

- 大声
- 急激な発声
- 高入力レベル
- その他解析困難条件

によってPitch/Formant Trackingが壊れることを防ぐ。

これは**解析精度・変換エンジン保護**の機能。

## B. Voice Quality Dynamics

目的：

入力音量・周波数帯レベルを検出し、

- EQ Gain
- Dynamic Gain Reduction
- Dynamic Pitch

をユーザー設定に従って変化させる。

これは**音作り・声質補正**の機能。

### 禁止事項

Tracking Protectionの状態をVoice Quality Dynamicsの発動条件として使わない。

Voice Quality DynamicsがTracking Protectionの処理内容を変更しない。

Dynamic Pitchの値をFormant Tracking Error回避へ利用しない。

Formant Tracking Error回避処理をDynamics UIに統合しない。

---

# 2. 検出音声と変換音声の分離

既存方針を維持する。

概念的には、

```text
Input
 ↓
Pre FX
 ↓
Gate
 ↓
 ├─────────────────────────────┐
 │                             │
 │ Detection Bus               │ Conversion Bus
 │                             │
 │ Fullband Detector           │ Engine Protection
 │ Band Detectors              │ ↓
 │ ↓                           │ TD-PSOLA
 │ Control Signal              │ ↓
 │                             │ Restore
 └───────────────→ controls →  │ ↓
                               │ EQ / Dynamics
                               │ ↓
                               │ Output
```

とする。

重要：

**Detection Busの音声そのものを変換出力へ戻さない。**

Detection BusからConversion Busへ渡すのは制御値だけ。

例：

```cpp
fullLevelDb
bandLevelDb[band]
bandGainReductionDb[band]
dynamicPitchSemi
```

---

# 3. 現在のコードで確認済みの関連箇所

現行mainでは主に以下が関係する。

```text
dsp/AdaptiveVoiceDynamics.h
dsp/VocalEffortEstimator.h
dsp/ProtectionGain.h

dsp/PsolaEngine.h

src/PluginProcessor.cpp
src/PluginProcessor.h
src/PluginEditor.h
```

現在、

```cpp
VocalEffortEstimator vecEst;
AvdControl           vecCtl;
```

がProcessorに存在している。

Adaptive Voice Dynamicsは、

```text
Estimator
 ↓
AvdControl
 ↓
AvdView
 ↓
PsolaEngine
 ↓
AvdFilter
```

という構造。

このうち**Control Ringによる時間同期の思想は残す。**

---

# 4. 廃止対象

## VocalEffortEstimator

現在の以下の処理は今回不要。

- 約2秒のbaseline warmup
- 約10秒のbaseline tracking
- vocal effort estimation
- spectral tilt比較
- crest factor比較
- body excess
- presence excess
- warm/confidence表示

これらを新Dynamicsの基本ロジックとして使用しない。

必要ならLegacyコードとして一時的に残してもよいが、新Voice Qualityからは参照しない。

最終的には未使用なら削除対象。

---

# 5. 新しいシステム名

内部名は例えば、

```text
VoiceQualityDynamics
VoiceQualityControl
VoiceQualityDetector
DynamicEqProcessor
```

など。

`AVD`、`vec` という名前は旧設計を示しており、今回意味が大きく変わるため、新コードではなるべく使わないこと。

---

# 6. EQ基本仕様

今回のVOICE QUALITYはParametric EQを中心とする。

最低4 Band。

推奨初期構成：

```text
Band 1 : LOW
Band 2 : BODY
Band 3 : MID
Band 4 : PRESENCE
```

初期中心周波数例：

```text
LOW       120 Hz
BODY      400 Hz
MID      1200 Hz
PRESENCE 3500 Hz
```

これは固定帯域ではない。

ユーザーがFrequency/Q/Gainを変更できる**Parametric EQ**とする。

---

# 7. EQ Bandパラメータ

各Bandに以下を持たせる。

```text
Enabled
Filter Type
Frequency
Gain
Q
```

Filter Typeは初期実装では、

```text
Bell
Low Shelf
High Shelf
```

程度でよい。

必要なら後から、

```text
High Pass
Low Pass
Notch
```

を追加可能。

推奨レンジ：

```text
Frequency
20 Hz ～ 20 kHz
対数スケール

Gain
-18 ～ +18 dB
初期値 0 dB

Q
0.2 ～ 10
初期値 0.7～1.0程度
```

---

# 8. Softness / Tilt廃止

現在VOICE QUALITYにある、

```text
Softness / Tilt
parameter id: "tilt"
```

はUIから廃止する。

理由：

Tiltは実質的に、

```text
Low vs High
```

を一軸で動かすEQ処理であり、新Parametric EQでより柔軟に代替できる。

---

# 9. 旧Tilt DSP

現在PsolaEngine内部には、

```cpp
gLow
gHigh
tiltLp
tiltK
```

によるTilt処理が存在する。

最終的には通常VOICE QUALITY処理としては使わない。

ただし以下に注意。

### Parameter ID `"tilt"` は即削除しない

DAW automation、旧Session、Presetとの互換性問題があるため、

```text
"tilt"
```

自体はLegacy Parameterとして残す。

新規UIでは非表示。

DSPから完全に外す場合は旧Session migrationを実装する。

---

# 10. Tilt migration

旧SessionでTilt値が0以外の場合、

概念的に新EQへ変換する。

例：

```text
Tilt +X

Low Shelf  +X/2
High Shelf -X/2
```

または現在のTilt DSP特性により近い変換を実装する。

最初から完璧な周波数一致は不要。

重要なのは、

- 旧Sessionが破壊されない
- 極端に違う音にならない
- 新Sessionでは旧Tiltを使わない

こと。

Migrationの複雑度が高い場合は、

Legacy `"tilt"` を読み込んだ場合だけ旧Tilt DSPを有効にし、新UIから作る設定では0固定にする方式でもよい。

---

# 11. Dynamicsの考え方

各EQ BandにDynamic機能を追加する。

これは一般的な「Multiband Compressor」よりも、

**Dynamic EQ**

に近い。

例えばBODY Band：

```text
Frequency 420 Hz
Gain      -1.0 dB
Q          0.8
```

に対して、

```text
Threshold -18 dB
Ratio       2:1
Attack     25 ms
Release   180 ms
Knee        6 dB
```

を追加する。

通常時：

```text
-1.0 dB
```

大声時：

```text
-1.0 dB
 ↓
-2.0 dB
 ↓
-3.0 dB
```

のように追加Gain Reductionが掛かる。

---

# 12. Dynamics Bandパラメータ

各EQ Bandに、

```text
Dynamics Enabled
Threshold
Ratio
Attack
Release
Knee
Max Reduction
```

を持たせる。

推奨レンジ：

```text
Threshold
-60 ～ 0 dBFS

Ratio
1:1 ～ 10:1
必要なら∞:1

Attack
1 ～ 200 ms

Release
20 ～ 1000 ms

Knee
0 ～ 12 dB

Max Reduction
0 ～ 18 dB
```

---

# 13. Dynamic Gain計算

通常コンプレッサーと同じ考え方にする。

Hard kneeの簡易形：

```cpp
if (levelDb <= thresholdDb)
    gainReductionDb = 0;
else
{
    excess = levelDb - thresholdDb;
    compressedExcess = excess / ratio;
    gainReductionDb = excess - compressedExcess;
}
```

結果は負Gainとして使う。

例：

```text
Threshold = -18 dB
Input = -12 dB
Excess = 6 dB
Ratio = 2:1

Output excess = 3 dB
GR = 3 dB
```

---

# 14. Soft Knee

Threshold付近でGain Reductionが急に変わらないようSoft Kneeを実装する。

Knee = 0の場合はHard Knee。

Knee > 0ではThreshold ± Knee/2付近を滑らかに補間。

一般的なCompressor曲線でよい。

---

# 15. Detector

Dynamics Detectorは変換前音声を使用する。

位置：

```text
Pre FX
 ↓
Gate
 ↓
Detector
 ↓
Engine Protection
 ↓
PSOLA
```

現在のVocalEffortEstimatorと同様、

**Protection Gainより前**

で測定する。

理由：

Protection Gainによる人工的なレベル変更をDynamics Detectorに見せないため。

---

# 16. Band Detector

各EQ BandのDynamicsは、対応Band付近のエネルギーを測る。

EQフィルターそのものをDetectorへそのまま使ってもよいが、Audio Filter StateとDetector Filter Stateは分離すること。

Stereo時：

L/RのBand Energyを平均するか、最大値を使用する。

Gain ReductionはStereo Linkedとし、

**左右で別々のGRを掛けない。**

ステレオ定位が動くため。

---

# 17. Detector level方式

基本はPeakではなくRMS/short-term powerを使用する。

概念：

```cpp
energy += coeff * (x*x - energy);
levelDb = 10 * log10(energy + eps);
```

Attack/ReleaseはGain Reduction側にも別途持つ。

Transientだけで激しく動かないこと。

---

# 18. EQ Audio処理位置

新EQ/Dynamic EQは、

```text
PSOLA
 ↓
Protection Gain Restore
 ↓
Voice Quality EQ / Dynamics
 ↓
Mute
 ↓
Output Gain
 ↓
Spatial
 ↓
Post FX
```

に置く。

重要：

**Protection Restoreより後。**

先にDynamic EQを掛け、その後Protection RestoreするとGain Reductionの意味が変わるため。

---

# 19. EQ処理対象

EQは**変換後のwet/output音声**へ適用する。

Detectionは変換前。

つまり、

```text
Input Body Level
      ↓
Body GR Control

Converted Output Body
      ↓
GR適用
```

とする。

これが今回意図する構造。

---

# 20. Filter実装

リアルタイム・低レイテンシ優先。

Parametric EQは通常のBiquadでよい。

以下を避ける。

```text
Linear Phase FIR
FFT EQ
追加lookahead
```

今回のEQ自体でレイテンシを増加させないこと。

---

# 21. Filter coefficient更新

Frequency / Q / GainをUI操作した際のzipper noiseを防止。

方法候補：

- parameter smoothing
- coefficient interpolation
- 10～50 ms程度のparameter glide

急なBiquad coefficient jumpによる不安定性を避ける。

GainのみならdB値をsmoothして再計算でもよい。

Frequency/QのAutomationも考慮する。

---

# 22. Dynamic EQ Gain適用

Dynamic GainとStatic Gainを合成する。

例：

```cpp
effectiveGainDb =
    staticGainDb
  + dynamicGainDb;
```

Dynamic Gainは通常0以下。

```text
static  -1 dB
dynamic -2 dB

effective -3 dB
```

とする。

---

# 23. Makeup Gainは当面不要

一般的なMultiband CompressorにはMakeupがあるが、今回EQ Gainそのものが存在する。

そのため最初の実装では、

```text
Static EQ Gain
+
Dynamic Reduction
```

で十分。

別Makeup Gainは不要。

必要なら将来追加。

---

# 24. Dynamic Pitch

EQとは別処理だが同じVoice Quality Dynamics内に実装。

目的：

入力音量がThresholdを超えた際にPitch Shift量へ小さなOffsetを加える。

例：

```text
Base Pitch +9 st

Dynamic Pitch:
Threshold -18 dB
Range      12 dB
Amount    +0.4 st
```

入力が大きくなるにつれ、

```text
+9.00
+9.10
+9.20
+9.30
+9.40
```

と変化する。

---

# 25. Dynamic Pitchパラメータ

```text
Enabled
Threshold
Range
Amount
Attack
Release
```

推奨範囲：

```text
Threshold
-60 ～ 0 dBFS

Range
1 ～ 30 dB

Amount
-2.0 ～ +2.0 semitone
```

通常利用では±0.1～0.5程度を想定。

レンジを広めに取る。

```text
Attack
5 ～ 300 ms

Release
20 ～ 1000 ms
```

---

# 26. Dynamic Pitch mapping

基本：

```cpp
x = (levelDb - thresholdDb) / rangeDb;
x = clamp(x, 0, 1);
```

そのまま線形でもよいが、Threshold付近の不自然さを抑えるためsmoothstep推奨。

```cpp
x = x * x * (3 - 2 * x);
```

その後Attack/Release smoothing。

```cpp
dynamicPitchSemi = smoothedX * amountSemi;
```

---

# 27. Dynamic Pitch Detector

Full-band input levelを使用。

各EQ Band Detectorとは別。

```text
Full-band RMS
 ↓
Dynamic Pitch
```

初期実装ではDetector Source選択、

```text
Full
Low
Body
Mid
High
```

などは不要。

将来拡張可能にしておく程度でよい。

---

# 28. Dynamic Pitchの時間同期

ここが非常に重要。

現在PsolaEngineには約、

```text
Normal      ~43 ms
Low Latency ~21 ms
```

のlookaheadがある。

現在Blockの音量をそのまま、

```cpp
p.pitchSemi = base + dynamicPitch;
```

に足してはいけない。

異なる音節へ補正が掛かる可能性がある。

---

# 29. Control Ringを再利用

現行Adaptive Voice Dynamicsの、

```text
input stream time
 ↓
control ring
 ↓
PsolaEngineがn-Dに対応して読む
```

という構造を再利用する。

新Control View例：

```cpp
struct VoiceQualityView
{
    const float* dynamicPitchSemi;

    const float* bandGain0;
    const float* bandGain1;
    const float* bandGain2;
    const float* bandGain3;

    ...
};
```

ただしEQ Gain ReductionについてはProcessor後段で適用するため、必ずしもEngineへ渡す必要はない。

Control Ringの主な用途は、

```text
Dynamic Pitch
```

および必要ならoutput側Dynamicsの時間整合。

---

# 30. Dynamic PitchをPSOLAのどこへ適用するか

現在Pitchは、

```cpp
pitchRatio = pow(2, pitchSemi / 12);
```

から始まり、

High RangeやIntonationが適用される。

Dynamic Pitchは通常Pitch knobそのものへ混ぜず、

**最終的なPitch compensationとして加える。**

推奨順：

```text
Input F0
 ↓
Base Pitch Shift
 ↓
High Range
 ↓
Intonation
 ↓
Dynamic Pitch
 ↓
Low Limit
 ↓
Output F0
```

理由：

Dynamic Pitch Amount +0.3 stをIntonation Rangeによって+0.45等へ増幅させたくないため。

---

# 31. Robotize時

Robotizeは一定Pitchを意図するため、

```text
Dynamic Pitch = disabled
```

とする。

Robot Pitchを音量で変化させない。

---

# 32. Dynamic Pitch smoothing

Pitch modulationはGain Compressionより遅めの挙動がよい。

初期Default候補：

```text
Attack  30～60 ms
Release 150～300 ms
```

高速すぎると、

- warble
- vibrato的揺れ
- ロボット感
- ピッチのflutter

が出るため。

---

# 33. Dynamic PitchとPitch Trackingを混同しない

Dynamic Pitchは、

```text
入力Pitch Detectorの結果を修正するものではない。
```

あくまで、

```text
Output Pitch Shift AmountへのOffset
```

として作用する。

F0 Trackerそのものへfeedbackしない。

---

# 34. Formant Dynamicsは今回実装しない

将来的に、

```text
Dynamic Formant
```

を追加可能な設計にはしてよい。

しかし今回の実装対象にはしない。

Tracking Protectionとの混乱を避けるため。

---

# 35. UI全体方針

現在VOICE QUALITYにある単純なSlider中心UIから、

**EQ Graph中心UI**

へ変更する。

参考イメージ：

一般的なDAW / EQ pluginの、

```text
Spectrum
+
Parametric EQ Curve
+
Dynamic Range
```

型。

---

# 36. Graph表示

横軸：

```text
20 Hz ～ 20 kHz
Log Scale
```

縦軸：

```text
EQ Gain
-18 ～ +18 dB
```

推奨目盛：

```text
+18
+12
 +6
  0
 -6
-12
-18
```

---

# 37. EQ Node

各Bandを円形Nodeで表示。

例：

```text
●1
●2
●3
●4
```

操作：

```text
横ドラッグ
Frequency

縦ドラッグ
Gain
```

Qは、

- Mouse Wheel
- Shift+Drag
- 選択後下部Control

のいずれか。

少なくとも下部Controlでは必ず直接入力可能にする。

---

# 38. Selected Band UI

Bandをクリックした際、Graph下部にEditorを表示。

例：

```text
BAND 2 · BODY

TYPE        BELL
FREQ        420 Hz
GAIN       -1.2 dB
Q           0.85

DYNAMICS
ENABLE      ON
THRESHOLD  -18 dB
RATIO       2.0 : 1
ATTACK      25 ms
RELEASE    180 ms
KNEE         6 dB
MAX GR       6 dB
```

---

# 39. Static EQ Curve

通常EQカーブを明瞭な実線で表示。

各BandのFilter Responseを合成した最終カーブを描画する。

---

# 40. Dynamic Range表示

Dynamics Enabled Bandについて、

現在Gain ReductionによってEQカーブがどこまで動いているか視覚化する。

最低限、

```text
Static EQ curve
Current effective curve
```

の2本を表示。

さらに可能なら、

```text
maximum dynamic range
```

を半透明領域として表示。

例：

```text
Static Curve    実線
Current Curve   明るい別線
Possible Range  半透明帯
```

---

# 41. Gain Reduction visualization

各Nodeに、

- リング
- Halo
- 外周ゲージ

などで現在のGain Reduction量を表示するとよい。

例：

```text
GR 0 dB
普通のNode

GR -4 dB
Node周囲リングが強く表示
```

---

# 42. Spectrum表示

Graph背景にリアルタイムSpectrumを表示。

既存Visualizer処理を可能な限り流用する。

候補：

```text
Input Spectrum
Output Spectrum
```

Input = blue系
Output = pink系

ただしGraph UIが見にくくならないよう低Alpha。

Spectrum表示ON/OFF切替があってもよい。

---

# 43. EQ CurveとSpectrumのレイヤー順

推奨：

```text
Background
Grid
Spectrum
Dynamic range shading
Static EQ curve
Current EQ curve
Nodes
Labels
```

---

# 44. ThresholdのGraph表現

EQ Graphの縦軸はGain dBなので、

Dynamics Threshold(dBFS)を同じ縦軸へ直接描かない。

単位が違うため。

ThresholdはSelected Band controlで表示。

必要ならNodeのリングや小Meterで現在Level vs Thresholdを表示する。

---

# 45. Dynamic Pitch UI

Dynamic PitchはEQ Bandではないため、EQ Graph Nodeとして表示しない。

Graph下部に独立セクション。

例：

```text
DYNAMIC PITCH

ENABLE       ON
THRESHOLD   -18 dB
RANGE        12 dB
AMOUNT      +0.35 st
ATTACK       40 ms
RELEASE     200 ms

CURRENT     +0.18 st
```

---

# 46. UI名称

VOICE QUALITYの中に、

```text
EQ / DYNAMICS
```

を設ける。

「Adaptive Voice Dynamics」という旧名称はUIから撤去する。

---

# 47. 初期Default

新規Sessionで音を変えないこと。

全Band：

```text
Gain = 0 dB
Dynamics = OFF
```

Dynamic Pitch：

```text
OFF
```

つまり初期状態で完全Neutral。

---

# 48. 旧Session

旧Sessionに、

```text
vecenabled
vecamount
```

が存在しても、新システムへ自動変換する必要はない。

意味が全く異なるため。

旧Adaptive Voice Dynamicsは、

```text
legacy unsupported
```

としてOFF扱いでもよい。

ただしCrash、missing parameter、state corruptionを起こさないこと。

---

# 49. Parameter ID

既存Parameter IDを別の意味へ使い回さない。

例えば、

```text
vecenabled
vecamount
```

をDynamic EQへ再利用しない。

新しい明確なIDを作る。

例：

```text
vqb1_on
vqb1_type
vqb1_freq
vqb1_gain
vqb1_q

vqb1_dyn
vqb1_thr
vqb1_ratio
vqb1_atk
vqb1_rel
vqb1_knee
vqb1_maxgr
```

Band 2～4も同様。

Dynamic Pitch：

```text
vqdp_on
vqdp_thr
vqdp_range
vqdp_amt
vqdp_atk
vqdp_rel
```

名称は必要ならコード規約へ合わせて変更可。

---

# 50. APVTS順序

既存Automation互換性を壊さないため、新Parameterは現在Layout末尾へ追加。

既存Parameterの順序を変更しない。

---

# 51. UI Undo / Lock

既存のParameter Lock / Undo systemとの互換性を維持する。

新EQ Parameterも通常Parameterとして、

- Undo
- Reset
- Lock
- Preset Save/Load

へ参加させる。

Graph drag操作もUndo Historyへ適切に登録する。

1回のdragを数百Undo stepにしないこと。

MouseDown → begin gesture  
MouseUp → end gesture

として1操作扱いにする。

---

# 52. Reset

Band Reset：

```text
Gain 0
Q default
Frequency default
Dynamics OFF
```

全体Reset：

全Band Neutral + Dynamic Pitch OFF。

---

# 53. Preset

新Voice Quality設定をPresetへ保存する。

Band：

```text
type
freq
gain
Q
dynamics params
```

Dynamic Pitchも保存。

---

# 54. Stereo

Detector：

Stereo linked。

EQ/Dynamics：

同じfilter parametersとGRをL/Rへ適用。

左右で別GRは禁止。

---

# 55. Mono

従来通り1 Channel。

---

# 56. Latency

新EQ/Dynamics処理によって追加Latencyを発生させない。

Control smoothingはLatencyとしてhostへ報告しない。

FIR/Lookahead Compressorは使わない。

---

# 57. Performance

4Band × Stereoでもリアルタイム処理が軽いこと。

可能なら処理を、

```text
Dynamics OFF + Gain 0
```

Bandでは完全Bypass。

全Band NeutralならEQ処理自体をskip。

Dynamic Pitch OFFならDetector Full-band処理もskip可能。

---

# 58. NaN / Inf

すべてのDetector、Filter、Gain calculationでNaN/Infを出力へ伝播させない。

既存コードと同じ方針。

異常値時はNeutralへフォールバック。

---

# 59. Sample Rate

44.1 / 48 / 88.2 / 96 kHzで動作。

Filter frequencyはNyquist以下へclamp。

---

# 60. Host Buffer Size

32 / 64 / 128 / 256 / 512 / 1024等で、

Dynamics挙動が大きく変わらないこと。

Time constantはBlock単位ではなくSample Rate基準で計算。

---

# 61. Dynamic Pitch control cadence

Block size依存にしない。

既存AVDと同様、

```text
sample stream time
```

基準で同期。

必要なら32 sample程度のcontrol cadenceでもよい。

ただしControl Ringへは補間された値を書き込む。

---

# 62. Dynamic EQ control timing

Detectionはinput time。

出力側はPSOLA latency分遅れる。

そのためGRも、可能ならDynamic Pitchと同様に**input stream time基準で保存し、output sampleが対応する入力時刻のGRを読む。**

現在のProtection Gain Restoreと同様の考え方。

これにより大声の母音と、その大声から生成されたOutputのDynamics処理が一致する。

---

# 63. Dynamic EQ同期の推奨

Processorに、

```text
VoiceQualityControlRing
```

を持ち、

```text
input t
band GR
dynamic pitch
```

を保存。

PSOLA Output後に、

```text
output sample t corresponds to input t-D
```

としてGRを読む。

これは現在ProtectionGainが既に行っている考え方と同じ。

---

# 64. Control RingはAudio Delayではない

音声を遅らせない。

制御信号だけ保存する。

追加Audio Latency = 0。

---

# 65. ProtectionGainとの独立

現在：

```text
ProtectionGain::processPre
PSOLA
ProtectionGain::processPost
```

が存在する。

これは維持。

Voice Quality Dynamicsはその外側に独立して配置。

ProtectionGainのThreshold -7 dBFS等をDynamicsのThresholdとして使わない。

---

# 66. UI Meter

可能ならSelected Bandに、

```text
Input Band Level
Threshold
Current GR
```

を簡易Meterで表示。

例：

```text
LEVEL     -14.2 dB
THRESHOLD -18.0 dB
GR         -2.1 dB
```

---

# 67. Graph更新Rate

Audio Threadで描画しない。

UI用atomic値をPublishし、Message Thread Timerから描画。

既存Visualizerと同じ方式。

Performance Mode時は更新Rateを下げてよい。

Audio DSP結果は変えない。

---

# 68. Graph node accessibility

数値入力でも全操作可能にする。

Graph dragだけに依存しない。

---

# 69. EQ Curve計算

描画用CurveはAudio Filterを実際に流して測定しない。

Biquad coefficientからfrequency responseを計算。

UI Thread側で算出。

---

# 70. Current Dynamic Curve

現在Gain Reductionを、

```text
staticGain + currentDynamicGain
```

として描画用responseを計算。

---

# 71. Max Dynamic Range

```text
staticGain
staticGain - maxGR
```

の2 response間を塗る。

---

# 72. Dynamic EQ Bandの方向

初期実装はDownward Compressionのみ。

つまりThreshold超過時にGainを下げる。

Upward Expansion / Upward Compressionは今回不要。

将来的な拡張余地のみ残す。

---

# 73. Positive static Gain

Static Gainが+4 dBでも、

Dynamic GR -3 dBなら、

```text
effective +1 dB
```

になる。

Dynamic処理はStatic Gainを0へ戻す処理ではなく、追加Gainとして考える。

---

# 74. Max Reduction

Gain ReductionがMax Reductionを超えない。

```cpp
GR = min(GR, maxGR);
```

---

# 75. Filter state

BandをOFFにした場合、

クリック音を防ぐためGain/processingを短くglide outしてもよい。

完全OFF後はFilter state reset。

再ON時にstale stateを出さない。

---

# 76. Dynamic Pitch OFF

Dynamic PitchをOFFにした時はPitch OffsetをAttack/Releaseより短い安全なRampで0へ戻す。

突然0にしてPitch stepを作らない。

---

# 77. Base Pitch automationとの共存

Base Pitch parameterをユーザーが動かしても、

```text
Base Pitch
+
Dynamic Pitch Offset
```

として動作。

Dynamic PitchがBase Parameter自体を書き換えないこと。

APVTS値を自動操作しない。

---

# 78. UI表示値

Base Pitch knobはユーザー設定値のみ表示。

Dynamic Pitchによる実効値は、

```text
+0.18 st
```

など別表示。

---

# 79. Detection readout

既存DetectionLane等でOutput F0を表示している場合、Dynamic Pitchを反映した実際のOutput Pitchを表示する。

ただしInput detector値そのものは変更しない。

---

# 80. テスト：OFF時

最重要。

新システム全OFFで、

```text
旧処理を除いたbaseline
```

と可能な限りbit-identical。

最低限audibly identicalではなく、できれば既存bitexact testで確認。

---

# 81. テスト：EQ Flat

全Band Gain 0、Dynamics OFFで完全Neutral。

EQ Filterが有効でも0 dB時に不要処理を避ける。

---

# 82. テスト：Single Band

Sine sweep等で、

```text
Band Gain
Freq
Q
```

が期待通り。

---

# 83. テスト：Dynamics

固定Sine/Noiseレベルを変え、

Thresholdより下：

```text
GR = 0
```

Thresholdより上：

Ratio通り。

Attack / ReleaseがSample Rateによらず一致。

---

# 84. テスト：Stereo

左右異なる信号を入力し、

Gain Reductionが共通で、

Stereo balanceが移動しないこと。

---

# 85. テスト：Dynamic Pitch

一定Pitch tone/voiceをレベルだけ変化。

Threshold以下：

```text
Offset 0
```

Threshold以上：

設定Amountへ滑らかに近づく。

---

# 86. テスト：Dynamic Pitch同期

急に小声→大声となる素材を使用。

Dynamic Pitchが前後の音節ではなく、対応する大声部分へ掛かること。

Normal Latency / Low Latency両方で確認。

---

# 87. テスト：Dynamic EQ同期

同じく小声→大声素材。

Gain Reductionが対応する出力音節に一致。

PSOLA lookahead分ズレないこと。

---

# 88. テスト：Buffer Size

32 / 64 / 128 / 256 / 512 / 1024で、

- Dynamics onset
- release
- Dynamic Pitch
- GR量

がほぼ一致。

---

# 89. テスト：Sample Rate

44.1 / 48 / 96 kHz。

---

# 90. テスト：Parameter Automation

DAWから、

```text
Gain
Freq
Q
Threshold
Dynamic Pitch Amount
```

をAutomationして、

- crashしない
- NaNしない
- 激しいzipper noiseを起こさない

こと。

---

# 91. テスト：Session Migration

旧Sessionをロード。

- crashしない
- missing parameter errorなし
- tilt legacy問題なし
- vec legacy問題なし

---

# 92. 実装順序

以下の順序を推奨。

## Phase 1

Parameter追加。

まだDSPを動かさない。

## Phase 2

Static Parametric EQのみ実装。

Graph UIは最低限。

## Phase 3

Band Detector + Dynamic Gain。

Dynamic EQ完成。

## Phase 4

Control Ringの再設計。

Output Dynamics時間同期。

## Phase 5

Dynamic Pitch。

PsolaEngine内でinput-time同期。

## Phase 6

Graph UI完成。

Spectrum / Dynamic Curve / GR表示。

## Phase 7

旧Softness/Tilt UI削除。

Legacy compatibility整理。

## Phase 8

テスト・軽微修正。

---

# 93. 実装上の優先順位

優先順位：

1. 音声処理の正しさ
2. レイテンシ同期
3. OFF時Neutral
4. Crash/NaN防止
5. UI操作
6. UI装飾

見た目を作り込む前にDSPを成立させる。

---

# 94. 過剰な自動化をしない

以下は禁止。

```text
声を自動
```

（保存者注: ここで参照APIの取得上限に到達したため本文終了。上の閉じコードフェンスは表示を整えるための補足であり、原会話の追記ではない。）
