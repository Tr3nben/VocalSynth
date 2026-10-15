# vs-eval: offline evaluation CLI for the voice pipeline (docs/M2-DESIGN.md 5). Plain C++, no JUCE.
add_executable(vs-eval
    main.cpp
    Metrics.h
    Metrics.cpp
    WavFile.h
    WavFile.cpp)

target_link_libraries(vs-eval PRIVATE VocalSynthCore)
# TestSignals.h (synthetic corpus for --self-test / --write-corpus) is shared with the core tests
target_include_directories(vs-eval PRIVATE ${PROJECT_SOURCE_DIR}/tests/core)
set_target_properties(vs-eval PROPERTIES FOLDER "Tools")

if (MSVC)
    target_compile_options(vs-eval PRIVATE /W4 /Zc:__cplusplus)
else ()
    target_compile_options(vs-eval PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif ()

if (VOCALSYNTH_BUILD_TESTS)
    add_test(NAME vs-eval.self-test COMMAND vs-eval --self-test)

    # CI builds only "VocalSynthAll VocalSynthCoreTests VocalSynthPluginTests" and then runs every ctest
    # entry: building the core tests must also build vs-eval, or vs-eval.self-test has no executable.
    if (TARGET VocalSynthCoreTests)
        add_dependencies(VocalSynthCoreTests vs-eval)
    endif ()
endif ()
