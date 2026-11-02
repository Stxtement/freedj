#include "ControllerManager.h"

namespace pathboard
{

ControllerManager::ControllerManager (ActionRegistry& r, ControllerMapping& m) : registry (r), mapping (m)
{
    ledSink = [this] (const juce::MidiMessage& message)
    {
        if (output != nullptr)
            output->sendMessageNow (message);
    };
    startTimerHz (30);
}

ControllerManager::~ControllerManager()
{
    stopTimer();
    cancelPendingUpdate();
    disconnect();
}

//==============================================================================
juce::Array<juce::MidiDeviceInfo> ControllerManager::getInputDevices()
{
    return juce::MidiInput::getAvailableDevices();
}

bool ControllerManager::connect (const juce::String& identifier)
{
    disconnect();

    for (const auto& d : juce::MidiInput::getAvailableDevices())
    {
        if (d.identifier != identifier)
            continue;

        input = juce::MidiInput::openDevice (d.identifier, this);
        if (input == nullptr)
            return false; // e.g. another program (rekordbox, VirtualDJ) has the device open
        connectedName = d.name;

        // LED output: the output port with the same name.
        for (const auto& o : juce::MidiOutput::getAvailableDevices())
            if (o.name == d.name)
            {
                output = juce::MidiOutput::openDevice (o.identifier);
                break;
            }

        input->start();
        refreshLeds (true);
        sendChangeMessage();
        return true;
    }
    return false;
}

void ControllerManager::disconnect()
{
    if (input != nullptr)
        input->stop();
    input.reset();
    output.reset();
    connectedName.clear();
    ledCache.clear();
    sendChangeMessage();
}

void ControllerManager::setAutoConnect (bool enabled)
{
    autoConnect = enabled;
    autoConnectCountdown = 0;
}

void ControllerManager::timerCallback()
{
    if (autoConnect && ! isConnected() && --autoConnectCountdown <= 0)
    {
        autoConnectCountdown = 60; // look again in 2 s
        for (const auto& d : juce::MidiInput::getAvailableDevices())
            if (d.name.containsIgnoreCase (mapping.preferredDevice))
            {
                connect (d.identifier);
                break;
            }
    }
    else if (isConnected())
    {
        // A device that was unplugged disappears from the list.
        bool present = false;
        for (const auto& d : juce::MidiInput::getAvailableDevices())
            present = present || d.name == connectedName;
        if (! present)
            disconnect();
    }

    if (isConnected())
        refreshLeds();
}

//==============================================================================
void ControllerManager::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& message)
{
    {
        const juce::SpinLock::ScopedLockType sl (queueLock);
        // The cap only protects against a frozen message thread; a burst of jog ticks must never be dropped.
        if (queue.size() < 65536)
            queue.push_back (message);
    }
    triggerAsyncUpdate();
}

void ControllerManager::handleAsyncUpdate()
{
    std::vector<juce::MidiMessage> messages;
    {
        const juce::SpinLock::ScopedLockType sl (queueLock);
        messages.swap (queue);
    }
    for (const auto& m : messages)
        processMessage (m);
}

//==============================================================================
void ControllerManager::startLearn (const juce::String& actionId)
{
    learning = actionId;
    sendChangeMessage();
}

void ControllerManager::cancelLearn()
{
    learning.clear();
    sendChangeMessage();
}

void ControllerManager::mappingChanged()
{
    ledCache.clear();
    refreshLeds (true);
    if (onMappingChanged)
        onMappingChanged();
    sendChangeMessage();
}

void ControllerManager::describeActivity (const juce::MidiMessage& m, const MappingEntry* entry)
{
    if (! onActivity)
        return;

    juce::String line = juce::String::toHexString (m.getRawData(), m.getRawDataSize(), 1).toUpperCase() + "   ";
    if (m.isNoteOnOrOff())
        line << "Note " << (m.isNoteOn() ? "on  " : "off ") << "0x" << juce::String::toHexString (m.getNoteNumber()).toUpperCase() << " ch " << m.getChannel() << " vel " << (int) m.getVelocity();
    else if (m.isController())
        line << "CC  0x" << juce::String::toHexString (m.getControllerNumber()).toUpperCase() << " ch " << m.getChannel() << " value " << m.getControllerValue();
    else
        line << "other";
    line << (entry != nullptr ? "   ->  " + entry->action : juce::String ("   (not mapped)"));
    onActivity (line);
}

