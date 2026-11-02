#pragma once

#include "Controller/ActionRegistry.h"
#include "Controller/ControllerMapping.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>
#include <map>

namespace pathboard
{

// Connects a MIDI controller to the application's actions through a ControllerMapping.
//  - MIDI arrives on the driver's thread, is queued and handled on the message thread (never in the audio thread).
//  - 14-bit controls (MSB + LSB) are combined; relative encoders are turned into signed tick counts.
//  - Learn mode assigns the next message to an action; a monitor callback reports all traffic.
//  - LEDs: buttons whose mapping has `led` set are lit according to the action's state.
class ControllerManager : public juce::MidiInputCallback,
                          public juce::ChangeBroadcaster,
                          private juce::AsyncUpdater,
                          private juce::Timer
{
public:
    ControllerManager (ActionRegistry& registry, ControllerMapping& mapping);
    ~ControllerManager() override;

    // Devices
    static juce::Array<juce::MidiDeviceInfo> getInputDevices();
    bool connect (const juce::String& inputIdentifier);    // also opens the output with the same name for LEDs
    void disconnect();
    bool isConnected() const { return input != nullptr; }
    juce::String getConnectedName() const { return connectedName; }
    // Opens the first device whose name contains the mapping's preferred device name; keeps watching for
    // it, so plugging the controller in later (or after a crash) connects it automatically.
    void setAutoConnect (bool enabled);

    // Message handling (message thread). Public so tests can inject messages.
    void processMessage (const juce::MidiMessage& message);
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;

    // Learn
    void startLearn (const juce::String& actionId);
    void cancelLearn();
    bool isLearning() const { return learning.isNotEmpty(); }
    juce::String getLearningAction() const { return learning; }
    std::function<void (const juce::String& actionId)> onLearned;

    // Activity monitor: one text line per received message.
    std::function<void (const juce::String& line)> onActivity;

    // LEDs
    void refreshLeds (bool force = false);
    std::function<void (const juce::MidiMessage&)> ledSink;     // replaced in tests; default writes to the MIDI output
    void flashAllLeds();                                        // test button in the settings page

    ControllerMapping& getMapping() { return mapping; }
    void mappingChanged();                                      // after the mapping was edited: save, refresh
    std::function<void()> onMappingChanged;

private:
    void handleAsyncUpdate() override;
    void timerCallback() override;
    void dispatch (const MappingEntry& entry, const ControlEvent& event);
    void describeActivity (const juce::MidiMessage&, const MappingEntry*);

    ActionRegistry& registry;
    ControllerMapping& mapping;

    std::unique_ptr<juce::MidiInput> input;
    std::unique_ptr<juce::MidiOutput> output;
    juce::String connectedName;
    bool autoConnect = false;
    int autoConnectCountdown = 0;

    juce::SpinLock queueLock;
    std::vector<juce::MidiMessage> queue;

    // 14-bit controls: last MSB and LSB per (channel, controller)
    std::map<int, int> msbValues, lsbValues;
    juce::String learning;
    std::map<juce::String, bool> ledCache;

    JUCE_DECLARE_WEAK_REFERENCEABLE (ControllerManager)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControllerManager)
};

} // namespace pathboard
