#pragma once

#include "Spotify/SpotifyMatch.h"
#include <functional>
#include <juce_data_structures/juce_data_structures.h>

namespace pathboard::spotify
{

// Read-only access to the user's own Spotify library (playlists, liked songs) through the Spotify Web API.
// Spotify gives no access to the audio, so this is for browsing and matching against local files only.
// Sign-in: OAuth "authorization code with PKCE" - no client secret, the user's own app (client ID) from the
// Spotify developer dashboard. The browser returns to http://127.0.0.1:<redirectPort>/callback.
// The refresh token is kept in the settings file (plain text, like the rest of the settings).
// All public methods are for the message thread; callbacks arrive on the message thread.
class SpotifyClient
{
public:
    static constexpr int redirectPort = 8898;
    static juce::String getRedirectUri() { return "http://127.0.0.1:" + juce::String (redirectPort) + "/callback"; }

    explicit SpotifyClient (juce::PropertiesFile& settings);
    ~SpotifyClient();

    void setClientId (const juce::String& id);
    juce::String getClientId() const;
    bool isLinked() const;                      // a refresh token is stored
    void unlink();

    // Opens the browser for the consent page and waits (up to 3 minutes) for the return. error is empty on success.
    void link (std::function<void (const juce::String& error)> done);

    struct Source { juce::String id, name; bool liked = false; int total = -1; };
    void fetchSources (std::function<void (std::vector<Source> sources, const juce::String& error)> done);
    void fetchTracks (const Source& source, std::function<void (std::vector<SpotifyTrack> tracks, const juce::String& error)> done);

    // Pure helpers (tested).
    static juce::String makeCodeChallenge (const juce::String& verifier);
    static std::vector<SpotifyTrack> parseTrackItems (const juce::var& items);

private:
    struct Http { int status = 0; juce::String body; juce::String error; };
    Http post (const juce::String& url, const juce::String& form);
    Http get (const juce::String& url);
    bool ensureAccessToken (juce::String& error);
    void runLink (juce::String clientId, juce::String verifier, juce::String state, std::function<void (const juce::String&)> done);

    juce::PropertiesFile& settings;
    juce::CriticalSection tokenLock;
    juce::String accessToken;
    juce::int64 accessTokenExpiresAtMs = 0;
    std::atomic<bool> shuttingDown { false };
    juce::ThreadPool pool { 1 };

    JUCE_DECLARE_WEAK_REFERENCEABLE (SpotifyClient)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpotifyClient)
};

} // namespace pathboard::spotify
