# VocalSynth – Umsetzungsplan

Stand: 2026-10-14. Grundlage: `docs/RESEARCH.md` (Fakten, Quellen, FL-Testmatrix T1–T7) und die Projektregeln in `.claude/skills/juce-audio-plugin-dev/SKILL.md`. Was RESEARCH.md als **(unbestätigt)** markiert, bleibt auch hier unbestätigt, bis Meilenstein M1 es geprüft hat.

Annahme für alle Zeitangaben: **ein Solo-Hobby-Entwickler mit ca. 8–10 Stunden pro Woche**. Bei Vollzeit grob durch 4 teilen.

---

## 1. Ziele & Nicht-Ziele

### 1.1 MVP (Version 0.x → 1.0)

- **Echtes Plugin** (VST3, Windows, FL Studio 2026), keine Standalone-App mit virtuellem Treiber wie bei Dubler 2.
- **Pitch → MIDI**, monophon:
  - YIN/MPM
  - Gate mit Hysterese, Stickiness-Regler mit Latenzanzeige in ms
  - Scale-Lock
  - Pitchbend in den Modi „Anchored“ und „True“
  - Legato/Retrigger
- **Akkord-Modus**: diatonisch aus der Skala oder feste Form. Voicings kommen aus einer Tabelle.
- **Beatbox → Drum-Trigger**:
  - Onset über HFC + Flux/SuperFlux
  - MFCC + spektrale Features, kNN mit Klasse „Unbekannt“
  - bis 8 Sounds pro Profil, Training mit 5–12 Beispielen pro Sound
  - Velocity
- **Expression → CC**: Hüllkurve, Spectral Flatness, Brightness (Zentroid/MFCC) und Vokal-Gewichte bzw. F1/F2 mit Kalibrierung pro Nutzer.
- **Capture**: Audio und Events eines Takes aufnehmen, die Detektionsverzögerung herausrechnen, dann `.mid` und `.wav` per Drag & Drop nach FL ziehen. Zusätzlich gibt es „Speichern unter“.
- **Interne Klangerzeugung** als Fallback:
  - Drum-Sampler, auch mit eigenen Beatbox-Samples
  - einfacher Synth
- **Drei Ausgabewege** nach RESEARCH 2.5:
  - Plugin-MIDI-Out
  - interne Engine
  - Capture/Drag & Drop, optional virtueller MIDI-Port
- **Öffentlicher Quellcode** mit CI und pluginval. JUCE läuft unter der kostenlosen JUCE-Lizenz (Starter); die Lizenz des eigenen Codes ist noch offen (siehe Abschnitt 6).

### 1.2 Später (nach 1.0)

- CLAP als vollwertiges Format, falls T5 einen Vorteil zeigt. Gebaut wird es aber schon ab M0.
- macOS mit AU, Signierung und Notarisierung.
- ML-Stufe nach RESEARCH 3.6:
  - RTNeural-Embedding-CNN für Trigger (Few-Shot mit Prototypen)
  - SwiftF0 per ONNX Runtime im Hintergrund für „Clean MIDI“
- MPE-Ausgabe. Session-Historie und Tonart-Vorschlag.
- Auto-Slicing von Beatbox-Takes zu einem Sample-Kit (WAV-Ordner).

### 1.3 Nicht-Ziele

- Polyphone Erkennung (Gesang ist monophon), Offline-Transkription in Melodyne-Qualität, Cloud-Funktionen.
- VST2. Das SDK ist für neue Entwickler nicht lizenzierbar.
- AAX.
- Ein eigener virtueller MIDI-Treiber wie bei Dubler. Wir nutzen nur, was JUCE und das Betriebssystem bieten.
- Modelle mit nicht-kommerziellen Lizenzen, z. B. die NeuralNote-Gewichte (CC BY-NC).

---

## 2. Architektur-Überblick

### 2.1 Module

**`core/`** ist die statische Bibliothek `VocalSynthCore`, in C++23 ohne `AudioProcessor`-, GUI- und Host-Abhängigkeit. Als FFT-Backend dient `juce::dsp::FFT` hinter einer dünnen Abstraktion, optional PFFFT. Alles in `core/` lässt sich headless testen, wird in `prepare()` vorallokiert und läuft echtzeitsicher.

| Modul | Aufgabe |
|---|---|
| `InputConditioner` | DC-Filter, HPF 60 Hz, optionales Noise-Gate |
| `Decimator` | Polyphase-Halbband auf 16/24 kHz (Pitch) bzw. ca. 11 kHz (LPC), geringe Latenz |
| `Framer` | eigener Hop-Takt (64–256 Samples), Ringpuffer; Ergebnis unabhängig von der Host-Blockgröße |
| `PitchDetector` | Interface mit den Implementierungen `Yin` (FFT-basiert) und `Mpm` (NSDF); liefert f0, Confidence, RMS |
| `NoteSegmenter` | Voicing-Gate mit Hysterese, Median 3–5 Frames, Oktav-Lock, Stickiness (0,5 + h Halbtöne, T_min), Velocity |
| `ScaleQuantizer` / `ChordMapper` | Skalen-Snap (immer oder nur am Notenanfang), Akkordformen, Voicings |
| `BendGenerator` | Residuum in Cent, Totzone (Anchored) oder 1:1 (True), Bend-Range, Ausgaberate 200–500 Hz mit Glättung |
| `OnsetDetector` | Energie-Vorstufe, HFC, SuperFlux; adaptive Schwelle, Refractory 30–50 ms, höchstens 1–2 Frames Lookahead |
| `FeatureExtractor` | 13–20 MFCC + ΔMFCC, Zentroid, Flatness, Rolloff, ZCR, Attack-Steilheit, Band-Energien |
| `TriggerClassifier` | z-Normierung, kNN (k = 3–5, Cosinus-Distanz), Distanzschwelle für „Unbekannt“, Velocity-Kalibrierung pro Klasse; Modi „Fast“ (~10–15 ms) und „Accurate“ (~25–40 ms) |
| `ExpressionTracker` | RMS-Hüllkurve, Flatness, Brightness, LPC/Levinson-Durbin → F1/F2 → Vokal-Ebene, Glättung; CC-Ratenbegrenzung |
| `EventQueue` | eigenes `VsEvent`-Format (Note, Bend, CC, Trigger) mit Sample-Offset; `juce::MidiMessage` gibt es nur in `plugin/` |
| `CleanMidi` (offline) | Ghost- und Übergangsnoten entfernen, Latenz-Kompensation, später pYIN/SwiftF0-Nachanalyse |

