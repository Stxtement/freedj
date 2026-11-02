#include "SpotifyComponent.h"
#include "UI/DJLookAndFeel.h"

namespace pathboard::spotify
{

SpotifyComponent::SpotifyComponent (SpotifyClient& c, LibraryManager& lib) : client (c), library (lib)
{
    clientIdLabel.setColour (juce::Label::textColourId, Colours::textDim);
    clientIdBox.setText (client.getClientId(), false);
    clientIdBox.setTextToShowWhenEmpty ("paste the client ID from developer.spotify.com/dashboard", Colours::textDim);
    clientIdBox.setTooltip ("Create an app at developer.spotify.com/dashboard, add the redirect URI shown below, and paste its client ID here. "
                            "The free development mode allows your own account.");

    help.setText ("Redirect URI to enter in the app settings: " + SpotifyClient::getRedirectUri()
                      + "\nSpotify gives no access to the audio: this lists your playlists and finds the songs you own as files.",
                  juce::dontSendNotification);
    help.setColour (juce::Label::textColourId, Colours::textDim);
    help.setMinimumHorizontalScale (1.0f);

    linkButton.onClick = [this]
    {
        if (client.isLinked())
        {
            client.unlink();
            sources.clear();
            rows.clear();
            sourceBox.clear();
            list.updateContent();
            setStatus ("Signed out.");
            updateLinkState();
            return;
        }

        client.setClientId (clientIdBox.getText());
        busy = true;
        setStatus ("Complete the sign-in in your browser ...");
        updateLinkState();
        client.link ([safe = juce::Component::SafePointer<SpotifyComponent> (this)] (const juce::String& error)
        {
            if (safe == nullptr)
                return;
            safe->busy = false;
            safe->updateLinkState();
            if (error.isNotEmpty())
                safe->setStatus (error, true);
            else
                safe->loadSources();
        });
    };

    reloadButton.onClick = [this] { loadTracks(); };
    sourceBox.onChange = [this] { loadTracks(); };
    playlistButton.onClick = [this] { createPlaylist(); };

    list.setRowHeight (24);
    list.setColour (juce::ListBox::backgroundColourId, Colours::panelRaised);
    status.setColour (juce::Label::textColourId, Colours::textDim);

    for (auto* comp : std::initializer_list<juce::Component*> { &clientIdLabel, &clientIdBox, &linkButton, &help, &sourceBox,
                                                                &reloadButton, &playlistButton, &list, &status })
        addAndMakeVisible (comp);

    updateLinkState();
    if (client.isLinked())
        loadSources();
}

void SpotifyComponent::resized()
{
    auto area = getLocalBounds().reduced (12);
    clientIdLabel.setBounds (area.removeFromTop (20));
    auto idRow = area.removeFromTop (28);
    linkButton.setBounds (idRow.removeFromRight (110).reduced (2, 0));
    clientIdBox.setBounds (idRow.reduced (2, 0));
    help.setBounds (area.removeFromTop (44));

    auto row = area.removeFromTop (28);
    reloadButton.setBounds (row.removeFromRight (90).reduced (2, 0));
    sourceBox.setBounds (row.reduced (2, 0));
    area.removeFromTop (6);

    status.setBounds (area.removeFromBottom (22));
    playlistButton.setBounds (area.removeFromBottom (30).reduced (2, 0));
    area.removeFromBottom (6);
    list.setBounds (area);
}

void SpotifyComponent::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (! juce::isPositiveAndBelow (row, (int) rows.size()))
        return;
    const auto& r = rows[(size_t) row];

    if (selected)
        g.fillAll (Colours::panel.brighter (0.15f));

    const bool local = r.localId != 0;
    g.setColour (local ? Colours::text : Colours::textDim);
    g.setFont (13.0f);
    g.drawText (r.track.artist + "  -  " + r.track.title, 8, 0, width - 90, height, juce::Justification::centredLeft, true);

