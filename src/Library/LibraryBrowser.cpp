#include "LibraryBrowser.h"
#include "Library/TrackMetadata.h"
#include "UI/DJLookAndFeel.h"
#include <algorithm>
#include <map>

namespace pathboard
{

//==============================================================================
namespace TrackDrag
{
    bool isTrackDrag (const juce::var& d)
    {
        return d.getProperty ("kind", {}).toString() == "pdj-tracks";
    }

    juce::String getFirstPath (const juce::var& d) { return d.getProperty ("firstPath", {}).toString(); }

    std::vector<juce::int64> getIds (const juce::var& d)
    {
        std::vector<juce::int64> ids;
        if (auto* a = d.getProperty ("ids", {}).getArray())
            for (const auto& v : *a)
                ids.push_back ((juce::int64) v);
        return ids;
    }
}

namespace
{
    struct ColumnDef { TrackColumn column; int width; bool visible; };
    constexpr ColumnDef defaultColumns[] = {
        { TrackColumn::title, 240, true },   { TrackColumn::artist, 170, true },  { TrackColumn::remix, 130, true },
        { TrackColumn::bpm, 62, true },      { TrackColumn::key, 48, true },      { TrackColumn::genre, 120, true },
        { TrackColumn::energy, 56, true },   { TrackColumn::length, 56, true },
        { TrackColumn::analysisQuality, 70, true }, { TrackColumn::rating, 90, true },
        { TrackColumn::album, 150, false },  { TrackColumn::format, 56, false },  { TrackColumn::bitrate, 62, false }, { TrackColumn::year, 50, false },
        { TrackColumn::playCount, 50, false }, { TrackColumn::lastPlayed, 110, false }, { TrackColumn::firstSeen, 110, false },
        { TrackColumn::path, 300, false },
    };

    int columnId (TrackColumn c) { return (int) c + 1; }
    TrackColumn columnFromId (int id) { return (TrackColumn) (id - 1); }

    juce::String formatLength (double seconds)
    {
        const int s = (int) std::round (seconds);
        return juce::String (s / 60) + ":" + juce::String (s % 60).paddedLeft ('0', 2);
    }

    juce::String formatDate (juce::int64 ms)
    {
        return ms > 0 ? juce::Time (ms).formatted ("%d.%m.%y %H:%M") : juce::String();
    }

    constexpr int menuLoadA = 1, menuLoadB = 2, menuNewPlaylist = 3, menuRemoveFromPlaylist = 4, menuReveal = 5,
                  menuRename = 6, menuMove = 7, menuCopy = 8, menuRemoveLibrary = 9, menuLocate = 10,
                  menuAnalyse = 11, menuResetManual = 12, menuPrepareStems = 13, menuDetectGenre = 14,
                  menuRatingBase = 100, menuPlaylistBase = 1000;
}

//==============================================================================
class LibraryBrowser::Sidebar
{
public:
    struct Node : public juce::TreeViewItem
    {
        Node (LibraryBrowser& o, juce::String text, bool isHeader, ViewSelection s, juce::Colour c = Colours::text)
            : owner (o), label (std::move (text)), header (isHeader), selection (std::move (s)), colour (c) {}

        bool mightContainSubItems() override { return header || hasChildren; }
        bool canBeSelected() const override { return ! header; }
        juce::String getUniqueName() const override
        {
            return label + "|" + juce::String ((int) selection.view) + "|" + juce::String (selection.playlistId) + "|" + selection.folder
                   + "|" + juce::String (selection.minBpm);
        }

        void paintItem (juce::Graphics& g, int width, int height) override
        {
            if (header)
            {
                g.setColour (Colours::textDim);
                g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
                g.drawText (label.toUpperCase(), 2, 0, width - 4, height, juce::Justification::centredLeft);
                return;
            }

            if (isSelected())
            {
                g.setColour (Colours::deckA.withAlpha (0.3f));
                g.fillRect (0, 0, width, height);
            }
            g.setColour (colour);
            g.setFont (juce::FontOptions (13.5f));
            g.drawText (label, 4, 0, width - 8, height, juce::Justification::centredLeft, true);
        }

        void itemSelectionChanged (bool isNowSelected) override
        {
            if (isNowSelected && ! header)
                owner.selectView (selection);
        }

