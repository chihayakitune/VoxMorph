cmake_minimum_required(VERSION 3.22)
project(VoxMorph VERSION 0.68.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

# ローカルビルド時に ON にすると ~/Library/Audio/Plug-Ins へ自動コピー
option(COPY_AFTER_BUILD "Copy plugin to system folder after build" OFF)

# JUCE は初回ビルド時に自動ダウンロードされます
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(VoxMorph
    COMPANY_NAME                "HakamaAudio"
    PLUGIN_MANUFACTURER_CODE    Hkma
    PLUGIN_CODE                 Vxmf
    FORMATS                     AU VST3 Standalone
    PRODUCT_NAME                "VoxMorph"
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    AU_MAIN_TYPE                kAudioUnitType_Effect
    COPY_PLUGIN_AFTER_BUILD     ${COPY_AFTER_BUILD}
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT  "VoxMorph needs microphone access for live voice processing."
)

target_sources(VoxMorph PRIVATE src/PluginProcessor.cpp)
target_include_directories(VoxMorph PRIVATE dsp)

# ANOKOE UI art (v0.31.2). Every PNG in assets/ is compiled into the binary,
# so nothing has to be installed alongside the plugin. Names in BinaryData are
# the file names with '.' replaced by '_' — code looks them up by string
# through BinaryData::getNamedResource(), so adding art here is enough.
file(GLOB ANOKOE_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/assets/*.png")
juce_add_binary_data(AnokoeAssets SOURCES ${ANOKOE_ASSETS})
target_link_libraries(VoxMorph PRIVATE AnokoeAssets)

if (MSVC)
    target_compile_options(VoxMorph PRIVATE /utf-8)   # 日本語UI文字列のため
endif()

target_compile_definitions(VoxMorph PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_PLUGINHOST_VST3=1   # スタンドアロンのPre/Post FXホスティング用
)

target_link_libraries(VoxMorph
    PRIVATE juce::juce_audio_utils
    PUBLIC  juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags
)

# ---- Diagnostics fault-injection probe (developer tool, OFF by default) --
# Runs EngineDiagnostics through the REAL VoxMorphProcessor::processBlock and
# injects faults at a chosen observation point. VOXMORPH_DIAG_TEST_HOOKS is set
# ONLY here, so the injection hook does not exist in any shipping build.
#   cmake -B build -DVOXMORPH_DIAG_PROBE=ON
#   cmake --build build --target VoxMorphDiagProbe
option(VOXMORPH_DIAG_PROBE "Build the diagnostics fault-injection probe" OFF)
if (VOXMORPH_DIAG_PROBE)
    juce_add_console_app(VoxMorphDiagProbe PRODUCT_NAME "VoxMorphDiagProbe")
    target_sources(VoxMorphDiagProbe PRIVATE test/diag_probe.cpp src/PluginProcessor.cpp)
    target_include_directories(VoxMorphDiagProbe PRIVATE dsp src)
    target_compile_definitions(VoxMorphDiagProbe PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_PLUGINHOST_VST3=1
        JUCE_STANDALONE_APPLICATION=1
        JUCE_MODAL_LOOPS_PERMITTED=1
        VOXMORPH_DIAG_TEST_HOOKS=1)
    target_link_libraries(VoxMorphDiagProbe
        PRIVATE juce::juce_audio_utils AnokoeAssets
        PUBLIC  juce::juce_recommended_config_flags)

    # The SAME sources with the diagnostics compiled out. Renders the
    # pre-v0.68.0 signal path so the probe's dump can be diffed against a real
    # baseline rather than against another copy of itself.
    juce_add_console_app(VoxMorphDiagBaseline PRODUCT_NAME "VoxMorphDiagBaseline")
    target_sources(VoxMorphDiagBaseline PRIVATE test/diag_probe.cpp src/PluginProcessor.cpp)
    target_include_directories(VoxMorphDiagBaseline PRIVATE dsp src)
    target_compile_definitions(VoxMorphDiagBaseline PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_PLUGINHOST_VST3=1
        JUCE_STANDALONE_APPLICATION=1
        JUCE_MODAL_LOOPS_PERMITTED=1
        # the hook is unreachable with the diagnostics compiled out, but the
        # shared probe source still has to compile
        VOXMORPH_DIAG_TEST_HOOKS=1
        VOXMORPH_DIAG_ENABLED=0)
    target_link_libraries(VoxMorphDiagBaseline
        PRIVATE juce::juce_audio_utils AnokoeAssets
        PUBLIC  juce::juce_recommended_config_flags)
endif()

# ---- UI verification harness (developer tool, OFF by default) ------------
# Renders the editor offscreen WITH a desktop peer and audits the layout
# numerically. See test/ui_shot.cpp for why the peer matters.
#   cmake -B build -G Ninja -DVOXMORPH_UI_HARNESS=ON
#   cmake --build build --target VoxMorphUiShot
option(VOXMORPH_UI_HARNESS "Build the offscreen UI screenshot/audit tool" OFF)
if (VOXMORPH_UI_HARNESS)
    juce_add_console_app(VoxMorphUiShot PRODUCT_NAME "VoxMorphUiShot")
    target_sources(VoxMorphUiShot PRIVATE test/ui_shot.cpp src/PluginProcessor.cpp)
    target_include_directories(VoxMorphUiShot PRIVATE dsp src)
    target_compile_definitions(VoxMorphUiShot PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_PLUGINHOST_VST3=1
        JUCE_STANDALONE_APPLICATION=1
        JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(VoxMorphUiShot
        PRIVATE juce::juce_audio_utils AnokoeAssets
        PUBLIC  juce::juce_recommended_config_flags)
endif()
