#include "DeckComponent.h"
#include "UI/DJLookAndFeel.h"

namespace pathboard
{

namespace
{
    constexpr double fallbackBpm = 120.0;

    const juce::StringArray& supportedExtensions()
    {
        static const juce::StringArray exts { ".mp3", ".wav", ".flac", ".aif", ".aiff", ".ogg",
                                              ".m4a", ".aac", ".wma", ".mp4" };
        return exts;
    }

    bool isAudioFile (const juce::File& f)
    {
        return supportedExtensions().contains (f.getFileExtension().toLowerCase());
    }

    juce::Colour hotCueColour (int slot)
    {
        return juce::Colour::fromHSV ((float) slot / (float) DeckEngine::numHotCues, 0.75f, 1.0f, 1.0f);
    }
}

DeckComponent::DeckComponent (DeckEngine& d, DeckEngine& other, int index)
    : deck (d), otherDeck (other), deckIndex (index), accent (Colours::forDeck (index)),
      overview (d, index, WaveformDisplay::Mode::overview),
      jogWheel (d, index)
{
    deck.addChangeListener (this);

    titleLabel.setFont (juce::FontOptions (17.0f, juce::Font::bold));
    titleLabel.setText ("Deck " + juce::String (deckIndex + 1) + " - empty", juce::dontSendNotification);
    bpmLabel.setFont (juce::FontOptions (26.0f, juce::Font::bold));
    bpmLabel.setJustificationType (juce::Justification::centredRight);
    bpmLabel.setColour (juce::Label::textColourId, accent);
    keyLabel.setJustificationType (juce::Justification::centredRight);
    keyLabel.setColour (juce::Label::textColourId, Colours::textDim);
    keyLabel.setText ("KEY --", juce::dontSendNotification);
    keyLabel.setTooltip ("Automatic key detection arrives with the analysis engine (phase 4).");
    timeLabel.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    remainLabel.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    remainLabel.setJustificationType (juce::Justification::centredRight);
    remainLabel.setColour (juce::Label::textColourId, Colours::textDim);
    pitchLabel.setJustificationType (juce::Justification::centred);
    pitchLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));

    origBpmLabel.setJustificationType (juce::Justification::centredRight);
    origBpmLabel.setFont (juce::FontOptions (12.0f));
    origBpmLabel.setTooltip ("Original tempo of the track (from the library). The big number above is the playback tempo with the pitch fader applied.");

    for (auto* l : { &titleLabel, &bpmLabel, &origBpmLabel, &keyLabel, &timeLabel, &remainLabel, &pitchLabel })
        addAndMakeVisible (l);

    addAndMakeVisible (overview);
    addAndMakeVisible (jogWheel);

    pitchSlider.setRange (-pitchRangePercent, pitchRangePercent, 0.01);
    pitchSlider.setValue (0.0, juce::dontSendNotification);
    pitchSlider.setDoubleClickReturnValue (true, 0.0);
    pitchSlider.setColour (juce::Slider::rotarySliderFillColourId, accent);
    pitchSlider.onValueChange = [this] { deck.setPitch (pitchSlider.getValue() / 100.0); };
    addAndMakeVisible (pitchSlider);

    pitchRangeButton.setButtonText ("8%");
    pitchRangeButton.setTooltip ("Pitch fader range");
    pitchRangeButton.onClick = [this] { cyclePitchRange(); };
    pitchResetButton.setTooltip ("Reset pitch to 0%");
    pitchResetButton.onClick = [this] { setPitchPercent (0.0); };
    addAndMakeVisible (pitchRangeButton);
    addAndMakeVisible (pitchResetButton);

    // CUE needs press *and* release (preview while held), so it listens to state changes.
    cueButton.setColour (juce::TextButton::buttonColourId, Colours::cue.darker (0.6f));
    cueButton.onStateChange = [this]
    {
        const bool down = cueButton.isDown();
        if (down != cueButton.getToggleState())
        {
            cueButton.setToggleState (down, juce::dontSendNotification);
            if (down) deck.cueDown(); else deck.cueUp();
        }
    };
    playButton.setColour (juce::TextButton::buttonColourId, Colours::play.darker (0.6f));
    playButton.setColour (juce::TextButton::buttonOnColourId, Colours::play);
    playButton.onClick = [this] { deck.togglePlay(); };
    syncButton.setTooltip ("Match this deck's tempo to the other deck (needs BPM on both decks).");
    syncButton.onClick = [this] { syncToOtherDeck(); };
    tapButton.setTooltip ("Tap along with the beat to set the BPM.");
    tapButton.onClick = [this] { tapTempo(); };
    loadButton.onClick = [this] { openFileChooser(); };

    rewindButton.setRepeatSpeed (250, 60);
    forwardButton.setRepeatSpeed (250, 60);
    rewindButton.onClick = [this] { deck.seekRelativeSeconds (-0.5); };
    forwardButton.onClick = [this] { deck.seekRelativeSeconds (0.5); };

    gridLeftButton.setTooltip ("Move the beat grid 10 ms earlier (hold to repeat). Stored as a manual correction.");
    gridRightButton.setTooltip ("Move the beat grid 10 ms later (hold to repeat). Stored as a manual correction.");
    gridSetButton.setTooltip ("Make the current playback position beat 1 of a bar. Stored as a manual correction.");
    gridLeftButton.setRepeatSpeed (400, 60);
    gridRightButton.setRepeatSpeed (400, 60);
    gridLeftButton.onClick = [this] { shiftGrid (-0.010); };
    gridRightButton.onClick = [this] { shiftGrid (0.010); };
    gridSetButton.onClick = [this] { setGridHere(); };

    for (auto* b : { &cueButton, &playButton, &syncButton, &tapButton, &loadButton, &rewindButton, &forwardButton,
                     &gridLeftButton, &gridSetButton, &gridRightButton })
        addAndMakeVisible (b);

    static const char* stemNames[] = { "VOCAL", "INSTRU", "BASS", "DRUMS" };
    static const char* stemTips[] = {
        "Vocals on/off. Off = the vocal stem is exactly silent and the original track is not playing underneath.",
        "Instrumental on/off (everything except vocals). Off = acapella.",
        "Bass on/off.",
        "Drums on/off." };
    for (int i = 0; i < 4; ++i)
    {
        auto& b = stemButtons[(size_t) i];
        b.setButtonText (stemNames[i]);
        b.setTooltip (juce::String (stemTips[i]) + "\nNeeds separated stems (see the status button).");
        b.setEnabled (false);
        b.onClick = [this, i] { toggleStem ((StemButton) i); };
        addAndMakeVisible (b);
    }
    stemStatusButton.onClick = [this] { if (onStemRequested) onStemRequested(); };
    stemQualityButton.setTooltip ("Separation quality: FAST (quick preview), BALANCED, HIGH (best, slowest).\nStems that exist in the cache are reused.");
    stemQualityButton.onClick = [this]
    {
        setStemQuality ((stems::Quality) (((int) stemQuality + 1) % 3));
        if (onStemQualityChanged)
            onStemQualityChanged (stemQuality);
    };
    setStemQuality (stems::Quality::balanced);
    addAndMakeVisible (stemStatusButton);
    addAndMakeVisible (stemQualityButton);

    for (int i = 0; i < DeckEngine::numHotCues; ++i)
    {
        auto& b = hotCueButtons[(size_t) i];
        b.setButtonText (juce::String (i + 1));
        b.setTooltip ("Click: set / jump to hot cue. Shift+Click: delete.");
        b.onClick = [this, i] { triggerHotCue (i, juce::ModifierKeys::getCurrentModifiers().isShiftDown()); };
        addAndMakeVisible (b);
    }

    for (size_t i = 0; i < loopBeatOptions.size(); ++i)
    {
        auto& b = loopButtons[i];
        const double beats = loopBeatOptions[i];
        b.setButtonText (beats < 1.0 ? "1/2" : juce::String ((int) beats));
        b.setTooltip ("Loop " + b.getButtonText() + " beat(s). Uses the deck BPM, or 120 BPM if none is set.");
        b.onClick = [this, beats] { triggerLoop (beats); };
        addAndMakeVisible (b);
    }
    loopExitButton.onClick = [this] { deck.exitLoop(); activeLoopBeats = 0.0; };
    addAndMakeVisible (loopExitButton);

    startTimerHz (30);
}

