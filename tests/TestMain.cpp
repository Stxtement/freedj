#include "TestSupport.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include "Analysis/AudioAnalysis.h"
#include <juce_events/juce_events.h>

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // `--midi-monitor <device name part> <seconds>`: prints every incoming message as "<unix ms>|<hex bytes>".
    if (argc == 4 && juce::String (argv[1]) == "--midi-monitor")
    {
        struct Printer : juce::MidiInputCallback
        {
            void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m) override
            {
                std::cout << juce::Time::currentTimeMillis() << "|"
                          << juce::String::toHexString (m.getRawData(), m.getRawDataSize(), 1) << std::endl;
            }
        } printer;

        std::unique_ptr<juce::MidiInput> input;
        for (const auto& d : juce::MidiInput::getAvailableDevices())
            if (d.name.containsIgnoreCase (argv[2]))
            {
                input = juce::MidiInput::openDevice (d.identifier, &printer);
                break;
            }
        if (input == nullptr)
        {
            std::cout << "device not found" << std::endl;
            return 1;
        }
        input->start();
        std::cout << "READY" << std::endl;
        juce::Thread::sleep ((int) (juce::String (argv[3]).getDoubleValue() * 1000.0));
        input->stop();
        return 0;
    }

    if (argc == 2 && juce::String (argv[1]) == "--midi-list")
    {
        for (const auto& d : juce::MidiInput::getAvailableDevices())
            std::cout << "MIDI in : " << d.name << "  [" << d.identifier << "]" << std::endl;
        for (const auto& d : juce::MidiOutput::getAvailableDevices())
            std::cout << "MIDI out: " << d.name << "  [" << d.identifier << "]" << std::endl;
        return 0;
    }

    if (argc >= 3 && juce::String (argv[1]) == "--analyse")
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        for (int i = 2; i < argc; ++i)
        {
            const juce::File file (juce::String::fromUTF8 (argv[i]));
            if (pathboard::analysis::isOnlineOnly (file))
            {
                std::cout << file.getFileName() << ": skipped (OneDrive online-only, would be downloaded)" << std::endl;
                continue;
            }

            pathboard::analysis::MonoAudio audio;
            juce::String error;
            const auto started = juce::Time::getMillisecondCounterHiRes();
            if (! pathboard::analysis::decodeToMono (file, formats, audio, error))
            {
                std::cout << file.getFileName() << ": " << error << std::endl;
                continue;
            }
            const auto decodeMs = juce::Time::getMillisecondCounterHiRes() - started;
            const auto a = pathboard::analysis::analyseAudio (audio);
            std::cout << file.getFileName() << "\n    BPM " << juce::String (a.tempo.bpm, 2) << " (conf " << juce::String (a.tempo.confidence, 2)
                      << ", alt " << juce::String (a.tempo.alternativeBpm, 2) << (a.tempo.variableTempo ? ", VARIABLE" : "")
                      << ")  first beat " << juce::String (a.tempo.firstBeatSeconds, 3) << " s  downbeat +" << a.tempo.downbeatIndex
                      << " (conf " << juce::String (a.tempo.downbeatConfidence, 2) << ")\n    key " << a.key.name << " / " << a.key.camelot
                      << " (conf " << juce::String (a.key.confidence, 2) << ")  energy " << juce::String (a.energy, 1)
                      << "  loudness " << juce::String (a.loudnessDb, 1) << " dB  length " << juce::String (a.durationSeconds, 1)
                      << " s  [decode " << (int) decodeMs << " ms, analysis " << (int) (juce::Time::getMillisecondCounterHiRes() - started - decodeMs) << " ms]"
                      << std::endl;
        }
        return 0;
    }

    if (argc == 5 && juce::String (argv[1]) == "--stem-probe")
        return runStemProbe (juce::File (juce::String::fromUTF8 (argv[2])), juce::String (argv[3]).getDoubleValue(), juce::String (argv[4]).getDoubleValue());

    if (argc == 5 && juce::String (argv[1]) == "--seed-library")
        return seedLibrary (juce::File (juce::String::fromUTF8 (argv[2])), juce::File (juce::String::fromUTF8 (argv[3])),
                            juce::String (argv[4]).getIntValue());

    std::cout << "=== Engine ===" << std::endl;
    runEngineTests (argc, argv);

    // With file arguments only the decoder check runs (see EngineTests.cpp).
    if (argc <= 1)
    {
        std::cout << std::endl << "=== Library ===" << std::endl;
        runLibraryTests();

        std::cout << std::endl << "=== Analysis ===" << std::endl;
        runAnalysisTests();
        runAnalysisStorageTests();

        std::cout << std::endl << "=== Controller ===" << std::endl;
        runControllerTests();

        std::cout << std::endl << "=== Stems ===" << std::endl;
        runStemTests();
    }

    std::cout << std::endl
              << (testsupport::failures == 0 ? juce::String ("ALL TESTS PASSED")
                                             : juce::String (testsupport::failures) + " TEST(S) FAILED")
              << std::endl;
    return testsupport::failures == 0 ? 0 : 1;
}
