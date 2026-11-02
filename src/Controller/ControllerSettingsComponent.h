#pragma once

#include "Controller/ControllerManager.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace pathboard
{

// Settings page for the controller: choose the device, see what the controller sends (monitor), assign any
// action to any control with "Learn", change how a control is read, save/load mappings.
class ControllerSettingsComponent : public juce::Component,
                                    private juce::TableListBoxModel,
                                    private juce::ChangeListener,
                                    private juce::Timer
{
public:
    ControllerSettingsComponent (ControllerManager& manager, ActionRegistry& registry);
    ~ControllerSettingsComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // TableListBoxModel
    int getNumRows() override { return (int) rows.size(); }
    void paintRowBackground (juce::Graphics&, int row, int width, int height, bool selected) override;
    void paintCell (juce::Graphics&, int row, int column, int width, int height, bool selected) override;
    void cellClicked (int row, int column, const juce::MouseEvent&) override;
    void cellDoubleClicked (int row, int column, const juce::MouseEvent&) override;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void rebuildRows();
    void refreshDevices();
    void addMonitorLine (const juce::String& line);
    const ActionDef* actionAt (int row) const;

    ControllerManager& manager;
    ActionRegistry& registry;
    std::vector<const ActionDef*> rows;

    juce::ComboBox deviceBox;
    juce::TextButton connectButton { "Connect" };
    juce::ToggleButton autoConnectToggle { "Connect automatically" };
    juce::Label statusLabel;
    juce::TableListBox table { "actions", this };
    juce::TextEditor monitor;
    juce::Label jogLabel, bendLabel;
    juce::Slider jogTicks, bendSensitivity;
    juce::TextButton resetButton { "Reset to FLX4 defaults" }, loadButton { "Load..." }, saveButton { "Save as..." }, ledTestButton { "Test LEDs" };
    std::unique_ptr<juce::FileChooser> chooser;
    juce::StringArray monitorLines;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControllerSettingsComponent)
};

} // namespace pathboard