DeckComponent::~DeckComponent()
{
    deck.removeChangeListener (this);
}

//==============================================================================
void DeckComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (Colours::panel);
    g.fillRoundedRectangle (bounds.reduced (2.0f), 6.0f);
    g.setColour (accent.withAlpha (0.6f));
    g.fillRoundedRectangle (bounds.reduced (2.0f).removeFromTop (3.0f), 1.5f);
}

void DeckComponent::resized()
{
    auto area = getLocalBounds().reduced (10);
    area.removeFromTop (4);

    auto header = area.removeFromTop (26);
    bpmLabel.setBounds (header.removeFromRight (110));
    titleLabel.setBounds (header);

    auto subHeader = area.removeFromTop (22);
    keyLabel.setBounds (subHeader.removeFromRight (110));
    origBpmLabel.setBounds (subHeader.removeFromRight (170));
    timeLabel.setBounds (subHeader.removeFromLeft (subHeader.getWidth() / 2));
    remainLabel.setBounds (subHeader);

    area.removeFromTop (4);
    overview.setBounds (area.removeFromTop (44));
    area.removeFromTop (8);

    auto loopRow = area.removeFromBottom (30);
    area.removeFromBottom (4);
    auto cueRow = area.removeFromBottom (32);
    area.removeFromBottom (4);
    auto stemRow = area.removeFromBottom (30);
    area.removeFromBottom (6);
    auto transportRow = area.removeFromBottom (44);

    // Stem row: four toggles, quality, status/progress
    for (auto& b : stemButtons)
        b.setBounds (stemRow.removeFromLeft (76).reduced (2, 0));
    stemQualityButton.setBounds (stemRow.removeFromLeft (92).reduced (2, 0));
    stemStatusButton.setBounds (stemRow.reduced (2, 0));
    area.removeFromBottom (8);

    // Loop row
    const int loopW = loopRow.getWidth() / ((int) loopButtons.size() + 1);
    for (auto& b : loopButtons)
        b.setBounds (loopRow.removeFromLeft (loopW).reduced (2, 0));
    loopExitButton.setBounds (loopRow.reduced (2, 0));

    // Hot cue row
    const int cueW = cueRow.getWidth() / DeckEngine::numHotCues;
    for (auto& b : hotCueButtons)
        b.setBounds (cueRow.removeFromLeft (cueW).reduced (2, 0));

    // Transport row
    const int tw = transportRow.getWidth() / 7;
    cueButton.setBounds (transportRow.removeFromLeft (tw * 2).reduced (2, 0));
    playButton.setBounds (transportRow.removeFromLeft (tw * 2).reduced (2, 0));
    syncButton.setBounds (transportRow.removeFromLeft (tw).reduced (2, 0));
    tapButton.setBounds (transportRow.removeFromLeft (tw).reduced (2, 0));
    loadButton.setBounds (transportRow.reduced (2, 0));

    // Middle: jog + pitch fader
    auto pitchArea = deckIndex == 0 ? area.removeFromRight (56) : area.removeFromLeft (56);
    pitchRangeButton.setBounds (pitchArea.removeFromTop (22).reduced (2, 0));
    pitchArea.removeFromTop (2);
    pitchResetButton.setBounds (pitchArea.removeFromBottom (22).reduced (2, 0));
    pitchLabel.setBounds (pitchArea.removeFromBottom (18));
    pitchSlider.setBounds (pitchArea);

    auto searchRow = area.removeFromBottom (26);
    rewindButton.setBounds (searchRow.removeFromLeft (50));
    forwardButton.setBounds (searchRow.removeFromRight (50));
    auto gridRow = searchRow.withSizeKeepingCentre (juce::jmin (searchRow.getWidth(), 240), searchRow.getHeight());
    gridLeftButton.setBounds (gridRow.removeFromLeft (gridRow.getWidth() / 3).reduced (2, 0));
    gridRightButton.setBounds (gridRow.removeFromRight (gridRow.getWidth() / 2).reduced (2, 0));
    gridSetButton.setBounds (gridRow.reduced (2, 0));

    jogWheel.setBounds (area.reduced (4));
}

