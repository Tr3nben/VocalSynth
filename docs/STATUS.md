# Projektstand (Übergabe-Notiz)

Stand: 2026-10-21. Diese Datei fasst zusammen, wo das Projekt steht, damit eine neue Sitzung oder ein Agent
ohne den bisherigen Chatverlauf weitermachen kann.

## Erledigt

| Meilenstein | Ergebnis |
|---|---|
| M0 – Grundgerüst | JUCE 9.0.4 + clap-juce-extensions (Submodules), zwei Produkte aus einem Code (VocalSynth FX / Instrument), CI auf Windows/macOS/Linux, Windows-Installationspaket |
| M1 – Routing-Test (v0.1.0) | Testmodus mit Test-MIDI, Monitor-Ton, virtueller MIDI-Port, Diagnoseanzeigen. **In FL Studio 2026 getestet: T1 funktioniert** (Noten, Pitchbend, CC über MIDI-Port an VST3-Synth). FL nimmt Plugin-MIDI nicht in die Piano Roll auf. |
| M2 – Singen wird zu MIDI (v0.2.0) | Pitch → MIDI mit YIN/MPM, Stimmlagen, Skalen, Stickiness, Gate, Bend, Legato, Tuner-UI, `vs-eval`. **In FL Studio 2026 getestet (2026-10-15): Einzeltöne, Tonleitern, Vibrato, Legato o.k., MPM etwas besser als YIN.** Über PR #2 in `master` gemergt. |
| M2.1 – Feinschliff (v0.2.1) | Ruhiges Stimmgerät, schnellere Notenanfänge, Latenz- und DSP-Anzeige, Gate −40 dB. **Vom Nutzer in FL getestet (2026-10-16): „funktioniert alles super“.** Über PR #3 in `master`. |
| M3 – Aufnahme und Drag & Drop (v0.3.0) | Immer laufende Aufnahme, Takes synchron/frei, `.mid`/`.wav` per Drag & Drop, Latenzausgleich, Clean/Raw, Grab. **Vom Nutzer in FL getestet (2026-10-21): Timing, Drag & Drop, Tempowechsel, „Grab last 8 bars“ und CPU o.k., Clean etwas besser als Raw; Problem: Takes im Hintergrund zu lang.** Über PR #4 in `master`. |

M0/M1 sind über PR #1, M2 (v0.2.0) über PR #2, M2.1 (v0.2.1) über PR #3, M3 (v0.3.0) über PR #4 in `master` gemergt.

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
- **Take-Länge (0.3.1, 2026-10-21, Nutzerwunsch):** Hintergrund-Takes beginnen am Takt der ersten Note und werden
  nach *Max length* Takten abgeschnitten (Standard 16), dazu ein Record-Modus mit genau N Takten ab dem nächsten
  Taktstrich (Standard 8). Gate-Standard −35 dB (Korpus-Sweep und Nutzer: −35 … −40 dB).
  Details: [docs/M3.1-DESIGN.md](M3.1-DESIGN.md).

## Aktuell: M3.1 – Take-Länge, Record-Modus, weniger Zusatznoten (v0.3.1)

- Branch: `m3.1-takes` (von `master` nach v0.3.0 / PR #4). Stand im Arbeitsbaum: WIP-Commit `fe41d9e` (Design,
  Gerüst, Capture-Umsetzung) plus noch nicht committete Änderungen (Segmentierung, Doku, End-to-end-Tests).
  Design: [docs/M3.1-DESIGN.md](M3.1-DESIGN.md), Segmentierung [docs/M2-DESIGN.md](M2-DESIGN.md) Abschnitt 12.
  Kurztest für den Nutzer (deutsch): [docs/M3-TESTING.md](M3-TESTING.md) Abschnitt 0, im Windows-Paket
  `VocalSynth-0.3.1-Windows-<Commit>.zip`.
