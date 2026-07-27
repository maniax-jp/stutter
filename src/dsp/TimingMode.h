#pragma once
#include <juce_core/juce_core.h>

namespace stutter
{

/**
    Rhythmic modifier applied to a note value, following Glitch 2's Common Timing model.

    Glitch 2 gives every time-based module the same four-way selector, which is a good idea
    for a reason worth stating: it makes "1/8 triplet" expressible as (1/8, Triplet) rather
    than as its own entry in a flat list. v1 took the flat-list approach -- rateChoices() in
    ParameterLayout.cpp enumerated eleven strings inline -- which meant adding a dotted entry
    required touching the list, the lookup table, and every preset that indexed past the
    insertion point.

    Decomposing into (base note, modifier) collapses those eleven entries into five bases and
    four modifiers, and makes the two orthogonal in the UI.
*/
enum class TimingMode
{
    Free = 0,   ///< No quantization; the caller's raw value is used as-is.
    Even,       ///< Straight powers of two: 1/4, 1/8, 1/16, ...
    Triplet,    ///< Two thirds of the even duration (three in the space of two).
    Dotted      ///< Three halves of the even duration (note plus half its value).
};

/** Duration multiplier for a timing mode. Free returns 1.0 since an unquantized value is
    already the duration the caller wants. */
constexpr double timingMultiplier (TimingMode mode) noexcept
{
    switch (mode)
    {
        case TimingMode::Triplet: return 2.0 / 3.0;
        case TimingMode::Dotted:  return 3.0 / 2.0;
        case TimingMode::Free:
        case TimingMode::Even:
        default:                  return 1.0;
    }
}

// ---- Base note values ------------------------------------------------------------------
//
// Expressed as a fraction of a bar (4 quarter notes in 4/4), matching what v1's
// rateToFraction() returned. Keeping the same unit matters: it is what lets WP1 land without
// changing a single sample of output, which the golden-baseline gate in
// tests/fixtures/golden/ checks by SHA-256.

/** Number of entries in the base note table. */
inline constexpr int numBaseNotes = 5;

/** Base note value as a fraction of a bar: 1/4, 1/8, 1/16, 1/32, 1/64. */
constexpr double baseNoteFraction (int index) noexcept
{
    // Written as a switch rather than a static table so the whole thing is constexpr and
    // usable in the per-sample path without a guaranteed-init check.
    switch (index)
    {
        case 0:  return 1.0 / 4.0;
        case 1:  return 1.0 / 8.0;
        case 2:  return 1.0 / 16.0;
        case 3:  return 1.0 / 32.0;
        case 4:  return 1.0 / 64.0;
        default: return 1.0 / 16.0;   // matches v1's default rate index of 2
    }
}

/** Fraction of a bar for a (base note, modifier) pair. */
constexpr double noteFraction (int baseIndex, TimingMode mode) noexcept
{
    return baseNoteFraction (baseIndex) * timingMultiplier (mode);
}

// ---- v1 compatibility ------------------------------------------------------------------

/**
    Maps v1's flat eleven-entry rate index onto the same fraction it produced, so effects can
    be migrated to LaneParams without simultaneously changing their rate semantics.

    Two changes at once would make a golden-baseline mismatch ambiguous -- was it the
    parameter plumbing or the rate table? Keeping this function bit-exact to v1's duplicated
    rateToFraction() (StutterEffect.h:145-164 and ReverseEffect.h:113-121, which were
    identical) means WP1 can be verified as a pure refactor. The flat index is retired in
    favour of (base, mode) when the choice parameters are reworked; until then this is the
    single definition the two effects share.

    Index layout: 0-4 straight, 5-7 triplet, 8-10 dotted.

    IMPORTANT -- the fractions are relative to a QUARTER NOTE, not a bar. Index 0 returns
    1/4, and the callers multiply by four divisions-per-quarter, so it produces a 16th-note
    loop. Through v1.1.2 the UI labelled these entries "1/4".."1/64" as if they were bar
    fractions, which made every displayed rate four times longer than what was heard. The
    labels are now corrected in legacyRateIndexLabels() below; the arithmetic here is
    unchanged, so presets are unaffected.

    Out-of-range indices clamp to [0, 10] to match v1's jlimit(0, n-1, index) -- note this
    means a too-large index yields the last table entry (dotted), not the shortest straight
    value. That is not reachable through the choice parameter, whose range is exactly 0..10,
    but modulation can write arbitrary values into a parameter slot, so the edge is defined
    rather than left to diverge from v1.
*/
constexpr double legacyRateIndexToFraction (int index) noexcept
{
    const int clamped = index < 0 ? 0 : (index > 10 ? 10 : index);

    if (clamped >= 5 && clamped <= 7)
        return baseNoteFraction (clamped - 5) * timingMultiplier (TimingMode::Triplet);

    if (clamped >= 8)
        return baseNoteFraction (clamped - 8) * timingMultiplier (TimingMode::Dotted);

    return baseNoteFraction (clamped);
}

/** Number of entries in the legacy rate choice list. */
inline constexpr int numLegacyRateIndices = 11;

/**
    Display labels for the legacy rate indices, in index order.

    These name the duration the DSP actually produces. Index 0's fraction is 1/4 *of a
    quarter note*, which is a 16th, hence "1/16" rather than "1/4". Keeping the labels here
    beside legacyRateIndexToFraction() is deliberate: the two must agree, and v1 kept them
    four files apart, which is how they came to disagree by a factor of four in the first
    place.

    The array is the single definition; legacyRateIndexLabel() indexes into it for callers
    that want one entry, and ParamDescriptor::choices points at it directly.
*/
inline const char* const* legacyRateIndexLabels() noexcept
{
    static const char* const labels[numLegacyRateIndices] = {
        "1/16", "1/32", "1/64", "1/128", "1/256",   // 0-4 straight
        "1/16T", "1/32T", "1/64T",                  // 5-7 triplet
        "1/16.", "1/32.", "1/64."                   // 8-10 dotted
    };
    return labels;
}

/** One label by index; nullptr when out of range, so callers cannot read past the end. */
inline const char* legacyRateIndexLabel (int index) noexcept
{
    return (index >= 0 && index < numLegacyRateIndices) ? legacyRateIndexLabels()[index] : nullptr;
}

} // namespace stutter
