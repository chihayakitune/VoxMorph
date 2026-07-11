# VoxMorph 開発引き継ぎ書 (AIセッション用)

最終更新: v0.11.3 時点。新しいAIセッションを開始する際は、このファイルを読ませること。

## プロジェクト概要

- **VoxMorph**: リアルタイム波形変換型ボイスチェンジャー。Logic Pro「Vocal Transformer」の上位互換が目標(達成済み)。AI音声変換ではなく信号処理方式。
- リポジトリ: `github.com/chihayakitune/VoxMorph`(Public)
- 形態: AU / VST3 プラグイン + スタンドアロンアプリ(macOS/Windows)、JUCE 8 + CMake
- ビルド: GitHub Actions が push 毎に自動ビルド。Artifacts の `VoxMorph-macOS-Installer`(pkg、ダブルクリックで上書きインストール可)と `VoxMorph-Windows`
- ユーザー(袴さん)は非プログラマー。**開発ワークフロー(v0.6.0から): AIがgitコマンドで直接コミット&プッシュ**。`~/bin/gh` に GitHub CLI 導入済み・chihayakitune で認証済み(repo+workflowスコープ、キーチェーン保存)。手順: リポジトリを浅くクローン→変更ファイルをコピー→commit→push。ブラウザ操作はしない約束(トークン浪費のため)。認証が切れていたら `gh auth login` のデバイスコード方式でユーザーに再認証を依頼

## アーキテクチャ

- `dsp/PsolaEngine.h` — **依存ゼロの単一ヘッダC++17**。エンジン本体(TD-PSOLA):
  - YINピッチ検出(×2デシメーション+全レート精密補正、512サンプル毎、オクターブ誤り補正、パルスレート優先、上方向復帰許可)
  - ピッチ同期グレイン切出し(ピーク整列、グレイン幅=min(入力周期, 1.25×出力間隔)←二重声対策)
  - スペクトル層(グレイン毎FFT 2048/4096適応): 倍音頂点結合の包絡推定→F1/F2/F3追跡→区分線形ワープ+個別ゲイン、Breath(ノイズ励振、Beta)
  - **どんなホストバッファも内部で512サンプル毎に分割処理**(全挙動バッファ非依存、v0.5.2)
  - **High Rangeガード(v0.8.0、バ美声の可変ピッチ参考)**: 入力f0がhifreq(0=オフ)を超えると、Pitch/Formantの半音量に hipitch/hiformant(%)を乗じた量へ log2ブレンド(1オクターブ上で100%移行、グレイン毎・平滑化済みcurP駆動でシームレス)。用途=笑い声が非人間的高音に飛ぶのを防ぐ。検証: 入力150→300Hz/+7st/開始200Hz/量0%で高音側448.6→355.6Hz(理論値一致)。Robotize時は無効
  - **GCI Grain Sync(v0.7.0、gciトグル・既定オフ)**: グレイン整列を従来のエネルギーピーク探索から、声帯閉鎖瞬間(微分エネルギー最大点=各周期の最急峻フランク、中央差分でHFノイズ耐性)+周期間トラッキング(lastGci+k*P近傍を優先、自由探索ピークが2倍強い時だけ乗換=ダブルパルス飛び移り防止)に切替。息/無声/平坦区間は自動でalignToPeakへフォールバック、オフ時は完全従来動作。検証: クリーン母音は完全一致(非破壊)、不規則フライ声+7stで出力周期性0.066→0.225(3.4倍)。**v0.7.1でBeta化**: ユーザー実声評価「あ/う のトーン移動でゴロゴロと周期的にガタつく・音程を動かすとロボットっぽい」→ 原因はグライド中の lastGci+k*P 予測グリッドと実エポックのずり。対策: ピッチ変化>4%/フレームで4フレーム従来整列へ自動退避(Low Voice Mode時は除外=フライ周期交代で常時発動してしまうため)。これ以上のチューニングはユーザー指示で凍結
  - **Air Preserve(Mixed区間処理、v0.6.0/強化v0.6.1)**: 因果2タップピッチコム(P, 2P遅延・Catmull-Rom補間)で周期成分を予測→残差をairband(既定1kHz)ハイパス=息成分。グレインは息を除いた `harmBuf` から切出し、息は**ピッチ変換せず**遅延整列して出力に加算(連続ノイズのまま)。ノブ0〜1.0=分離量0〜最大(2/3=分散最適、これ超の減算は逆にノイズ増)でエネルギー中立、1.0〜1.5=息を追加ブースト(最大約+4dB、可聴性・マスキング用。v0.7.0でユーザー要望によりこのスケールに変更)。有声ゲート+5msデジッパー、ノブ0で完全従来動作+自動スキップ
  - 未使用機能は全て自動スキップ(CPU: 通常1.4%、F1-F3使用時2.6% @48k)
