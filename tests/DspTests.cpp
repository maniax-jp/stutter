#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestHelpers.h"
#include "dsp/SceneSelector.h"
#include "dsp/ModulationEngine.h"
#include "dsp/TimingMode.h"
#include "dsp/effects/StutterEffect.h"
#include "dsp/effects/ReverseEffect.h"

using namespace stutter;
using namespace stutter::test;
using Catch::Matchers::WithinAbs;

// ---------------------------------------------------------------------------------------
// Rate labels.
// ---------------------------------------------------------------------------------------

TEST_CASE ("Rate labels name the duration actually produced", "[labels]")
{
    // Through v1.1.2 they did not: the table's fractions are relative to a quarter note but
    // the labels read as bar fractions, so every displayed value was 4x longer than what was
    // heard. Nothing checked the labels against the arithmetic, so the mismatch shipped.
    auto notesPerBarFromLabel = [] (const juce::String& label)
    {
        // Take the denominator only -- retaining all digits folds the leading "1/" into the
        // number and turns "1/16" into 116.
        const double base = (double) label.fromFirstOccurrenceOf ("/", false, false)
                                          .retainCharacters ("0123456789").getIntValue();
        if (label.endsWith ("T")) return base * 1.5;   // triplet: 2/3 length -> 3/2 as many
        if (label.endsWith (".")) return base / 1.5;   // dotted: 3/2 length -> 2/3 as many
        return base;
    };

    constexpr double divisionsPerBar = 16.0;
    for (int i = 0; i < numLegacyRateIndices; ++i)
    {
        const double fraction = legacyRateIndexToFraction (i);
        const double notesPerBar = divisionsPerBar / (fraction / 0.25);
        const juce::String label = legacyRateIndexLabels()[i];

        INFO ("index " << i << " labelled \"" << label << "\"");
        CHECK_THAT (notesPerBarFromLabel (label), WithinAbs (notesPerBar, 0.01));
    }
}

TEST_CASE ("Both parameter surfaces expose the same rate labels", "[labels]")
{
    // The APVTS choice list and the ParamDescriptor list are built separately, so fixing one
    // and not the other would leave the UI and the host disagreeing.
    StutterAudioProcessor proc;
    proc.setPlayConfigDetails (2, 2, sampleRate, blockSize);
    proc.prepareToPlay (sampleRate, blockSize);

    auto checkApvts = [&proc] (int lane, const juce::String& paramName)
    {
        auto* raw = proc.getAPVTS().getParameter (ID::lanePrefix (lane) + paramName);
        auto* choice = dynamic_cast<juce::AudioParameterChoice*> (raw);
        REQUIRE (choice != nullptr);
        REQUIRE (choice->choices.size() == numLegacyRateIndices);
        for (int i = 0; i < numLegacyRateIndices; ++i)
            CHECK (choice->choices[i] == juce::String (legacyRateIndexLabels()[i]));
    };

    checkApvts (lanes::stutterLane, ID::stutterRate);
    checkApvts (lanes::reverse, ID::reverseSliceLen);

    StutterEffect stutterEffect;
    ReverseEffect reverseEffect;
    for (auto* effect : { (LaneEffect*) &stutterEffect, (LaneEffect*) &reverseEffect })
    {
        const auto d = effect->getParamDescriptors()[0];
        REQUIRE (d.numChoices == numLegacyRateIndices);
        for (int i = 0; i < numLegacyRateIndices; ++i)
            CHECK (juce::String (d.choices[i]) == juce::String (legacyRateIndexLabels()[i]));
    }
}

// ---------------------------------------------------------------------------------------
// Block sequencer behaviour that has no v1 counterpart.
// ---------------------------------------------------------------------------------------

