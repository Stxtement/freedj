// Offline tests for the audio engine: renders real audio through DeckEngine and
// MixerEngine (no sound card needed) and checks measurable behaviour.

#include "Audio/DeckEngine.h"
#include "Mixer/MixerEngine.h"
#include "TestSupport.h"
#include <juce_events/juce_events.h>

using namespace pathboard;
using testsupport::check;

namespace
{
constexpr double testRate = 44100.0;
constexpr int blockSize = 512;

juce::File writeTestWav (const juce::File& file, double seconds, std::function<float (int, int)> generator)
{
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (testRate)
                                                                              .withNumChannels (2)
                                                                              .withBitsPerSample (24));
    jassert (writer != nullptr);

    const int total = (int) (seconds * testRate);
    juce::AudioBuffer<float> buffer (2, total);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < total; ++i)
            buffer.setSample (ch, i, generator (ch, i));

    writer->writeFromAudioSampleBuffer (buffer, 0, total);
    return file;
}

bool waitForLoad (DeckEngine& deck)
{
    for (int i = 0; i < 500 && deck.isLoading(); ++i)
        juce::Thread::sleep (10);
    return deck.hasTrackLoaded() && ! deck.isLoading();
}

// Renders `seconds` of deck output; returns the RMS of channel 0.
float renderDeck (DeckEngine& deck, double seconds, juce::AudioBuffer<float>* capture = nullptr)
{
    const int total = (int) (seconds * testRate);
    juce::AudioBuffer<float> block (2, blockSize);
    double sumSquares = 0.0;
    if (capture != nullptr)
        capture->setSize (2, total);

    for (int pos = 0; pos < total; pos += blockSize)
    {
        const int n = juce::jmin (blockSize, total - pos);
        deck.getNextAudioBlock (juce::AudioSourceChannelInfo (&block, 0, n));
        for (int i = 0; i < n; ++i)
            sumSquares += (double) block.getSample (0, i) * block.getSample (0, i);
        if (capture != nullptr)
            for (int ch = 0; ch < 2; ++ch)
                capture->copyFrom (ch, pos, block, ch, 0, n);
    }
    return (float) std::sqrt (sumSquares / juce::jmax (1, total));
}

float sine (int i, double freq, float amp = 0.5f)
{
    return amp * (float) std::sin (juce::MathConstants<double>::twoPi * freq * i / testRate);
}
}