- `src/PluginProcessor.{h,cpp}` — JUCEラッパ、パラメータ(APVTS)、ハウリング自動ミュート(時間基準、RMS>0.70が1.5秒で3秒ミュート)
- `src/PluginEditor.h` — 英語UI+英日バイリンガルツールチップ、セクション: PITCH/HIGH RANGE/FORMANT/INTONATION/VOICE QUALITY/ADVANCED/OUTPUT。Cmd+S保存(スタンドアロン)。**v0.8.0からViewportでスクロール+リサイズ可能**。行の追加はコンストラクタに addSliderRow/addToggleRow を1行書くだけで、レイアウト・スクロール・ウィンドウサイズは自動調整(ファイル冒頭に保守手順コメントあり。UI調整は高度AIでなくても可能な構造)。**v0.9.0: SpectrumView**(最上段、入力ミント/出力ピンクの重ねスペクトラム 20Hz-20kHz対数軸、30Hzタイマー、FFTはエンジンのfftForViz流用=依存追加なし。データはProcessorのvizIn/vizOut/vizPosリングから)+全体をパステル配色(色は mainLnf 設定の1箇所に集約。v0.9.1でユーザー指定のミント基調=見出し/スライダー/つまみ/リセット矢印がミント、本文と数値が濃グレー、背景ほぼ白に確定。グラフ配色は入力ミント/出力ピンクのまま)。ユーザー保有のUI完成イメージ(サイドバー+カード式のかわいい系デザイン)への本格スキン化は未着手。**v0.10.0でMAIN/ANALYZEの2タブ構成**(TabbedComponent、ページはFnComponent。数値欄の白文字問題はsendLookAndFeelChange()で解決=スライダーのテキストボックスはLnF適用前に生成されるため必須)
- `dsp/VoiceAnalyzer.h` — **声質プロファイル分析(v0.10.0、依存ゼロ)**。2048フレーム/hop1024、4xデシメYIN f0+FFT包絡→F1-F3・相対レベル・tilt・抑揚spread(p10-p90/2、MADは二峰性で破綻するため不可)。全て中央値でロバスト化。ANALYZEタブと offline_test で共用
- `src/PluginEditor.h` 内 AnalyzePanel — **ANALYZEタブ(v0.10.0)**: ①Record 5s(processorのcapBufに音声スレッドで録音)→Profile1 ②Load Target File(wav/mp3等、60秒まで、線形リサンプルでprevBufへ、Playで出力にミックス再生)→Profile2 ③Auto-Set=差分からpitch/formant/F1-3 shift&gain/range/center/tilt(0.25倍)を自動設定。v0.10.1: 手順をファイル→録音の順に変更、録音時間5/10/15sプルダウン(capTarget)、**個別フォルマントは控えめ設定**(shift=差×0.5・±3st、gain=差×0.5・±8dB。振り切ると乾いた電子音になるため)。分析側もF1探索下限=max(250, 1.35×f0)で基音の誤検出を防止(実例: F0 185Hzの声でF1=246と誤検出→F1 Shift+6に振り切れた)。**v0.11.0: Refineループ(手順④⑤)**=Record Converted + Refineで変換後出力を録音(capFromOutput=trueで最終段g*mをキャプチャ、ファイル再生音は混入しない)→目標との残差×減衰係数(pitch0.8/formant0.7/トリム0.4/tilt0.25/range比0.75-1.35クランプ)を現在値に加算。繰り返しで収束する設計。v0.11.1: ProfileGraph=測定プロファイルから逆算した想定スペクトル線グラフ(F0+F1-F3の山、高さ=相対レベル、F3以上-9dB/oct、50Hz-8kHz対数軸、塗りなし。You=ミント/Target=パステルイエロー/Converted=ピンク、測定完了時に静的更新)。v0.11.2: 見出し付き4セクション(Target/Analyze MyVoice/Re-Analyze MyVoice [Converted]/Apply Analyzed Settings、ユーザー指定の並び=Auto-Setは一番下)+「with Play」ボタン2種(ターゲット再生と録音を同時開始、録音完了で自動停止=playStartedByCapture)。v0.11.3: ユーザー評価「Pitchが強めに出る」→ kPitchBias=1st(F0差より1st控えめ)+kRangeBoost=1.15(Intonation増しで山の高さを合わせる。話し声の知覚的高さは山が担うため)。Auto-SetとRefine両方に同一バイアス(打ち消し合い防止)。Auto-Setは hifreq=自分のF0×2^((spread+2)/12)(通常域の上端、hipitch50%/hiformant100%も設定)と pitchfloor=目標F0×2^(-(spread+1)/12)(目標音域の下端)も自動設定
- `test/offline_test.cpp` + `analyze.py` — 合成母音での数値検証(Linux g++でコンパイル可、JUCE不要)。**エンジン変更時は必ず実行**: f0/フォルマント独立性、抑揚、子音シフト、フライ声、VoiceAnalyzer、回帰一式

