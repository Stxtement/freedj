#pragma once

#include "Audio/DeckEngine.h"
#include "Decks/JogWheel.h"
#include "Decks/SyncMath.h"
#include "Waveform/WaveformDisplay.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace pathboard
{

class DeckComponent : public juce::Component,
                      public juce::FileDragAndDropTarget,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    DeckComponent (DeckEngine& deck, DeckEngine& otherDeck, int deckIndex);
    ~DeckComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    // Returns true if the load was started (a playing deck is never overwritten).
    bool loadTrack (const juce::File& file);

    // Analysis data known from the library; shown on the deck and used as base tempo and beat grid.
    // quality: -1 = not analysed, 0..100 = how much the automatic result can be trusted.
    struct LibraryInfo
    {
        double bpm = 0.0, firstBeat = 0.0;
        int downbeat = 0;
        juce::String key;
        int quality = -1;
        bool variableTempo = false;
    };
    void setLibraryInfo (const LibraryInfo& info);

    // Called when the user changes BPM or beat grid by hand (tap, grid shift, grid set). The owner stores it.
    std::function<void (double bpm, double firstBeat, int downbeat)> onGridEdited;
    std::function<void (bool enabled)> onAutoSyncChanged;
    void setAutoSync (bool enabled) { autoSyncButton.setToggleState (enabled, juce::dontSendNotification); autoSyncPending = enabled && ! deck.isSynced(); }

    // Stems. The four toggles behave like VirtualDJ's stem buttons: lit = audible.
    //   Vocal = vocals, Instrumental = drums + bass + other, Bass and Drums switch those parts inside the instrumental.
    enum StemButton { stemVocal = 0, stemInstrumental = 1, stemBass = 2, stemDrums = 3 };
    void toggleStem (StemButton which);
    bool isStemOn (StemButton which) const { return stemOn[(size_t) which]; }
    void setStemStatus (const juce::String& text, bool stemsReady, bool busy);
    void setTooltipForStemStatus (const juce::String& tip) { stemStatusButton.setTooltip (tip); }
    stems::Quality getStemQuality() const { return stemQuality; }
    void setStemQuality (stems::Quality q);
    std::function<void()> onStemRequested;                       // user clicked the status button
    std::function<void (stems::Quality)> onStemQualityChanged;

    void shiftGrid (double seconds);
    void setGridHere();      // the current playback position becomes a bar start (beat 1)

    // Actions shared by on-screen buttons, keyboard shortcuts and MIDI.
    void syncToOtherDeck();
    void tapTempo();
    void triggerHotCue (int slot, bool deleteInstead);
    void triggerLoop (double beats);
    void setPitchPercent (double percent);
    void setPitchNormalised (double value01);   // 0..1 across the current pitch range (0.5 = centre)
    void exitLoop();
    void nudgePitchSlider (double deltaPercent);
    JogWheel& getJogWheel() { return jogWheel; }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void updateTrackInfo();
    void cyclePitchRange();
    void openFileChooser();
    static juce::String formatTime (double seconds);

    DeckEngine& deck;
    DeckEngine& otherDeck;
    const int deckIndex;
    const juce::Colour accent;

    juce::Label titleLabel, bpmLabel, origBpmLabel, keyLabel, timeLabel, remainLabel, pitchLabel;
    juce::uint32 syncMessageUntil = 0;
    void showSyncMessage (const juce::String& text);
    static constexpr double maxSyncPercent = 16.0;   // sync never pitches further than this (key would drift)
    WaveformDisplay overview;
    JogWheel jogWheel;

    juce::Slider pitchSlider { juce::Slider::LinearVertical, juce::Slider::NoTextBox };
    juce::TextButton pitchRangeButton, pitchResetButton { "0" };

    juce::TextButton cueButton { "CUE" }, playButton { "PLAY" }, syncButton { "SYNC" }, autoSyncButton { "AUTO" };
    bool autoSyncPending = false;
    juce::TextButton tapButton { "TAP" }, loadButton { "LOAD" };
    juce::TextButton rewindButton { "<<" }, forwardButton { ">>" };
    std::array<juce::TextButton, 4> stemButtons;
    juce::TextButton stemStatusButton { "STEMS" }, stemQualityButton;
    std::array<bool, 4> stemOn { true, true, true, true };
    stems::Quality stemQuality = stems::Quality::balanced;
    bool stemsReady = false;
    void applyStemGains();

    juce::TextButton gridLeftButton { "GRID-" }, gridSetButton { "SET 1" }, gridRightButton { "GRID+" };

    std::array<juce::TextButton, DeckEngine::numHotCues> hotCueButtons;

    static constexpr std::array<double, 6> loopBeatOptions { 0.5, 1.0, 2.0, 4.0, 8.0, 16.0 };
    std::array<juce::TextButton, loopBeatOptions.size()> loopButtons;
    juce::TextButton loopExitButton { "EXIT" };

    std::unique_ptr<juce::FileChooser> fileChooser;

    double pitchRangePercent = 8.0;
    juce::Array<double> tapTimes;
    double activeLoopBeats = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeckComponent)
};

} // namespace pathboard