TEST_CASE ("A held block lets a directional envelope finish", "[sequencer]")
{
    // This is why variable-length blocks exist. v1's grid restarted TapeStop every 16th, so
    // it never actually reached a stop.
    CaptureBuffer cap;
    cap.prepare (sampleRate, 2, 2.5);

    BlockSequencer seq;
    installAllLaneEffects (seq);
    seq.prepare (sampleRate, 2);
    seq.setEnabled (true);

    auto scene = makeFullLaneScene (seq, lanes::tapeStop);
    auto& lane = scene.lanes[(size_t) lanes::tapeStop];
    lane.blocks[0].startDiv = 0;
    lane.blocks[0].lengthDiv = 8;    // one block spanning half the pattern
    lane.numBlocks = 1;
    seq.updateChainOrder (scene);

    const int total = (int) (sampleRate * 2.0);
    juce::AudioBuffer<float> source (2, total);
    fillTestSignal (source);

    const double pps = (bpm / 60.0) / sampleRate;
    const double divLen = 0.25 / pps;
    double clock = 0.0, tailRms = 0.0;
    int tailCount = 0;

    for (int pos = 0; pos < total; pos += blockSize)
    {
        const int n = juce::jmin (blockSize, total - pos);
        juce::AudioBuffer<float> blk (2, n);
        for (int c = 0; c < 2; ++c) blk.copyFrom (c, 0, source, c, pos, n);
        cap.write (blk);
        seq.processBlock (blk, cap, scene, clock, pps);
        clock += pps * (double) n;

        if ((double) pos > divLen * 7.0 && (double) pos < divLen * 8.0)
        {
            tailRms += rmsOf (blk);
            ++tailCount;
        }
    }

    const double avgTail = tailCount > 0 ? tailRms / (double) tailCount : 1.0;
    INFO ("tail RMS " << avgTail << " against a dry level near 0.5");
    CHECK (avgTail < 0.2);
}

TEST_CASE ("Swing displaces odd boundaries and pins even ones", "[sequencer][swing]")
{
    // Two properties, and the second is the one easily lost: rescaling each division
    // independently moves the odd boundary but also shortens the pattern, which is a tempo
    // change wearing a groove costume.
    BlockSequencer seq;
    installAllLaneEffects (seq);
    seq.prepare (sampleRate, 2);
    seq.setEnabled (true);

    auto boundarySample = [&seq] (float swing, int fromDiv, int toDiv)
    {
        CaptureBuffer cap;
        cap.prepare (sampleRate, 2, 2.5);
        seq.reset();

        SceneSnapshot scene {};
        scene.beats = 4;
        scene.divisions = 4;
        scene.swing = swing;
        scene.populated = true;
        seq.updateChainOrder (scene);

        const double pps = (bpm / 60.0) / sampleRate;
        juce::AudioBuffer<float> blk (2, 1);
        double clock = 0.0;
        int lastDiv = -1;

        for (int i = 0; i < (int) (sampleRate * 0.9); ++i)
        {
            blk.clear();
            cap.write (blk);
            seq.processBlock (blk, cap, scene, clock, pps);
            clock += pps;
            const int d = seq.getPlayheadDivision();
            if (lastDiv == fromDiv && d == toDiv)
                return i;
            lastDiv = d;
        }
        return -1;
    };

    const double divisionSamples = 0.25 / ((bpm / 60.0) / sampleRate);

    SECTION ("the 0->1 boundary moves by half the swing amount")
    {
        const int straight = boundarySample (0.0f, 0, 1);
        const int swung    = boundarySample (0.6f, 0, 1);
        REQUIRE (straight > 0);
        REQUIRE (swung > 0);

        const double expected = 0.3 * divisionSamples;   // swing 0.6 -> 30% of a division
        INFO ("shift " << (swung - straight) << " samples, expected about " << expected);
        CHECK_THAT ((double) (swung - straight), WithinAbs (expected, divisionSamples * 0.05));
    }

    SECTION ("the 1->2 boundary does not move, so the pattern length is unchanged")
    {
        const int straight = boundarySample (0.0f, 1, 2);
        const int swung    = boundarySample (0.6f, 1, 2);
        REQUIRE (straight > 0);
        REQUIRE (swung > 0);
        CHECK (std::abs (swung - straight) <= 2);
    }
}

// ---------------------------------------------------------------------------------------
// Scene selection and the wet gate.
// ---------------------------------------------------------------------------------------

