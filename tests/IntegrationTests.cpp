#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestHelpers.h"
#include <thread>
#include "PresetManager.h"
#include "FactoryScenes.h"
#include "ui/SceneBrowser.h"
#include "dsp/CurveModulator.h"
#include "dsp/ModulationEngine.h"

using namespace stutter;
using namespace stutter::test;
using Catch::Matchers::WithinAbs;

// ---------------------------------------------------------------------------------------
// Whole-processor behaviour.
// ---------------------------------------------------------------------------------------

TEST_CASE ("A fresh instance passes the dry signal through untouched", "[processor]")
{
    // Curve modulators that should be neutral out of the box would otherwise colour the
    // sound before the user has done anything.
    StutterAudioProcessor proc;
    proc.setPlayConfigDetails (2, 2, sampleRate, blockSize);
    proc.prepareToPlay (sampleRate, blockSize);

    const int total = (int) (sampleRate * 1.0);
    juce::AudioBuffer<float> source (2, total);
    fillTestSignal (source);

    juce::AudioBuffer<float> out (2, total);
    out.clear();
    juce::MidiBuffer midi;

    for (int pos = 0; pos < total; pos += blockSize)
    {
        const int n = juce::jmin (blockSize, total - pos);
        juce::AudioBuffer<float> blk (2, n);
        for (int c = 0; c < 2; ++c) blk.copyFrom (c, 0, source, c, pos, n);
        midi.clear();
        proc.processBlock (blk, midi);
        for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, blk, c, 0, n);
    }

    double maxDiff = 0.0;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < total; ++i)
            maxDiff = juce::jmax (maxDiff, std::abs ((double) out.getReadPointer (c)[i]
                                                     - (double) source.getReadPointer (c)[i]));

    INFO ("dry RMS " << rmsOf (source) << ", wet RMS " << rmsOf (out));
    CHECK (maxDiff < 0.01);
}

TEST_CASE ("Loading Init resets every curve to neutral", "[processor][presets]")
{
    // A preset that sets a non-neutral curve must leave no residue behind when Init is
    // loaded after it.
    StutterAudioProcessor proc;
    proc.setPlayConfigDetails (2, 2, sampleRate, blockSize);
    proc.prepareToPlay (sampleRate, blockSize);

    auto& pm = proc.getPresetManager();
    const auto& presets = pm.getPresets();

    int tranceIdx = -1, initIdx = -1;
    for (int i = 0; i < (int) presets.size(); ++i)
    {
        if (presets[(size_t) i].name.contains ("Trance Gate")) tranceIdx = i;
        if (presets[(size_t) i].name == "Init")               initIdx = i;
    }
    REQUIRE (tranceIdx >= 0);
    REQUIRE (initIdx >= 0);

    pm.loadPreset (tranceIdx);
    pm.loadPreset (initIdx);

    auto curveIsNeutral = [&proc] (ModTarget target, float expected)
    {
        const auto& c = proc.getCurve (target);
        for (int i = 0; i <= 16; ++i)
            if (std::abs (c.getValueAtPhase ((float) i / 16.0f) - expected) > 1.0e-4f)
                return false;
        return true;
    };

    CHECK (curveIsNeutral (ModTarget::Volume, ID::neutralValueForCurve (ID::curveNameVolume)));
    CHECK (curveIsNeutral (ModTarget::Filter, ID::neutralValueForCurve (ID::curveNameFilter)));
    CHECK (curveIsNeutral (ModTarget::Pan,    ID::neutralValueForCurve (ID::curveNamePan)));
}

TEST_CASE ("Malformed curve state falls back to neutral", "[processor][state]")
{
    // Each fixture is a way a curve node can be broken. The point is that a truncated or
    // corrupt tree yields a usable neutral curve rather than stale or garbage state.
    struct Fixture { const char* name; std::function<juce::ValueTree()> build; };

    auto makeCurvesNode = [] (std::function<void (juce::ValueTree&)> mutate)
    {
        juce::ValueTree curves (ID::curvesNode);
        juce::ValueTree curve (ID::curveNode);
        curve.setProperty (ID::propName, ID::curveNameVolume, nullptr);
        curve.setProperty (ID::propEnabled, true, nullptr);
        mutate (curve);
        curves.appendChild (curve, nullptr);
        return curves;
    };

    const std::vector<Fixture> fixtures {
        { "no curve node at all", [] { return juce::ValueTree (ID::curvesNode); } },
        { "a single point",       [&] { return makeCurvesNode ([] (juce::ValueTree& c) {
                                            juce::ValueTree p (ID::pointNode);
                                            p.setProperty (ID::propPosition, 0.0f, nullptr);
                                            p.setProperty (ID::propValue, 0.9f, nullptr);
                                            c.appendChild (p, nullptr); }); } },
        { "no points",            [&] { return makeCurvesNode ([] (juce::ValueTree&) {}); } },
        { "a point missing its value", [&] { return makeCurvesNode ([] (juce::ValueTree& c) {
                                            juce::ValueTree p (ID::pointNode);
                                            p.setProperty (ID::propPosition, 0.0f, nullptr);
                                            c.appendChild (p, nullptr);
                                            juce::ValueTree q (ID::pointNode);
                                            q.setProperty (ID::propPosition, 1.0f, nullptr);
                                            c.appendChild (q, nullptr); }); } },
    };

    for (const auto& f : fixtures)
    {
        INFO (f.name);
        StutterAudioProcessor proc;
        proc.setPlayConfigDetails (2, 2, sampleRate, blockSize);
        proc.prepareToPlay (sampleRate, blockSize);

        proc.getCurve (ModTarget::Volume).fromValueTree (f.build().getChild (0));

        const float neutral = ID::neutralValueForCurve (ID::curveNameVolume);
        const auto& c = proc.getCurve (ModTarget::Volume);
        for (int i = 0; i <= 8; ++i)
            CHECK_THAT (c.getValueAtPhase ((float) i / 8.0f), WithinAbs (neutral, 1.0e-3f));
    }
}

