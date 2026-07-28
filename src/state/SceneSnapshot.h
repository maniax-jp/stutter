#pragma once
#include "../dsp/ParamIndex.h"
#include <juce_core/juce_core.h>
#include <array>
#include <cmath>
#include <cstdint>

namespace stutter
{

/**
    One sequencer block: a lane is active from startDiv for lengthDiv divisions.

    v1 stored a fixed 16-entry bool array per lane. Blocks replace that because effects with a
    directional envelope (TapeStop decelerating, a Retrigger rate ramp) need to know how long
    they are held; a grid of independent on/off cells can only say "on again", never "still
    going". Positions are integer divisions rather than PPQ so that changing beats/divisions
    re-times a pattern instead of corrupting it.
*/
struct Block
{
    int16_t startDiv = 0;
    int16_t lengthDiv = 1;

    /** Locked / Split / Custom -- how this block's parameters are modulated. See
        ParamDescriptor.h; the tier is resolved to a baked curve at snapshot time, so the
        audio thread never branches on it. */
    uint8_t tier = 0;

    /** bit0: reverse direction (the Split arrow pointing left). Remaining bits reserved. */
    uint8_t flags = 0;

    /** 0..1 chance this block fires on any given pass. Evaluated against the scene seed, so a
        looping pattern makes the same choices every time. 1.0 = always. */
    float probability = 1.0f;

    /** Mixed into the per-trigger seed so two blocks in the same scene randomise
        differently while both staying reproducible. */
    uint32_t seedOffset = 0;

    int16_t endDiv() const noexcept { return (int16_t) (startDiv + lengthDiv); }
    bool isReversed() const noexcept { return (flags & 1u) != 0u; }
};

/** One lane's parameters, blocks, and mixer settings. */
struct LaneSnapshot
{
    std::array<float, maxParamsPerLane> params {};
    std::array<Block, maxBlocksPerLane> blocks {};

    /** Number of valid entries in `blocks`. Entries at or beyond this index are ignored, not
        zeroed, so the message thread can rebuild without clearing the whole array. */
    int numBlocks = 0;

    float mix = 1.0f;
    float gain = 1.0f;
    float pan = 0.0f;

    /** Common Filter (Glitch 2's per-module insert filter). type 0 = off. */
    int filterType = 0;
    float filterCutoff = 1.0f;
    float filterResonance = 0.2f;

    /** Position in the Texture serial chain. Lanes are processed in ascending order of this
        value, which is what makes the chain drag-reorderable without touching the DSP. */
    int chainPosition = 0;

    bool mute = false;
    bool solo = false;
    bool enabled = true;
};

/**
    One modulation curve, already baked.

    The audio thread only ever reads `table`; the breakpoints that produced it live in the
    ValueTree and never reach here. That is deliberate -- baking on the message thread is what
    makes Locked, Split and Custom cost the same at run time.
*/
struct CurveSnapshot
{
    static constexpr int tableSize = 1024;

    std::array<float, tableSize> table {};

    /** Flat ParamIndex this curve drives, or -1 when unrouted. Validate with
        isValidParamIndex() when it comes from deserialized state. */
    int16_t targetParam = -1;

    /** Cycles per pattern: 0.25x to 4x, snapped to {0.25, 0.5, 1, 2, 4} to avoid phase drift
        over long patterns. Matches Stutter Edit 2's per-TVM Speed control. */
    float speedMultiplier = 1.0f;

    /** Scales the curve's effect on the base value. */
    float depth = 1.0f;

    uint8_t tier = 0;

    /** Bipolar curves offset around 0.5 (so 0.5 is "no change"); unipolar curves interpolate
        from the base value toward the curve value. */
    bool bipolar = false;

    bool enabled = false;

