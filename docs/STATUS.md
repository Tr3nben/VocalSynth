# Projektstand (Übergabe-Notiz)

Stand: 2026-10-17. Diese Datei fasst zusammen, wo das Projekt steht, damit eine neue Sitzung oder ein Agent
ohne den bisherigen Chatverlauf weitermachen kann.

## Erledigt

| Meilenstein | Ergebnis |
|---|---|
| M0 – Grundgerüst | JUCE 9.0.4 + clap-juce-extensions (Submodules), zwei Produkte aus einem Code (VocalSynth FX / Instrument), CI auf Windows/macOS/Linux, Windows-Installationspaket |
| M1 – Routing-Test (v0.1.0) | Testmodus mit Test-MIDI, Monitor-Ton, virtueller MIDI-Port, Diagnoseanzeigen. **In FL Studio 2026 getestet: T1 funktioniert** (Noten, Pitchbend, CC über MIDI-Port an VST3-Synth). FL nimmt Plugin-MIDI nicht in die Piano Roll auf. |
| M2 – Singen wird zu MIDI (v0.2.0) | Pitch → MIDI mit YIN/MPM, Stimmlagen, Skalen, Stickiness, Gate, Bend, Legato, Tuner-UI, `vs-eval`. **In FL Studio 2026 getestet (2026-10-15): Einzeltöne, Tonleitern, Vibrato, Legato o.k., MPM etwas besser als YIN.** Über PR #2 in `master` gemergt. |
| M2.1 – Feinschliff (v0.2.1) | Ruhiges Stimmgerät, schnellere Notenanfänge, Latenz- und DSP-Anzeige, Gate −40 dB. **Vom Nutzer in FL getestet (2026-10-16): „funktioniert alles super“.** Über PR #3 in `master`. |

M0/M1 sind über PR #1, M2 (v0.2.0) über PR #2, M2.1 (v0.2.1) über PR #3 in `master` gemergt.

## Entscheidungen

- **Hauptweg:** VocalSynth FX auf dem Mikro-Insert, MIDI über den Output-Port des FL-Wrappers an einen Synth mit
  gleichem Input-Port. Die Instrument-Variante bleibt, ist aber sekundär.
- **Lizenz:** JUCE läuft unter der kostenlosen JUCE-Starter-Lizenz (nicht AGPLv3). Lizenz des eigenen Codes ist offen.
- **Plugin-Codes** (nie ändern): Hersteller `Tr3n`, FX `VsFx`, Instrument `VsIn`, Bundle-IDs `com.tr3nben.vocalsynth.fx/.instrument`.
- **Pitch-Algorithmus:** MPM ist Standard (vs-eval und Nutzertest 0.2.0), YIN bleibt wählbar. Alle Regler bleiben einstellbar.
- **Stimme des Nutzers:** Tenor. Stimmlage muss trotzdem per Dropdown wählbar sein (wie bei Auto-Tune).
- **Aufnahme (M3, 2026-10-16):** `.mid` ab dem Takt des Takes, Stimme als WAV (an, 10 min, `Dokumente\VocalSynth\Takes`),
  Latenzausgleich Auto + Feinregler (Kalibrierung später), Takes nicht im Projekt, Drag-Standard Clean ohne
  Quantisierung mit Bend wie im Plugin, Freimodus 3 s Pause (Details: [docs/M3-SPEC.md](M3-SPEC.md) „Decisions“).

## Aktuell: M3 – Aufnahme und Drag & Drop (v0.3.0)

- Branch: `m3-capture` (von `master` nach v0.2.1). Vorgabe: [docs/M3-SPEC.md](M3-SPEC.md) mit den vom Nutzer am
  2026-10-16 bestätigten 6 Entscheidungen (Abschnitt „Decisions“). Design: [docs/M3-DESIGN.md](M3-DESIGN.md)
  (Abschnitt 19 = Änderungen bei der Umsetzung). Testanleitung (deutsch): [docs/M3-TESTING.md](M3-TESTING.md),
  liegt auch im Windows-Paket `VocalSynth-0.3.0-Windows-<Commit>.zip`.
