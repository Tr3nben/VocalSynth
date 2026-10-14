# VocalSynth

**Mit der Stimme Synths spielen, Beats einsingen und MIDI oder Samples erzeugen, direkt als Plugin in FL Studio.**

VocalSynth ist ein geplantes Open-Source-Plugin (VST3 + CLAP) für FL Studio. Hauptplattform ist Windows, macOS folgt später. Das Plugin macht aus einem Mikrofonsignal in Echtzeit MIDI: gesungene Töne werden zu Noten, Beatbox-Laute zu Drum-Triggern, Lautstärke und Vokale zu Controller-Daten. Vom Funktionsumfang her orientiert es sich an Vochlea Dubler 2.

> **Status: Planungsphase – noch kein Code.**
> Bisher gibt es nur die technische Recherche ([docs/RESEARCH.md](docs/RESEARCH.md)) und den Plan ([docs/PLAN.md](docs/PLAN.md)). Alles unten beschreibt das Ziel, nicht den aktuellen Stand.

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

> **Ehrlicher Hinweis:** Ob VST3-MIDI-Out aus einem Mixer-Effekt in FL Studio 2026 zuverlässig funktioniert, ist **noch nicht verifiziert**. Es gibt widersprüchliche und teils negative Forenberichte, z. B. zu Scaler und einem JUCE-Arpeggiator. Deshalb steht in Meilenstein 0 ein Pflicht-Spike mit den Tests T1–T7 (siehe [docs/RESEARCH.md, Abschnitt 2.5](docs/RESEARCH.md#25-folgerungen-und-pflicht-spike-vor-der-architektur)).

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
- **CMake** auf Basis des Templates **[pamplejuce](https://github.com/sudara/pamplejuce)** (JUCE als Submodule, CPM)
- **[clap-juce-extensions](https://github.com/free-audio/clap-juce-extensions)** für den CLAP-Build, da JUCE 9 kein natives CLAP kann
- **Catch2** für Unit-Tests und Benchmarks mit Referenzaufnahmen
- **pluginval** (Strictness 10) und der Steinberg-`validator` für die Plugin-Validierung
- **GitHub Actions** für Builds unter Windows, macOS und Linux
- Später optional: **RTNeural** (Trigger-Embedding) und **ONNX Runtime** im Hintergrund-Thread (SwiftF0 für die Capture-Nachanalyse)

Formate: VST3 (primär), CLAP, später AU, dazu ein Standalone-Target zum Debuggen.

---

## Projektstruktur (geplant)

Die tatsächliche Struktur entsteht in Meilenstein 0 aus dem pamplejuce-Template und kann davon abweichen.

```
VocalSynth/
├── CMakeLists.txt
├── JUCE/                    # Submodule (JUCE 9)
├── modules/                 # clap-juce-extensions u. a.
├── source/
│   ├── PluginProcessor.*    # Audio-Thread, MIDI-Out
│   ├── PluginEditor.*       # UI (Tuner, Trigger-Pads, Vokal-XY, Capture)
│   ├── dsp/                 # Pitch (YIN/MPM), Onset, LPC/MFCC, kNN
│   ├── midi/                # Gate, Scale-Lock, Akkorde, Bend, CC
│   ├── capture/             # Recorder, Clean-Nachanalyse, Drag & Drop
│   └── engine/              # interner Sampler/Synth
├── tests/                   # Catch2-Tests + Audio-Fixtures
├── docs/
│   ├── RESEARCH.md
│   └── PLAN.md
└── .github/workflows/       # Build, Tests, pluginval
```

---

## Build

*Folgt mit Meilenstein 0.*

Voraussichtliche Voraussetzungen:
- CMake ≥ 3.25
- C++23-fähiger Compiler (unter Windows Visual Studio)
- Git (mit Submodules)
- FL Studio 2026 zum Testen, unter Windows mit ASIO-Treiber
- pluginval zur Validierung
- Ein Mikrofon, laut Erfahrungen mit Dubler 2 bevorzugt ein dynamisches, und Kopfhörer gegen Übersprechen

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

**Noch nicht festgelegt.**

Wichtig für die Entscheidung: JUCE ist dual lizenziert, unter **AGPLv3** oder der kommerziellen JUCE-EULA. Wer JUCE kostenlos nutzt, muss das gesamte Plugin unter AGPLv3 veröffentlichen, und wer Binaries verteilt, muss den Quellcode bereitstellen. Eine permissive Lizenz wie MIT ginge mit JUCE nur, wenn eine kommerzielle JUCE-Lizenz erworben wird (oder mit einem anderen Framework wie iPlug2 oder DPF).
