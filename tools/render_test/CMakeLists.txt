# Offline verification harness: instantiates StutterAudioProcessor directly (no plugin
# wrapper/host needed) and renders each lane in isolation to a WAV file, then reports
# discontinuity metrics. Used to prove the before/after effect of the noise-fix changes.
# Not part of the shipped plugin: only built when STUTTER_BUILD_TESTS=ON.

add_executable(render_test
    render_test.cpp
    ${CMAKE_SOURCE_DIR}/src/PluginProcessor.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/ParameterLayout.cpp
    ${CMAKE_SOURCE_DIR}/src/PluginEditor.cpp
    ${CMAKE_SOURCE_DIR}/src/PresetManager.cpp
    ${CMAKE_SOURCE_DIR}/src/FactoryPresets.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/StutterLookAndFeel.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/HeaderBar.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/StepGrid.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/LaneParamPanel.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/CurveEditor.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/BottomTabs.cpp
)

target_include_directories(render_test
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

# PluginProcessor.cpp only references JucePlugin_Name (as a value) and guards
# JucePlugin_PreferredChannelConfigurations with #ifndef, so that's the only plugin-client
# define this standalone harness needs to supply.
target_compile_definitions(render_test
    PRIVATE
        JucePlugin_Name="Stutter"
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=0
        # Enables the slice-window read-range violation counter in the lane effects
        # (stutter::debug::sliceRangeViolations); never defined for the shipped plugin.
        STUTTER_RANGE_CHECK=1
)

target_link_libraries(render_test
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_audio_processors
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
