#pragma once

#include <juce_core/juce_core.h>
#include <iostream>

namespace testsupport
{
inline int failures = 0;

inline void check (bool condition, const juce::String& name, const juce::String& detail = {})
{
    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << name;
    if (detail.isNotEmpty())
        std::cout << "  (" << detail << ")";
    std::cout << std::endl;
    if (! condition)
        ++failures;
}
}

int seedLibrary (const juce::File& dbFile, const juce::File& folder, int count);
int runEngineTests (int argc, char* argv[]);
int runLibraryTests();
int runAnalysisTests();
int runAnalysisStorageTests();
int runStemTests();
int runControllerTests();
int runGenreTests();
int runMatchTests();
int runEffectTests();
int runTidyTests();
int runGenreProbe (const juce::StringArray& files);
int runStemProbe (const juce::File& audioFile, double start, double seconds);
