#include "ModRoutePanel.h"
#include "../PluginProcessor.h"
#include "BlockGrid.h"

namespace stutter::ui
{

namespace
{
/** Speed multipliers, snapped to the set the engine expects. Arbitrary values would drift
    in phase over a long pattern, which is why the engine snaps rather than accepting a
    continuous control. */
const float speedValues[] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };
const char* speedLabels[] = { "1/4x", "1/2x", "1x", "2x", "4x" };
constexpr int numSpeeds = 5;
}

ModRoutePanel::ModRoutePanel (StutterAudioProcessor& processor, SceneDocument& document)
    : proc (processor), doc (document)
{
    addAndMakeVisible (addButton);
    addButton.onClick = [this] { addRoute(); };
    refresh();
}

ModRoutePanel::~ModRoutePanel() = default;

void ModRoutePanel::setSceneIndex (int index)
{
    if (index == sceneIndex)
        return;
    sceneIndex = index;
    refresh();
}

void ModRoutePanel::refresh()
{
    rebuildRows();
    resized();
    repaint();
}

void ModRoutePanel::populateTargetMenu (juce::ComboBox& box) const
{
    box.clear (juce::dontSendNotification);

    // Lane parameters, grouped by lane and named from each effect's descriptor table. This
    // is why ParamDescriptor exists as the single source of truth -- the menu, the APVTS
    // layout and the effect's own read all come from the same declaration, so a parameter
    // cannot appear here under a name it does not actually have.
    for (int lane = 0; lane < maxLanes; ++lane)
    {
        auto* effect = proc.getBlockSequencerEffect (lane);
        if (effect == nullptr)
            continue;

        const auto set = effect->getParamDescriptors();
        if (set.count <= 0)
            continue;

        box.addSectionHeading (BlockGrid::getLaneName (lane));
        for (int i = 0; i < set.count && i < maxParamsPerLane; ++i)
        {
            // Some parameters are structurally unmodulatable; omitting them beats listing
            // them and silently ignoring the choice.
            if (! set[i].modulatable)
                continue;
            box.addItem (set[i].label, menuIdForParam (paramIndex (lane, i)));
        }
    }

    box.addSectionHeading ("Global");
    box.addItem ("Dry/Wet", menuIdForParam (paramIndex (GlobalParam::dryWet)));
    box.addItem ("Output Gain", menuIdForParam (paramIndex (GlobalParam::outputGain)));
}

void ModRoutePanel::rebuildRows()
{
    rows.clear();

    auto scene = doc.ensureScene (sceneIndex);
    if (! scene.isValid())
        return;

    auto curves = scene.getChildWithName (SceneIDs::curvesNode);
    if (! curves.isValid())
        return;

    for (int i = 0; i < curves.getNumChildren(); ++i)
    {
        const auto curve = curves.getChild (i);

        auto row = std::make_unique<RouteRow>();
        row->curveIndex = i;

        row->target = std::make_unique<juce::ComboBox>();
        populateTargetMenu (*row->target);
        const int target = (int) curve.getProperty (SceneIDs::target, -1);
        if (isValidParamIndex (target))
            row->target->setSelectedId (menuIdForParam (target), juce::dontSendNotification);
        const int rowIndex = (int) rows.size();
        row->target->onChange = [this, rowIndex] { applyRow (rowIndex); };
        addAndMakeVisible (*row->target);

        row->depth = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal,
                                                     juce::Slider::TextBoxRight);
        row->depth->setRange (0.0, 1.0, 0.01);
        row->depth->setValue ((double) curve.getProperty (SceneIDs::depth, 1.0f),
                              juce::dontSendNotification);
        row->depth->setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 18);
        row->depth->onValueChange = [this, rowIndex] { applyRow (rowIndex); };
        addAndMakeVisible (*row->depth);

        row->speed = std::make_unique<juce::ComboBox>();
        for (int s = 0; s < numSpeeds; ++s)
            row->speed->addItem (speedLabels[s], s + 1);
        {
            const float sp = (float) curve.getProperty (SceneIDs::speed, 1.0f);
            int best = 2;
            for (int s = 0; s < numSpeeds; ++s)
                if (std::abs (speedValues[s] - sp) < std::abs (speedValues[best] - sp))
                    best = s;
            row->speed->setSelectedId (best + 1, juce::dontSendNotification);
        }
        row->speed->onChange = [this, rowIndex] { applyRow (rowIndex); };
        addAndMakeVisible (*row->speed);

        row->remove = std::make_unique<juce::TextButton> ("X");
        const int curveIdx = i;
        row->remove->onClick = [this, curveIdx] { removeRoute (curveIdx); };
        addAndMakeVisible (*row->remove);

        rows.push_back (std::move (row));
    }
}