int runEngineTests (int argc, char* argv[])
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    // Optional: decode real files given on the command line (e.g. MP3/FLAC/M4A from a library).
    for (int a = 1; a < argc; ++a)
    {
        const juce::File file (juce::String::fromUTF8 (argv[a]));
        DeckEngine deck (formats);
        deck.prepareToPlay (blockSize, testRate);
        const auto started = juce::Time::getMillisecondCounterHiRes();
        const bool ok = deck.loadFile (file) && waitForLoad (deck);
        const auto ms = juce::Time::getMillisecondCounterHiRes() - started;
        deck.play();
        const float rms = ok ? renderDeck (deck, 2.0) : 0.0f;
        check (ok && rms > 0.001f, "decode " + file.getFileName(),
               ok ? juce::String (deck.getLengthSeconds(), 1) + " s, loaded in " + juce::String ((int) ms) + " ms"
                  : deck.getLastError());
    }
    if (argc > 1)
        return 0;

    const auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("FreeDJTests");
    tempDir.createDirectory();

    const auto toneFile = writeTestWav (tempDir.getChildFile ("tone440.wav"), 10.0, [] (int, int i) { return sine (i, 440.0); });
    const auto bassFile = writeTestWav (tempDir.getChildFile ("bass80.wav"), 4.0,  [] (int, int i) { return sine (i, 80.0); });
    const auto rampFile = writeTestWav (tempDir.getChildFile ("ramp.wav"), 4.0,
                                        [] (int, int i) { return (float) i / (float) (4.0 * testRate); });

    std::cout << "Loading" << std::endl;
    {
        DeckEngine deck (formats);
        deck.prepareToPlay (blockSize, testRate);
        check (! deck.loadFile (tempDir.getChildFile ("missing.wav")), "missing file is rejected");
        check (deck.loadFile (toneFile) && waitForLoad (deck), "WAV decodes in background");
        check (std::abs (deck.getLengthSeconds() - 10.0) < 0.01, "length is correct", juce::String (deck.getLengthSeconds(), 3));

        auto garbage = tempDir.getChildFile ("garbage.wav");
        garbage.replaceWithText ("this is not audio");
        deck.loadFile (garbage);
        for (int i = 0; i < 200 && deck.isLoading(); ++i) juce::Thread::sleep (10);
        check (deck.getLastError().isNotEmpty() && ! deck.isLoading(), "corrupt file reports an error", deck.getLastError());
    }

    std::cout << "Transport" << std::endl;
    {
        DeckEngine deck (formats);
        deck.prepareToPlay (blockSize, testRate);
        deck.loadFile (toneFile);
        waitForLoad (deck);

        check (renderDeck (deck, 0.5) < 1.0e-6f, "stopped deck is silent");

        deck.play();
        const float rms = renderDeck (deck, 2.0);
        check (std::abs (rms - 0.3536f) < 0.02f, "playing deck outputs the track", "rms " + juce::String (rms, 4));
        check (std::abs (deck.getCurrentPositionSeconds() - 2.0) < 0.02, "position advances in real time",
               juce::String (deck.getCurrentPositionSeconds(), 3));

        deck.pause();
        renderDeck (deck, 0.1);
        const double pausedAt = deck.getCurrentPositionSeconds();
        renderDeck (deck, 1.0);
        check (std::abs (deck.getCurrentPositionSeconds() - pausedAt) < 1.0e-6, "pause holds position");

        deck.setPositionSeconds (5.0);
        renderDeck (deck, 0.01);
        check (std::abs (deck.getCurrentPositionSeconds() - 5.0) < 0.01, "seek works");

        deck.setPositionSeconds (9.0);
        deck.play();
        renderDeck (deck, 2.0);
        check (! deck.isPlaying(), "deck stops at end of track");
    }

    std::cout << "Pitch" << std::endl;
    {
        DeckEngine deck (formats);
        deck.prepareToPlay (blockSize, testRate);
        deck.loadFile (toneFile);
        waitForLoad (deck);

        deck.setPitch (0.10);
        deck.play();
        renderDeck (deck, 0.2);
        const double start = deck.getCurrentPositionSeconds();
        juce::AudioBuffer<float> out;
        renderDeck (deck, 2.0, &out);
        const double advanced = deck.getCurrentPositionSeconds() - start;
        check (std::abs (advanced - 2.2) < 0.01, "+10% pitch plays 10% faster", juce::String (advanced, 4) + " s in 2 s");

        int crossings = 0;
        for (int i = 1; i < out.getNumSamples(); ++i)
            if (out.getSample (0, i - 1) < 0.0f && out.getSample (0, i) >= 0.0f)
                ++crossings;
        const double freq = crossings / 2.0;
        check (std::abs (freq - 484.0) < 3.0, "+10% pitch raises a 440 Hz tone to 484 Hz", juce::String (freq, 1) + " Hz");
    }

    std::cout << "Loop" << std::endl;
    {
        DeckEngine deck (formats);
        deck.prepareToPlay (blockSize, testRate);
        deck.loadFile (toneFile);
        waitForLoad (deck);

        deck.setPositionSeconds (1.0);
        renderDeck (deck, 0.01);
        deck.toggleLoop (0.5);
        deck.play();
        renderDeck (deck, 3.0);
        const double pos = deck.getCurrentPositionSeconds();
        check (deck.isLooping() && pos >= 1.0 && pos <= 1.5, "loop keeps playback inside 1.0-1.5 s", juce::String (pos, 3));
        deck.exitLoop();
        renderDeck (deck, 1.0);
        check (deck.getCurrentPositionSeconds() > 1.5, "exiting loop continues playback");
    }

    std::cout << "Scratch" << std::endl;
    {
        DeckEngine deck (formats);
        deck.prepareToPlay (blockSize, testRate);
        deck.loadFile (rampFile);
        waitForLoad (deck);

        deck.setPositionSeconds (2.0);
        renderDeck (deck, 0.01);
        deck.beginScratch();
        deck.setScratchRate (-1.0);
        juce::AudioBuffer<float> out;
        renderDeck (deck, 0.5, &out);
        check (deck.getCurrentPositionSeconds() < 1.6, "negative scratch rate plays backwards",
               juce::String (deck.getCurrentPositionSeconds(), 3));
        // The ramp file's sample value encodes position, so reverse playback must produce a falling signal.
        check (out.getSample (0, out.getNumSamples() - 1) < out.getSample (0, 4000), "reverse audio is actually output");

        deck.setScratchRate (0.0);
        renderDeck (deck, 0.1);
        const double held = deck.getCurrentPositionSeconds();
        const float heldRms = renderDeck (deck, 0.2);
        check (std::abs (deck.getCurrentPositionSeconds() - held) < 0.001 && heldRms < 1.0e-4f,
               "holding the platter stops audio cleanly");
        deck.endScratch();
        check (! deck.isPlaying(), "scratch on a paused deck leaves it paused");
    }

    std::cout << "EQ / filter / gain" << std::endl;
    {
        DeckEngine deck (formats);
        deck.prepareToPlay (blockSize, testRate);
        deck.loadFile (bassFile);
        waitForLoad (deck);
        deck.play();

        const float flat = renderDeck (deck, 0.5);
        deck.setEq (-1.0f, 0.0f, 0.0f);
        renderDeck (deck, 0.1);
        const float lowKilled = renderDeck (deck, 0.5);
        const double dbDrop = juce::Decibels::gainToDecibels (lowKilled / flat);
        check (dbDrop < -15.0, "LOW EQ at minimum cuts an 80 Hz tone", juce::String (dbDrop, 1) + " dB");

        deck.setEq (0.0f, 0.0f, 0.0f);
        deck.setFilter (0.8f);
        renderDeck (deck, 0.1);
        const float hp = renderDeck (deck, 0.5);
        check (juce::Decibels::gainToDecibels (hp / flat) < -20.0, "high-pass filter removes bass",
               juce::String (juce::Decibels::gainToDecibels (hp / flat), 1) + " dB");

        deck.setFilter (0.0f);
        deck.setGainTrim (2.0f);
        renderDeck (deck, 0.1);
        const float boosted = renderDeck (deck, 0.5);
        check (std::abs (boosted / flat - 2.0f) < 0.05f, "gain trim doubles level", juce::String (boosted / flat, 3));
    }

    std::cout << "Mixer / crossfader" << std::endl;
    {
        DeckEngine deckA (formats), deckB (formats);
        for (auto* d : { &deckA, &deckB })
        {
            d->prepareToPlay (blockSize, testRate);
            d->loadFile (toneFile);
            waitForLoad (*d);
        }
        MixerEngine mixer (deckA, deckB);
        mixer.setMasterVolume (1.0f);

        juce::AudioBuffer<float> out (4, blockSize);
        auto renderMaster = [&] (int blocks)
        {
            double sum = 0.0;
            for (int b = 0; b < blocks; ++b)
            {
                mixer.audioDeviceIOCallbackWithContext (nullptr, 0, out.getArrayOfWritePointers(), 4, blockSize, {});
                for (int i = 0; i < blockSize; ++i)
                    sum += (double) out.getSample (0, i) * out.getSample (0, i);
            }
            return (float) std::sqrt (sum / (blocks * blockSize));
        };

        deckA.play();
        mixer.setCrossfader (-1.0f);
        renderMaster (20);
        const float aLeft = renderMaster (40);
        mixer.setCrossfader (1.0f);
        renderMaster (20);
        const float aRight = renderMaster (40);
        check (aLeft > 0.3f && aRight < 1.0e-4f, "crossfader fully right silences deck A",
               juce::String (aLeft, 3) + " -> " + juce::String (aRight, 6));

        deckA.pause();
        deckB.play();
        const float bRight = renderMaster (40);
        check (bRight > 0.3f, "crossfader fully right passes deck B", juce::String (bRight, 3));

        mixer.setChannelFader (1, 0.0f);
        renderMaster (5);
        check (renderMaster (20) < 1.0e-6f, "channel fader down silences deck B");

        mixer.setChannelFader (1, 1.0f);
        mixer.setHeadphoneCue (1, true);
        mixer.setHeadphoneMix (0.0f);
        mixer.setMasterVolume (0.0f);
        renderMaster (5);
        double hpSum = 0.0;
        for (int i = 0; i < blockSize; ++i)
            hpSum += std::abs (out.getSample (2, i));
        check (renderMaster (5) < 1.0e-6f && hpSum > 1.0, "headphone cue is audible on outputs 3/4 with master muted");
        check (mixer.getDeckLevel (1) > 0.3f, "VU meter reports deck level", juce::String (mixer.getDeckLevel (1), 3));
    }

    tempDir.deleteRecursively();

    return 0;
}
