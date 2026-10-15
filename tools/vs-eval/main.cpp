// vs-eval: runs the VocalSynth voice pipeline offline on WAV files and prints JSON metrics
// (docs/M2-DESIGN.md 5). JUCE-free.
//
// Usage:
//   vs-eval [options] <file.wav> [<file.wav> ...]
//   vs-eval --self-test               synthetic corpus in memory, exit code 1 if a gate fails (ctest)
//   vs-eval --write-corpus <dir>      writes the synthetic corpus as WAV + CSV labels
// Options:
//   --range <bass|baritone|tenor|alto|soprano|whistle|wide>   default tenor
//   --algorithm <yin|mpm|both>                                default both
//   --labels <file.csv>      labels for the (single) input file; default: <file>.csv next to the WAV if present
//   --block <n>              host block size for the offline run, default 512
//   --key <0..11> --scale <index> --stickiness <%> --gate <dB> --min-note <ms>   pipeline settings

#include "Metrics.h"
#include "WavFile.h"

#include "vocalsynth/VoicePipeline.h"

#include <cstdio>
#include <cstring>

int main (int argc, char** argv)
{
    // TODO(C): argument parsing, offline runs (YIN and MPM), JSON output, --self-test gates, --write-corpus.
    const bool selfTest = argc > 1 && std::strcmp (argv[1], "--self-test") == 0;

    std::printf ("{\"tool\":\"vs-eval\",\"status\":\"scaffold\",\"selfTest\":%s,\"latencyTenor48kMs\":%.3f}\n",
                 selfTest ? "true" : "false",
                 vocalsynth::VoicePipeline::computeAlgorithmicLatencyMs (48000.0, vocalsynth::VoiceRange::tenor));
    return 0;
}