TEST_CASE ("sequencerOn=false bypasses the lanes but leaves curves working", "[processor]")
{
    StutterAudioProcessor proc;
    proc.setPlayConfigDetails (2, 2, sampleRate, blockSize);
    proc.prepareToPlay (sampleRate, blockSize);

    auto& apvts = proc.getAPVTS();
    apvts.getParameter (ID::hostSync)->setValueNotifyingHost (0.0f);
    setInternalBpm (proc, bpm);
    apvts.getParameter (ID::sequencerOn)->setValueNotifyingHost (0.0f);

    const int total = (int) (sampleRate * 2.0);
    juce::AudioBuffer<float> source (2, total);
    fillTestSignal (source);

    auto render = [&] (bool withDuckCurve)
    {
        if (withDuckCurve)
        {
            proc.getCurve (ModTarget::Volume).applyPreset ("SidechainDuck");
            proc.getCurve (ModTarget::Volume).setSyncDivision (2);
        }

        juce::AudioBuffer<float> out (2, total);
        out.clear();
        juce::MidiBuffer midi;
        for (int pos = 0; pos < total; pos += blockSize)
        {
            const int n = juce::jmin (blockSize, total - pos);
            juce::AudioBuffer<float> blk (2, n);
            for (int c = 0; c < 2; ++c) blk.copyFrom (c, 0, source, c, pos, n);
            midi.clear();
            proc.processBlock (blk, midi);
            for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, blk, c, 0, n);
        }
        return out;
    };

    SECTION ("a fully active lane is silenced by the bypass")
    {
        auto& doc = proc.getSceneDocument();
        const int sceneIdx = proc.getSceneSelector().getActiveScene();
        for (int d = 0; d < numSteps; ++d)
            doc.addBlock (sceneIdx, StutterAudioProcessor::laneStutter, d, 1);
        doc.publish();

        const auto out = render (false);
        double maxDiff = 0.0;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < total; ++i)
                maxDiff = juce::jmax (maxDiff, std::abs ((double) out.getReadPointer (c)[i]
                                                         - (double) source.getReadPointer (c)[i]));
        CHECK (maxDiff < 1.0e-5);
    }

    SECTION ("a curve still modulates while the sequencer is bypassed")
    {
        const auto out = render (true);
        double maxDiff = 0.0;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < total; ++i)
                maxDiff = juce::jmax (maxDiff, std::abs ((double) out.getReadPointer (c)[i]
                                                         - (double) source.getReadPointer (c)[i]));
        CHECK (maxDiff > 0.05);
    }
}

TEST_CASE ("internalBpm changes the free-running playhead speed", "[processor][transport]")
{
    auto countAdvances = [] (double tempo)
    {
        StutterAudioProcessor proc;
        proc.setPlayConfigDetails (2, 2, sampleRate, blockSize);
        proc.prepareToPlay (sampleRate, blockSize);
        proc.getAPVTS().getParameter (ID::hostSync)->setValueNotifyingHost (0.0f);
        setInternalBpm (proc, tempo);

        const int total = (int) (sampleRate * 2.0);
        juce::AudioBuffer<float> blk (2, blockSize);
        juce::MidiBuffer midi;

        int lastDiv = -1, advances = 0;
        for (int pos = 0; pos < total; pos += blockSize)
        {
            const int n = juce::jmin (blockSize, total - pos);
            blk.setSize (2, n, false, false, true);
            blk.clear();
            midi.clear();
            proc.processBlock (blk, midi);

            const int d = proc.getBlockPlayheadDivision();
            if (lastDiv >= 0 && d != lastDiv) ++advances;
            lastDiv = d;
        }
        return advances;
    };

    const int atBase = countAdvances (bpm);
    const int atDouble = countAdvances (bpm * 2.0);
    INFO ("advances at " << bpm << " = " << atBase << ", at " << bpm * 2.0 << " = " << atDouble);
    CHECK (atBase > 0);
    CHECK (atDouble > atBase * 3 / 2);
}

// ---------------------------------------------------------------------------------------
// Automation reaching the processor, and the APVTS mirror.
// ---------------------------------------------------------------------------------------

TEST_CASE ("The plugin declares and handles no MIDI", "[processor]")
{
    // This is an audio effect that is called from the automation lane, not played from a
    // keyboard. Declaring MIDI input made hosts offer a routing that did nothing useful and
    // made auval warn about an aufx exporting MusicDeviceMIDIEvent.
    StutterAudioProcessor proc;
    proc.setPlayConfigDetails (2, 2, sampleRate, blockSize);
    proc.prepareToPlay (sampleRate, blockSize);

    CHECK_FALSE (proc.acceptsMidi());
    CHECK_FALSE (proc.producesMidi());
    CHECK_FALSE (proc.isMidiEffect());

    // A host that ignores the declaration and sends notes anyway must not get them back, and
    // must not have them change anything.
    const int sceneBefore = proc.getSceneSelector().getActiveScene();

    juce::AudioBuffer<float> block (2, blockSize);
    for (int c = 0; c < 2; ++c)
    {
        auto* d = block.getWritePointer (c);
        for (int i = 0; i < blockSize; ++i) d[i] = 0.5f;
    }

    juce::MidiBuffer notes;
    notes.addEvent (juce::MidiMessage::noteOn (1, 64, 1.0f), 0);
    proc.processBlock (block, notes);

    CHECK (notes.isEmpty());
    CHECK (proc.getSceneSelector().getActiveScene() == sceneBefore);
    CHECK (rmsOf (block) > 0.1);
}