    /** Read the baked table at phase 0..1 with linear interpolation. Wraps. Identical to
        CurveModulator::getValueAtPhase (CurveModulator.h:98) minus the atomic load, which
        SceneStore handles one level up by publishing whole banks. */
    float valueAtPhase (float phase) const noexcept
    {
        phase = phase - std::floor (phase);
        const float posF = phase * (float) (tableSize - 1);
        const int i0 = (int) posF;
        const int i1 = i0 + 1 < tableSize ? i0 + 1 : tableSize - 1;
        const float frac = posF - (float) i0;
        return table[(size_t) i0] + frac * (table[(size_t) i1] - table[(size_t) i0]);
    }
};

/** Loop behaviour for a pattern, per scene. */
/**
    How a pattern repeats.

    NOT IMPLEMENTED beyond Forward. The value is parsed, stored and round-trips through state,
    but BlockSequencer derives its position from PPQ alone and always runs forward --
    SampleContext::reverseDirection is hardcoded false. Palindrome needs the block cursor,
    which is forward-only by design, to be rebuilt at each turnaround, and OneShot needs a
    stop condition the sequencer has no concept of.

    No UI sets this, so a user cannot reach the unimplemented paths; a hand-edited preset
    would silently play Forward instead. Left in place rather than removed because the
    schema round-trip is already covered by tests and dropping the field would be the harder
    change to reverse.
*/
enum class LoopPolicy : uint8_t
{
    Forward = 0,   ///< Restart from zero at the end. The only behaviour implemented.
    Palindrome,    ///< Reverse direction at each end (Stutter Edit 2's Palindrome). Not implemented.
    OneShot        ///< Play once, then stop. Not implemented.
};

/**
    Retired with the MIDI performance layer, which is what "release" referred to.

    Nothing reads it now -- the gate is the `active` parameter's, and automation says where a
    scene stops as directly as it says where it starts. The enum stays because scenes saved by
    earlier versions carry the property, and dropping the type would make those files fail to
    parse rather than simply ignoring a field that no longer means anything.
*/
enum class ReleaseMode : uint8_t
{
    OnGrid = 0,
    FullGesture,
    Latch,
    Instant,
    Stick
};

/**
    A complete scene: everything one automation value recalls.

    This is deliberately a POD of fixed extent -- no std::vector, no juce::String, no
    ValueTree. It is memcpy'd into a double-buffered bank that the audio thread reaches
    through an atomic pointer (see SceneStore), so any heap-owning member would put an
    allocation inside a structure read on the audio thread.

    Size: roughly 12 lanes * (48B params + 64 blocks * 12B) + 16 curves * 4KB
        ~= 10KB of lanes + 66KB of curve tables ~= 76KB per scene, ~9.7MB for 128.
    The curve tables dominate; halving tableSize halves the bank if that ever matters.
*/
struct SceneSnapshot
{
    std::array<LaneSnapshot, maxLanes> lanes {};
    std::array<CurveSnapshot, maxCurves> curves {};

    /** Indices into `curves` of the enabled, routed entries, so the modulation engine
        iterates only live routes instead of scanning all maxCurves every sample. */
    std::array<int16_t, maxCurves> activeCurves {};
    int numActiveCurves = 0;

    /** Pattern geometry. beats 1..8, divisions 2..8 -- odd division counts give triplet and
        polyrhythmic feels for free (Glitch 2's approach). */
    int beats = 4;
    int divisions = 4;

    /** -1..1. Shifts odd division boundaries; 0 = straight. */
    float swing = 0.0f;

    /** Re-seeds the RNGs on every (re)trigger so a looping pattern is identical each pass and
        an offline bounce matches what was heard. Glitch 2 makes this a first-class scene
        property for the same reason. */
    uint32_t seed = 0;

    LoopPolicy loopPolicy = LoopPolicy::Forward;
    ReleaseMode releaseMode = ReleaseMode::OnGrid;

    /** False for a scene index with no data, so the gesture layer can ignore notes that map
        to empty slots rather than triggering silence. */
    bool populated = false;

    int totalDivisions() const noexcept { return beats * divisions; }

    /** True when any lane has at least one block. `populated` only says the slot was defined;
        a scene can be defined and still be an empty grid, which sounds exactly like a missing
        scene and so is not somewhere to land the playhead by default. */
    bool hasAnyBlocks() const noexcept
    {
        for (const auto& lane : lanes)
            if (lane.numBlocks > 0)
                return true;
        return false;
    }
};

/** Bank array size. Slot 0 is deliberately unused -- see firstSceneIndex. */
inline constexpr int maxScenes = 128;

/**
    Scenes are numbered from 1, and index 0 means "no scene".

    The number is not an implementation detail the user is shielded from: they read it off the
    browser and type it into an automation lane by hand, so the two have to be the same number.
    Starting at 1 is what a person counting slots expects, and it leaves 0 free to mean
    "unspecified" -- which an automation lane that was never written reads as, so an untouched
    lane leaves the scene alone instead of yanking it to the first slot.

    The cost is one wasted array entry, which is the cheapest way to buy that.
*/
inline constexpr int firstSceneIndex = 1;
inline constexpr int lastSceneIndex  = maxScenes - 1;   // 127

/** Index meaning "no scene selected". Automation sends this when the lane is unwritten. */
inline constexpr int noSceneIndex = 0;

/** The scene a fresh instance opens and plays. Factory content targets this, the sceneSelect
    parameter defaults to it, and the browser scrolls to it -- they must agree, or the editor
    shows one scene while a different one is heard. */
inline constexpr int defaultSceneIndex = firstSceneIndex;

/**
    Derive a per-trigger seed deterministically.

    Effects that randomise must seed a local generator from this and must never touch a global
    RNG: a global makes output depend on how the host happened to chunk the buffer, which
    breaks both offline reproducibility and the DeterminismTests check that identical seeds
    survive a change of block size.

    Uses the murmur3 finalizer, which mixes well enough that adjacent block indices produce
    unrelated streams.
*/
inline uint32_t deriveSeed (uint32_t sceneSeed, uint32_t triggerCount, int blockIndex) noexcept
{
    uint32_t h = sceneSeed
               ^ (triggerCount * 2654435761u)
               ^ ((uint32_t) blockIndex * 40503u);
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

} // namespace stutter
