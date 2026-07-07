#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include <vector>
#include "ParameterIDs.h"

namespace stutter
{

/** A single draggable breakpoint in a curve: position (0..1 across the cycle),
    value (0..1), and curvature of the segment leading OUT of this point (-1..1, 0 = linear). */
struct CurvePoint
{
    float position = 0.0f;
    float value = 0.5f;
    float curvature = 0.0f;
};

/**
    One modulation curve (Volume, Filter, or Pan). Holds breakpoints, bakes them into a
    lookup table (audio-rate friendly), and can be evaluated at any phase 0..1.

    Table baking happens off the audio thread (UI edits / preset loads); processBlock only
    ever reads the already-baked table, so this is real-time safe to *read*.
*/
class CurveModulator
{
public:
    static constexpr int tableSize = 1024;

    CurveModulator()
    {
        // Default: flat line at 0.5 (no modulation)
        points = { { 0.0f, 0.5f, 0.0f }, { 1.0f, 0.5f, 0.0f } };
        bakeTable();
    }

    void setPoints (std::vector<CurvePoint> newPoints)
    {
        if (newPoints.size() < 2)
            return;
        std::sort (newPoints.begin(), newPoints.end(),
                    [] (const CurvePoint& a, const CurvePoint& b) { return a.position < b.position; });
        points = std::move (newPoints);
        bakeTable();
    }

    const std::vector<CurvePoint>& getPoints() const noexcept { return points; }

    void setEnabled (bool e) noexcept { enabled = e; }
    bool isEnabled() const noexcept { return enabled; }

    void setSyncDivision (int divIndex) noexcept { syncDivIndex = divIndex; }
    int getSyncDivision() const noexcept { return syncDivIndex; }

    /** Evaluate the baked table at phase 0..1 (wraps). Real-time safe. */
    float getValueAtPhase (float phase) const noexcept
    {
        phase = phase - std::floor (phase);
        const float posF = phase * (float) (tableSize - 1);
        const int i0 = (int) posF;
        const int i1 = juce::jmin (i0 + 1, tableSize - 1);
        const float frac = posF - (float) i0;
        return table[(size_t) i0] + frac * (table[(size_t) i1] - table[(size_t) i0]);
    }

    // ---- Preset shapes ----
    void applyPreset (const juce::String& presetName)
    {
        std::vector<CurvePoint> p;
        if (presetName == "SawDown")
            p = { { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } };
        else if (presetName == "SawUp")
            p = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.0f } };
        else if (presetName == "Sine")
            p = { { 0.0f, 0.5f, 0.0f }, { 0.25f, 1.0f, 0.0f }, { 0.5f, 0.5f, 0.0f }, { 0.75f, 0.0f, 0.0f }, { 1.0f, 0.5f, 0.0f } };
        else if (presetName == "Square")
            p = { { 0.0f, 1.0f, 1.0f }, { 0.5f, 1.0f, -1.0f }, { 0.5001f, 0.0f, 1.0f }, { 1.0f, 0.0f, -1.0f } };
        else if (presetName == "SidechainDuck")
            p = { { 0.0f, 0.0f, 0.0f }, { 0.08f, 1.0f, -0.6f }, { 1.0f, 0.9f, 0.6f } };
        else if (presetName == "Steps")
            p = { { 0.0f, 0.0f, 1.0f }, { 0.24f, 0.0f, -1.0f }, { 0.25f, 0.5f, 1.0f }, { 0.49f, 0.5f, -1.0f },
                  { 0.5f, 1.0f, 1.0f }, { 0.74f, 1.0f, -1.0f }, { 0.75f, 0.25f, 1.0f }, { 1.0f, 0.25f, 0.0f } };
        else
            return;

        setPoints (std::move (p));
    }

    // ---- Persistence ----
    juce::ValueTree toValueTree() const
    {
        juce::ValueTree tree (ID::curveNode);
        tree.setProperty (ID::propEnabled, enabled, nullptr);
        tree.setProperty (ID::propSyncDiv, syncDivIndex, nullptr);
        for (auto& p : points)
        {
            juce::ValueTree pt (ID::pointNode);
            pt.setProperty (ID::propPosition, p.position, nullptr);
            pt.setProperty (ID::propValue, p.value, nullptr);
            pt.setProperty (ID::propCurvature, p.curvature, nullptr);
            tree.appendChild (pt, nullptr);
        }
        return tree;
    }

    void fromValueTree (const juce::ValueTree& tree)
    {
        if (! tree.isValid())
            return;

        enabled = tree.getProperty (ID::propEnabled, true);
        syncDivIndex = tree.getProperty (ID::propSyncDiv, syncDivIndex);

        std::vector<CurvePoint> pts;
        for (int i = 0; i < tree.getNumChildren(); ++i)
        {
            auto child = tree.getChild (i);
            if (child.hasType (ID::pointNode))
            {
                CurvePoint p;
                p.position = child.getProperty (ID::propPosition, 0.0f);
                p.value = child.getProperty (ID::propValue, 0.5f);
                p.curvature = child.getProperty (ID::propCurvature, 0.0f);
                pts.push_back (p);
            }
        }

        if (pts.size() >= 2)
            setPoints (std::move (pts));
    }

private:
    void bakeTable()
    {
        for (int i = 0; i < tableSize; ++i)
        {
            const float phase = (float) i / (float) (tableSize - 1);
            table[(size_t) i] = evaluateAt (phase);
        }
    }

    float evaluateAt (float phase) const
    {
        // Find the segment [pA, pB] that contains phase
        size_t segStart = 0;
        for (size_t i = 0; i + 1 < points.size(); ++i)
        {
            if (phase >= points[i].position && phase <= points[i + 1].position)
            {
                segStart = i;
                break;
            }
            segStart = i; // fallback: last segment if phase beyond range
        }

        const CurvePoint& a = points[segStart];
        const CurvePoint& b = points[juce::jmin (segStart + 1, points.size() - 1)];

        const float span = b.position - a.position;
        float t = span > 1.0e-6f ? (phase - a.position) / span : 0.0f;
        t = juce::jlimit (0.0f, 1.0f, t);

        // Curvature warps t: curvature in [-1,1]. Positive = ease-in (slow start),
        // negative = ease-out (fast start). Implemented via power curve.
        const float c = a.curvature;
        float shapedT = t;
        if (std::abs (c) > 1.0e-3f)
        {
            const float exponent = std::pow (10.0f, -c); // c=1 -> exponent 0.1 (fast rise), c=-1 -> exponent 10
            shapedT = std::pow (t, exponent);
        }

        return a.value + shapedT * (b.value - a.value);
    }

    std::vector<CurvePoint> points;
    std::array<float, tableSize> table {};
    bool enabled = true;
    int syncDivIndex = 4; // index into tempo-sync division table, default 1/4 bar or similar
};

} // namespace stutter
