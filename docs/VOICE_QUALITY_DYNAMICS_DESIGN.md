# VOICE QUALITY: Parametric EQ / Dynamics / Dynamic Pitch

実装基点: `origin/main b867461f0bba2a92c9d7aca055ada88170e2f539`
ローカルブランチ: `codex/voice-quality`。2026-09-21。

この文書は後続実装者向けの決定事項、数式、実装境界、互換性、検証方法を記録する。
会話から取得できた詳細仕様本文は `VOICE_QUALITY_CONVERSATION_REFERENCE.md` に要約せず保存した。
この文書とユーザーの15項目の必須方針を実装の基準とする。

## 目的・変更しない機能

従来の「ONとAmountだけで自動的に声質を補正する」Adaptive Voice Dynamicsを、
ユーザーがFrequency、Gain、Q、Threshold、Ratio等を直接決めるDynamic EQへ置換する。
baseline学習、Vocal Effort推定、Body/Presence excess、自動Tilt補正は新機能に持ち込まない。
`VocalEffortEstimator` / `AvdControl` / `AvdFilter` の実装と専用旧テストを削除した。
有用だった入力ストリーム時刻・制御リング・ステレオリンク・分割処理の設計を新しい名前で再実装した。

Tracking Protectionは入力F0/Formant追跡の破綻回避であり、今回の音作り機能とは独立する。
Tracking Protectionの状態をDynamics/Pitchの発動条件にしない。
Dynamic Pitch/EQの値や処理済み音声をTrackerへ戻さない。Formantの動的補正も今回の対象外。
`ProtectionGain`のprocessPre / processPost、閾値、ゲイン復元処理は維持する。

## 信号経路

```text
Input -> Pre FX -> Gate
                    |
                    +-> Detection Bus (read-only observation)
                    |    fullband RMS -> Dynamic Pitch envelope
                    |    4 independent band filters -> RMS -> downward GR
                    |                         |
                    |                  input-time control ring
                    |                [GR x 4, pitch semitones]
                    |                         |
                    +-> Conversion Bus       |
                         ProtectionGain::processPre
                         PSOLA <--------------+ pitch at source grain time c
                         ProtectionGain::processPost
                         4-band IIR EQ <------+ GR at output time t-D
                         Mute -> Output Gain -> Spatial -> Post FX -> Output
```

Detection Busの音声を出力に混ぜない。音声Delayや追加lookaheadはない。
EQの追加ホスト報告Latencyは0。Filter smoothing、RMS integration、Attack/Releaseは
意図した時定数であり追加音声Delayではない。
EQはProcessorのProtection Restore直後の変換出力に適用する。
現行エンジンのMixはエンジン内部にあるため、この地点ではMix済み信号全体をEQする。
従ってMix=0でも明示的に設定したEQは有効。Dynamic PitchはPSOLAのwet側にのみ作用する。

## ソース構成

- `dsp/VoiceQualityDynamics.h`: JUCE非依存の4 Band設定、係数生成、フィルタ、RMS、GR、Pitch、制御リング。
- `dsp/PsolaEngine.h`: 制御Viewと入力時刻原点を受け、実際の入力グレイン中心でPitch offsetを読む。
- `src/PluginProcessor.cpp/.h`: APVTS、ポインタキャッシュ、prepare/reset、音声バスの接続、UI atomic publish。
- `src/VoiceQualityEditor.h`: EQグラフ、選択Band Editor、Dynamic Pitch Editor。
- `src/PluginEditor.h`: SpectrumData/ParamRow定義後に新Editorをincludeし、VOICE QUALITYカードへ配置。
- `test/voice_quality_test.cpp`: DSP単体の同期・Neutral・Stereo・sample-rate/block検証。
- `test/voice_quality_proc_test.cpp`: JUCE ProcessorのState/Preset/Lock/Undo/Legacyと短い遷移試験。

## 54個の新規パラメータ

既存APVTSパラメータのID、version hint、登録順は維持する。
新規項目は旧末尾`vecamount`の後へ追加。旧`vec*`/`tilt`を別の意味に流用しない。
Band名は LOW / BODY / MID / PRESENCE。初期周波数以外は同一仕様。
これはクロスオーバーで固定分割・再合成するコンプレッサーではない。
全Bandを20 Hz〜20 kHzに動かせるParametric EQである。

各Bandのprefixは`vqb1_`〜`vqb4_`。

| suffix | 意味 | 範囲 | 初期値 |
|---|---|---|---|
| on | Band有効 | boolean | ON |
| type | Filter Type | Bell / Low Shelf / High Shelf | Bell |
| freq | Frequency | 20〜20000 Hz、対数的操作 | 120 / 400 / 1200 / 3500 Hz |
| gain | Static Gain | -18〜+18 dB | 0 |
| q | Q | 0.2〜10 | 0.8 |
| dyn | Dynamics有効 | boolean | OFF |
| thr | Threshold | -60〜0 dBFS | -18 |
| ratio | 圧縮比 | 1〜10 | 2 |
| atk | Attack | 1〜200 ms | 25 |
| rel | Release | 20〜1000 ms | 180 |
| knee | Knee幅 | 0〜12 dB | 6 |
| maxgr | 最大Reduction | 0〜18 dB | 6 |

