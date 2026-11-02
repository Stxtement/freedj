#include "SpotifyClient.h"
#include <juce_cryptography/juce_cryptography.h>

namespace pathboard::spotify
{

namespace
{
    constexpr const char* clientIdKey = "spotifyClientId";
    constexpr const char* refreshTokenKey = "spotifyRefreshToken";

    juce::String base64Url (const void* data, size_t size)
    {
        return juce::Base64::toBase64 (data, size).replace ("+", "-").replace ("/", "_").replace ("=", "");
    }

    juce::String randomToken (int bytes)
    {
        juce::MemoryBlock block ((size_t) bytes);
        auto& random = juce::Random::getSystemRandom();
        for (int i = 0; i < bytes; ++i)
            static_cast<juce::uint8*> (block.getData())[i] = (juce::uint8) random.nextInt (256);
        return base64Url (block.getData(), block.getSize());
    }

    juce::String formEncode (std::initializer_list<std::pair<const char*, juce::String>> fields)
    {
        juce::String out;
        for (const auto& [key, value] : fields)
            out << (out.isEmpty() ? "" : "&") << key << "=" << juce::URL::addEscapeChars (value, true);
        return out;
    }

    juce::String describe (int status, const juce::var& json)
    {
        auto text = "Spotify answered " + juce::String (status);
        if (const auto d = json["error_description"].toString(); d.isNotEmpty()) return text + ": " + d;
        if (const auto e = json["error"]; e.isObject() && e["message"].toString().isNotEmpty()) return text + ": " + e["message"].toString();
        if (const auto e = json["error"].toString(); e.isNotEmpty()) return text + ": " + e;
        return text;
    }
}

SpotifyClient::SpotifyClient (juce::PropertiesFile& s) : settings (s) {}

SpotifyClient::~SpotifyClient()
{
    shuttingDown = true;
    pool.removeAllJobs (true, 10000);
}

void SpotifyClient::setClientId (const juce::String& id)
{
    const auto trimmed = id.trim();
    if (trimmed != getClientId())
    {
        settings.setValue (clientIdKey, trimmed);
        unlink();   // tokens belong to one client ID
    }
}

juce::String SpotifyClient::getClientId() const { return settings.getValue (clientIdKey, {}); }
bool SpotifyClient::isLinked() const { return settings.getValue (refreshTokenKey, {}).isNotEmpty(); }

void SpotifyClient::unlink()
{
    settings.removeValue (refreshTokenKey);
    const juce::ScopedLock sl (tokenLock);
    accessToken.clear();
    accessTokenExpiresAtMs = 0;
}

juce::String SpotifyClient::makeCodeChallenge (const juce::String& verifier)
{
    const juce::SHA256 hash (verifier.toRawUTF8(), verifier.getNumBytesAsUTF8());
    const auto raw = hash.getRawData();
    return base64Url (raw.getData(), raw.getSize());
}

//==============================================================================
SpotifyClient::Http SpotifyClient::post (const juce::String& url, const juce::String& form)
{
    Http result;
    int status = 0;
    auto stream = juce::URL (url).withPOSTData (form).createInputStream (
        juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
            .withExtraHeaders ("Content-Type: application/x-www-form-urlencoded\r\n")
            .withConnectionTimeoutMs (15000)
            .withStatusCode (&status));
    if (stream == nullptr)
    {
        result.error = "Spotify cannot be reached (no internet connection?)";
        return result;
    }
    result.status = status;
    result.body = stream->readEntireStreamAsString();
    return result;
}

SpotifyClient::Http SpotifyClient::get (const juce::String& url)
{
    Http result;
    for (int attempt = 0; attempt < 4 && ! shuttingDown.load(); ++attempt)
    {
        juce::String token;
        {
            const juce::ScopedLock sl (tokenLock);
            token = accessToken;
        }

        int status = 0;
        juce::StringPairArray headers;
        auto stream = juce::URL (url).createInputStream (
            juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                .withExtraHeaders ("Authorization: Bearer " + token + "\r\n")
                .withConnectionTimeoutMs (15000)
                .withResponseHeaders (&headers)
                .withStatusCode (&status));
        if (stream == nullptr)
        {
            result.error = "Spotify cannot be reached (no internet connection?)";
            return result;
        }

        result.status = status;
        result.body = stream->readEntireStreamAsString();
        if (status != 429)
            return result;

        // Rate limited: wait as long as Spotify asks (at most 10 s), then try again.
        const int waitSeconds = juce::jlimit (1, 10, headers.getValue ("Retry-After", "2").getIntValue());
        for (int i = 0; i < waitSeconds * 10 && ! shuttingDown.load(); ++i)
            juce::Thread::sleep (100);
    }
    return result;
}

bool SpotifyClient::ensureAccessToken (juce::String& error)
{
    {
        const juce::ScopedLock sl (tokenLock);
        if (accessToken.isNotEmpty() && juce::Time::currentTimeMillis() < accessTokenExpiresAtMs - 30000)
            return true;
    }

    const auto refresh = settings.getValue (refreshTokenKey, {});
    const auto clientId = getClientId();
    if (refresh.isEmpty() || clientId.isEmpty())
    {
        error = "Not signed in to Spotify.";
        return false;
    }

    const auto r = post ("https://accounts.spotify.com/api/token",
                         formEncode ({ { "grant_type", "refresh_token" }, { "refresh_token", refresh }, { "client_id", clientId } }));
    if (r.error.isNotEmpty())
    {
        error = r.error;
        return false;
    }
    const auto json = juce::JSON::parse (r.body);
    if (r.status != 200 || json["access_token"].toString().isEmpty())
    {
        error = describe (r.status, json) + " - sign in again.";
        if (json["error"].toString() == "invalid_grant")
            unlink();
        return false;
    }

    const juce::ScopedLock sl (tokenLock);
    accessToken = json["access_token"].toString();
    accessTokenExpiresAtMs = juce::Time::currentTimeMillis() + (juce::int64) ((double) json["expires_in"] * 1000.0);
    if (const auto rotated = json["refresh_token"].toString(); rotated.isNotEmpty())
        settings.setValue (refreshTokenKey, rotated);
    return true;
}

//==============================================================================
void SpotifyClient::link (std::function<void (const juce::String&)> done)
{
    const auto clientId = getClientId();
    if (clientId.isEmpty())
    {
        done ("Enter the client ID of your Spotify app first.");
        return;
    }

    const auto verifier = randomToken (48);
    const auto state = randomToken (12);
    const auto url = "https://accounts.spotify.com/authorize?"
                     + formEncode ({ { "response_type", "code" }, { "client_id", clientId },
                                     { "scope", "playlist-read-private playlist-read-collaborative user-library-read" },
                                     { "redirect_uri", getRedirectUri() }, { "state", state },
                                     { "code_challenge_method", "S256" }, { "code_challenge", makeCodeChallenge (verifier) } });

    if (! juce::URL (url).launchInDefaultBrowser())
    {
        done ("The browser could not be opened.");
        return;
    }

    pool.addJob ([this, clientId, verifier, state, done = std::move (done)]() mutable
                 { runLink (clientId, verifier, state, std::move (done)); });
}

void SpotifyClient::runLink (juce::String clientId, juce::String verifier, juce::String state, std::function<void (const juce::String&)> done)
{
    juce::String error;
    juce::String code;

    juce::StreamingSocket listener;
    if (! listener.createListener (redirectPort, "127.0.0.1"))
    {
        error = "Port " + juce::String (redirectPort) + " is in use - close the program using it and try again.";
    }
    else
    {
        const auto deadline = juce::Time::getMillisecondCounter() + 180000;
        std::unique_ptr<juce::StreamingSocket> connection;
        while (connection == nullptr && juce::Time::getMillisecondCounter() < deadline && ! shuttingDown.load())
            if (listener.waitUntilReady (true, 500) > 0)
                connection.reset (listener.waitForNextConnection());
        if (connection == nullptr)
        {
            error = "No answer from the browser (timed out).";
        }
        else
        {
            char buffer[4096] = {};
            const int n = connection->read (buffer, sizeof (buffer) - 1, false);
            const auto request = juce::String::fromUTF8 (buffer, juce::jmax (0, n));
            const auto line = request.upToFirstOccurrenceOf ("\r\n", false, false);   // GET /callback?code=..&state=.. HTTP/1.1
            const auto query = line.fromFirstOccurrenceOf ("?", false, false).upToLastOccurrenceOf (" ", false, false);

            juce::String returnedState, spotifyError;
            for (const auto& pair : juce::StringArray::fromTokens (query, "&", ""))
            {
                const auto key = pair.upToFirstOccurrenceOf ("=", false, false);
                const auto value = juce::URL::removeEscapeChars (pair.fromFirstOccurrenceOf ("=", false, false));
                if (key == "code") code = value;
                else if (key == "state") returnedState = value;
                else if (key == "error") spotifyError = value;
            }

            if (spotifyError.isNotEmpty())  error = "Spotify: " + spotifyError;
            else if (returnedState != state) error = "The answer did not match the request (wrong state).";
            else if (code.isEmpty())        error = "Spotify returned no code.";

            const juce::String page = error.isEmpty() ? "Signed in. You can close this tab and go back to Free DJ."
                                                      : "Sign-in failed: " + error;
            const auto html = "<!doctype html><meta charset=utf-8><title>Free DJ</title><body style=\"font-family:sans-serif;margin:3em\"><h2>" + page + "</h2>";
            const auto response = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: "
                                  + juce::String (html.getNumBytesAsUTF8()) + "\r\n\r\n" + html;
            connection->write (response.toRawUTF8(), (int) response.getNumBytesAsUTF8());
        }
    }

    if (error.isEmpty())
    {
        const auto r = post ("https://accounts.spotify.com/api/token",
                             formEncode ({ { "grant_type", "authorization_code" }, { "code", code }, { "redirect_uri", getRedirectUri() },
                                           { "client_id", clientId }, { "code_verifier", verifier } }));
        const auto json = juce::JSON::parse (r.body);
        if (r.error.isNotEmpty())
            error = r.error;
        else if (r.status != 200 || json["refresh_token"].toString().isEmpty())
            error = describe (r.status, json);
        else
        {
            juce::MessageManager::callAsync ([weak = juce::WeakReference<SpotifyClient> (this), token = json["refresh_token"].toString()]
                                             { if (auto* self = weak.get()) self->settings.setValue (refreshTokenKey, token); });
            const juce::ScopedLock sl (tokenLock);
            accessToken = json["access_token"].toString();
            accessTokenExpiresAtMs = juce::Time::currentTimeMillis() + (juce::int64) ((double) json["expires_in"] * 1000.0);
        }
    }

    juce::MessageManager::callAsync ([weak = juce::WeakReference<SpotifyClient> (this), done = std::move (done), error]
                                     { if (weak.get() != nullptr) done (error); });
}

//==============================================================================
std::vector<SpotifyTrack> SpotifyClient::parseTrackItems (const juce::var& items)
{
    std::vector<SpotifyTrack> tracks;
    if (const auto* array = items.getArray())
        for (const auto& entry : *array)
        {
            // Playlist entries hold the track as "item" (new) or "track" (old); liked songs as "track".
            auto t = entry["item"].isObject() ? entry["item"] : entry["track"];
            if (! t.isObject() || t["type"].toString() == "episode" || t["is_local"])
                continue;

            SpotifyTrack s;
            s.id = t["id"].toString();
            s.title = t["name"].toString();
            s.album = t["album"]["name"].toString();
            s.durationMs = (int) t["duration_ms"];
            juce::StringArray artists;
            if (const auto* a = t["artists"].getArray())
                for (const auto& artist : *a)
                    artists.add (artist["name"].toString());
            s.artist = artists.joinIntoString (", ");
            if (s.title.isNotEmpty())
                tracks.push_back (std::move (s));
        }
    return tracks;
}

void SpotifyClient::fetchSources (std::function<void (std::vector<Source>, const juce::String&)> done)
{
    pool.addJob ([this, done = std::move (done)]
    {
        std::vector<Source> sources;
        juce::String error;
        sources.push_back ({ "liked", "Liked songs", true, -1 });

        if (ensureAccessToken (error))
        {
            juce::String next = "https://api.spotify.com/v1/me/playlists?limit=50";
            while (next.isNotEmpty() && ! shuttingDown.load())
            {
                const auto r = get (next);
                const auto json = juce::JSON::parse (r.body);
                if (r.error.isNotEmpty()) { error = r.error; break; }
                if (r.status != 200) { error = describe (r.status, json); break; }

                if (const auto* items = json["items"].getArray())
                    for (const auto& p : *items)
                        if (p.isObject())
                        {
                            const auto total = p["items"].isObject() ? p["items"]["total"] : p["tracks"]["total"];
                            sources.push_back ({ p["id"].toString(), p["name"].toString(), false, total.isVoid() ? -1 : (int) total });
                        }
                next = json["next"].toString();
            }
        }

        juce::MessageManager::callAsync ([weak = juce::WeakReference<SpotifyClient> (this), done, sources = std::move (sources), error]() mutable
                                         { if (weak.get() != nullptr) done (std::move (sources), error); });
    });
}

void SpotifyClient::fetchTracks (const Source& source, std::function<void (std::vector<SpotifyTrack>, const juce::String&)> done)
{
    pool.addJob ([this, source, done = std::move (done)]
    {
        std::vector<SpotifyTrack> tracks;
        juce::String error;

        if (ensureAccessToken (error))
        {
            juce::String next = source.liked ? juce::String ("https://api.spotify.com/v1/me/tracks?limit=50")
                                             : "https://api.spotify.com/v1/playlists/" + source.id + "/items?limit=50";
            while (next.isNotEmpty() && ! shuttingDown.load())
            {
                const auto r = get (next);
                const auto json = juce::JSON::parse (r.body);
                if (r.error.isNotEmpty()) { error = r.error; break; }
                if (r.status == 403 && ! source.liked)
                {
                    error = "Spotify does not release this playlist (only your own and shared playlists can be read).";
                    break;
                }
                if (r.status != 200) { error = describe (r.status, json); break; }

                auto page = parseTrackItems (json["items"]);
                tracks.insert (tracks.end(), std::make_move_iterator (page.begin()), std::make_move_iterator (page.end()));
                next = json["next"].toString();
            }
        }

        juce::MessageManager::callAsync ([weak = juce::WeakReference<SpotifyClient> (this), done, tracks = std::move (tracks), error]() mutable
                                         { if (weak.get() != nullptr) done (std::move (tracks), error); });
    });
}

} // namespace pathboard::spotify