        void itemClicked (const juce::MouseEvent& e) override
        {
            if (! e.mods.isPopupMenu() || selection.view != View::playlist)
                return;

            const auto id = selection.playlistId;
            juce::PopupMenu m;
            m.addItem (1, "Rename...");
            m.addItem (2, "Delete playlist");
            m.showMenuAsync ({}, [safe = juce::WeakReference<LibraryBrowser> (&owner), id] (int r)
            {
                auto* self = safe.get();
                if (self == nullptr || r == 0)
                    return;

                if (r == 2)
                {
                    self->library.deletePlaylist (id);
                    return;
                }

                auto* w = new juce::AlertWindow ("Rename playlist", {}, juce::MessageBoxIconType::NoIcon);
                w->addTextEditor ("name", self->findPlaylist (id).name);
                w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
                w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
                w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, id, w] (int result)
                {
                    if (auto* s = safe.get())
                        if (result == 1 && w->getTextEditorContents ("name").trim().isNotEmpty())
                            s->library.renamePlaylist (id, w->getTextEditorContents ("name").trim());
                }), true);
            });
        }

        bool isInterestedInDragSource (const juce::DragAndDropTarget::SourceDetails& d) override
        {
            return selection.view == View::playlist && TrackDrag::isTrackDrag (d.description)
                   && owner.findPlaylist (selection.playlistId).kind == PlaylistInfo::Kind::playlist;
        }

        void itemDropped (const juce::DragAndDropTarget::SourceDetails& d, int) override
        {
            owner.dropTracksOnPlaylist (selection.playlistId, d.description);
        }

        LibraryBrowser& owner;
        juce::String label;
        bool header;
        ViewSelection selection;
        juce::Colour colour;
        bool hasChildren = false;   // a folder with subfolders: selectable and expandable
    };

    explicit Sidebar (LibraryBrowser& o) : owner (o) {}

    // Folder tree from the tracks' paths: only folders that contain music, with the number of tracks.
    struct Dir
    {
        std::map<juce::String, Dir> kids;
        int count = 0;
    };

    void addFolderNodes (Node* parent, const Dir& dir, const juce::String& fullPath)
    {
        for (const auto& [name, kid] : dir.kids)
        {
            const auto path = fullPath + juce::File::getSeparatorString() + name;
            ViewSelection s;
            s.view = View::folder;
            s.folder = path;
            auto* n = new Node (owner, name + "  (" + juce::String (kid.count) + ")", false, s);
            n->hasChildren = ! kid.kids.empty();
            parent->addSubItem (n);
            n->setOpenness (juce::TreeViewItem::Openness::opennessClosed);
            addFolderNodes (n, kid, path);
        }
    }

    Node* build (const juce::String& signatureFolders)
    {
        juce::ignoreUnused (signatureFolders);
        root = std::make_unique<Node> (owner, "root", true, ViewSelection());

        auto addSection = [this] (const juce::String& title, bool open = true) -> Node*
        {
            auto* n = new Node (owner, title, true, ViewSelection());
            n->setOpenness (open ? juce::TreeViewItem::Openness::opennessOpen : juce::TreeViewItem::Openness::opennessClosed);
            root->addSubItem (n);
            return n;
        };

        auto addView = [this] (Node* section, const juce::String& name, View view, double minBpm = 0.0, double maxBpm = 0.0)
        {
            ViewSelection s;
            s.view = view;
            s.minBpm = minBpm;
            s.maxBpm = maxBpm;
            section->addSubItem (new Node (owner, name, false, s));
        };

        // Structure: what to play, by tempo, what still needs work, where the files are, own playlists.
        auto* librarySection = addSection ("Library");
        addView (librarySection, "All songs", View::allSongs);
        addView (librarySection, "Favorites (4+ stars)", View::favorites);
        addView (librarySection, "Recently added", View::recentlyAdded);
        addView (librarySection, "Recently played", View::recentlyPlayed);
        addView (librarySection, "History", View::history);

        auto* tempoSection = addSection ("By tempo", false);   // folded: folders and playlists stay in view
        addView (tempoSection, "Under 100 BPM", View::tempo, 0.01, 99.99);
        addView (tempoSection, "100 - 115 BPM", View::tempo, 100.0, 114.99);
        addView (tempoSection, "115 - 125 BPM", View::tempo, 115.0, 124.99);
        addView (tempoSection, "125 - 135 BPM", View::tempo, 125.0, 134.99);
        addView (tempoSection, "Over 135 BPM", View::tempo, 135.0, 400.0);

        auto* tidySection = addSection ("To do", false);
        addView (tidySection, "Not analysed", View::notAnalysed);
        addView (tidySection, "No genre", View::noGenre);
        addView (tidySection, "Missing files", View::missing);
        addView (tidySection, "Duplicates", View::duplicates);

        auto* folders = addSection ("Music folders");
        for (const auto& f : owner.library.getMusicFolders())
        {
            const auto root = f.getFullPathName().trimCharactersAtEnd ("\\/");
            const auto prefix = root + juce::File::getSeparatorString();

            Dir tree;
            int total = 0;
            for (const auto& t : owner.library.getTracks())
            {
                if (t.missing || ! t.path.startsWithIgnoreCase (prefix))
                    continue;
                ++total;
                auto* dir = &tree;
                const auto relative = t.path.substring (prefix.length());
                auto parts = juce::StringArray::fromTokens (relative, "\\/", "");
                for (int i = 0; i < parts.size() - 1 && i < 4; ++i)   // folders only (not the file), four levels deep
                {
                    dir = &dir->kids[parts[i]];
                    ++dir->count;
                }
            }

            ViewSelection s;
            s.view = View::folder;
            s.folder = root;
            auto* n = new Node (owner, (f.getFileName().isEmpty() ? f.getFullPathName() : f.getFileName()) + "  (" + juce::String (total) + ")", false, s);
            n->hasChildren = ! tree.kids.empty();
            folders->addSubItem (n);
            n->setOpenness (juce::TreeViewItem::Openness::opennessClosed);
            addFolderNodes (n, tree, root);
        }

        auto* playlists = addSection ("Playlists");
        for (const auto& p : owner.library.getPlaylists())
        {
            ViewSelection s;
            s.view = View::playlist;
            s.playlistId = p.id;
            playlists->addSubItem (new Node (owner, (p.kind == PlaylistInfo::Kind::smart ? juce::String ("* ") : juce::String()) + p.name,
                                             false, s, p.kind == PlaylistInfo::Kind::smart ? Colours::cue : Colours::text));
        }
        return root.get();
    }

    Node* find (const ViewSelection& s, Node* from)
    {
        for (int i = 0; i < from->getNumSubItems(); ++i)
        {
            auto* n = static_cast<Node*> (from->getSubItem (i));
            if (! n->header && n->selection.view == s.view && n->selection.playlistId == s.playlistId && n->selection.folder == s.folder
                && n->selection.minBpm == s.minBpm)
                return n;
            if (n->header || n->hasChildren)
                if (auto* r = find (s, n))
                    return r;
        }
        return nullptr;
    }

    LibraryBrowser& owner;
    std::unique_ptr<Node> root;
};