//==============================================================================
bool DeckComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1 && isAudioFile (juce::File (files[0]));
}

void DeckComponent::filesDropped (const juce::StringArray& files, int, int)
{
    if (! files.isEmpty())
        loadTrack (juce::File (files[0]));
}

void DeckComponent::setLibraryInfo (const LibraryInfo& info)
{
    if (info.bpm > 0.0)
    {
        deck.setNativeBpm (info.bpm);
        deck.setBeatGrid (info.firstBeat, info.downbeat);
    }

    keyLabel.setText ("KEY " + (info.key.isNotEmpty() ? info.key : juce::String ("--")), juce::dontSendNotification);

    juce::String tip;
    if (info.quality < 0)
        tip = "Not analysed yet.";
    else
        tip = "Analysis confidence " + juce::String (info.quality) + "%"
              + (info.quality < 50 ? " - please check the BPM (tap or use the grid buttons)." : ".");
    if (info.variableTempo)
        tip << "\nThis track changes tempo; one beat grid cannot follow it everywhere.";
    bpmLabel.setTooltip (tip);
    bpmLabel.setColour (juce::Label::textColourId, info.quality >= 0 && info.quality < 50 ? Colours::cue : accent);
}

void DeckComponent::applyStemGains()
{
    // Vocal = vocals; Instrumental = drums + bass + other; Bass and Drums refine the instrumental.
    const bool instru = stemOn[stemInstrumental];
    std::array<float, stems::numStems> gains {};
    gains[(size_t) stems::Stem::vocals] = stemOn[stemVocal] ? 1.0f : 0.0f;
    gains[(size_t) stems::Stem::other]  = instru ? 1.0f : 0.0f;
    gains[(size_t) stems::Stem::bass]   = instru && stemOn[stemBass] ? 1.0f : 0.0f;
    gains[(size_t) stems::Stem::drums]  = instru && stemOn[stemDrums] ? 1.0f : 0.0f;
    deck.setStemGains (gains);
}

