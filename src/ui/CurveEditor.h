#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "StutterLookAndFeel.h"
#include "../dsp/CurveModulator.h"
#include "../PluginProcessor.h" // for stutter::ModTarget

namespace stutter::ui
{

/**
    Editor for one of the three global modulation curves (Volume / Filter / Pan).
    - Left-drag a breakpoint to move it (position/value)
    - Double-click empty space to add a point, double-click a point to delete it
    - Right-drag (or alt/cmd-drag) on a segment adjusts its curvature
    - Preset buttons apply canonical shapes via CurveModulator::applyPreset
    - Edits call CurveModulator::setPoints/applyPreset on the UI thread only,
      per SPEC (never touched from the audio thread).
*/
class CurveEditor : public juce::Component, private juce::Timer
{
public:
    explicit CurveEditor (StutterAudioProcessor& processor, stutter::ModTarget target, juce::Colour accent);
    ~CurveEditor() override;

    /** Called by BottomTabs::refreshAfterPresetLoad() right after a preset finishes loading, so
        the sync-division combo box (which isn't an APVTS-bound control, since syncDiv lives in
        the Curves structural ValueTree, not a parameter) picks up the newly-loaded value
        immediately rather than waiting for the next timer poll. */
    void refreshAfterPresetLoad() { refreshSyncDivCombo(); repaint(); }

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;

private:
    void timerCallback() override;

    /** Re-syncs syncDivCombo's selected item from curve().getSyncDivision() without firing
        onChange (used on construction, on the timer poll, and explicitly right after a preset
        load via BottomTabs::refreshAfterPresetLoad -> CurveEditor::refreshAfterPresetLoad). */
    void refreshSyncDivCombo();

    stutter::CurveModulator& curve() const;

    juce::Rectangle<float> getPlotArea() const;
    juce::Point<float> pointToXY (const stutter::CurvePoint& p, juce::Rectangle<float> plot) const;
    stutter::CurvePoint xyToPoint (juce::Point<float> xy, juce::Rectangle<float> plot) const;

    int findNearestPointIndex (juce::Point<float> screenPos, juce::Rectangle<float> plot, float maxDist) const;
    int findSegmentIndex (float screenX, juce::Rectangle<float> plot) const;

    void pushPoints (std::vector<stutter::CurvePoint> pts);

    StutterAudioProcessor& proc;
    stutter::ModTarget modTarget;
    juce::Colour accentColour;

    juce::TextButton enableButton { "ON" };
    std::vector<std::unique_ptr<juce::TextButton>> presetButtons;
    juce::ComboBox syncDivCombo;

    // interaction state
    int draggingPointIndex = -1;
    int curvatureSegmentIndex = -1;
    bool isCurvatureDrag = false;
    juce::Point<float> lastDragScreenPos;
    int hoverPointIndex = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CurveEditor)
};

} // namespace stutter::ui
