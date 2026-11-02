#pragma once

#include "Library/LibraryManager.h"
#include "Spotify/SpotifyClient.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace pathboard::spotify
{

// Dialog: sign in, pick a playlist or the liked songs, see which songs exist as local files,
// and turn the local matches into a library playlist.
class SpotifyComponent : public juce::Component, private juce::ListBoxModel
{
public:
    SpotifyComponent (SpotifyClient& client, LibraryManager& library);

    void resized() override;

private:
    struct Row { SpotifyTrack track; juce::int64 localId = 0; };

    int getNumRows() override { return (int) rows.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;

    void updateLinkState();
    void loadSources();
    void loadTracks();
    void createPlaylist();
    void setStatus (const juce::String& text, bool error = false);

    SpotifyClient& client;
    LibraryManager& library;

    juce::Label clientIdLabel { {}, "Client ID of your Spotify app:" };
    juce::TextEditor clientIdBox;
    juce::TextButton linkButton { "SIGN IN" }, reloadButton { "RELOAD" }, playlistButton { "MAKE PLAYLIST FROM LOCAL MATCHES" };
    juce::Label help;
    juce::ComboBox sourceBox;
    juce::ListBox list { {}, this };
    juce::Label status;

    std::vector<SpotifyClient::Source> sources;
    std::vector<Row> rows;
    int matchCount = 0;
    bool busy = false;

    JUCE_DECLARE_WEAK_REFERENCEABLE (SpotifyComponent)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpotifyComponent)
};

} // namespace pathboard::spotify