//==============================================================================
LibraryBrowser::LibraryBrowser (LibraryManager& lib) : library (lib)
{
    sidebar = std::make_unique<Sidebar> (*this);
    sidebarTree.setRootItemVisible (false);
    sidebarTree.setDefaultOpenness (true);
    sidebarTree.setIndentSize (10);
    sidebarTree.setColour (juce::TreeView::backgroundColourId, Colours::panel);
    addAndMakeVisible (sidebarTree);

    searchBox.setTextToShowWhenEmpty ("Search title, artist, album, genre, key, tags...", Colours::textDim);
    searchBox.setColour (juce::TextEditor::backgroundColourId, Colours::panelRaised);
    searchBox.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    searchBox.onTextChange = [this] { spec.searchText = searchBox.getText(); refreshPending = true; startTimer (120); };
    addAndMakeVisible (searchBox);

    auto setupSmall = [this] (juce::TextEditor& e, const juce::String& hint, const juce::String& restriction)
    {
        e.setTextToShowWhenEmpty (hint, Colours::textDim);
        e.setColour (juce::TextEditor::backgroundColourId, Colours::panelRaised);
        e.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        if (restriction.isNotEmpty())
            e.setInputRestrictions (7, restriction);
        e.onTextChange = [this]
        {
            spec.minBpm = bpmFrom.getText().getDoubleValue();
            spec.maxBpm = bpmTo.getText().getDoubleValue();
            spec.keyFilter = keyFilter.getText().trim();
            refreshPending = true;
            startTimer (120);
        };
        addAndMakeVisible (e);
    };
    setupSmall (bpmFrom, "BPM min", "0123456789.");
    setupSmall (bpmTo, "BPM max", "0123456789.");
    setupSmall (keyFilter, "Key", {});

    ratingFilter.addItem ("Any rating", 1);
    for (int i = 1; i <= 5; ++i)
        ratingFilter.addItem (juce::String (i) + "+ stars", i + 1);
    ratingFilter.setSelectedId (1, juce::dontSendNotification);
    ratingFilter.onChange = [this] { spec.minRating = ratingFilter.getSelectedId() - 1; refresh(); };
    addAndMakeVisible (ratingFilter);

    smartMatchToggle.setTooltip ("Smart Matching: marks tracks that fit the track on the deck.\n"
                                 "Green = very good, blue = good, no colour = does not fit.\n"
                                 "Based on tempo (also half / double time), key (Camelot wheel) and genre. "
                                 "Only values that are known are used.");
    smartMatchToggle.onClick = [this]
    {
        setSmartMatch (smartMatchToggle.getToggleState(), matchMode);
        if (onSmartMatchSettingsChanged)
            onSmartMatchSettingsChanged();
    };
    addAndMakeVisible (smartMatchToggle);

    matchModeBox.addItem ("Auto", 1);
    matchModeBox.addItem ("Deck 1", 2);
    matchModeBox.addItem ("Deck 2", 3);
    matchModeBox.addItem ("Both decks", 4);
    matchModeBox.setSelectedId (1, juce::dontSendNotification);
    matchModeBox.setTooltip ("Which deck the matching refers to.\nAuto: the playing deck (or the deck loaded last if none is playing).\nBoth: tracks must fit both decks.");
    matchModeBox.onChange = [this]
    {
        setSmartMatch (smartMatchEnabled, (MatchMode) (matchModeBox.getSelectedId() - 1));
        if (onSmartMatchSettingsChanged)
            onSmartMatchSettingsChanged();
    };
    addAndMakeVisible (matchModeBox);

    matchFilterBox.addItem ("All tracks", 1);
    matchFilterBox.addItem ("Matches only", 2);
    matchFilterBox.addItem ("Green only", 3);
    matchFilterBox.setSelectedId (1, juce::dontSendNotification);
    matchFilterBox.setTooltip ("Filter the list by Smart Matching.\nMatches only: green and blue tracks. Green only: very good matches.\nNeeds Smart Match switched on and a track on a deck.");
    matchFilterBox.onChange = [this]
    {
        matchFilterLevel = matchFilterBox.getSelectedId() - 1;
        refresh();
    };
    addAndMakeVisible (matchFilterBox);

    addFolderButton.onClick = [this] { addFolder(); };
    rescanButton.setTooltip ("Rescan all music folders: finds new, changed, moved and missing files");
    rescanButton.onClick = [this]
    {
        if (! library.rescanAll())
            statusLabel.setText (library.isScanning() ? "A scan is already running" : "No music folders yet - use + Folder", juce::dontSendNotification);
        startTimer (250);
    };
    newPlaylistButton.onClick = [this]
    {
        juce::PopupMenu m;
        m.addItem (1, "New playlist...");
        m.addItem (2, "New smart playlist...");
        m.showMenuAsync ({}, [safe = juce::WeakReference<LibraryBrowser> (this)] (int r)
        {
            if (auto* self = safe.get())
                if (r != 0)
                    self->createPlaylistDialog (r == 2);
        });
    };
    analyseButton.setTooltip ("Analyse BPM, key, beat grid and energy of all tracks that are not analysed yet.\n"
                              "Runs automatically in the background; click again to stop.");
    analyseButton.onClick = [this]
    {
        if (analysis == nullptr)
            return;
        if (analysis->isBusy())
        {
            analysis->cancel();
        }
        else
        {
            analysis->retrySkipped();
            const int queued = analysis->enqueuePending();
            if (queued == 0)
                statusLabel.setText ("Everything is already analysed.", juce::dontSendNotification);
        }
        updateStatus();
    };

    for (auto* b : { &addFolderButton, &rescanButton, &analyseButton, &newPlaylistButton })
        addAndMakeVisible (b);

    statusLabel.setColour (juce::Label::textColourId, Colours::textDim);
    statusLabel.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (statusLabel);

    table.setColour (juce::ListBox::backgroundColourId, Colours::panel);
    table.setMultipleSelectionEnabled (true);
    table.setRowHeight (22);
    table.getViewport()->setScrollBarThickness (10);
    auto& header = table.getHeader();
    for (const auto& c : defaultColumns)
    {
        int columnFlags = juce::TableHeaderComponent::defaultFlags;
        if (! c.visible)
            columnFlags &= ~juce::TableHeaderComponent::visible;
        header.addColumn (columnDisplayName (c.column), columnId (c.column), c.width, 30, 800, columnFlags);
    }
    header.setSortColumnId (columnId (TrackColumn::title), true);
    header.setStretchToFitActive (true);
    addAndMakeVisible (table);

    infoPanel.onSave = [this] (const TrackRecord& edited, bool toFile) { saveTrackEdits (edited, toFile); };
    infoPanel.onRatingChanged = [this] (juce::int64 id, int r) { library.setRating (id, r); };
    addAndMakeVisible (infoPanel);

    layout.setItemLayout (0, 130, 400, 190);   // sidebar
    layout.setItemLayout (1, 5, 5, 5);
    layout.setItemLayout (2, 300, -1.0, -0.8); // list
    layout.setItemLayout (3, 5, 5, 5);
    layout.setItemLayout (4, 200, 450, 270);   // info
    addAndMakeVisible (resizerLeft);
    addAndMakeVisible (resizerRight);

    library.addChangeListener (this);
    rebuildSidebarIfNeeded();
    refresh();
}

LibraryBrowser::~LibraryBrowser()
{
    setAnalysisManager (nullptr);
    library.removeChangeListener (this);
    stopTimer();
    sidebarTree.setRootItem (nullptr);
}

juce::String LibraryBrowser::getLayoutState() const
{
    return table.getHeader().toString();
}

void LibraryBrowser::setLayoutState (const juce::String& state)
{
    if (state.isNotEmpty())
        table.getHeader().restoreFromString (state);
}

void LibraryBrowser::setLoadedTracks (juce::int64 d1, juce::int64 d2)
{
    if (loadedOnDeck[0] != d1 || loadedOnDeck[1] != d2)
    {
        loadedOnDeck[0] = d1;
        loadedOnDeck[1] = d2;
        table.repaint();
    }
}

//==============================================================================
void LibraryBrowser::paint (juce::Graphics& g)
{
    g.setColour (Colours::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (2.0f), 6.0f);
}

void LibraryBrowser::resized()
{
    auto area = getLocalBounds().reduced (6);

    auto top = area.removeFromTop (28);
    newPlaylistButton.setBounds (top.removeFromRight (84).reduced (2, 0));
    analyseButton.setBounds (top.removeFromRight (70).reduced (2, 0));
    rescanButton.setBounds (top.removeFromRight (70).reduced (2, 0));
    addFolderButton.setBounds (top.removeFromRight (76).reduced (2, 0));
    ratingFilter.setBounds (top.removeFromRight (100).reduced (2, 0));
    matchFilterBox.setBounds (top.removeFromRight (104).reduced (2, 0));
    matchModeBox.setBounds (top.removeFromRight (92).reduced (2, 0));
    smartMatchToggle.setBounds (top.removeFromRight (104).reduced (2, 0));
    keyFilter.setBounds (top.removeFromRight (54).reduced (2, 0));
    bpmTo.setBounds (top.removeFromRight (70).reduced (2, 0));
    bpmFrom.setBounds (top.removeFromRight (70).reduced (2, 0));
    searchBox.setBounds (top.reduced (2, 0));

    area.removeFromTop (4);
    statusLabel.setBounds (area.removeFromBottom (18));

    juce::Component* comps[] = { &sidebarTree, &resizerLeft, &table, &resizerRight, &infoPanel };
    layout.layOutComponents (comps, 5, area.getX(), area.getY(), area.getWidth(), area.getHeight(), false, true);
}

