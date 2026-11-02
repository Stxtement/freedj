#include "StemCache.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>

namespace pathboard::stems
{

namespace
{
    constexpr int metaVersion = 1;
    const char* stemFileName (int s) { static const char* n[] = { "drums.flac", "bass.flac", "other.flac", "vocals.flac" }; return n[s]; }
}

StemCache::StemCache (const juce::File& folder, juce::int64 maxBytesToKeep, const juce::String& model)
    : root (folder), maxBytes (maxBytesToKeep), modelId (model)
{
    root.createDirectory();
}

juce::File StemCache::entryFolder (const juce::String& fingerprint, Quality q) const
{
    return root.getChildFile (fingerprint + "-" + juce::String ((int) q));
}

juce::int64 StemCache::folderSize (const juce::File& folder)
{
    juce::int64 total = 0;
    for (const auto& f : folder.findChildFiles (juce::File::findFiles, false))
        total += f.getSize();
    return total;
}

bool StemCache::isEntryValid (const juce::File& folder, const juce::String& fingerprint, Quality q, juce::var* metaOut) const
{
    const auto metaFile = folder.getChildFile ("meta.json");
    if (! metaFile.existsAsFile())
        return false;

    const auto meta = juce::JSON::parse (metaFile);
    if (! meta.isObject()
        || (int) meta.getProperty ("version", 0) != metaVersion
        || meta.getProperty ("fingerprint", {}).toString() != fingerprint
        || meta.getProperty ("modelId", {}).toString() != modelId
        || (int) meta.getProperty ("quality", -1) != (int) q)
        return false;

    // Every stem file must exist with the recorded size (detects truncated or partially deleted entries).
    auto* sizes = meta.getProperty ("sizes", {}).getArray();
    if (sizes == nullptr || sizes->size() != numStems)
        return false;
    for (int s = 0; s < numStems; ++s)
    {
        const auto file = folder.getChildFile (stemFileName (s));
        if (! file.existsAsFile() || file.getSize() != (juce::int64) (*sizes)[s])
            return false;
    }

    if (metaOut != nullptr)
        *metaOut = meta;
    return true;
}

bool StemCache::contains (const juce::String& fingerprint, Quality atLeast)
{
    const juce::ScopedLock sl (lock);
    for (int q = (int) atLeast; q <= (int) Quality::high; ++q)
        if (isEntryValid (entryFolder (fingerprint, (Quality) q), fingerprint, (Quality) q))
            return true;
    return false;
}

std::shared_ptr<StemSet> StemCache::load (const juce::String& fingerprint, Quality atLeast, juce::String* error)
{
    const juce::ScopedLock sl (lock);

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    for (int q = (int) atLeast; q <= (int) Quality::high; ++q)
    {
        const auto folder = entryFolder (fingerprint, (Quality) q);
        juce::var meta;
        if (! isEntryValid (folder, fingerprint, (Quality) q, &meta))
            continue;

        auto set = std::make_shared<StemSet>();
        set->quality = (Quality) q;
        set->fingerprint = fingerprint;
        set->sampleRate = (double) meta.getProperty ("sampleRate", modelSampleRate);
        const int length = (int) meta.getProperty ("length", 0);
        bool ok = length > 0;

        for (int s = 0; s < numStems && ok; ++s)
        {
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (folder.getChildFile (stemFileName (s))));
            if (reader == nullptr || reader->lengthInSamples != length || reader->numChannels != 2)
            {
                ok = false;
                break;
            }
            set->audio[(size_t) s].setSize (2, length);
            ok = reader->read (&set->audio[(size_t) s], 0, length, 0, true, true);
            set->audio[(size_t) s].applyGain (1.0f / headroomGain);
        }

        if (ok)
        {
            // Mark as recently used for the least-recently-used cleanup.
            folder.getChildFile ("meta.json").setLastModificationTime (juce::Time::getCurrentTime());
            return set;
        }

        // Damaged entry: remove it so it gets recomputed instead of failing every time.
        folder.deleteRecursively();
        if (error != nullptr)
            *error = "cached stems were damaged and have been discarded";
    }
    return nullptr;
}

