# VoxMorph

Real-time voice transformer — AU / VST3 plugin + standalone app (macOS & Windows).
リアルタイム波形変換型ボイスチェンジャー。Logic Pro「Vocal Transformer」と同系統の
TD-PSOLA方式で、ピッチとフォルマントを完全に独立して操作できます。

## Features / 機能

- **Pitch / Formant** — fully independent shifting (TD-PSOLA), ±24 st
- **Per-formant control** — F1/F2/F3 individual shift & gain (spectral envelope warping)
- **Intonation** — scale the pitch movement around a pivot (natural male-to-female)
- **Consonant Shift** — unvoiced consonants shifted separately
- **Natural Air** — keeps the voice's own breath & aperiodic detail while
  suppressing old-pitch leakage; **Air Shine** adds high-frequency openness
- **Robotize / Pitch Floor / Natural Jitter / Softness (Tilt)**
- **Low Voice Mode** — vocal-fry tracking down to 40 Hz with pitch hold
- **Low Latency Mode** — ~21 ms conversion delay for live use (normal: ~43 ms)
- **Auto-Mute on Feedback**, Cmd+S state save, bilingual (EN/JP) tooltips

## Install / インストール

Actions タブ → 最新の実行 → Artifacts:

- **VoxMorph-macOS-Installer** — pkgをダブルクリック (AU/VST3/App)
- **VoxMorph-Windows** — VST3 + Standalone

Logic Pro: Audio FX > Audio Units > HakamaAudio > VoxMorph

## Docs / ドキュメント

- [OBS_SETUP_GUIDE.md](OBS_SETUP_GUIDE.md) — OBS/Discordへ変換音声を入力する(BlackHole/VB-CABLE)
- [DESIGN.md](DESIGN.md) — アルゴリズム設計とVocal Transformer分析
- [FEATURE_PLAN.md](FEATURE_PLAN.md) — パラメータ設計の音声学的背景
- [GITHUB_BUILD_GUIDE.md](GITHUB_BUILD_GUIDE.md) / [BUILD_GUIDE.md](BUILD_GUIDE.md) — ビルド手順

## Specs

TD-PSOLA + per-grain spectral layer (4096-pt FFT), CPU ~3% (spectral off) / ~9% (on) @48 kHz.
Latency 43 ms (default) / 21 ms (Low Latency Mode) / 43 ms (Low Voice Mode).
Built with JUCE 8 (GPL). DSP core (`dsp/PsolaEngine.h`) is dependency-free C++17.

## Roadmap

Bundled virtual audio device (1-click OBS setup), continuous-noise Breath, custom skinned UI.
