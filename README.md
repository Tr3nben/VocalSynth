# VocalSynth

**Mit der Stimme Synths spielen, Beats einsingen und MIDI oder Samples erzeugen, direkt als Plugin in FL Studio.**

VocalSynth ist ein geplantes Open-Source-Plugin (VST3 + CLAP) für FL Studio. Hauptplattform ist Windows, macOS folgt später. Das Plugin macht aus einem Mikrofonsignal in Echtzeit MIDI: gesungene Töne werden zu Noten, Beatbox-Laute zu Drum-Triggern, Lautstärke und Vokale zu Controller-Daten. Vom Funktionsumfang her orientiert es sich an Vochlea Dubler 2.

> **Status: Meilenstein 0 – Grundgerüst.**
> Es gibt ein baubares, validiertes Plugin-Gerüst (Audio wird durchgereicht, Eingangspegel wird angezeigt), aber noch keine Analyse und kein MIDI. Grundlage sind die technische Recherche ([docs/RESEARCH.md](docs/RESEARCH.md)) und der Plan ([docs/PLAN.md](docs/PLAN.md)). Die Features unten beschreiben das Ziel, nicht den aktuellen Stand.

---

## Features (geplant)

### Pitch-to-MIDI
- Monophones Pitch-Tracking in Echtzeit (YIN/MPM) mit Range-Presets (Bass, Tenor, Sopran, Pfeifen)
- **Scale-Lock**: Quantisierung auf Tonart und Skala, optional nur am Notenanfang einrasten und danach benden
- **Stickiness-Regler** (Noten-Hysterese) mit Anzeige der dadurch entstehenden Latenz in ms
- **Akkord-Modus**: Die gesungene Grundnote löst einen diatonischen oder festen Akkord aus, mit Voicings und Inversionen
- **Pitchbend** in zwei Modi: „Anchored“ (Totzone, erst bewusstes Wegbenden zählt) und „True“ (1:1), Bend-Range einstellbar
- Legato oder Retrigger bei Notenwechseln, Velocity aus der Anschlagsenergie

### Beatbox-Trigger
- Trainierbare Sounds pro Profil: jeden Laut ein paar Mal vorsingen, fertig
- Onset-Erkennung (HFC + Spectral Flux) und nutzerspezifische Klassifikation (MFCC-Features + kNN)
- „Unbekannt“-Klasse gegen Fehlauslösungen durch Störgeräusche
- Modi „Fast“ und „Accurate“ (Latenz gegen Treffsicherheit), Velocity pro Sound kalibriert
- Live-Konfidenzanzeige und Konfusionsmatrix im Training

### Expression / CC
- Hüllkurve (Lautstärke) → CC oder Velocity
- **Vokal-/Formant-Erkennung** → CC, z. B. „Offenheit“ und „Helligkeit“ oder Morph-Gewichte zwischen Vokalen, mit Kalibrierung pro Nutzer
- Geräuschanteil und Pitch-Abweichung als weitere CC-Quellen

### Capture & Samples
- Eingebauter Recorder für MIDI und Audio, da FL Studio Plugin-MIDI nicht direkt in die Piano Roll aufnehmen kann
- **Drag & Drop** von `.mid` und `.wav` aus dem Plugin in Piano Roll, Playlist oder Channel Rack, zusätzlich „Speichern unter“
- **Clean MIDI**: Nachanalyse eines Takes (pYIN, später SwiftF0) entfernt Ghost-Notes, und die Detektionslatenz wird herausgerechnet
- Sample-Erzeugung: Aufnahme wird automatisch an den Onsets geschnitten, normalisiert und mit Fades versehen. Ergebnis ist ein Drum-Kit aus den eigenen Beatbox-Sounds als WAV-Ordner

### Interne Klangerzeugung
- Einfacher Sampler (für Beatbox → Drum-Samples) und Synth im Plugin selbst
- Funktioniert ohne jedes MIDI-Routing und dient als garantierter Fallback