TEST_CASE ("Selecting a scene flags exactly one mirror", "[selector]")
{
    // The mirror is what refills the editor's knobs from the scene being heard. Hosts re-send
    // the same automation value every block, so a selector that flagged on every call would
    // rebuild the mirror continuously; one that never flagged would leave the editor showing
    // the wrong scene.
    SceneSelector s;
    s.prepare (sampleRate);

    s.applyAutomation (60, true);
    CHECK (s.getActiveScene() == 60);
    CHECK (s.consumePendingMirror() == 60);
    CHECK (s.consumePendingMirror() == -1);   // consumed exactly once

    // Re-sending the same value must not flag again.
    s.applyAutomation (60, true);
    CHECK (s.consumePendingMirror() == -1);

    s.applyAutomation (11, true);
    CHECK (s.getActiveScene() == 11);
    CHECK (s.consumePendingMirror() == 11);
}

TEST_CASE ("Scene selection is clamped to the bank", "[selector]")
{
    // A host is free to send anything the parameter range allows, and a corrupt project could
    // send worse. Reading past the bank would be a wild pointer on the audio thread.
    SceneSelector s;
    s.prepare (sampleRate);

    s.applyAutomation (-5, true);
    CHECK (s.getActiveScene() == firstSceneIndex);

    s.applyAutomation (maxScenes + 100, true);
    CHECK (s.getActiveScene() == lastSceneIndex);
}

TEST_CASE ("Scene 0 means unspecified, not scene zero", "[selector]")
{
    // Users type these numbers into an automation lane by hand, so the numbering starts at 1
    // to match what the browser shows. That leaves 0 free to mean "nothing written here",
    // which is what an untouched lane sends -- and an untouched lane must not drag the
    // selection away from whatever was picked in the editor.
    SceneSelector s;
    s.prepare (sampleRate);

    s.applyAutomation (7, true);
    REQUIRE (s.getActiveScene() == 7);
    (void) s.consumePendingMirror();

    s.applyAutomation (noSceneIndex, true);
    CHECK (s.getActiveScene() == 7);              // unchanged
    CHECK (s.consumePendingMirror() == -1);       // and nothing to re-mirror

    // The gate is still the `active` parameter's business, even when no scene is specified.
    s.applyAutomation (noSceneIndex, false);
    for (int i = 0; i < (int) (sampleRate * 0.05); ++i)
        s.nextGateGain();
    CHECK (s.getGateGain() < 0.01f);
    CHECK (s.getActiveScene() == 7);
}

TEST_CASE ("Scene numbering starts at one", "[selector]")
{
    // Slot 0 exists in the array but is never addressable: the number on screen and the
    // number typed into automation have to be the same, and people count slots from 1.
    CHECK (firstSceneIndex == 1);
    CHECK (noSceneIndex == 0);
    CHECK (defaultSceneIndex == firstSceneIndex);
    CHECK (lastSceneIndex == maxScenes - 1);
}

TEST_CASE ("The gate ramps rather than steps", "[selector][click]")
{
    // The gate is a gain multiplied into the wet mix, not a mute on the sequencer. Automation
    // flips `active` at a block boundary, so without the ramp every edge would be a step --
    // and a step in a gain is a click.
    SceneSelector s;
    s.prepare (sampleRate);

    // Opening.
    s.applyAutomation (0, false);
    for (int i = 0; i < (int) (sampleRate * 0.05); ++i)
        s.nextGateGain();
    REQUIRE (s.getGateGain() < 0.01f);

    s.applyAutomation (0, true);
    float previous = s.getGateGain();
    float maxStep = 0.0f;
    for (int i = 0; i < (int) (sampleRate * 0.05); ++i)
    {
        const float g = s.nextGateGain();
        maxStep = juce::jmax (maxStep, std::abs (g - previous));
        previous = g;
    }

    CHECK (s.getGateGain() > 0.99f);
    // 5ms at this rate means no single sample may move more than 1/240th; allow a little
    // slack for the integer rounding in gateRampSamples.
    INFO ("max per-sample step " << maxStep);
    CHECK (maxStep <= 1.0f / 200.0f);

    // Closing has to be just as gradual.
    s.applyAutomation (0, false);
    previous = s.getGateGain();
    maxStep = 0.0f;
    for (int i = 0; i < (int) (sampleRate * 0.05); ++i)
    {
        const float g = s.nextGateGain();
        maxStep = juce::jmax (maxStep, std::abs (g - previous));
        previous = g;
    }

    CHECK (s.getGateGain() < 0.01f);
    CHECK (maxStep <= 1.0f / 200.0f);
}

