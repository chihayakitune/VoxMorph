# GitHubクラウドビルド手順(Macに何もインストール不要)

GitHubの無料CI(GitHub Actions)がMac用・Windows用のプラグインを自動ビルドします。
あなたのMacですることは「ファイルをアップロード」と「完成品をダウンロード」だけです。

## 1. GitHubアカウント作成(初回のみ)

[github.com](https://github.com) → Sign up。無料プランでOK。

## 2. リポジトリ(プロジェクト置き場)を作る

1. 右上の「+」→「New repository」
2. Repository name: `VoxMorph`
3. **Public** を選択(Publicならビルド時間が完全無料・無制限。Privateだと月2,000分制限で、Macビルドは10倍消費されるため月13回程度まで)
4. 「Create repository」

## 3. ファイルをアップロード

1. 作成直後の画面にある「**uploading an existing file**」リンクをクリック
2. Finderで VoxMorph フォルダを開き、**中身を全部選択**してドラッグ&ドロップ
   (CMakeLists.txt、src、dsp、test、scripts、.mdファイル)
   ※ `.github` フォルダは名前が「.」で始まるためFinderでは見えず、アップロードされません。次の手順で作ります。
3. 「Commit changes」をクリック

## 4. ビルド設定ファイルを作る(コピペ1回)

1. リポジトリの「**Add file**」→「**Create new file**」
2. ファイル名欄に次を入力(`/`を打つと自動でフォルダになります):

   ```
   .github/workflows/build.yml
   ```

3. 手元の `VoxMorph/.github/workflows/build.yml` をテキストエディットで開き、中身を全部コピーして貼り付け
   (Finderで見えない場合: Finderで `Cmd+Shift+.` を押すと隠しフォルダが表示されます)
4. 「Commit changes」→ この瞬間にビルドが自動スタートします

## 5. 完成品をダウンロード(10〜15分後)

1. リポジトリ上部の「**Actions**」タブ → 一番上の実行をクリック
2. 緑のチェックが付いたら、下の「Artifacts」欄から **VoxMorph-macOS** をダウンロード
   (**VoxMorph-Windows** も同時にできています)
3. ダウンロードしたzipをダブルクリックで解凍

## 6. インストール

解凍したフォルダに `install-mac.command` が入っています。

1. ターミナルを開く
2. `bash ` (半角スペースまで)と打つ
3. `install-mac.command` をターミナルへドラッグして Enter

AU/VST3/アプリ版が自動配置され、macOSの検疫属性も解除されます。
Logic Pro を再起動 → **Audio FX > Audio Units > HakamaAudio > VoxMorph**。

## 以後の更新

コードを修正したら、GitHubのファイル画面で鉛筆アイコン(Edit)→貼り替え→Commit、
またはファイルを再アップロードするだけで、自動で新しいビルドが走ります。

## トラブルシューティング

- **Actionsに赤い×** → ×をクリック→失敗したジョブのログをコピーしてClaudeへ貼り付けてください。
- **Logicに出てこない** → ターミナルで `killall -9 AudioComponentRegistrar` → Logic再起動。
- **「開発元を確認できない」と出る** → install-mac.command経由でインストールすれば出ません(検疫解除済み)。手動コピーした場合は `xattr -cr <プラグインのパス>` を実行。
