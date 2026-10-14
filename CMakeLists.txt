cmake_minimum_required(VERSION 3.25)

# Must be set before project(): macOS minimum version (JUCE 9 supports 10.13+)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

# Universal binaries only for Release builds on CI (they double the build time)
if (APPLE AND DEFINED ENV{CI} AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
endif ()

# Static MSVC runtime, so users don't need a VC++ redistributable
if (WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif ()

file(STRINGS VERSION VOCALSYNTH_VERSION LIMIT_COUNT 1)
project(VocalSynth VERSION ${VOCALSYNTH_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS YES)

option(VOCALSYNTH_BUILD_TESTS "Build the unit tests" ON)

# --- Dependencies (pinned submodules, see .gitmodules) -----------------------
add_subdirectory(JUCE)
add_subdirectory(modules/clap-juce-extensions EXCLUDE_FROM_ALL)

# --- DSP core: plain C++, no JUCE, testable headless ---------------------------
add_subdirectory(core)

# --- Plugins: "VocalSynth FX" and "VocalSynth Instrument" ---------------------
include(cmake/VocalSynthPlugin.cmake)

# --- Tests ---------------------------------------------------------------------
if (VOCALSYNTH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()