    g.setColour (local ? Colours::play : Colours::textDim.withAlpha (0.6f));
    g.drawText (local ? "LOCAL" : "not local", width - 80, 0, 72, height, juce::Justification::centredRight);
}

void SpotifyComponent::updateLinkState()
{
    const bool linked = client.isLinked();
    linkButton.setButtonText (linked ? "SIGN OUT" : "SIGN IN");
    linkButton.setEnabled (! busy);
    clientIdBox.setEnabled (! linked && ! busy);
    sourceBox.setEnabled (linked && ! busy);
    reloadButton.setEnabled (linked && ! busy);
    playlistButton.setEnabled (linked && ! busy && matchCount > 0);
}

void SpotifyComponent::setStatus (const juce::String& text, bool error)
{
    status.setText (text, juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, error ? Colours::danger : Colours::textDim);
}

void SpotifyComponent::loadSources()
{
    busy = true;
    setStatus ("Loading your playlists ...");
    updateLinkState();
    client.fetchSources ([safe = juce::Component::SafePointer<SpotifyComponent> (this)] (std::vector<SpotifyClient::Source> found, const juce::String& error)
    {
        if (safe == nullptr)
            return;
        auto& self = *safe;
        self.busy = false;
        self.sources = std::move (found);
        self.sourceBox.clear (juce::dontSendNotification);
        for (size_t i = 0; i < self.sources.size(); ++i)
            self.sourceBox.addItem (self.sources[i].name + (self.sources[i].total >= 0 ? "  (" + juce::String (self.sources[i].total) + ")" : juce::String()), (int) i + 1);
        self.updateLinkState();
        if (error.isNotEmpty())
            self.setStatus (error, true);
        if (! self.sources.empty())
            self.sourceBox.setSelectedId (1, juce::sendNotificationSync);   // loads the liked songs
    });
}

void SpotifyComponent::loadTracks()
{
    const int index = sourceBox.getSelectedId() - 1;
    if (! juce::isPositiveAndBelow (index, (int) sources.size()))
        return;

    busy = true;
    rows.clear();
    matchCount = 0;
    list.updateContent();
    setStatus ("Loading the songs ...");
    updateLinkState();

    client.fetchTracks (sources[(size_t) index], [safe = juce::Component::SafePointer<SpotifyComponent> (this)]
                                                 (std::vector<SpotifyTrack> tracks, const juce::String& error)
    {
        if (safe == nullptr)
            return;
        auto& self = *safe;
        self.busy = false;

        const LocalIndex index (self.library.getTracks());
        self.rows.clear();
        self.matchCount = 0;
        for (auto& t : tracks)
        {
            Row r;
            r.localId = index.find (t);
            r.track = std::move (t);
            if (r.localId != 0)
                ++self.matchCount;
            self.rows.push_back (std::move (r));
        }
        self.list.updateContent();
        self.list.repaint();
        self.updateLinkState();

        if (error.isNotEmpty())
            self.setStatus (error, true);
        else
            self.setStatus (juce::String (self.rows.size()) + " songs, " + juce::String (self.matchCount) + " of them exist as local files.");
    });
}

void SpotifyComponent::createPlaylist()
{
    const int index = sourceBox.getSelectedId() - 1;
    if (! juce::isPositiveAndBelow (index, (int) sources.size()))
        return;

    std::vector<juce::int64> ids;
    for (const auto& r : rows)
        if (r.localId != 0)
            ids.push_back (r.localId);
    if (ids.empty())
        return;

    const auto id = library.createPlaylist ("Spotify - " + sources[(size_t) index].name);
    if (id > 0 && library.addToPlaylist (id, ids))
        setStatus ("Playlist \"Spotify - " + sources[(size_t) index].name + "\" created with " + juce::String ((int) ids.size()) + " songs.");
    else
        setStatus ("The playlist could not be created.", true);
}

} // namespace pathboard::spotify
