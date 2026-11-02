#include "ModelDownload.h"
#include <juce_cryptography/juce_cryptography.h>

namespace pathboard
{

juce::String ModelDownload::sha256Of (const juce::File& file)
{
    juce::FileInputStream in (file);
    if (in.failedToOpen())
        return {};
    return juce::SHA256 (in).toHexString();
}

bool ModelDownload::start (const juce::String& u, const juce::File& t, juce::int64 size, const juce::String& sha,
                           std::function<void (float)> onProgress, std::function<void (const juce::String&)> onFinished)
{
    if (running.exchange (true))
        return false;

    stopThread (15000);   // a previous run has finished; this just joins the thread
    url = u;
    target = t;
    expectedSize = size;
    expectedSha = sha;
    progressCallback = std::move (onProgress);
    finishedCallback = std::move (onFinished);
    startThread (juce::Thread::Priority::low);
    return true;
}

void ModelDownload::run()
{
    juce::String error;
    target.getParentDirectory().createDirectory();
    const auto part = target.getSiblingFile (target.getFileName() + ".part");
    part.deleteFile();

    {
        juce::URL source (url);
        std::unique_ptr<juce::InputStream> in (source.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                                             .withConnectionTimeoutMs (20000)
                                                                             .withNumRedirectsToFollow (8)));
        if (in == nullptr)
        {
            error = "Could not connect to the download server.";
        }
        else
        {
            juce::FileOutputStream out (part);
            if (out.failedToOpen())
            {
                error = "Cannot write " + part.getFullPathName();
            }
            else
            {
                juce::HeapBlock<char> buffer (1 << 20);
                juce::int64 total = 0;
                while (! threadShouldExit())
                {
                    const int n = in->read (buffer, 1 << 20);
                    if (n < 0) { error = "The download was interrupted."; break; }
                    if (n == 0) break;
                    if (! out.write (buffer, (size_t) n)) { error = "Disk full or write error."; break; }
                    total += n;

                    juce::MessageManager::callAsync ([weak = juce::WeakReference<ModelDownload> (this), f = (float) total / (float) juce::jmax<juce::int64> (1, expectedSize)]
                                                     {
                                                         if (weak != nullptr && weak->progressCallback)
                                                             weak->progressCallback (juce::jlimit (0.0f, 1.0f, f));
                                                     });
                }
                out.flush();
                if (threadShouldExit() && error.isEmpty())
                    error = "cancelled";
            }
        }
    }

    if (error.isEmpty())
    {
        if (expectedSize > 0 && part.getSize() != expectedSize)
            error = "The downloaded file has the wrong size (" + juce::String (part.getSize()) + " instead of " + juce::String (expectedSize) + " bytes).";
        else if (expectedSha.isNotEmpty() && ! sha256Of (part).equalsIgnoreCase (expectedSha))
            error = "The downloaded file does not match the expected checksum.";
    }

    if (error.isEmpty())
    {
        target.deleteFile();
        if (! part.moveFileTo (target))
            error = "Cannot move the model into place.";
    }
    if (error.isNotEmpty())
        part.deleteFile();

    juce::MessageManager::callAsync ([weak = juce::WeakReference<ModelDownload> (this), error]
                                     {
                                         if (auto* self = weak.get())
                                         {
                                             self->running = false;
                                             if (self->finishedCallback)
                                                 self->finishedCallback (error);
                                         }
                                     });
}

} // namespace pathboard