void DeckComponent::toggleStem (StemButton which)
{
    if (! deck.hasStems())
        return;
    stemOn[(size_t) which] = ! stemOn[(size_t) which];
    applyStemGains();
}

void DeckComponent::setStemQuality (stems::Quality q)
{
    stemQuality = q;
    stemQualityButton.setButtonText (stems::qualityName (q));
}

void DeckComponent::setStemStatus (const juce::String& text, bool ready, bool busy)
{
    stemStatusButton.setButtonText (text);
    stemStatusButton.setEnabled (! busy);
    stemsReady = ready;
    for (auto& b : stemButtons)
        b.setEnabled (ready);
}

void DeckComponent::shiftGrid (double seconds)
{
    const double bpm = deck.getNativeBpm();
    if (bpm <= 0.0)
        return;
    const double period = 60.0 / bpm;
    double first = std::fmod (deck.getGridFirstBeat() + seconds, period);
    if (first < 0.0)
        first += period;
    deck.setBeatGrid (first, deck.getGridDownbeat());
    if (onGridEdited)
        onGridEdited (bpm, first, deck.getGridDownbeat());
}

void DeckComponent::setGridHere()
{
    const double bpm = deck.getNativeBpm();
    if (bpm <= 0.0 || ! deck.hasTrackLoaded())
        return;
    const double period = 60.0 / bpm;
    const double position = deck.getCurrentPositionSeconds();
    // The anchor is the first beat of the track; the current position is a bar start.
    const double first = std::fmod (position, period);
    const int beatsBefore = (int) std::lround ((position - first) / period);
    const int downbeat = ((beatsBefore % 4) + 4) % 4;
    deck.setBeatGrid (first, downbeat);
    if (onGridEdited)
        onGridEdited (bpm, first, downbeat);
}

