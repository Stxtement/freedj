#pragma once

#include "Analysis/AnalysisManager.h"
#include "Library/LibraryManager.h"
#include "Library/SmartMatch.h"
#include "Library/TrackInfoPanel.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace pathboard
{

// Drag-and-drop payload created by the song list: { kind: "pdj-tracks", ids: [..], firstPath: "..." }
namespace TrackDrag
{
    bool isTrackDrag (const juce::var& description);
    juce::String getFirstPath (const juce::var& description);
    std::vector<juce::int64> getIds (const juce::var& description);
}

// Music browser: sidebar (library views, folders, playlists), searchable/sortable song list for
// tens of thousands of tracks (virtualised rows over the in-memory cache) and a details pane.
class LibraryBrowser : public juce::Component,
                       private juce::TableListBoxModel,
                       private juce::ChangeListener,
                       private juce::Timer
{
public:
    explicit LibraryBrowser (LibraryManager& library);
    ~LibraryBrowser() override;

    // deckIndex -1 = first free deck.
    std::function<void (const juce::File& file, int deckIndex)> onLoadRequest;

    void setAnalysisManager (AnalysisManager* manager);

    // Smart Matching: tracks that fit what is on the decks are marked green (very good) or blue (good) in the list.
    // Only uses values that are known; filters and sorting are not touched.
    enum class MatchMode { automatic = 0, deck1 = 1, deck2 = 2, both = 3 };
    struct MatchReference
    {
        juce::int64 trackId = 0;      // library id of the track on the deck, 0 = empty deck (or not in the library)
        double playbackBpm = 0.0;     // audible tempo (with pitch applied), 0 = unknown
        bool playing = false;
    };
    void setMatchReferences (const std::array<MatchReference, 2>& refs);
    void setSmartMatch (bool enabled, MatchMode mode);
    bool isSmartMatchEnabled() const { return smartMatchEnabled; }
    MatchMode getMatchMode() const { return matchMode; }
    std::function<void()> onSmartMatchSettingsChanged;

    // Asked to detect the genre of these tracks (the owner installs the model first if needed).
    std::function<void (const std::vector<juce::int64>&)> onDetectGenre;

    // Controller / keyboard navigation.
    void moveSelection (int rowsDown);          // negative = up
    void loadSelectedToDeck (int deckIndex);    // -1 = first free deck

    // Asked to prepare (separate and cache) stems for these files in the background.
    std::function<void (const std::vector<juce::File>&)> onPrepareStems;

    // Highlights the tracks currently loaded on the decks (0 = none).
    void setLoadedTracks (juce::int64 deck1, juce::int64 deck2);

    juce::String getLayoutState() const;
    void setLayoutState (const juce::String& state);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Sidebar;
    enum class View { allSongs, favorites, recentlyPlayed, history, missing, duplicates, folder, playlist };
    struct ViewSelection
    {
        View view = View::allSongs;
        juce::int64 playlistId = 0;
        juce::String folder;
    };

    // TableListBoxModel
    int getNumRows() override { return (int) rows.size(); }
    void paintRowBackground (juce::Graphics&, int row, int width, int height, bool selected) override;
    void paintCell (juce::Graphics&, int row, int column, int width, int height, bool selected) override;
    void sortOrderChanged (int newSortColumnId, bool isForwards) override;
    void selectedRowsChanged (int lastRowSelected) override;
    void cellDoubleClicked (int row, int column, const juce::MouseEvent&) override;
    void cellClicked (int row, int column, const juce::MouseEvent&) override;
    void backgroundClicked (const juce::MouseEvent&) override;
    juce::var getDragSourceDescription (const juce::SparseSet<int>& selectedRows) override;
    void deleteKeyPressed (int lastRowSelected) override;
    juce::String getCellTooltip (int row, int column) override;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    void selectView (const ViewSelection&);
    void refresh();
    void rebuildSidebarIfNeeded();
    const TrackRecord* trackAt (int row) const;
    std::vector<juce::int64> getSelectedIds() const;
    PlaylistInfo findPlaylist (juce::int64 id) const;

    void rebuildMatchSubjects();
    MatchLevel getMatchLevel (int row) const;
    MatchDetail getMatchDetail (int row) const;

    void showContextMenu (int row);
    void loadTrack (const TrackRecord& track, int deckIndex);
    void addFolder();
    void createPlaylistDialog (bool smart);
    void renameFile (const TrackRecord& track);
    void moveOrCopyFiles (const std::vector<juce::int64>& ids, bool move);
    void locateFile (const TrackRecord& track);
    void saveTrackEdits (const TrackRecord& edited, bool writeToFile);
    void updateStatus();
    void dropTracksOnPlaylist (juce::int64 playlistId, const juce::var& description);

    LibraryManager& library;
    AnalysisManager* analysis = nullptr;

    bool smartMatchEnabled = false;
    MatchMode matchMode = MatchMode::automatic;
    std::array<MatchReference, 2> matchRefs;
    int lastLoadedDeck = -1;
    std::vector<MatchSubject> matchSubjects;
    std::vector<juce::int64> matchReferenceIds;
    mutable std::vector<signed char> rowMatchCache;   // per row: -1 not computed, else MatchLevel
    juce::ToggleButton smartMatchToggle { "Smart Match" };
    juce::ComboBox matchModeBox;
    juce::ComboBox matchFilterBox;   // list filter: all / good+very good / very good only
    int matchFilterLevel = 0;        // 0 = off, 1 = only matches, 2 = only very good matches
    ViewSelection currentView;
    std::vector<int> rows;
    QuerySpec spec;
    juce::int64 loadedOnDeck[2] { 0, 0 };

    std::unique_ptr<Sidebar> sidebar;
    juce::TreeView sidebarTree;
    juce::String sidebarSignature;

    juce::TextEditor searchBox, bpmFrom, bpmTo, keyFilter;
    juce::ComboBox ratingFilter;
    juce::TextButton addFolderButton { "+ Folder" }, rescanButton { "Rescan" }, analyseButton { "Analyse" }, newPlaylistButton { "+ Playlist" };
    juce::Label statusLabel;
    juce::TableListBox table { "tracks", this };
    TrackInfoPanel infoPanel;

    juce::StretchableLayoutManager layout;
    juce::StretchableLayoutResizerBar resizerLeft { &layout, 1, true }, resizerRight { &layout, 3, true };
    std::unique_ptr<juce::FileChooser> chooser;
    bool refreshPending = false;

    JUCE_DECLARE_WEAK_REFERENCEABLE (LibraryBrowser)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LibraryBrowser)
};

} // namespace pathboard
