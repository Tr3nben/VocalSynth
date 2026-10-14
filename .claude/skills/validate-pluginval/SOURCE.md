# Source / attribution

- Upstream: https://github.com/iPlug3/audio-plugin-dev-skills (plugins/audio-plugin-validators/skills/validate-pluginval), commit e593a808a6fc97d0f13a6fd2e91e61fcf2e5a314
- Author: Oli Larkin. License: MIT, see LICENSE (the copyright notice must stay with this file).
- Local modifications (2026-10-14), checked against pluginval's docs/Command line options.md:
  - `--seed` corrected to `--random-seed`; the undocumented `-s` short flag replaced with `--strictness-level`.
  - Corrected the claim that the VST3 validator runs automatically at level 5+: it runs only when `--vst3validator <path>` is given.
  - Added the `--rtcheck` option (macOS/Linux only).