**`plugin/`** ist der JUCE-Teil. Zwei Targets entstehen aus demselben Code (RESEARCH 4.1):

- **„VocalSynth FX“** mit `IS_SYNTH FALSE`, `NEEDS_MIDI_OUTPUT TRUE`, Audio-In.
- **„VocalSynth Instrument“** mit `IS_SYNTH TRUE`, MIDI-Out und Sidechain-Input.

Bausteine:

- `VocalSynthProcessor`: APVTS, State-Verwaltung, `processBlock` auf Basis von `core`. Gemeldete Latenz ist **0**.
- `MidiOutputStage`: `VsEvent` → `MidiBuffer`, sample-genau. Ein Stuck-Note-Guard sendet Note-Offs bei Bypass, Reset, Kanal- oder Moduswechsel. Trigger bekommen einen Note-Off-Countdown.
- `InternalEngine`: Drum-Sampler mit vorgeladenen Samples und einfacher Synth. Spielt dieselben `VsEvent`s.
- `CaptureRecorder`: SPSC-FIFO (Audio + Events) → Writer-Thread.
- `CaptureExporter`: `juce::MidiFile` und WAV, BPM aus der Host-Playhead.
- `VirtualMidiPort`: `MidiOutput::createNewDevice`, gespeist über ein FIFO, nur optional.
- `ModelStore`: hält das kNN-Profil im atomaren Pointer und gibt alte Modelle verzögert frei.
- `ProfileStore`: Trigger- und Vokalprofile als Child-`ValueTree` mit `version`.

**`ui/`** läuft im Message-Thread und liest nur Atomics und FIFOs. Komponenten:

- Editor
- TunerView und PitchCurveView
- TriggerPads mit Live-Konfidenz
- TrainingWizard mit Konfusionsmatrix
- VowelXY
- CapturePanel als Drag-Quelle
- LatencyBadge
- RoutingHelp mit dem FL-Rezept aus M1

### 2.2 Thread-Modell

Eine Festlegung aus RESEARCH 5.2 gilt durchgehend: Die **Live-Analyse läuft im Audio-Thread**. Ein separater Analyse-Thread brächte Jitter und mindestens einen Block Latenz. Hintergrund-Threads bekommen nur teure, nicht zeitkritische Arbeit.

```
 Mic ─► processBlock (Audio-Thread, keine Allokation/Locks/IO)
        │ InputConditioner ─► Decimator ─► Framer (eigener Hop)
        │      ├─ OnsetDetector ─► FeatureExtractor ─► TriggerClassifier ◄─┐ Modell (atomic ptr)
        │      ├─ PitchDetector ─► NoteSegmenter ─► ScaleQuantizer/Chord   │
        │      │                    └─► BendGenerator                      │
        │      └─ ExpressionTracker ─► CC-Smoother                         │
        │ ─► EventQueue (VsEvent, Sample-Offset)                           │
        │      ├─► MidiOutputStage ─► MidiBuffer (Note/PB/CC → VST3 Legacy CC Out)
        │      ├─► InternalEngine ─► Audio-Out (optional, sonst Dry-Passthrough)
        │      ├─► SPSC ─► [Capture-Writer-Thread]  WAV + Events → Temp-Dateien
        │      ├─► SPSC ─► [VirtualMidi-Thread]     MidiOutput (optional)
        │      ├─► SPSC ─► [Training-Thread]        Feature-Vektoren sammeln
        │      └─► Atomics/SPSC ─► [UI-Timer 30–60 Hz] Pitch, Pegel, Trigger-Log
        │                                                                  │
 [Training-Thread] baut kNN-Profil ─► ModelStore.publish() ────────────────┘
                   altes Modell ─► Retire-Queue ─► Freigabe außerhalb des Audio-Threads
 [Message-Thread]  UI, APVTS-Attachments, Drag & Drop (performExternalDragDropOfFiles)
 [Offline-Job]     CleanMidi / Nachanalyse eines Takes (später SwiftF0/ONNX)
```

- **FIFOs:**
  - `juce::AbstractFifo` mit vorallokierten `std::array`s, strikt SPSC.
  - Ist ein FIFO voll, verwirft der Audio-Thread die Daten und zählt ein Overflow-Atomic hoch. Er wartet nie.
