#pragma once

#include "Stems/StemSet.h"

namespace pathboard::stems
{

// On-disk cache of separated stems. One folder per (file fingerprint, quality):
//   <cache>/<fingerprint>-<quality>/{drums,bass,other,vocals}.flac + meta.json
// Stems are stored as 24-bit FLAC with 6 dB of headroom (the model can overshoot full scale).
// An entry is valid only if its meta matches the current model and the current content fingerprint of
// the source file, so edited or replaced files never get old stems. Thread-safe.
class StemCache
{
public:
    // maxBytes: least recently used entries are deleted when the cache grows beyond this (0 = unlimited).
    StemCache (const juce::File& folder, juce::int64 maxBytes, const juce::String& modelId);

    // Best cached entry for the fingerprint with at least the requested quality, or nullptr.
    std::shared_ptr<StemSet> load (const juce::String& fingerprint, Quality atLeast, juce::String* error = nullptr);
    bool contains (const juce::String& fingerprint, Quality atLeast);

    bool store (const juce::String& fingerprint, Quality quality, const std::array<juce::AudioBuffer<float>, numStems>& stems,
                const juce::String& sourceName);

    void remove (const juce::String& fingerprint);
    void clear();
    juce::int64 getUsedBytes();
    int getNumEntries();
    // Deletes the least recently used entries until the cache fits into the limit. Returns how many were removed.
    int enforceLimit();

    static constexpr float headroomGain = 0.5f;

private:
    juce::File entryFolder (const juce::String& fingerprint, Quality q) const;
    bool isEntryValid (const juce::File& folder, const juce::String& fingerprint, Quality q, juce::var* metaOut = nullptr) const;
    static juce::int64 folderSize (const juce::File& folder);

    juce::File root;
    juce::int64 maxBytes;
    juce::String modelId;
    juce::CriticalSection lock;
};

} // namespace pathboard::stems
