# Source / attribution

- Upstream: https://github.com/kunitoki/sonic-skills (skills/juce-review), commit 8b6dcfe89030d7de9ba9d6afcc2d927fb11fe7fa
- Author: kunitoki (Sonic Skills). License: The Unlicense (public domain), see LICENSE.
- Local modifications (2026-10-14):
  - Step 0 no longer requires `audio-numerics-review` (not installed here).
  - Fixed the ValueTree listener row: listeners fire synchronously on the modifying thread, not via MessageManager.
  - references/juce-violations.md: replaced a confusing "`juce::dsp::DelayLine`-style atomic flag" with `std::atomic` / `AbstractFifo`.