TEST_CASE ("Automation selects the scene and gates the wet path", "[processor][automation]")
{
    // The whole automation-facing interface is these two parameters: sceneSelect says which
    // built scene plays, active says where it is heard. Both are polled on the audio thread
    // in processChunk, so this drives real blocks rather than inspecting the engine directly.
    StutterAudioProcessor proc;
    proc.setPlayConfigDetails (2, 2, sampleRate, blockSize);
    proc.prepareToPlay (sampleRate, blockSize);

    auto& apvts = proc.getAPVTS();
    auto setScene = [&] (int index)
    {
        auto* p = apvts.getParameter (ID::sceneSelect);
        REQUIRE (p != nullptr);
        p->setValueNotifyingHost (p->convertTo0to1 ((float) index));
    };
    auto setActive = [&] (bool on)
    {
        auto* p = apvts.getParameter (ID::active);
        REQUIRE (p != nullptr);
        p->setValueNotifyingHost (on ? 1.0f : 0.0f);
    };

    juce::AudioBuffer<float> block (2, blockSize);
    auto renderDc = [&] ()
    {
        for (int c = 0; c < 2; ++c)
        {
            auto* d = block.getWritePointer (c);
            for (int i = 0; i < blockSize; ++i) d[i] = 0.5f;
        }
        juce::MidiBuffer midi;
        proc.processBlock (block, midi);
        return rmsOf (block);
    };

    SECTION ("sceneSelect moves the active scene")
    {
        // Polled in processChunk, so it takes a block to land -- and it must land on the
        // block it belongs to, not one later.
        setScene (11);
        renderDc();
        CHECK (proc.getSceneSelector().getActiveScene() == 11);

        setScene (7);
        renderDc();
        CHECK (proc.getSceneSelector().getActiveScene() == 7);
    }

    SECTION ("the number typed into automation is the number shown in the browser")
    {
        // The whole point of numbering from 1: a user reads a slot number off the browser and
        // types that number into an automation lane. If the two ever disagree -- by an offset,
        // or by the parameter storing something the UI translates -- the plugin becomes
        // guesswork to automate.
        for (const int wanted : { firstSceneIndex, 2, 24, lastSceneIndex })
        {
            setScene (wanted);
            renderDc();

            INFO ("typed " << wanted);
            CHECK (proc.getSceneSelector().getActiveScene() == wanted);

            // And reading the parameter back gives the same number, not a normalised value
            // the user would have to decode.
            auto* p = apvts.getParameter (ID::sceneSelect);
            CHECK ((int) p->convertFrom0to1 (p->getValue()) == wanted);
        }
    }

    SECTION ("scene 0 leaves the selection alone")
    {
        // An automation lane the user never wrote reads as 0. That must not drag the scene
        // away from whatever they picked in the editor.
        setScene (9);
        renderDc();
        REQUIRE (proc.getSceneSelector().getActiveScene() == 9);

        setScene (noSceneIndex);
        renderDc();
        renderDc();
        CHECK (proc.getSceneSelector().getActiveScene() == 9);
    }

    SECTION ("active gates the wet path without silencing the dry")
    {
        // Closing the gate collapses wet toward dry, never toward silence: an inactive bar
        // must pass the source through, not punch a hole in the track.
        setActive (false);
        for (int i = 0; i < 8; ++i)
            renderDc();
        CHECK (renderDc() > 0.1);

        setActive (true);
        for (int i = 0; i < 8; ++i)
            renderDc();
        CHECK (renderDc() > 0.1);
    }

    SECTION ("toggling active does not click")
    {
        // The gate ramps over ~5ms rather than stepping. A bool parameter switches at a block
        // boundary, so without the ramp every automation edge would be an audible tick.
        auto& doc = proc.getSceneDocument();
        const int scene = proc.getSceneSelector().getActiveScene();
        for (int d = 0; d < 16; d += 2)
            doc.addBlock (scene, StutterAudioProcessor::laneGate, d, 1);
        doc.publish();

        const int total = (int) (sampleRate * 1.0);
        juce::AudioBuffer<float> buf (2, total);
        fillTestSignal (buf);

        bool on = true;
        for (int off = 0; off + blockSize <= total; off += blockSize)
        {
            if ((off / blockSize) % 4 == 0)
            {
                on = ! on;
                setActive (on);
            }
            juce::AudioBuffer<float> chunk (buf.getArrayOfWritePointers(), 2, off, blockSize);
            juce::MidiBuffer midi;
            proc.processBlock (chunk, midi);
        }

        CHECK (analyze (buf).severeClickCount == 0);
    }
}

TEST_CASE ("APVTS mirrors the active scene", "[processor][mirror]")
{
    // Without this a scene change alters what is heard but leaves every knob showing the
    // previous scene, so the plugin sounds one way and looks another.
    StutterAudioProcessor proc;
    proc.setPlayConfigDetails (2, 2, sampleRate, blockSize);
    proc.prepareToPlay (sampleRate, blockSize);

    auto& doc = proc.getSceneDocument();
    const juce::String cutoffId = ID::lanePrefix (StutterAudioProcessor::laneFilter) + "cutoff";

    auto setSceneCutoff = [&doc] (int sceneIdx, float hz)
    {
        auto scene = doc.ensureScene (sceneIdx);
        auto lp = scene.getOrCreateChildWithName (SceneIDs::laneParams, nullptr);
        juce::ValueTree laneNode (SceneIDs::lane);
        laneNode.setProperty (SceneIDs::index, StutterAudioProcessor::laneFilter, nullptr);
        juce::ValueTree pt (SceneIDs::param);
        pt.setProperty (SceneIDs::paramIndexProp, 1, nullptr);   // descriptor slot 1 = cutoff
        pt.setProperty (SceneIDs::value, hz, nullptr);
        laneNode.appendChild (pt, nullptr);
        lp.appendChild (laneNode, nullptr);
    };

    setSceneCutoff (10, 400.0f);
    setSceneCutoff (11, 8000.0f);
    doc.publish();

    auto triggerScene = [&proc] (int sceneIdx)
    {
        // Drive it the way a host does: write the parameter, then render. Calling
        // setActiveScene directly would bypass the polling in processChunk and prove less.
        auto* p = proc.getAPVTS().getParameter (ID::sceneSelect);
        REQUIRE (p != nullptr);
        p->setValueNotifyingHost (p->convertTo0to1 ((float) sceneIdx));

        juce::AudioBuffer<float> b (2, blockSize);
        b.clear();
        juce::MidiBuffer m;
        proc.processBlock (b, m);
        proc.pumpSceneMirror();
    };

    auto cutoff = [&proc, &cutoffId] { return proc.getAPVTS().getRawParameterValue (cutoffId)->load(); };

    triggerScene (10);
    CHECK_THAT (cutoff(), WithinAbs (400.0f, 5.0f));

    triggerScene (11);
    CHECK_THAT (cutoff(), WithinAbs (8000.0f, 50.0f));

    // No feedback: mirroring scene 11 must not have rewritten scene 10.
    triggerScene (10);
    CHECK_THAT (cutoff(), WithinAbs (400.0f, 5.0f));
}

// ---------------------------------------------------------------------------------------
// The v2 effects, with determinism as the headline property.
// ---------------------------------------------------------------------------------------

