# VoxMorph ビルド手順(初心者向け・Mac)

所要時間: 初回のみ準備に30分〜1時間(ほぼ待ち時間)。2回目以降のビルドは数分。

## 1. 開発ツールの準備(初回のみ)

1. **Xcode** を App Store からインストール(無料、サイズが大きいので時間がかかります)。
   インストール後に一度起動し、追加コンポーネントのインストールに同意してください。
2. **ターミナル**を開き(Spotlightで「ターミナル」)、次を1行ずつ実行:

   ```
   xcode-select --install
   ```
   (「既にインストール済み」と出たらOK)

3. **Homebrew**(ソフト管理ツール)をインストール:

   ```
   /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
   ```
   画面の指示に従ってください(パスワード入力あり)。

4. **CMake** をインストール:

   ```
   brew install cmake
   ```

## 2. ビルド

ターミナルでこのフォルダ(VoxMorph)に移動します。`cd ` と打った後にVoxMorphフォルダをターミナルへドラッグ&ドロップしてEnterが簡単です。

```
cd <VoxMorphフォルダ>
cmake -B build -G Xcode -DCOPY_AFTER_BUILD=ON
cmake --build build --config Release
```

- 1回目の `cmake -B build` は JUCE(開発フレームワーク)を自動ダウンロードするので数分かかります。
- ビルドが成功すると、AUプラグインが自動で
  `~/Library/Audio/Plug-Ins/Components/VoxMorph.component` にコピーされます。

## 3. 動作確認

AUの検証(Logicが読み込む前の関門):

```
auval -v aufx Vxmf Hkma
```

最後に `PASS` と出れば成功です。

## 4. Logic Pro で使う

1. Logic Pro を完全に終了して再起動。
2. オーディオトラックの Audio FX スロット → **Audio Units > HakamaAudio > VoxMorph**。
3. Pitch(ピッチ±24半音)、Formant(フォルマント±24半音)は完全に独立して動きます。
   Robotize をオンにすると Robot Pitch(Hz)の固定音程になります。

## 5. スタンドアロン版(おまけ)

ビルドすると `build/VoxMorph_artefacts/Release/Standalone/VoxMorph.app` も生成されます。
起動してマイクを選べば単体でリアルタイム変換できます(**必ずヘッドホン使用**。スピーカーだとハウリングします)。

## トラブルシューティング

- **Logicに出てこない** → ターミナルで `killall -9 AudioComponentRegistrar` を実行してLogicを再起動。それでも駄目なら Logic Pro > 設定 > プラグインマネージャー で再スキャン。
- **ビルドエラー** → `build` フォルダを削除して手順2をやり直し。エラーメッセージをコピーしてClaudeに貼り付けてもらえれば直します。
- **音が遅れて聞こえる** → 本バージョンのレイテンシは約43msです(再生時はLogicが自動補正)。低遅延化はv0.2で対応予定。
- **音が二重に聞こえる** → Mixを1.0(100%)にしてください。
