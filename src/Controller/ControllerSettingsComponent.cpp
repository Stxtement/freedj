#include "ControllerSettingsComponent.h"
#include "UI/DJLookAndFeel.h"

namespace pathboard
{

namespace
{
    enum Column { colAction = 1, colControl, colType, colInvert, colLearn, colClear };

    const char* kindLabel (ControlKind k)
    {
        switch (k)
        {
            case ControlKind::button:           return "Button";
            case ControlKind::absolute7:        return "Knob / fader (7 bit)";
            case ControlKind::absolute14:       return "Knob / fader (14 bit)";
            case ControlKind::relativeCentered: return "Endless (64 = still)";
            case ControlKind::relativeTwos:     return "Endless (+/- steps)";
        }
        return "";
    }
}

ControllerSettingsComponent::ControllerSettingsComponent (ControllerManager& m, ActionRegistry& r) : manager (m), registry (r)
{
    setOpaque (true);

    deviceBox.onChange = [this] { connectButton.setEnabled (deviceBox.getSelectedId() > 0); };
    addAndMakeVisible (deviceBox);

    connectButton.onClick = [this]
    {
        if (manager.isConnected())
        {
            manager.setAutoConnect (false);
            autoConnectToggle.setToggleState (false, juce::dontSendNotification);
            manager.disconnect();
        }
        else
        {
            const auto devices = ControllerManager::getInputDevices();
            const int index = deviceBox.getSelectedId() - 1;
            if (juce::isPositiveAndBelow (index, devices.size()) && ! manager.connect (devices[index].identifier))
                statusLabel.setText ("Could not open the device. Is another program (rekordbox, VirtualDJ, Serato) using it?", juce::dontSendNotification);
        }
    };
    addAndMakeVisible (connectButton);

    autoConnectToggle.setToggleState (true, juce::dontSendNotification);
    autoConnectToggle.onClick = [this] { manager.setAutoConnect (autoConnectToggle.getToggleState()); };
    addAndMakeVisible (autoConnectToggle);

    statusLabel.setColour (juce::Label::textColourId, Colours::textDim);
    addAndMakeVisible (statusLabel);

    table.setColour (juce::ListBox::backgroundColourId, Colours::panel);
    table.setRowHeight (24);
    auto& header = table.getHeader();
    header.addColumn ("Function", colAction, 250, 100, 500, juce::TableHeaderComponent::notSortable);
    header.addColumn ("Control", colControl, 150, 80, 300, juce::TableHeaderComponent::notSortable);
    header.addColumn ("Type", colType, 150, 80, 300, juce::TableHeaderComponent::notSortable);
    header.addColumn ("Invert", colInvert, 55, 40, 80, juce::TableHeaderComponent::notSortable);
    header.addColumn ("", colLearn, 70, 50, 100, juce::TableHeaderComponent::notSortable);
    header.addColumn ("", colClear, 55, 40, 80, juce::TableHeaderComponent::notSortable);
    addAndMakeVisible (table);

    monitor.setMultiLine (true, false);
    monitor.setReadOnly (true);
    monitor.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
    monitor.setColour (juce::TextEditor::backgroundColourId, juce::Colours::black.withAlpha (0.4f));
    monitor.setText ("Move a control on the controller: every MIDI message appears here.\n");
    addAndMakeVisible (monitor);

    auto setupSlider = [this] (juce::Slider& s, juce::Label& l, const juce::String& text, double min, double max, double value, double step)
    {
        l.setText (text, juce::dontSendNotification);
        l.setColour (juce::Label::textColourId, Colours::textDim);
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 20);
        s.setRange (min, max, step);
        s.setValue (value, juce::dontSendNotification);
        addAndMakeVisible (l);
        addAndMakeVisible (s);
    };
    setupSlider (jogTicks, jogLabel, "Jog wheel: ticks per turn (lower = more sensitive)", 64.0, 4096.0, manager.getMapping().jogTicksPerRevolution, 1.0);
    setupSlider (bendSensitivity, bendLabel, "Pitch bend sensitivity", 0.1, 5.0, manager.getMapping().bendSensitivity, 0.05);
    jogTicks.onValueChange = [this] { manager.getMapping().jogTicksPerRevolution = jogTicks.getValue(); manager.mappingChanged(); };
    bendSensitivity.onValueChange = [this] { manager.getMapping().bendSensitivity = bendSensitivity.getValue(); manager.mappingChanged(); };