namespace
{
juce::AudioBuffer<float> renderLane (int lane, int chunkSize, juce::uint32 seed)
{
    CaptureBuffer cap;
    cap.prepare (sampleRate, 2, 2.5);

    BlockSequencer seq;
    installAllLaneEffects (seq);
    seq.prepare (sampleRate, 2);
    seq.setEnabled (true);

    auto scene = makeFullLaneScene (seq, lane);
    scene.seed = seed;
    auto& laneSnap = scene.lanes[(size_t) lane];
    laneSnap.blocks[0].startDiv = 0;
    laneSnap.blocks[0].lengthDiv = 16;
    laneSnap.numBlocks = 1;
    seq.updateChainOrder (scene);

    const int preRoll = (int) sampleRate;
    const int total = preRoll + (int) (sampleRate * 1.0);
    juce::AudioBuffer<float> source (2, total);
    fillTestSignal (source);

    juce::AudioBuffer<float> out (2, total);
    out.clear();

    const double pps = (bpm / 60.0) / sampleRate;
    double clock = 0.0;
    for (int pos = 0; pos < total; pos += chunkSize)
    {
        const int n = juce::jmin (chunkSize, total - pos);
        juce::AudioBuffer<float> blk (2, n);
        for (int c = 0; c < 2; ++c) blk.copyFrom (c, 0, source, c, pos, n);
        cap.write (blk);
        // The scene is active throughout: switching it mid-render would engage on whichever
        // block boundary each chunk size happens to provide, which is a property of the
        // harness and would mask the block-size dependence this exists to detect.
        seq.processBlock (blk, cap, scene, clock, pps);
        clock += pps * (double) n;
        for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, blk, c, 0, n);
    }
    return out;
}

/** True when two renders are bit-identical. Deliberately an exact comparison: these
    effects are seeded, so "close enough" would let a real determinism regression through. */
bool bitIdentical (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b);

double maxDiff (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    double m = 0.0;
    const int n = juce::jmin (a.getNumSamples(), b.getNumSamples());
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i)
            m = juce::jmax (m, std::abs ((double) a.getReadPointer (c)[i]
                                         - (double) b.getReadPointer (c)[i]));
    return m;
}

bool bitIdentical (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    return ! (maxDiff (a, b) > 0.0);
}
} // namespace

TEST_CASE ("The v2 effects are audible, clean, and deterministic", "[effects]")
{
    struct Case { int lane; const char* name; };
    const Case cases[] = {
        { lanes::stretcher, "Stretcher" },
        { lanes::shuffler,  "Shuffler"  },
        { lanes::delay,     "Delay"     },
        { lanes::distort,   "Distort"   },
    };

    for (const auto& lc : cases)
    {
        INFO (lc.name);

        const auto a = renderLane (lc.lane, 512, 1234u);
        const auto metrics = analyze (a);
        CHECK (metrics.rms > 0.001);            // produces audio at all
        CHECK (metrics.severeClickCount == 0);  // and does not click

        // Same seed, same chunking.
        CHECK (bitIdentical (a, renderLane (lc.lane, 512, 1234u)));

        // Same seed, DIFFERENT chunking. This is the check a global RNG fails: it would make
        // output depend on how the host happened to split the buffer, so an offline bounce
        // would stop matching what was heard.
        CHECK (bitIdentical (a, renderLane (lc.lane, 128, 1234u)));
    }
}

TEST_CASE ("A different seed changes the seeded effects", "[effects]")
{
    // Otherwise the seed is not actually wired up and the determinism above is vacuous.
    CHECK (maxDiff (renderLane (lanes::shuffler, 512, 1u),
                    renderLane (lanes::shuffler, 512, 999u)) > 0.001);
}

TEST_CASE ("A routed curve audibly changes the rendered output", "[effects][modulation]")
{
    // Everything else tests the modulation engine in isolation. This requires the output to
    // differ with the engine attached -- an engine that is correct but unplugged passes
    // every unit test of itself.
    auto render = [] (bool withModulation)
    {
        CaptureBuffer cap;
        cap.prepare (sampleRate, 2, 2.5);

        BlockSequencer seq;
        installAllLaneEffects (seq);
        seq.prepare (sampleRate, 2);
        seq.setEnabled (true);

        auto scene = makeFullLaneScene (seq, lanes::filter);
        auto& lane = scene.lanes[(size_t) lanes::filter];
        lane.blocks[0].startDiv = 0;
        lane.blocks[0].lengthDiv = 16;
        lane.numBlocks = 1;

        const std::vector<CurvePointV2> sweep {
            { 0.0f, 0.05f, 0.0f, PointWeight::Hard },
            { 1.0f, 1.0f,  0.0f, PointWeight::Hard }
        };
        SceneSchema::bakeCurveTable (sweep, scene.curves[0].table.data(), CurveSnapshot::tableSize);
        scene.curves[0].targetParam = (juce::int16) paramIndex (lanes::filter, 1);
        scene.curves[0].depth = 1.0f;
        scene.curves[0].speedMultiplier = 1.0f;
        scene.curves[0].enabled = true;
        scene.activeCurves[0] = 0;
        scene.numActiveCurves = 1;
        seq.updateChainOrder (scene);

        ModulationEngine mod;
        mod.prepare (sampleRate);

        const int total = (int) (sampleRate * 1.0);
        juce::AudioBuffer<float> source (2, total);
        fillTestSignal (source);

        juce::AudioBuffer<float> out (2, total);
        out.clear();
        const double pps = (bpm / 60.0) / sampleRate;
        double clock = 0.0;
        for (int pos = 0; pos < total; pos += blockSize)
        {
            const int n = juce::jmin (blockSize, total - pos);
            juce::AudioBuffer<float> blk (2, n);
            for (int c = 0; c < 2; ++c) blk.copyFrom (c, 0, source, c, pos, n);
            cap.write (blk);
            seq.processBlock (blk, cap, scene, clock, pps, withModulation ? &mod : nullptr);
            clock += pps * (double) n;
            for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, blk, c, 0, n);
        }
        return out;
    };

    const auto plain = render (false);
    const auto swept = render (true);

    INFO ("modulated vs unmodulated: maxDiff " << maxDiff (plain, swept));
    CHECK (maxDiff (plain, swept) > 0.01);
    CHECK (analyze (swept).severeClickCount == 0);
}

// ---------------------------------------------------------------------------------------
// Factory content has to be audible. Every preset once loaded as silence -- the version
// guard rejected them all and substituted Init -- and nothing failed, because no test
// asked whether choosing a preset changed the sound.
// ---------------------------------------------------------------------------------------

