# VoxMorph 開発引き継ぎ書 (AIセッション用)

最終更新: v0.6.0 時点。新しいAIセッションを開始する際は、このファイルを読ませること。

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
  - **Air Preserve(Mixed区間処理、v0.6.0)**: 因果2タップピッチコム(P, 2P遅延・Catmull-Rom補間)で周期成分を予測→残差を1.4kHzハイパス=息成分。グレインは息を除いた `harmBuf` から切出し、息は**ピッチ変換せず**遅延整列して出力に加算(連続ノイズのまま)。残差の2/3分離でエネルギー厳密保存。有声ゲート+5msデジッパー、ノブ0で完全従来動作+自動スキップ
  - 未使用機能は全て自動スキップ(CPU: 通常1.4%、F1-F3使用時2.6% @48k)
- `src/PluginProcessor.{h,cpp}` — JUCEラッパ、パラメータ(APVTS)、ハウリング自動ミュート(時間基準、RMS>0.70が1.5秒で3秒ミュート)
- `src/PluginEditor.h` — 英語UI+英日バイリンガルツールチップ、セクション: PITCH/FORMANT/INTONATION/VOICE QUALITY/ADVANCED/OUTPUT。Cmd+S保存(スタンドアロン)
- `test/offline_test.cpp` + `analyze.py` — 合成母音での数値検証(Linux g++でコンパイル可、JUCE不要)。**エンジン変更時は必ず実行**: f0/フォルマント独立性、抑揚、子音シフト、フライ声、回帰一式

## 主要パラメータ(内部ID)

pitch, formant, consonant, f1shift/f1gain/f2shift/f2gain/f3shift/f3gain, range(抑揚)/center, breath2(Beta), air(Air Preserve), tilt, jitter, robot/robotHz, lowvoice, lowlat, pitchfloor, automute, mix, gain

## 重要な設計判断・経緯

- Vocal Transformerはgranular(PSOLA近縁)方式 — Logic 9マニュアルに明記。同系統を自作した
- 二重声バグ→グレイン幅を出力間隔に適応(v0.3.2)。低音フライ声→Low Voice Mode(40Hz+ホールド、v0.3系)
- Breathは加算方式→HNM置換方式まで試したが**ユーザー評価は「電子ノイズ」でBeta棚上げ中**。次の本命=連続ノイズを声道フィルタに通す方式(グレイン毎ノイズ再生成をやめる)
- 大バッファのブツブツ/無音バグ(v0.5.2で解決)=自動ミュートのバッファ依存誤爆+検出頻度低下
- Air Preserve(v0.6.0)=息混じり母音の金属的ジャリジャリ対策。グレイン再配置がノイズに新ピッチの周期性を刻む(上方シフトのグレイン再利用で最悪)のが原因で、息成分をシフト前に分離しバイパスする。数値検証: HF周期性 素材≈0/off=0.34/on(0.8)=0.14、f0・フォルマント・エネルギー保存。コムの補間は線形だと高域キャンセルが甘く旧ピッチのゴーストが漏れるため**Catmull-Rom必須**
- 推奨バッファ256@48k。バッファは音質に影響しない設計
- バージョン番号は `CMakeLists.txt` の project(VERSION) と `.github/workflows/build.yml` の pkgbuild --version の**2箇所**を同時に上げる

## 凍結中(ユーザー指示によりストップ)

- Low Latency Mode の追加開発(実装済み・動作するが優先度下げ)
- Breath改良、専用仮想デバイス同梱

## 次の候補(未着手)

- ChatGPT調査資料(`psola_engine_research_for_claude.txt`、ユーザー保有)より: GCI/ESOLA同期 — 残る音質向上の本命(Mixed区間処理はv0.6.0のAir Preserveとして実装済み)
- Air Preserveの発展: 息成分へのフォルマント追従(現状は無変換バイパス)、実声での既定値チューニング
- 専用スキンUI、プリセット機能

## 引き継ぎ手順

新セッションで: このファイルと、必要に応じ `DESIGN.md` / `dsp/PsolaEngine.h` を読む。検証はサンドボックスで `g++ -O2 -std=c++17 test/offline_test.cpp` → `python3 test/analyze.py`(要numpy)。修正ファイルはユーザーに渡し、アップロード先フォルダを明記すること。
