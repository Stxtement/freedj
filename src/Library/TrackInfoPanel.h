#pragma once

#include "Library/TrackRecord.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <map>

namespace pathboard
{

class RatingStars : public juce::Component
{
public:
    void setRating (int r) { rating = r; repaint(); }
    int getRating() const { return rating; }
    std::function<void (int)> onChange;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

    static void drawStars (juce::Graphics&, juce::Rectangle<float> area, int rating, juce::Colour filled, juce::Colour empty);

private:
    int rating = 0;
};

// Right-hand details pane: cover art, editable tag fields, statistics.
class TrackInfoPanel : public juce::Component
{
public:
    TrackInfoPanel();
    ~TrackInfoPanel() override;

    void setTrack (const TrackRecord* track); // nullptr clears
    juce::int64 getTrackId() const { return current.id; }
    bool isDirty() const { return dirty; }

    // Called when the user presses Save. `writeToFile` mirrors the "write tags to file" toggle.
    std::function<void (const TrackRecord& edited, bool writeToFile)> onSave;
    std::function<void (juce::int64 id, int rating)> onRatingChanged;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // Everything lives in a scrollable content component so nothing is unreachable in a small window.
    struct Content : juce::Component
    {
        explicit Content (TrackInfoPanel& o) : owner (o) {}
        void paint (juce::Graphics& g) override { owner.paintContent (g, getLocalBounds()); }
        TrackInfoPanel& owner;
    };

    void paintContent (juce::Graphics&, juce::Rectangle<int> bounds);
    void layoutContent (int width);
    int getCoverSize (int width) const { return juce::jlimit (60, 200, width - 20); }

    struct CoverJob;
    void loadCover (const TrackRecord&);
    void setCover (const juce::String& path, juce::Image image);
    void markDirty();
    void save();
    TrackRecord collectEdits() const;
    juce::TextEditor& addField (const juce::String& caption);

    TrackRecord current;
    bool dirty = false;
    juce::Image cover;
    juce::String coverPath;
    std::map<juce::String, juce::Image> coverCache;
    juce::ThreadPool coverPool { 1 };

    struct Field { juce::Label caption; juce::TextEditor editor; };
    std::vector<std::unique_ptr<Field>> fields;
    juce::TextEditor *title = nullptr, *artist = nullptr, *remix = nullptr, *album = nullptr, *year = nullptr,
                     *genre = nullptr, *bpm = nullptr, *key = nullptr, *comment = nullptr, *tags = nullptr;

    Content content { *this };
    juce::Viewport viewport;
    RatingStars stars;
    juce::Label statsLabel;
    juce::ToggleButton writeToFileToggle { "Write tags into the file" };
    juce::TextButton saveButton { "Save" };
    juce::TextButton useDetectedGenreButton { "Use detected genre" };

    JUCE_DECLARE_WEAK_REFERENCEABLE (TrackInfoPanel)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackInfoPanel)
};

} // namespace pathboard