namespace
{
/** Mean absolute difference between the processed signal and the dry input.

    The probe signal is deliberately not the shared test sine. Buffer-category lanes work by
    replaying a slice of captured audio, and replaying a slice of a *constant* tone reproduces
    that tone sample for sample -- a working Stutter measures as zero difference. Adding
    per-beat amplitude steps and a sparse impulse train gives the replay something to be
    misaligned against, so the metric reflects whether the effect ran rather than whether the
    source happened to be self-similar. */
double processedDelta (StutterAudioProcessor& proc)
{
    // Two bars at 120 BPM. One second only reaches division 7 of a 16-division pattern, so a
    // preset whose blocks sit in the second half of the bar -- several do -- would be scored
    // on a stretch of time it never plays in.
    constexpr int numSamples = 48000 * 4;
    juce::AudioBuffer<float> buf (2, numSamples);
    stutter::test::fillTestSignal (buf);

    for (int c = 0; c < buf.getNumChannels(); ++c)
        for (int i = 0; i < numSamples; i += 1000)
            buf.setSample (c, i, buf.getSample (c, i) + 0.6f);

    juce::AudioBuffer<float> dry (2, numSamples);
    dry.makeCopyOf (buf);

    juce::MidiBuffer midi;
    for (int off = 0; off < numSamples; off += 512)
    {
        const int len = juce::jmin (512, numSamples - off);
        juce::AudioBuffer<float> chunk (buf.getArrayOfWritePointers(), 2, off, len);
        proc.processBlock (chunk, midi);
    }

    double diff = 0.0;
    for (int c = 0; c < 2; ++c)
        for (int s = 0; s < numSamples; ++s)
            diff += std::abs (buf.getSample (c, s) - dry.getSample (c, s));
    return diff / (double) (numSamples * 2);
}
}

TEST_CASE ("Factory scene banks are not silently substituted by Init", "[presets][audio]")
{
    // Each bank gets a fresh processor. Loading them in sequence on one instance lets the
    // previous bank's scenes stay live when the next load leaves them untouched, so an inert
    // preset reads as audible -- the measurement would confirm itself.
    StutterAudioProcessor probe;
    probe.prepareToPlay (stutter::test::sampleRate, stutter::test::blockSize);
    stutter::PresetManager probeList (probe);

    std::vector<juce::String> bankNames;
    for (const auto& e : probeList.getPresets())
        if (e.sceneBankIndex >= 0)
            bankNames.push_back (e.name);

    REQUIRE (bankNames.size() == (size_t) FactoryScenes::getNumBanks());

    for (const auto& wanted : bankNames)
    {
        StutterAudioProcessor proc;
        proc.prepareToPlay (stutter::test::sampleRate, stutter::test::blockSize);
        stutter::PresetManager pm (proc);

        int idx = -1;
        for (int i = 0; i < (int) pm.getPresets().size(); ++i)
            if (pm.getPresets()[(size_t) i].name == wanted)
            {
                idx = i;
                break;
            }
        REQUIRE (idx >= 0);

        pm.loadPreset (idx);

        INFO ("bank: " << wanted);
        CHECK (processedDelta (proc) > 1.0e-6);
    }
}

TEST_CASE ("Every factory scene bank reaches the audio path", "[presets][scenes]")
{
    StutterAudioProcessor proc;
    proc.prepareToPlay (stutter::test::sampleRate, stutter::test::blockSize);
    stutter::PresetManager pm (proc);

    int banksSeen = 0;
    for (const auto& e : pm.getPresets())
    {
        if (e.sceneBankIndex < 0)
            continue;
        ++banksSeen;

        int idx = -1;
        for (int i = 0; i < (int) pm.getPresets().size(); ++i)
            if (pm.getPresets()[(size_t) i].name == e.name)
            {
                idx = i;
                break;
            }
        REQUIRE (idx >= 0);
        pm.loadPreset (idx);

        int populated = 0, blocks = 0;
        for (int s = 0; s < stutter::maxScenes; ++s)
            if (const auto* sn = proc.getSceneStore().get (s))
                if (sn->populated)
                {
                    ++populated;
                    for (int l = 0; l < stutter::maxLanes; ++l)
                        blocks += sn->lanes[(size_t) l].numBlocks;
                }

        INFO ("bank: " << e.name);
        CHECK (populated > 0);
        CHECK (blocks > 0);
    }

    CHECK (banksSeen == FactoryScenes::getNumBanks());
}

TEST_CASE ("A freshly loaded preset is not marked dirty", "[presets][ui]")
{
    StutterAudioProcessor proc;
    proc.prepareToPlay (stutter::test::sampleRate, stutter::test::blockSize);
    stutter::PresetManager pm (proc);

    int bankIdx = -1;
    for (int i = 0; i < (int) pm.getPresets().size(); ++i)
        if (pm.getPresets()[(size_t) i].sceneBankIndex >= 0)
        {
            bankIdx = i;
            break;
        }
    REQUIRE (bankIdx >= 0);

    pm.loadPreset (bankIdx);
    CHECK_FALSE (pm.isDirty());

    // The mirror runs on a timer, so it lands after loadPreset returns. It writes parameter
    // values, and those writes must not read back as user edits -- otherwise every preset
    // shows as modified the instant it is chosen.
    proc.pumpSceneMirror();
    CHECK_FALSE (pm.isDirty());
}

TEST_CASE ("Every rhythmic factory preset actually plays", "[presets][audio]")
{
    // Same isolation rule as the bank test: one fresh processor per preset, because a preset
    // that changes nothing would otherwise be scored on the previous preset's scenes.
    StutterAudioProcessor probe;
    probe.prepareToPlay (stutter::test::sampleRate, stutter::test::blockSize);
    stutter::PresetManager probeList (probe);

    std::vector<juce::String> names;
    for (const auto& e : probeList.getPresets())
        if (e.isFactory && e.sceneBankIndex < 0 && e.name != "Init")
            names.push_back (e.name);

    REQUIRE (names.size() > 20);

    std::vector<juce::String> inert;
    for (const auto& wanted : names)
    {
        StutterAudioProcessor proc;
        proc.prepareToPlay (stutter::test::sampleRate, stutter::test::blockSize);
        stutter::PresetManager pm (proc);

        int idx = -1;
        for (int i = 0; i < (int) pm.getPresets().size(); ++i)
            if (pm.getPresets()[(size_t) i].name == wanted)
            {
                idx = i;
                break;
            }
        REQUIRE (idx >= 0);
        pm.loadPreset (idx);

        if (processedDelta (proc) <= 1.0e-6)
            inert.push_back (wanted);
    }

    juce::String report;
    for (auto& n : inert) report << n << ", ";
    INFO ("presets that leave the signal untouched: " << (int) inert.size() << " -- " << report);
    CHECK (inert.empty());
}

