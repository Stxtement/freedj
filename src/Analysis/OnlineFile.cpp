#include "AudioAnalysis.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace pathboard::analysis
{

bool isOnlineOnly (const juce::File& file)
{
#if JUCE_WINDOWS
    const DWORD attributes = GetFileAttributesW (file.getFullPathName().toWideCharPointer());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return false;

    // RECALL_ON_DATA_ACCESS / RECALL_ON_OPEN / OFFLINE: content is fetched from the cloud when read.
    constexpr DWORD recallOnDataAccess = 0x00400000, recallOnOpen = 0x00040000, offline = 0x00001000;
    return (attributes & (recallOnDataAccess | recallOnOpen | offline)) != 0;
#else
    juce::ignoreUnused (file);
    return false;
#endif
}

} // namespace pathboard::analysis