TEST_CASE ("The gate starts open", "[selector]")
{
    // Matches the `active` parameter's default. Starting closed would silence any path that
    // never reaches applyAutomation, which reads as the plugin being broken on insertion.
    SceneSelector s;
    s.prepare (sampleRate);
    CHECK (s.getGateGain() > 0.99f);
}


// ---------------------------------------------------------------------------------------
// Modulation matrix.
// ---------------------------------------------------------------------------------------

namespace
{
SceneSnapshot makeModScene (float depth, float speed, bool bipolar, float baseValue)
{
    SceneSnapshot s {};
    s.beats = 4;
    s.divisions = 4;
    s.populated = true;
    s.lanes[6].params[1] = baseValue;

    const std::vector<CurvePointV2> ramp {
        { 0.0f, 0.0f, 0.0f, PointWeight::Hard },
        { 1.0f, 1.0f, 0.0f, PointWeight::Hard }
    };
    // These cases reason in plain 0..1, so declare that range for the slot they use. The
    // registry is process-global and a processor-level test may already have registered the
    // real filter range (20Hz..20kHz) for this slot -- without pinning it here, the curve
    // would be mapped onto Hz and every expectation below would be off by orders of
    // magnitude. Registering rather than clearing keeps other suites unaffected.
    {
        std::array<float, maxParamsPerLane> lo {}, hi {};
        hi.fill (1.0f);
        std::array<float, maxParamsPerLane> sk {}; sk.fill (1.0f);
        SceneSchema::setLaneRanges (6, lo.data(), hi.data(), sk.data(), maxParamsPerLane);
    }

    SceneSchema::bakeCurveTable (ramp, s.curves[0].table.data(), CurveSnapshot::tableSize);
    s.curves[0].targetParam = (juce::int16) paramIndex (6, 1);
    s.curves[0].depth = depth;
    s.curves[0].speedMultiplier = speed;
    s.curves[0].bipolar = bipolar;
    s.curves[0].enabled = true;
    s.activeCurves[0] = 0;
    s.numActiveCurves = 1;
    return s;
}
} // namespace

TEST_CASE ("An unrouted parameter reads its scene value", "[modulation]")
{
    SceneSnapshot plain {};
    plain.lanes[6].params[1] = 0.42f;

    ModulationEngine eng;
    eng.prepare (sampleRate);
    CHECK_THAT (eng.nextSample (plain, 0.0)[paramIndex (6, 1)], WithinAbs (0.42f, 1.0e-5f));
    CHECK_FALSE (ModulationEngine::hasActiveRoutes (plain));
}

TEST_CASE ("A routed curve drives its target", "[modulation]")
{
    const auto scene = makeModScene (1.0f, 1.0f, false, 0.0f);
    ModulationEngine eng;
    eng.prepare (sampleRate);

    const float atStart = eng.nextSample (scene, 0.0)[paramIndex (6, 1)];
    for (int i = 0; i < 64; ++i) eng.nextSample (scene, 0.75);
    const float atThreeQuarters = eng.nextSample (scene, 0.75)[paramIndex (6, 1)];

    INFO ("phase 0.00 -> " << atStart << ", phase 0.75 -> " << atThreeQuarters);
    CHECK (atThreeQuarters > atStart + 0.5f);
    CHECK (atThreeQuarters <= 1.0f);
    CHECK (atStart >= 0.0f);
}

TEST_CASE ("The base sets the starting point and the curve offsets from it", "[modulation]")
{
    // Automating a knob a curve is also driving must move the whole modulated range, which
    // is what SE2 and ShaperBox both do. Getting this backwards is easy and invisible.
    ModulationEngine low, high;
    low.prepare (sampleRate);
    high.prepare (sampleRate);

    const auto sceneLow  = makeModScene (0.5f, 1.0f, false, 0.2f);
    const auto sceneHigh = makeModScene (0.5f, 1.0f, false, 0.8f);

    for (int i = 0; i < 64; ++i) { low.nextSample (sceneLow, 0.0); high.nextSample (sceneHigh, 0.0); }
    const float lowV  = low.nextSample (sceneLow, 0.0)[paramIndex (6, 1)];
    const float highV = high.nextSample (sceneHigh, 0.0)[paramIndex (6, 1)];

    INFO ("base 0.2 -> " << lowV << ", base 0.8 -> " << highV);
    CHECK (highV > lowV);
}

