#pragma once
#include <juce_core/juce_core.h>

namespace stutter::ID
{
// ---- Global ----
static const juce::String dryWet        = "dryWet";
static const juce::String outputGain    = "outputGain";
static const juce::String sequencerOn   = "sequencerOn";
static const juce::String hostSync      = "hostSync";
static const juce::String internalBpm   = "internalBpm";

// ---- Per-lane parameter prefix helper ----
// Lane indices 0..7 map to: 0 Stutter, 1 TapeStop, 2 TapeStart, 3 Reverse,
//                           4 Repitch, 5 Gate, 6 Filter, 7 Crush
inline juce::String lanePrefix (int laneIndex) { return "lane" + juce::String (laneIndex) + "_"; }

// Common per-lane params
static const juce::String laneRate       = "rate";      // tempo-synced rate index (shared meaning per-lane)

// Stutter (lane 0)
static const juce::String stutterRate       = "rate";
static const juce::String stutterDecay      = "decay";      // loop-length decay amount
static const juce::String stutterPitchSlide = "pitchSlide"; // semitones slide over repeats

// TapeStop (lane 1)
static const juce::String tapeStopCurve = "curve"; // 0=linear .. 1=exponential
static const juce::String tapeStopTime  = "time";  // decel time scale

// TapeStart (lane 2)
static const juce::String tapeStartCurve = "curve";
static const juce::String tapeStartTime  = "time";

// Reverse (lane 3)
static const juce::String reverseSliceLen = "sliceLen"; // in tempo-synced divisions

// Repitch (lane 4)
static const juce::String repitchSemitones = "semitones";
static const juce::String repitchSlide     = "slide";

// Gate (lane 5)
static const juce::String gateRate  = "rate";
static const juce::String gateDuty  = "duty";
static const juce::String gateShape = "shape"; // 0 = square .. 1 = sine

// Filter (lane 6)
static const juce::String filterType     = "type"; // 0 LP, 1 BP, 2 HP
static const juce::String filterCutoff   = "cutoff";
static const juce::String filterResonance = "resonance";
static const juce::String filterLfoRate  = "lfoRate";
static const juce::String filterLfoDepth = "lfoDepth";

// Crush (lane 7)
static const juce::String crushBitDepth   = "bitDepth";
static const juce::String crushRateDiv    = "rateDiv";

// ---- Step grid & curves are stored as raw ValueTree children of the APVTS state,
//      NOT as APVTS parameters (they are structural/pattern data, not automatable). ----
static const juce::Identifier stateRoot       { "StutterState" };
static const juce::Identifier sequencerNode   { "Sequencer" };
static const juce::Identifier laneNode        { "Lane" };
static const juce::Identifier stepNode        { "Step" };
static const juce::Identifier curvesNode      { "Curves" };
static const juce::Identifier curveNode       { "Curve" };
static const juce::Identifier pointNode       { "Point" };

static const juce::Identifier propIndex     { "index" };
static const juce::Identifier propOn        { "on" };
static const juce::Identifier propName      { "name" };
static const juce::Identifier propEnabled   { "enabled" };
static const juce::Identifier propSyncDiv   { "syncDiv" };
static const juce::Identifier propPosition  { "position" };  // 0..1 across the cycle
static const juce::Identifier propValue     { "value" };     // 0..1 modulation value
static const juce::Identifier propCurvature { "curvature" }; // -1..1, 0 = linear

// ---- Curve neutral values: single source of truth ----
// Every modulation curve's 0..1 range maps to a "no-op" flat value differently depending on
// what it modulates. Volume (0..1 -> 0..2x gain) and Pan (0..1 -> -1..1 pan pos) are both
// neutral at 0.5 (unity gain / center pan). Filter (0..1 -> 200Hz..20kHz exponential) is neutral
// at 1.0 (fully open, ~20kHz, i.e. leaves the signal untouched) -- 0.5 there would be an audible
// ~2kHz lowpass. This is the ONE place these numbers are hardcoded; CurveModulator's default
// construction, PluginProcessor's curves array, and PresetManager's preset-building all pull
// from here rather than each hardcoding their own copy.
static const juce::String curveNameVolume = "Volume";
static const juce::String curveNameFilter = "Filter";
static const juce::String curveNamePan    = "Pan";

inline float neutralValueForCurve (const juce::String& curveName) noexcept
{
    return curveName == curveNameFilter ? 1.0f : 0.5f;
}

} // namespace stutter::ID
