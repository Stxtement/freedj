#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace pathboard
{

// How a MIDI control is read.
enum class ControlKind
{
    button,            // note on/off, or a CC where value > 0 means pressed
    absolute7,         // CC 0..127 -> 0..1
    absolute14,        // CC number (MSB) + number+32 (LSB) -> 0..1
    relativeCentered,  // CC, 64 = no movement, 65 = +1, 63 = -1 (jog wheels)
    relativeTwos       // CC, 1..63 = +n, 127..65 = -n (endless encoders such as the browse knob)
};

const char* controlKindName (ControlKind);

struct MidiControl
{
    bool isNote = true;     // false = control change
    int channel = 0;        // 0..15
    int number = 0;         // note or controller number

    bool operator== (const MidiControl& o) const { return isNote == o.isNote && channel == o.channel && number == o.number; }
    juce::String describe() const;
};

struct MappingEntry
{
    juce::String action;
    MidiControl control;
    ControlKind kind = ControlKind::button;
    bool invert = false;    // absolute: 1 - value; relative: reverse direction
    double scale = 1.0;     // relative: multiplier for the tick count
    bool led = false;       // light the button on the controller with the action's state (note controls only)
};

// Which MIDI message does what. Plain data, saved as JSON, editable in the application.
class ControllerMapping
{
public:
    // Jog wheel resolution: ticks for one full turn. Used by the application to turn ticks into time.
    double jogTicksPerRevolution = 1024.0;
    double bendSensitivity = 1.0;     // multiplier for the outer ring / nudge
    juce::String preferredDevice = "DDJ-FLX4";

    const std::vector<MappingEntry>& getEntries() const { return entries; }
    const MappingEntry* findByAction (const juce::String& action) const;
    // For incoming messages: the entry whose control matches (for absolute14 the LSB number also matches).
    const MappingEntry* findByControl (const MidiControl& control) const;
    const MappingEntry* findByLsb (const MidiControl& lsbControl) const;

    // Replaces whatever the action or the control was assigned to before.
    void set (const MappingEntry& entry);
    void clearAction (const juce::String& action);
    void clear() { entries.clear(); }

    juce::String toJson() const;
    juce::Result fromJson (const juce::String& json);
    bool saveTo (const juce::File& file) const;
    juce::Result loadFrom (const juce::File& file);

    // A first guess for the Pioneer DDJ-FLX4 (based on the message layout of the DDJ-400 family).
    // It has not been verified against the hardware: use the learn function to correct it.
    static ControllerMapping createDefaultFlx4();

private:
    std::vector<MappingEntry> entries;
};

} // namespace pathboard
