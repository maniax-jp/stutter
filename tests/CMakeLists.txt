# Catch2 unit and integration suites. This is the gate: render_test still exists, but its
# job is producing WAVs a human can audition, which is not something a test framework should
# be asked to do.

add_executable(stutter_tests
    TestHelpers.cpp
    StateTests.cpp
    DspTests.cpp
    IntegrationTests.cpp
    ${STUTTER_PLUGIN_SOURCES}
)

target_include_directories(stutter_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# PluginProcessor.cpp references JucePlugin_Name as a value and guards
# JucePlugin_PreferredChannelConfigurations with #ifndef, so that is the only plugin-client
# define a standalone host-less binary needs to supply.
target_compile_definitions(stutter_tests
    PRIVATE
        JucePlugin_Name="Stutter"
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=0
        # Enables the slice-window read-range counter in the lane effects
        # (stutter::debug::sliceRangeViolations); never defined for the shipped plugin.
        STUTTER_RANGE_CHECK=1
)

target_link_libraries(stutter_tests
    PRIVATE
        Catch2::Catch2WithMain
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_audio_processors
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# Register with ctest as a single entry rather than via catch_discover_tests: discovery runs
# the binary at build time to enumerate cases, which doubles the cost of every build for no
# benefit here -- the suites are fast and a failure names its own case anyway.
add_test(NAME stutter_tests COMMAND stutter_tests)
