#pragma once

#include <juce_core/juce_core.h>
#include <functional>
#include <vector>

namespace pathboard
{

// What a controller element delivers to an action.
struct ControlEvent
{
    double value = 0.0;       // absolute: 0..1 (already inverted if the mapping says so); relative: signed ticks * scale
    bool pressed = false;     // buttons: true on press, false on release
};

// Every function the user can put on a hardware control. The application registers its actions once;
// the mapping only refers to them by id, so mappings are plain data and work for any controller.
struct ActionDef
{
    enum class Type { button, absolute, relative };

    juce::String id;          // e.g. "deck1.play"
    juce::String group;       // "Deck 1", "Mixer", "Browser"
    juce::String label;       // "Play / Pause"
    Type type = Type::button;
    std::function<void (const ControlEvent&)> handler;
    std::function<bool()> ledState;     // optional: lets the controller light the button
};

class ActionRegistry
{
public:
    void add (ActionDef def) { actions.push_back (std::move (def)); }

    const ActionDef* find (const juce::String& id) const
    {
        for (const auto& a : actions)
            if (a.id == id)
                return &a;
        return nullptr;
    }

    const std::vector<ActionDef>& getAll() const { return actions; }

private:
    std::vector<ActionDef> actions;
};

} // namespace pathboard
