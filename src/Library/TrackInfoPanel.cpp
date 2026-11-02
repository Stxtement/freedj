#include "TrackInfoPanel.h"
#include "Library/TrackMetadata.h"
#include "UI/DJLookAndFeel.h"

namespace pathboard
{

//==============================================================================
void RatingStars::drawStars (juce::Graphics& g, juce::Rectangle<float> area, int rating, juce::Colour filled, juce::Colour empty)
{
    const float size = juce::jmin (area.getHeight(), area.getWidth() / 5.0f);
    for (int i = 0; i < 5; ++i)
    {
        juce::Path star;
        star.addStar ({ area.getX() + size * ((float) i + 0.5f), area.getCentreY() }, 5, size * 0.22f, size * 0.46f);
        g.setColour (i < rating ? filled : empty);
        g.fillPath (star);
    }
}

void RatingStars::paint (juce::Graphics& g)
{
    drawStars (g, getLocalBounds().toFloat(), rating, Colours::cue, Colours::outline);
}

void RatingStars::mouseDown (const juce::MouseEvent& e)
{
    const float size = juce::jmin ((float) getHeight(), (float) getWidth() / 5.0f);
    int clicked = juce::jlimit (0, 5, (int) std::floor (e.position.x / size) + 1);
    if (clicked == rating)
        clicked = 0; // click the current rating again to clear it
    rating = clicked;
    repaint();
    if (onChange)
        onChange (rating);
}

//==============================================================================
struct TrackInfoPanel::CoverJob : juce::ThreadPoolJob
{
    CoverJob (TrackInfoPanel& o, juce::String p) : ThreadPoolJob ("cover"), owner (&o), path (std::move (p)) {}

    JobStatus runJob() override
    {
        auto image = TrackMetadata::readCoverArt (juce::File (path));
        // Keep memory small: covers are shown in a small square.
        if (image.isValid() && juce::jmax (image.getWidth(), image.getHeight()) > 512)
            image = image.rescaled (512 * image.getWidth() / juce::jmax (image.getWidth(), image.getHeight()),
                                    512 * image.getHeight() / juce::jmax (image.getWidth(), image.getHeight()));

        juce::MessageManager::callAsync ([weak = owner, path = path, image]
                                         {
                                             if (weak != nullptr)
                                                 weak->setCover (path, image);
                                         });
        return jobHasFinished;
    }

