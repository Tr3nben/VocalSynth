# Standalone build of vs-eval: only the JUCE-free core + vs-eval, no JUCE / plugin / submodules.
# Used by .github/workflows/corpus-eval.yml (docs/CORPUS.md):
#   cmake -S tools/vs-eval/standalone -B build-eval -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-eval --target vs-eval
cmake_minimum_required(VERSION 3.25)

set(VOCALSYNTH_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
file(STRINGS ${VOCALSYNTH_ROOT}/VERSION VOCALSYNTH_VERSION LIMIT_COUNT 1)
project(VocalSynthEval VERSION ${VOCALSYNTH_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VOCALSYNTH_BUILD_TESTS "Register the vs-eval ctest entries" ON)

if (VOCALSYNTH_BUILD_TESTS)
    enable_testing()
endif ()

add_subdirectory(${VOCALSYNTH_ROOT}/core core)
add_subdirectory(${VOCALSYNTH_ROOT}/tools/vs-eval vs-eval)