- **Modell-Swap:**
  - Ein atomarer Rohzeiger mit Retire-Queue oder ein Double-Buffer.
  - `std::atomic<std::shared_ptr>` ist auf gängigen Toolchains meist nicht lock-free. Außerdem darf der letzte `shared_ptr` nie im Audio-Thread freigegeben werden.
- **Parameter:**
  - Gecachte `getRawParameterValue`-Atomics.
  - Parameter-IDs mit Versionshinweis, nach dem ersten Release unveränderlich.
- **CPU-Budget:**
  - Worst-Case von `processBlock` unter 20 % der Blockzeit (SKILL.md).
  - Teure Schritte wie MFCC und LPC werden über die Hops verteilt, damit kein einzelner Block eine CPU-Spitze erzeugt.

### 2.3 Datenfluss für Capture

1. Der Audio-Thread schreibt das Mic-Audio (dezimiert oder in voller Rate) und alle `VsEvent`s mit absolutem Sample-Zeitstempel ins Capture-FIFO.
2. Der Writer-Thread schreibt die WAV über `AudioFormatWriter::ThreadedWriter` und puffert die Events.
3. Beim Stop rechnet `CleanMidi` die bekannte Detektionsverzögerung heraus, quantisiert optional und erzeugt eine Raw- und eine Clean-Variante.
4. Das Ergebnis landet als Temp-Datei, die das CapturePanel per Drag & Drop an FL übergibt: MIDI in ein Pattern, WAV in Playlist oder Channel Rack.

### 2.4 Verzeichnisstruktur (geplant)

```
VocalSynth/
├─ CMakeLists.txt            # C++23, CMake ≥ 3.25 (nach dem Vorbild von pamplejuce)
├─ cmake/                    # Hilfsfunktionen, Versionierung
├─ JUCE/                     # Submodule, gepinnt auf Tag 9.0.4
├─ modules/clap-juce-extensions/ # Submodule, gepinnt auf einen Commit nach 0.26.0 (JUCE-9-Support gibt es erst auf main)
├─ core/                     # VocalSynthCore (ohne GUI/Host)
│  ├─ include/vocalsynth/    # Framer.h, PitchDetector.h, NoteSegmenter.h, ...
│  └─ src/
├─ plugin/                   # Processor, MidiOutputStage, InternalEngine, Capture*, VirtualMidiPort
├─ ui/                       # Editor und Views
├─ tests/
│  ├─ unit/                  # Catch2 v3
│  ├─ data/                  # kleine WAV-Fixtures (< 1 MB gesamt)
│  └─ corpus/                # Manifest + Labels; Audio extern oder per Git LFS
├─ tools/
│  ├─ vs-eval/               # CLI: Korpus → Metriken (JSON) für Regressionen
│  └─ vs-spike/              # Testmuster-Generator für M1
├─ resources/                # Default-Samples, Presets
├─ packaging/                # Installer-Skripte
├─ docs/                     # RESEARCH.md, PLAN.md, FL-ROUTING.md, LATENCY.md
└─ .github/workflows/        # build_and_test.yml, nightly.yml
```

---

## 3. Meilensteine

Gesamtdauer bis 1.0 (Windows): **ca. 7–10 Monate** bei 8–10 h/Woche.

| Meilenstein | Inhalt | Dauer |
|---|---|---|
| M0 | Repo, Build, CI | 1–2 Wochen |
| M1 | FL-Studio-Routing-Spike (Go/No-go) | 1–2 Wochen |
| M2 | Pitch → MIDI MVP | 5–7 Wochen |
| M3 | Capture und Drag & Drop | 3–4 Wochen |
| M4 | Beatbox-Trigger mit Training | 5–7 Wochen |
| M5 | Expression → CC und Akkord-Modus | 3–4 Wochen |
| M6 | Interne Engine (Sampler/Synth) | 3–4 Wochen |
| M7 | UI/UX | 3–5 Wochen |
| M8 | Beta und Release 1.0 (Windows) | 3–4 Wochen |

### M0 – Repo, Build, CI (1–2 Wochen)

Ziel: ein leeres, aber valides Plugin, das auf allen drei CI-Plattformen baut und pluginval besteht.

- [x] Repo nach dem Vorbild von pamplejuce aufsetzen (eigenes schlankes CMake). JUCE 9.0.4 als Submodule pinnen, clap-juce-extensions einbinden (gepinnt auf einen Commit nach 0.26.0, weil erst `main` JUCE 9 unterstützt).
- [x] Targets „VocalSynth FX“ (VST3, CLAP, Standalone) und „VocalSynth Instrument“ (VST3, CLAP) anlegen, AU nur im macOS-Build.
- [x] 4-Zeichen-Codes festlegen und als „nie ändern“ dokumentieren (`cmake/VocalSynthPlugin.cmake`): Manufacturer `Tr3n`, FX `VsFx`, Instrument `VsIn`, Bundle-IDs `com.tr3nben.vocalsynth.fx` / `.instrument`.
- [x] `core/` als eigene statische Bibliothek mit Catch2-Testprojekt anlegen (erste echte Funktionen: Frequenz ↔ MIDI-Note, Cent-Abstand).
- [x] CI-Pipeline:
  - [x] Linux, Windows und macOS bauen
  - [x] Unit-Tests ausführen (Core + Processor über Blockgrößen {1, 7, 64, 480, 512, 4096} und 44,1/48/96 kHz)
  - [x] pluginval laufen lassen, direkt auf Level 10 inklusive GUI-Tests
  - [x] Steinberg-`validator` laufen lassen
  - [x] Zusätzlich: `clap-validator` für beide CLAPs (seit M1 ohne Ausnahmen) und `auval` auf macOS
