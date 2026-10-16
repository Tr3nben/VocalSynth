# Projektstand (Übergabe-Notiz)

Stand: 2026-10-16. Diese Datei fasst zusammen, wo das Projekt steht, damit eine neue Sitzung oder ein Agent
ohne den bisherigen Chatverlauf weitermachen kann.

## Erledigt

| Meilenstein | Ergebnis |
|---|---|
| M0 – Grundgerüst | JUCE 9.0.4 + clap-juce-extensions (Submodules), zwei Produkte aus einem Code (VocalSynth FX / Instrument), CI auf Windows/macOS/Linux, Windows-Installationspaket |
| M1 – Routing-Test (v0.1.0) | Testmodus mit Test-MIDI, Monitor-Ton, virtueller MIDI-Port, Diagnoseanzeigen. **In FL Studio 2026 getestet: T1 funktioniert** (Noten, Pitchbend, CC über MIDI-Port an VST3-Synth). FL nimmt Plugin-MIDI nicht in die Piano Roll auf. |
| M2 – Singen wird zu MIDI (v0.2.0) | Pitch → MIDI mit YIN/MPM, Stimmlagen, Skalen, Stickiness, Gate, Bend, Legato, Tuner-UI, `vs-eval`. **In FL Studio 2026 getestet (2026-10-15): Einzeltöne, Tonleitern, Vibrato, Legato o.k., MPM etwas besser als YIN.** Über PR #2 in `master` gemergt. |

M0/M1 sind über PR #1, M2 (v0.2.0) über PR #2 in `master` gemergt.

## Entscheidungen

- **Hauptweg:** VocalSynth FX auf dem Mikro-Insert, MIDI über den Output-Port des FL-Wrappers an einen Synth mit
  gleichem Input-Port. Die Instrument-Variante bleibt, ist aber sekundär.
- **Lizenz:** JUCE läuft unter der kostenlosen JUCE-Starter-Lizenz (nicht AGPLv3). Lizenz des eigenen Codes ist offen.
- **Plugin-Codes** (nie ändern): Hersteller `Tr3n`, FX `VsFx`, Instrument `VsIn`, Bundle-IDs `com.tr3nben.vocalsynth.fx/.instrument`.
- **Pitch-Algorithmus:** MPM ist Standard (vs-eval und Nutzertest 0.2.0), YIN bleibt wählbar. Alle Regler bleiben einstellbar.
- **Stimme des Nutzers:** Tenor. Stimmlage muss trotzdem per Dropdown wählbar sein (wie bei Auto-Tune).

## Aktuell: M2.1 – Feinschliff nach dem Nutzertest (v0.2.1)

- Branch: `m2.1-polish` (von `master` nach dem Merge von v0.2.0). Design: [docs/M2-DESIGN.md](M2-DESIGN.md)
  Abschnitt 11. Testanleitung: [docs/M2-TESTING.md](M2-TESTING.md) (Abschnitt 0 = Kurztest für 0.2.1).
- Rückmeldung des Nutzers zu 0.2.0: Stimmgerät-Anzeige zu hektisch; Latenz träge bei ASIO-Puffer 2048 (≈ 46 ms
  pro Puffer, FL-MIDI-Port vermutlich ein weiterer Puffer) und Stimmlage Tenor; Gate −35 … −40 dB passt am besten;
  CPU 15–20 % in FL zusammen mit Vital (Anteil von VocalSynth unklar); Frage nach öffentlichen Gesangsdaten.
- Umgesetzt (implementiert, lokal alle Gates grün, noch nicht committet/gepusht, wartet auf CI und Nutzertest):
  - Ruhiges Stimmgerät (`plugin/TunerDisplayModel.*`), MIDI unverändert.
  - Schnellere Notenanfänge (`FastOnsetDetector` + vorläufige Note im `NoteSegmenter`): Tenor bei mittlerer
    Tonlage ca. 11 statt 20 ms; am tiefsten Ton kaum besser (zwei Perioden A2 = 18 ms). Angezeigte Analyse-Latenz
    (ungünstigster Fall) Tenor 23,2 ms (vorher 24,2), Bass 38,1 ms (vorher 39,1).
  - Latenz-Anzeige mit Audio-Puffer und Gesamt-Schätzung (Analyse + 2 × Puffer), Hinweis bei > 512 Samples.
  - Eigene DSP-Last im Plugin (`DspLoadMeter`).
  - Gate-Standard −40 dB (`VoiceSettings.h`, `Parameters.h`, static_assert-gekoppelt).
- **Korpus-Workflow** ([docs/CORPUS.md](CORPUS.md)): `.github/workflows/corpus-eval.yml`, nur manuell
  (`workflow_dispatch`), baut nur `vs-eval` (ohne JUCE), lädt Teilmengen von vocadito, Dagstuhl ChoirSet und
  VocalSet (alle CC BY 4.0) von Zenodo, wertet YIN/MPM aus und macht einen Sweep (Gate × Stickiness). Artefakt nur
  Metriken, kein Audio. Erst startbar, wenn die Datei auf `master` liegt (Actions → „Corpus evaluation“ → Run
  workflow, oder `gh workflow run corpus-eval.yml --ref m2.1-polish`). Der echte Zenodo-Download ist ungetestet
  (aus dem Container nicht erreichbar). Erster Hinweis auf einer echten Aufnahme (vocadito_1): Tonhöhe sehr gut
  (RPA 99,5 % MPM), aber gehaltene Noten zerfallen in viele Zusatznoten → Thema für die nächste
  Feinabstimmung. Note-F1 (Gate −40, MPM/YIN): v0.2.0 0,264/0,284; v0.2.1 vor dem Review-Fix 0,225/0,265
  (Schleifer: falscher Halbton am Notenanfang); nach dem Fix 0,278/0,304.
- **M3-Vorgabe als Entwurf:** [docs/M3-SPEC.md](M3-SPEC.md) (Aufnahme + Drag & Drop, Ziel 0.3.0). **Wartet auf die
  Antworten des Nutzers** zu den 6 offenen Fragen am Ende und auf seine Freigabe; erst dann Branch `m3-capture`.
- 2026-10-16: 0.2.1 vom Nutzer in FL getestet („funktioniert alles super“), PR nach `master`.
- Nächster Meilenstein: M3 (Capture + Drag & Drop) nach [docs/M3-SPEC.md](M3-SPEC.md). Der Nutzer hat alle 6
  empfohlenen Antworten bestätigt (Abschnitt „Decisions“ dort). Branch `m3-capture`.

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
CI: Bei Push/PR laufen nur Linux und Windows. macOS (AU, auval) nur bei manuellem Start
(Actions → Build & Test → Run workflow). Grund: Im Oktober 2026 waren die Gratis-Minuten des privaten Repos aufgebraucht
(macOS zählt 10-fach), danach wurde das Repo öffentlich gemacht.