Dynamic Pitch prefixは`vqdp_`。

| suffix | 意味 | 範囲 | 初期値 |
|---|---|---|---|
| on | 有効 | boolean | OFF |
| thr | Threshold | -60〜0 dBFS | -18 |
| range | 最大Amountへ達する閾値超過量 | 1〜30 dB | 12 |
| amt | Pitch offset | -2〜+2 semitones | +0.30 |
| atk | Attack | 5〜300 ms | 40 |
| rel | Release | 20〜1000 ms | 200 |

新規インスタンスはGain=0、Dynamics OFF、Pitch OFFでNeutral。
APVTSの量子化誤差によるゼロ付近の微小な値（絶対値1e-5未満）はDSPで正確な0にする。
これは微小な「0.00000085 dB」のためにバイパスを失わないため。

## Detector / Dynamics の数式

DetectorはGate後、Protectionより前の信号だけを読む。
BandごとのDetector状態と出力EQのFilter状態は完全に分離する。
Bellの検出は中心周波数でunityのbandpass、Low Shelfはlowpass、High Shelfはhighpass。
Detector Frequency/Q/Typeは選択Band設定に従う。Static GainはDetectorへ適用しない。

ステレオは `power = (L_filtered^2 + R_filtered^2)/2`。
Monoはその1chのpower。Fullbandにも同じ平均power方式を使う。
位相反転したL/Rが相殺するmono sumはDetectorに使わない。
RMS energyは10 msのone-pole:

```
k(ms) = 1 - exp(-1/(sampleRate * ms/1000))
energy += k(10) * (power - energy)
levelDb = 10*log10(max(energy, 1e-12))
```

dBFSはRMS power基準。Peak=0 dBFSの正弦波のfullband levelは約-3.01 dBFS。
Band levelはフィルター通過後のRMSなので帯域幅にも依存する。

```
x = levelDb - thresholdDb
slope = 1 - 1/ratio
hardKneeGR = slope * max(0,x)
softKneeGR = slope * (x+knee/2)^2 / (2*knee)  [ -knee/2 < x < knee/2 ]
targetGR = min(maxReduction, GR)
GR += k(targetGR > GR ? attack : release) * (targetGR-GR)
```

GRは正のReduction量として保存。下流で `effectiveGain = staticGain - GR` とする。
静的Gainが+4でGR=3なら実効Gain=+1。0に向かって戻すだけの機能ではない。
初期実装はdownward compressionのみ。Makeup Gain、Upward動作、Detector source選択は追加しない。
OFF/disabledのBandはGR targetを0にして減衰する。

## 出力EQとパラメータ変更

4つのRBJ IIR biquadを直列にする。L/Rには同じ係数とGR、独立したFilter memoryを使う。
周波数をsampleRate*0.45以下にclampし、Nyquist近傍での係数異常を避ける。
ShelfのQはpole-Q規約。high-Q shelfは共振が生じ得るため、標準値0.8から調整する。

出力のFrequency/Q/effective Gainには20 msのglideを設け、係数を再計算する。
このglideはDynamicsのAttack/Releaseに加わる短いデジッパーである。
Filter Type変更時はGainを0へfadeし、旧状態をresetしてから新Typeへfadeする。
Band OFFもGain=0へglideする。完全に0になったBandは乗算やFilterを通さず、stateをclearする。
新規Neutralでは最初から全Bandをexact bypassする。

Coefficient/state計算に非有限値が入った場合は状態をresetし、有限な入力へfallbackする。
Detector入力の非有限値は0へ、極端な振幅は±32へ制限する。
このDetector側保護はユーザー音声を出力へ戻す処理ではない。
最後に出力へ残るNaN/Infは0へ置換する。
prepare以外でリングを確保しない。Audio Threadで描画・FFT解析・UI Component更新をしない。

## Dynamic Pitch / 時間同期

Fullband RMSに対して:

```
x = clamp((levelDb - thresholdDb)/rangeDb, 0, 1)
x = x*x*(3-2*x)   // smoothstep
smoothedX += k(attack or release)*(x-smoothedX)
pitchTarget = smoothedX * amountSemitones
```

Amount automationとOFF切替には追加10 ms glideを使う。
OFF時のenvelopeも10 msで0へ戻し、十分小さい値を正確な0にsnapする。
Base PitchのAPVTS値は書き換えない。

PSOLAの適用順:

```
Base Pitch -> High Range -> Intonation -> Dynamic Pitch -> Low Limit -> 40..1000 Hz clamp
```

Pitchをhost blockの`p.pitchSemi`に足さない。
グレインを選択する`c`（GCI/peak alignment後）を先に求め、その入力時刻の制御値を読む。
`qualityTimeOffset = processorInputBase - engineWritePos`で各engineの内部時刻を
Processorの共通時刻へ変換する。Stereo Input切替で片側engineの内部時刻が違っても対応できる。
`ft *= pow(2, dynamicPitchSemi/12)` はIntonation後なので設定AmountがIntonationで再増幅されない。
Robotize branchはこの処理を通らない。Input F0/Formant tracking値は変えない。
既存Output F0 readoutはこの補正後のTsから求める。

