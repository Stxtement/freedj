#include "ControllerMapping.h"

namespace pathboard
{

const char* controlKindName (ControlKind k)
{
    switch (k)
    {
        case ControlKind::button:           return "button";
        case ControlKind::absolute7:        return "absolute7";
        case ControlKind::absolute14:       return "absolute14";
        case ControlKind::relativeCentered: return "relativeCentered";
        case ControlKind::relativeTwos:     return "relativeTwos";
    }
    return "button";
}

static ControlKind kindFromName (const juce::String& s)
{
    for (auto k : { ControlKind::button, ControlKind::absolute7, ControlKind::absolute14, ControlKind::relativeCentered, ControlKind::relativeTwos })
        if (s == controlKindName (k))
            return k;
    return ControlKind::button;
}

juce::String MidiControl::describe() const
{
    return juce::String (isNote ? "Note " : "CC ") + "0x" + juce::String::toHexString (number).toUpperCase().paddedLeft ('0', 2)
           + "  ch " + juce::String (channel + 1);
}

//==============================================================================
const MappingEntry* ControllerMapping::findByAction (const juce::String& action) const
{
    for (const auto& e : entries)
        if (e.action == action)
            return &e;
    return nullptr;
}

const MappingEntry* ControllerMapping::findByControl (const MidiControl& c) const
{
    for (const auto& e : entries)
        if (e.control == c)
            return &e;
    return nullptr;
}

const MappingEntry* ControllerMapping::findByLsb (const MidiControl& c) const
{
    if (c.isNote || c.number < 32 || c.number >= 64)
        return nullptr;
    const MidiControl msb { false, c.channel, c.number - 32 };
    const auto* e = findByControl (msb);
    return e != nullptr && e->kind == ControlKind::absolute14 ? e : nullptr;
}

void ControllerMapping::set (const MappingEntry& entry)
{
    entries.erase (std::remove_if (entries.begin(), entries.end(), [&] (const MappingEntry& e)
                                   { return e.action == entry.action || e.control == entry.control; }),
                   entries.end());
    entries.push_back (entry);
}

void ControllerMapping::clearAction (const juce::String& action)
{
    entries.erase (std::remove_if (entries.begin(), entries.end(), [&] (const MappingEntry& e) { return e.action == action; }), entries.end());
}

//==============================================================================
juce::String ControllerMapping::toJson() const
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("version", 1);
    root->setProperty ("device", preferredDevice);
    root->setProperty ("jogTicksPerRevolution", jogTicksPerRevolution);
    root->setProperty ("bendSensitivity", bendSensitivity);

    juce::Array<juce::var> list;
    for (const auto& e : entries)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("action", e.action);
        o->setProperty ("type", e.control.isNote ? "note" : "cc");
        o->setProperty ("channel", e.control.channel + 1);
        o->setProperty ("number", e.control.number);
        o->setProperty ("kind", controlKindName (e.kind));
        o->setProperty ("invert", e.invert);
        o->setProperty ("scale", e.scale);
        o->setProperty ("led", e.led);
        list.add (juce::var (o));
    }
    root->setProperty ("entries", list);
    return juce::JSON::toString (juce::var (root), false);
}

juce::Result ControllerMapping::fromJson (const juce::String& json)
{
    const auto parsed = juce::JSON::parse (json);
    if (! parsed.isObject())
        return juce::Result::fail ("not a valid mapping file");

    ControllerMapping fresh;
    fresh.preferredDevice = parsed.getProperty ("device", "DDJ-FLX4").toString();
    fresh.jogTicksPerRevolution = juce::jlimit (16.0, 100000.0, (double) parsed.getProperty ("jogTicksPerRevolution", 1024.0));
    fresh.bendSensitivity = juce::jlimit (0.05, 20.0, (double) parsed.getProperty ("bendSensitivity", 1.0));

    auto* list = parsed.getProperty ("entries", {}).getArray();
    if (list == nullptr)
        return juce::Result::fail ("the mapping file has no entries");

    for (const auto& v : *list)
    {
        MappingEntry e;
        e.action = v.getProperty ("action", {}).toString();
        e.control.isNote = v.getProperty ("type", "note").toString() == "note";
        e.control.channel = juce::jlimit (1, 16, (int) v.getProperty ("channel", 1)) - 1;
        e.control.number = juce::jlimit (0, 127, (int) v.getProperty ("number", 0));
        e.kind = kindFromName (v.getProperty ("kind", "button").toString());
        e.invert = (bool) v.getProperty ("invert", false);
        e.scale = (double) v.getProperty ("scale", 1.0);
        e.led = (bool) v.getProperty ("led", false);
        if (e.action.isNotEmpty())
            fresh.set (e);
    }

    *this = std::move (fresh);
    return juce::Result::ok();
}

