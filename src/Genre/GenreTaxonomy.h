#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace pathboard::genre
{

// A genre as main genre + optional sub-genre, e.g. { "House", "Tech House" } or { "Rock", "Alternative Rock" }.
struct GenreLabel
{
    juce::String main;       // "House", "Techno", "Hip Hop", "Rock", ...
    juce::String sub;        // "Tech House" (empty if only the main genre is known)
    juce::String category;   // Discogs top level: "Electronic", "Rock", "Hip Hop", ... (the musical style family)

    bool isEmpty() const { return main.isEmpty(); }
    juce::String toString() const { return sub.isNotEmpty() && sub != main ? main + " / " + sub : main; }
};

// "Electronic---Tech House" -> { House, Tech House, Electronic }
GenreLabel fromDiscogs (const juce::String& discogsLabel);

// Free text from tags or the user: "Tech House", "House / Deep House", "hip-hop; rap", "DnB".
// Unknown words are kept as a main genre (compared by name), nothing is invented.
std::vector<GenreLabel> parseGenreText (const juce::String& text);

// 0..1: how well two genres go together in a DJ set. 1 = same genre, ~0.9 same main genre, lower for related
// genres (House-Techno, Pop-Rock, ...), 0 = unrelated. Returns -1 if either side has no genre at all.
double genreSimilarity (const std::vector<GenreLabel>& a, const std::vector<GenreLabel>& b);

} // namespace pathboard::genre