TEST_CASE ("A converted preset's blocks land on the scene the editor shows", "[presets][ui]")
{
    // The audio path and the editor pick their scene independently: the gesture engine plays
    // whatever state load selected, while the grid opens on SceneBrowser's default. When those
    // disagree the preset sounds right and looks empty, which reads as a failed load.
    StutterAudioProcessor proc;
    proc.prepareToPlay (stutter::test::sampleRate, stutter::test::blockSize);
    stutter::PresetManager pm (proc);

    int idx = -1;
    for (int i = 0; i < (int) pm.getPresets().size(); ++i)
        if (pm.getPresets()[(size_t) i].name == "Classic Stutter Build")
            idx = i;
    REQUIRE (idx >= 0);
    pm.loadPreset (idx);

    const int shown = stutter::ui::SceneBrowser::defaultScene;
    const auto* scene = proc.getSceneStore().get (shown);
    REQUIRE (scene != nullptr);

    INFO ("scene the editor opens on: " << shown);
    CHECK (scene->populated);
    CHECK (scene->hasAnyBlocks());

    // And the engine must be playing that same scene, or it sounds like a different preset.
    CHECK (proc.getSceneSelector().getActiveScene() == shown);
}

TEST_CASE ("A converted preset keeps its parameter values after mirroring", "[presets][audio]")
{
    // The scene becomes the authority the moment it loads: the mirror pushes its lane values
    // into APVTS a beat later. A converted scene that carried blocks but no parameters would
    // therefore reset the preset's own settings to the descriptor defaults -- the pattern kept
    // playing, but with the wrong rate and no decay, which is a subtler wrong than silence.
    StutterAudioProcessor proc;
    proc.prepareToPlay (stutter::test::sampleRate, stutter::test::blockSize);
    stutter::PresetManager pm (proc);

    int idx = -1;
    for (int i = 0; i < (int) pm.getPresets().size(); ++i)
        if (pm.getPresets()[(size_t) i].name == "Classic Stutter Build")
            idx = i;
    REQUIRE (idx >= 0);
    pm.loadPreset (idx);

    const auto rateId  = ID::lanePrefix (0) + ID::stutterRate;
    const auto decayId = ID::lanePrefix (0) + ID::stutterDecay;

    const float rateAfterLoad  = proc.getAPVTS().getRawParameterValue (rateId)->load();
    const float decayAfterLoad = proc.getAPVTS().getRawParameterValue (decayId)->load();
    CHECK_THAT (decayAfterLoad, WithinAbs (0.7f, 1.0e-4f));

    proc.pumpSceneMirror();

    CHECK_THAT (proc.getAPVTS().getRawParameterValue (rateId)->load(),
                WithinAbs (rateAfterLoad, 1.0e-4f));
    CHECK_THAT (proc.getAPVTS().getRawParameterValue (decayId)->load(),
                WithinAbs (decayAfterLoad, 1.0e-4f));
}

TEST_CASE ("Automation selects a scene from a loaded bank", "[presets][automation]")
{
    // The headline promise of the manual: load a bank, automate Scene, hear that scene.
    StutterAudioProcessor proc;
    proc.prepareToPlay (stutter::test::sampleRate, stutter::test::blockSize);
    stutter::PresetManager pm (proc);

    int idx = -1;
    for (int i = 0; i < (int) pm.getPresets().size(); ++i)
        if (pm.getPresets()[(size_t) i].name == "Playable Set")
            idx = i;
    REQUIRE (idx >= 0);
    pm.loadPreset (idx);

    // Find a populated scene that is not the one we start on.
    const int start = proc.getSceneSelector().getActiveScene();
    int target = -1;
    for (int i = 0; i < stutter::maxScenes && target < 0; ++i)
        if (i != start)
            if (const auto* s = proc.getSceneStore().get (i))
                if (s->populated && s->hasAnyBlocks())
                    target = i;
    REQUIRE (target >= 0);

    auto* p = proc.getAPVTS().getParameter (ID::sceneSelect);
    REQUIRE (p != nullptr);
    p->setValueNotifyingHost (p->convertTo0to1 ((float) target));

    juce::AudioBuffer<float> buf (2, stutter::test::blockSize);
    stutter::test::fillTestSignal (buf);
    juce::MidiBuffer midi;
    proc.processBlock (buf, midi);

    INFO ("automating Scene to " << target << " should have selected it");
    CHECK (proc.getSceneSelector().getActiveScene() == target);
}

TEST_CASE ("Switching scenes mid-playback does not click", "[presets][click]")
{
    // Automating Scene across a track is the normal way to use this, so a switch has to be
    // silent with real factory content rather than only for the gate ramp in isolation.
    StutterAudioProcessor proc;
    proc.prepareToPlay (stutter::test::sampleRate, stutter::test::blockSize);
    stutter::PresetManager pm (proc);

    int idx = -1;
    for (int i = 0; i < (int) pm.getPresets().size(); ++i)
        if (pm.getPresets()[(size_t) i].name == "Playable Set")
            idx = i;
    REQUIRE (idx >= 0);
    pm.loadPreset (idx);

    std::vector<int> populated;
    for (int i = 0; i < stutter::maxScenes; ++i)
        if (const auto* s = proc.getSceneStore().get (i))
            if (s->populated && s->hasAnyBlocks())
                populated.push_back (i);
    REQUIRE (populated.size() >= 2);

    const int total = (int) (stutter::test::sampleRate * 4.0);
    juce::AudioBuffer<float> source (2, total);
    stutter::test::fillTestSignal (source);

    juce::AudioBuffer<float> out (2, total);
    out.clear();

    // Switch scene every half second, straight through the material.
    const int switchEvery = (int) (stutter::test::sampleRate * 0.5);
    size_t next = 0;

    for (int pos = 0; pos < total; pos += stutter::test::blockSize)
    {
        const int n = juce::jmin (stutter::test::blockSize, total - pos);
        juce::AudioBuffer<float> blk (2, n);
        for (int c = 0; c < 2; ++c) blk.copyFrom (c, 0, source, c, pos, n);

        if (pos / switchEvery != (pos - stutter::test::blockSize) / switchEvery)
        {
            auto* p = proc.getAPVTS().getParameter (ID::sceneSelect);
            p->setValueNotifyingHost (
                p->convertTo0to1 ((float) populated[next % populated.size()]));
            ++next;
        }

        juce::MidiBuffer midi;
        proc.processBlock (blk, midi);
        for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, blk, c, 0, n);
    }

    const auto m = stutter::test::analyze (out);
    INFO ("max adjacent delta " << m.maxAdjacentDelta
          << ", severe clicks " << m.severeClickCount);
    CHECK (m.severeClickCount == 0);
}