//==============================================================================
const TrackRecord* LibraryBrowser::trackAt (int row) const
{
    if (! juce::isPositiveAndBelow (row, (int) rows.size()))
        return nullptr;
    return &library.getTracks()[(size_t) rows[(size_t) row]];
}

std::vector<juce::int64> LibraryBrowser::getSelectedIds() const
{
    std::vector<juce::int64> ids;
    const auto selected = table.getSelectedRows();
    for (int i = 0; i < selected.size(); ++i)
        if (auto* t = trackAt (selected[i]))
            ids.push_back (t->id);
    return ids;
}

PlaylistInfo LibraryBrowser::findPlaylist (juce::int64 id) const
{
    for (const auto& p : library.getPlaylists())
        if (p.id == id)
            return p;
    return {};
}

void LibraryBrowser::selectView (const ViewSelection& s)
{
    currentView = s;
    refresh();
}

void LibraryBrowser::rebuildSidebarIfNeeded()
{
    juce::String signature;
    for (const auto& f : library.getMusicFolders())
        signature << f.getFullPathName() << "\n";
    for (const auto& p : library.getPlaylists())
        signature << p.id << ":" << p.name << ":" << (int) p.kind << "\n";
    signature << library.getTracks().size() << "\n";   // the folder tree shows what is in the library

    if (signature == sidebarSignature)
        return;
    sidebarSignature = signature;

    sidebarTree.setRootItem (nullptr);
    auto* root = sidebar->build (signature);
    sidebarTree.setRootItem (root);

    if (auto* node = sidebar->find (currentView, root))
        node->setSelected (true, true, juce::dontSendNotification);
    else if (currentView.view == View::playlist)
        currentView = {}; // playlist was deleted
}

void LibraryBrowser::refresh()
{
    refreshPending = false;
    rebuildSidebarIfNeeded();

    // Remember the selection so edits and rescans don't make it jump.
    const auto previouslySelected = getSelectedIds();

    auto viewSpec = spec;
    viewSpec.hideMissing = currentView.view != View::missing;

    switch (currentView.view)
    {
        case View::allSongs:
            rows = library.query (viewSpec);
            break;
        case View::favorites:
            viewSpec.minRating = juce::jmax (4, spec.minRating);
            rows = library.query (viewSpec);
            break;
        case View::recentlyPlayed:
            viewSpec.onlyPlayed = true;
            viewSpec.sortColumn = TrackColumn::lastPlayed;
            viewSpec.ascending = false;
            rows = library.query (viewSpec);
            break;
        case View::history:
            rows = library.queryHistory (500);
            break;
        case View::missing:
            viewSpec.onlyMissing = true;
            rows = library.query (viewSpec);
            break;
        case View::duplicates:
        {
            rows.clear();
            for (const auto& group : library.findDuplicates())
                for (auto id : group)
                {
                    const auto* t = library.findTrack (id);
                    if (t != nullptr)
                        rows.push_back ((int) (t - library.getTracks().data()));
                }
            break;
        }
        case View::folder:
            viewSpec.folderPrefix = currentView.folder;
            rows = library.query (viewSpec);
            break;
        case View::recentlyAdded:
        {
            viewSpec.sortColumn = TrackColumn::firstSeen;
            viewSpec.ascending = false;
            rows = library.query (viewSpec);
            if (rows.size() > 200)
                rows.resize (200);
            break;
        }
        case View::tempo:
            viewSpec.minBpm = currentView.minBpm;
            viewSpec.maxBpm = currentView.maxBpm;
            rows = library.query (viewSpec);
            break;
        case View::notAnalysed:
        case View::noGenre:
        {
            rows = library.query (viewSpec);
            const bool analysisView = currentView.view == View::notAnalysed;
            const auto& all = library.getTracks();
            rows.erase (std::remove_if (rows.begin(), rows.end(), [&] (int index)
                        {
                            const auto& t = all[(size_t) index];
                            return analysisView ? t.analysisVersion > 0 : t.displayGenre().isNotEmpty();
                        }), rows.end());
            break;
        }
        case View::playlist:
        {
            const auto playlist = findPlaylist (currentView.playlistId);
            viewSpec.hideMissing = false;
            rows = library.queryPlaylist (playlist, viewSpec);
            break;
        }
    }

    // Smart Matching filter: keep only tracks that fit the reference deck(s). Without a
    // reference track nothing can be judged, so the list stays complete.
    if (smartMatchEnabled && matchFilterLevel > 0 && ! matchSubjects.empty())
    {
        const auto minLevel = matchFilterLevel == 2 ? MatchLevel::veryGood : MatchLevel::good;
        std::vector<int> kept;
        for (int i = 0; i < (int) rows.size(); ++i)
            if ((int) getMatchDetail (i).level >= (int) minLevel)
                kept.push_back (rows[(size_t) i]);
        rows = std::move (kept);
    }

    table.updateContent();
    rowMatchCache.assign (rows.size(), -1);   // rows changed: match colours are computed again when painted

    juce::SparseSet<int> restore;
    if (! previouslySelected.empty())
    {
        const std::unordered_set<juce::int64> wanted (previouslySelected.begin(), previouslySelected.end());
        for (int i = 0; i < (int) rows.size(); ++i)
            if (wanted.count (library.getTracks()[(size_t) rows[(size_t) i]].id) != 0)
                restore.addRange ({ i, i + 1 });
    }
    table.setSelectedRows (restore, juce::dontSendNotification);
    table.repaint();

    // Info pane: keep what the user is typing; otherwise follow the selection.
    if (restore.size() == 1)
    {
        if (const auto* t = trackAt (restore[0]))
            if (! (infoPanel.isDirty() && infoPanel.getTrackId() == t->id))
                infoPanel.setTrack (t);
    }
    else if (! infoPanel.isDirty())
    {
        infoPanel.setTrack (nullptr);
    }

    updateStatus();
}

//==============================================================================
// Smart Matching
void LibraryBrowser::setSmartMatch (bool enabled, MatchMode mode)
{
    smartMatchEnabled = enabled;
    matchMode = mode;
    smartMatchToggle.setToggleState (enabled, juce::dontSendNotification);
    matchModeBox.setSelectedId ((int) mode + 1, juce::dontSendNotification);
    matchModeBox.setEnabled (enabled);
    matchFilterBox.setEnabled (enabled);
    rebuildMatchSubjects();
}

void LibraryBrowser::setMatchReferences (const std::array<MatchReference, 2>& refs)
{
    // Remember which deck got a track last: it is the reference when nothing is playing.
    for (size_t d = 0; d < 2; ++d)
        if (refs[d].trackId != matchRefs[d].trackId && refs[d].trackId != 0)
            lastLoadedDeck = (int) d;

    bool changed = false;
    for (size_t d = 0; d < 2; ++d)
        changed = changed || refs[d].trackId != matchRefs[d].trackId || refs[d].playing != matchRefs[d].playing
                  || std::abs (refs[d].playbackBpm - matchRefs[d].playbackBpm) > 0.01;
    if (! changed)
        return;

    matchRefs = refs;
    rebuildMatchSubjects();
}

