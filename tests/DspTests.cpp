#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestHelpers.h"
#include "dsp/GestureEngine.h"
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
// Gesture layer.
// ---------------------------------------------------------------------------------------

namespace
{
juce::MidiBuffer noteOn (int note, int offset = 0)
{
    juce::MidiBuffer b;
    b.addEvent (juce::MidiMessage::noteOn (1, note, 1.0f), offset);
    return b;
}

juce::MidiBuffer noteOff (int note, int offset = 0)
{
    juce::MidiBuffer b;
    b.addEvent (juce::MidiMessage::noteOff (1, note), offset);
    return b;
}

constexpr double ppqPerSample = (bpm / 60.0) / sampleRate;
} // namespace

TEST_CASE ("A note selects a scene, subject to Scene Lock", "[gesture]")
{
    GestureEngine g;
    g.prepare (sampleRate);
    g.setIdentityMapping();
    g.setPlayMode (PlayMode::Midi);

    g.processMidi (noteOn (60), blockSize, 0.0, ppqPerSample, ReleaseMode::Instant);
    CHECK (g.getActiveScene() == 60);
    CHECK (g.consumePendingMirror() == 60);
    CHECK (g.consumePendingMirror() == -1);   // consumed once

    g.setSceneLock (true);
    g.processMidi (noteOn (72), blockSize, 0.0, ppqPerSample, ReleaseMode::Instant);
    CHECK (g.getActiveScene() == 60);

    g.setSceneLock (false);
    g.processMidi (noteOn (72), blockSize, 0.0, ppqPerSample, ReleaseMode::Instant);
    CHECK (g.getActiveScene() == 72);
}

TEST_CASE ("The gate ramps rather than steps", "[gesture][click]")
{
    // The gate is a ramp on the mix rather than a mute on the sequencer specifically so that
    // every transition is click-free by construction. A regression to a hard mute would be
    // silent in a spectrum plot and obvious on a note-off.
    GestureEngine g;
    g.prepare (sampleRate);
    g.setIdentityMapping();
    g.setPlayMode (PlayMode::Midi);

    float maxStep = 0.0f;
    float prev = g.getGateGain();
    auto run = [&] (int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const float gain = g.nextGateGain();
            maxStep = juce::jmax (maxStep, std::abs (gain - prev));
            prev = gain;
        }
    };

    g.processMidi (noteOn (60), blockSize, 0.0, ppqPerSample, ReleaseMode::Instant);
    run (blockSize);
    CHECK (g.getGateGain() > 0.99f);

    g.processMidi (noteOff (60), blockSize, 0.0, ppqPerSample, ReleaseMode::Instant);
    run (blockSize);
    CHECK (g.getGateGain() < 0.01f);

    INFO ("max per-sample gate step " << maxStep);
    CHECK (maxStep <= 1.0f / 200.0f);   // a 5ms ramp at 48k is 240 samples
}

TEST_CASE ("Auto mode holds the gate open", "[gesture]")
{
    GestureEngine g;
    g.prepare (sampleRate);
    g.setIdentityMapping();
    g.setPlayMode (PlayMode::Auto);

    g.processMidi ({}, blockSize, 0.0, ppqPerSample, ReleaseMode::Instant);
    for (int i = 0; i < blockSize; ++i) g.nextGateGain();
    CHECK (g.getGateGain() > 0.99f);
}

TEST_CASE ("Release modes", "[gesture]")
{
    SECTION ("Latch ignores note-off")
    {
        GestureEngine g;
        g.prepare (sampleRate);
        g.setIdentityMapping();
        g.setPlayMode (PlayMode::Midi);

        g.processMidi (noteOn (60), blockSize, 0.0, ppqPerSample, ReleaseMode::Latch);
        for (int i = 0; i < blockSize; ++i) g.nextGateGain();
        g.processMidi (noteOff (60), blockSize, 0.0, ppqPerSample, ReleaseMode::Latch);
        for (int i = 0; i < blockSize; ++i) g.nextGateGain();
        CHECK (g.getGateGain() > 0.99f);
    }

    SECTION ("OnGrid holds until the boundary, then closes")
    {
        GestureEngine g;
        g.prepare (sampleRate);
        g.setIdentityMapping();
        g.setPlayMode (PlayMode::Midi);
        g.setTriggerQuantize (1.0);

        double clock = 0.0;
        g.processMidi (noteOn (60), blockSize, clock, ppqPerSample, ReleaseMode::OnGrid);
        for (int i = 0; i < blockSize; ++i) g.nextGateGain();
        clock += ppqPerSample * blockSize;

        g.processMidi (noteOff (60), blockSize, clock, ppqPerSample, ReleaseMode::OnGrid);
        for (int i = 0; i < blockSize; ++i) g.nextGateGain();
        clock += ppqPerSample * blockSize;
        CHECK (g.getGateGain() > 0.99f);

        while (clock < 1.2)
        {
            g.processMidi ({}, blockSize, clock, ppqPerSample, ReleaseMode::OnGrid);
            for (int i = 0; i < blockSize; ++i) g.nextGateGain();
            clock += ppqPerSample * blockSize;
        }
        CHECK (g.getGateGain() < 0.01f);
    }
}

TEST_CASE ("Quantization accepts an early note", "[gesture]")
{
    // A note arriving just before a boundary is aiming at that boundary, not the previous
    // one. This is what lets a player anticipate the beat instead of chasing it.
    GestureEngine g;
    g.prepare (sampleRate);
    g.setIdentityMapping();
    g.setPlayMode (PlayMode::Midi);
    g.setTriggerQuantize (1.0);

    g.processMidi (noteOn (64), blockSize, 0.9, ppqPerSample, ReleaseMode::Instant);
    CHECK (g.getActiveScene() != 64);   // waits rather than firing late

    double clock = 0.9;
    for (int b = 0; b < 40 && g.getActiveScene() != 64; ++b)
    {
        g.processMidi ({}, blockSize, clock, ppqPerSample, ReleaseMode::Instant);
        clock += ppqPerSample * blockSize;
    }
    CHECK (g.getActiveScene() == 64);

    GestureEngine late;
    late.prepare (sampleRate);
    late.setIdentityMapping();
    late.setPlayMode (PlayMode::Midi);
    late.setTriggerQuantize (1.0);
    late.processMidi (noteOn (67), blockSize, 2.1, ppqPerSample, ReleaseMode::Instant);
    CHECK (late.getActiveScene() == 67);   // well past the boundary: fire at once
}

TEST_CASE ("Only the last released note ends the gesture", "[gesture]")
{
    GestureEngine g;
    g.prepare (sampleRate);
    g.setIdentityMapping();
    g.setPlayMode (PlayMode::Midi);

    g.processMidi (noteOn (60), blockSize, 0.0, ppqPerSample, ReleaseMode::Instant);
    g.processMidi (noteOn (64), blockSize, 0.0, ppqPerSample, ReleaseMode::Instant);
    for (int i = 0; i < blockSize; ++i) g.nextGateGain();

    g.processMidi (noteOff (60), blockSize, 0.0, ppqPerSample, ReleaseMode::Instant);
    for (int i = 0; i < blockSize; ++i) g.nextGateGain();
    CHECK (g.getGateGain() > 0.99f);

    g.processMidi (noteOff (64), blockSize, 0.0, ppqPerSample, ReleaseMode::Instant);
    for (int i = 0; i < blockSize; ++i) g.nextGateGain();
    CHECK (g.getGateGain() < 0.01f);
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
