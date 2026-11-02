#pragma once

#include "Analysis/AnalysisManager.h"
#include "Audio/AudioEngine.h"
#include "Controller/ControllerManager.h"
#include "Decks/DeckComponent.h"
#include "Effects/FxComponent.h"
#include "Library/LibraryBrowser.h"
#include "Library/LibraryManager.h"
#include "Mixer/MixerComponent.h"
#include "Spotify/SpotifyClient.h"
#include "Stems/StemManager.h"
#include "UI/DJLookAndFeel.h"
#include "Waveform/WaveformDisplay.h"
#include <juce_data_structures/juce_data_structures.h>
#include <unordered_set>

namespace pathboard
{

class MainComponent : public juce::Component,
                      public juce::FileDragAndDropTarget,
                      public juce::DragAndDropTarget,
                      public juce::DragAndDropContainer,
                      private juce::Timer
{
public:
    explicit MainComponent (juce::PropertiesFile& settings);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

    void saveSettings();

    static juce::File getDataFolder();

private:
    void timerCallback() override;
    void showAudioSettings();
    void loadIntoDeck (const juce::File& file, int deckIndex);
    int deckAt (juce::Point<int> p) const;
    void openLibrary();
    void createDailyBackup();
    void trackPlayHistory();
    void updateDeckInfo (int deck);

    // Controller
    void registerActions();
    void showControllerSettings();
    juce::File getControllerMappingFile() const { return getDataFolder().getChildFile ("controller-mapping.json"); }

    // Smart Matching and genre detection
    void updateMatchReferences();
    void detectGenre (const std::vector<juce::int64>& ids);
    void startGenreModelDownload (std::vector<juce::int64> thenDetect);
    static juce::File findGenreModelFile();
    ModelDownload genreDownload;

    // Stems
    void requestStems (int deck);
    void updateStemStatus (int deck);
    void stemStatusClicked (int deck);
    void startModelDownload();
    void prepareStems (const std::vector<juce::File>& files);
    static juce::File findModelFile();
    int analysisCheckCounter = 0;
    void precomputeStemsStep();   // needs <unordered_set>, included below
    bool autoPrecomputeStems = false;   // heavy on the GPU/CPU: off until switched on (PRE-STEMS)
    juce::ToggleButton preStemsButton { "PRE-STEMS" };
    std::unordered_set<juce::int64> stemPrepRequested;   // tracks already queued for background stems in this run

    juce::PropertiesFile& settings;
    DJLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltipWindow { this, 600 };

    AudioEngine engine;
    LibraryManager library;
    AnalysisManager analysis { library, engine.getFormatManager() };
    static constexpr juce::int64 stemCacheBytes = 20LL * 1024 * 1024 * 1024;
    stems::StemManager stemManager { getDataFolder().getChildFile ("stems"), stemCacheBytes, engine.getFormatManager() };

    struct StemRequest
    {
        int id = 0;
        juce::String path;
        juce::String message;     // last status text shown
        bool haveStems = false;
        stems::Quality deliveredQuality = stems::Quality::balanced;
        bool fromCache = false;
    };
    std::array<StemRequest, 2> stemRequests;
    stems::Quality stemQuality = stems::Quality::balanced;
    bool autoStems = true;
    float modelDownloadProgress = -1.0f;   // < 0: not downloading

    WaveformDisplay waveA { engine.getDeck (0), 0, WaveformDisplay::Mode::scrolling };
    WaveformDisplay waveB { engine.getDeck (1), 1, WaveformDisplay::Mode::scrolling };
    DeckComponent deckA { engine.getDeck (0), engine.getDeck (1), 0 };
    DeckComponent deckB { engine.getDeck (1), engine.getDeck (0), 1 };
    MixerComponent mixer { engine };
    FxComponent fxStrip { engine.getMixer() };
    int fxTarget = 2;   // the controller's FX section acts on: 0 = deck 1, 1 = deck 2, 2 = both
    LibraryBrowser browser { library };

    // Hardware controller: functions are registered as named actions, the mapping is data (editable, saved).
    ActionRegistry actionRegistry;
    ControllerMapping controllerMapping = ControllerMapping::createDefaultFlx4();
    ControllerManager controller { actionRegistry, controllerMapping };
    std::array<bool, 2> jogTouched { false, false };
    juce::Component::SafePointer<juce::DialogWindow> controllerDialog, audioDialog, spotifyDialog;
    juce::TextButton controllerButton { "CONTROLLER" }, spotifyButton { "SPOTIFY" };
    spotify::SpotifyClient spotifyClient { settings };
    void showSpotify();

    struct PlayTracking
    {
        juce::String path;
        juce::int64 trackId = 0;
        double playedSeconds = 0.0;
        bool counted = false;
        DeckComponent::LibraryInfo shown;   // what the deck currently displays
        bool infoShown = false;
    };
    std::array<PlayTracking, 2> tracking;

    juce::Label titleLabel, statusLabel;
    juce::TextButton settingsButton { "AUDIO SETUP" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace pathboard
