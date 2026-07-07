#pragma once
#include <juce_data_structures/juce_data_structures.h>
#include <vector>

namespace stutter
{

/** One factory preset's raw definition, in a form that's easy to hand-author from
    .tmp/PRESETS_DESIGN.md and easy to turn into a full state ValueTree (see
    FactoryPresets::buildStateTree()). Any field left at its default reproduces the SPEC's
    documented default value for that parameter/step/curve. */
struct FactoryPresetDef
{
    struct ParamValue
    {
        juce::String paramId; // full APVTS id, e.g. "lane0_rate" or "dryWet"
        float value = 0.0f;   // denormalised (real) value: choice index, dB, Hz, 0..1, etc.
    };

    struct StepOn
    {
        int lane = 0;
        int step = 0; // 0-based
    };

    struct CurvePointDef
    {
        float position = 0.0f;
        float value = 0.5f;
        float curvature = 0.0f;
    };

    struct CurveDef
    {
        juce::String name; // "Volume" | "Filter" | "Pan"
        bool enabled = false;
        int syncDiv = 4; // index into the CurveModulator/UI sync-division table; 4 == 1/4 bar-ish default
        std::vector<CurvePointDef> points; // empty == leave curve at its default flat line
    };

    juce::String name;
    juce::String category;
    std::vector<ParamValue> paramValues; // only params that differ from the default layout
    std::vector<StepOn> stepsOn;         // only steps that are ON (all others OFF)
    std::vector<CurveDef> curves;        // only curves that differ from "disabled, flat, syncDiv=4"
};

/** All 28 designed factory presets (.tmp/PRESETS_DESIGN.md), in catalogue order.
    "Init" is intentionally NOT included here -- PresetManager::buildInitState() covers it
    separately as preset index 0, matching the SPEC's "Init as preset 0" requirement. */
const std::vector<FactoryPresetDef>& getFactoryPresetDefs();

} // namespace stutter
