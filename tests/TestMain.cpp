#include "TestSupport.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include "Analysis/AudioAnalysis.h"
#include "Library/LibraryManager.h"
#include "Library/SmartMatch.h"
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

    // `--match-report <library.db> <title part>...`: how every track of the library matches each reference track.
    if (argc >= 4 && juce::String (argv[1]) == "--match-report")
    {
        pathboard::LibraryManager lib;
        if (! lib.open (juce::File (juce::String::fromUTF8 (argv[2]))).wasOk())
            return 1;
        for (int a = 3; a < argc; ++a)
        {
            const pathboard::TrackRecord* reference = nullptr;
            for (const auto& t : lib.getTracks())
                if (t.title.containsIgnoreCase (juce::String::fromUTF8 (argv[a]))) { reference = &t; break; }
            if (reference == nullptr)
                continue;
            const auto refSubject = pathboard::makeSubject (*reference);
            std::cout << "\nREFERENCE " << reference->title << ": " << (refSubject.bpm > 0 ? juce::String (refSubject.bpm, 2) : juce::String ("BPM unknown")) << " BPM, key "
                      << (refSubject.key ? refSubject.key->text() : juce::String ("unknown")) << ", genre "
                      << (refSubject.genres.empty() ? juce::String ("unknown") : refSubject.genres[0].toString()) << std::endl;
            int veryGood = 0, good = 0;
            for (const auto& t : lib.getTracks())
            {
                if (t.id == reference->id) continue;
                const auto d = pathboard::evaluateMatch (refSubject, pathboard::makeSubject (t));
                if (d.level == pathboard::MatchLevel::none) continue;
                (d.level == pathboard::MatchLevel::veryGood ? veryGood : good)++;
                std::cout << "  " << (d.level == pathboard::MatchLevel::veryGood ? "GREEN " : "BLUE  ") << t.title.paddedRight (' ', 34) << d.explanation << std::endl;
            }
            std::cout << "  -> " << veryGood << " green, " << good << " blue of " << lib.getTracks().size() - 1 << " tracks" << std::endl;
        }
        return 0;
    }

    if (argc >= 3 && juce::String (argv[1]) == "--genre-probe")
    {
        juce::StringArray files;
        for (int i = 2; i < argc; ++i)
            files.add (juce::String::fromUTF8 (argv[i]));
        return runGenreProbe (files);
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

        std::cout << std::endl << "=== Matching, genre, downloads ===" << std::endl;
        runMatchTests();
        runGenreTests();

        std::cout << std::endl << "=== Library texts ===" << std::endl;
        runTidyTests();

        std::cout << std::endl << "=== Effects ===" << std::endl;
        runEffectTests();

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
