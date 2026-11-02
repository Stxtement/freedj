#include "MainComponent.h"
#include "Controller/ControllerSettingsComponent.h"

namespace pathboard
{

namespace
{
    constexpr const char* audioDeviceKey = "audioDeviceState";
    constexpr const char* libraryColumnsKey = "libraryColumns";
    constexpr const char* stemQualityKey = "stemQuality";
    constexpr const char* autoStemsKey = "autoStems";
    constexpr const char* smartMatchKey = "smartMatch";
    constexpr const char* smartMatchModeKey = "smartMatchMode";
    constexpr double countAsPlayedAfterSeconds = 15.0;
    constexpr int backupsToKeep = 7;
}

juce::File MainComponent::getDataFolder()
{
    // Lets tests and development runs use an isolated library instead of the user's real one.
    const auto override = juce::SystemStats::getEnvironmentVariable ("PATHBOARD_DATA_DIR", {});
    if (override.isNotEmpty())
        return juce::File (override);

    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("PathboardDJ");
}

MainComponent::MainComponent (juce::PropertiesFile& s) : settings (s)
{
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
    setLookAndFeel (&lookAndFeel);

    const auto savedDevice = settings.getXmlValue (audioDeviceKey);
    const auto error = engine.initialise (savedDevice.get());
    if (error.isNotEmpty())
        juce::NativeMessageBox::showAsync (juce::MessageBoxOptions()
                                               .withIconType (juce::MessageBoxIconType::WarningIcon)
                                               .withTitle ("Audio device problem")
                                               .withMessage (error + "\n\nOpen AUDIO SETUP to choose another device."),
                                           nullptr);

    titleLabel.setText ("FREE DJ", juce::dontSendNotification);
    titleLabel.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    titleLabel.setColour (juce::Label::textColourId, Colours::text);
    statusLabel.setJustificationType (juce::Justification::centredRight);
    statusLabel.setColour (juce::Label::textColourId, Colours::textDim);
    settingsButton.onClick = [this] { showAudioSettings(); };

    controllerButton.setTooltip ("Connect and configure the DJ controller (MIDI mapping, MIDI learn, monitor)");
    controllerButton.onClick = [this] { showControllerSettings(); };

    for (auto* c : std::initializer_list<juce::Component*> { &titleLabel, &statusLabel, &settingsButton, &controllerButton,
                                                             &waveA, &waveB, &deckA, &deckB, &mixer, &browser })
        addAndMakeVisible (c);

    browser.onLoadRequest = [this] (const juce::File& file, int deckIndex) { loadIntoDeck (file, deckIndex); };
    browser.setAnalysisManager (&analysis);
    browser.setLayoutState (settings.getValue (libraryColumnsKey));

    // Beat grid / BPM corrections made on a deck are stored for the loaded track and are never overwritten by analysis.
    for (int d = 0; d < 2; ++d)
        (d == 0 ? deckA : deckB).onGridEdited = [this, d] (double bpm, double firstBeat, int downbeat)
        {
            if (tracking[(size_t) d].trackId != 0)
                library.setManualGrid (tracking[(size_t) d].trackId, bpm, firstBeat, downbeat);
        };

    // Genre detection (optional AI model) and Smart Matching (on by default, switchable, remembered).
    analysis.setGenreModelFile (findGenreModelFile());
    browser.setSmartMatch (settings.getBoolValue (smartMatchKey, true),
                           (LibraryBrowser::MatchMode) juce::jlimit (0, 3, settings.getIntValue (smartMatchModeKey, 0)));
    browser.onSmartMatchSettingsChanged = [this]
    {
        settings.setValue (smartMatchKey, browser.isSmartMatchEnabled());
        settings.setValue (smartMatchModeKey, (int) browser.getMatchMode());
    };
    browser.onDetectGenre = [this] (const std::vector<juce::int64>& ids) { detectGenre (ids); };

    // Controller: saved mapping if there is one, otherwise the FLX4 defaults. Every change is saved at once.
    if (getControllerMappingFile().existsAsFile())
    {
        const auto loaded = controllerMapping.loadFrom (getControllerMappingFile());
        if (loaded.failed())
            controllerMapping = ControllerMapping::createDefaultFlx4();
    }
    registerActions();
    controller.onMappingChanged = [this] { controllerMapping.saveTo (getControllerMappingFile()); };
    controller.setAutoConnect (true);

    // Stems: AI separation in the background; the decks only ever read finished stems.
    stemManager.setModelFile (findModelFile());
    stemQuality = (stems::Quality) juce::jlimit (0, 2, settings.getIntValue (stemQualityKey, (int) stems::Quality::balanced));
    autoStems = settings.getBoolValue (autoStemsKey, true);
    for (int d = 0; d < 2; ++d)
    {
        auto& deckComponent = d == 0 ? deckA : deckB;
        deckComponent.setStemQuality (stemQuality);
        deckComponent.onStemRequested = [this, d] { stemStatusClicked (d); };
        deckComponent.onStemQualityChanged = [this, d] (stems::Quality q)
        {
            stemQuality = q;
            settings.setValue (stemQualityKey, (int) q);
            (d == 0 ? deckB : deckA).setStemQuality (q);
            for (int i = 0; i < 2; ++i)
                if (engine.getDeck (i).hasTrackLoaded())
                    requestStems (i);
        };
        updateStemStatus (d);
    }
    browser.onPrepareStems = [this] (const std::vector<juce::File>& files) { prepareStems (files); };

    openLibrary();

    setSize (1440, 900);
    startTimerHz (4);
}

MainComponent::~MainComponent()
{
    stopTimer();
    // Dialogs refer to objects owned by this component.
    delete controllerDialog.getComponent();
    delete audioDialog.getComponent();
    stemManager.cancelAll();
    analysis.cancel();
    genreDownload.cancel();
    saveSettings();
    setLookAndFeel (nullptr);
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
}

void MainComponent::saveSettings()
{
    if (auto xml = engine.createDeviceStateXml())
        settings.setValue (audioDeviceKey, xml.get());
    settings.setValue (libraryColumnsKey, browser.getLayoutState());
    settings.saveIfNeeded();
}

//==============================================================================
void MainComponent::openLibrary()
{
    const auto result = library.open (getDataFolder().getChildFile ("library.db"));
    if (result.failed())
    {
        juce::NativeMessageBox::showAsync (juce::MessageBoxOptions()
                                               .withIconType (juce::MessageBoxIconType::WarningIcon)
                                               .withTitle ("Music library unavailable")
                                               .withMessage ("The library database could not be opened:\n" + result.getErrorMessage()
                                                             + "\n\nDJ playback still works, but the library is empty."),
                                           nullptr);
        return;
    }

    if (library.getDatabase().getRecoveryNote().isNotEmpty())
        juce::NativeMessageBox::showAsync (juce::MessageBoxOptions()
                                               .withIconType (juce::MessageBoxIconType::WarningIcon)
                                               .withTitle ("Library database was repaired")
                                               .withMessage (library.getDatabase().getRecoveryNote()),
                                           nullptr);

    createDailyBackup();
}

void MainComponent::createDailyBackup()
{
    const auto folder = getDataFolder().getChildFile ("backups");
    const auto today = folder.getChildFile ("library-" + juce::Time::getCurrentTime().formatted ("%Y-%m-%d") + ".db");

    if (! today.existsAsFile() && library.getTracks().size() > 0)
        library.backup (today);

    auto backups = folder.findChildFiles (juce::File::findFiles, false, "library-*.db");
    std::sort (backups.begin(), backups.end(), [] (const juce::File& a, const juce::File& b)
               { return a.getFileName() > b.getFileName(); });
    for (int i = backupsToKeep; i < backups.size(); ++i)
        backups.getReference (i).deleteFile();
}

void MainComponent::loadIntoDeck (const juce::File& file, int deckIndex)
{
    if (deckIndex < 0)
        deckIndex = ! engine.getDeck (0).isPlaying() ? 0 : (! engine.getDeck (1).isPlaying() ? 1 : 0);

    auto& target = deckIndex == 0 ? deckA : deckB;
    if (target.loadTrack (file))
    {
        // Forget what the previous track showed; updateDeckInfo() pushes the new track's data.
        tracking[(size_t) deckIndex] = {};
        tracking[(size_t) deckIndex].path = file.getFullPathName();
        if (const auto* t = library.findTrackByPath (tracking[(size_t) deckIndex].path))
            tracking[(size_t) deckIndex].trackId = t->id;
        updateDeckInfo (deckIndex);
        updateMatchReferences();   // matching follows the new track at once, not on the next timer tick

        // A new track: drop what was requested for the previous one and start stems for this one.
        stemManager.cancel (stemRequests[(size_t) deckIndex].id);
        stemRequests[(size_t) deckIndex] = {};
        if (autoStems && stemManager.isModelPresent())
            requestStems (deckIndex);
        else
            updateStemStatus (deckIndex);
    }
}

//==============================================================================
juce::File MainComponent::findModelFile()
{
    const juce::String name = stems::ModelInfo::fileName;
    const auto env = juce::SystemStats::getEnvironmentVariable ("PATHBOARD_MODEL_DIR", {});
    juce::Array<juce::File> candidates;
    if (env.isNotEmpty())
        candidates.add (juce::File (env).getChildFile (name));
    candidates.add (getDataFolder().getChildFile ("models").getChildFile (name));
    candidates.add (juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory().getChildFile ("models").getChildFile (name));

    for (const auto& f : candidates)
        if (stems::ModelInfo::isCompleteAndValid (f, false))
            return f;
    return getDataFolder().getChildFile ("models").getChildFile (name); // where a download goes
}

void MainComponent::requestStems (int d)
{
    auto& deck = engine.getDeck (d);
    auto& state = stemRequests[(size_t) d];
    if (! deck.hasTrackLoaded() && ! deck.isLoading())
        return;

    const auto file = deck.getLoadedFile();
    stemManager.cancel (state.id);
    state.path = file.getFullPathName();
    state.message.clear();
    const auto path = state.path;

    state.id = stemManager.request (file, stemQuality, true,
        [this, d, path] (std::shared_ptr<const stems::StemSet> set, const juce::String& error)
        {
            auto& s = stemRequests[(size_t) d];
            // Ignore results for a track that is no longer on the deck.
            if (engine.getDeck (d).getLoadedFile().getFullPathName() != path || s.path != path)
                return;

            if (set != nullptr && error.isEmpty())
            {
                // The deck could still be decoding its track; wait until the audio exists.
                s.haveStems = true;
                s.deliveredQuality = set->quality;
                s.fromCache = stemManager.getProgress (s.id).fromCache;
                engine.getDeck (d).setStems (set);
                (d == 0 ? deckA : deckB).setStemStatus ("STEMS ready", true, false);
            }
            else
            {
                s.message = "STEMS: " + error;
            }
            updateStemStatus (d);
        });
    updateStemStatus (d);
}

void MainComponent::updateStemStatus (int d)
{
    auto& comp = d == 0 ? deckA : deckB;
    auto& state = stemRequests[(size_t) d];
    auto& deck = engine.getDeck (d);

    juce::String text;
    bool ready = deck.hasStems(), busy = false;
    juce::String tip;

    if (modelDownloadProgress >= 0.0f)
    {
        text = "Downloading AI model " + juce::String (juce::roundToInt (modelDownloadProgress * 100.0f)) + "%";
        busy = true;
        ready = false;
    }
    else if (state.id != 0 && ! state.haveStems && state.message.isEmpty())
    {
        const auto p = stemManager.getProgress (state.id);
        if (p.state == stems::JobState::done)
        {
            text = "STEMS ready";
        }
        else
        {
            text = juce::String ("Stems: ") + stems::jobStateName (p.state);
            if (p.state == stems::JobState::separating)
                text << " " << juce::roundToInt (p.fraction * 100.0f) << "%";
            busy = true;
        }
    }
    else if (! state.message.isEmpty())
    {
        text = "Stems failed - click to retry";
        tip = state.message;
    }
    else if (deck.hasStems())
    {
        text = juce::String ("Stems ready - ") + stems::qualityName (state.deliveredQuality) + (state.fromCache ? " (saved)" : "");
        const auto device = stemManager.getDeviceName();
        if (device.isNotEmpty() && ! state.fromCache)
            text << " on " << device;
    }
    else if (! stemManager.isModelPresent())
    {
        text = "Stems: install AI model (370 MB) - click";
        tip = "The stem separation model is not installed. Click to download it (MIT licence, from huggingface.co).";
    }
    else
    {
        text = deck.hasTrackLoaded() ? "Separate stems - click" : "Stems";
    }

    comp.setStemStatus (text, ready, busy);
    comp.setTooltipForStemStatus (tip);
}

void MainComponent::stemStatusClicked (int d)
{
    if (! stemManager.isModelPresent())
    {
        startModelDownload();
        return;
    }
    requestStems (d);
}

void MainComponent::startModelDownload()
{
    if (stemManager.isDownloading())
        return;

    auto* window = new juce::AlertWindow ("Install the AI stem model",
                                          "Stem separation needs a neural network model (HTDemucs, MIT licence, by Meta AI Research).\n\n"
                                          "Download size: about 370 MB from huggingface.co. The file is checked against its published checksum. "
                                          "Continue?",
                                          juce::MessageBoxIconType::QuestionIcon);
    window->addButton ("Download", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    window->enterModalState (true, juce::ModalCallbackFunction::create ([this] (int result)
    {
        if (result != 1)
            return;

        modelDownloadProgress = 0.0f;
        stemManager.setModelFile (findModelFile());
        stemManager.downloadModel (
            [this] (float fraction) { modelDownloadProgress = fraction; updateStemStatus (0); updateStemStatus (1); },
            [this] (const juce::String& error)
            {
                modelDownloadProgress = -1.0f;
                if (error.isNotEmpty())
                {
                    juce::NativeMessageBox::showAsync (juce::MessageBoxOptions()
                                                           .withIconType (juce::MessageBoxIconType::WarningIcon)
                                                           .withTitle ("Model download failed")
                                                           .withMessage (error),
                                                       nullptr);
                }
                else
                {
                    stemManager.setModelFile (findModelFile());
                    for (int d = 0; d < 2; ++d)
                        if (engine.getDeck (d).hasTrackLoaded())
                            requestStems (d);
                }
                updateStemStatus (0);
                updateStemStatus (1);
            });
        updateStemStatus (0);
        updateStemStatus (1);
    }), true);
}

void MainComponent::prepareStems (const std::vector<juce::File>& files)
{
    if (! stemManager.isModelPresent())
    {
        startModelDownload();
        return;
    }
    for (const auto& f : files)
        stemManager.request (f, stemQuality, false, nullptr);
}

// Background stems for the library: a few tracks at a time, only while nothing is playing and the
// analysis is idle, so the audio and the analysis are not disturbed. The cache is filled up to 80 % of
// its limit; older entries are never evicted for this (the cache is least-recently-used only for stems
// that were asked for). Tracks need a fingerprint, i.e. they have been analysed first.
void MainComponent::precomputeStemsStep()
{
    if (! autoPrecomputeStems || ! stemManager.isModelPresent() || stemManager.isDownloading()
        || stemManager.isBusy() || stemManager.getQueueLength() > 0 || analysis.isBusy() || library.isScanning())
        return;
    for (int d = 0; d < 2; ++d)
        if (engine.getDeck (d).isPlaying() || engine.getDeck (d).isLoading())
            return;

    auto& cache = stemManager.getCache();
    if (cache.getUsedBytes() >= stemCacheBytes / 10 * 8)
        return;

    int queued = 0;
    for (const auto& t : library.getTracks())
    {
        if (t.missing || t.fingerprint.isEmpty() || t.analysisVersion <= 0 || stemPrepRequested.count (t.id) != 0)
            continue;
        stemPrepRequested.insert (t.id);
        if (cache.contains (t.fingerprint, stemQuality))
            continue;
        stemManager.request (juce::File (t.path), stemQuality, false, nullptr);
        if (++queued >= 2)
            break;
    }
}

void MainComponent::updateDeckInfo (int d)
{
    auto& tr = tracking[(size_t) d];
    const auto* t = library.findTrack (tr.trackId);
    if (t == nullptr)
        return;

    DeckComponent::LibraryInfo info;
    info.bpm = t->bpm;
    info.firstBeat = t->firstBeat;
    info.downbeat = t->downbeat;
    info.key = t->key;
    info.quality = t->analysisQuality();
    info.variableTempo = t->variableTempo;

    const auto& s = tr.shown;
    if (tr.infoShown && s.bpm == info.bpm && s.firstBeat == info.firstBeat && s.downbeat == info.downbeat
        && s.key == info.key && s.quality == info.quality && s.variableTempo == info.variableTempo)
        return;

    tr.shown = info;
    tr.infoShown = true;
    (d == 0 ? deckA : deckB).setLibraryInfo (info);
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (6);

    auto top = area.removeFromTop (32);
    settingsButton.setBounds (top.removeFromRight (120).reduced (2));
    controllerButton.setBounds (top.removeFromRight (120).reduced (2));
    titleLabel.setBounds (top.removeFromLeft (200));
    statusLabel.setBounds (top.reduced (8, 0));

    area.removeFromTop (4);
    const int waveH = juce::jlimit (60, 120, area.getHeight() / 9);
    waveA.setBounds (area.removeFromTop (waveH));
    area.removeFromTop (3);
    waveB.setBounds (area.removeFromTop (waveH));
    area.removeFromTop (6);

    auto decksRow = area.removeFromTop ((int) (area.getHeight() * 0.58f));
    const int mixerW = juce::jlimit (300, 380, decksRow.getWidth() / 4);
    const int deckW = (decksRow.getWidth() - mixerW) / 2;
    deckA.setBounds (decksRow.removeFromLeft (deckW));
    deckB.setBounds (decksRow.removeFromRight (deckW));
    mixer.setBounds (decksRow);

    area.removeFromTop (4);
    browser.setBounds (area);
}

//==============================================================================
int MainComponent::deckAt (juce::Point<int> p) const
{
    if (waveA.getBounds().contains (p) || deckA.getBounds().contains (p)) return 0;
    if (waveB.getBounds().contains (p) || deckB.getBounds().contains (p)) return 1;
    if (mixer.getBounds().contains (p)) return p.x < mixer.getBounds().getCentreX() ? 0 : 1;
    return -1;
}

bool MainComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1 && deckA.isInterestedInFileDrag (files);
}

void MainComponent::filesDropped (const juce::StringArray& files, int x, int y)
{
    const int deck = deckAt ({ x, y });
    if (deck >= 0)
        loadIntoDeck (juce::File (files[0]), deck);
}

bool MainComponent::isInterestedInDragSource (const SourceDetails& details)
{
    return TrackDrag::isTrackDrag (details.description) && deckAt (details.localPosition) >= 0;
}

void MainComponent::itemDropped (const SourceDetails& details)
{
    const int deck = deckAt (details.localPosition);
    const juce::File file (TrackDrag::getFirstPath (details.description));
    if (deck >= 0 && file.existsAsFile())
        loadIntoDeck (file, deck);
}

//==============================================================================
void MainComponent::trackPlayHistory()
{
    for (int d = 0; d < 2; ++d)
    {
        auto& deck = engine.getDeck (d);
        auto& tr = tracking[(size_t) d];

        const auto path = deck.getLoadedFile().getFullPathName();
        if (path != tr.path || ! deck.hasTrackLoaded())
        {
            tr = {};
            tr.path = deck.hasTrackLoaded() ? path : juce::String();
            if (const auto* t = library.findTrackByPath (tr.path))
                tr.trackId = t->id;
        }

        if (tr.trackId != 0)
            updateDeckInfo (d);

        if (deck.isPlaying() && ! tr.counted && tr.trackId != 0)
        {
            tr.playedSeconds += 0.25;
            if (tr.playedSeconds >= countAsPlayedAfterSeconds)
            {
                tr.counted = true;
                library.recordPlay (tr.trackId);
            }
        }
    }

    browser.setLoadedTracks (tracking[0].trackId, tracking[1].trackId);
    updateMatchReferences();
}

void MainComponent::updateMatchReferences()
{
    std::array<LibraryBrowser::MatchReference, 2> refs;
    for (size_t d = 0; d < 2; ++d)
    {
        auto& deck = engine.getDeck ((int) d);
        refs[d].trackId = deck.hasTrackLoaded() ? tracking[d].trackId : 0;
        refs[d].playing = deck.isPlaying();
        refs[d].playbackBpm = deck.getPlaybackBpm();   // what is audible: original tempo with the pitch applied
    }
    browser.setMatchReferences (refs);
}

//==============================================================================
juce::File MainComponent::findGenreModelFile()
{
    const juce::String name = genre::ModelInfo::fileName;
    juce::Array<juce::File> candidates;
    const auto env = juce::SystemStats::getEnvironmentVariable ("PATHBOARD_MODEL_DIR", {});
    if (env.isNotEmpty())
        candidates.add (juce::File (env).getChildFile (name));
    candidates.add (getDataFolder().getChildFile ("models").getChildFile (name));
    candidates.add (juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory().getChildFile ("models").getChildFile (name));

    for (const auto& f : candidates)
        if (f.existsAsFile() && f.getSize() == genre::ModelInfo::sizeBytes)
            return f;
    return getDataFolder().getChildFile ("models").getChildFile (name);   // where a download goes
}

void MainComponent::detectGenre (const std::vector<juce::int64>& ids)
{
    if (analysis.isGenreModelInstalled())
    {
        analysis.retrySkipped();
        analysis.enqueueGenre (ids);
        return;
    }
    startGenreModelDownload (ids);
}

void MainComponent::startGenreModelDownload (std::vector<juce::int64> thenDetect)
{
    if (genreDownload.isRunning())
        return;

    auto* window = new juce::AlertWindow ("Install the genre model",
                                          "Genre detection needs a neural network model (Discogs-EffNet, by the Music Technology Group, "
                                          "Universitat Pompeu Fabra).\n\n"
                                          "Download: about 18 MB from essentia.upf.edu, checked against its checksum.\n"
                                          "Licence: CC BY-NC-SA 4.0. It may be used for non-commercial purposes only.\n\n"
                                          "Afterwards the genre of all your tracks is detected in the background. Continue?",
                                          juce::MessageBoxIconType::QuestionIcon);
    window->addButton ("Download", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    window->enterModalState (true, juce::ModalCallbackFunction::create ([this, thenDetect] (int result)
    {
        if (result != 1)
            return;

        const auto target = getDataFolder().getChildFile ("models").getChildFile (genre::ModelInfo::fileName);
        genreDownload.start (genre::ModelInfo::downloadUrl, target, genre::ModelInfo::sizeBytes, genre::ModelInfo::sha256,
                             [this] (float f) { statusLabel.setText ("Downloading the genre model " + juce::String (juce::roundToInt (f * 100.0f)) + " %", juce::dontSendNotification); },
                             [this, thenDetect] (const juce::String& error)
                             {
                                 if (error.isNotEmpty())
                                 {
                                     juce::NativeMessageBox::showAsync (juce::MessageBoxOptions()
                                                                            .withIconType (juce::MessageBoxIconType::WarningIcon)
                                                                            .withTitle ("Model download failed")
                                                                            .withMessage (error),
                                                                        nullptr);
                                     return;
                                 }
                                 analysis.setGenreModelFile (findGenreModelFile());
                                 analysis.retrySkipped();
                                 analysis.enqueueGenre (thenDetect);   // the ones the user asked for first
                                 analysis.enqueuePending();            // then everything else that is missing
                             });
    }), true);
}

void MainComponent::timerCallback()
{
    trackPlayHistory();
    updateStemStatus (0);
    updateStemStatus (1);

    // New or changed tracks are analysed in the background once scanning is done (every 2 s check).
    if (++analysisCheckCounter >= 8)
    {
        analysisCheckCounter = 0;
        if (! library.isScanning() && ! analysis.isBusy())
            analysis.enqueuePending();
        precomputeStemsStep();
    }

    auto& dm = engine.getDeviceManager();
    if (auto* device = dm.getCurrentAudioDevice())
    {
        const double latencyMs = (device->getOutputLatencyInSamples() + device->getCurrentBufferSizeSamples())
                                 * 1000.0 / device->getCurrentSampleRate();
        statusLabel.setText (device->getTypeName() + " | " + device->getName()
                                 + " | " + juce::String ((int) device->getCurrentSampleRate()) + " Hz"
                                 + " | buffer " + juce::String (device->getCurrentBufferSizeSamples())
                                 + " (~" + juce::String (latencyMs, 1) + " ms)"
                                 + " | outputs " + juce::String (engine.getMixer().getNumOutputChannels())
                                 + " | CPU " + juce::String (juce::roundToInt (dm.getCpuUsage() * 100.0)) + "%"
                                 + " | dropouts " + juce::String (dm.getXRunCount())
                                 + (analysis.isBusy() ? " | analysing " + juce::String (analysis.getDone() + analysis.getFailed()) + "/" + juce::String (analysis.getQueued()) : juce::String())
                                 + (stemManager.getQueueLength() > 0 ? " | stems queued: " + juce::String (stemManager.getQueueLength()) : juce::String())
                                 + (controller.isConnected() ? " | controller: " + controller.getConnectedName() : juce::String (" | no controller")),
                             juce::dontSendNotification);
    }
    else
    {
        statusLabel.setText ("No audio device - open AUDIO SETUP", juce::dontSendNotification);
    }
}

void MainComponent::showControllerSettings()
{
    if (controllerDialog != nullptr)
    {
        controllerDialog->toFront (true);
        return;
    }

    auto content = std::make_unique<ControllerSettingsComponent> (controller, actionRegistry);
    content->setSize (820, 700);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (content.release());
    options.dialogTitle = "DJ controller";
    options.dialogBackgroundColour = Colours::panel;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.componentToCentreAround = this;
    controllerDialog = options.launchAsync();
}

//==============================================================================
void MainComponent::registerActions()
{
    using Type = ActionDef::Type;

    auto add = [this] (const juce::String& id, const juce::String& group, const juce::String& label, Type type,
                       std::function<void (const ControlEvent&)> handler, std::function<bool()> led = {})
    {
        ActionDef def;
        def.id = id;
        def.group = group;
        def.label = label;
        def.type = type;
        def.handler = std::move (handler);
        def.ledState = std::move (led);
        actionRegistry.add (std::move (def));
    };

    for (int d = 0; d < 2; ++d)
    {
        const juce::String p = "deck" + juce::String (d + 1) + ".";
        const juce::String group = "Deck " + juce::String (d + 1);
        DeckComponent* comp = d == 0 ? &deckA : &deckB;
        DeckEngine* deck = &engine.getDeck (d);
        MixerComponent::Channel* ch = &mixer.getChannel (d);

        add (p + "play", group, "Play / Pause", Type::button, [deck] (const ControlEvent& e) { if (e.pressed) deck->togglePlay(); },
             [deck] { return deck->isPlaying(); });
        add (p + "cue", group, "Cue", Type::button, [deck] (const ControlEvent& e) { if (e.pressed) deck->cueDown(); else deck->cueUp(); });
        add (p + "sync", group, "Sync", Type::button, [comp] (const ControlEvent& e) { if (e.pressed) comp->syncToOtherDeck(); });

        add (p + "loop.4", group, "Loop 4 beats (on/off)", Type::button, [comp] (const ControlEvent& e) { if (e.pressed) comp->triggerLoop (4.0); },
             [deck] { return deck->isLooping(); });
        add (p + "loop.exit", group, "Loop exit", Type::button, [comp] (const ControlEvent& e) { if (e.pressed) comp->exitLoop(); });
        static const double loopBeats[] = { 1.0, 2.0, 4.0, 8.0 };
        for (double beats : loopBeats)
            add (p + "loop." + juce::String ((int) beats) + ".pad", group, "Loop " + juce::String ((int) beats) + " beats (pad)", Type::button,
                 [comp, beats] (const ControlEvent& e) { if (e.pressed) comp->triggerLoop (beats); }, [deck] { return deck->isLooping(); });

        // Jog wheel: touched = scratch, otherwise the top also nudges the pitch like the outer ring.
        add (p + "jog.touch", group, "Jog wheel touch", Type::button, [this, d, comp] (const ControlEvent& e)
             {
                 jogTouched[(size_t) d] = e.pressed;
                 comp->getJogWheel().touch (e.pressed);
             });
        add (p + "jog.turn", group, "Jog wheel (top): scratch / nudge", Type::relative, [this, d, comp] (const ControlEvent& e)
             {
                 const double revolutions = e.value / controllerMapping.jogTicksPerRevolution;
                 if (jogTouched[(size_t) d])
                     comp->getJogWheel().spin (revolutions);
                 else
                     comp->getJogWheel().bend (revolutions * controllerMapping.bendSensitivity * 4.0);
             });
        add (p + "jog.ring", group, "Jog wheel (outer ring): pitch bend", Type::relative, [this, comp] (const ControlEvent& e)
             { comp->getJogWheel().bend (e.value / controllerMapping.jogTicksPerRevolution * controllerMapping.bendSensitivity * 4.0); });

        add (p + "tempo", group, "Tempo fader", Type::absolute, [comp] (const ControlEvent& e) { comp->setPitchNormalised (e.value); });

        add (p + "trim", group, "Trim / gain", Type::absolute, [ch] (const ControlEvent& e) { ch->gain.setValue (-12.0 + 24.0 * e.value, juce::sendNotificationSync); });
        add (p + "eq.high", group, "EQ high", Type::absolute, [ch] (const ControlEvent& e) { ch->high.setValue (e.value * 2.0 - 1.0, juce::sendNotificationSync); });
        add (p + "eq.mid", group, "EQ mid", Type::absolute, [ch] (const ControlEvent& e) { ch->mid.setValue (e.value * 2.0 - 1.0, juce::sendNotificationSync); });
        add (p + "eq.low", group, "EQ low", Type::absolute, [ch] (const ControlEvent& e) { ch->low.setValue (e.value * 2.0 - 1.0, juce::sendNotificationSync); });
        add (p + "filter", group, "Filter", Type::absolute, [ch] (const ControlEvent& e) { ch->filter.setValue (e.value * 2.0 - 1.0, juce::sendNotificationSync); });
        add (p + "fader", group, "Channel fader", Type::absolute, [ch] (const ControlEvent& e) { ch->fader.setValue (e.value, juce::sendNotificationSync); });
        add (p + "headphoneCue", group, "Headphone cue", Type::button, [ch] (const ControlEvent& e) { if (e.pressed) ch->cueButton.triggerClick(); },
             [ch] { return ch->cueButton.getToggleState(); });

        for (int i = 0; i < DeckEngine::numHotCues; ++i)
        {
            add (p + "hotcue." + juce::String (i + 1), group, "Hot cue " + juce::String (i + 1) + " (set / jump)", Type::button,
                 [comp, i] (const ControlEvent& e) { if (e.pressed) comp->triggerHotCue (i, false); }, [deck, i] { return deck->isHotCueSet (i); });
            add (p + "hotcueDelete." + juce::String (i + 1), group, "Hot cue " + juce::String (i + 1) + " delete", Type::button,
                 [comp, i] (const ControlEvent& e) { if (e.pressed) comp->triggerHotCue (i, true); });
        }

        struct StemAction { const char* id; const char* label; DeckComponent::StemButton button; };
        for (const auto& s : { StemAction { "stem.vocal", "Stem: vocals on/off", DeckComponent::stemVocal },
                               StemAction { "stem.instru", "Stem: instrumental on/off", DeckComponent::stemInstrumental },
                               StemAction { "stem.bass", "Stem: bass on/off", DeckComponent::stemBass },
                               StemAction { "stem.drums", "Stem: drums on/off", DeckComponent::stemDrums } })
            add (p + s.id, group, s.label, Type::button, [comp, button = s.button] (const ControlEvent& e) { if (e.pressed) comp->toggleStem (button); },
                 [comp, button = s.button] { return comp->isStemOn (button); });
    }

    add ("mixer.crossfader", "Mixer", "Crossfader", Type::absolute, [this] (const ControlEvent& e) { mixer.getCrossfader().setValue (e.value * 2.0 - 1.0, juce::sendNotificationSync); });
    add ("mixer.master", "Mixer", "Master volume", Type::absolute, [this] (const ControlEvent& e) { mixer.getMasterVolume().setValue (e.value, juce::sendNotificationSync); });
    add ("mixer.headphoneMix", "Mixer", "Headphone cue / master mix", Type::absolute, [this] (const ControlEvent& e) { mixer.getHeadphoneMix().setValue (e.value, juce::sendNotificationSync); });
    add ("mixer.headphoneVolume", "Mixer", "Headphone volume", Type::absolute, [this] (const ControlEvent& e) { mixer.getHeadphoneVolume().setValue (e.value, juce::sendNotificationSync); });

    add ("browser.scroll", "Browser", "Scroll the track list", Type::relative, [this] (const ControlEvent& e) { browser.moveSelection ((int) std::lround (e.value)); });
    add ("browser.press", "Browser", "Load selected track (first free deck)", Type::button, [this] (const ControlEvent& e) { if (e.pressed) browser.loadSelectedToDeck (-1); });
    add ("browser.load.deck1", "Browser", "Load selected track to deck 1", Type::button, [this] (const ControlEvent& e) { if (e.pressed) browser.loadSelectedToDeck (0); });
    add ("browser.load.deck2", "Browser", "Load selected track to deck 2", Type::button, [this] (const ControlEvent& e) { if (e.pressed) browser.loadSelectedToDeck (1); });
}

//==============================================================================
void MainComponent::showAudioSettings()
{
    auto selector = std::make_unique<juce::AudioDeviceSelectorComponent> (
        engine.getDeviceManager(),
        0, 0,   // no inputs
        2, 6,   // master, headphones, booth
        false,  // no MIDI here - controller setup is a separate page
        false,
        true,   // stereo pairs
        false);
    selector->setSize (560, 460);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (selector.release());
    options.dialogTitle = "Audio setup";
    options.dialogBackgroundColour = Colours::panel;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.componentToCentreAround = this;

    if (auto* window = options.launchAsync())
    {
        audioDialog = window;
        // Persist the device choice when the dialog closes.
        struct SaveOnClose : juce::ComponentListener
        {
            explicit SaveOnClose (MainComponent& o) : owner (o) {}
            void componentBeingDeleted (juce::Component& c) override
            {
                c.removeComponentListener (this);
                owner.saveSettings();
                delete this;
            }
            MainComponent& owner;
        };
        window->addComponentListener (new SaveOnClose (*this));
    }
}

} // namespace pathboard