Control ringはサンプルごとに `{ GR[4], pitch }` だけを保存。
最大lookaheadより長い0.15秒+4096sampleをprepareで確保する。
未来・負時刻・保存期限外のreadはNeutralを返す。ringには音声を保存しない。
Processorは512sample以下に分割して「検出→変換→復元→EQ」を進めるので、
巨大なhost blockで未読の制御値を上書きしない。
Normal/Low Latencyの両方で、EQは`base+i-engine.latencySamples()`のGRを読む。
Latency変更時はringを残し、出力Filter memoryをclearする。
Host reset / state load / Stereo切替は検出器・ring・出力stateをNeutralへresetする。

## UI / 操作 / 永続化

MAIN > VOICE QUALITYの旧Softness/Tiltを撤去し、EQ / Dynamics panelを配置。
AdvancedのAdaptive Voice Dynamics、Amount、Effort/warmup表示は削除。

Graphの横軸は20〜20000 Hz対数、縦軸は-18〜+18 dB EQ Gain。
Band nodeは横drag=Frequency、縦drag=Static Gain。クリックで選択Bandが切り替わる。
LOW/BODY/MID/PRESENCEのdropdownでも選択可能。
Q、Type、Dynamicsの全項目は下部の数値Editorから入力できる。
ThresholdはdBFSの独立コントロール。EQ Gain軸にThreshold線を描かない。

表示レイヤーはgrid、Input/Output Spectrum、Max Reduction領域、Static白線、Current桃色線、Node。
ResponseはMessage Threadで係数から計算。Current線はAudio Threadがatomic公開した実際の出力係数を使い、20 ms glideも反映する。Audio Filterへ測定信号を流さない。
Max領域は全有効Dynamicsが最大Reductionに達した場合のresponseとの間を塗る。
実際に全Bandが同時に最大になるという予測ではない。Node ringの太さでGRを表示する。
Readoutは入力Band level、出力時刻のGR、fullband level、同期済みPitch controlを表示する。
Pitch control表示はsample-time制御値であり、無声音中にもenvelopeの値を示し得る。
実際の変換Pitchは既存Output F0表示を参照する。Robotize中のPitch表示は0。

Spectrumは既存SpectrumDataの共通FFTを利用する。Inputは変換前、Outputは既存出力tap。
既存tapはエフェクトチェーンの表示用なので、EQ単体のtransfer measurementではない。
Spectrumには独立した-66〜+6 dBFS display scaleを使い、その旨を凡例へ表示する。
Timerは通常20 Hz、Performance Modeで描画のみ減速。DSPは変えない。

全新パラメータは既存ParamRowを使用し、数値入力、Lock、個別Reset、Preset、Undoへ参加。
Band Resetは選択Bandの全12項目をdefaultへ戻し、locked項目を維持する。
全体ResetとPreset loadは既存の全parameter loopで新パラメータも扱う。
Graph dragはmouseDownでhistory.beginGesture / APVTS beginChangeGesture、
mouseUpでendする。ドラッグ中のhistory pollingを抑止し、途中で止めてもUndoを分割しない。
Freq/Gainがlockedなら対応する軸を書き換えない。

## Legacyの選択

`vecenabled` / `vecamount`: 登録順・ID・値の保存を維持するが、DSP/UIから参照しない。
旧ONのセッションも新Dynamic EQへ変換しない。旧AVDの効果が無くなることは意図した変更。
ホストautomation一覧の名称にはLegacy / unusedを付ける。

`tilt`: 既存IDとDSPをLegacyとして保持し、通常UIから隠す。
旧セッションの非0 Tilt、旧DAW automationは従来のwet-path Tiltとして動作する。
自動で新EQへ近似変換すると音色が変わるため、今回migrationは実装しない。
新規defaultは0。新UIからはTiltを操作せずEQを使う。
Legacy状態はPreset保存にも残る。旧Presetを読み込めばLegacy Tiltも復元される。

setStateInformationでは新parameterが無い古い状態を読み込む際、欠けた`vq*`を
defaultで補完してからreplaceStateする。実行中の新しい値を旧セッションへ持ち越さない。
Preset loadも既存の「欠落項目はdefault、locked項目は維持」の規則で処理する。
既存parameterを削除・並べ替えしないため、host automation indexを維持する。

## 検証範囲と実機確認

過剰な長時間検証より実装を優先する。短い決定的な回帰試験とローカルビルドを行う。
検証結果とコマンドは `VOICE_QUALITY_VALIDATION.md` に記録する。
Windows実機/DAWホスト互換性、実際のマイクでの音質はユーザーの確認範囲。
特に小声→大声でのGR/Pitch、Normal/Low Latency切替、Robotize、Stereo、
高Q/周波数automation、旧Tilt非0セッションの再現を確認する。

公開・push・デプロイ・外部アップロードはこの実装作業では行わない。