bool StemCache::store (const juce::String& fingerprint, Quality quality, const std::array<juce::AudioBuffer<float>, numStems>& stems,
                       const juce::String& sourceName)
{
    const juce::ScopedLock sl (lock);

    const int length = stems[0].getNumSamples();
    if (length <= 0 || fingerprint.isEmpty())
        return false;

    // Write into a temporary folder and rename it when complete: a crash never leaves a half-written entry.
    const auto finalFolder = entryFolder (fingerprint, quality);
    const auto tempFolder = root.getChildFile (fingerprint + "-" + juce::String ((int) quality) + ".tmp");
    tempFolder.deleteRecursively();
    if (! tempFolder.createDirectory())
        return false;

    juce::FlacAudioFormat flac;
    juce::Array<juce::var> sizes;

    for (int s = 0; s < numStems; ++s)
    {
        std::unique_ptr<juce::OutputStream> stream (tempFolder.getChildFile (stemFileName (s)).createOutputStream());
        if (stream == nullptr)
        {
            tempFolder.deleteRecursively();
            return false;
        }
        auto writer = flac.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (modelSampleRate)
                                                                                  .withNumChannels (2)
                                                                                  .withBitsPerSample (24));
        if (writer == nullptr)
        {
            tempFolder.deleteRecursively();
            return false;
        }

        juce::AudioBuffer<float> scaled (2, length);
        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::copyWithMultiply (scaled.getWritePointer (ch), stems[(size_t) s].getReadPointer (ch), headroomGain, length);

        const bool written = writer->writeFromAudioSampleBuffer (scaled, 0, length);
        writer.reset();
        if (! written)
        {
            tempFolder.deleteRecursively();
            return false;
        }
        sizes.add ((juce::int64) tempFolder.getChildFile (stemFileName (s)).getSize());
    }

    auto* meta = new juce::DynamicObject();
    meta->setProperty ("version", metaVersion);
    meta->setProperty ("fingerprint", fingerprint);
    meta->setProperty ("modelId", modelId);
    meta->setProperty ("quality", (int) quality);
    meta->setProperty ("sampleRate", modelSampleRate);
    meta->setProperty ("length", length);
    meta->setProperty ("source", sourceName);
    meta->setProperty ("created", juce::Time::getCurrentTime().toISO8601 (true));
    meta->setProperty ("sizes", sizes);
    if (! tempFolder.getChildFile ("meta.json").replaceWithText (juce::JSON::toString (juce::var (meta))))
    {
        tempFolder.deleteRecursively();
        return false;
    }

    finalFolder.deleteRecursively();
    if (! tempFolder.moveFileTo (finalFolder))
    {
        tempFolder.deleteRecursively();
        return false;
    }

    enforceLimit();
    return true;
}

void StemCache::remove (const juce::String& fingerprint)
{
    const juce::ScopedLock sl (lock);
    for (int q = 0; q <= (int) Quality::high; ++q)
        entryFolder (fingerprint, (Quality) q).deleteRecursively();
}

void StemCache::clear()
{
    const juce::ScopedLock sl (lock);
    for (const auto& f : root.findChildFiles (juce::File::findDirectories, false))
        f.deleteRecursively();
}

juce::int64 StemCache::getUsedBytes()
{
    const juce::ScopedLock sl (lock);
    juce::int64 total = 0;
    for (const auto& f : root.findChildFiles (juce::File::findDirectories, false))
        total += folderSize (f);
    return total;
}

int StemCache::getNumEntries()
{
    const juce::ScopedLock sl (lock);
    return root.getNumberOfChildFiles (juce::File::findDirectories);
}

int StemCache::enforceLimit()
{
    const juce::ScopedLock sl (lock);

    // Leftovers of interrupted writes first.
    for (const auto& f : root.findChildFiles (juce::File::findDirectories, false, "*.tmp"))
        f.deleteRecursively();

    if (maxBytes <= 0)
        return 0;

    struct Entry { juce::File folder; juce::int64 size; juce::Time used; };
    std::vector<Entry> entries;
    juce::int64 total = 0;
    for (const auto& f : root.findChildFiles (juce::File::findDirectories, false))
    {
        const auto size = folderSize (f);
        total += size;
        entries.push_back ({ f, size, f.getChildFile ("meta.json").getLastModificationTime() });
    }

    std::sort (entries.begin(), entries.end(), [] (const Entry& a, const Entry& b) { return a.used < b.used; });

    int removed = 0;
    for (const auto& e : entries)
    {
        if (total <= maxBytes)
            break;
        if (e.folder.deleteRecursively())
        {
            total -= e.size;
            ++removed;
        }
    }
    return removed;
}

} // namespace pathboard::stems
