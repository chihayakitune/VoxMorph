cmake_minimum_required(VERSION 3.22)
project(VoxMorph VERSION 0.8.0)

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

if (MSVC)
    target_compile_options(VoxMorph PRIVATE /utf-8)   # 日本語UI文字列のため
endif()

target_compile_definitions(VoxMorph PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(VoxMorph
    PRIVATE juce::juce_audio_utils
    PUBLIC  juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags
)