void LibraryBrowser::rebuildMatchSubjects()
{
    matchSubjects.clear();
    matchReferenceIds.clear();
    rowMatchCache.assign (rows.size(), -1);

    if (smartMatchEnabled)
    {
        std::vector<int> decks;
        auto loaded = [this] (int d) { return matchRefs[(size_t) d].trackId != 0; };

        if (matchMode == MatchMode::deck1)       { if (loaded (0)) decks.push_back (0); }
        else if (matchMode == MatchMode::deck2)  { if (loaded (1)) decks.push_back (1); }
        else if (matchMode == MatchMode::both)   { for (int d = 0; d < 2; ++d) if (loaded (d)) decks.push_back (d); }
        else
        {
            // Automatic: what is playing; if nothing plays, the deck that was loaded last.
            for (int d = 0; d < 2; ++d)
                if (loaded (d) && matchRefs[(size_t) d].playing)
                    decks.push_back (d);
            if (decks.empty())
            {
                if (lastLoadedDeck >= 0 && loaded (lastLoadedDeck)) decks.push_back (lastLoadedDeck);
                else for (int d = 0; d < 2; ++d) if (loaded (d)) decks.push_back (d);
            }
        }

        for (int d : decks)
            if (const auto* t = library.findTrack (matchRefs[(size_t) d].trackId))
            {
                matchSubjects.push_back (makeSubject (*t, matchRefs[(size_t) d].playbackBpm));
                matchReferenceIds.push_back (t->id);
            }
    }

    // An active match filter depends on the reference tracks, so the list is built again.
    if (matchFilterLevel > 0 && smartMatchEnabled)
        refresh();
    else
        table.repaint();
}

MatchDetail LibraryBrowser::getMatchDetail (int row) const
{
    MatchDetail result;
    if (! smartMatchEnabled || matchSubjects.empty())
        return result;
    const auto* t = trackAt (row);
    if (t == nullptr || t->missing || std::find (matchReferenceIds.begin(), matchReferenceIds.end(), t->id) != matchReferenceIds.end())
        return result;

    // With several reference decks a track must fit all of them: the weakest result counts.
    const auto candidate = makeSubject (*t);
    result.level = MatchLevel::veryGood;
    for (const auto& subject : matchSubjects)
    {
        const auto d = evaluateMatch (subject, candidate);
        if ((int) d.level < (int) result.level || result.explanation.isEmpty())
            result = d;
        if (d.level == MatchLevel::none)
            break;
    }
    return result;
}

MatchLevel LibraryBrowser::getMatchLevel (int row) const
{
    if (! smartMatchEnabled || ! juce::isPositiveAndBelow (row, (int) rows.size()))
        return MatchLevel::none;
    if (rowMatchCache.size() != rows.size())
        rowMatchCache.assign (rows.size(), -1);
    auto& cached = rowMatchCache[(size_t) row];
    if (cached < 0)
        cached = (signed char) getMatchDetail (row).level;
    return (MatchLevel) cached;
}

void LibraryBrowser::moveSelection (int rowsDown)
{
    if (rows.empty() || rowsDown == 0)
        return;
    const auto selected = table.getSelectedRows();
    const int current = selected.isEmpty() ? (rowsDown > 0 ? -1 : (int) rows.size()) : selected[0];
    const int next = juce::jlimit (0, (int) rows.size() - 1, current + rowsDown);
    table.selectRow (next);
    table.scrollToEnsureRowIsOnscreen (next);
}

void LibraryBrowser::loadSelectedToDeck (int deckIndex)
{
    const auto ids = getSelectedIds();
    if (ids.empty())
        return;
    if (const auto* t = library.findTrack (ids[0]))
        loadTrack (*t, deckIndex);
}

void LibraryBrowser::setAnalysisManager (AnalysisManager* manager)
{
    if (analysis != nullptr)
        analysis->removeChangeListener (this);
    analysis = manager;
    if (analysis != nullptr)
        analysis->addChangeListener (this);
}

void LibraryBrowser::updateStatus()
{
    juce::String text = juce::String ((int) rows.size()) + " tracks";

    if (analysis != nullptr)
    {
        analyseButton.setButtonText (analysis->isBusy() ? "Stop" : "Analyse");
        if (! analysis->isGenreModelInstalled())
            text << "   |   genre detection: model not installed (right-click > Detect genre)";
        if (analysis->isBusy())
            text << "   |   analysing " << analysis->getDone() + analysis->getFailed() + analysis->getSkippedCloud() << "/" << analysis->getQueued();
        else if (analysis->getSkippedCloud() > 0)
            text << "   |   " << analysis->getSkippedCloud() << " cloud-only tracks not analysed (make them available offline)";
        if (analysis->getFailed() > 0)
            text << "   |   " << analysis->getFailed() << " could not be analysed";
    }

    if (library.isScanning())
        text << "   |   scanning... " << library.getFilesFound() << " files found, "
             << library.getScanProgressPercent() << "% processed";
    else if (library.getLastScanSummary().filesFound > 0)
    {
        const auto& s = library.getLastScanSummary();
        text << "   |   last scan: " << s.added << " added, " << s.updated << " updated, " << s.relinked << " moved, "
             << s.markedMissing << " missing";
        if (s.failed > 0)
            text << ", " << s.failed << " unreadable";
    }
    statusLabel.setText (text, juce::dontSendNotification);
}

void LibraryBrowser::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == analysis)
    {
        updateStatus(); // progress only; track data changes arrive through the library
        return;
    }
    refreshPending = true;
    startTimer (100);
}

void LibraryBrowser::timerCallback()
{
    if (refreshPending)
        refresh();
    else
        updateStatus();

    if (! refreshPending && ! library.isScanning())
        stopTimer();
    else if (library.isScanning())
        startTimer (250);
}

//==============================================================================
void LibraryBrowser::paintRowBackground (juce::Graphics& g, int row, int width, int height, bool selected)
{
    if (row % 2 == 1)
        g.fillAll (juce::Colours::white.withAlpha (0.025f));

    // Smart Matching: tint and a bar on the left; the selection highlight is drawn on top and stays visible.
    const auto matchLevel = getMatchLevel (row);
    if (matchLevel != MatchLevel::none)
    {
        const auto colour = matchLevel == MatchLevel::veryGood ? Colours::matchVeryGood : Colours::matchGood;
        g.setColour (colour.withAlpha (0.16f));
        g.fillRect (0, 0, width, height);
        g.setColour (colour);
        g.fillRect (0, 0, 4, height);
    }

    if (selected)
    {
        g.setColour (Colours::deckA.withAlpha (0.28f));
        g.fillRect (0, 0, width, height);
    }
}

