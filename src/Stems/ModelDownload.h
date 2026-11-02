#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <functional>

namespace pathboard
{

// Downloads one model file in a background thread: to "<name>.part", checks size and SHA-256, then renames it.
// A damaged or wrong file is deleted and reported; nothing half-finished is ever left under the final name.
// onProgress / onFinished are called on the message thread; onFinished gets an empty string on success.
class ModelDownload : private juce::Thread
{
public:
    ModelDownload() : juce::Thread ("Model download") {}
    ~ModelDownload() override { stopThread (15000); }

    bool isRunning() const { return running.load(); }
    void cancel() { signalThreadShouldExit(); }

    // Returns false if a download is already running.
    bool start (const juce::String& url, const juce::File& target, juce::int64 expectedSize, const juce::String& expectedSha256,
                std::function<void (float)> onProgress, std::function<void (const juce::String&)> onFinished);

    // SHA-256 of a file as lower-case hex, empty if it cannot be read.
    static juce::String sha256Of (const juce::File& file);

private:
    void run() override;

    juce::String url, expectedSha;
    juce::File target;
    juce::int64 expectedSize = 0;
    std::function<void (float)> progressCallback;
    std::function<void (const juce::String&)> finishedCallback;
    std::atomic<bool> running { false };

    JUCE_DECLARE_WEAK_REFERENCEABLE (ModelDownload)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModelDownload)
};

} // namespace pathboard