    resetButton.onClick = [this]
    {
        const auto preferred = manager.getMapping().preferredDevice;
        manager.getMapping() = ControllerMapping::createDefaultFlx4();
        manager.getMapping().preferredDevice = preferred;
        jogTicks.setValue (manager.getMapping().jogTicksPerRevolution, juce::dontSendNotification);
        bendSensitivity.setValue (manager.getMapping().bendSensitivity, juce::dontSendNotification);
        manager.mappingChanged();
    };
    loadButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Load a controller mapping", juce::File(), "*.json");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
        {
            if (! fc.getResult().existsAsFile())
                return;
            const auto result = manager.getMapping().loadFrom (fc.getResult());
            if (result.failed())
                statusLabel.setText ("Could not load: " + result.getErrorMessage(), juce::dontSendNotification);
            jogTicks.setValue (manager.getMapping().jogTicksPerRevolution, juce::dontSendNotification);
            bendSensitivity.setValue (manager.getMapping().bendSensitivity, juce::dontSendNotification);
            manager.mappingChanged();
        });
    };
    saveButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Save the controller mapping", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("my-controller-mapping.json"), "*.json");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this] (const juce::FileChooser& fc)
                              {
                                  if (fc.getResult() != juce::File() && ! manager.getMapping().saveTo (fc.getResult().withFileExtension (".json")))
                                      statusLabel.setText ("Could not save the file.", juce::dontSendNotification);
                              });
    };
    ledTestButton.setTooltip ("Lights every button that has an LED mapped, for a moment.");
    ledTestButton.onClick = [this] { manager.flashAllLeds(); };
    for (auto* b : { &resetButton, &loadButton, &saveButton, &ledTestButton })
        addAndMakeVisible (b);

    manager.onActivity = [this] (const juce::String& line) { addMonitorLine (line); };
    manager.addChangeListener (this);

    rebuildRows();
    refreshDevices();
    changeListenerCallback (nullptr);
    startTimerHz (2);
}

ControllerSettingsComponent::~ControllerSettingsComponent()
{
    manager.removeChangeListener (this);
    manager.onActivity = nullptr;
    manager.cancelLearn();
}

//==============================================================================
void ControllerSettingsComponent::rebuildRows()
{
    rows.clear();
    for (const auto& a : registry.getAll())
        rows.push_back (&a);
    table.updateContent();
}

const ActionDef* ControllerSettingsComponent::actionAt (int row) const
{
    return juce::isPositiveAndBelow (row, (int) rows.size()) ? rows[(size_t) row] : nullptr;
}

void ControllerSettingsComponent::refreshDevices()
{
    const auto devices = ControllerManager::getInputDevices();
    const auto selectedName = deviceBox.getText();
    deviceBox.clear (juce::dontSendNotification);
    int select = 0;
    for (int i = 0; i < devices.size(); ++i)
    {
        deviceBox.addItem (devices[i].name, i + 1);
        if (devices[i].name == manager.getConnectedName() || (select == 0 && devices[i].name == selectedName)
            || (select == 0 && devices[i].name.containsIgnoreCase (manager.getMapping().preferredDevice)))
            select = i + 1;
    }
    if (select == 0 && devices.size() > 0)
        select = 1;
    deviceBox.setSelectedId (select, juce::dontSendNotification);
    connectButton.setEnabled (select > 0 || manager.isConnected());
}

void ControllerSettingsComponent::timerCallback()
{
    refreshDevices();
}

void ControllerSettingsComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (manager.isConnected())
        statusLabel.setText ("Connected: " + manager.getConnectedName(), juce::dontSendNotification);
    else if (ControllerManager::getInputDevices().isEmpty())
        statusLabel.setText ("No MIDI device found. Connect the controller by USB.", juce::dontSendNotification);
    else
        statusLabel.setText ("Not connected", juce::dontSendNotification);

    connectButton.setButtonText (manager.isConnected() ? "Disconnect" : "Connect");
    table.updateContent();
    table.repaint();
}

void ControllerSettingsComponent::addMonitorLine (const juce::String& line)
{
    monitorLines.add (line);
    while (monitorLines.size() > 14)
        monitorLines.remove (0);
    monitor.setText (monitorLines.joinIntoString ("\n"), false);
    monitor.moveCaretToEnd();
}

//==============================================================================
void ControllerSettingsComponent::paintRowBackground (juce::Graphics& g, int row, int, int, bool selected)
{
    const auto* a = actionAt (row);
    if (a != nullptr && manager.getLearningAction() == a->id)
        g.fillAll (Colours::cue.withAlpha (0.35f));
    else if (selected)
        g.fillAll (Colours::deckA.withAlpha (0.25f));
    else if (row % 2 == 1)
        g.fillAll (juce::Colours::white.withAlpha (0.025f));
}

