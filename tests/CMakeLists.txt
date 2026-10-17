include(FetchContent)

FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.9.1
    GIT_SHALLOW TRUE
    SYSTEM)
FetchContent_MakeAvailable(Catch2)
include(${catch2_SOURCE_DIR}/extras/Catch.cmake)

# --- Core tests: pure C++, no JUCE ----------------------------------------------
add_executable(VocalSynthCoreTests
    core/BlockSizeHistogramTests.cpp
    core/MonitorSynthTests.cpp
    core/PitchTests.cpp
    core/TestPatternGeneratorTests.cpp
    # M2 pitch -> MIDI (docs/M2-DESIGN.md 6)
    core/TestSignals.h
    core/InputConditionerTests.cpp
    core/DecimatorTests.cpp
    core/FramerTests.cpp
    core/PitchDetectorTests.cpp
    core/PitchPostProcessorTests.cpp
    core/ScaleQuantizerTests.cpp
    core/NoteSegmenterTests.cpp
    core/BendGeneratorTests.cpp
    core/VoicePipelineTests.cpp
    # M3 capture (docs/M3-DESIGN.md 16)
    core/CaptureRecordTests.cpp
    core/CaptureOptionsTests.cpp
    core/SpscRingTests.cpp
    core/CaptureTapTests.cpp
    core/CaptureHistoryTests.cpp
    core/TakeTimelineTests.cpp
    core/TakeSegmenterTests.cpp
    core/CaptureDrainTests.cpp
    core/TakeRendererTests.cpp
    core/SmfWriterTests.cpp
    core/WavWriterTests.cpp
    core/CaptureCompensationTests.cpp)
target_link_libraries(VocalSynthCoreTests PRIVATE VocalSynthCore Catch2::Catch2WithMain)
set_target_properties(VocalSynthCoreTests PROPERTIES FOLDER "Tests")
catch_discover_tests(VocalSynthCoreTests DISCOVERY_MODE PRE_TEST)

# --- Plugin tests: the processor without a host -------------------------------------
# Compiles the shared plugin code with the FX target's JUCE definitions; both variants are
# constructed directly (createPluginFilter is not part of the shared code).
add_executable(VocalSynthPluginTests
    plugin/TestMain.cpp
    plugin/MidiOutputStageTests.cpp
    plugin/ParameterTests.cpp
    plugin/PluginEditorTests.cpp
    plugin/ProcessorTests.cpp
    plugin/TestClipExporterTests.cpp
    # M3 capture (docs/M3-DESIGN.md 16)
    plugin/CaptureParameterTests.cpp
    plugin/CaptureWorkerTests.cpp
    plugin/CaptureExporterTests.cpp
    plugin/CapturePanelTests.cpp)
target_link_libraries(VocalSynthPluginTests PRIVATE VocalSynthPluginCode Catch2::Catch2)
target_compile_definitions(VocalSynthPluginTests PRIVATE
    $<TARGET_PROPERTY:VocalSynthFX,COMPILE_DEFINITIONS>
    JUCE_MODAL_LOOPS_PERMITTED=1)
set_target_properties(VocalSynthPluginTests PROPERTIES FOLDER "Tests")
catch_discover_tests(VocalSynthPluginTests DISCOVERY_MODE PRE_TEST)