bool ControllerMapping::saveTo (const juce::File& file) const
{
    file.getParentDirectory().createDirectory();
    const auto temp = file.withFileExtension (".tmp");
    return temp.replaceWithText (toJson()) && temp.moveFileTo (file);
}

juce::Result ControllerMapping::loadFrom (const juce::File& file)
{
    if (! file.existsAsFile())
        return juce::Result::fail ("file not found");
    return fromJson (file.loadFileAsString());
}

//==============================================================================
ControllerMapping ControllerMapping::createDefaultFlx4()
{
    ControllerMapping m;

    auto add = [&m] (const juce::String& action, bool note, int channel, int number, ControlKind kind, bool led = false)
    {
        MappingEntry e;
        e.action = action;
        e.control = { note, channel, number };
        e.kind = kind;
        e.led = led;
        m.set (e);
    };

    for (int d = 0; d < 2; ++d)
    {
        const juce::String p = "deck" + juce::String (d + 1) + ".";
        const int ch = d;            // deck section: channel 1 / 2
        const int padCh = 7 + d;     // performance pads: channel 8 / 9
        const int mixCh = 6;         // mixer and browser section: channel 7

        add (p + "play",        true, ch, 0x0B, ControlKind::button, true);
        add (p + "cue",         true, ch, 0x0C, ControlKind::button, true);
        add (p + "sync",        true, ch, 0x58, ControlKind::button, true);
        add (p + "loop.4",      true, ch, 0x10, ControlKind::button, true);
        add (p + "loop.exit",   true, ch, 0x11, ControlKind::button);
        add (p + "jog.turn",    false, ch, 0x22, ControlKind::relativeCentered);
        add (p + "jog.ring",    false, ch, 0x21, ControlKind::relativeCentered);
        add (p + "jog.touch",   true, ch, 0x36, ControlKind::button);
        add (p + "tempo",       false, ch, 0x00, ControlKind::absolute14);
        add (p + "trim",        false, ch, 0x04, ControlKind::absolute14);
        add (p + "eq.high",     false, ch, 0x07, ControlKind::absolute14);
        add (p + "eq.mid",      false, ch, 0x0B, ControlKind::absolute14);
        add (p + "eq.low",      false, ch, 0x0F, ControlKind::absolute14);
        add (p + "fader",       false, ch, 0x13, ControlKind::absolute14);
        add (p + "filter",      false, mixCh, 0x17 + d, ControlKind::absolute14);
        add (p + "headphoneCue", true, ch, 0x54, ControlKind::button, true);

        for (int i = 0; i < 4; ++i)
        {
            add (p + "hotcue." + juce::String (i + 1),       true, padCh, 0x00 + i, ControlKind::button, true);
            add (p + "hotcueDelete." + juce::String (i + 1), true, padCh, 0x08 + i, ControlKind::button);
        }
        static const char* loopPads[] = { "loop.1", "loop.2", "loop.4", "loop.8" };
        for (int i = 0; i < 4; ++i)
            add (p + loopPads[i] + ".pad", true, padCh, 0x10 + i, ControlKind::button, true);

        static const char* stemNames[] = { "stem.vocal", "stem.instru", "stem.bass", "stem.drums" };
        for (int i = 0; i < 4; ++i)
            add (p + stemNames[i], true, padCh, 0x30 + i, ControlKind::button, true);
    }

    add ("mixer.crossfader",      false, 6, 0x1F, ControlKind::absolute14);
    add ("mixer.master",          false, 6, 0x08, ControlKind::absolute14);
    add ("mixer.headphoneMix",    false, 6, 0x0C, ControlKind::absolute14);
    add ("mixer.headphoneVolume", false, 6, 0x0D, ControlKind::absolute14);
    add ("browser.scroll",        false, 6, 0x40, ControlKind::relativeTwos);
    add ("browser.press",         true,  6, 0x40, ControlKind::button);
    add ("browser.load.deck1",    true,  6, 0x46, ControlKind::button);
    add ("browser.load.deck2",    true,  6, 0x47, ControlKind::button);
    return m;
}

} // namespace pathboard
