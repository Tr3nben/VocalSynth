# Two plugin products are built from the same processor code (see docs/PLAN.md, section 2.1):
#   VocalSynthFX          audio effect on the mic's mixer insert, audio in -> MIDI out
#   VocalSynthInstrument  instrument with a sidechain input, audio in -> MIDI out (+ internal engine later)
# Which one is the primary FL Studio path is decided by the M1 routing spike.
#
# IMPORTANT: The 4-character codes and bundle IDs below must NEVER change after the first public
# release. Hosts use them to identify the plugin in saved projects.

set(VOCALSYNTH_COMPANY_NAME "Tr3nben")
set(VOCALSYNTH_MANUFACTURER_CODE "Tr3n") # first char uppercase (AU requirement)
set(VOCALSYNTH_BUNDLE_ID_PREFIX "com.tr3nben.vocalsynth")

set(VOCALSYNTH_FORMATS VST3)
if (APPLE)
    list(APPEND VOCALSYNTH_FORMATS AU)
endif ()

# --- Shared plugin code (INTERFACE, so the plugin targets and the tests compile it alike) ---
add_library(VocalSynthPluginCode INTERFACE)

target_sources(VocalSynthPluginCode INTERFACE
    ${CMAKE_SOURCE_DIR}/plugin/MidiOutputStage.h
    ${CMAKE_SOURCE_DIR}/plugin/MidiOutputStage.cpp
    ${CMAKE_SOURCE_DIR}/plugin/Parameters.h
    ${CMAKE_SOURCE_DIR}/plugin/PluginEditor.h
    ${CMAKE_SOURCE_DIR}/plugin/PluginEditor.cpp
    ${CMAKE_SOURCE_DIR}/plugin/PluginProcessor.h
    ${CMAKE_SOURCE_DIR}/plugin/PluginProcessor.cpp
    ${CMAKE_SOURCE_DIR}/plugin/TestClipExporter.h
    ${CMAKE_SOURCE_DIR}/plugin/TestClipExporter.cpp
    ${CMAKE_SOURCE_DIR}/plugin/TunerDisplayModel.h
    ${CMAKE_SOURCE_DIR}/plugin/TunerDisplayModel.cpp
    ${CMAKE_SOURCE_DIR}/plugin/VirtualMidiPort.h
    ${CMAKE_SOURCE_DIR}/plugin/VirtualMidiPort.cpp)

target_include_directories(VocalSynthPluginCode INTERFACE ${CMAKE_SOURCE_DIR}/plugin)

target_compile_definitions(VocalSynthPluginCode INTERFACE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    VOCALSYNTH_VERSION_STRING="${PROJECT_VERSION}")

if (MSVC)
    target_compile_options(VocalSynthPluginCode INTERFACE /Zc:__cplusplus)
endif ()

target_link_libraries(VocalSynthPluginCode INTERFACE
    VocalSynthCore
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# --- Helper that creates one plugin product -----------------------------------------------
function(vocalsynth_add_plugin target)
    set(oneValueArgs PRODUCT_NAME PLUGIN_CODE BUNDLE_SUFFIX IS_SYNTH STANDALONE)
    set(multiValueArgs VST3_CATEGORIES CLAP_FEATURES)
    cmake_parse_arguments(VS "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    set(formats ${VOCALSYNTH_FORMATS})
    if (VS_STANDALONE)
        list(APPEND formats Standalone)
    endif ()

    juce_add_plugin(${target}
        PRODUCT_NAME "${VS_PRODUCT_NAME}"
        COMPANY_NAME "${VOCALSYNTH_COMPANY_NAME}"
        BUNDLE_ID "${VOCALSYNTH_BUNDLE_ID_PREFIX}.${VS_BUNDLE_SUFFIX}"
        PLUGIN_MANUFACTURER_CODE ${VOCALSYNTH_MANUFACTURER_CODE}
        PLUGIN_CODE ${VS_PLUGIN_CODE}
        FORMATS ${formats}
        IS_SYNTH ${VS_IS_SYNTH}
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT TRUE
        IS_MIDI_EFFECT FALSE # a JUCE MIDI effect has no audio input, but we need the mic
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        VST3_CATEGORIES ${VS_VST3_CATEGORIES}
        MICROPHONE_PERMISSION_ENABLED TRUE
        MICROPHONE_PERMISSION_TEXT "VocalSynth listens to your voice to turn it into MIDI."
        COPY_PLUGIN_AFTER_BUILD FALSE) # copying to system folders needs admin rights

    # createPluginFilter() lives outside the shared code so the tests can build both variants
    target_sources(${target} PRIVATE ${CMAKE_SOURCE_DIR}/plugin/PluginFactory.cpp)
    target_link_libraries(${target} PRIVATE VocalSynthPluginCode)
    set_target_properties(${target} PROPERTIES FOLDER "VocalSynth")

    clap_juce_extensions_plugin(TARGET ${target}
        CLAP_ID "${VOCALSYNTH_BUNDLE_ID_PREFIX}.${VS_BUNDLE_SUFFIX}"
        CLAP_FEATURES ${VS_CLAP_FEATURES})
endfunction()

vocalsynth_add_plugin(VocalSynthFX
    PRODUCT_NAME "VocalSynth FX"
    PLUGIN_CODE VsFx
    BUNDLE_SUFFIX fx
    IS_SYNTH FALSE
    STANDALONE TRUE
    VST3_CATEGORIES Fx Analyzer
    CLAP_FEATURES audio-effect analyzer)

vocalsynth_add_plugin(VocalSynthInstrument
    PRODUCT_NAME "VocalSynth Instrument"
    PLUGIN_CODE VsIn
    BUNDLE_SUFFIX instrument
    IS_SYNTH TRUE
    STANDALONE FALSE
    VST3_CATEGORIES Instrument
    CLAP_FEATURES instrument)

# Convenience target that builds every format of both products
add_custom_target(VocalSynthAll)
foreach (product VocalSynthFX VocalSynthInstrument)
    add_dependencies(VocalSynthAll ${product}_All ${product}_CLAP)
endforeach ()
