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
    src/CaptureMode.cpp
    # 0.4.0 synth (docs/SYNTH-DESIGN.md 19): A = wavetable, B = voice, C = engine + FX + expression
    include/vocalsynth/synth/RealFft.h
    include/vocalsynth/synth/Wavetable.h
    include/vocalsynth/synth/WavetableBuilder.h
    include/vocalsynth/synth/FactoryWavetables.h
    include/vocalsynth/synth/WavReader.h
    include/vocalsynth/synth/WavetableOscillator.h
    include/vocalsynth/synth/SubOscillator.h
    include/vocalsynth/synth/NoiseSource.h
    include/vocalsynth/synth/SvfFilter.h
    include/vocalsynth/synth/Envelope.h
    include/vocalsynth/synth/Lfo.h
    include/vocalsynth/synth/ModMatrix.h
    include/vocalsynth/synth/SynthSettings.h
    include/vocalsynth/synth/SynthVoice.h
    include/vocalsynth/synth/VoiceAllocator.h
    include/vocalsynth/synth/SynthEngine.h
    include/vocalsynth/synth/fx/Chorus.h
    include/vocalsynth/synth/fx/StereoDelay.h
    include/vocalsynth/synth/fx/FdnReverb.h
    include/vocalsynth/ExpressionFrame.h
    include/vocalsynth/ExpressionTracker.h
    src/synth/RealFft.cpp
    src/synth/Wavetable.cpp
    src/synth/WavetableBuilder.cpp
    src/synth/FactoryWavetables.cpp
    src/synth/WavReader.cpp
    src/synth/WavetableOscillator.cpp
    src/synth/SubOscillator.cpp
    src/synth/NoiseSource.cpp
    src/synth/SvfFilter.cpp
    src/synth/Envelope.cpp
    src/synth/Lfo.cpp
    src/synth/ModMatrix.cpp
    src/synth/SynthSettings.cpp
    src/synth/SynthVoice.cpp
    src/synth/VoiceAllocator.cpp
    src/synth/SynthEngine.cpp
    src/synth/fx/Chorus.cpp
    src/synth/fx/StereoDelay.cpp
    src/synth/fx/FdnReverb.cpp
    src/ExpressionTracker.cpp)

target_include_directories(VocalSynthCore PUBLIC include)
target_compile_features(VocalSynthCore PUBLIC cxx_std_23)

# Position-independent code: the core is linked into plugin shared libraries
set_target_properties(VocalSynthCore PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER "VocalSynth")

if (MSVC)
    target_compile_options(VocalSynthCore PRIVATE /W4 /Zc:__cplusplus)
else ()
    target_compile_options(VocalSynthCore PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif ()