void ModRoutePanel::applyRow (int rowIndex)
{
    if (rowIndex < 0 || rowIndex >= (int) rows.size())
        return;

    auto& row = *rows[(size_t) rowIndex];
    auto scene = doc.ensureScene (sceneIndex);
    auto curves = scene.getChildWithName (SceneIDs::curvesNode);
    if (! curves.isValid() || row.curveIndex < 0 || row.curveIndex >= curves.getNumChildren())
        return;

    auto curve = curves.getChild (row.curveIndex);
    auto& undo = doc.getUndoManager();
    undo.beginNewTransaction();

    const int target = paramForMenuId (row.target->getSelectedId());
    curve.setProperty (SceneIDs::target, isValidParamIndex (target) ? target : -1, &undo);
    curve.setProperty (SceneIDs::depth, (float) row.depth->getValue(), &undo);

    const int speedIdx = juce::jlimit (0, numSpeeds - 1, row.speed->getSelectedId() - 1);
    curve.setProperty (SceneIDs::speed, speedValues[speedIdx], &undo);

    // A route with no target would still be baked and iterated; enabling only when it points
    // somewhere keeps the engine's active-route list honest.
    curve.setProperty (SceneIDs::enabled, isValidParamIndex (target), &undo);

    doc.publish();
}

void ModRoutePanel::addRoute()
{
    auto scene = doc.ensureScene (sceneIndex);
    auto curves = scene.getOrCreateChildWithName (SceneIDs::curvesNode, nullptr);

    if (curves.getNumChildren() >= maxCurves)
        return;

    auto& undo = doc.getUndoManager();
    undo.beginNewTransaction();

    juce::ValueTree c (SceneIDs::curve);
    c.setProperty (SceneIDs::target, -1, nullptr);
    c.setProperty (SceneIDs::depth, 1.0f, nullptr);
    c.setProperty (SceneIDs::speed, 1.0f, nullptr);
    c.setProperty (SceneIDs::enabled, false, nullptr);
    c.setProperty (SceneIDs::tier, 2, nullptr);

    // A default saw ramp, so a freshly added route does something as soon as it is pointed
    // at a parameter rather than sitting flat and looking broken.
    for (int i = 0; i < 2; ++i)
    {
        juce::ValueTree p (SceneIDs::point);
        p.setProperty (SceneIDs::position, (float) i, nullptr);
        p.setProperty (SceneIDs::value, (float) i, nullptr);
        p.setProperty (SceneIDs::curvature, 0.0f, nullptr);
        p.setProperty (SceneIDs::pointWeight, (int) PointWeight::Medium, nullptr);
        c.appendChild (p, nullptr);
    }

    curves.appendChild (c, &undo);
    doc.publish();
    refresh();
}

void ModRoutePanel::removeRoute (int curveIndex)
{
    auto scene = doc.ensureScene (sceneIndex);
    auto curves = scene.getChildWithName (SceneIDs::curvesNode);
    if (! curves.isValid() || curveIndex < 0 || curveIndex >= curves.getNumChildren())
        return;

    auto& undo = doc.getUndoManager();
    undo.beginNewTransaction();
    curves.removeChild (curveIndex, &undo);
    doc.publish();
    refresh();
}

void ModRoutePanel::resized()
{
    auto r = getLocalBounds().reduced (6);

    for (auto& row : rows)
    {
        auto line = r.removeFromTop (rowHeight).reduced (0, 2);
        row->remove->setBounds (line.removeFromRight (26));
        row->speed->setBounds (line.removeFromRight (64).reduced (2, 0));
        row->depth->setBounds (line.removeFromRight (140).reduced (2, 0));
        row->target->setBounds (line.reduced (2, 0));
    }

    addButton.setBounds (r.removeFromTop (rowHeight).removeFromLeft (90).reduced (2));
}

void ModRoutePanel::paint (juce::Graphics& g)
{
    g.fillAll (Palette::bg1);

    if (rows.empty())
    {
        g.setColour (Palette::textLo);
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText ("No modulation routes. Add one to drive any lane parameter from a curve.",
                    getLocalBounds().reduced (10, 0).withTrimmedBottom (rowHeight),
                    juce::Justification::centred, false);
    }
}

} // namespace stutter::ui
