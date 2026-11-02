#include "TrackMetadata.h"
#include "TrackTidy.h"

#include <fileref.h>
#include <tag.h>
#include <tpropertymap.h>
#include <audioproperties.h>
#include <tvariant.h>

namespace pathboard::TrackMetadata
{

namespace
{
    juce::String toJuce (const TagLib::String& s)
    {
        return juce::String::fromUTF8 (s.toCString (true));
    }

    TagLib::String toTagLib (const juce::String& s)
    {
        return TagLib::String (s.toRawUTF8(), TagLib::String::UTF8);
    }

    juce::String firstProperty (const TagLib::PropertyMap& map, const char* key)
    {
        const auto it = map.find (key);
        if (it == map.end() || it->second.isEmpty())
            return {};
        return toJuce (it->second.front());
    }

    bool looksLikeRemixTag (const juce::String& inside)
    {
        for (auto word : { "remix", "mix", "edit", "bootleg", "rework", "vip", "version", "flip", "mashup" })
            if (inside.containsIgnoreCase (word))
                return true;
        return false;
    }
}

const juce::StringArray& getSupportedExtensions()
{
    static const juce::StringArray exts { ".mp3", ".wav", ".flac", ".aif", ".aiff", ".ogg", ".m4a",
                                          ".aac", ".wma", ".mp4", ".opus" };
    return exts;
}

bool isSupportedAudioFile (const juce::File& file)
{
    return getSupportedExtensions().contains (file.getFileExtension().toLowerCase());
}

void splitRemix (juce::String& title, juce::String& remix)
{
    const auto trimmed = title.trim();
    if (trimmed.isEmpty())
        return;

    const auto closeChar = trimmed.getLastCharacter();
    if (closeChar != ')' && closeChar != ']')
        return;

    const auto openChar = closeChar == ')' ? '(' : '[';
    const int open = trimmed.lastIndexOfChar (openChar);
    if (open <= 0)
        return;

    const auto inside = trimmed.substring (open + 1, trimmed.length() - 1).trim();
    if (inside.isNotEmpty() && looksLikeRemixTag (inside))
    {
        remix = inside;
        title = trimmed.substring (0, open).trim();
    }
}

bool read (const juce::File& file, TrackRecord& out)
{
    try
    {
        TagLib::FileRef ref (file.getFullPathName().toWideCharPointer(), true, TagLib::AudioProperties::Fast);

        out.path = file.getFullPathName();
        out.format = file.getFileExtension().trimCharactersAtStart (".").toUpperCase();
        out.fileSize = file.getSize();
        out.modified = file.getLastModificationTime().toMilliseconds();

        if (ref.isNull())
            return false;

        if (auto* props = ref.audioProperties())
        {
            out.lengthSeconds = (double) props->lengthInMilliseconds() / 1000.0;
            out.bitrate = props->bitrate();
        }

        if (auto* tag = ref.tag())
        {
            out.title = toJuce (tag->title()).trim();
            out.artist = toJuce (tag->artist()).trim();
            out.album = toJuce (tag->album()).trim();
            out.genre = toJuce (tag->genre()).trim();
            out.comment = toJuce (tag->comment()).trim();
            out.year = (int) tag->year();
        }

        const auto map = ref.properties();
        out.bpm = firstProperty (map, "BPM").getDoubleValue();
        out.key = firstProperty (map, "INITIALKEY").trim();

        // Fallbacks for untagged files: "Artist - Title" in the file name.
        if (out.title.isEmpty())
        {
            const auto name = file.getFileNameWithoutExtension();
            const int dash = name.indexOf (" - ");
            if (out.artist.isEmpty() && dash > 0)
            {
                out.artist = name.substring (0, dash).trim();
                out.title = name.substring (dash + 3).trim();
            }
            else
            {
                out.title = name;
            }
        }

        tidyTrackText (out);   // pool / site clutter out of title and artist, remix split off (see TrackTidy.h)
        return out.lengthSeconds > 0.0;
    }
    catch (...)
    {
        return false;
    }
}

juce::Image readCoverArt (const juce::File& file)
{
    try
    {
        TagLib::FileRef ref (file.getFullPathName().toWideCharPointer(), false);
        if (ref.isNull())
            return {};

        TagLib::ByteVector chosen;

        for (const auto& picture : ref.complexProperties ("PICTURE"))
        {
            const auto data = picture.value ("data", TagLib::Variant()).toByteVector();
            if (data.isEmpty())
                continue;

            if (chosen.isEmpty())
                chosen = data;

            if (picture.value ("pictureType", TagLib::Variant()).toString() == "Front Cover")
            {
                chosen = data;
                break;
            }
        }

        if (chosen.isEmpty())
            return {};

        return juce::ImageFileFormat::loadFrom (chosen.data(), chosen.size());
    }
    catch (...)
    {
        return {};
    }
}

bool writeTags (const juce::File& file, const TrackRecord& track)
{
    try
    {
        TagLib::FileRef ref (file.getFullPathName().toWideCharPointer(), false);
        if (ref.isNull() || ref.tag() == nullptr)
            return false;

        auto* tag = ref.tag();
        const auto fullTitle = track.remix.isNotEmpty() ? track.title + " (" + track.remix + ")" : track.title;
        tag->setTitle (toTagLib (fullTitle));
        tag->setArtist (toTagLib (track.artist));
        tag->setAlbum (toTagLib (track.album));
        tag->setGenre (toTagLib (track.genre));
        tag->setComment (toTagLib (track.comment));
        tag->setYear ((unsigned int) juce::jmax (0, track.year));

        auto map = ref.properties();
        if (track.bpm > 0.0)
            map.replace ("BPM", TagLib::StringList (toTagLib (juce::String (juce::roundToInt (track.bpm)))));
        if (track.key.isNotEmpty())
            map.replace ("INITIALKEY", TagLib::StringList (toTagLib (track.key)));
        ref.setProperties (map);

        return ref.save();
    }
    catch (...)
    {
        return false;
    }
}

} // namespace pathboard::TrackMetadata