void LibraryBrowser::paintCell (juce::Graphics& g, int row, int column, int width, int height, bool selected)
{
    const auto* t = trackAt (row);
    if (t == nullptr)
        return;

    const auto col = columnFromId (column);

    if (col == TrackColumn::rating)
    {
        RatingStars::drawStars (g, juce::Rectangle<float> (2.0f, 2.0f, (float) width - 4.0f, (float) height - 4.0f),
                                t->rating, Colours::cue, Colours::outline.withAlpha (0.7f));
        return;
    }

    if (col == TrackColumn::analysisQuality)
    {
        const int q = t->analysisQuality();
        juce::String label;
        juce::Colour colour = Colours::textDim;
        if (t->analysisVersion < 0)      { label = "failed"; colour = Colours::danger; }
        else if (q >= 0)
        {
            label = juce::String (q) + "%";
            colour = q >= 75 ? Colours::play : (q >= 45 ? Colours::cue : Colours::danger);
            if (t->manualFlags != 0)
                label << " M";
            else if (t->variableTempo)
                label << " ~";
        }
        g.setColour (colour);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (label, 4, 0, width - 8, height, juce::Justification::centredRight, true);
        return;
    }

    juce::String text;
    auto justification = juce::Justification::centredLeft;
    switch (col)
    {
        case TrackColumn::energy:     text = t->energy > 0.0 ? juce::String (t->energy, 1) : juce::String(); justification = juce::Justification::centredRight; break;
        case TrackColumn::analysisQuality: break;
        case TrackColumn::title:      text = t->title; break;
        case TrackColumn::artist:     text = t->artist; break;
        case TrackColumn::remix:      text = t->remix; break;
        case TrackColumn::album:      text = t->album; break;
        case TrackColumn::genre:      text = t->displayGenre(); break;
        case TrackColumn::key:        text = t->key; justification = juce::Justification::centred; break;
        case TrackColumn::bpm:        text = t->bpm > 0.0 ? juce::String (t->bpm, 2) : juce::String(); justification = juce::Justification::centredRight; break;
        case TrackColumn::length:     text = formatLength (t->lengthSeconds); justification = juce::Justification::centredRight; break;
        case TrackColumn::path:       text = t->path; break;
        case TrackColumn::bitrate:    text = t->bitrate > 0 ? juce::String (t->bitrate) : juce::String(); justification = juce::Justification::centredRight; break;
        case TrackColumn::format:     text = t->format; break;
        case TrackColumn::year:       text = t->year > 0 ? juce::String (t->year) : juce::String(); break;
        case TrackColumn::playCount:  text = t->playCount > 0 ? juce::String (t->playCount) : juce::String(); justification = juce::Justification::centredRight; break;
        case TrackColumn::lastPlayed: text = formatDate (t->lastPlayed); break;
        case TrackColumn::firstSeen:  text = formatDate (t->firstSeen); break;
        case TrackColumn::rating:     break;
    }

    juce::Colour colour = Colours::text;
    if (t->missing)
        colour = Colours::danger.withAlpha (0.85f);
    else if (t->id == loadedOnDeck[0])
        colour = Colours::deckA;
    else if (t->id == loadedOnDeck[1])
        colour = Colours::deckB;
    else if (! selected && col != TrackColumn::title && col != TrackColumn::artist)
        colour = Colours::text.withAlpha (0.8f);

    // A genre the program detected (not the user's own) is shown dimmed and in italics.
    const bool automaticGenre = col == TrackColumn::genre && t->isGenreAutomatic();
    if (automaticGenre)
        colour = colour.withAlpha (0.62f);

    g.setColour (colour);
    g.setFont (juce::FontOptions (13.5f, automaticGenre ? juce::Font::italic : (col == TrackColumn::title ? juce::Font::bold : juce::Font::plain)));
    g.drawText (text, 4, 0, width - 8, height, justification, true);
}

void LibraryBrowser::sortOrderChanged (int newSortColumnId, bool isForwards)
{
    if (newSortColumnId == 0)
        return;
    spec.sortColumn = columnFromId (newSortColumnId);
    spec.ascending = isForwards;

    // Playlists keep their own order until the user explicitly picks a column.
    if (currentView.view == View::playlist && findPlaylist (currentView.playlistId).kind == PlaylistInfo::Kind::playlist)
    {
        sortTrackIndices (library.getTracks(), rows, spec.sortColumn, spec.ascending);
        rowMatchCache.assign (rows.size(), -1);
        table.updateContent();
        table.repaint();
        return;
    }
    refresh();
}

void LibraryBrowser::selectedRowsChanged (int)
{
    const auto ids = getSelectedIds();
    if (ids.size() == 1 && ! infoPanel.isDirty())
        infoPanel.setTrack (library.findTrack (ids[0]));
    else if (ids.size() != 1 && ! infoPanel.isDirty())
        infoPanel.setTrack (nullptr);
}

juce::String LibraryBrowser::getCellTooltip (int row, int column)
{
    const auto* t = trackAt (row);
    if (t == nullptr)
        return {};

    if (columnFromId (column) == TrackColumn::bpm && t->bpm > 0.0)
        return "Original tempo of the file: " + juce::String (t->bpm, 2) + " BPM"
               + (t->analysisVersion > 0 && ! t->isManual (TrackRecord::manualBpm) ? "\nDetected automatically, " + juce::String (juce::roundToInt (t->bpmConfidence * 100.0)) + " % confidence" : juce::String())
               + "\n(a deck shows the playback tempo with the pitch fader applied)";

    if (columnFromId (column) == TrackColumn::genre && t->genreAutoMain.isNotEmpty())
    {
        juce::String tip = t->genre.isNotEmpty() ? "Your genre: " + t->genre + "\n" : juce::String();
        tip << "Detected: " << (t->genreAutoSub.isNotEmpty() ? t->genreAutoMain + " / " + t->genreAutoSub : t->genreAutoMain)
            << " (" << juce::roundToInt (t->genreConfidence * 100.0) << " %)"
            << (t->genreConfidence < 0.25 ? " - too uncertain to be used" : "");
        const auto alternatives = juce::JSON::parse (t->genreAlternatives);
        if (auto* list = alternatives.getArray())
        {
            juce::StringArray names;
            for (const auto& a : *list)
                names.add (a.getProperty ("style", {}).toString().replace ("---", " / ") + " " + juce::String (juce::roundToInt ((double) a.getProperty ("p", 0.0) * 100.0)) + " %");
            if (! names.isEmpty())
                tip << "\nAlso possible: " << names.joinIntoString (", ");
        }
        return tip;
    }

    juce::String tip = t->artist + " - " + t->title + (t->missing ? "\nFILE IS MISSING" : "") + "\n" + t->path;
    const auto detail = getMatchDetail (row);
    if (detail.level != MatchLevel::none)
        tip = juce::String (detail.level == MatchLevel::veryGood ? "Very good match: " : "Good match: ") + detail.explanation + "\n" + tip;
    return tip;
}

void LibraryBrowser::cellDoubleClicked (int row, int column, const juce::MouseEvent&)
{
    if (columnFromId (column) == TrackColumn::rating)
        return;
    if (const auto* t = trackAt (row))
        loadTrack (*t, -1);
}

