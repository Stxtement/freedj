#pragma once

#include "Library/TrackRecord.h"
#include <juce_graphics/juce_graphics.h>

namespace pathboard
{

namespace TrackMetadata
{
    bool isSupportedAudioFile (const juce::File& file);
    const juce::StringArray& getSupportedExtensions();

    // Reads tags and audio properties. Never throws; returns false if the file cannot be parsed.
    // Fills id-independent fields; file size/modified/firstSeen are set from the file system.
    bool read (const juce::File& file, TrackRecord& out);

    // Embedded cover art (front cover preferred); invalid image if there is none.
    juce::Image readCoverArt (const juce::File& file);

    // Writes the editable text fields (title, artist, album, genre, comment, year, bpm, key) back into the file.
    bool writeTags (const juce::File& file, const TrackRecord& track);

    // "Song (Club Remix)" -> title "Song", remix "Club Remix". Leaves other brackets alone.
    void splitRemix (juce::String& title, juce::String& remix);
}

} // namespace pathboard
