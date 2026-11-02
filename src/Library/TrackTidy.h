#pragma once

#include "Library/TrackRecord.h"

namespace pathboard
{

// Cleans the clutter that DJ pools and download sites leave in titles and artists, so the library reads well:
//   "Get lucky (Remix).mp3_PN"            -> "Get lucky"  + remix "Remix"
//   artist "10A - 128 - Pink"             -> artist "Pink" (key 10A and BPM 128 are taken only where the track has none)
//   "Lockdown myfreemp3.vip"              -> "Lockdown"
//   "Hi_Tack_-_Its_Alright_[djsite.com]"  -> artist "Hi Tack", title "Its Alright"
// Nothing is invented: only text that is clearly clutter is removed, and an "Artist - Title" split happens only
// when the artist is empty. Values the user fixed by hand (manual BPM / key) are not touched.
// Returns true when the record changed.
bool tidyTrackText (TrackRecord& track);

juce::String tidyTitleText (const juce::String& text);   // exposed for tests

} // namespace pathboard
