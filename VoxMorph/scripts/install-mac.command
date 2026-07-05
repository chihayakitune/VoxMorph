#!/bin/bash
# VoxMorph インストーラ (macOS)
# ダウンロードした artifact を解凍したフォルダでこのスクリプトを実行してください:
#   ターミナルに「bash 」(半角スペースまで)と打ち、このファイルをドラッグして Enter
set -e
cd "$(dirname "$0")"

DEST_AU=~/Library/Audio/Plug-Ins/Components
DEST_VST3=~/Library/Audio/Plug-Ins/VST3
mkdir -p "$DEST_AU" "$DEST_VST3"

if [ -d "AU/VoxMorph.component" ]; then
    rm -rf "$DEST_AU/VoxMorph.component"
    cp -R "AU/VoxMorph.component" "$DEST_AU/"
    xattr -cr "$DEST_AU/VoxMorph.component"
    echo "AU  -> $DEST_AU/VoxMorph.component"
fi

if [ -d "VST3/VoxMorph.vst3" ]; then
    rm -rf "$DEST_VST3/VoxMorph.vst3"
    cp -R "VST3/VoxMorph.vst3" "$DEST_VST3/"
    xattr -cr "$DEST_VST3/VoxMorph.vst3"
    echo "VST3 -> $DEST_VST3/VoxMorph.vst3"
fi

if [ -d "Standalone/VoxMorph.app" ]; then
    rm -rf ~/Applications/VoxMorph.app
    mkdir -p ~/Applications
    cp -R "Standalone/VoxMorph.app" ~/Applications/
    xattr -cr ~/Applications/VoxMorph.app
    echo "App -> ~/Applications/VoxMorph.app"
fi

# Logic のプラグインキャッシュをリセット
killall -9 AudioComponentRegistrar 2>/dev/null || true

echo ""
echo "インストール完了。Logic Pro を再起動してください。"
echo "場所: Audio FX > Audio Units > HakamaAudio > VoxMorph"
