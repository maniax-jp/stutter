#include "FactoryPresets.h"
#include "dsp/ParameterIDs.h"

// Translates .tmp/PRESETS_DESIGN.md (28 factory presets) into concrete parameter/step/curve
// data. Lane index legend (fixed by SPEC/ParameterIDs.h):
//   0 Stutter (ST), 1 TapeStop (TS), 2 TapeStart (TT), 3 Reverse (RV),
//   4 Repitch (RP), 5 Gate (GT), 6 Filter (FL), 7 Crush (CR)
//
// Rate/slice-length choice index legend (StutterEffect::rateToFraction / ReverseEffect,
// shared table): 0=1/4 1=1/8 2=1/16 3=1/32 4=1/64 5=1/4T 6=1/8T 7=1/16T 8=1/4. 9=1/8. 10=1/16.
//
// Curve sync-division index legend (PluginProcessor.cpp barFractionTable):
// 0=1/1 1=1/2 2=1/4 3=1/8 4=1/16

namespace stutter
{
using P = FactoryPresetDef::ParamValue;
using S = FactoryPresetDef::StepOn;
using CP = FactoryPresetDef::CurvePointDef;
using CV = FactoryPresetDef::CurveDef;

namespace
{
juce::String lp (int lane, const juce::String& name) { return ID::lanePrefix (lane) + name; }

// Helper: steps for a lane at 1-based step numbers "on" (design doc is 1..16); converts to 0-based.
std::vector<S> stepsFor (int lane, std::initializer_list<int> oneBasedSteps)
{
    std::vector<S> out;
    out.reserve (oneBasedSteps.size());
    for (int s : oneBasedSteps)
        out.push_back ({ lane, s - 1 });
    return out;
}

void append (std::vector<S>& dst, std::vector<S> src)
{
    dst.insert (dst.end(), src.begin(), src.end());
}

constexpr int laneStutter = 0, laneTapeStop = 1, laneTapeStart = 2, laneReverse = 3,
              laneRepitch = 4, laneGate = 5, laneFilter = 6, laneCrush = 7;
}

const std::vector<FactoryPresetDef>& getFactoryPresetDefs()
{
    static const std::vector<FactoryPresetDef> defs = [] {
        std::vector<FactoryPresetDef> v;

        // ================= Stutter系 (6) =================

        // 1. Classic Stutter Build -- ST decay=0.7, steps 9-16 ON, pitchSlide=0 (gradient comes
        //    from decay shrinking the loop each repeat across the sustained ON region).
        {
            FactoryPresetDef d;
            d.name = "Classic Stutter Build";
            d.category = "Stutter";
            d.paramValues = {
                { lp (laneStutter, ID::stutterRate), 1.0f }, // 1/8 base rate
                { lp (laneStutter, ID::stutterDecay), 0.7f },
                { lp (laneStutter, ID::stutterPitchSlide), 0.0f },
            };
            d.stepsOn = stepsFor (laneStutter, { 9, 10, 11, 12, 13, 14, 15, 16 });
            v.push_back (d);
        }

        // 2. Eighth Bounce -- ST rate=1/8, decay=0, steps 5,6,13,14 ON.
        {
            FactoryPresetDef d;
            d.name = "Eighth Bounce";
            d.category = "Stutter";
            d.paramValues = {
                { lp (laneStutter, ID::stutterRate), 1.0f },
                { lp (laneStutter, ID::stutterDecay), 0.0f },
                { lp (laneStutter, ID::stutterPitchSlide), 0.0f },
            };
            d.stepsOn = stepsFor (laneStutter, { 5, 6, 13, 14 });
            v.push_back (d);
        }

        // 3. Rising Riser -- ST rate=1/16, pitchSlide=+12, decay=0.5, steps 13-16 ON.
        {
            FactoryPresetDef d;
            d.name = "Rising Riser";
            d.category = "Stutter";
            d.paramValues = {
                { lp (laneStutter, ID::stutterRate), 2.0f },
                { lp (laneStutter, ID::stutterDecay), 0.5f },
                { lp (laneStutter, ID::stutterPitchSlide), 12.0f },
            };
            d.stepsOn = stepsFor (laneStutter, { 13, 14, 15, 16 });
            v.push_back (d);
        }

        // 4. Falling Glitch -- ST rate=1/32, pitchSlide=-12, decay=0.3, steps 4,8,12,16 ON.
        {
            FactoryPresetDef d;
            d.name = "Falling Glitch";
            d.category = "Stutter";
            d.paramValues = {
                { lp (laneStutter, ID::stutterRate), 3.0f },
                { lp (laneStutter, ID::stutterDecay), 0.3f },
                { lp (laneStutter, ID::stutterPitchSlide), -12.0f },
            };
            d.stepsOn = stepsFor (laneStutter, { 4, 8, 12, 16 });
            v.push_back (d);
        }

        // 5. Machine Gun Fill -- ST rate=1/64, decay=0, steps 15,16 ON.
        {
            FactoryPresetDef d;
            d.name = "Machine Gun Fill";
            d.category = "Stutter";
            d.paramValues = {
                { lp (laneStutter, ID::stutterRate), 4.0f },
                { lp (laneStutter, ID::stutterDecay), 0.0f },
                { lp (laneStutter, ID::stutterPitchSlide), 0.0f },
            };
            d.stepsOn = stepsFor (laneStutter, { 15, 16 });
            v.push_back (d);
        }

        // 6. Triplet Skip -- ST rate=1/8T, steps 3,7,11,15 ON.
        {
            FactoryPresetDef d;
            d.name = "Triplet Skip";
            d.category = "Stutter";
            d.paramValues = {
                { lp (laneStutter, ID::stutterRate), 6.0f },
                { lp (laneStutter, ID::stutterDecay), 0.0f },
                { lp (laneStutter, ID::stutterPitchSlide), 0.0f },
            };
            d.stepsOn = stepsFor (laneStutter, { 3, 7, 11, 15 });
            v.push_back (d);
        }

        // ================= Tape系 (5) =================

        // 7. Tape Drop -- TS steps 13-16 ON, curve=0.7, time=0.8.
        {
            FactoryPresetDef d;
            d.name = "Tape Drop";
            d.category = "Tape";
            d.paramValues = {
                { lp (laneTapeStop, ID::tapeStopCurve), 0.7f },
                { lp (laneTapeStop, ID::tapeStopTime), 0.8f },
            };
            d.stepsOn = stepsFor (laneTapeStop, { 13, 14, 15, 16 });
            v.push_back (d);
        }

        // 8. Half-Bar Brake -- TS steps 9-16 ON, curve=0.5, time=1.0.
        {
            FactoryPresetDef d;
            d.name = "Half-Bar Brake";
            d.category = "Tape";
            d.paramValues = {
                { lp (laneTapeStop, ID::tapeStopCurve), 0.5f },
                { lp (laneTapeStop, ID::tapeStopTime), 1.0f },
            };
            d.stepsOn = stepsFor (laneTapeStop, { 9, 10, 11, 12, 13, 14, 15, 16 });
            v.push_back (d);
        }

        // 9. Spin Up Intro -- TT steps 1-4 ON, curve=0.6, time=0.7.
        {
            FactoryPresetDef d;
            d.name = "Spin Up Intro";
            d.category = "Tape";
            d.paramValues = {
                { lp (laneTapeStart, ID::tapeStartCurve), 0.6f },
                { lp (laneTapeStart, ID::tapeStartTime), 0.7f },
            };
            d.stepsOn = stepsFor (laneTapeStart, { 1, 2, 3, 4 });
            v.push_back (d);
        }

        // 10. Brake & Rev -- TS 7-8 ON / TT 9-10 ON: stop then immediately re-accelerate.
        {
            FactoryPresetDef d;
            d.name = "Brake & Rev";
            d.category = "Tape";
            d.paramValues = {
                { lp (laneTapeStop, ID::tapeStopCurve), 0.5f },
                { lp (laneTapeStop, ID::tapeStopTime), 0.5f },
                { lp (laneTapeStart, ID::tapeStartCurve), 0.5f },
                { lp (laneTapeStart, ID::tapeStartTime), 0.5f },
            };
            d.stepsOn = stepsFor (laneTapeStop, { 7, 8 });
            append (d.stepsOn, stepsFor (laneTapeStart, { 9, 10 }));
            v.push_back (d);
        }

        // 11. Slow Motion End -- TS steps 15-16 ON, curve=0.3, time=0.4.
        {
            FactoryPresetDef d;
            d.name = "Slow Motion End";
            d.category = "Tape";
            d.paramValues = {
                { lp (laneTapeStop, ID::tapeStopCurve), 0.3f },
                { lp (laneTapeStop, ID::tapeStopTime), 0.4f },
            };
            d.stepsOn = stepsFor (laneTapeStop, { 15, 16 });
            v.push_back (d);
        }

        // ================= Gate・Sidechain系 (6) =================

        // 12. Trance Gate 16th -- GT all 16 steps ON, rate=1/16 (index 7 in gate's own list),
        //     duty=0.55, shape=0.2.
        {
            FactoryPresetDef d;
            d.name = "Trance Gate 16th";
            d.category = "Gate & Sidechain";
            d.paramValues = {
                { lp (laneGate, ID::gateRate), 7.0f }, // gate's own choice list: ...,"1/16"=index7
                { lp (laneGate, ID::gateDuty), 0.55f },
                { lp (laneGate, ID::gateShape), 0.2f },
            };
            d.stepsOn = stepsFor (laneGate, { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 });
            v.push_back (d);
        }

        // 13. Trance Gate Offbeat -- GT all ON, rate=1/8, duty=0.45, shape=0.1 + Volume=Steps.
        {
            FactoryPresetDef d;
            d.name = "Trance Gate Offbeat";
            d.category = "Gate & Sidechain";
            d.paramValues = {
                { lp (laneGate, ID::gateRate), 5.0f }, // gate list: "1/8"=index5
                { lp (laneGate, ID::gateDuty), 0.45f },
                { lp (laneGate, ID::gateShape), 0.1f },
            };
            d.stepsOn = stepsFor (laneGate, { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 });
            d.curves = {
                { "Volume", true, 3, // Steps preset is authored at 1/8 sync feel -> 1/8 syncDiv
                  { { 0.0f, 0.0f, 1.0f }, { 0.24f, 0.0f, -1.0f }, { 0.25f, 0.5f, 1.0f }, { 0.49f, 0.5f, -1.0f },
                    { 0.5f, 1.0f, 1.0f }, { 0.74f, 1.0f, -1.0f }, { 0.75f, 0.25f, 1.0f }, { 1.0f, 0.25f, 0.0f } } },
            };
            v.push_back (d);
        }

        // 14. Sidechain Pump -- all lanes OFF. Volume=SidechainDuck, syncDiv=1/4, enabled.
        {
            FactoryPresetDef d;
            d.name = "Sidechain Pump";
            d.category = "Gate & Sidechain";
            d.curves = {
                { "Volume", true, 2 /* 1/4 */,
                  { { 0.0f, 0.5f, 0.0f }, { 0.02f, 0.02f, 0.8f }, { 1.0f, 0.5f, 0.6f } } },
            };
            v.push_back (d);
        }

        // 15. Slow Pump 1/2 -- Volume=SidechainDuck, syncDiv=1/2.
        {
            FactoryPresetDef d;
            d.name = "Slow Pump 1/2";
            d.category = "Gate & Sidechain";
            d.curves = {
                { "Volume", true, 1 /* 1/2 */,
                  { { 0.0f, 0.5f, 0.0f }, { 0.02f, 0.02f, 0.8f }, { 1.0f, 0.5f, 0.6f } } },
            };
            v.push_back (d);
        }

        // 16. Choppy Square -- Volume=Square, syncDiv=1/8.
        {
            FactoryPresetDef d;
            d.name = "Choppy Square";
            d.category = "Gate & Sidechain";
            d.curves = {
                { "Volume", true, 3 /* 1/8 */,
                  { { 0.0f, 1.0f, 1.0f }, { 0.5f, 1.0f, -1.0f }, { 0.5001f, 0.0f, 1.0f }, { 1.0f, 0.0f, -1.0f } } },
            };
            v.push_back (d);
        }

        // 17. Saw Fade Loop -- Volume=SawDown, syncDiv=1/4.
        {
            FactoryPresetDef d;
            d.name = "Saw Fade Loop";
            d.category = "Gate & Sidechain";
            d.curves = {
                { "Volume", true, 2 /* 1/4 */, { { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } } },
            };
            v.push_back (d);
        }

        // ================= Glitch(複合)系 (6) =================

        // 18. Glitch Storm -- ST(1/32): 4,8 / RV: 5-6 / CR(bit=6): 9-12 / TS: 15-16.
        {
            FactoryPresetDef d;
            d.name = "Glitch Storm";
            d.category = "Glitch";
            d.paramValues = {
                { lp (laneStutter, ID::stutterRate), 3.0f }, // 1/32
                { lp (laneStutter, ID::stutterDecay), 0.0f },
                { lp (laneStutter, ID::stutterPitchSlide), 0.0f },
                { lp (laneReverse, ID::reverseSliceLen), 2.0f }, // 1/16 default slice
                { lp (laneCrush, ID::crushBitDepth), 6.0f },
                { lp (laneCrush, ID::crushRateDiv), 0.0f },
                { lp (laneTapeStop, ID::tapeStopCurve), 0.5f },
                { lp (laneTapeStop, ID::tapeStopTime), 0.5f },
            };
            d.stepsOn = stepsFor (laneStutter, { 4, 8 });
            append (d.stepsOn, stepsFor (laneReverse, { 5, 6 }));
            append (d.stepsOn, stepsFor (laneCrush, { 9, 10, 11, 12 }));
            append (d.stepsOn, stepsFor (laneTapeStop, { 15, 16 }));
            v.push_back (d);
        }

        // 19. Broken Beat -- RV: 3-4,11-12 / ST(1/16): 7,15.
        {
            FactoryPresetDef d;
            d.name = "Broken Beat";
            d.category = "Glitch";
            d.paramValues = {
                { lp (laneStutter, ID::stutterRate), 2.0f }, // 1/16
                { lp (laneStutter, ID::stutterDecay), 0.0f },
                { lp (laneStutter, ID::stutterPitchSlide), 0.0f },
                { lp (laneReverse, ID::reverseSliceLen), 2.0f },
            };
            d.stepsOn = stepsFor (laneReverse, { 3, 4, 11, 12 });
            append (d.stepsOn, stepsFor (laneStutter, { 7, 15 }));
            v.push_back (d);
        }

        // 20. Robo Funk -- RP(semitones=-12, slide=0.4): 5-8 / CR(bit=8, rateDiv=0.5): 13-16.
        {
            FactoryPresetDef d;
            d.name = "Robo Funk";
            d.category = "Glitch";
            d.paramValues = {
                { lp (laneRepitch, ID::repitchSemitones), -12.0f },
                { lp (laneRepitch, ID::repitchSlide), 0.4f },
                { lp (laneCrush, ID::crushBitDepth), 8.0f },
                { lp (laneCrush, ID::crushRateDiv), 0.5f },
            };
            d.stepsOn = stepsFor (laneRepitch, { 5, 6, 7, 8 });
            append (d.stepsOn, stepsFor (laneCrush, { 13, 14, 15, 16 }));
            v.push_back (d);
        }

        // 21. Stutter & Dive -- ST(1/16, decay=0.6): 9-12 / TS(curve=0.8): 13-16.
        {
            FactoryPresetDef d;
            d.name = "Stutter & Dive";
            d.category = "Glitch";
            d.paramValues = {
                { lp (laneStutter, ID::stutterRate), 2.0f },
                { lp (laneStutter, ID::stutterDecay), 0.6f },
                { lp (laneStutter, ID::stutterPitchSlide), 0.0f },
                { lp (laneTapeStop, ID::tapeStopCurve), 0.8f },
                { lp (laneTapeStop, ID::tapeStopTime), 0.5f },
            };
            d.stepsOn = stepsFor (laneStutter, { 9, 10, 11, 12 });
            append (d.stepsOn, stepsFor (laneTapeStop, { 13, 14, 15, 16 }));
            v.push_back (d);
        }

        // 22. Reverse Tail -- RV: 13-16 (sliceLen=1/4, index 0).
        {
            FactoryPresetDef d;
            d.name = "Reverse Tail";
            d.category = "Glitch";
            d.paramValues = {
                { lp (laneReverse, ID::reverseSliceLen), 0.0f }, // 1/4
            };
            d.stepsOn = stepsFor (laneReverse, { 13, 14, 15, 16 });
            v.push_back (d);
        }

        // 23. Octave Wobble -- RP(semitones=+12, slide=0.8): 2,6,10,14.
        {
            FactoryPresetDef d;
            d.name = "Octave Wobble";
            d.category = "Glitch";
            d.paramValues = {
                { lp (laneRepitch, ID::repitchSemitones), 12.0f },
                { lp (laneRepitch, ID::repitchSlide), 0.8f },
            };
            d.stepsOn = stepsFor (laneRepitch, { 2, 6, 10, 14 });
            v.push_back (d);
        }

        // ================= Filter・Texture系 (5) =================

        // 24. LoFi Crusher -- CR all 16 ON, bit=8, rateDiv=0.6 + Filter=Sine, syncDiv=1/1.
        {
            FactoryPresetDef d;
            d.name = "LoFi Crusher";
            d.category = "Filter & Texture";
            d.paramValues = {
                { lp (laneCrush, ID::crushBitDepth), 8.0f },
                { lp (laneCrush, ID::crushRateDiv), 0.6f },
            };
            d.stepsOn = stepsFor (laneCrush, { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 });
            {
                std::vector<CP> sinePts;
                constexpr int sineSegments = 16;
                sinePts.reserve (sineSegments + 1);
                for (int i = 0; i <= sineSegments; ++i)
                {
                    const float phase = (float) i / (float) sineSegments;
                    const float value = 0.5f + 0.5f * std::sin (juce::MathConstants<float>::twoPi * phase);
                    sinePts.push_back ({ phase, value, 0.0f });
                }
                d.curves = { { "Filter", true, 0 /* 1/1 */, sinePts } };
            }
            v.push_back (d);
        }

        // 25. Filter Sweep Bar -- FL all ON, type=LP, cutoff=800, res=0.6, lfoRate=1/1, lfoDepth=0.8.
        {
            FactoryPresetDef d;
            d.name = "Filter Sweep Bar";
            d.category = "Filter & Texture";
            d.paramValues = {
                { lp (laneFilter, ID::filterType), 0.0f }, // LP
                { lp (laneFilter, ID::filterCutoff), 800.0f },
                { lp (laneFilter, ID::filterResonance), 0.6f },
                { lp (laneFilter, ID::filterLfoRate), 2.0f }, // filter's own list: "1/1"=index2
                { lp (laneFilter, ID::filterLfoDepth), 0.8f },
            };
            d.stepsOn = stepsFor (laneFilter, { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 });
            v.push_back (d);
        }

        // 26. Acid Wobble -- FL all ON, type=LP, res=0.85, lfoRate=1/4, lfoDepth=1.0 + Pan=Sine 1/8.
        {
            FactoryPresetDef d;
            d.name = "Acid Wobble";
            d.category = "Filter & Texture";
            d.paramValues = {
                { lp (laneFilter, ID::filterType), 0.0f },
                { lp (laneFilter, ID::filterCutoff), 1000.0f },
                { lp (laneFilter, ID::filterResonance), 0.85f },
                { lp (laneFilter, ID::filterLfoRate), 0.0f }, // filter's own list: "1/4"=index0
                { lp (laneFilter, ID::filterLfoDepth), 1.0f },
            };
            d.stepsOn = stepsFor (laneFilter, { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 });
            {
                std::vector<CP> sinePts;
                constexpr int sineSegments = 16;
                sinePts.reserve (sineSegments + 1);
                for (int i = 0; i <= sineSegments; ++i)
                {
                    const float phase = (float) i / (float) sineSegments;
                    const float value = 0.5f + 0.5f * std::sin (juce::MathConstants<float>::twoPi * phase);
                    sinePts.push_back ({ phase, value, 0.0f });
                }
                d.curves = { { "Pan", true, 3 /* 1/8 */, sinePts } };
            }
            v.push_back (d);
        }

        // 27. Radio Cut -- FL type=BP, cutoff=1500, res=0.7: steps 5-8,13-16 ON.
        {
            FactoryPresetDef d;
            d.name = "Radio Cut";
            d.category = "Filter & Texture";
            d.paramValues = {
                { lp (laneFilter, ID::filterType), 1.0f }, // BP
                { lp (laneFilter, ID::filterCutoff), 1500.0f },
                { lp (laneFilter, ID::filterResonance), 0.7f },
                { lp (laneFilter, ID::filterLfoDepth), 0.0f },
            };
            d.stepsOn = stepsFor (laneFilter, { 5, 6, 7, 8, 13, 14, 15, 16 });
            v.push_back (d);
        }

        // 28. Auto Pan Groove -- all lanes OFF. Pan=Sine 1/8 + Volume=SawDown 1/16, thinned toward
        //     unity so it reads as a subtle groove rather than a hard fade.
        {
            FactoryPresetDef d;
            d.name = "Auto Pan Groove";
            d.category = "Filter & Texture";
            {
                std::vector<CP> sinePts;
                constexpr int sineSegments = 16;
                sinePts.reserve (sineSegments + 1);
                for (int i = 0; i <= sineSegments; ++i)
                {
                    const float phase = (float) i / (float) sineSegments;
                    const float value = 0.5f + 0.5f * std::sin (juce::MathConstants<float>::twoPi * phase);
                    sinePts.push_back ({ phase, value, 0.0f });
                }
                d.curves = {
                    { "Pan", true, 3 /* 1/8 */, sinePts },
                    // Thin SawDown: unity (0.5) down to 0.35 and back to unity -- gentle volume
                    // breathing rather than a hard fade, per the design doc's "薄め" note.
                    { "Volume", true, 4 /* 1/16 */, { { 0.0f, 0.5f, 0.0f }, { 0.5f, 0.35f, 0.0f }, { 1.0f, 0.5f, 0.0f } } },
                };
            }
            v.push_back (d);
        }

        return v;
    }();

    return defs;
}

} // namespace stutter