- **Capture:** drei neue Parameter (Version-Hint 3): *Mode* (Background/Record), *Max length* (Off/4/8/16/32,
  Standard 16), *Length* (1–32, Standard 8). Synchrone Hintergrund-Takes sind am Takt der ersten Note verankert
  und werden nach N Takten geschnitten (gehaltene Note über den Schnitt → nächster Take nahtlos bei X + N; nach
  einer Pause am Takt der eigenen ersten Note); Free-Takes nach N 4/4-Takten beim Host-Tempo (sonst 120 BPM).
  Export-Ende am Taktstrich nach der letzten Note der Version, WAV-Ende wie Clean (Klicks am Ende verlängern
  nichts mehr). Record-Modus: Knopf *Record*/*Cancel*, Start am nächsten Taktstrich (gestoppt: am Taktstrich des
  Starts), genau N Takte, Herunterzählen und „Recording bar k/N“, keine Hintergrund-Takes, Grab geht weiter.
  Blockgrößen-invariant (`.mid` byteweise gleich für 1/7/64/480/512/4096).
- **Segmentierung** (`NoteSegmenter`): Analyse auf vocadito_1 – die Zusatznoten kommen fast nur von der Tonhöhe
  (späte Wechsel, Durchgangsnoten in Schleifern/Glissandi, schnelle Fehlstarts, Intonation), kaum von
  Aussetzern. Geändert: Wechsel nach halber Mindest-Wechselzeit, sobald die Stimme auf dem neuen Ton angekommen
  ist; kein Wechsel, solange die Tonhöhe noch gleitet; Aussetzer-Halten (höchstens 60 ms) nach einem Pegelsprung
  über dem Gate. Vorher/nachher (Gate −35 dB):

  | Input | MPM F1 | YIN F1 |
  |---|---|---|
  | vocadito_1, annotator A1 | 0.284 → 0.317 (precision 0.218 → 0.248) | 0.313 → 0.305 |
  | vocadito_1, annotator A2 | 0.310 → 0.355 | 0.339 → 0.343 |
  | vocadito_1, notes from reference f0 | 0.354 → 0.423 | 0.326 → 0.400 |
  | semi-synthetic sung set | 0.370 → 0.497 | 0.348 → 0.442 |
  | self-test corpus | 0.808 → 0.832 | 0.779 → 0.826 |

  Note-On aus der Stille unverändert (Selbsttest 15,4 / 30,9 ms), Rauschen/Atem/Stille 0 Noten, Vibrato eine Note,
  Oktavfehler unverändert. Bekannt: YIN auf A1 eine Note schlechter; ca. 10 ms völlige Stille zwischen zwei
  gleichen Tönen ergibt in Bass/Tenor eine Note (ab 20 ms immer zwei).
- Integration 2026-10-21 (lokal, Linux, clang Release, alle eigenen Quellen neu übersetzt: 0 Warnungen): ctest
  (xvfb) 503/503, darunter zwei neue End-to-end-Tests im Prozessor: Record 8 Takte, bei Takt 2 Schlag 3 scharf
  geschaltet (120 BPM, gesungene Phrase, danach Mausklick-artige Geräusche über das Ende hinaus) → genau ein Take
  Takt 3 bis 11, alle 12 Noten im Take, nichts von den Klicks (auch nicht live); Hintergrund mit *Max length* 4 und
  10 Takten Gesang → drei Takes ab Takt 2, 6, 10, je höchstens 4 Takte, jede Note in genau einem Take.
  pluginval Strictness 10 beide VST3, Steinberg-Validator 47/47 beide VST3, clap-validator 32 bestanden / 0 Fehler
  beide CLAP (1 bekannte Warnung `state-invalid-random`, JUCE-CLAP-Wrapper). vs-eval-Standalone (Korpus-CI) baut,
  2/2 ctests; Korpus-pytest mit vs-eval 30/30.
- **Review erledigt (2026-10-26):** 11 bestätigte Befunde behoben, je mit Regressionstest (Liste in
  [M3.1-DESIGN.md](M3.1-DESIGN.md) Abschnitt 12); danach 511/511 ctest, pluginval 10, VST3- und clap-validator grün.
- **Nächste Schritte:** CI → ZIP-Link an
  den Nutzer → Kurztest in FL Studio 2026 ([docs/M3-TESTING.md](M3-TESTING.md) Abschnitt 0). **Den
  Korpus-Workflow auf `m3.1-takes` neu laufen lassen** (`gh workflow run corpus-eval.yml --ref m3.1-takes`): die
  Zahlen oben sind lokal (vocadito_1 und ein halbsynthetischer Satz), die Wirkung auf Dagstuhl ChoirSet und
  VocalSet und der Gate-Sweep um −35 dB sind noch nicht gemessen. Für den Nutzer prüfen: Standard 16 Takte,
  Länge der Record-Takes in FL (Pattern und Audio-Clip), deutsche Texte mit vielen Konsonanten, neu
  angeschlagene Silben auf demselben Ton.
- Danach: weitere Segmentierung (der Rest ist tonhöhengetrieben; mehr bräuchte Lookahead = Latenz) und M4
  (Beatbox-Trigger, nutzt die Capture-Maschinerie).

## Vorher: M3 – Aufnahme und Drag & Drop (v0.3.0, erledigt)

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
- Integration 2026-10-17 (lokal, Linux, clang Release, 0 Warnungen): alle Gates grün – ctest (xvfb) 447/447, pluginval
  Strictness 10 beide VST3, Steinberg-Validator 47/47 beide VST3, clap-validator 32 bestanden / 0 Fehler beide
  CLAP. Eine clap-validator-Warnung `state-invalid-random` (JUCE-CLAP-Wrapper nimmt zufällige State-Bytes an),
  nicht durch M3 verursacht. End-to-end-Test (A3 C4 E4 A3 ab Takt 3, ASIO 2048 simuliert, Host-Blöcke
  2048/480/64): Noten in Reihenfolge, höchstens 4 Ticks (ca. 2 ms) neben dem Schlag, `.mid` byteweise gleich, WAV ab Takt 3.
- Review-Fixes 2026-10-17 (RT-1 bis RT-5, F1 bis F7, F9; Liste in [M3-DESIGN.md](M3-DESIGN.md) 19.1): eigener
  Datei-Thread (das Leeren der Ringe wartet nie mehr auf die Festplatte), begrenzte Tempo-Historie, sicheres
  suspend/resume, Takt-Ursprung nur aus exportierten Noten, Übergangsnoten-Ketten, Quantisierung pro Takt
  (7/8), Takes über einen Samplerate-Wechsel, Test-Pattern ohne Umweg-Ausgleich, Grab endet am Klick,
  Notenzahl live. Gates danach: ctest (xvfb) 461/461, pluginval 10 beide VST3, Steinberg 47/47 beide,
  clap-validator 32/0 beide (1 bekannte Warnung), 0 Warnungen.
- 2026-10-21 vom Nutzer in FL Studio 2026 getestet (Ergebnis in der Tabelle oben), per PR #4 in `master`.
  Korpus-Auswertung danach: Note-F1 auf echtem Gesang ca. 0,33 (Präzision 0,26, gehaltene Noten zerfallen in
  Zusatznoten), Sweep: Gate −35 dB am besten → 0.3.1.

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