void ControllerSettingsComponent::paintCell (juce::Graphics& g, int row, int column, int width, int height, bool)
{
    const auto* a = actionAt (row);
    if (a == nullptr)
        return;

    const auto* entry = manager.getMapping().findByAction (a->id);
    const bool learningThis = manager.getLearningAction() == a->id;

    g.setFont (juce::FontOptions (13.0f));
    g.setColour (Colours::text);
    juce::String text;
    juce::Colour colour = Colours::text;
    auto justification = juce::Justification::centredLeft;

    switch (column)
    {
        case colAction:  text = a->group + "  -  " + a->label; break;
        case colControl: text = learningThis ? "Move or press a control..." : (entry != nullptr ? entry->control.describe() : juce::String ("not assigned"));
                         colour = learningThis ? juce::Colours::black : (entry != nullptr ? Colours::text : Colours::textDim); break;
        case colType:    text = entry != nullptr ? kindLabel (entry->kind) : ""; colour = Colours::textDim; break;
        case colInvert:  text = entry != nullptr && entry->kind != ControlKind::button ? (entry->invert ? "yes" : "no") : ""; colour = Colours::textDim; justification = juce::Justification::centred; break;
        case colLearn:   text = learningThis ? "Cancel" : "Learn"; colour = Colours::deckA; justification = juce::Justification::centred; break;
        case colClear:   text = entry != nullptr ? "Clear" : ""; colour = Colours::danger; justification = juce::Justification::centred; break;
        default: break;
    }
    g.setColour (colour);
    g.drawText (text, 6, 0, width - 10, height, justification, true);
}

void ControllerSettingsComponent::cellClicked (int row, int column, const juce::MouseEvent&)
{
    const auto* a = actionAt (row);
    if (a == nullptr)
        return;

    auto& mapping = manager.getMapping();
    const auto* entry = mapping.findByAction (a->id);

    if (column == colLearn)
    {
        if (manager.getLearningAction() == a->id)
            manager.cancelLearn();
        else
            manager.startLearn (a->id);
    }
    else if (column == colClear && entry != nullptr)
    {
        mapping.clearAction (a->id);
        manager.mappingChanged();
    }
    else if (column == colInvert && entry != nullptr && entry->kind != ControlKind::button)
    {
        auto changed = *entry;
        changed.invert = ! changed.invert;
        mapping.set (changed);
        manager.mappingChanged();
    }
    else if (column == colType && entry != nullptr)
    {
        juce::PopupMenu menu;
        const ControlKind kinds[] = { ControlKind::button, ControlKind::absolute7, ControlKind::absolute14, ControlKind::relativeCentered, ControlKind::relativeTwos };
        for (int i = 0; i < 5; ++i)
            menu.addItem (i + 1, kindLabel (kinds[i]), true, entry->kind == kinds[i]);
        const auto actionId = a->id;
        menu.showMenuAsync ({}, [this, kinds, actionId] (int result)
        {
            if (result <= 0)
                return;
            if (const auto* current = manager.getMapping().findByAction (actionId))
            {
                auto changed = *current;
                changed.kind = kinds[result - 1];
                manager.getMapping().set (changed);
                manager.mappingChanged();
            }
        });
    }
    table.repaint();
}

void ControllerSettingsComponent::cellDoubleClicked (int row, int column, const juce::MouseEvent&)
{
    if (column == colAction || column == colControl)
        if (const auto* a = actionAt (row))
            manager.startLearn (a->id);
}

//==============================================================================
void ControllerSettingsComponent::paint (juce::Graphics& g)
{
    g.fillAll (Colours::panel);
}

void ControllerSettingsComponent::resized()
{
    auto area = getLocalBounds().reduced (12);

    auto top = area.removeFromTop (28);
    connectButton.setBounds (top.removeFromRight (110));
    top.removeFromRight (8);
    deviceBox.setBounds (top.removeFromLeft (280));
    top.removeFromLeft (10);
    autoConnectToggle.setBounds (top);
    area.removeFromTop (4);
    statusLabel.setBounds (area.removeFromTop (22));
    area.removeFromTop (6);

    auto bottom = area.removeFromBottom (36);
    resetButton.setBounds (bottom.removeFromLeft (170).reduced (2));
    loadButton.setBounds (bottom.removeFromLeft (90).reduced (2));
    saveButton.setBounds (bottom.removeFromLeft (100).reduced (2));
    ledTestButton.setBounds (bottom.removeFromLeft (100).reduced (2));
    area.removeFromBottom (6);

    auto sliders = area.removeFromBottom (54);
    auto row1 = sliders.removeFromTop (26);
    jogLabel.setBounds (row1.removeFromLeft (330));
    jogTicks.setBounds (row1);
    bendLabel.setBounds (sliders.removeFromLeft (330));
    bendSensitivity.setBounds (sliders);
    area.removeFromBottom (6);

    monitor.setBounds (area.removeFromBottom (150));
    area.removeFromBottom (6);
    table.setBounds (area);
}

} // namespace pathboard