---

## Wie es in FL Studio funktionieren soll

1. **Mikrofon auf einen Mixer-Insert** legen und VocalSynth dort als Effekt laden.
2. Im Plugin-Wrapper (Zahnrad → „VST wrapper settings“) einen **MIDI Output Port** setzen, z. B. 5.
3. Beim Ziel-Synth im **Channel Rack** denselben Port als **MIDI Input Port** einstellen. Native FL-Synths wie Sytrus oder FLEX haben keinen Input-Port, sie erreicht man über **Patcher**.
4. Singen, beatboxen, spielen. Gelungene Takes über den Capture-Bereich per Drag & Drop ins Projekt ziehen.

> **Ehrlicher Hinweis:** Ob VST3-MIDI-Out aus einem Mixer-Effekt in FL Studio 2026 zuverlässig funktioniert, ist **noch nicht verifiziert**. Es gibt widersprüchliche und teils negative Forenberichte, z. B. zu Scaler und einem JUCE-Arpeggiator. Deshalb ist Meilenstein 1 ein Pflicht-Spike (Go/No-go-Gate, siehe [docs/PLAN.md](docs/PLAN.md)) mit den Tests T1–T7 (siehe [docs/RESEARCH.md, Abschnitt 2.5](docs/RESEARCH.md#25-folgerungen-und-pflicht-spike-vor-der-architektur)).

**Fallbacks, falls das Port-Routing nicht klappt:**
- Variante als **Instrument** im Channel Rack (Mikrofon per Sidechain) statt als Effekt
- Komplettes Setup in **Patcher** auf dem Mikrofon-Insert
- **CLAP-Build** (CLAP wird seit FL Studio 2024 gehostet; ob Note-Out in die Ports gelangt, ist ebenfalls offen)
- **Interne Klangerzeugung**, die ganz ohne Routing auskommt
- **Virtueller MIDI-Port** aus dem Plugin (unter Windows über Windows MIDI Services) → normaler FL-MIDI-Input → direkte Aufnahme in die Piano Roll
- **Capture + Drag & Drop**

---

## Abgrenzung zu Dubler 2

Dubler 2 hat den Workflow „Stimme → MIDI“ etabliert. VocalSynth setzt bei einigen Punkten bewusst anders an:

| | Dubler 2 | VocalSynth (geplant) |
|---|---|---|
| Architektur | Standalone-App mit virtuellem MIDI-Treiber, MIDI Capture als Begleit-Plugin | **Ein Plugin** (VST3/CLAP) direkt im Mixer-Insert, keine separate App, kein Treiber |
| Latenz/Stabilität | „Stickiness“-Regler | Hysterese-Regler **mit ms-Anzeige**, Range-Presets |
| Aufgenommenes MIDI | Ansichten „Raw“ und „Clean“ | Offline-Nachanalyse des mitgeschnittenen Audios für Clean MIDI |
| Beatbox-Sounds | Trigger auf MIDI | Trigger auf MIDI **plus** Export der eigenen Laute als Sample-Kit |
| Klangerzeugung | über DAW-Instrumente | zusätzlich eingebauter Sampler/Synth |
| Lizenz | kommerziell | Open Source (Lizenz noch offen, siehe unten) |

*Dubler 2 ist eine Marke von Vochlea. VocalSynth ist ein unabhängiges Projekt und steht in keiner Verbindung zu Vochlea.*

---

## Geplanter Tech-Stack

- **JUCE 9** (aktuell 9.0.4) als Plugin-Framework
- **C++23**
- **CMake** (eigenes schlankes Setup nach dem Vorbild von [pamplejuce](https://github.com/sudara/pamplejuce)), JUCE und clap-juce-extensions als gepinnte Submodules
- **[clap-juce-extensions](https://github.com/free-audio/clap-juce-extensions)** für den CLAP-Build, da JUCE 9 kein natives CLAP kann
- **Catch2** für Unit-Tests und Benchmarks mit Referenzaufnahmen
- **pluginval** (Strictness 10) und der Steinberg-`validator` für die Plugin-Validierung
- **GitHub Actions** für Builds unter Windows, macOS und Linux
- Später optional: **RTNeural** (Trigger-Embedding) und **ONNX Runtime** im Hintergrund-Thread (SwiftF0 für die Capture-Nachanalyse)

Formate: VST3 (primär), CLAP, später AU, dazu ein Standalone-Target zum Debuggen.

---

## Projektstruktur

```
VocalSynth/
├── CMakeLists.txt             # Wurzel: C++23, JUCE, CLAP, core, Plugins, Tests
├── cmake/VocalSynthPlugin.cmake # Zwei Produkte aus einem Code: FX und Instrument
├── JUCE/                      # Submodule, gepinnt auf 9.0.4
├── modules/clap-juce-extensions/ # Submodule (CLAP-Format), gepinnt auf einen JUCE-9-fähigen Commit
├── core/                      # VocalSynthCore: DSP in reinem C++, ohne JUCE (headless testbar)
├── plugin/                    # Processor, Editor, Parameter, createPluginFilter
├── tests/                     # Catch2: core/ (DSP) und plugin/ (Processor ohne Host)
├── docs/                      # RESEARCH.md, PLAN.md
└── .github/workflows/         # Build, Tests, pluginval, VST3- und CLAP-Validator
```

Geplant sind außerdem `tools/` (Auswertung, Testmuster für den FL-Routing-Test) und `resources/` (Samples, Presets), siehe [docs/PLAN.md](docs/PLAN.md#24-verzeichnisstruktur-geplant).

---

## Einfach installieren (Windows)

1. Im [Actions-Tab](https://github.com/Tr3nben/VocalSynth/actions) den neuesten grünen Lauf öffnen und unten bei „Artifacts“ die Datei **`VocalSynth-<Version>-Windows-<Commit>.zip`** herunterladen (dafür bei GitHub angemeldet sein).
2. ZIP entpacken und **`Install-VocalSynth.bat`** doppelklicken. Das Skript kopiert beide Plugins in je einen eigenen Ordner `VocalSynth`:
   - `C:\Program Files\Common Files\VST3\VocalSynth\` (oder in deinen eigenen VST3-Ordner, das Skript fragt danach)
   - `C:\Program Files\Common Files\CLAP\VocalSynth\`
3. In FL Studio: *Options → Manage plugins → Find installed plugins*.

Details stehen in der `LIESMICH.txt` im Paket. Die Plugins sind noch nicht signiert; meldet Windows „Der Computer wurde durch Windows geschützt“, über „Weitere Informationen → Trotzdem ausführen“ starten.

---

## Build

Aus einem Code entstehen zwei Plugins:

| Produkt | Typ | Formate |
|---|---|---|
| **VocalSynth FX** | Effekt für den Mixer-Insert (Mikro rein, MIDI raus) | VST3, CLAP, Standalone, AU (macOS) |
| **VocalSynth Instrument** | Instrument mit Sidechain-Eingang | VST3, CLAP, AU (macOS) |

Welches davon in FL Studio der Hauptweg wird, entscheidet der Routing-Test in Meilenstein 1.

**Voraussetzungen**
- CMake ≥ 3.25 und Ninja (oder ein anderer CMake-Generator)
- C++23-Compiler: Visual Studio 2022 oder neuer (Windows), Xcode/Clang (macOS), GCC ≥ 13 oder Clang ≥ 17 (Linux)
- Git mit Submodules
- Linux zusätzlich: die JUCE-Abhängigkeiten, siehe [JUCE/docs/Linux Dependencies.md](JUCE/docs/Linux%20Dependencies.md) bzw. der Schritt „Install Linux dependencies“ in [.github/workflows/build_and_test.yml](.github/workflows/build_and_test.yml)

**Bauen und testen**

```bash
git clone --recurse-submodules https://github.com/Tr3nben/VocalSynth.git
cd VocalSynth
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target VocalSynthAll VocalSynthCoreTests VocalSynthPluginTests
ctest --test-dir build --output-on-failure
```

Unter Windows die Befehle in der „Developer PowerShell for VS“ ausführen (damit der MSVC-Compiler gefunden wird). Ohne Ninja geht auch `cmake -B build` mit dem Visual-Studio-Generator; dann `--config Release` an Build und `ctest` anhängen.

Die fertigen Plugins liegen danach unter `build/VocalSynthFX_artefacts/Release/` bzw. `build/VocalSynthInstrument_artefacts/Release/` (Unterordner `VST3`, `CLAP`, `Standalone`). Zum Testen in FL Studio die `.vst3`-Ordner nach `C:\Program Files\Common Files\VST3` kopieren und im Plugin-Manager „Find installed plugins“ ausführen.

**Validierung** (läuft auch in der CI auf Windows, macOS und Linux)
- [pluginval](https://github.com/Tracktion/pluginval) auf Strenge 10 für beide VST3s
- Steinberg-`validator` aus dem VST3-SDK
- [clap-validator](https://github.com/free-audio/clap-validator) für beide CLAPs
- `auval` für die AU-Versionen (macOS)

Fertige Builds jedes Commits gibt es als Artefakte im [Actions-Tab](https://github.com/Tr3nben/VocalSynth/actions). Sie sind noch **unsigniert**; Windows zeigt deshalb ggf. eine SmartScreen-Warnung.

Zum Testen braucht es außerdem FL Studio 2026 (unter Windows mit ASIO-Treiber), ein Mikrofon (laut Erfahrungen mit Dubler 2 bevorzugt ein dynamisches) und Kopfhörer gegen Übersprechen.

---

## Entwicklung mit Claude Code

Im Ordner [`.claude/skills/`](.claude/skills) liegen Skills für die Entwicklung mit Claude Code:

| Skill | Zweck |
|---|---|
| `juce-audio-plugin-dev` | Projektregeln für VocalSynth: AudioProcessor, DSP, APVTS, MIDI-Out, Build-Setup, Tests, FL-Studio-Routing |
| `audio-dsp-review` | Review von Audio-/DSP-Code auf Echtzeit-Sicherheit (Allokationen, Locks usw. im Audio-Thread) |
| `juce-review` | Review auf JUCE-spezifische Fehler: Thread-Safety, APVTS, MessageManager, ValueTree, MIDI |
| `validate-pluginval` | VST3/AU-Plugins mit pluginval in wählbarer Strictness validieren (kein CLAP) |

---

## Dokumentation

- [docs/RESEARCH.md](docs/RESEARCH.md): Marktanalyse, MIDI-Routing in FL Studio, DSP-/ML-Verfahren, Tech-Stack, Risiken
- [docs/PLAN.md](docs/PLAN.md): Roadmap und Meilensteine

---

## Lizenz

- **JUCE:** wird unter der **kostenlosen JUCE-Lizenz (Starter)** genutzt, nicht unter AGPLv3. Laut [docs/RESEARCH.md](docs/RESEARCH.md) gilt sie bis 20.000 USD Jahresumsatz; maßgeblich ist die [JUCE-9-EULA](https://juce.com/legal/juce-9-licence/). Die Lizenz ist an den JUCE-Account gebunden, am Code und am Build ändert sich dadurch nichts. Wer VocalSynth selbst aus dem Quellcode baut und verteilt, braucht dafür eine eigene JUCE-Lizenz (oder nutzt JUCE unter AGPLv3).
- **Eigener Code von VocalSynth:** Lizenz noch nicht festgelegt. Durch die JUCE-Lizenz ist das Projekt nicht mehr an AGPLv3 gebunden.
