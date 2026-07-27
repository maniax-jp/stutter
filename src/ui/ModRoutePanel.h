#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "StutterLookAndFeel.h"
#include "../state/SceneDocument.h"

class StutterAudioProcessor;

namespace stutter::ui
{

/**
    Modulation routing table: which curve drives which parameter, how deep, and how fast.

    The existing CurveEditor draws and edits curve SHAPES for the three fixed v1 targets.
    This is the other half that v2 needs -- the routing itself -- and it is deliberately a
    separate component rather than more controls bolted onto the curve editor. Shape editing
    is a direct-manipulation task and routing is a tabular one; putting a target dropdown, a
    depth field and a speed field on every curve editor would crowd the drawing surface that
    is the editor's whole point.

    One row per routed curve, plus an empty row to add one. Targets are named from each
    effect's ParamDescriptor table, so a lane's parameters appear under their real labels
    rather than as indices -- which is the payoff of having made the descriptors the single
    source of truth back in WP1.
*/
class ModRoutePanel : public juce::Component
{
public:
    ModRoutePanel (StutterAudioProcessor& processor, SceneDocument& document);
    ~ModRoutePanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void setSceneIndex (int index);

    /** Rebuild from the document, e.g. after a preset load or a scene change. */
    void refresh();

private:
    struct RouteRow
    {
        std::unique_ptr<juce::ComboBox> target;
        std::unique_ptr<juce::Slider> depth;
        std::unique_ptr<juce::ComboBox> speed;
        std::unique_ptr<juce::TextButton> remove;
        int curveIndex = -1;
    };

    void rebuildRows();
    void populateTargetMenu (juce::ComboBox& box) const;
    void applyRow (int rowIndex);
    void addRoute();
    void removeRoute (int curveIndex);

    /** Menu item id for a parameter index. Offset by one because 0 means "nothing chosen"
        in juce::ComboBox, and a legitimate ParamIndex of 0 (lane 0, param 0) would otherwise
        be indistinguishable from an empty selection. */
    static int menuIdForParam (int paramIdx) noexcept { return paramIdx + 1; }
    static int paramForMenuId (int menuId) noexcept { return menuId - 1; }

    StutterAudioProcessor& proc;
    SceneDocument& doc;
    int sceneIndex = 0;

    std::vector<std::unique_ptr<RouteRow>> rows;
    juce::TextButton addButton { "+ ROUTE" };

    static constexpr int rowHeight = 28;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModRoutePanel)
};

} // namespace stutter::ui