void LibraryBrowser::cellClicked (int row, int column, const juce::MouseEvent& e)
{
    const auto* t = trackAt (row);
    if (t == nullptr)
        return;

    if (e.mods.isPopupMenu())
    {
        if (! table.getSelectedRows().contains (row))
            table.selectRow (row);
        showContextMenu (row);
        return;
    }

    if (columnFromId (column) == TrackColumn::rating)
    {
        const auto cell = table.getCellPosition (column, row, false);
        const float x = e.getEventRelativeTo (&table).position.x - (float) cell.getX();
        const float starWidth = ((float) cell.getWidth() - 4.0f) / 5.0f;
        int newRating = juce::jlimit (0, 5, (int) std::floor ((x - 2.0f) / juce::jmax (1.0f, starWidth)) + 1);
        if (newRating == t->rating)
            newRating = 0;
        library.setRating (t->id, newRating);
    }
}

void LibraryBrowser::backgroundClicked (const juce::MouseEvent&)
{
    table.deselectAllRows();
}

juce::var LibraryBrowser::getDragSourceDescription (const juce::SparseSet<int>& selectedRows)
{
    if (selectedRows.isEmpty())
        return {};

    auto* obj = new juce::DynamicObject();
    juce::Array<juce::var> ids;
    for (int i = 0; i < selectedRows.size(); ++i)
        if (const auto* t = trackAt (selectedRows[i]))
            ids.add ((juce::int64) t->id);

    const auto* first = trackAt (selectedRows[0]);
    obj->setProperty ("kind", "pdj-tracks");
    obj->setProperty ("ids", ids);
    obj->setProperty ("firstPath", first != nullptr ? first->path : juce::String());
    return juce::var (obj);
}

void LibraryBrowser::deleteKeyPressed (int)
{
    if (currentView.view == View::playlist && findPlaylist (currentView.playlistId).kind == PlaylistInfo::Kind::playlist)
        library.removeFromPlaylist (currentView.playlistId, getSelectedIds());
}

//==============================================================================
void LibraryBrowser::loadTrack (const TrackRecord& track, int deckIndex)
{
    if (track.missing || ! track.getFile().existsAsFile())
    {
        statusLabel.setText ("File not found: " + track.path + "  (right-click > Locate file...)", juce::dontSendNotification);
        return;
    }
    if (onLoadRequest)
        onLoadRequest (track.getFile(), deckIndex);
}

void LibraryBrowser::showContextMenu (int row)
{
    const auto ids = getSelectedIds();
    if (ids.empty())
        return;

    const auto* first = trackAt (row);
    const bool single = ids.size() == 1;
    const bool isPlaylist = currentView.view == View::playlist
                            && findPlaylist (currentView.playlistId).kind == PlaylistInfo::Kind::playlist;
    const auto playlists = library.getPlaylists();

    juce::PopupMenu menu;
    if (single)
    {
        menu.addItem (menuLoadA, "Load to deck 1");
        menu.addItem (menuLoadB, "Load to deck 2");
        menu.addSeparator();
    }

    juce::PopupMenu addTo;
    int normalCount = 0;
    for (size_t i = 0; i < playlists.size(); ++i)
        if (playlists[i].kind == PlaylistInfo::Kind::playlist)
        {
            addTo.addItem (menuPlaylistBase + (int) i, playlists[i].name);
            ++normalCount;
        }
    if (normalCount > 0)
        addTo.addSeparator();
    addTo.addItem (menuNewPlaylist, "New playlist...");
    menu.addSubMenu ("Add to playlist", addTo);
    if (isPlaylist)
        menu.addItem (menuRemoveFromPlaylist, "Remove from this playlist");

    juce::PopupMenu rating;
    for (int r = 0; r <= 5; ++r)
        rating.addItem (menuRatingBase + r, r == 0 ? juce::String ("No rating") : juce::String (r) + (r == 1 ? " star" : " stars"));
    menu.addSubMenu ("Rating", rating);
    menu.addSeparator();
    menu.addItem (menuAnalyse, "Analyse again (BPM, key, grid)", analysis != nullptr);
    menu.addItem (menuPrepareStems, "Prepare stems in the background", (bool) onPrepareStems);
    menu.addItem (menuDetectGenre, "Detect genre (AI)", (bool) onDetectGenre);
    if (single && first != nullptr && first->manualFlags != 0)
        menu.addItem (menuResetManual, "Reset manual corrections (BPM, key, grid, genre)");
    menu.addSeparator();

    if (single)
    {
        menu.addItem (menuReveal, "Show in Explorer", first != nullptr && ! first->missing);
        menu.addItem (menuRename, "Rename file...", first != nullptr && ! first->missing);
        if (first != nullptr && first->missing)
            menu.addItem (menuLocate, "Locate file...");
    }
    menu.addItem (menuMove, "Move file(s) to...");
    menu.addItem (menuCopy, "Copy file(s) to...");
    menu.addSeparator();
    menu.addItem (menuRemoveLibrary, "Remove from library (keeps the file)");

    menu.showMenuAsync ({}, [safe = juce::WeakReference<LibraryBrowser> (this), ids, playlists] (int result)
    {
        auto* self = safe.get();
        if (self == nullptr || result == 0)
            return;

        auto& lib = self->library;
        const auto* t = ids.empty() ? nullptr : lib.findTrack (ids[0]);

        if (result == menuLoadA || result == menuLoadB)       { if (t) self->loadTrack (*t, result == menuLoadA ? 0 : 1); }
        else if (result == menuNewPlaylist)                   self->createPlaylistDialog (false);
        else if (result == menuRemoveFromPlaylist)            lib.removeFromPlaylist (self->currentView.playlistId, ids);
        else if (result == menuReveal)                        { if (t) t->getFile().revealToUser(); }
        else if (result == menuRename)                        { if (t) self->renameFile (*t); }
        else if (result == menuLocate)                        { if (t) self->locateFile (*t); }
        else if (result == menuMove)                          self->moveOrCopyFiles (ids, true);
        else if (result == menuCopy)                          self->moveOrCopyFiles (ids, false);
        else if (result == menuRemoveLibrary)                 lib.removeTracks (ids);
        else if (result == menuAnalyse)                       { if (self->analysis != nullptr) { self->analysis->retrySkipped(); self->analysis->enqueue (ids, true); } }
        else if (result == menuDetectGenre)
        {
            if (self->onDetectGenre)
                self->onDetectGenre (ids);
        }
        else if (result == menuPrepareStems)
        {
            std::vector<juce::File> files;
            for (auto id : ids)
                if (const auto* track = lib.findTrack (id))
                    if (! track->missing)
                        files.push_back (track->getFile());
            if (self->onPrepareStems && ! files.empty())
                self->onPrepareStems (files);
        }
        else if (result == menuResetManual)                   { if (t) { lib.resetManualCorrections (t->id); if (self->analysis != nullptr) self->analysis->enqueue ({ t->id }, true); } }
        else if (result >= menuRatingBase && result <= menuRatingBase + 5)
        {
            for (auto id : ids)
                lib.setRating (id, result - menuRatingBase);
        }
        else if (result >= menuPlaylistBase)
        {
            const auto index = (size_t) (result - menuPlaylistBase);
            if (index < playlists.size())
                lib.addToPlaylist (playlists[index].id, ids);
        }
    });
}