bool DeckComponent::loadTrack (const juce::File& file)
{
    if (deck.isPlaying())
    {
        // Protect the live mix: never replace a playing track silently.
        juce::NativeMessageBox::showAsync (juce::MessageBoxOptions()
                                               .withIconType (juce::MessageBoxIconType::WarningIcon)
                                               .withTitle ("Deck " + juce::String (deckIndex + 1) + " is playing")
                                               .withMessage ("Stop the deck before loading a new track."),
                                           nullptr);
        return false;
    }

    keyLabel.setText ("KEY --", juce::dontSendNotification);
    activeLoopBeats = 0.0;
    tapTimes.clear();
    stemOn.fill (true);
    setStemStatus ("STEMS", false, false);
    const bool started = deck.loadFile (file);
    if (! started)
        juce::NativeMessageBox::showAsync (juce::MessageBoxOptions()
                                               .withIconType (juce::MessageBoxIconType::WarningIcon)
                                               .withTitle ("Could not load track")
                                               .withMessage (deck.getLastError()),
                                           nullptr);
    updateTrackInfo();
    return started;
}

void DeckComponent::openFileChooser()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Load track into deck " + juce::String (deckIndex + 1),
                                                       juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                       "*" + supportedExtensions().joinIntoString (";*"));

    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
                              {
                                  const auto result = fc.getResult();
                                  if (result.existsAsFile())
                                      loadTrack (result);
                              });
}

//==============================================================================
void DeckComponent::syncToOtherDeck()
{
    // Own ORIGINAL tempo against the other deck's PLAYBACK tempo (what is audible), half/double-time aware.
    const auto plan = planSync (deck.getOriginalBpm(), otherDeck.getPlaybackBpm(), maxSyncPercent);

    if (! plan.possible)
    {
        showSyncMessage ("NO BPM");
        syncButton.setTooltip ("Sync needs a BPM on both decks - analyse the tracks or use TAP.");
        return;
    }

    if (! plan.withinLimit)
    {
        // A pitch change this large would also shift the key by several semitones: do not do it silently.
        showSyncMessage ("TOO FAR");
        syncButton.setTooltip ("Sync would need " + juce::String (plan.percent, 1) + " % pitch (limit +/-"
                               + juce::String ((int) maxSyncPercent) + " %). The tracks are too far apart in tempo.");
        return;
    }

    while (std::abs (plan.percent) > pitchRangePercent && pitchRangePercent < maxSyncPercent)
        cyclePitchRange();
    setPitchPercent (plan.percent);

    syncButton.setTooltip (plan.targetFactor == 1.0 ? juce::String ("Matched to the other deck.")
                                                    : "Matched in " + juce::String (plan.targetFactor == 0.5 ? "half" : "double") + "-time to the other deck.");
}

void DeckComponent::showSyncMessage (const juce::String& text)
{
    syncButton.setButtonText (text);
    syncMessageUntil = juce::Time::getMillisecondCounter() + 1800;
}

