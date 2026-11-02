// Controller layer tests: mapping data, message decoding (buttons, 7/14-bit, relative), learn, LEDs, threading.

#include "TestSupport.h"
#include "Controller/ControllerManager.h"
#include <juce_events/juce_events.h>

using namespace pathboard;
using testsupport::check;

namespace
{
struct Recorder
{
    std::vector<std::pair<juce::String, ControlEvent>> events;
    int count (const juce::String& id) const
    {
        int n = 0;
        for (const auto& e : events) if (e.first == id) ++n;
        return n;
    }
    ControlEvent last (const juce::String& id) const
    {
        for (auto it = events.rbegin(); it != events.rend(); ++it) if (it->first == id) return it->second;
        return {};
    }
};

void addAction (ActionRegistry& reg, Recorder& rec, const juce::String& id, ActionDef::Type type, std::function<bool()> led = {})
{
    ActionDef def;
    def.id = id;
    def.label = id;
    def.type = type;
    def.handler = [&rec, id] (const ControlEvent& e) { rec.events.push_back ({ id, e }); };
    def.ledState = std::move (led);
    reg.add (std::move (def));
}

void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }
}

int runControllerTests()
{
    //==========================================================================
    std::cout << "Mapping data" << std::endl;
    {
        const auto mapping = ControllerMapping::createDefaultFlx4();
        check (mapping.getEntries().size() >= 70, "the default FLX4 mapping covers decks, pads, mixer and browser", juce::String ((int) mapping.getEntries().size()) + " controls");

        bool unique = true;
        for (size_t i = 0; i < mapping.getEntries().size(); ++i)
            for (size_t j = i + 1; j < mapping.getEntries().size(); ++j)
                if (mapping.getEntries()[i].control == mapping.getEntries()[j].control || mapping.getEntries()[i].action == mapping.getEntries()[j].action)
                    unique = false;
        check (unique, "no control or action is assigned twice");

        const auto json = mapping.toJson();
        ControllerMapping loaded;
        check (loaded.fromJson (json).wasOk() && loaded.getEntries().size() == mapping.getEntries().size(), "mapping survives a JSON round trip");
        const auto* a = loaded.findByAction ("deck2.eq.high");
        const auto* b = mapping.findByAction ("deck2.eq.high");
        check (a != nullptr && b != nullptr && a->control == b->control && a->kind == b->kind, "entries keep control and kind");

        check (ControllerMapping().fromJson ("this is not json").failed(), "garbage is rejected");
        check (ControllerMapping().fromJson ("{\"entries\": 5}").failed(), "a file without entries is rejected");

        const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("pdj-mapping-test/mapping.json");
        check (mapping.saveTo (file) && ControllerMapping().loadFrom (file).wasOk(), "mapping is saved to and loaded from a file");
        file.getParentDirectory().deleteRecursively();

        ControllerMapping edited = mapping;
        MappingEntry e;
        e.action = "deck1.play";
        e.control = { true, 0, 0x0C }; // the cue button's control
        edited.set (e);
        check (edited.findByAction ("deck1.cue") == nullptr && edited.findByControl ({ true, 0, 0x0C })->action == "deck1.play",
               "assigning a control to an action takes it from the previous action");
    }

    //==========================================================================
    std::cout << "Message decoding" << std::endl;
    {
        Recorder rec;
        ActionRegistry reg;
        for (auto id : { "deck1.play", "deck1.cue", "deck1.hotcue.1" })
            addAction (reg, rec, id, ActionDef::Type::button);
        for (auto id : { "deck1.fader", "deck1.eq.high", "mixer.crossfader", "deck1.tempo" })
            addAction (reg, rec, id, ActionDef::Type::absolute);
        for (auto id : { "deck1.jog.turn", "browser.scroll" })
            addAction (reg, rec, id, ActionDef::Type::relative);

        auto mapping = ControllerMapping::createDefaultFlx4();
        ControllerManager manager (reg, mapping);

        // Buttons: press and release are reported separately (CUE needs both).
        manager.processMessage (juce::MidiMessage::noteOn (1, 0x0C, (juce::uint8) 127));
        manager.processMessage (juce::MidiMessage::noteOff (1, 0x0C));
        check (rec.count ("deck1.cue") == 2 && ! rec.last ("deck1.cue").pressed, "button: press and release arrive");
        manager.processMessage (juce::MidiMessage::noteOn (1, 0x0B, (juce::uint8) 0)); // note-on with velocity 0 is a release
        check (! rec.last ("deck1.play").pressed, "note-on with velocity 0 counts as release");
        manager.processMessage (juce::MidiMessage::noteOn (8, 0x00, (juce::uint8) 100));
        check (rec.count ("deck1.hotcue.1") == 1 && rec.last ("deck1.hotcue.1").pressed, "pad on channel 8 maps to hot cue 1");

        // 14-bit fader: MSB then LSB.
        manager.processMessage (juce::MidiMessage::controllerEvent (1, 0x13, 127));
        manager.processMessage (juce::MidiMessage::controllerEvent (1, 0x13 + 32, 127));
        check (std::abs (rec.last ("deck1.fader").value - 1.0) < 1.0e-9, "14-bit: full travel is exactly 1.0");
        manager.processMessage (juce::MidiMessage::controllerEvent (1, 0x13, 0));
        manager.processMessage (juce::MidiMessage::controllerEvent (1, 0x13 + 32, 0));
        check (rec.last ("deck1.fader").value == 0.0, "14-bit: bottom is exactly 0.0");
        manager.processMessage (juce::MidiMessage::controllerEvent (1, 0x13, 64));
        manager.processMessage (juce::MidiMessage::controllerEvent (1, 0x13 + 32, 0));
        check (std::abs (rec.last ("deck1.fader").value - 8192.0 / 16383.0) < 1.0e-9, "14-bit: MSB 64 / LSB 0 is the middle");
        manager.processMessage (juce::MidiMessage::controllerEvent (1, 0x13, 10));
        manager.processMessage (juce::MidiMessage::controllerEvent (1, 0x13 + 32, 100));
        check (std::abs (rec.last ("deck1.fader").value - (10.0 * 128 + 100) / 16383.0) < 1.0e-9, "14-bit: fine resolution is used");

        // Mixer controls arrive on channel 7.
        manager.processMessage (juce::MidiMessage::controllerEvent (7, 0x1F, 127));
        manager.processMessage (juce::MidiMessage::controllerEvent (7, 0x1F + 32, 127));
        check (rec.count ("mixer.crossfader") == 2 && rec.last ("mixer.crossfader").value > 0.999, "crossfader on the mixer channel");

        // Relative encoders.
        manager.processMessage (juce::MidiMessage::controllerEvent (1, 0x22, 64 + 3));
        check (rec.last ("deck1.jog.turn").value == 3.0, "jog: 64 + 3 = three ticks forward");
        manager.processMessage (juce::MidiMessage::controllerEvent (1, 0x22, 64 - 2));
        check (rec.last ("deck1.jog.turn").value == -2.0, "jog: 64 - 2 = two ticks back");
        const int before = rec.count ("deck1.jog.turn");
        manager.processMessage (juce::MidiMessage::controllerEvent (1, 0x22, 64));
        check (rec.count ("deck1.jog.turn") == before, "jog: 64 means no movement and is ignored");
        manager.processMessage (juce::MidiMessage::controllerEvent (7, 0x40, 1));
        check (rec.last ("browser.scroll").value == 1.0, "browse knob: 1 = one step down");
        manager.processMessage (juce::MidiMessage::controllerEvent (7, 0x40, 127));
        check (rec.last ("browser.scroll").value == -1.0, "browse knob: 127 = one step up");
        manager.processMessage (juce::MidiMessage::controllerEvent (7, 0x40, 126));
        check (rec.last ("browser.scroll").value == -2.0, "browse knob: 126 = two steps up");

        // Mapping options.
        MappingEntry inverted = *mapping.findByAction ("deck1.eq.high");
        inverted.invert = true;
        mapping.set (inverted);
        manager.processMessage (juce::MidiMessage::controllerEvent (1, 0x07, 127));
        manager.processMessage (juce::MidiMessage::controllerEvent (1, 0x07 + 32, 127));
        check (rec.last ("deck1.eq.high").value < 1.0e-9, "invert flips an absolute control");
        MappingEntry jog = *mapping.findByAction ("deck1.jog.turn");
        jog.scale = 0.5;
        jog.invert = true;
        mapping.set (jog);
        manager.processMessage (juce::MidiMessage::controllerEvent (1, 0x22, 64 + 4));
        check (rec.last ("deck1.jog.turn").value == -2.0, "relative scale and invert are applied");

        // Unknown and unsupported messages do nothing.
        const int total = (int) rec.events.size();
        manager.processMessage (juce::MidiMessage::noteOn (3, 99, (juce::uint8) 100));
        manager.processMessage (juce::MidiMessage::pitchWheel (1, 8192));
        manager.processMessage (juce::MidiMessage::programChange (1, 5));
        check ((int) rec.events.size() == total, "unmapped and unsupported messages are ignored");
    }

    //==========================================================================
    std::cout << "MIDI learn" << std::endl;
    {
        Recorder rec;
        ActionRegistry reg;
        addAction (reg, rec, "deck1.play", ActionDef::Type::button, [] { return false; });
        addAction (reg, rec, "deck1.cue", ActionDef::Type::button);
        addAction (reg, rec, "mixer.crossfader", ActionDef::Type::absolute);
        addAction (reg, rec, "deck1.jog.turn", ActionDef::Type::relative);
        ControllerMapping mapping;
        ControllerManager manager (reg, mapping);

        juce::String learned;
        int mappingNotifications = 0;
        manager.onLearned = [&] (const juce::String& id) { learned = id; };
        manager.onMappingChanged = [&] { ++mappingNotifications; };

        manager.startLearn ("deck1.play");
        check (manager.isLearning(), "learn mode is active");
        manager.processMessage (juce::MidiMessage::noteOn (2, 0x20, (juce::uint8) 100));
        check (! manager.isLearning() && learned == "deck1.play" && rec.events.empty(), "the first press is assigned, not executed");
        const auto* entry = mapping.findByAction ("deck1.play");
        check (entry != nullptr && entry->control == MidiControl { true, 1, 0x20 } && entry->kind == ControlKind::button && entry->led,
               "button learned: note, channel and LED flag", entry != nullptr ? entry->control.describe() : juce::String());
        check (mappingNotifications == 1, "the application is told to save the mapping");

        manager.processMessage (juce::MidiMessage::noteOn (2, 0x20, (juce::uint8) 100));
        check (rec.count ("deck1.play") == 1, "the learned control works immediately");

        manager.startLearn ("mixer.crossfader");
        manager.processMessage (juce::MidiMessage::noteOff (2, 0x20));
        check (manager.isLearning(), "a button release does not end learning");
        manager.processMessage (juce::MidiMessage::controllerEvent (7, 0x1F, 40));
        entry = mapping.findByAction ("mixer.crossfader");
        check (! manager.isLearning() && entry != nullptr && entry->kind == ControlKind::absolute14 && entry->control == MidiControl { false, 6, 0x1F },
               "a low controller number is learned as a 14-bit control");

        // The LSB that follows the MSB is handled as part of that control, not as something new.
        manager.processMessage (juce::MidiMessage::controllerEvent (7, 0x1F + 32, 99));
        check (rec.count ("mixer.crossfader") == 1 && mapping.findByControl ({ false, 6, 0x1F + 32 }) == nullptr,
               "the LSB after the MSB belongs to the learned control");

        manager.startLearn ("deck1.jog.turn");
        manager.processMessage (juce::MidiMessage::controllerEvent (1, 0x22, 66));
        const auto* jogEntry = mapping.findByAction ("deck1.jog.turn");
        check (jogEntry != nullptr && jogEntry->kind == ControlKind::relativeCentered && jogEntry->control == MidiControl { false, 0, 0x22 },
               "a relative action is learned as a centred encoder (also on CC 0x22, which is in the LSB number range)");

        // Taking over a control from another action.
        manager.startLearn ("deck1.cue");
        manager.processMessage (juce::MidiMessage::noteOn (2, 0x20, (juce::uint8) 100));
        check (mapping.findByAction ("deck1.play") == nullptr && mapping.findByAction ("deck1.cue") != nullptr,
               "learning a control that another action uses moves it");

        manager.startLearn ("deck1.cue");
        manager.cancelLearn();
        manager.processMessage (juce::MidiMessage::noteOn (2, 0x55, (juce::uint8) 100));
        check (mapping.findByControl ({ true, 1, 0x55 }) == nullptr, "cancelled learning assigns nothing");
    }

    //==========================================================================
    std::cout << "LEDs" << std::endl;
    {
        Recorder rec;
        ActionRegistry reg;
        bool playing = false;
        addAction (reg, rec, "deck1.play", ActionDef::Type::button, [&] { return playing; });
        auto mapping = ControllerMapping::createDefaultFlx4();
        ControllerManager manager (reg, mapping);

        std::vector<juce::MidiMessage> sent;
        manager.ledSink = [&] (const juce::MidiMessage& m) { sent.push_back (m); };

        manager.refreshLeds (true);
        check (sent.size() == 1 && sent[0].getRawDataSize() == 3 && sent[0].getRawData()[0] == 0x90 && sent[0].getRawData()[1] == 0x0B && sent[0].getRawData()[2] == 0,
               "the play LED is switched off on connect (note 0x0B on channel 1, velocity 0)", sent.empty() ? juce::String() : sent[0].getDescription());
        sent.clear();
        manager.refreshLeds();
        check (sent.empty(), "nothing is sent while the state does not change");
        playing = true;
        manager.refreshLeds();
        check (sent.size() == 1 && sent[0].isNoteOn() && sent[0].getVelocity() == 127, "the LED lights when the deck plays");
        sent.clear();
        manager.refreshLeds (true);
        check (sent.size() == 1, "a forced refresh re-sends the state");
    }

    //==========================================================================
    std::cout << "MIDI thread to message thread" << std::endl;
    {
        Recorder rec;
        ActionRegistry reg;
        addAction (reg, rec, "deck1.jog.turn", ActionDef::Type::relative);
        auto mapping = ControllerMapping::createDefaultFlx4();
        ControllerManager manager (reg, mapping);

        const int messages = 5000;
        std::thread midiThread ([&]
        {
            for (int i = 0; i < messages; ++i)
                manager.handleIncomingMidiMessage (nullptr, juce::MidiMessage::controllerEvent (1, 0x22, 65));
        });
        midiThread.join();
        for (int i = 0; i < 100 && (int) rec.events.size() < messages; ++i)
            pump (20);
        check ((int) rec.events.size() == messages, "messages from the MIDI driver thread are all handled on the message thread",
               juce::String ((int) rec.events.size()) + " of " + juce::String (messages));
    }

    return 0;
}
