# VocalSynthCore: real-time DSP building blocks without any JUCE, host or GUI dependency.
# Everything here must be allocation-free after prepare() and safe to call on the audio thread.
add_library(VocalSynthCore STATIC
    src/Pitch.cpp)

target_include_directories(VocalSynthCore PUBLIC include)
target_compile_features(VocalSynthCore PUBLIC cxx_std_23)

# Position-independent code: the core is linked into plugin shared libraries
set_target_properties(VocalSynthCore PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER "VocalSynth")

if (MSVC)
    target_compile_options(VocalSynthCore PRIVATE /W4 /Zc:__cplusplus)
else ()
    target_compile_options(VocalSynthCore PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif ()