TEST_CASE ("Host automation does not rewrite the scene document", "[processor][mirror][automation]")
{
    // APVTS holds a mirror of the active scene's lane values, so anything that writes a lane
    // parameter looks identical to a knob edit. Host automation writes those parameters every
    // block -- without telling the two apart, replaying an automation lane would bake the
    // host's values into the scene and make them permanent on the next project save.
    //
    // The distinction is the gesture pair: JUCE's Slider/ComboBox attachments always wrap a
    // drag in begin/endChangeGesture, and automation playback never does.
    StutterAudioProcessor proc;
    proc.prepareToPlay (stutter::test::sampleRate, stutter::test::blockSize);

    auto& doc = proc.getSceneDocument();
    doc.ensureScene (60);
    doc.publish();

    proc.getSceneSelector().setActiveScene (60);
    proc.pumpSceneMirror();

    const auto id = ID::lanePrefix (0) + ID::stutterDecay;
    auto* param = proc.getAPVTS().getParameter (id);
    REQUIRE (param != nullptr);
    const auto& range = proc.getAPVTS().getParameterRange (id);

    const auto* before = proc.getSceneStore().get (60);
    REQUIRE (before != nullptr);
    const float original = before->lanes[0].params[1];

    SECTION ("automation (no gesture) leaves the scene alone")
    {
        param->setValueNotifyingHost (range.convertTo0to1 (0.85f));
        proc.pumpSceneMirror();

        const auto* stored = proc.getSceneStore().get (60);
        REQUIRE (stored != nullptr);
        CHECK_THAT (stored->lanes[0].params[1], WithinAbs (original, 1.0e-4f));
    }

    SECTION ("a UI edit (wrapped in a gesture) still reaches the scene")
    {
        // The other half of the contract: gating write-back must not break the edit path it
        // was built for.
        param->beginChangeGesture();
        param->setValueNotifyingHost (range.convertTo0to1 (0.85f));
        param->endChangeGesture();
        proc.pumpSceneMirror();

        const auto* stored = proc.getSceneStore().get (60);
        REQUIRE (stored != nullptr);
        CHECK_THAT (stored->lanes[0].params[1], WithinAbs (0.85f, 1.0e-4f));
    }
}

TEST_CASE ("A lane knob edit survives a scene round-trip", "[processor][mirror]")
{
    // APVTS only mirrors the active scene, so an edit that stops there is discarded the next
    // time a scene change refills the mirror. Turning a knob and then playing another note
    // used to silently lose the edit.
    StutterAudioProcessor proc;
    proc.prepareToPlay (stutter::test::sampleRate, stutter::test::blockSize);

    auto& doc = proc.getSceneDocument();
    doc.ensureScene (60);
    doc.ensureScene (61);
    doc.publish();

    proc.getSceneSelector().setActiveScene (60);
    proc.pumpSceneMirror();

    const auto id = ID::lanePrefix (0) + ID::stutterDecay;
    auto* param = proc.getAPVTS().getParameter (id);
    REQUIRE (param != nullptr);
    const auto& range = proc.getAPVTS().getParameterRange (id);

    // Wrapped in a gesture because that is what turning a knob does: the attachments JUCE
    // gives sliders and combo boxes always bracket a drag this way, and write-back only
    // treats gestured changes as edits so host automation cannot masquerade as one.
    param->beginChangeGesture();
    param->setValueNotifyingHost (range.convertTo0to1 (0.85f));
    param->endChangeGesture();

    // Writes are deferred to the timer: parameterChanged fires on whatever thread set the
    // parameter, so it only records the value. pumpSceneMirror drives the same flush the
    // timer does.
    proc.pumpSceneMirror();

    // The edit must reach the scene, not just the mirror.
    const auto* stored = proc.getSceneStore().get (60);
    REQUIRE (stored != nullptr);
    CHECK_THAT (stored->lanes[0].params[1], WithinAbs (0.85f, 1.0e-4f));

    // Switch away and back, as playing two notes would.
    proc.getSceneSelector().setActiveScene (61);
    proc.pumpSceneMirror();
    proc.getSceneSelector().setActiveScene (60);
    proc.pumpSceneMirror();

    CHECK_THAT (proc.getAPVTS().getRawParameterValue (id)->load(), WithinAbs (0.85f, 1.0e-4f));
}

TEST_CASE ("Editing one scene's knob does not leak into another", "[processor][mirror]")
{
    // The write-back has to target the scene the mirror was filled from. Taking the live
    // scene instead would copy the outgoing scene's values over the incoming one.
    StutterAudioProcessor proc;
    proc.prepareToPlay (stutter::test::sampleRate, stutter::test::blockSize);

    auto& doc = proc.getSceneDocument();
    doc.ensureScene (60);
    doc.ensureScene (61);
    doc.publish();

    const auto id = ID::lanePrefix (0) + ID::stutterDecay;
    const auto& range = proc.getAPVTS().getParameterRange (id);
    auto* param = proc.getAPVTS().getParameter (id);

    // Gestured, because these stand in for knob turns; write-back ignores ungestured changes
    // so that host automation cannot rewrite the scene.
    auto turnKnob = [param, &range] (float value)
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (range.convertTo0to1 (value));
        param->endChangeGesture();
    };

    proc.getSceneSelector().setActiveScene (60);
    proc.pumpSceneMirror();
    turnKnob (0.85f);
    proc.pumpSceneMirror();   // flush into scene 60 before the scene changes

    proc.getSceneSelector().setActiveScene (61);
    proc.pumpSceneMirror();
    turnKnob (0.20f);
    proc.pumpSceneMirror();

    const auto* a = proc.getSceneStore().get (60);
    const auto* b = proc.getSceneStore().get (61);
    REQUIRE (a != nullptr);
    REQUIRE (b != nullptr);
    CHECK_THAT (a->lanes[0].params[1], WithinAbs (0.85f, 1.0e-4f));
    CHECK_THAT (b->lanes[0].params[1], WithinAbs (0.20f, 1.0e-4f));
}

