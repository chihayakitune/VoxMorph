# Voice Quality implementation validation — 2026-09-21

Base: `b867461f0bba2a92c9d7aca055ada88170e2f539` (fresh `origin/main` fetch).
Local branch: `codex/voice-quality`. No push/upload/deployment performed.

## Local build

AppleClang 17, C++17, JUCE 8.0.4, Release, native macOS arm64.
Successfully built AU, VST3 and Standalone with `COPY_AFTER_BUILD=OFF`.
No installed plugin or application was overwritten.
The build also linked `VoxMorphUiShot`; only the focused Voice Quality UI render was run,
not the long general-purpose UI audit. The Voice Quality panel was visually inspected at 320 px wide.
The build has existing deprecation warnings and signed-index conversion warnings, no compile errors.
The VST3 packaging step replaced its build-time signature with an ad-hoc signature successfully.
This is a local development build, not a notarized release.

## Regression results

- `test/bitexact.cpp` built once against the base commit and once against the final modified engine,
  using the same compiler/settings. Both 12-scenario output files compare byte-for-byte equal.
  Includes pitch/formant, mixed wet/dry, Natural Air, Robotize, Low Latency, Legacy Tilt,
  jitter, GCI and host chunk sizes including 32, 64, 96, 128, 200, 256, 480, 512.
- `test/voice_quality_test.cpp`: all checks pass.
  - Bell/low-shelf/high-shelf response at +6 dB.
  - Hard/soft knee and 2:1 mapping.
  - Zero-gain exact bypass including APVTS-sized zero rounding error.
  - 44.1/48/88.2/96 kHz, block requests 32/64/128/256/512/1024/8192.
  - Sample-identical detector/EQ outputs across block partitions at each rate.
  - Stereo linked control preserves a scaled L/R pair.
  - Null control vs zero control engine identity, Normal and Low Latency.
  - Instrumented grain log: Dynamic Pitch offset is read at actual source `inMark`,
    after Intonation; both latency modes pass.
  - Robotize ignores Dynamic Pitch exactly.
  - Dynamic EQ GR reads the exact delayed input timestamp; the first observable float32
    output change is one sample later than the first nonzero control in this tiny-onset test
    (initial biquad difference is below float32 resolution). No lookahead/block timing error.
  - Pitch envelope at equal elapsed seconds agrees across sample rates within 0.0001 st.
  - NaN/Inf input does not escape the new EQ stage.
- `test/voice_quality_proc_test.cpp`: all checks pass.
  - Loading an old session resets missing new parameters to defaults.
  - Legacy Tilt and new EQ/Pitch state round trip.
  - Preset save/load and new parameter locks.
  - One graph gesture restores both frequency and gain with one undo, including polling mid-drag.
  - `vecenabled=ON / vecamount=100` is now inert; oversized 8192-sample block included.
  - Active EQ/Pitch remains finite across Stereo, reset and Low Latency transitions.
  - Panel rendered with a desktop peer; dBFS labels fit in the minimum tested width.
- Existing literal parameter ID registration order compared to base: unchanged (66 literal IDs;
  existing generated parameter loops also left in place). New 48 Band + 6 Pitch parameters appended.
- `git diff --check`: clean.

The initial state test exposed sub-micro-dB APVTS quantization around zero. The DSP now
canonicalizes magnitudes below 1e-5 to exact zero, and the regression includes that case.
The initial screenshot exposed clipped dBFS suffixes. Threshold rows now reserve a wider readout.
The UI harness emitted a macOS hiservices connection diagnostic in this execution environment;
the process exited successfully and the resulting panel image was inspected.

## Reproduction

```sh
clang++ -O2 -std=c++17 -I dsp test/voice_quality_test.cpp -o /tmp/vq-test
/tmp/vq-test
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVOXMORPH_VQ_TEST=ON -DCOPY_AFTER_BUILD=OFF
cmake --build build --target VoxMorphVqTest VoxMorph_AU VoxMorph_VST3 VoxMorph_Standalone
build/VoxMorphVqTest_artefacts/Release/VoxMorphVqTest /tmp/vq-panel.png
```

JUCE can be supplied offline via `-DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE-8.0.4`.
The focused processor test writes a uniquely named temporary `.vmpreset` in the working directory
then removes it. Run from a writable directory.

## User listening / host checks (not performed here)

1. MAIN > VOICE QUALITY: drag each Band node, edit Q, switch Type, adjust Dynamics.
2. Below Threshold vs above Threshold; Ratio, Max Reduction, attack/release and shelf Q by ear.
3. Dynamic Pitch ±0.1..0.5 st on quiet-to-loud syllables; Normal and Low Latency; Robotize bypass.
4. Stereo microphone material: image stability and linked GR with unequal left/right input.
5. Old nonzero-Tilt DAW sessions and automation: Legacy Tilt still works, AVD is intentionally absent.
6. Actual AU/VST3 host loading, DAW automation/save/reopen and Windows build/host validation.
7. High-Q gain/frequency automation and CPU headroom at the user's device buffer.

No Windows CI or listening claims are made. No external service received the source or generated builds.
