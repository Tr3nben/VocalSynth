# VocalSynthCore: real-time DSP building blocks without any JUCE, host or GUI dependency.
# Everything here must be allocation-free after prepare() and safe to call on the audio thread.
add_library(VocalSynthCore STATIC
    include/vocalsynth/BlockSizeHistogram.h
    include/vocalsynth/MonitorSynth.h
    include/vocalsynth/Pitch.h
    include/vocalsynth/TestPatternGenerator.h
    include/vocalsynth/VsEvent.h
    src/MonitorSynth.cpp
    src/Pitch.cpp
    src/TestPatternGenerator.cpp
    # M2 pitch -> MIDI (docs/M2-DESIGN.md)
    include/vocalsynth/VoiceSettings.h
    include/vocalsynth/InputConditioner.h
    include/vocalsynth/Decimator.h
    include/vocalsynth/Framer.h
    include/vocalsynth/PitchDetector.h
    include/vocalsynth/PitchPostProcessor.h
    include/vocalsynth/ScaleQuantizer.h
    include/vocalsynth/NoteSegmenter.h
    include/vocalsynth/BendGenerator.h
    include/vocalsynth/VoicePipeline.h
    src/InputConditioner.cpp
    src/Decimator.cpp
    src/Framer.cpp
    src/PitchDetector.cpp
    src/PitchPostProcessor.cpp
    src/ScaleQuantizer.cpp
    src/NoteSegmenter.cpp
    src/BendGenerator.cpp
    src/VoicePipeline.cpp
    # M3 capture (docs/M3-DESIGN.md): A = capture, B = render
    include/vocalsynth/EventTiming.h
    include/vocalsynth/CaptureRecord.h
    include/vocalsynth/CaptureOptions.h
    include/vocalsynth/SpscRing.h
    include/vocalsynth/CaptureTap.h
    include/vocalsynth/CaptureHistory.h
    include/vocalsynth/TakeTimeline.h
    include/vocalsynth/Take.h
    include/vocalsynth/TakeSegmenter.h
    include/vocalsynth/CaptureDrain.h
    include/vocalsynth/TakeRenderer.h
    include/vocalsynth/SmfWriter.h
    include/vocalsynth/WavWriter.h
    src/CaptureRecord.cpp
    src/CaptureOptions.cpp
    src/CaptureTap.cpp
    src/CaptureHistory.cpp
    src/TakeTimeline.cpp
    src/TakeSegmenter.cpp
    src/CaptureDrain.cpp
    src/TakeRenderer.cpp
    src/SmfWriter.cpp
    src/WavWriter.cpp
    # M3.1 take length and Record mode (docs/M3.1-DESIGN.md)
    include/vocalsynth/CaptureMode.h
    src/CaptureMode.cpp)

target_include_directories(VocalSynthCore PUBLIC include)
target_compile_features(VocalSynthCore PUBLIC cxx_std_23)

# Position-independent code: the core is linked into plugin shared libraries
set_target_properties(VocalSynthCore PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER "VocalSynth")

if (MSVC)
    target_compile_options(VocalSynthCore PRIVATE /W4 /Zc:__cplusplus)
else ()
    target_compile_options(VocalSynthCore PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif ()
