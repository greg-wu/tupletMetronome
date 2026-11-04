cmake_minimum_required(VERSION 3.22)

project(SubMetronome VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(external/JUCE)

juce_add_plugin(SubMetronome
    COMPANY_NAME                 "SubMetronome"
    PLUGIN_CODE                  "SbmT"
    PLUGIN_MANUFACTURER_CODE     "GrWu"
    FORMATS                      VST3 Standalone
    PRODUCT_NAME                 "SubMetronome"
    IS_SYNTH                     FALSE
    NEEDS_MIDI_INPUT             FALSE
    NEEDS_MIDI_OUTPUT            FALSE
    IS_MIDI_EFFECT               FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS  FALSE
    VST3_CATEGORIES              "Tools")

# 生成 <JuceHeader.h>，供 source/PluginProcessor.h 与 PluginEditor.h 使用
juce_generate_juce_header(SubMetronome)

target_sources(SubMetronome PRIVATE
    source/PluginProcessor.cpp
    source/PluginEditor.cpp)

target_compile_definitions(SubMetronome PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

# 源码含中文注释：统一按 UTF-8 解释，消除 MSVC 在 936 代码页下的 C4819 警告
if(MSVC)
    target_compile_options(SubMetronome PRIVATE /utf-8)
endif()

target_link_libraries(SubMetronome
    PRIVATE   juce::juce_audio_utils
    PUBLIC    juce::juce_recommended_config_flags
              juce::juce_recommended_lto_flags
              juce::juce_recommended_warning_flags)
