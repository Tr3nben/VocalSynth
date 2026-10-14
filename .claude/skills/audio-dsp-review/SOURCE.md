# Source / attribution

- Upstream: https://github.com/kunitoki/sonic-skills (skills/audio-dsp-review), commit 8b6dcfe89030d7de9ba9d6afcc2d927fb11fe7fa
- Author: kunitoki (Sonic Skills). License: The Unlicense (public domain), see LICENSE.
- Local modifications (2026-10-14), references/realtime-violations.md:
  - Fixed the `juce::AbstractFifo` logging example to the real 5-argument `prepareToWrite/prepareToRead` API, handling both regions.
  - Fixed the double-buffer example (it redeclared `p`) and added a note that a safe version needs triple buffering or an ack.