- Umgesetzt: immer laufende Aufnahme (`CaptureTap` auf dem Audio-Thread, zwei SPSC-Ringe, Capture-Worker-Thread
  mit `CaptureHistory`, 10 Minuten Rückblick), Takes synchron zum Transport (Play bis Stop, Loops ausgerollt)
  oder frei (3 s Pause), „New take“, „Grab last N bars / N s“, Ausgleich von Erkennungs-Verzögerung und
  Soundkarten-Umweg (Auto = 2 × Puffer + Feinregler), `.mid` ab dem Takt des Takes (Taktnummer im Dateinamen),
  Clean/Raw, Quantisierung, `.wav` der trockenen Stimme (gezogene WAVs in `Dokumente\VocalSynth\Takes`),
  Reiter „Capture“ mit Take-Liste und Drag-Quellen, Aufräumen der Temp-Ordner. Takes werden nicht im Projekt
  gespeichert, nur die Einstellungen. Fünf neue Parameter (Version-Hint 2).
- Integration 2026-10-17 (lokal, Linux, clang Release): alle Gates grün – ctest (xvfb) alle Tests, pluginval
  Strictness 10 beide VST3, Steinberg-Validator 47/47 beide VST3, clap-validator 32 bestanden / 0 Fehler beide
  CLAP. Eine clap-validator-Warnung `state-invalid-random` (JUCE-CLAP-Wrapper nimmt zufällige State-Bytes an),
  nicht durch M3 verursacht. End-to-end-Test (A3 C4 E4 A3 ab Takt 3, ASIO 2048 simuliert, Host-Blöcke
  2048/480/64): Noten in Reihenfolge, höchstens 2 ms neben dem Schlag, `.mid` byteweise gleich.
- **Nächste Schritte:** Review (Echtzeit + Korrektheit) mit Gegenprüfung → Fixes → Commit/Push → CI → ZIP-Link
  an den Nutzer → Nutzertest in FL Studio 2026 nach [docs/M3-TESTING.md](M3-TESTING.md). Dabei besonders
  prüfen: ob FLs vorbereitete maximale Blockgröße dem ASIO-Puffer entspricht (der Capture-Reiter zeigt Auto
  neben dem größten tatsächlich gesendeten Block und warnt ab Faktor 2), Import der Bends (Bereich), WAV und
  Pattern am selben Takt, Aufräumen von `%TEMP%\VocalSynth\Capture` beim Schließen von FL (falls FL die DLL
  entlädt, ohne den Prozessor zu zerstören, könnte das Beenden des Worker-Threads hängen – beobachten).
- Danach: Feinabstimmung mit dem Korpus ([docs/CORPUS.md](CORPUS.md); gehaltene Noten zerfallen auf echten
  Aufnahmen in Zusatznoten) und M4 (Beatbox-Trigger, nutzt die Capture-Maschinerie).

## Vorher: M2.1 – Feinschliff nach dem Nutzertest (v0.2.1, erledigt)

- Branch `m2.1-polish`, Design [docs/M2-DESIGN.md](M2-DESIGN.md) Abschnitt 11, Test [docs/M2-TESTING.md](M2-TESTING.md).
  2026-10-16 vom Nutzer in FL getestet („funktioniert alles super“), per PR #3 in `master` gemergt.
- Ruhiges Stimmgerät, schnellere Notenanfänge (`FastOnsetDetector`, vorläufige Note; Tenor mittlere Lage ca.
  11 statt 20 ms), Latenz-Anzeige mit Puffer und Gesamt-Schätzung, eigene DSP-Last, Gate-Standard −40 dB.
- **Korpus-Workflow** ([docs/CORPUS.md](CORPUS.md)): `.github/workflows/corpus-eval.yml`, nur manuell
  (`workflow_dispatch`), wertet vocadito, Dagstuhl ChoirSet und VocalSet (CC BY 4.0) mit `vs-eval` aus. Der echte
  Zenodo-Download ist ungetestet (aus dem Container nicht erreichbar). Erster Hinweis (vocadito_1): Tonhöhe sehr
  gut (RPA 99,5 % MPM), gehaltene Noten zerfallen in Zusatznoten. Note-F1 (Gate −40, MPM/YIN) v0.2.1: 0,278/0,304.

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
