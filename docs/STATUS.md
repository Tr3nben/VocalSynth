# Projektstand (Übergabe-Notiz)

Stand: 2026-10-15. Diese Datei fasst zusammen, wo das Projekt steht, damit eine neue Sitzung oder ein Agent
ohne den bisherigen Chatverlauf weitermachen kann.

## Erledigt

| Meilenstein | Ergebnis |
|---|---|
| M0 – Grundgerüst | JUCE 9.0.4 + clap-juce-extensions (Submodules), zwei Produkte aus einem Code (VocalSynth FX / Instrument), CI auf Windows/macOS/Linux, Windows-Installationspaket |
| M1 – Routing-Test (v0.1.0) | Testmodus mit Test-MIDI, Monitor-Ton, virtueller MIDI-Port, Diagnoseanzeigen. **In FL Studio 2026 getestet: T1 funktioniert** (Noten, Pitchbend, CC über MIDI-Port an VST3-Synth). FL nimmt Plugin-MIDI nicht in die Piano Roll auf. |

Beides ist über PR #1 in `master` gemergt.

## Entscheidungen

- **Hauptweg:** VocalSynth FX auf dem Mikro-Insert, MIDI über den Output-Port des FL-Wrappers an einen Synth mit
  gleichem Input-Port. Die Instrument-Variante bleibt, ist aber sekundär.
- **Lizenz:** JUCE läuft unter der kostenlosen JUCE-Starter-Lizenz (nicht AGPLv3). Lizenz des eigenen Codes ist offen.
- **Plugin-Codes** (nie ändern): Hersteller `Tr3n`, FX `VsFx`, Instrument `VsIn`, Bundle-IDs `com.tr3nben.vocalsynth.fx/.instrument`.
- **Stimme des Nutzers:** Tenor. Stimmlage muss trotzdem per Dropdown wählbar sein (wie bei Auto-Tune).

## Als Nächstes: M2 – Singen wird zu MIDI (v0.2.0)

- Branch: `m2-pitch-to-midi` (von `master`).
- Verbindliche Vorgabe: [docs/M2-SPEC.md](M2-SPEC.md).

## Arbeitsweise (vom Nutzer gewünscht)

- Planung, Programmierung, Test und Review jeweils mit Agenten (Workflow-Tool). Große Aufgaben teilen sich in
  Unteragenten mit getrennten Dateien auf; möglichst viel parallel. Hinweis: Der Container hat 4 Kerne, daher laufen
  höchstens 2 Workflow-Agenten gleichzeitig, und JUCE-Builds dürfen sich kein Build-Verzeichnis teilen.
- Ablauf pro Meilenstein: Design + kompilierbares Gerüst → parallele Implementierung mit eigenen Unit-Tests →
  Integration (Build, ctest, pluginval 10, VST3-Validator, clap-validator) → Review (Echtzeit + Korrektheit) →
  Gegenprüfung jedes Befunds → Fixes → Commit/Push → CI (≈ 40 min) → Windows-ZIP-Link an den Nutzer.
- Der Nutzer testet in FL Studio 2026 unter Windows; Antworten und Doku für ihn auf Deutsch.

## Werkzeuge im Container (bei neuem Container neu bauen)

Liegen im Scratchpad und gehen beim Neustart des Containers verloren:

```bash
S=<scratchpad>
# pluginval
mkdir -p $S/pv && cd $S/pv && curl -fsSL -o pv.zip https://github.com/Tracktion/pluginval/releases/download/v1.0.4/pluginval_Linux.zip && unzip -oq pv.zip && chmod +x pluginval
# Steinberg VST3 validator (braucht libgtkmm-3.0-dev)
git clone -q --depth 1 --branch v3.8.1_build_84 https://github.com/steinbergmedia/vst3sdk $S/vst3sdk && cd $S/vst3sdk \
  && git submodule update --init --depth 1 base pluginterfaces public.sdk cmake \
  && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSMTG_ENABLE_VSTGUI_SUPPORT=OFF -DSMTG_ENABLE_VST3_PLUGIN_EXAMPLES=OFF -DSMTG_RUN_VST_VALIDATOR=OFF \
  && cmake --build build --target validator
# clap-validator
cargo install --locked --git https://github.com/free-audio/clap-validator --tag 0.4.1 --root $S/cv clap-validator
```

Linux-Pakete für JUCE: siehe Schritt „Install Linux dependencies“ in `.github/workflows/build_and_test.yml`.
Lokaler Build: `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++`.
CI-Artefakte lassen sich aus dem Container nicht herunterladen (Proxy blockiert den Blob-Speicher).