void ControllerManager::processMessage (const juce::MidiMessage& m)
{
    MidiControl control;
    int value = 0;
    bool pressed = false;

    if (m.isNoteOnOrOff())
    {
        control = { true, m.getChannel() - 1, m.getNoteNumber() };
        pressed = m.isNoteOn() && m.getVelocity() > 0;
        value = pressed ? 127 : 0;
    }
    else if (m.isController())
    {
        control = { false, m.getChannel() - 1, m.getControllerNumber() };
        value = m.getControllerValue();
        pressed = value > 0;
    }
    else
    {
        return;
    }

    // Learn mode: the next press / movement becomes the assignment.
    if (learning.isNotEmpty())
    {
        // Ignore button releases and the LSB half of an already assigned 14-bit control, so only the real
        // gesture counts. (Controller numbers 32..63 are not LSBs in general: the jog wheel uses 0x22.)
        if ((control.isNote && ! pressed) || (! control.isNote && mapping.findByLsb (control) != nullptr))
            return;

        if (const auto* def = registry.find (learning))
        {
            MappingEntry entry;
            if (const auto* previous = mapping.findByAction (learning))
                entry = *previous;
            entry.action = learning;
            entry.control = control;

            switch (def->type)
            {
                case ActionDef::Type::button:   entry.kind = ControlKind::button; break;
                case ActionDef::Type::absolute: entry.kind = control.isNote ? ControlKind::button : (control.number < 32 ? ControlKind::absolute14 : ControlKind::absolute7); break;
                case ActionDef::Type::relative: entry.kind = ControlKind::relativeCentered; break;
            }
            entry.led = def->ledState != nullptr && control.isNote;

            const auto learnedAction = learning;
            learning.clear();
            mapping.set (entry);
            mappingChanged();
            describeActivity (m, &entry);
            if (onLearned)
                onLearned (learnedAction);
            return;
        }
        learning.clear();
    }

    const auto* entry = mapping.findByControl (control);
    describeActivity (m, entry);

    // LSB of a 14-bit control: remember it, apply the combined value.
    if (entry == nullptr)
    {
        if (const auto* msbEntry = mapping.findByLsb (control))
        {
            const int key = control.channel * 256 + msbEntry->control.number;
            lsbValues[key] = value;
            const double v = (double) ((msbValues[key] << 7) | value) / 16383.0;
            dispatch (*msbEntry, { msbEntry->invert ? 1.0 - v : v, true });
        }
        return;
    }

    ControlEvent event;
    switch (entry->kind)
    {
        case ControlKind::button:
            event.pressed = pressed;
            event.value = pressed ? 1.0 : 0.0;
            break;

        case ControlKind::absolute7:
        {
            const double v = (double) value / 127.0;
            event.value = entry->invert ? 1.0 - v : v;
            event.pressed = true;
            break;
        }

        case ControlKind::absolute14:
        {
            const int key = control.channel * 256 + control.number;
            msbValues[key] = value;
            const double v = (double) ((value << 7) | lsbValues[key]) / 16383.0;
            event.value = entry->invert ? 1.0 - v : v;
            event.pressed = true;
            break;
        }

        case ControlKind::relativeCentered:
        case ControlKind::relativeTwos:
        {
            const int ticks = entry->kind == ControlKind::relativeCentered ? value - 64 : (value < 64 ? value : value - 128);
            if (ticks == 0)
                return;
            event.value = (double) (entry->invert ? -ticks : ticks) * entry->scale;
            event.pressed = true;
            break;
        }
    }
    dispatch (*entry, event);
}

void ControllerManager::dispatch (const MappingEntry& entry, const ControlEvent& event)
{
    if (const auto* def = registry.find (entry.action))
        if (def->handler)
            def->handler (event);
}

//==============================================================================
void ControllerManager::refreshLeds (bool force)
{
    if (! ledSink)
        return;

    for (const auto& entry : mapping.getEntries())
    {
        if (! entry.led || ! entry.control.isNote)
            continue;
        const auto* def = registry.find (entry.action);
        if (def == nullptr || ! def->ledState)
            continue;

        const bool on = def->ledState();
        const auto key = entry.control.describe();
        const auto it = ledCache.find (key);
        if (! force && it != ledCache.end() && it->second == on)
            continue;

        ledCache[key] = on;
        ledSink (juce::MidiMessage::noteOn (entry.control.channel + 1, entry.control.number, (juce::uint8) (on ? 127 : 0)));
    }
}

void ControllerManager::flashAllLeds()
{
    if (! ledSink)
        return;
    for (const auto& entry : mapping.getEntries())
        if (entry.led && entry.control.isNote)
            ledSink (juce::MidiMessage::noteOn (entry.control.channel + 1, entry.control.number, (juce::uint8) 127));

    juce::Timer::callAfterDelay (700, [weak = juce::WeakReference<ControllerManager> (this)]
                                 {
                                     if (auto* self = weak.get())
                                         self->refreshLeds (true);
                                 });
}

} // namespace pathboard