- [ ] Signing-Schritte nur ausführen, wenn Secrets gesetzt sind. So bleibt die CI für Forks grün. *(verschoben nach M8, Releases bis dahin unsigniert)*
- [x] JUCE-Lizenz entscheiden: kostenlose JUCE-Lizenz (Starter) statt AGPLv3 (Entscheidung vom 2026-10-14).
- [ ] Lizenz für den eigenen Code festlegen, dann `LICENSE` und `CONTRIBUTING.md` anlegen.
- [x] Fertiges Windows-Paket aus der CI: ZIP mit `Install-VocalSynth.bat`, installiert nach `<VST3-Ordner>\VocalSynth\` und `<CLAP-Ordner>\VocalSynth\`.
- [x] Projekt-Skill (`juce-audio-plugin-dev/SKILL.md`) an JUCE 9 / C++23 / CMake ≥ 3.25 angleichen.
- [ ] Manuell: beide Plugins in FL Studio 2026 laden (CI-Artefakt „VocalSynth-Windows-…“) und prüfen, dass sie ohne Absturz laufen.

**Lieferung:** CI-Artefakte (`.vst3`, `.clap`, Standalone).
**Abnahme:** CI auf drei Plattformen grün. pluginval besteht Level ≥ 5 mit FX und Instrument. Das Plugin lädt in FL 2026 ohne Absturz.

### M1 – FL-Studio-Routing-Spike: Go/No-go-Gate (1–2 Wochen)

Ziel: klären, auf welchem Weg MIDI zuverlässig von VocalSynth zum Klangerzeuger kommt. Erst danach wird der primäre Ausgabeweg festgelegt.

- [x] Testmodus im Plugin bauen (als Parameter „Test Pattern“ statt eines eigenen `tools/vs-spike`; Design in `docs/M1-DESIGN.md`, Testbuild Version 0.1.0):
  - [x] Notenfolge, Pitchbend-Sweep und CC-Sweep (Parameter für MIDI-Kanal, Tempo und CC-Nummer; Modus „All“ im Wechsel)
  - [x] Pegelanzeige für ankommendes Audio, wichtig für T3 (dazu Anzeige der Eingangskanäle)
  - [x] Histogramm der Host-Blockgrößen über Atomics, angezeigt im UI
  - [x] Zähler der gesendeten MIDI-Nachrichten im UI („nicht gesendet“ vs. „nicht geroutet“) und Anzeige des geladenen Formats (VST3/CLAP, für T5)
  - [x] Monitor-Ton im Plugin für T6 und virtueller MIDI-Port (Parameter „Virtual MIDI Out“) für T7
  - [x] Test-`.mid` und Test-`.wav` zum Herausziehen per Drag & Drop
  - [x] Keine hängenden Noten bei Musterwechsel, Bypass und Neustart der Verarbeitung (Unit-Tests über Blockgrößen {1, 7, 64, 480, 512, 4096})
- [x] Testanleitung `docs/FL-ROUTING.md` mit Klickwegen für T1–T7, Zusatzchecks und Ergebnistabelle zum Ausfüllen.
- [ ] T1–T7 nach RESEARCH 2.5 in FL 2026 (Windows, ASIO, 128–256 Samples) durchführen. Pro Test notieren, ob Note, Pitchbend und CC ankommen, mit Screenshot des Wrapper-Setups.
- [ ] Zusatzchecks, die Capture und DSP absichern:
  - [ ] Drag & Drop einer Test-`.mid` in ein Pattern und einer `.wav` in die Playlist
  - [ ] Blockgrößen mit und ohne „Use fixed size buffers“
  - [ ] gemeldete Latenz 0 verschiebt nichts
- [ ] T7: virtueller Port über Windows MIDI Services (Windows 11 24H2/25H2) testen. Prüfen, ob FL ihn als Input sieht und aufnimmt, und was bei mehreren Plugin-Instanzen passiert.
- [ ] Ergebnisse in `docs/FL-ROUTING.md` festhalten: FL-Version, Build, Datum, Ergebnistabelle, Schritt-für-Schritt-Rezept.

**Entscheidungsregel:**

| Ergebnis | Primärer Weg | Folge für den Plan |
|---|---|---|
| T1 o.k. (Note + PB + CC) | FX-Target im Mic-Insert, Port-Routing | Plan wie unten |
| T1 scheitert, T3 o.k. | Instrument-Target mit Sidechain | FX-Target bleibt sekundär |
| nur T4 (Patcher) o.k. | Patcher-Rezept als Standard | RoutingHelp in M7 aufwerten |
| T5 besser als VST3 | CLAP für FL priorisieren | CLAP-Tests in die CI-Pflicht |
| T1–T5 scheitern | interne Engine (T6) + Capture + T7 | **M6 rückt direkt hinter M2** |

**Abnahme:** Für jeden Test T1–T7 ist das Ergebnis dokumentiert. Der primäre Weg ist entschieden und in README/Doku eingetragen. SKILL.md (Abschnitt 7) ist an die Testergebnisse angepasst.

### M2 – Pitch → MIDI MVP (5–7 Wochen)

Ziel: Singen erzeugt in FL saubere Noten mit Bend.

- [ ] `Framer`, `InputConditioner` und `Decimator` mit Blockgrößen-Invarianztests.
- [ ] `Yin` und `Mpm` hinter einem gemeinsamen Interface. Range-Presets: Bass, Tenor, Sopran, Pfeifen.
- [ ] `NoteSegmenter`: Gate mit Hysterese, Median, Oktav-Lock, Stickiness-Regler, minimale Notenlänge, Velocity aus der Onset-Energie.
- [ ] `ScaleQuantizer` mit Skalen-Snap am Anfang oder immer und `BendGenerator` (Anchored/True, Range ±2/±12/±48, Ratenbegrenzung).
- [ ] Legato/Retrigger-Schalter. `MidiOutputStage` mit Stuck-Note-Guard.
- [ ] Ein minimales UI zeigt Tuner, aktuelle Note und Latenz in ms (Fenster + Gate + Stickiness).
- [ ] `vs-eval` misst den Pitch-Teil des Korpus und vergleicht YIN mit MPM.
- [ ] Stretch: Note-On beim Onset mit vorläufiger Tonhöhe, danach Korrektur per Bend (RESEARCH 5.4).

**Abnahme** (gesetzte Projektziele, keine gemessenen Fakten):
- Sinus und Sägezahn von 80–1000 Hz werden auf ±10 Cent genau erkannt.
- Stille und Rauschen erzeugen 0 Noten.
- Ergebnisse sind bitgleich für Blockgrößen {1, 7, 64, 480, 512, 4096} und 44,1/48/96 kHz.
- Auf dem Gesangskorpus:
  - Oktavfehlerrate < 2 % der stimmhaften Frames
  - Note-F1 ≥ 0,85 (Onset-Toleranz ±50 ms)
- Algorithmische Note-On-Verzögerung ≤ 30 ms im Preset Tenor und ≤ 40 ms im Preset Bass.
- CPU: Worst-Case < 20 % der Blockzeit, Mittel < 5 % (Release, 48 kHz, Block 128, Mittelklasse-Laptop).
- MIDI-Invarianten: Jede Note-On hat eine Note-Off, keine doppelten Note-Ons.

### M3 – Capture und Drag & Drop (3–4 Wochen)

- [ ] `CaptureRecorder` mit SPSC-FIFO zum Writer-Thread, BPM und Position aus der Playhead. Aufnahme läuft bei laufendem Transport oder manuell.
- [ ] `CaptureExporter` erzeugt `.mid` (Raw/Clean) und `.wav`. Die bekannte Detektionsverzögerung wird herausgerechnet.
- [ ] Erste Stufe von `CleanMidi`: Ghost-Noten unter Mindestlänge entfernen, Übergangsnoten zusammenfassen.
- [ ] CapturePanel mit Drag-Quelle (`performExternalDragDropOfFiles`), „Speichern unter“ und Aufräumen der Temp-Dateien.
- [ ] Optional nach T7: `VirtualMidiPort` als Ausgabeweg mit eigenem FIFO und Thread.

**Abnahme:**
- Drag & Drop nach FL funktioniert für Pattern und Playlist.
- Zeitversatz zwischen Audio und MIDI im Export ≤ 1 Hop (Messung siehe 4.5).
- Kein Audio-Dropout bei 10-Minuten-Takes.
- FIFO-Overflow-Zähler bleibt bei 0.

### M4 – Beatbox-Trigger mit Training (5–7 Wochen)

- [ ] `OnsetDetector` (HFC + SuperFlux, adaptive Schwelle, Refractory) und `FeatureExtractor` (MFCC + Deskriptoren).
- [ ] `TriggerClassifier` (kNN, Cosinus-Distanz, „Unbekannt“-Schwelle) mit den Modi Fast und Accurate. Die Klasse wird in der Capture-Spur nachträglich korrigiert.
- [ ] Training-Pfad:
  - [ ] Features per SPSC an den Training-Thread
  - [ ] Profil bauen
  - [ ] Modell atomar tauschen, das alte außerhalb des Audio-Threads freigeben
- [ ] Bis 8 Sounds pro Profil, Notenbelegung und MIDI-Kanal pro Sound (Standard: Kanal 10, Noten getrennt vom Pitch-Kanal).
- [ ] Profile im State speichern (versioniert) und als Datei exportieren/importieren.
- [ ] Minimaler Drum-Sampler mit Default-Kit, damit Trigger ohne Routing hörbar sind. Das ist die Vorstufe zu M6.

**Abnahme:**
- Synthetische Impulse und aufgenommene Hits erzeugen genau einen Trigger am richtigen Sample (±1 Hop).
- Bei 4 Klassen mit 5–12 Trainingsbeispielen pro Klasse: Accuracy ≥ 90 % im Modus Accurate und ≥ 80 % im Modus Fast, gemessen auf einem separaten Testteil des Korpus.
- Fehlauslösungen ≤ 1 pro Minute bei Raumgeräusch und Sprache.
- Onset → MIDI ≤ 15 ms im Modus Fast.
- Ein Modell-Swap während der Wiedergabe erzeugt keinen Glitch.

### M5 – Expression → CC und Akkord-Modus (3–4 Wochen)

- [ ] `ExpressionTracker`: Hüllkurve, Flatness, Brightness, LPC-Formanten F1/F2 mit Median + One-Pole.
- [ ] Vokal-Kalibrierung: jeden Vokal einmal halten, Referenzpunkte speichern. Ausgabe als 2 CCs oder 3 Morph-Gewichte.
- [ ] CC-Mapping (Quelle → CC-Nummer, Bereich, Kurve, Glättung) mit Ratenbegrenzung.
- [ ] `ChordMapper`: diatonisch oder feste Form, Voicings und Inversionen.

**Abnahme:**
- Auf kalibrierten Vokalen (a, e, i, o, u) wird die richtige Vokal-Region in ≥ 85 % der Frames erkannt.
- CCs kommen im gewählten FL-Weg an (Ergebnis von T1/T4).
- Die CC-Rate bleibt unter der eingestellten Grenze.
- Akkorde erzeugen keine hängenden Noten.

### M6 – Interne Engine: Sampler und Synth (3–4 Wochen; bei No-go in M1 direkt nach M2)

- [ ] Sampler:
  - [ ] Velocity-Layer und Choke-Gruppen (z. B. Hi-Hat)
  - [ ] Samples nur außerhalb des Audio-Threads laden, Übergabe per atomarem Swap
- [ ] Einfacher Synth: 1–2 Oszillatoren, Filter, ADSR, Glide; Pitchbend und CC aus `VsEvent`.
- [ ] Mix-Regler zwischen Dry-Mic und Engine, „Mute Dry“.
- [ ] Beatbox-Kit-Generator: aus einem Take an den Onsets schneiden, normalisieren, Fade, als WAV-Ordner exportieren.

**Abnahme:**
- T6 funktioniert ohne jedes Routing.
- 16 Stimmen bleiben im CPU-Budget aus M2.
- Kein Klick beim Sample-Tausch.

### M7 – UI/UX (3–5 Wochen)

- [ ] Endgültiges Layout:
  - [ ] Tuner und Pitch-Kurve
  - [ ] Trigger-Pads mit Konfidenz
  - [ ] Training-Wizard („Sound 5× wiederholen“, Konfusionsmatrix)
  - [ ] Vokal-XY, CC-Matrix
  - [ ] Capture-Panel
- [ ] Latenzanzeige: aktuelle Analyseverzögerung in ms und Hinweis auf den ASIO-Puffer.
- [ ] RoutingHelp im Plugin mit dem FL-Rezept aus M1. Kopfhörer-Hinweis gegen Übersprechen.
- [ ] Presets und Profile, Skalierung/HiDPI. Melatonin Inspector nur im Debug-Build.

**Abnahme:**
- Ein Testnutzer schafft das Erst-Setup in FL (Routing + Training von 4 Sounds) in ≤ 10 Minuten.
- Das UI läuft mit 30–60 Hz, ohne das Audio-Budget zu beeinflussen.
- pluginval Level 10 inklusive GUI-Tests grün.

### M8 – Beta und Release 1.0 (Windows; 3–4 Wochen)

- [ ] Geschlossene Beta (5–10 Nutzer) mit der FL-Checkliste (4.4) und einem Fehlerbericht-Template.
- [ ] Installer für Windows. Prüfen, was pamplejuce bereits mitbringt. Zusätzlich ein ZIP mit Installationsanleitung.
- [ ] Signierung nach der Entscheidung in Abschnitt 6. Unsigniert mit SmartScreen-Hinweis in der Doku.
- [ ] Release-Workflow: Tag → CI-Build → GitHub-Release mit Quellcode-Link (bei AGPL Pflicht).
- [ ] Doku: Installation, FL-Routing, Latenz-Leitfaden (ASIO, Puffergrößen), Troubleshooting.
- [ ] Danach: macOS-Release (AU, Notarisierung) und ML-Stufe als eigene Meilensteine (M9+).

**Abnahme:**
- Keine bekannten Abstürze oder hängenden Noten.
- Die FL-Checkliste ist auf mindestens 2 Rechnern vollständig bestanden.
- Projekte aus der Beta laden in 1.0 ohne Parameterverlust.

---

## 4. Teststrategie

### 4.1 Unit-Tests (Catch2 v3, `core/` headless)

- **Synthetische Signale** mit festen Seeds:
  - Sinus, Sägezahn, Fundamentale ohne Grundton
  - Vibrato (±50 Cent, 5–6 Hz), Glissandi, Oktavsprünge
  - weißes und rosa Rauschen, Stille, Impulse, Klick-Folgen mit bekannten Onsets
- **Invarianzen:**
  - Jeder Test läuft mit 44,1/48/96 kHz und den Blockgrößen {1, 7, 64, 480, 512, 4096}; die Ergebnisse müssen gleich sein.
  - Der Framer erzeugt pro Hop genau einen Frame, egal wie die Blöcke geschnitten sind.
- **MIDI-Invarianten:** Note-On/Off paarig, Offsets in `[0, numSamples)`, keine doppelten Note-Ons, Flush bei Reset und Bypass.
- **Echtzeit-Sicherheit:** Tests mit einem Allokations-Zähler (überladenes `operator new`) im `process()`-Pfad.
- **Benchmarks:** Catch2-Benchmarks pro Modul (Frames/s), als Regressionswarnung in der Nightly.

### 4.2 Gelabelter Korpus

- **Inhalt:**
  - eigene Aufnahmen, später ergänzt durch Beta-Nutzer mit Einverständnis
  - Sprecher: mindestens 3 Stimmlagen, Mikros dynamisch und Kondensator, ruhiger Raum und Hintergrundgeräusche
  - Material: Gesangsphrasen (langsam/schnell, mit Vibrato), Vokale gehalten, Beatbox (Kick, Snare, Hi-Hat, Rimshot und weitere), Störgeräusche (Sprache, Klopfen, Atmen)
- **Labels** als JSON/CSV:
  - Notenliste (Onset, Offset, MIDI-Note)
  - f0-Referenz (offline erzeugt, manuell geprüft)
  - Trigger-Onsets mit Klasse
  - Vokal-Abschnitte
- Externe Datensätze wie AVP/VPT nur nach Lizenzprüfung.
- **Ablage:** Kleine Fixtures liegen in `tests/data`. Der Korpus selbst liegt extern bzw. in Git LFS und ist über ein Manifest mit Prüfsummen referenziert.
- **`vs-eval`** liefert:
  - Raw Pitch Accuracy (±50 Cent), Oktavfehlerrate
  - Note-F1 (±50 ms)
  - Trigger-Accuracy + Konfusionsmatrix, Fehlauslösungen pro Minute
  - Vokal-Trefferrate
- Ergebnisse als JSON. Die Nightly vergleicht gegen eine Baseline und warnt bei Verschlechterung über 1 Prozentpunkt.

### 4.3 pluginval und Validatoren in der CI

- Pro PR werden beide Targets geprüft:
  - pluginval VST3, Ziel `--strictness-level 10` (bis M7 mit `--skip-gui-tests`)
  - Steinberg-`validator`
  - auf macOS zusätzlich `auval`
- Für CLAP wird mindestens das Laden geprüft. Ob es einen CLAP-Validator in der CI geben soll, ist in M0 offen.

### 4.4 Manuelle FL-Studio-Checkliste (pro Release-Kandidat)

- [ ] Installation, „Find installed plugins“, Erkennung als Effekt bzw. Instrument.
- [ ] Primärer Routing-Weg nach `docs/FL-ROUTING.md`: Noten, Pitchbend und CC kommen an.
- [ ] Puffergröße und Sample-Rate mitten in der Session ändern, „Use fixed size buffers“ an und aus.
- [ ] Bypass und Deaktivieren bei klingenden Noten führen nicht zu hängenden Noten. Plugin bei laufendem Audio entfernen.
- [ ] Projekt speichern und neu laden: Parameter, Trigger-Profil und Vokal-Kalibrierung bleiben erhalten.
- [ ] Automation aller Parameter. Mehrere Instanzen, auch mit virtuellem Port (T7).
- [ ] Drag & Drop von `.mid` und `.wav`, Lage im Takt korrekt.
- [ ] CPU-Anzeige von FL bei 128 Samples Puffer.

### 4.5 Latenzmessung

- **Algorithmisch (automatisch):**
  - In Unit-Tests ist der Sample-Index jedes synthetischen Onsets bzw. Tonbeginns bekannt.
  - Differenz zum Sample-Offset des erzeugten MIDI-Events, ausgewertet als Median und 95. Perzentil.
  - Diese Werte zeigt auch das UI an.
- **End-to-End (manuell):**
  - Ein Klick bzw. eine gesungene Silbe wird gleichzeitig über einen zweiten Kanal des Interfaces (Referenz) und über das Mikro aufgenommen.
  - Der Synth-Ausgang (interne Engine bzw. geroutetes Instrument) wird per Loopback-Kabel auf einen dritten Eingang gelegt.
  - Im Audio-Editor wird der Versatz zwischen Referenz-Onset und Synth-Onset gemessen.
  - Messung bei 64/128/256 Samples ASIO-Puffer, Ergebnisse in `docs/LATENCY.md`.
- **Budget** bei 48 kHz und 128 Samples ASIO (je ca. 2,7 ms In/Out):
  - Trigger Fast: Ziel ≤ 20 ms gesamt
  - Pitch Tenor: Ziel ≤ 35 ms gesamt (Fenster ≥ 25 ms bei tiefen Stimmen ist physikalisch bedingt)

---

## 5. Risiken & Gegenmaßnahmen

| Risiko | Wahrscheinlichkeit / Auswirkung | Gegenmaßnahme |
|---|---|---|
| VST3-MIDI-Out (v. a. PB/CC) aus FX oder Instrument kommt in FL nicht an | mittel / hoch | M1 als Gate, drei Ausgabewege, Patcher-Rezept, CLAP-Build, interne Engine vorziehen |
| Plugin-MIDI ist in FL nicht direkt aufnehmbar | hoch / mittel | eigener Capture mit Drag & Drop (M3), virtueller Port über Windows MIDI Services (T7) |
| Virtueller Port: Windows MIDI Services nicht überall ausgerollt, Konflikte bei mehreren Instanzen | mittel / niedrig | optional, eindeutige Port-Namen pro Instanz, Capture bleibt Hauptweg |
| Gesamtlatenz bei tiefen Stimmen fühlt sich träge an | hoch / mittel | Range-Presets, MPM-Kurzfenster, Onset-Note-On mit Bend-Korrektur, Latenzanzeige, ASIO-Leitfaden |
| Oktavfehler, „Spaghetti-MIDI“ | mittel / hoch | Median, Oktav-Lock, Stickiness-Regler, Clean-MIDI im Capture, Korpus-Regressionen |
| Fehlklassifikation, Übersprechen von Lautsprechern | mittel / mittel | „Unbekannt“-Klasse, Konfidenzanzeige, Kopfhörer-Hinweis, mehr Trainingsbeispiele |
| FL schickt variable oder winzige Blöcke | sicher / mittel | eigener Framer, Blockgrößen-Invarianztests, Last über Hops verteilen |
| CPU-Spitzen durch MFCC/LPC im Audio-Thread | mittel / mittel | Budget-Messung pro Release, Arbeit über Hops verteilen, Downsampling |
| Echtzeit-Verletzungen (Allokation, Locks) | mittel / hoch | Projektregeln in SKILL.md, Allokations-Zähler in Tests, Review-Skills `juce-review` und `audio-dsp-review` |
| Korpus zu klein oder zu einseitig (nur eigene Stimme) | hoch / mittel | Beta-Nutzer-Aufnahmen, mehrere Mikros und Räume, Metriken pro Sprecher |
| Zeitbudget als Hobbyprojekt läuft aus dem Ruder | hoch / mittel | strenge MVP-Grenze, Gate in M1, ML und macOS erst nach 1.0 |
| AGPL-Pflichten schrecken Forks oder Mitwirkende ab | niedrig / mittel | Lizenz in README und CONTRIBUTING klar dokumentieren, Entscheidung zu einer CLA früh treffen |
| Code-Signing für Einzelpersonen nicht verfügbar | mittel / niedrig | unsignierte Releases mit Doku, später über Verein, Firma oder Sponsor |
| ML-Abhängigkeiten (ONNX-Größe, Modelllizenzen) | mittel / mittel | v1 ohne ML, nur MIT/BSD-Modelle (SwiftF0 ist MIT), RTNeural bevorzugen |

---

## 6. Offene Entscheidungen

| Frage | Optionen | Empfehlung |
|---|---|---|
| **Lizenz** | AGPLv3 (JUCE kostenlos) oder kommerzielle JUCE-9-Lizenz (Starter kostenlos bis 20.000 USD, Indie, Pro) | **Entschieden (2026-10-14): JUCE über die kostenlose Starter-Lizenz.** Offen bleibt die Lizenz des eigenen Codes (z. B. MIT oder proprietär). Vor externen Beiträgen klären, ob eine CLA nötig ist. |
| **FX- vs. Instrument-Target als Standard** | FX im Mic-Insert oder Instrument mit Sidechain | **Erst nach M1 entscheiden.** Bis dahin beide bauen. |
| **CLAP-Priorität** | gleichrangig mit VST3 oder nur Zusatzformat | **Ab M0 mitbauen (kostet wenig), aber VST3 bleibt primär.** CLAP rückt nur auf, wenn T5 in FL besser abschneidet. |
| **macOS-Zeitpunkt** | parallel oder nach dem Windows-Release | **Nach 1.0 (Windows).** CI baut macOS schon ab M0, damit nichts verrottet. Apple-Account (99 USD/Jahr) erst mit dem macOS-Release. |
| **ML vs. klassisches DSP** | sofort ML oder erst DSP | **v1 klassisch** (YIN/MPM, kNN). **v2:** RTNeural-Embedding für Trigger, SwiftF0 über ONNX nur im Hintergrund für Clean MIDI. |
| **YIN vs. MPM** | einer von beiden fest oder beide | **Beide hinter einem Interface.** In M2 per `vs-eval` auf dem Korpus entscheiden, eventuell als Preset wählbar. |
| **Code-Signing** | unsigniert, Azure Artifact Signing (9,99 USD/Monat, Zulassung für Einzelpersonen unbestätigt), Organisation | **Beta und 1.0 unsigniert** mit SmartScreen-Doku. Parallel klären, ob Artifact Signing für Einzelpersonen geht, sonst über eine Organisation. |
| **Virtueller MIDI-Port** | Standardfunktion oder optional | **Optional**, abhängig von T7 und Nutzerfeedback. Capture mit Drag & Drop bleibt der garantierte Weg. |

---

## 7. Nächste konkrete Schritte

1. Lizenz für den eigenen Code festlegen (JUCE läuft unter der kostenlosen Starter-Lizenz), dann `LICENSE` anlegen.
2. pamplejuce als Vorlage übernehmen. JUCE auf Tag 9.0.4 pinnen. CMake-Targets „VocalSynth FX“ und „VocalSynth Instrument“ (VST3 + CLAP + Standalone) anlegen. Plugin-Codes festlegen.
3. CI auf Linux, Windows und macOS grün bekommen: Build, Dummy-Unit-Test, pluginval Level 5, Steinberg-`validator`, Signing nur bei gesetzten Secrets.
4. Testmuster-Modus (Noten, Pitchbend-Sweep, CC-Sweep, Blockgrößen-Histogramm, Sidechain-Pegel) für den M1-Spike bauen.
5. Spike T1–T7 plus Drag-&-Drop- und Blockgrößen-Checks in FL 2026 durchführen. Ergebnisse und Rezept in `docs/FL-ROUTING.md` festhalten. Go/No-go nach der Tabelle in M1 treffen.
6. Parallel einen ersten Mini-Korpus aufnehmen und labeln: ca. 20 Gesangsphrasen, 4 Beatbox-Klassen à 20 Hits, 5 Vokale, Störgeräusche. Das Label-Format festlegen.
7. In `core/` anfangen mit `Framer`, `Decimator` und `Yin`, inklusive der Blockgrößen- und Sinus-Tests aus 4.1. Das ist der Einstieg in M2.
