#pragma once
#include "ParamIndex.h"
#include <juce_core/juce_core.h>
#include <array>
#include <cmath>

namespace stutter
{

/**
    Describes one effect parameter completely enough that the APVTS layout, the UI control,
    and the effect's own read of the value can all be generated from this one declaration.

    v1 spread each parameter's definition across three places that had to be kept in sync by
    hand: its ID string in ParameterIDs.h, its range and default in ParameterLayout.cpp
    (addFloat/addRateChoice calls), and its knob construction in LaneParamPanel::rebuildForLane.
    Adding a parameter meant editing all three, and a mismatch between them was silent -- a
    range typo in the layout would just quietly clamp the effect's input. Collapsing them here
    means the effect that *uses* a parameter is the thing that declares it.

    Instances are static constexpr arrays owned by each effect (see getParamDescriptors()),
    so there is no allocation and no lifetime question: the pointers outlive any caller.
*/
struct ParamDescriptor
{
    /** Suffix of the APVTS parameter ID. The full ID is built as "lane{N}_{id}", preserving
        v1's naming so existing automation lanes in a DAW session keep pointing at the same
        parameter where the parameter itself is unchanged. */
    const char* id = "";

    /** Human-readable name for the APVTS parameter and the UI label. */
    const char* label = "";

    /** Inclusive value range in natural units (Hz, semitones, 0..1, ...). For choice
        parameters this is [0, numChoices-1] and the value is an index. */
    float minValue = 0.0f;
    float maxValue = 1.0f;
    float defaultValue = 0.0f;

    /** Step size in natural units; 0 means continuous. Used for integer-valued parameters
        such as Crush's bit depth (1..16 step 1). */
    float stepSize = 0.0f;

    /** Skew factor for the APVTS NormalisableRange; 1.0 is linear. Filter cutoff uses 0.3 so
        the low end of 20Hz..20kHz gets usable knob travel. */
    float skew = 1.0f;

    /** Unit suffix shown in the UI ("Hz", "st", "dB", "bit"); empty for bare numbers. */
    const char* unit = "";

    /** Non-null for choice parameters: a null-terminated array of option names. When set,
        the parameter is created as an AudioParameterChoice and the UI renders a combo box
        rather than a rotary. */
    const char* const* choices = nullptr;
    int numChoices = 0;

    /**
        Whether the effect reads this parameter once at block start or continuously.

        This is the crux of how modulation reaches effects, and it is a per-parameter
        property because the right answer genuinely differs:

          - latchAtBlockStart = true: the value is read in onBlockStart() and held for the
            block. Correct for anything that defines the *structure* of what the effect is
            doing -- Stutter's rate (changing loop length mid-loop would tear the read
            anchor), Reverse's slice length, Filter's type. Modulating these still works;
            the modulated value is simply sampled at each trigger rather than continuously.

          - latchAtBlockStart = false: the value is read from SampleContext::modulatedParams
            every sample. Correct for anything that is musically *meant* to sweep -- Filter's
            cutoff, Crush's bit depth, a lane's mix.

        Getting this wrong is audible in a specific way: a latched parameter marked
        continuous will zipper or click when modulated, and a continuous parameter marked
        latched will feel inert under a curve that should be sweeping it.
    */
    bool latchAtBlockStart = true;

    /** Whether a modulation curve may target this parameter. A few parameters are excluded
        because modulating them is meaningless rather than merely unusual (SE2 does the same
        thing -- its Jitter Rate is "inherently locked. It cannot be split or be displayed in
        the TVM"). Unroutable parameters are omitted from the curve editor's target list. */
    bool modulatable = true;
};

/** A lane's full parameter set: a pointer to its static descriptor array plus its length.
    Returned by LaneEffect::getParamDescriptors(). */
struct ParamDescriptorSet
{
    const ParamDescriptor* descriptors = nullptr;
    int count = 0;

    const ParamDescriptor* begin() const noexcept { return descriptors; }
    const ParamDescriptor* end() const noexcept { return descriptors + count; }
    const ParamDescriptor& operator[] (int i) const noexcept { return descriptors[i]; }
};

/**
    A lane's parameter values for one block, in natural units, indexed by the lane's own
    descriptor order (params[i] corresponds to getParamDescriptors()[i]).

    This is what replaces every effect's v1 getParam(apvts, ...) helper. Effects receive it
    by const reference in onBlockStart() and never look anything up themselves -- which is
    what lets the same effect be driven by a scene snapshot, a live overlay, a modulation
    curve, or a unit test fixture without knowing the difference.
*/
struct LaneParams
{
    std::array<float, maxParamsPerLane> params {};

    /** Read a parameter by descriptor position. Out-of-range slots read as 0 rather than
        UB, so an effect asking for a parameter it did not declare degrades to a defined
        value instead of reading past the array. */
    float get (int i) const noexcept
    {
        return (i >= 0 && i < maxParamsPerLane) ? params[(size_t) i] : 0.0f;
    }

    /** Read a parameter as an integer index (for choice parameters). Rounds rather than
        truncates: a modulated choice value of 1.9 should mean option 2, not option 1. */
    int getIndex (int i) const noexcept
    {
        return (int) std::lround ((double) get (i));
    }

    void set (int i, float v) noexcept
    {
        if (i >= 0 && i < maxParamsPerLane)
            params[(size_t) i] = v;
    }
};

/** Fill a LaneParams with every descriptor's default. Used to build the Init scene and to
    give unit tests a known-good starting point that matches the shipped defaults exactly. */
inline LaneParams makeDefaultLaneParams (const ParamDescriptorSet& set) noexcept
{
    LaneParams p;
    for (int i = 0; i < set.count && i < maxParamsPerLane; ++i)
        p.params[(size_t) i] = set[i].defaultValue;
    return p;
}

} // namespace stutter