void LibraryBrowser::dropTracksOnPlaylist (juce::int64 playlistId, const juce::var& description)
{
    library.addToPlaylist (playlistId, TrackDrag::getIds (description));
}

//==============================================================================
void LibraryBrowser::addFolder()
{
    chooser = std::make_unique<juce::FileChooser> ("Add music folder",
                                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [safe = juce::WeakReference<LibraryBrowser> (this)] (const juce::FileChooser& fc)
                          {
                              auto* self = safe.get();
                              if (self != nullptr && fc.getResult().isDirectory())
                              {
                                  if (! self->library.addFolderAndScan (fc.getResult()))
                                      self->statusLabel.setText ("A scan is already running", juce::dontSendNotification);
                                  self->startTimer (250);
                              }
                          });
}

void LibraryBrowser::createPlaylistDialog (bool smart)
{
    static const juce::StringArray fields { "title", "artist", "album", "genre", "key", "tags", "bpm", "rating", "year",
                                            "length", "playCount", "daysSincePlayed" };
    static const juce::StringArray ops { "contains", "is", ">", ">=", "<", "<=" };
    constexpr int numRules = 3;

    auto* w = new juce::AlertWindow (smart ? "New smart playlist" : "New playlist",
                                     smart ? "Tracks that match the rules are listed automatically. Empty values are ignored."
                                           : juce::String(),
                                     juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", {}, "Name");
    if (smart)
    {
        w->addComboBox ("match", { "Match ALL rules", "Match ANY rule" });
        for (int i = 0; i < numRules; ++i)
        {
            w->addComboBox ("field" + juce::String (i), fields, "Rule " + juce::String (i + 1));
            w->addComboBox ("op" + juce::String (i), ops);
            w->addTextEditor ("value" + juce::String (i), {});
        }
    }
    w->addButton ("Create", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::WeakReference<LibraryBrowser> (this), w, smart] (int r)
    {
        auto* self = safe.get();
        const auto name = w->getTextEditorContents ("name").trim();
        if (self == nullptr || r != 1 || name.isEmpty())
            return;

        if (! smart)
        {
            self->library.createPlaylist (name);
            return;
        }

        SmartRules rules;
        rules.matchAll = w->getComboBoxComponent ("match")->getSelectedItemIndex() == 0;
        for (int i = 0; i < numRules; ++i)
        {
            const auto value = w->getTextEditorContents ("value" + juce::String (i)).trim();
            if (value.isNotEmpty())
                rules.conditions.push_back ({ fields[w->getComboBoxComponent ("field" + juce::String (i))->getSelectedItemIndex()],
                                              ops[w->getComboBoxComponent ("op" + juce::String (i))->getSelectedItemIndex()],
                                              value });
        }
        if (! rules.conditions.empty())
            self->library.createSmartPlaylist (name, rules);
    }), true);
}

void LibraryBrowser::renameFile (const TrackRecord& track)
{
    const auto id = track.id;
    auto* w = new juce::AlertWindow ("Rename file", "The file name on disk is changed.", juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", track.getFile().getFileNameWithoutExtension());
    w->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::WeakReference<LibraryBrowser> (this), w, id] (int r)
    {
        auto* self = safe.get();
        if (self == nullptr || r != 1)
            return;

        const auto* t = self->library.findTrack (id);
        auto newName = juce::File::createLegalFileName (w->getTextEditorContents ("name").trim());
        if (t == nullptr || newName.isEmpty())
            return;

        const auto oldFile = t->getFile();
        const auto newFile = oldFile.getSiblingFile (newName + oldFile.getFileExtension());
        if (newFile == oldFile)
            return;
        if (newFile.exists())
        {
            self->statusLabel.setText ("A file with that name already exists.", juce::dontSendNotification);
            return;
        }
        if (! oldFile.moveFileTo (newFile))
        {
            self->statusLabel.setText ("Could not rename the file (in use or read-only?).", juce::dontSendNotification);
            return;
        }
        self->library.getDatabase().relinkTrack (id, newFile.getFullPathName(), newFile.getSize(),
                                                 newFile.getLastModificationTime().toMilliseconds());
        self->library.reload();
    }), true);
}

void LibraryBrowser::moveOrCopyFiles (const std::vector<juce::int64>& ids, bool move)
{
    chooser = std::make_unique<juce::FileChooser> (move ? "Move files to..." : "Copy files to...",
                                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [safe = juce::WeakReference<LibraryBrowser> (this), ids, move] (const juce::FileChooser& fc)
                          {
                              auto* self = safe.get();
                              const auto target = fc.getResult();
                              if (self == nullptr || ! target.isDirectory())
                                  return;

                              int done = 0, failed = 0;
                              for (auto id : ids)
                              {
                                  const auto* t = self->library.findTrack (id);
                                  if (t == nullptr || t->missing)
                                  {
                                      ++failed;
                                      continue;
                                  }

                                  const auto source = t->getFile();
                                  const auto dest = target.getChildFile (source.getFileName());
                                  if (dest.exists() || (move ? ! source.moveFileTo (dest) : ! source.copyFileTo (dest)))
                                  {
                                      ++failed;
                                      continue;
                                  }
                                  if (move)
                                      self->library.getDatabase().relinkTrack (id, dest.getFullPathName(), dest.getSize(),
                                                                               dest.getLastModificationTime().toMilliseconds());
                                  ++done;
                              }

                              if (move)
                                  self->library.reload();
                              self->statusLabel.setText (juce::String (done) + (move ? " moved" : " copied")
                                                             + (failed > 0 ? ", " + juce::String (failed) + " skipped (missing or name already exists)" : juce::String()),
                                                         juce::dontSendNotification);
                          });
}

void LibraryBrowser::locateFile (const TrackRecord& track)
{
    const auto id = track.id;
    chooser = std::make_unique<juce::FileChooser> ("Locate " + track.getFile().getFileName(), track.getFile().getParentDirectory());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::WeakReference<LibraryBrowser> (this), id] (const juce::FileChooser& fc)
                          {
                              auto* self = safe.get();
                              const auto file = fc.getResult();
                              if (self == nullptr || ! TrackMetadata::isSupportedAudioFile (file))
                                  return;
                              if (self->library.findTrackByPath (file.getFullPathName()) != nullptr)
                              {
                                  self->statusLabel.setText ("That file is already in the library.", juce::dontSendNotification);
                                  return;
                              }
                              self->library.getDatabase().relinkTrack (id, file.getFullPathName(), file.getSize(),
                                                                       file.getLastModificationTime().toMilliseconds());
                              self->library.reload();
                          });
}

void LibraryBrowser::saveTrackEdits (const TrackRecord& edited, bool writeToFile)
{
    juce::String error;
    const bool ok = library.updateTrack (edited, writeToFile, &error);
    infoPanel.setTrack (library.findTrack (edited.id));
    statusLabel.setText (ok ? "Saved." : (error.isNotEmpty() ? error : juce::String ("Could not save.")), juce::dontSendNotification);
}

} // namespace pathboard