void DeckComponent::tapTempo()
{
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    if (! tapTimes.isEmpty() && now - tapTimes.getLast() > 2.0)
        tapTimes.clear();

    tapTimes.add (now);
    while (tapTimes.size() > 9)
        tapTimes.remove (0);

    if (tapTimes.size() >= 3)
    {
        const double avgInterval = (tapTimes.getLast() - tapTimes.getFirst()) / (tapTimes.size() - 1);
        const double tappedBpm = 60.0 / avgInterval;
        // The tapped tempo includes the current pitch; store the track's native tempo.
        const double native = std::round (tappedBpm / (1.0 + deck.getPitch()) * 100.0) / 100.0;
        deck.setNativeBpm (native);
        if (onGridEdited)
            onGridEdited (native, deck.getGridFirstBeat(), deck.getGridDownbeat());
    }
}

void DeckComponent::triggerHotCue (int slot, bool deleteInstead)
{
    if (deleteInstead)
        deck.clearHotCue (slot);
    else if (deck.isHotCueSet (slot))
        deck.jumpToHotCue (slot);
    else
        deck.setHotCue (slot);
}

void DeckComponent::triggerLoop (double beats)
{
    const bool sameLoop = deck.isLooping() && std::abs (activeLoopBeats - beats) < 1.0e-6;
    if (deck.isLooping())
        deck.exitLoop();

    if (sameLoop)
    {
        activeLoopBeats = 0.0;
        return;
    }

    const double bpm = deck.getNativeBpm() > 0.0 ? deck.getNativeBpm() : fallbackBpm;
    deck.toggleLoop (beats * 60.0 / bpm);
    activeLoopBeats = deck.isLooping() ? beats : 0.0;
}

void DeckComponent::setPitchPercent (double percent)
{
    pitchSlider.setValue (juce::jlimit (-pitchRangePercent, pitchRangePercent, percent), juce::sendNotificationSync);
}

void DeckComponent::setPitchNormalised (double value01)
{
    setPitchPercent ((juce::jlimit (0.0, 1.0, value01) * 2.0 - 1.0) * pitchRangePercent);
}

void DeckComponent::exitLoop()
{
    deck.exitLoop();
    activeLoopBeats = 0.0;
}

void DeckComponent::nudgePitchSlider (double deltaPercent)
{
    setPitchPercent (pitchSlider.getValue() + deltaPercent);
}

void DeckComponent::cyclePitchRange()
{
    pitchRangePercent = pitchRangePercent < 10.0 ? 16.0 : (pitchRangePercent < 20.0 ? 50.0 : 8.0);
    const double current = pitchSlider.getValue();
    pitchSlider.setRange (-pitchRangePercent, pitchRangePercent, 0.01);
    pitchSlider.setValue (juce::jlimit (-pitchRangePercent, pitchRangePercent, current), juce::sendNotificationSync);
    pitchRangeButton.setButtonText (juce::String ((int) pitchRangePercent) + "%");
}

//==============================================================================
void DeckComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    updateTrackInfo();
}

void DeckComponent::updateTrackInfo()
{
    const auto file = deck.getLoadedFile();
    const auto prefix = juce::String (deckIndex + 1) + "  ";

    if (deck.isLoading())
        titleLabel.setText (prefix + "Loading " + file.getFileNameWithoutExtension() + "...", juce::dontSendNotification);
    else if (deck.getLastError().isNotEmpty())
        titleLabel.setText (prefix + "Load failed", juce::dontSendNotification);
    else if (deck.hasTrackLoaded())
        titleLabel.setText (prefix + file.getFileNameWithoutExtension(), juce::dontSendNotification);
    else
        titleLabel.setText (prefix + "empty", juce::dontSendNotification);

    titleLabel.setTooltip (file.getFullPathName());
}

juce::String DeckComponent::formatTime (double seconds)
{
    seconds = juce::jmax (0.0, seconds);
    const int mins = (int) (seconds / 60.0);
    const double secs = seconds - mins * 60.0;
    return juce::String (mins).paddedLeft ('0', 2) + ":" + juce::String (secs, 1).paddedLeft ('0', 4);
}