TEST_CASE ("Lane mute and solo reach the audio path", "[processor][mix]")
{
    // BlockSequencer honoured mute/solo from the start, but nothing could set them, so this
    // covers the whole path the grid's lane dot now drives.
    auto render = [] (StutterAudioProcessor& proc)
    {
        const int total = (int) (stutter::test::sampleRate * 2.0);
        juce::AudioBuffer<float> buf (2, total);
        stutter::test::fillTestSignal (buf);
        juce::AudioBuffer<float> dry (2, total);
        dry.makeCopyOf (buf);

        juce::MidiBuffer midi;
        for (int off = 0; off < total; off += stutter::test::blockSize)
        {
            const int n = juce::jmin (stutter::test::blockSize, total - off);
            juce::AudioBuffer<float> chunk (buf.getArrayOfWritePointers(), 2, off, n);
            proc.processBlock (chunk, midi);
        }

        double diff = 0.0;
        for (int c = 0; c < 2; ++c)
            for (int s = 0; s < total; ++s)
                diff += std::abs (buf.getSample (c, s) - dry.getSample (c, s));
        return diff / (double) (total * 2);
    };

    auto build = [] (StutterAudioProcessor& proc)
    {
        proc.prepareToPlay (stutter::test::sampleRate, stutter::test::blockSize);
        auto& doc = proc.getSceneDocument();
        const int scene = proc.getSceneSelector().getActiveScene();
        // Two lanes that each change the sound unmistakably on their own. Crush at its
        // default bit depth is nearly transparent, so using it here would make "the other
        // lane stopped" indistinguishable from "nothing happened".
        for (int d = 0; d < 16; d += 2)
        {
            doc.addBlock (scene, StutterAudioProcessor::laneGate, d, 1);
            doc.addBlock (scene, StutterAudioProcessor::laneReverse, d + 1, 1);
        }
        doc.publish();
        return scene;
    };

    double baseline = 0.0;
    {
        StutterAudioProcessor proc;
        build (proc);
        baseline = render (proc);
    }
    REQUIRE (baseline > 1.0e-6);

    SECTION ("muting every active lane silences the effect")
    {
        StutterAudioProcessor proc;
        const int scene = build (proc);
        proc.getSceneDocument().toggleLaneMute (scene, StutterAudioProcessor::laneGate);
        proc.getSceneDocument().toggleLaneMute (scene, StutterAudioProcessor::laneReverse);
        proc.getSceneDocument().publish();

        CHECK (render (proc) < 1.0e-6);
    }

    SECTION ("soloing one lane silences the others")
    {
        StutterAudioProcessor proc;
        const int scene = build (proc);
        proc.getSceneDocument().toggleLaneSolo (scene, StutterAudioProcessor::laneGate);
        proc.getSceneDocument().publish();

        const double soloed = render (proc);
        INFO ("baseline " << baseline << ", soloed " << soloed);
        CHECK (soloed > 1.0e-6);                                 // the soloed lane still plays
        CHECK (std::abs (soloed - baseline) > baseline * 0.01);  // the other one stopped
    }
}

TEST_CASE ("Parameter changes from many threads stay safe", "[processor][mirror][threads]")
{
    // This is what pluginval's "Parameter thread safety" test does, and it deadlocked the
    // first version of the write-back: parameterChanged fires on whatever thread set the
    // parameter, and that handler mutated a ValueTree and rebuilt the whole scene bank.
    StutterAudioProcessor proc;
    proc.prepareToPlay (stutter::test::sampleRate, stutter::test::blockSize);
    proc.getSceneDocument().ensureScene (60);
    proc.getSceneDocument().publish();
    proc.getSceneSelector().setActiveScene (60);
    proc.pumpSceneMirror();

    std::vector<juce::RangedAudioParameter*> params;
    for (auto* p : proc.getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p))
            if (r->paramID.startsWith ("lane"))
                params.push_back (r);
    REQUIRE (params.size() > 8);

    std::atomic<bool> go { false };
    std::atomic<int> done { 0 };
    constexpr int numThreads = 4;
    constexpr int iterations = 400;

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; ++t)
        threads.emplace_back ([&, t]
        {
            while (! go.load (std::memory_order_acquire)) {}
            juce::Random rng (t + 1);
            for (int i = 0; i < iterations; ++i)
            {
                auto* p = params[(size_t) rng.nextInt ((int) params.size())];
                p->setValueNotifyingHost (rng.nextFloat());
            }
            done.fetch_add (1, std::memory_order_release);
        });

    go.store (true, std::memory_order_release);

    // Drive the flush concurrently, as the message thread would, while audio also runs.
    juce::AudioBuffer<float> buf (2, stutter::test::blockSize);
    juce::MidiBuffer midi;
    const auto started = juce::Time::getMillisecondCounter();

    while (done.load (std::memory_order_acquire) < numThreads)
    {
        stutter::test::fillTestSignal (buf);
        proc.processBlock (buf, midi);
        proc.pumpSceneMirror();

        if (juce::Time::getMillisecondCounter() - started > 20000)
            break;   // do not hang the suite; the elapsed check below reports it
    }

    for (auto& th : threads)
        th.join();

    // Bound the wall time rather than only checking it finished. Publishing per parameter
    // change -- the original defect -- rebuilds the whole bank on every one of these writes,
    // which is what made pluginval's equivalent test time out at 30s. 1600 writes should
    // take well under a second.
    const auto elapsed = juce::Time::getMillisecondCounter() - started;
    INFO ("elapsed " << elapsed << " ms for " << (numThreads * iterations) << " parameter writes");
    CHECK (elapsed < 5000);

    proc.pumpSceneMirror();

    // Whatever the threads raced to, the scene must hold values that came from a parameter
    // rather than torn or out-of-range data.
    const auto* s = proc.getSceneStore().get (60);
    REQUIRE (s != nullptr);
    for (int l = 0; l < stutter::maxLanes; ++l)
        for (int p = 0; p < stutter::maxParamsPerLane; ++p)
            CHECK (std::isfinite (s->lanes[(size_t) l].params[(size_t) p]));
}