## 主要パラメータ(内部ID)

pitch, formant, consonant, f1shift/f1gain/f2shift/f2gain/f3shift/f3gain, range(抑揚)/center, breath2(Beta), air/airband(Air Preserve, airは0〜1.5), gci(GCI同期), hifreq/hipitch/hiformant(High Rangeガード), tilt, jitter, robot/robotHz, lowvoice, lowlat, pitchfloor, automute, mix, gain

## 重要な設計判断・経緯

- Vocal Transformerはgranular(PSOLA近縁)方式 — Logic 9マニュアルに明記。同系統を自作した
- 二重声バグ→グレイン幅を出力間隔に適応(v0.3.2)。低音フライ声→Low Voice Mode(40Hz+ホールド、v0.3系)
- Breathは加算方式→HNM置換方式まで試したが**ユーザー評価は「電子ノイズ」でBeta棚上げ中**。次の本命=連続ノイズを声道フィルタに通す方式(グレイン毎ノイズ再生成をやめる)
- 大バッファのブツブツ/無音バグ(v0.5.2で解決)=自動ミュートのバッファ依存誤爆+検出頻度低下
- Air Preserve(v0.6.0)=息混じり母音の金属的ジャリジャリ対策。グレイン再配置がノイズに新ピッチの周期性を刻む(上方シフトのグレイン再利用で最悪)のが原因で、息成分をシフト前に分離しバイパスする。コムの補間は線形だと高域キャンセルが甘く旧ピッチのゴーストが漏れるため**Catmull-Rom必須**
- v0.6.1: ユーザー評価「きれいになるが変化が微小」→ 帯域を1.4k→1k既定+airbandノブ化(下げるほど強い)、ノブ0.7超で息ブースト追加。数値検証: HF周期性 素材≈0 / off=0.34 / on0.8=0.06 / max(1.0,帯域700)=0.04、f0・フォルマント保存
- 推奨バッファ256@48k。バッファは音質に影響しない設計
- バージョン番号は `CMakeLists.txt` の project(VERSION) と `.github/workflows/build.yml` の pkgbuild --version の**2箇所**を同時に上げる

## 凍結中(ユーザー指示によりストップ)

- Low Latency Mode の追加開発(実装済み・動作するが優先度下げ)
- Breath改良、専用仮想デバイス同梱
- GCI Grain Sync の追加チューニング(v0.7.1でBeta化・グライド退避を入れて凍結。主用途はLow Voice Mode併用の低音・フライ声)

## 次の候補(未着手)

- 音質向上の本命2つ(Mixed区間処理=v0.6.0 Air Preserve、GCI/ESOLA同期=v0.7.0 GCI Grain Sync)は実装済み。ユーザーの実声評価待ち
- Air Preserveの発展: 息成分へのフォルマント追従(現状は無変換バイパス)、実声での既定値チューニング
- 専用スキンUI(ユーザーが完成イメージ画像を保有: サイドバーMain/Visualizer/Presets/Settings+カード式パステルデザイン。v0.9.0で配色とビジュアライザーまで実装済み)
- プリセット機能(ユーザー判断で保留中: スタンドアロンの状態保存で当面足りるため)

## 引き継ぎ手順

新セッションで: このファイルと、必要に応じ `DESIGN.md` / `dsp/PsolaEngine.h` を読む。検証はサンドボックスで `g++ -O2 -std=c++17 test/offline_test.cpp` → `python3 test/analyze.py`(要numpy)。修正ファイルはユーザーに渡し、アップロード先フォルダを明記すること。
