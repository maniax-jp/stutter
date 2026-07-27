#include "TestHelpers.h"

#include "dsp/effects/StutterEffect.h"
#include "dsp/effects/TapeStopEffect.h"
#include "dsp/effects/TapeStartEffect.h"
#include "dsp/effects/ReverseEffect.h"
#include "dsp/effects/RepitchEffect.h"
#include "dsp/effects/GateEffect.h"
#include "dsp/effects/FilterEffect.h"
#include "dsp/effects/CrushEffect.h"
#include "dsp/effects/StretcherEffect.h"
#include "dsp/effects/ShufflerEffect.h"
#include "dsp/effects/DelayEffect.h"
#include "dsp/effects/DistortionEffect.h"

namespace stutter::test
{

void installAllLaneEffects (BlockSequencer& seq)
{
    seq.setLaneEffect (lanes::stutterLane, std::make_unique<StutterEffect>());
    seq.setLaneEffect (lanes::tapeStop,    std::make_unique<TapeStopEffect>());
    seq.setLaneEffect (lanes::tapeStart,   std::make_unique<TapeStartEffect>());
    seq.setLaneEffect (lanes::reverse,     std::make_unique<ReverseEffect>());
    seq.setLaneEffect (lanes::repitch,     std::make_unique<RepitchEffect>());
    seq.setLaneEffect (lanes::gate,        std::make_unique<GateEffect>());
    seq.setLaneEffect (lanes::filter,      std::make_unique<FilterEffect>());
    seq.setLaneEffect (lanes::crush,       std::make_unique<CrushEffect>());
    seq.setLaneEffect (lanes::stretcher,   std::make_unique<StretcherEffect>());
    seq.setLaneEffect (lanes::shuffler,    std::make_unique<ShufflerEffect>());
    seq.setLaneEffect (lanes::delay,       std::make_unique<DelayEffect>());
    seq.setLaneEffect (lanes::distort,     std::make_unique<DistortionEffect>());
}

SceneSnapshot makeFullLaneScene (BlockSequencer& seq, int lane, int beats, int divisions)
{
    SceneSnapshot scene {};
    scene.beats = beats;
    scene.divisions = divisions;
    scene.populated = true;

    const int totalDivs = juce::jmin (beats * divisions, maxBlocksPerLane);
    auto& laneSnap = scene.lanes[(size_t) lane];
    for (int d = 0; d < totalDivs; ++d)
    {
        laneSnap.blocks[(size_t) d].startDiv = (juce::int16) d;
        laneSnap.blocks[(size_t) d].lengthDiv = 1;
    }
    laneSnap.numBlocks = totalDivs;

    // Seed the lane's parameters from its descriptors. A snapshot built by hand starts at
    // all-zero, and a Filter at cutoff 0 or a Gate at duty 0 is silent -- so without this a
    // test would render nothing and look like a DSP failure rather than a fixture mistake.
    if (auto* effect = seq.getLaneEffect (lane))
    {
        const auto set = effect->getParamDescriptors();
        for (int i = 0; i < set.count && i < maxParamsPerLane; ++i)
            laneSnap.params[(size_t) i] = set[i].defaultValue;
    }

    return scene;
}

} // namespace stutter::test
