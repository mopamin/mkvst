cmake_minimum_required(VERSION 3.22)
project(Mkvst04 VERSION 0.4.0)

# JUCE (Version 8) liegt als Unterordner "JUCE" neben dieser Datei
add_subdirectory(JUCE)

juce_add_plugin(Mkvst04
    COMPANY_NAME "Sarin"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    PLUGIN_MANUFACTURER_CODE Sarn
    PLUGIN_CODE Mk04
    FORMATS VST3 AU Standalone
    COPY_PLUGIN_AFTER_BUILD TRUE
    VERSION 0.4.0
    PLUGIN_NAME "mkvst0_4"
    PRODUCT_NAME "mkvst0_4"
    DESCRIPTION "mkvst0_4"
    BUNDLE_ID com.sarin.mkvst04
    VST3_CATEGORIES Instrument Synth)

target_sources(Mkvst04 PRIVATE
    Source/Mkvst04Processor.cpp
    Source/Mkvst04Processor.h
    Source/Mkvst04Editor.cpp
    Source/Mkvst04Editor.h
    Source/Mkvst04Voice.h)

target_compile_definitions(Mkvst04 PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Mkvst04
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