TEST_CASE ("The speed multiplier advances the curve proportionally", "[modulation]")
{
    // Probed at pattern phase 0.25, not 0.5: under speed 2 a phase of 0.5 maps to curve
    // phase exactly 1.0 and wraps to 0, putting the saw's discontinuity on the probe and
    // reading 0.0 regardless of whether the multiplier works.
    const auto s1 = makeModScene (1.0f, 1.0f, false, 0.0f);
    const auto s2 = makeModScene (1.0f, 2.0f, false, 0.0f);

    ModulationEngine e1, e2;
    e1.prepare (sampleRate);
    e2.prepare (sampleRate);
    for (int i = 0; i < 64; ++i) { e1.nextSample (s1, 0.25); e2.nextSample (s2, 0.25); }

    CHECK_THAT (e1.nextSample (s1, 0.25)[paramIndex (6, 1)], WithinAbs (0.25f, 0.02f));
    CHECK_THAT (e2.nextSample (s2, 0.25)[paramIndex (6, 1)], WithinAbs (0.50f, 0.02f));
}

TEST_CASE ("Modulation interpolates between control-rate updates", "[modulation]")
{
    // Control-rate evaluation only works if the interpolation is real; without it a swept
    // cutoff would move in 16-sample stairs and zipper.
    const auto scene = makeModScene (1.0f, 1.0f, false, 0.0f);
    ModulationEngine eng;
    eng.prepare (sampleRate);

    float prev = eng.nextSample (scene, 0.0)[paramIndex (6, 1)];
    float maxStep = 0.0f;
    constexpr int steps = 2000;
    for (int i = 1; i < steps; ++i)
    {
        const float v = eng.nextSample (scene, (double) i / (double) steps)[paramIndex (6, 1)];
        maxStep = juce::jmax (maxStep, std::abs (v - prev));
        prev = v;
    }

    INFO ("max per-sample modulation step " << maxStep);
    CHECK (maxStep < 0.005f);   // a 16-sample staircase would show about 0.008
}

TEST_CASE ("A fully-loaded scene stays inside the CPU budget", "[modulation][perf]")
{
    // 16 curves x 12 lanes x 12 params per sample at 48k is millions of lookups a second.
    // The mitigations (baked route list, control-rate evaluation) are only worth having if
    // they are measured.
    SceneSnapshot heavy {};
    heavy.beats = 4;
    heavy.divisions = 4;
    heavy.populated = true;

    const std::vector<CurvePointV2> pts {
        { 0.0f, 0.0f,  0.0f,  PointWeight::Hard },
        { 0.5f, 1.0f,  0.3f,  PointWeight::Hard },
        { 1.0f, 0.0f, -0.3f,  PointWeight::Hard }
    };
    for (int i = 0; i < maxCurves; ++i)
    {
        SceneSchema::bakeCurveTable (pts, heavy.curves[(size_t) i].table.data(),
                                     CurveSnapshot::tableSize);
        heavy.curves[(size_t) i].targetParam =
            (juce::int16) paramIndex (i % maxLanes, i % maxParamsPerLane);
        heavy.curves[(size_t) i].depth = 1.0f;
        heavy.curves[(size_t) i].speedMultiplier = 1.0f;
        heavy.curves[(size_t) i].enabled = true;
        heavy.activeCurves[(size_t) i] = (juce::int16) i;
    }
    heavy.numActiveCurves = maxCurves;

    ModulationEngine eng;
    eng.prepare (sampleRate);

    const int totalSamples = (int) (sampleRate * 10.0);
    const double start = juce::Time::getMillisecondCounterHiRes();
    double sink = 0.0;
    for (int i = 0; i < totalSamples; ++i)
        sink += eng.nextSample (heavy, (double) (i % 96000) / 96000.0)[0];
    const double elapsedMs = juce::Time::getMillisecondCounterHiRes() - start;
    const double cpuPercent = 100.0 * elapsedMs / (10.0 * 1000.0);

    juce::ignoreUnused (sink);
    INFO (maxCurves << " curves x " << totalParamSlots << " slots: " << cpuPercent << "% of realtime");
    CHECK (cpuPercent < 30.0);
}
