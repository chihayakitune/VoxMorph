# VoxMorph → OBS / Discord 連携ガイド (仮想オーディオ)

VoxMorphのスタンドアロン版(インストーラで `~/Applications/VoxMorph.app` に入っています)と
無料の仮想オーディオデバイスを組み合わせると、変換後の声をOBSやDiscordに入力できます。

```
マイク → VoxMorph.app (リアルタイム変換) → BlackHole (仮想デバイス) → OBS / Discord
```

## Mac での手順

### 1. BlackHole をインストール (初回のみ)

無料の仮想オーディオドライバです。

1. [existential.audio/blackhole](https://existential.audio/blackhole/) から **BlackHole 2ch** を入手
   (メール登録でダウンロードリンクが届きます)
2. pkgをダブルクリックしてインストール

### 2. VoxMorph.app の設定

1. `~/Applications/VoxMorph.app` を起動(初回はマイク許可を求められます)
2. 左上の **Options → Audio/MIDI Settings** を開く
3. **Input**: お使いのマイク
4. **Output**: **BlackHole 2ch**
5. **Audio buffer size**: 256 samples 推奨(小さいほど低遅延)

これで変換後の声がBlackHoleに流れ続けます。プラグイン版と同じパラメータ画面で調整できます。

### 3. OBS 側の設定

1. ソース追加 → **音声入力キャプチャ**
2. デバイス: **BlackHole 2ch**

Discordの場合: 設定 → 音声・ビデオ → 入力デバイス = **BlackHole 2ch**

### 4. 自分の耳でもモニターしたい場合

そのままだと変換後の声が自分に聞こえません。聞きたい場合:

1. **Audio MIDI設定**(Spotlightで検索)を開く
2. 左下「+」→ **複数出力装置** を作成
3. **BlackHole 2ch** と **ヘッドホン** の両方にチェック
4. VoxMorphのOutputをこの複数出力装置に変更

※必ずヘッドホンで。スピーカーだとマイクに回り込みます。

## Windows での手順

1. [VB-CABLE](https://vb-audio.com/Cable/) (無料)をインストールして再起動
2. VoxMorph Standalone(zipのStandaloneフォルダ)を起動
3. Options → Audio/MIDI Settings: Input=マイク、Output=**CABLE Input**
4. OBS/Discordの入力デバイス: **CABLE Output**

## 遅延について

現在の変換遅延は約43ms+オーディオバッファです。配信では映像との同期のため、
OBS側で映像に50〜80msの遅延を足すとリップシンクが合います
(ソースのフィルタ → 映像遅延)。低遅延モードは今後のバージョンで対応予定です。

## 将来の計画

現在はBlackHole/VB-CABLEという既存の仮想デバイスを利用していますが、
将来のバージョンでは専用の仮想デバイスを同梱し、この設定を1クリックにする予定です。
