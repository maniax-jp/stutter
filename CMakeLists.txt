cmake_minimum_required(VERSION 3.22)

project(Stutter VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment target")

option(STUTTER_COPY_PLUGIN "Copy built plugin formats to ~/Library after build" ON)

include(FetchContent)

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.8
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(Stutter
    COMPANY_NAME "Maniax"
    BUNDLE_ID "com.maniax.stutter"
    PLUGIN_MANUFACTURER_CODE Manx
    PLUGIN_CODE Stt1
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "Stutter"
    COPY_PLUGIN_AFTER_BUILD ${STUTTER_COPY_PLUGIN}
    VST3_CATEGORIES Fx
    AU_MAIN_TYPE kAudioUnitType_Effect
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_AUTO_MANIFEST FALSE
)

target_sources(Stutter
    PRIVATE
        src/PluginProcessor.cpp
        src/dsp/ParameterLayout.cpp
        src/PluginEditor.cpp
        src/PresetManager.cpp
        src/FactoryPresets.cpp
    src/FactoryScenes.cpp
        src/ui/StutterLookAndFeel.cpp
        src/ui/HeaderBar.cpp
        src/ui/BlockGrid.cpp
    src/ui/SceneBrowser.cpp
    src/ui/ModRoutePanel.cpp
    src/ui/PerformanceBar.cpp
        src/ui/LaneParamPanel.cpp
        src/ui/CurveEditor.cpp
        src/ui/BottomTabs.cpp
        src/state/SceneStore.cpp
        src/state/SceneSchema.cpp
)

target_include_directories(Stutter
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(Stutter
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_ALSA=0
)

target_link_libraries(Stutter
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# ---- Tests (optional, not part of the shipped plugin) ----
#
# Two targets with different jobs:
#   stutter_tests  Catch2 unit/integration suites. This is the gate.
#   render_test    renders each lane to a WAV for listening. Not a unit test -- producing
#                  audio a human can audition is not something Catch2 should be asked to do,
#                  and the golden-baseline gate consumes its output.
option(STUTTER_BUILD_TESTS "Build the test targets" OFF)
if (STUTTER_BUILD_TESTS)
    # Both test targets compile the plugin sources directly rather than linking the plugin
    # target, because juce_add_plugin produces format wrappers rather than a plain library.
    # Shared here so the two lists cannot drift -- they did once, and the symptom was a
    # target failing to link for reasons unrelated to the change being made.
    set(STUTTER_PLUGIN_SOURCES
        ${CMAKE_SOURCE_DIR}/src/PluginProcessor.cpp
        ${CMAKE_SOURCE_DIR}/src/PluginEditor.cpp
        ${CMAKE_SOURCE_DIR}/src/PresetManager.cpp
        ${CMAKE_SOURCE_DIR}/src/FactoryPresets.cpp
        ${CMAKE_SOURCE_DIR}/src/FactoryScenes.cpp
        ${CMAKE_SOURCE_DIR}/src/dsp/ParameterLayout.cpp
        ${CMAKE_SOURCE_DIR}/src/state/SceneStore.cpp
        ${CMAKE_SOURCE_DIR}/src/state/SceneSchema.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/StutterLookAndFeel.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/HeaderBar.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/BlockGrid.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/SceneBrowser.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/ModRoutePanel.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/PerformanceBar.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/LaneParamPanel.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/CurveEditor.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/BottomTabs.cpp
    )

    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.2
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(Catch2)

    enable_testing()
    add_subdirectory(tests)
    add_subdirectory(tools/render_test)
endif()
