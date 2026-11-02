cmake_minimum_required(VERSION 3.22)

project(FreeDJ VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#==============================================================================
# Third-party code (all vendored in libs/)
add_subdirectory(libs/JUCE)

# SQLite amalgamation (public domain)
add_library(sqlite3 STATIC libs/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC libs/sqlite)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_DQS=0
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_DEPRECATED
    SQLITE_DEFAULT_FOREIGN_KEYS=1)

# TagLib (LGPL/MPL) - tags and cover art for MP3/FLAC/M4A/OGG/WAV/AIFF
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_BINDINGS OFF CACHE BOOL "" FORCE)
set(WITH_ZLIB OFF CACHE BOOL "" FORCE)
add_subdirectory(libs/taglib EXCLUDE_FROM_ALL)

# The tag target only exports install-tree include paths; point at the source tree ourselves.
file(GLOB TAGLIB_SUBDIRS LIST_DIRECTORIES true "${CMAKE_SOURCE_DIR}/libs/taglib/taglib/*")
set(TAGLIB_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/libs/taglib/taglib" "${CMAKE_BINARY_DIR}/libs/taglib")
foreach(dir ${TAGLIB_SUBDIRS})
    if(IS_DIRECTORY "${dir}")
        list(APPEND TAGLIB_INCLUDE_DIRS "${dir}")
        file(GLOB sub LIST_DIRECTORIES true "${dir}/*")
        foreach(s ${sub})
            if(IS_DIRECTORY "${s}")
                list(APPEND TAGLIB_INCLUDE_DIRS "${s}")
            endif()
        endforeach()
    endif()
endforeach()

#==============================================================================
# Engine + library code shared by the app and the tests
set(JUCE_COMMON_DEFS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
    JUCE_USE_WINDOWS_MEDIA_FORMAT=1
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=1)

# ONNX Runtime with the DirectML GPU provider (MIT), vendored in libs/onnxruntime-dml
set(ORT_DIR "${CMAKE_SOURCE_DIR}/libs/onnxruntime-dml" CACHE PATH "ONNX Runtime (DirectML build) folder with include/lib/bin")
add_library(onnxruntime SHARED IMPORTED)
set_target_properties(onnxruntime PROPERTIES
    IMPORTED_LOCATION "${ORT_DIR}/bin/onnxruntime.dll"
    IMPORTED_IMPLIB "${ORT_DIR}/lib/onnxruntime.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${ORT_DIR}/include")
set(ORT_RUNTIME_FILES
    "${ORT_DIR}/bin/onnxruntime.dll"
    "${ORT_DIR}/bin/onnxruntime_providers_shared.dll"
    "${ORT_DIR}/bin/DirectML.dll")

function(pathboard_copy_runtime target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ORT_RUNTIME_FILES} "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
endfunction()

add_library(pathboard_core STATIC
    src/Genre/GenreClassifier.cpp
    src/Stems/StemCache.cpp
    src/Stems/StemManager.cpp
    src/Stems/StemSeparator.cpp
    src/Controller/ControllerManager.cpp
    src/Controller/ControllerMapping.cpp
    src/Analysis/AnalysisManager.cpp
    src/Analysis/AudioAnalysis.cpp
    src/Analysis/OnlineFile.cpp
    src/Audio/DeckEngine.cpp
    src/Mixer/MixerEngine.cpp
    src/Library/LibraryDatabase.cpp
    src/Library/LibraryManager.cpp
    src/Library/LibraryScanner.cpp
    src/Library/TrackMetadata.cpp
    src/Library/TrackQuery.cpp)

target_include_directories(pathboard_core PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_include_directories(pathboard_core PRIVATE ${TAGLIB_INCLUDE_DIRS})
target_compile_definitions(pathboard_core PUBLIC ${JUCE_COMMON_DEFS} TAGLIB_STATIC)
target_link_libraries(pathboard_core PUBLIC
    sqlite3
    tag
    onnxruntime
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_cryptography
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    PRIVATE
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

#==============================================================================
# The application
juce_add_gui_app(FreeDJ
    PRODUCT_NAME "Free DJ"
    COMPANY_NAME "FreeDJ"
    ICON_BIG ""
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    MICROPHONE_PERMISSION_ENABLED TRUE)

file(GLOB_RECURSE PATHBOARD_APP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/*.h")
# Engine and library sources come from pathboard_core.
list(FILTER PATHBOARD_APP_SOURCES EXCLUDE REGEX "src/(Genre/GenreClassifier|Controller/(ControllerManager|ControllerMapping)|Stems/(StemSeparator|StemCache|StemManager)|Analysis/(AnalysisManager|AudioAnalysis|OnlineFile)|Audio/DeckEngine|Mixer/MixerEngine|Library/(LibraryDatabase|LibraryManager|LibraryScanner|TrackMetadata|TrackQuery))\\.cpp$")

target_sources(FreeDJ PRIVATE ${PATHBOARD_APP_SOURCES})

target_compile_definitions(FreeDJ PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:FreeDJ,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:FreeDJ,JUCE_VERSION>")

pathboard_copy_runtime(FreeDJ)

target_link_libraries(FreeDJ PRIVATE
    pathboard_core
    juce::juce_audio_processors
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

#==============================================================================
# Tests (console, no sound card needed)
juce_add_console_app(FreeDJTests PRODUCT_NAME "FreeDJTests")

target_sources(FreeDJTests PRIVATE
    tests/EngineTests.cpp
    tests/LibraryTests.cpp
    tests/AnalysisTests.cpp
    tests/StemTests.cpp
    tests/ControllerTests.cpp
    tests/GenreTests.cpp
    tests/TestMain.cpp)

target_compile_definitions(FreeDJTests PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1)

pathboard_copy_runtime(FreeDJTests)

target_link_libraries(FreeDJTests PRIVATE
    pathboard_core
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