void DeckComponent::timerCallback()
{
    const double pos = deck.getCurrentPositionSeconds();
    const double len = deck.getLengthSeconds();
    const bool loaded = deck.hasTrackLoaded();

    timeLabel.setText (loaded ? formatTime (pos) : "--:--.-", juce::dontSendNotification);
    remainLabel.setText (loaded ? "-" + formatTime (len - pos) : "--:--.-", juce::dontSendNotification);

    // Flash the remaining time in the last 30 seconds.
    const bool warn = loaded && len - pos < 30.0 && deck.isPlaying()
                      && (juce::Time::getMillisecondCounter() / 400) % 2 == 0;
    remainLabel.setColour (juce::Label::textColourId, warn ? Colours::danger : Colours::textDim);

    // Big number: what is audible (original tempo with the pitch applied). Small line: the track's own tempo.
    const double playbackBpm = deck.getPlaybackBpm();
    const double originalBpm = deck.getOriginalBpm();
    const bool pitched = originalBpm > 0.0 && std::abs (deck.getPitch()) > 0.00005;
    bpmLabel.setText (playbackBpm > 0.0 ? juce::String (playbackBpm, 1) : "--.-", juce::dontSendNotification);
    origBpmLabel.setText (originalBpm > 0.0 ? "ORIG " + juce::String (originalBpm, 2)
                                                  + (pitched ? "   " + juce::String (deck.getPitch() * 100.0, 2).replace ("-", juce::String::charToString (0x2212)) + " %" : juce::String())
                                            : juce::String(),
                          juce::dontSendNotification);
    origBpmLabel.setColour (juce::Label::textColourId, pitched ? Colours::cue : Colours::textDim);
    if (syncMessageUntil != 0 && juce::Time::getMillisecondCounter() > syncMessageUntil)
    {
        syncMessageUntil = 0;
        syncButton.setButtonText ("SYNC");
    }

    const double pitchPct = deck.getPitch() * 100.0;
    pitchLabel.setText ((pitchPct >= 0 ? "+" : "") + juce::String (pitchPct, 2) + "%", juce::dontSendNotification);

    playButton.setToggleState (deck.isPlaying(), juce::dontSendNotification);
    playButton.setButtonText (deck.isPlaying() ? "PAUSE" : "PLAY");

    for (int i = 0; i < DeckEngine::numHotCues; ++i)
    {
        auto& b = hotCueButtons[(size_t) i];
        const bool set = deck.isHotCueSet (i);
        b.setColour (juce::TextButton::buttonColourId, set ? hotCueColour (i).darker (0.3f) : Colours::panelRaised);
        b.setColour (juce::TextButton::textColourOffId, set ? juce::Colours::black : Colours::text);
    }

    if (! deck.isLooping())
        activeLoopBeats = 0.0;

    for (size_t i = 0; i < loopButtons.size(); ++i)
    {
        const bool active = deck.isLooping() && std::abs (activeLoopBeats - loopBeatOptions[i]) < 1.0e-6;
        loopButtons[i].setColour (juce::TextButton::buttonColourId, active ? Colours::play.darker (0.2f) : Colours::panelRaised);
    }

    for (int i = 0; i < 4; ++i)
    {
        // Bass and drums sit inside the instrumental: with it off they are silent whatever their own state is.
        const bool on = stemOn[(size_t) i] && (i != stemBass && i != stemDrums ? true : stemOn[stemInstrumental]);
        stemButtons[(size_t) i].setColour (juce::TextButton::buttonColourId, ! stemsReady ? Colours::panelRaised.darker (0.3f)
                                                                              : on ? accent.darker (0.2f) : Colours::panelRaised);
        stemButtons[(size_t) i].setColour (juce::TextButton::textColourOffId, stemsReady && on ? juce::Colours::black : Colours::textDim);
    }

    syncButton.setEnabled (deck.getNativeBpm() > 0.0 && otherDeck.getNativeBpm() > 0.0);
}

} // namespace pathboard