    juce::WeakReference<TrackInfoPanel> owner;
    juce::String path;
};

//==============================================================================
TrackInfoPanel::TrackInfoPanel()
{
    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (8);
    addAndMakeVisible (viewport);

    title   = &addField ("Title");
    artist  = &addField ("Artist");
    remix   = &addField ("Remix");
    album   = &addField ("Album");
    year    = &addField ("Year");
    genre   = &addField ("Genre");
    bpm     = &addField ("BPM");
    key     = &addField ("Key");
    comment = &addField ("Comment");
    tags    = &addField ("Tags");

    year->setInputRestrictions (4, "0123456789");
    bpm->setInputRestrictions (7, "0123456789.");

    stars.onChange = [this] (int newRating) { if (onRatingChanged && current.id != 0) onRatingChanged (current.id, newRating); };
    content.addAndMakeVisible (stars);

    statsLabel.setFont (juce::FontOptions (12.0f));
    statsLabel.setColour (juce::Label::textColourId, Colours::textDim);
    statsLabel.setJustificationType (juce::Justification::topLeft);
    content.addAndMakeVisible (statsLabel);

    writeToFileToggle.setToggleState (false, juce::dontSendNotification);
    writeToFileToggle.setTooltip ("Also write the edited tags into the audio file itself (changes the file).");
    content.addAndMakeVisible (writeToFileToggle);

    saveButton.setEnabled (false);
    saveButton.onClick = [this] { save(); };
    content.addAndMakeVisible (saveButton);

    useDetectedGenreButton.setTooltip ("Copies the detected genre into the Genre field. Press Save to keep it as your own genre.");
    useDetectedGenreButton.onClick = [this]
    {
        genre->setText (current.genreAutoSub.isNotEmpty() && current.genreAutoSub != current.genreAutoMain ? current.genreAutoMain + " / " + current.genreAutoSub : current.genreAutoMain,
                        juce::sendNotification);
    };
    content.addAndMakeVisible (useDetectedGenreButton);

    setTrack (nullptr);
}

TrackInfoPanel::~TrackInfoPanel()
{
    coverPool.removeAllJobs (true, 5000);
}

juce::TextEditor& TrackInfoPanel::addField (const juce::String& captionText)
{
    auto f = std::make_unique<Field>();
    f->caption.setText (captionText + ":", juce::dontSendNotification);
    f->caption.setColour (juce::Label::textColourId, Colours::textDim);
    f->caption.setFont (juce::FontOptions (12.0f));
    f->editor.setColour (juce::TextEditor::backgroundColourId, Colours::panelRaised);
    f->editor.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    f->editor.setFont (juce::FontOptions (13.0f));
    f->editor.onTextChange = [this] { markDirty(); };
    f->editor.onReturnKey = [this] { save(); };
    content.addAndMakeVisible (f->caption);
    content.addAndMakeVisible (f->editor);
    auto& editor = f->editor;
    fields.push_back (std::move (f));
    return editor;
}

void TrackInfoPanel::markDirty()
{
    if (current.id != 0)
    {
        dirty = true;
        saveButton.setEnabled (true);
    }
}

void TrackInfoPanel::setTrack (const TrackRecord* track)
{
    const bool same = track != nullptr && track->id == current.id;
    current = track != nullptr ? *track : TrackRecord();
    dirty = false;
    saveButton.setEnabled (false);

    const bool has = track != nullptr;
    for (auto& f : fields)
        f->editor.setEnabled (has);
    stars.setEnabled (has);

    auto set = [] (juce::TextEditor* e, const juce::String& text) { e->setText (text, juce::dontSendNotification); };
    set (title, current.title);   set (artist, current.artist);  set (remix, current.remix);
    set (album, current.album);   set (genre, current.genre);    set (comment, current.comment);
    set (tags, current.tags);     set (key, current.key);
    genre->setTextToShowWhenEmpty (current.genreAutoMain.isNotEmpty()
                                       ? "detected: " + (current.genreAutoSub.isNotEmpty() ? current.genreAutoMain + " / " + current.genreAutoSub : current.genreAutoMain)
                                       : juce::String(),
                                   Colours::textDim);
    useDetectedGenreButton.setVisible (has && current.genre.isEmpty() && current.genreAutoMain.isNotEmpty());
    set (year, current.year > 0 ? juce::String (current.year) : juce::String());
    set (bpm, current.bpm > 0.0 ? juce::String (current.bpm, 2) : juce::String());
    stars.setRating (current.rating);

    juce::String stats;
    if (has)
    {
        auto date = [] (juce::int64 ms) { return ms > 0 ? juce::Time (ms).formatted ("%d.%m.%Y %H:%M") : juce::String ("never"); };
        const int secs = (int) current.lengthSeconds;
        stats << "Length: " << secs / 60 << ":" << juce::String (secs % 60).paddedLeft ('0', 2)
              << "    " << current.format << "  " << current.bitrate << " kbps\n"
              << "First seen: " << date (current.firstSeen) << "\n"
              << "Last played: " << date (current.lastPlayed) << "\n"
              << "Play count: " << current.playCount << "\n"
              << current.path;
        if (current.genreAutoMain.isNotEmpty())
        {
            stats << "\nGenre (detected): " << (current.genreAutoSub.isNotEmpty() ? current.genreAutoMain + " / " + current.genreAutoSub : current.genreAutoMain)
                  << ", " << juce::roundToInt (current.genreConfidence * 100.0) << " %"
                  << (current.genreConfidence < 0.25 ? " (too uncertain to be used)" : "");
            if (auto* list = juce::JSON::parse (current.genreAlternatives).getArray())
            {
                juce::StringArray names;
                for (const auto& a : *list)
                    names.add (a.getProperty ("style", {}).toString().fromLastOccurrenceOf ("---", false, false) + " "
                               + juce::String (juce::roundToInt ((double) a.getProperty ("p", 0.0) * 100.0)) + " %");
                if (! names.isEmpty())
                    stats << "\nAlso possible: " << names.joinIntoString (", ");
            }
        }
        else if (current.genreVersion < 0)
            stats << "\nGenre detection failed for this file";

        if (current.analysisVersion > 0)
        {
            stats << "\nAnalysis: BPM confidence " << juce::roundToInt (current.bpmConfidence * 100.0) << "%, key confidence "
                  << juce::roundToInt (current.keyConfidence * 100.0) << "%, energy " << juce::String (current.energy, 1)
                  << ", " << juce::Time (current.analysedAt).formatted ("%d.%m.%Y");
            if (current.variableTempo)
                stats << "\nTempo changes within the track";
        }
        else if (current.analysisVersion < 0)
            stats << "\nAnalysis failed for this file";
        else
            stats << "\nNot analysed yet";

        if (current.manualFlags != 0)
        {
            stats << "\nManual: ";
            if (current.isManual (TrackRecord::manualBpm))   stats << "BPM ";
            if (current.isManual (TrackRecord::manualKey))   stats << "key ";
            if (current.isManual (TrackRecord::manualGrid))  stats << "grid ";
            if (current.isManual (TrackRecord::manualGenre)) stats << "genre ";
        }
        if (current.missing)
            stats << "\nFILE IS MISSING";
    }
    statsLabel.setText (stats, juce::dontSendNotification);

    if (! same)
    {
        cover = {};
        coverPath.clear();
        if (has)
            loadCover (current);
    }
    repaint();
}

void TrackInfoPanel::loadCover (const TrackRecord& t)
{
    const auto it = coverCache.find (t.path);
    if (it != coverCache.end())
    {
        cover = it->second;
        coverPath = t.path;
        return;
    }

    if (! t.missing)
        coverPool.addJob (new CoverJob (*this, t.path), true);
}

void TrackInfoPanel::setCover (const juce::String& path, juce::Image image)
{
    if (coverCache.size() > 60)
        coverCache.clear();
    coverCache[path] = image;

    if (path == current.path)
    {
        cover = image;
        coverPath = path;
        repaint();
    }
}

TrackRecord TrackInfoPanel::collectEdits() const
{
    auto t = current;
    t.title = title->getText().trim();
    t.artist = artist->getText().trim();
    t.remix = remix->getText().trim();
    t.album = album->getText().trim();
    t.genre = genre->getText().trim();
    t.comment = comment->getText().trim();
    t.tags = tags->getText().trim();
    t.key = key->getText().trim();
    t.year = year->getText().getIntValue();
    t.bpm = juce::jlimit (0.0, 300.0, bpm->getText().getDoubleValue());
    t.rating = stars.getRating();
    return t;
}

void TrackInfoPanel::save()
{
    if (! dirty || current.id == 0 || ! onSave)
        return;
    onSave (collectEdits(), writeToFileToggle.getToggleState());
}

//==============================================================================
void TrackInfoPanel::paint (juce::Graphics& g)
{
    g.setColour (Colours::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (2.0f), 6.0f);
}

void TrackInfoPanel::paintContent (juce::Graphics& g, juce::Rectangle<int> bounds)
{
    const int size = getCoverSize (bounds.getWidth());
    const auto coverArea = juce::Rectangle<int> (size, size).withPosition ((bounds.getWidth() - size) / 2, 10);
    g.setColour (juce::Colours::black);
    g.fillRect (coverArea);

    if (cover.isValid())
    {
        g.drawImage (cover, coverArea.toFloat(), juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize);
    }
    else
    {
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (current.id == 0 ? "No track selected" : "No cover art", coverArea, juce::Justification::centred);
    }
}

void TrackInfoPanel::resized()
{
    viewport.setBounds (getLocalBounds().reduced (3));
    layoutContent (viewport.getMaximumVisibleWidth());
}

void TrackInfoPanel::layoutContent (int width)
{
    constexpr int statsHeight = 200;
    auto area = juce::Rectangle<int> (0, 0, width, 100000).reduced (10, 10);
    area.removeFromTop (getCoverSize (width) + 6);

    stars.setBounds (area.removeFromTop (22).removeFromLeft (130));
    area.removeFromTop (4);

    for (auto& f : fields)
    {
        auto row = area.removeFromTop (24);
        f->caption.setBounds (row.removeFromLeft (58));
        f->editor.setBounds (row.reduced (0, 1));
    }

    area.removeFromTop (4);
    auto buttons = area.removeFromTop (26);
    saveButton.setBounds (buttons.removeFromRight (70));
    writeToFileToggle.setBounds (buttons);
    area.removeFromTop (4);
    useDetectedGenreButton.setBounds (area.removeFromTop (24));
    area.removeFromTop (6);
    statsLabel.setBounds (area.removeFromTop (statsHeight));

    content.setSize (width, area.getY() + 10);
}

} // namespace pathboard
