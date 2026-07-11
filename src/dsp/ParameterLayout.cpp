#include "ParameterLayout.h"
#include "ParameterIDs.h"

namespace
{
juce::String laneParamId (int lane, const juce::String& name) { return stutter::ID::lanePrefix (lane) + name; }

juce::StringArray rateChoices()
{
    return { "1/4", "1/8", "1/16", "1/32", "1/64", "1/4T", "1/8T", "1/16T", "1/4.", "1/8.", "1/16." };
}
} // namespace

namespace stutter
{
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // ---- Global ----
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ID::dryWet, 1 }, "Dry/Wet",
        juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ID::outputGain, 1 }, "Output Gain",
        juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { ID::sequencerOn, 1 }, "Sequencer On", true));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { ID::hostSync, 1 }, "Host Sync", true));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ID::internalBpm, 1 }, "Internal BPM",
        juce::NormalisableRange<float> (40.0f, 240.0f), 120.0f));

    auto addRateChoice = [&params] (int lane, const juce::String& name, const juce::String& label, int defaultIndex)
    {
        params.push_back (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { laneParamId (lane, name), 1 }, label, rateChoices(), defaultIndex));
    };

    auto addFloat = [&params] (int lane, const juce::String& name, const juce::String& label,
                                juce::NormalisableRange<float> range, float def, const juce::String& unit = {})
    {
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { laneParamId (lane, name), 1 }, label, range, def,
            juce::AudioParameterFloatAttributes().withLabel (unit)));
    };

    // ---- Lane 0: Stutter ----
    addRateChoice (lanes::stutterLane, ID::stutterRate, "Stutter Rate", 2);
    addFloat (lanes::stutterLane, ID::stutterDecay, "Stutter Decay",
              { 0.0f, 1.0f }, 0.0f);
    addFloat (lanes::stutterLane, ID::stutterPitchSlide, "Stutter Pitch Slide",
              { -24.0f, 24.0f }, 0.0f, "st");

    // ---- Lane 1: TapeStop ----
    addFloat (lanes::tapeStop, ID::tapeStopCurve, "TapeStop Curve", { 0.0f, 1.0f }, 0.5f);
    addFloat (lanes::tapeStop, ID::tapeStopTime, "TapeStop Time", { 0.0f, 1.0f }, 0.5f);

    // ---- Lane 2: TapeStart ----
    addFloat (lanes::tapeStart, ID::tapeStartCurve, "TapeStart Curve", { 0.0f, 1.0f }, 0.5f);
    addFloat (lanes::tapeStart, ID::tapeStartTime, "TapeStart Time", { 0.0f, 1.0f }, 0.5f);

    // ---- Lane 3: Reverse ----
    addRateChoice (lanes::reverse, ID::reverseSliceLen, "Reverse Slice Length", 2);

    // ---- Lane 4: Repitch ----
    addFloat (lanes::repitch, ID::repitchSemitones, "Repitch Semitones",
              { -24.0f, 24.0f }, -12.0f, "st");
    addFloat (lanes::repitch, ID::repitchSlide, "Repitch Slide", { 0.0f, 1.0f }, 0.0f);

    // ---- Lane 5: Gate ----
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { laneParamId (lanes::gate, ID::gateRate), 1 }, "Gate Rate",
        juce::StringArray { "1/1", "1/2", "1/3", "1/4", "1/6", "1/8", "1/12", "1/16" }, 3));
    addFloat (lanes::gate, ID::gateDuty, "Gate Duty", { 0.01f, 0.99f }, 0.5f);
    addFloat (lanes::gate, ID::gateShape, "Gate Shape", { 0.0f, 1.0f }, 0.0f);

    // ---- Lane 6: Filter ----
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { laneParamId (lanes::filter, ID::filterType), 1 }, "Filter Type",
        juce::StringArray { "Low Pass", "Band Pass", "High Pass" }, 0));
    addFloat (lanes::filter, ID::filterCutoff, "Filter Cutoff",
              juce::NormalisableRange<float> (20.0f, 20000.0f, 1.0f, 0.3f), 1000.0f, "Hz");
    addFloat (lanes::filter, ID::filterResonance, "Filter Resonance", { 0.0f, 0.99f }, 0.2f);
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { laneParamId (lanes::filter, ID::filterLfoRate), 1 }, "Filter LFO Rate",
        juce::StringArray { "1/4", "1/2", "1/1", "2/1", "4/1", "8/1" }, 2));
    addFloat (lanes::filter, ID::filterLfoDepth, "Filter LFO Depth", { 0.0f, 1.0f }, 0.0f);

    // ---- Lane 7: Crush ----
    addFloat (lanes::crush, ID::crushBitDepth, "Crush Bit Depth", { 1.0f, 16.0f, 1.0f }, 16.0f, "bit");
    addFloat (lanes::crush, ID::crushRateDiv, "Crush Rate Div", { 0.0f, 1.0f }, 0.0f);

    return { params.begin(), params.end() };
}
} // namespace stutter
