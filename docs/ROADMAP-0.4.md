# VocalSynth – Roadmap 0.4 bis 0.7: eigener Klang, neues UI, Stimm-Expression

Stand: 2026-10-27, Ausgangspunkt v0.3.1 (Branch `m3.1-takes`). Grundlage: `docs/PLAN.md` (Architektur Abschnitt 2,
Meilensteine M4–M8, Teststrategie Abschnitt 4), `docs/STATUS.md`, `docs/M2-DESIGN.md`, `docs/M3-DESIGN.md` und
der aktuelle Code in `core/` und `plugin/`. Dieses Dokument ändert die **Reihenfolge** der Meilensteine aus
PLAN.md (M6 und Teile von M7/M5 kommen vor M4) und legt die technischen Leitplanken fest. Die Details stehen
später in je einem Design-Dokument pro Release (wie bisher `M2-DESIGN.md`, `M3-DESIGN.md`).

Nutzerwunsch: „Einen größeren Teil mit UI, Synths (eigener Wavetable) usw. durcharbeiten und fertig machen.“
Ziel des Blocks ist also ein **durchgehend benutzbares Instrument**: Plugin auf den Mikro-Insert, singen, einen
guten eigenen Synth hören – ohne MIDI-Routing, mit einem UI, das man ohne Anleitung versteht.

---

## 0. Kurzfassung

| Release | Thema | Inhalt (Kern) | PLAN-Bezug | Aufwand (grob) |
|---|---|---|---|---|
| **0.4.0** | Eigener Klang | Wavetable-Synth-Engine (2 WT-Osc + Sub + Noise, SVF-Filter, 3 Hüllkurven, 2 LFOs, 8 Mod-Slots, Mono/Legato/Poly 16, Glide, Chorus/Delay/Reverb), Wavetable-Import, 24 Factory-Presets, Stimm-Quellen *Level/Brightness/Noisiness/Pitch* als Modulation, Seite „Synth“ auf neuem Design-Fundament | M6 (Synth-Teil), Anfang M7, Anfang M5 | groß: ca. 10–14k Zeilen inkl. Tests, 8–12 Arbeitstage bis ZIP |
| 0.4.1 | Klang-Feinschliff | Presets und Defaults nach dem Hörtest des Nutzers, Fehler | – | klein: 1–3 Tage |
| **0.5.0** | Neues UI | Komplettes UI: Theme/LookAndFeel, skalierbar + HiDPI, Seiten Play / Synth / Capture / Setup, Wavetable-, Hüllkurven-, LFO-, Filter-Ansichten, Mod-Matrix, Preset-Browser, Ersteinrichtung mit Routing-Hilfe, Pitch-Kurve; **Vokal (F1/F2) als Mod-Quelle** mit Vokal-XY | M7, M5 (Vokal-Tracker) | groß: ca. 8–10k Zeilen, 8–12 Tage |
| **0.6.0** | Expression & Akkorde | Vokal-Kalibrierung, Expression → CC für externe Synths, Akkord-Modus (MIDI + interner Poly-Synth + Capture) | Rest M5 | mittel: ca. 4–6k Zeilen, 5–7 Tage |
| 0.7.0 | Beatbox (Ausblick) | M4: Onset, Features, kNN-Training, Drum-Sampler mit im Code erzeugtem Kit, Pads/Training-UI | M4, M6 (Sampler) | groß: 10–15 Tage + Korpus |

„Arbeitstage“ heißt: mit der bisherigen Agenten-Arbeitsweise (Design → parallele Implementierung → Integration →
Review → Fixes → CI), ohne die Wartezeit auf den Nutzertest. Die Hobby-Schätzungen aus PLAN.md gelten dafür
nicht mehr.

**Kernentscheidungen (Vorschlag, Details unten):** natives JUCE-UI statt WebView; Mip-Map-Wavetables (pro Oktave
bandbegrenzt) statt Oversampling; alle Wavetables und Presets im Code erzeugt (keine Fremd-Assets); Synth-Parameter
als normale, automatisierbare APVTS-Parameter mit Version-Hint 4; Wavetable-Tausch per atomarem Zeiger mit
verzögerter Freigabe; die Stimm-Expression läuft im Audio-Thread im Hop-Takt der VoicePipeline.

---

## 1. Ausgangslage im Code (relevant für diesen Block)

- **Klang heute:** `core/MonitorSynth` ist ein Sinus mit linearer Hüllkurve (Monitor-Ton für Routing-Test T6),
  Schalter `monitorTone` (Seite „Test & Routing“). Der Prozessor ruft ihn pro Chunk mit den `VsEvent`s der Quelle
  auf (`PluginProcessor.cpp`, `processBlock`), Bends werden auf ±2 Halbtöne umgerechnet
  (`prepareMonitorEvents`).
- **FX:** Stimme läuft durch, außer `muteDry`. **Instrument:** Ausgang ist nur der Monitor-Ton.
- **Events:** `VsEvent` (noteOn/Off, pitchBend, CC) mit Sample-Offset, `VsEventBuffer<1024>` pro Chunk; die
  Capture zeichnet genau diese Events auf (`CaptureTap`, 32-Byte-Records). → Das Event-Format bleibt unverändert;
  Expression kommt über einen **eigenen** Puffer (Abschnitt 3.5).
- **UI:** `VocalSynthEditor` (940 × 470, feste Größe), drei Seiten Voice / Test & Routing / Capture, Standard-
  LookAndFeel, `TunerDisplay`, `CapturePanel` (eigene Klasse). 23 Editor-Tests in `tests/plugin/PluginEditorTests.cpp`
  (u. a. „Every M2 parameter has a control on the Voice page“, Label-Abgleich mit `FL-ROUTING.md`/`LIESMICH.txt`).
- **Parameter:** 30 Stück, IDs dauerhaft, Version-Hints 1 (M1/M2), 2 (M3), 3 (M3.1). `migrateState()` ergänzt
  fehlende Parameter mit Defaults und hat bereits eine Sonderregel (alte Projekte mit Testmuster → `source`).
  Nicht-Parameter-State als Kind `CAPTURE` (mit `version`).
- **Build:** `core/` ist JUCE-frei (keine FFT im Core, PitchDetector arbeitet im Zeitbereich);
  `JUCE_WEB_BROWSER=0`; Catch2 v3; CI Linux + Windows pro Push, macOS manuell.

---

## 2. Reihenfolge und Begründung

1. **0.4.0 Synth zuerst.**
   - Größter spürbarer Gewinn: Singen → sofort ein guter Klang auf dem Mikro-Insert, ohne Port-Routing (Weg T6
     aus RESEARCH 2.5 wird vom Notbehelf zum Hauptweg für Einsteiger).
   - Fast alles ist `core/`-DSP: headless testbar, gut parallelisierbar, unabhängig von der offenen
     Segmentierungs-/Korpus-Arbeit.
   - Die Engine legt fest, **welche** Regler, Ansichten und Presets das UI braucht. Ein UI vorher müsste man für
     den Synth noch einmal umbauen.
   - Damit die Synth-Seite nicht zweimal gebaut wird, kommt das **Design-Fundament** (Theme, LookAndFeel,
     Drehregler, Sektionsrahmen, skalierbare Editor-Hülle) schon in 0.4.0. 0.5.0 baut darauf weiter.
2. **0.5.0 UI danach.** Jetzt ist bekannt, was gezeigt werden muss; die Seiten Play/Setup/Capture ziehen auf das
   neue Design um, und der Preset-Browser bekommt echte Inhalte. Der Vokal-Tracker gehört hierher, weil er ohne
   Vokal-XY-Anzeige kaum einzustellen ist – und weil „Vokal → Wavetable-Position“ das Alleinstellungsmerkmal ist,
   das man im neuen UI zeigen will.
3. **0.6.0 Expression → CC und Akkorde.** Baut auf den Mod-Quellen (0.4/0.5) und dem UI (Kalibrier-Assistent)
   auf. Akkorde brauchen den Poly-Modus des Synths.
4. **0.7.0 Beatbox (M4) zuletzt in diesem Block.** Größtes Genauigkeitsrisiko (Ziele ≥ 90 % Accuracy) und
   korpusabhängig. Profitiert von RealFft (0.4), Asset-Tausch per atomarem Zeiger (0.4, gleiches Muster für
   Samples), UI-Fundament und Seitenstruktur (0.5). Wer Beatbox früher will: Entscheidung E1.

**Was ausdrücklich nicht in diesen Block gehört:** ML-Stufe, MPE-Ausgabe, macOS-Release, Installer/Signierung
(M8), zweiter Filter, Wavetable-Editor/-Zeichnen, Warp-Modi (FM/Sync/Bend pro Oszillator), Oversampling-HQ-Modus,
Arpeggiator. Kandidaten für 0.8+.

---

## 3. Gemeinsame Architektur

### 3.1 Signalfluss nach 0.4.0 (Audio-Thread, pro Chunk ≤ 4096 Samples)

```
 Mic ─► downmix ─► VoicePipeline.process (mono, n, voiceSettings, events, timings, expression)   (0.4: neue Überladung)
                    │ events (VsEvent)          │ expression (ExpressionFrame im Hop-Takt, eigener Puffer)
 Test Pattern ──────┤ (statt Pipeline)          │ (Ruhewerte)
                    ▼                           ▼
              MidiOutputStage ─► MIDI-Out     SynthEngine.process (events, expression, synthSettings, out L/R, n)
              CaptureTap (unverändert)          │ VoiceAllocator ─► 16 SynthVoice (2 WT-Osc+Unison, Sub, Noise,
              MonitorSynth (Test-Ton, bleibt)   │                    SVF, Env1–3, LFO1–2, ModMatrix)
                                                └► Chorus ─► Delay ─► Reverb ─► Synth-Volume
 Ausgang FX:          dry × dryLevel (0, wenn Mute Dry)  +  Synth (wenn synthOn)  [+ Test-Ton]
 Ausgang Instrument:  Synth (wenn synthOn)  [+ Test-Ton]

 [Message-Thread]   UI, Presets laden (setzt Parameter), Import-Dialog
 [WavetableWorker]  WAV lesen → Mip-Maps bauen → WavetableStore.publish() ─► atomic Slot A/B ─► Audio-Thread
                    alte Tabellen ─► Retire-Liste ─► Freigabe, sobald der Audio-Thread den Tausch quittiert hat
```

Der `MonitorSynth` bleibt als **Test-Ton** (Parameter `monitorTone`, Seite Setup) für Routing-Diagnose und
seine Tests; der neue Synth ersetzt ihn als Klangquelle zum Spielen. Gemeldete Latenz bleibt 0: der Synth
spielt im selben Block, in dem das Event entsteht.

### 3.2 Synth-Engine (0.4.0)

**Oszillatoren**
- **2 Wavetable-Oszillatoren A/B:** Tabelle (Factory oder Import), Position 0–100 % (kontinuierlich, lineare
  Interpolation zwischen Nachbar-Frames), Oktave ±3, Halbton ±12, Fein ±100 ct, Level, Pan.
- **Unison** 1–8 Stimmen pro Oszillator: Detune 0–100 ct (symmetrisch verteilt), Stereo-Spread, Start-Phase
  zufällig 0–100 %. Ein gemeinsamer Satz von Phasen in Structure-of-Arrays, damit der Compiler über die
  Unison-Stimmen vektorisiert (kein SIMD-Fremdcode nötig).
- **Sub:** Sinus/Dreieck/Rechteck (bandbegrenzt, aus eigener kleiner Tabelle), −1/−2 Oktaven, Level.
- **Noise:** weiß → rosa (Farbregler, Tilt-Filter), Level; deterministischer PRNG (xorshift) pro Instanz, in
  `prepare` gesät → Tests reproduzierbar.

**Wavetables und Anti-Aliasing**
- Frame-Größe intern **2048 Samples**, bis **256 Frames** pro Tabelle.
- **Mip-Map pro Oktave** (11 Stufen): Stufe ℓ enthält nur Harmonische ≤ 1024 / 2^ℓ (per FFT → Abschneiden →
  IFFT, also exakt bandbegrenzt). Länge pro Stufe max (256, 4 × Harmonische), höchstens 2048 → ca. 7,4k Floats
  pro Frame, **≤ 7,6 MB für 256 Frames**, ca. 1–2 MB für typische Factory-Tabellen (32–64 Frames).
- Stufenwahl pro Steuer-Tick (16 Samples) aus dem größten Phaseninkrement der Stimme (inkl. Unison-Detune,
  Bend, Glide, Pitch-Modulation), so dass die höchste Harmonische unter ca. 0,45 × fs bleibt; Hysterese gegen
  Flattern. Falls der Klick-Test (Abschnitt 4.1 Tests) Sprünge zeigt: Überblendung über einen Tick.
- Interpolation: linear innerhalb des Frames (durch die 2–4-fache Überabtastung jeder Stufe ausreichend),
  linear zwischen Frames.
- **Kein Oversampling** für die Oszillatoren. Nichtlinearitäten (Filter-Drive) sind weich und begrenzt; ADAA bzw.
  2×-Oversampling für Drive sind ein späterer HQ-Modus.
- `RealFft` (eigene radix-2-FFT im Core, ca. 150 Zeilen, nur Hintergrund-Threads in 0.4; 0.4/0.5 auch für
  Brightness/Formanten, 0.7 für MFCC).

**Factory-Wavetables (im Code erzeugt, keine Assets, ca. 14):** Basic Shapes (Sinus → Dreieck → Säge → Rechteck
→ Puls), PWM, Harmonic Sweep (ungerade/gerade Obertöne), Saw Bright→Dark, **Formant/Vokal** (additiv mit
Formant-Hüllkurve A–E–I–O–U, passt zu Vokal → Position), Sync Sweep (Hard-Sync-Verhältnis 1 → 8, bandbegrenzt
über die FFT), FM 2-Op (Index-Sweep), Fold (Wavefolder-Sweep), Organ (Zugriegel-Kombinationen), Digital
(Quantisierung), Spectral Random (gesät), Bell-ish (spektral), Square Stack, Choir (Formant-Mischung).
Erzeugung **bei Bedarf** in einem Hintergrund-Thread, prozessweit geteilt (unveränderlich, Referenzzählung nur
außerhalb des Audio-Threads); bis eine Tabelle fertig ist, spielt der Oszillator einen in `prepare` erzeugten
Sinus.

**Wavetable-Import**
- WAV (PCM 16/24/32 Bit, Float 32), mono oder stereo (→ Mitte).
- Frame-Größe: aus dem `clm `-Chunk (Konvention von Serum und anderen: Text „<!>2048 …“), sonst 2048, wenn die
  Länge ein Vielfaches ist; sonst **Single-Cycle** (ganze Datei = ein Zyklus, per FFT auf 2048 umgerechnet).
  Im Import-Dialog überschreibbar (256/512/1024/2048/4096).
- > 256 Frames: gleichmäßig ausgedünnt; DC entfernt, auf Spitze normalisiert, Frames phasenstabil belassen.
- Eigener `WavReader` im Core (Gegenstück zum vorhandenen `WavWriter`), damit Chunks, kaputte Dateien und
  Grenzfälle headless und per Fuzz-Test geprüft werden. Lesen + Mip-Map-Bau im `WavetableWorker`, nie im
  Audio-Thread.
- **Im Projekt eingebettet**: Importierte Tabellen werden als 16-Bit-PCM (Base64) im State-Kind `SYNTH`
  gespeichert (≤ 1 MB pro 256-Frame-Tabelle), zusätzlich Dateiname + Hash. Projekte funktionieren dann auch auf
  einem anderen Rechner. Gleiche Tabellen (Hash) werden zwischen Instanzen geteilt.

**Filter:** Zustandsvariables Filter (TPT/„Zero-Delay-Feedback“-SVF, öffentlich beschriebenes Verfahren, eigene
Implementierung), Modi **LP12, LP24 (zwei Stufen), BP, HP, Notch**; Cutoff 20 Hz–20 kHz (logarithmisch),
Resonanz bis kurz vor Selbstoszillation, **Drive** 0–24 dB (weiche Sättigung vor dem Filter, begrenzt), Keytrack,
Hüllkurven-Anteil (Env 2). Pro Stimme stereo (wegen Unison-Spread). Koeffizienten pro Steuer-Tick, Cutoff
geglättet; stabil bei schneller Modulation (Test).

**Hüllkurven:** 3 × ADSR – Env 1 = Amp, Env 2 = Filter (auch frei in der Matrix), Env 3 = Mod. Attack/Decay/
Release 0–10 s (logarithmischer Regler), exponentielle Segmente, Velocity → Amp (Regler). Legato: kein
Neustart der Hüllkurven.

**LFOs:** 2 × (Sinus, Dreieck, Säge auf/ab, Rechteck, S&H, geglättetes Zufallssignal), 0,01–30 Hz oder
tempo-synchron (Host-BPM aus der Playhead, wie die Capture sie liest), Retrigger frei/pro Note, Fade-In.
Global (nicht pro Stimme) – reicht für Mono und hält Poly günstig.

**Mod-Matrix:** **8 Slots** (Quelle, Ziel, Betrag ±100 %).
- Quellen (Liste nur hinten erweitern): Off, Env 2, Env 3, LFO 1, LFO 2, Velocity, Note (Keytrack), Random pro
  Note, **Voice Level, Voice Brightness, Voice Noisiness, Voice Pitch** (Abweichung vom Notenzentrum, ±100 ct);
  0.5 hängt **Vowel X (F2)** und **Vowel Y (F1)** an, 0.6 die kalibrierten Vokal-Gewichte.
- Ziele (Liste nur hinten erweitern): Off, Osc A Position/Pitch/Level/Detune, Osc B Position/Pitch/Level/Detune,
  Sub Level, Noise Level, Filter Cutoff (±5 Oktaven)/Resonance/Drive, Amp Level, Pan, LFO 1 Rate, LFO 2 Rate,
  Chorus/Delay/Reverb Mix.
- Modulation wird intern addiert, **nie in Parameter zurückgeschrieben**.

**Stimmen, Glide, Spielmodus**
- `playMode`: **Mono**, **Legato** (Mono ohne Hüllkurven-Neustart bei überlappenden Noten; Standard, weil die
  Stimme einstimmig ist), **Poly** (16 Stimmen, Stehlen: älteste in Release zuerst, dann älteste; 2-ms-Fade).
- Glide 0–2000 ms (konstante Zeit), Modus „immer“ oder „nur Legato“.
- **Korrektur kurz nach dem Onset:** Die VoicePipeline schickt bei einer vorläufigen Note (FastOnset, M2.1)
  manchmal ~10–30 ms später den richtigen Ton. Regel im Synth: ein Notenwechsel ≤ 40 ms nach dem Note-On wird ohne
  Glide und ohne Hüllkurven-Neustart übernommen (sonst hört man den Fehlstart als Glissando). Ob dafür die
  vorhandenen `VsEventTiming`-Infos reichen, klärt das Design.

**Effekte (global, nach der Stimmen-Summe):** Chorus (2 modulierte Verzögerungen, stereo), Delay (stereo/Ping-Pong,
1–2000 ms oder Tempo-Sync, Feedback mit Hoch-/Tiefpass), Reverb (eigenes 8-Kanal-FDN mit Householder-Matrix,
leicht moduliert, Größe/Dämpfung). Puffer in `prepare` für die größte Zeit bei 192 kHz. Alles eigener Code im
Core (headless testbar), kein `juce::dsp::Reverb`.

**Factory-Presets (24):** Init, Lead ×6, Bass ×5, Pad ×3 (Poly), Pluck/Keys ×3, Vocal/Formant ×4 (Vokal bzw.
Brightness → Position/Cutoff), FX ×2. Mindestens 8 nutzen Stimm-Quellen (z. B. Voice Level → Cutoff, Brightness →
Osc-Position, Noisiness → Noise Level, Voice Pitch → Vibrato-Tiefe). Als Daten im Code
(`FactoryPresets.cpp`), nicht als Dateien.

### 3.3 Echtzeit-Regeln für die Engine

- Alles in `prepare()` vorallokiert: 16 Stimmen × 2 Osc × 8 Unison, FX-Puffer, Steuer-Puffer. `process()` ohne
  Allokation, Locks, I/O (Allokations-Zähler-Test wie in `CaptureTapTests`).
- **Wavetable-Tausch:** `WavetableStore` hält pro Slot (A, B) einen `std::atomic<const Wavetable*>`. Der
  Audio-Thread lädt den Zeiger einmal pro Block, speichert ihn als „in Benutzung“ (`std::atomic`) und erhöht einen
  Block-Zähler. Der Worker legt alte Tabellen in eine Retire-Liste und gibt sie erst frei, wenn der Audio-Thread
  sie nicht mehr meldet und der Zähler weitergelaufen ist; ohne laufendes Audio (suspendiert) beim nächsten
  `prepareToPlay`/`releaseResources`. Kein `std::atomic<std::shared_ptr>`, keine Freigabe im Audio-Thread
  (PLAN 2.2). Ein Tabellenwechsel während einer Note: harter Wechsel am nächsten Steuer-Tick mit 5-ms-Überblendung
  (Test: kein Klick).
- **Steuer-Takt:** Modulation, Filterkoeffizienten, Mip-Stufe alle 16 Samples an **absoluten** Sample-Positionen
  (eigener Zähler seit `reset`, wie der Framer), dazwischen lineare Rampen. Dadurch ist das Ergebnis unabhängig
  von der Blockaufteilung: **bitgleich** für Blockgrößen {1, 7, 64, 480, 512, 4096} innerhalb eines Builds.
- Parameter: pro Block aus den Raw-Atomics in ein `SynthSettings` (wie `VoiceSettings`), stetige Werte geglättet
  (20 ms, Steuer-Takt). Choice-Wechsel (Filtermodus, Unison-Zahl) am Steuer-Tick, Filter-State bleibt.
- Denormals: `ScopedNoDenormals` plus explizites Nullsetzen ausgeklungener Stimmen und FX-Rückkopplungen
  (Ausgang nach dem Ausklingen exakt 0, Stimmen inaktiv → kein CPU-Anstieg in Stille).
- NaN-Schutz: Filter- und FDN-Zustand werden bei nicht-endlichen Werten zurückgesetzt (zählt ein Atomic hoch).

### 3.4 Parameter- und State-Design

- **IDs:** lowerCamelCase wie bisher, Gruppen-Präfixe (`synthOn`, `oscAPosition`, `filterCutoff`, `env2Attack`,
  `lfo1Rate`, `mod3Amount`, `delayFeedback`). **Version-Hint 4** für alle Parameter von 0.4.0, 5 für 0.5.0, 6 für
  0.6.0 (Begründung AU-Reihenfolge: `docs/M3-DESIGN.md` 13). Neue Parameter werden **hinten** an `createLayout()`
  angehängt; bestehende IDs, Bereiche und Choice-Listen ändern sich nie (Choice-Listen nur hinten erweitern).
- Synth-Parameter stehen in einer eigenen Datei `plugin/SynthParameters.h` (IDs, Listen, `addSynthParameters
  (layout)`); `Parameters.h` ruft sie am Ende von `createLayout()` auf.
- Alle stetigen Synth-Regler sind automatisierbar (FL-Automation-Clips). Gesamt ca. 100 neue Parameter (Liste in
  4.1). Werttexte wie bisher ohne Einheit (Label trägt sie), damit Text ↔ Wert auch bei CLAP stabil bleibt.
- **Nicht-Parameter-State:** neues Kind `SYNTH` (`version` = 1): Tabellen-Referenz pro Oszillator
  (`factory:<name>` oder `user:<hash>` + Name + eingebettete Daten), Name des geladenen Presets, „geändert“-Flag.
  Ab 0.5 zusätzlich `UI` (Skalierung, letzte Seite), ab 0.6 `VOWEL` (Kalibrierung). Laden toleriert fehlende,
  zusätzliche und kaputte Kinder (Defaults); Kinder werden wie `CAPTURE` vor `apvts.replaceState` herausgenommen.
- Wurzel-Eigenschaft `stateVersion` (= 4 ab 0.4.0) für künftige Migrationen; fehlt sie, stammt der State aus
  ≤ 0.3.x.
- **Kompatibilität:** `migrateState()` bekommt eine Sonderregel wie bei `source`: fehlt `synthOn` im State
  (alte Projekte), wird er **aus** gesetzt → ein 0.3.1-Projekt klingt nach dem Update genau wie vorher. Neue
  Instanzen bekommen den Default aus Entscheidung E4. Test mit eingecheckten State-Fixtures
  (`tests/data/state/0.3.1-fx.xml`, `…-instrument.xml`); ab jetzt pro Release ein Fixture.
- **Presets:** Datei `.vspreset` (XML: `version`, Name, Kategorie, Autor, Tags; Synth-Parameter **mit Klarwerten**
  pro ID, Tabellen-Referenzen/-Daten). Ein Preset setzt **nur** Synth-Parameter (Präfix-Liste) – nie Stimmlage,
  Gate, Skala, Capture. Fehlende IDs → Default, unbekannte ignoriert. Laden auf dem Message-Thread mit
  `setValueNotifyingHost` je Parameter (Gesten-Klammer), Tabellen über den Worker. Nutzer-Presets in
  `Dokumente\VocalSynth\Presets`.
- CLAP: nach Preset-/State-Laden wie bisher `updateHostDisplay (…withProgramChanged (true))`.

### 3.5 Wie die VoicePipeline den Synth speist

- **Noten:** unverändert die `VsEvent`s der Quelle (Voice oder Test Pattern), exakt dieselben wie am MIDI-Ausgang
  → Synth, MIDI-Out und Capture sind immer konsistent. Velocity aus der Onset-Energie (M2).
- **Pitchbend:** der Synth liest Bends mit dem eingestellten `bendRange` (wie heute `prepareMonitorEvents`, aber
  ohne Umrechnung) und glättet sie (einpolig, ca. 5 ms, Steuer-Takt), da sie mit 200–500 Hz kommen. Bend-Modus
  „True“ ergibt so das direkte Folgen der Stimme, „Anchored“ das saubere Halten.
- **Expression (neu):** `ExpressionFrame { offset, level, brightness, noisiness, pitchCents [, vowelX, vowelY] }`
  in einem eigenen `ExpressionBuffer` (feste Kapazität, ein Frame pro Analyse-Hop, Sample-Offset nach derselben
  Regel wie die Events → blockgrößen-invariant). Neue Überladung `VoicePipeline::process (…, ExpressionBuffer&)`;
  die alte Signatur und ihre Events bleiben bitgleich (bestehende Tests unverändert). Der Synth interpoliert die
  Werte linear im Steuer-Takt.
  - *Level:* RMS-dB (schon im `PitchFrame`) relativ zu Gate … −6 dBFS → 0…1, Attack 5 ms / Release 50 ms.
  - *Brightness:* spektraler Schwerpunkt über `RealFft` (512 Punkte auf dem dezimierten Signal, jeder 2. Hop),
    logarithmisch normiert.
  - *Noisiness:* spektrale Flachheit, kombiniert mit (1 − Pitch-Konfidenz) – Zischlaute, Hauchen.
  - *Pitch:* Abweichung der gesungenen Tonhöhe vom aktiven Notenzentrum in Cent (Vibrato, Intonation) – erlaubt
    z. B. Vibrato → Filter, auch wenn Bend aus ist.
  - *Vowel X/Y (0.5):* F2/F1 aus `FormantTracker` (Abschnitt 4.2).
- **Test Pattern:** Expression-Ruhewerte (Level 1 bei klingender Note, sonst 0; Rest 0), damit Presets hörbar
  bleiben.
- **Stimme im Mikro:** Expression läuft nur im Voice-Modus; im Synth-Seitenbereich zeigt eine Mini-Anzeige die
  vier Werte live (Atomics, 30 Hz).

### 3.6 CPU-Budget (48 kHz, Block 128, Release, Mittelklasse-Laptop)

| Fall | Ziel (Anteil an der Blockzeit) |
|---|---|
| Gesamt-Worst-Case `processBlock` (Pipeline + Synth + Capture) | **< 20 %** (SKILL.md), unverändert |
| Typisches Preset (Legato, 2 Osc × 4 Unison, Filter, Chorus+Delay+Reverb) | Mittel ≤ 3 %, Spitze ≤ 6 % |
| Schwerster Fall (Poly 16 × 2 Osc × 8 Unison = 256 Tabellenstimmen, alle FX) | ≤ 12 % |
| Stille (alle Stimmen aus, FX ausgeklungen) | ≤ 0,5 % |
| Expression-Tracker (0.4) / + FormantTracker (0.5) | ≤ 0,5 % / ≤ 1 % zusätzlich, über Hops verteilt |

Abschätzung: 256 Tabellenstimmen × 48k ≈ 12 M Lesezugriffe/s ≈ 2–4 % eines Kerns; 32 SVF ≈ 1 %. Reicht der
schwerste Fall nicht, sinkt die Unison-Obergrenze im Poly-Modus auf 4 (Design-Entscheidung, kein Nutzer-Thema).
Die DSP-Last-Anzeige (M2.1) zeigt künftig Analyse und Synth getrennt.

### 3.7 UI-Technik: nativ JUCE oder WebView?

| Kriterium | Native JUCE-Komponenten + eigenes LookAndFeel | WebView-UI (JUCE 8+/9 `WebBrowserComponent`, Relays) |
|---|---|---|
| FL Studio/Windows | bewährt; Direct2D-Renderer seit JUCE 8 | braucht Edge-WebView2-Runtime (auf Win 11 meist da, nicht garantiert) + Loader/statisches Linken; Fokus/Tastatur/DPI im FL-Wrapper **(unbestätigt)**; Benutzerdatenordner bei mehreren Instanzen |
| pluginval 10 mit GUI, Linux-CI (xvfb) | läuft heute | WebKitGTK auf Linux, headless fehleranfällig; GUI-Tests müssten neu gebaut werden |
| Vorhandene Tests | `PluginEditorTests` (23) bleiben nutzbar | Editor-Tests in Catch2 entfallen, JS-Tests + Bridge-Tests nötig |
| Live-Grafik (Tuner 60 Hz, Pitch-Kurve, Wavetable-Position) | direkt aus Atomics im Timer | Datenbrücke Message-Thread → JS, zusätzlicher Overhead/Jitter |
| Werkzeuge, Build | nur C++/CMake | zweite Toolchain (npm, Bundling in BinaryData), mehr Abhängigkeiten (Lizenzen prüfen) |
| Aufwand für schönes Design | höher pro Widget, aber ein Satz eigener Widgets reicht | schnelleres Styling |

**Empfehlung: nativ.** Vektor-gezeichnete eigene Widgets (keine Bitmaps → automatisch HiDPI-scharf), ein Theme mit
Farb-/Größen-Tokens, eine skalierbare Editor-Hülle. Risiko und Testaufwand einer WebView sind für ein Ein-Personen-
Projekt mit FL als Haupt-Host zu hoch; `JUCE_WEB_BROWSER=0` bleibt. Schrift: System-Schrift oder eine
eingebettete Schrift unter SIL OFL 1.1 (z. B. Inter) mit Lizenztext im Repo.

---

## 4. Releases

Für jedes Release gilt der bewährte Ablauf (STATUS.md „Arbeitsweise“): **Design-Agent** (Design-Dokument +
kompilierbares Gerüst mit eingefrorenen Schnittstellen, leeren Testdateien, CMake-Einträgen, Parametern) →
**Kritik-Agent** (Gegenlesen des Designs) → **Implementierungs-Wellen** mit höchstens 2 parallelen Agenten, je mit
eigenem Build-Verzeichnis und **disjunkten Dateien** → **Integrations-Agent** (einziger, der
`PluginProcessor.*`, `Parameters.h`, `CMakeLists.txt` nach dem Gerüst anfasst) → **Review-Agenten**
(`juce-review`, `audio-dsp-review`, Korrektheit) → **Gegenprüfung** jedes Befunds → **Fix-Agent** mit
Regressionstest je Befund → Commit/Push → CI → Windows-ZIP an den Nutzer.

**Gates für jedes Release** (alle grün, sonst kein ZIP):
- ctest komplett (Linux unter xvfb), 0 Compiler-Warnungen (clang lokal, MSVC in CI).
- pluginval **Strictness 10 mit GUI-Tests** für beide VST3; Steinberg-`validator` beide VST3; `clap-validator`
  beide CLAP (nur die bekannte Warnung `state-invalid-random`).
- CI Linux + Windows grün; macOS-Lauf manuell vor dem Merge.
- Neue Tests: Blockgrößen-Invarianz {1, 7, 64, 480, 512, 4096} × {44,1, 48, 96 kHz}, Allokations-Zähler im
  Audio-Pfad, State-Fixtures der Vorversionen laden.
- Benchmarks (Catch2 `BENCHMARK`, Tag `[!benchmark]`, lokal Release) mit Werten im Design-Dokument; CPU-Ziele aus
  3.6 sind Gate für die lokale Messung, in der CI nur Bericht (Runner schwanken).
- pluginval-Laufzeit mit ca. 130 Parametern prüfen (Timeouts in der CI ggf. anheben).

### 4.1 Release 0.4.0 – Eigener Klang

**Ziel:** VocalSynth FX auf dem Mikro-Insert, Synth an, singen → ein guter, ausdrucksstarker Wavetable-Synth klingt
sofort, ohne MIDI-Routing. Der Weg über den MIDI-Port zu externen Synths bleibt unverändert.

**Features**
- Engine nach 3.2 (2 WT-Osc + Unison, Sub, Noise, SVF mit Drive, 3 ADSR, 2 LFOs, 8 Mod-Slots, Mono/Legato/Poly 16,
  Glide, Chorus/Delay/Reverb), Factory-Wavetables, Import inkl. Einbettung ins Projekt.
- Stimm-Quellen Level, Brightness, Noisiness, Pitch als Mod-Quellen (3.5).
- Mix: `synthOn`, `synthVolume`, `dryLevel`; `muteDry` bleibt als harter Schalter. Instrument-Variante spielt den
  Synth aus dem Sidechain-Mikro (Entscheidung E5).
- 24 Factory-Presets, Preset-Leiste (Name, vor/zurück, Menü nach Kategorie, Speichern als Nutzer-Preset).
- Design-Fundament: `Theme`, `VsLookAndFeel`, Widgets `Knob` (mit Modulations-Ring), `SectionPanel`,
  `ChoiceChip`; Editor-Hülle skalierbar (siehe 4.2, in 0.4 schon mit fester Seitenverhältnis-Skalierung 75–200 %).
- Neue Seite **„Synth“** (Osc A/B mit einfacher 2D-Wavetable-Anzeige und Positionsmarke, Sub/Noise, Filter,
  Hüllkurven als Regler, LFOs, Mod-Matrix als 8 Zeilen, Voice, FX). Die alten Seiten bleiben in 0.4 inhaltlich
  gleich und bekommen nur das neue LookAndFeel.

**Module (neu)**
- Core `core/include/vocalsynth/synth/` + `core/src/synth/`: `RealFft`, `Wavetable` (Daten + Mip-Stufen),
  `WavetableBuilder` (Frames → Mip-Maps, Normalisieren, Ausdünnen), `FactoryWavetables`, `WavReader`
  (inkl. `clm `), `WavetableOscillator` (Unison), `SubOscillator`, `NoiseSource`, `SvfFilter`, `Envelope`, `Lfo`,
  `ModMatrix`, `SynthSettings` (Klarwerte, Sanitisierung), `SynthVoice`, `VoiceAllocator`, `SynthEngine`,
  `fx/Chorus`, `fx/StereoDelay`, `fx/FdnReverb`; außerhalb von `synth/`: `ExpressionFrame.h` (Frame + Puffer),
  `ExpressionTracker` (Level/Brightness/Noisiness/Pitch). `VoicePipeline` bekommt nur die neue Überladung.
- Plugin `plugin/`: `SynthParameters.h`, `SynthSettingsBuilder.{h,cpp}` (Raw-Atomics → `SynthSettings`),
  `WavetableStore.{h,cpp}` (Slots, Retire, Factory-Cache, Hash-Teilen), `WavetableWorker.{h,cpp}` (Import,
  Bau), `SynthState.{h,cpp}` (Kind `SYNTH`), `PresetManager.{h,cpp}`, `FactoryPresets.{h,cpp}`;
  UI in `plugin/ui/`: `Theme.h`, `VsLookAndFeel.{h,cpp}`, `Knob.{h,cpp}`, `SectionPanel.{h,cpp}`,
  `WavetableView.{h,cpp}` (2D), `PresetBar.{h,cpp}`, `SynthPanel.{h,cpp}`.
- Geändert (nur Integration): `PluginProcessor.*` (Engine statt Monitor-Ton als Klang, Mix, State, Migration),
  `PluginEditor.*` (vierte Seite, LookAndFeel), `Parameters.h` (Aufruf), CMake-Listen.

**Parameter (Version-Hint 4, ca. 100)**

| Gruppe | IDs (x = A/B, n = Nummer) |
|---|---|
| Global/Mix | `synthOn`, `synthVolume` (dB, −60…+6, Default −6), `dryLevel` (dB, −60…0, Default 0) |
| Spielweise | `playMode` (Mono, Legato, Poly; Default Legato), `glideTime` (0–2000 ms, 0), `glideMode` (Always, Legato only), `ampVelocity` (0–100 %) |
| Osc A/B | `oscXOn`, `oscXPosition`, `oscXOctave`, `oscXSemi`, `oscXFine`, `oscXLevel`, `oscXPan`, `oscXUnison` (1–8), `oscXDetune`, `oscXSpread`, `oscXPhaseRand` |
| Sub/Noise | `subLevel`, `subOctave`, `subShape`, `noiseLevel`, `noiseColor` |
| Filter | `filterOn`, `filterMode` (LP12, LP24, BP, HP, Notch), `filterCutoff`, `filterResonance`, `filterDrive`, `filterKeytrack`, `filterEnvAmount` |
| Hüllkurven | `env1Attack` … `env3Release` (4 × 3) |
| LFOs | `lfoNShape`, `lfoNRate`, `lfoNSync` (Off, 1/32 … 4 bars), `lfoNRetrig`, `lfoNFade` (n = 1, 2) |
| Mod-Matrix | `modNSource`, `modNDest`, `modNAmount` (n = 1…8) |
| FX | `chorusOn/Rate/Depth/Mix`, `delayOn/Time/Sync/Feedback/PingPong/Mix`, `reverbOn/Size/Damping/Mix` |

Nicht als Parameter: Tabellenwahl (State `SYNTH`, da Nutzer-Tabellen keine feste Liste haben), Preset-Name.

**Tests**
- *Wavetable:* jede Mip-Stufe hat keine Energie über ihrer Grenze (FFT, < −120 dB), DC = 0, Spitze = 1; Builder
  deterministisch; `WavReader`: `clm`-Chunk, Vielfache von 2048, Single-Cycle ungerader Länge, stereo, 8/16/24/32f,
  > 256 Frames, leere/abgeschnittene/zufällige Dateien (Fuzz, kein Absturz, sauberer Fehler).
- *Oszillator:* Frequenz ±0,1 ct (FFT-Spitze) über 20 Hz–8 kHz; **Alias-Test:** Säge-Sweep und Factory-Tabellen
  bei 44,1/48/96 kHz, Energie nicht-harmonischer Komponenten ≥ 60 dB unter der Grundwelle bis f0 = 5 kHz (≥ 70 dB
  bis 1 kHz); Unison-Detune-Verteilung; Positions-Morph und Mip-Wechsel während eines Glides ohne Sprung
  (max. Sample-Differenz gegen Referenz begrenzt).
- *Filter:* LP12/LP24 −3 dB bei fc ± 0,5 dB und Flankensteilheit, BP/HP/Notch-Form; Stabilität bei maximaler
  Resonanz + Drive + Cutoff-Sprüngen 20 Hz ↔ 20 kHz (keine NaN/Inf, begrenzt).
- *Hüllkurven/LFO:* Segmentzeiten ± 1 Sample am Steuer-Tick, Legato ohne Neustart, Tempo-Sync gegen Playhead-BPM.
- *Allocator:* keine hängenden Stimmen bei beliebigen Event-Folgen (Zufalls-Sequenzen + Invarianten), höchstens 16,
  Stehlreihenfolge, Mono-Last-Note-Priorität, Glide-Zeit, Korrektur-Regel ≤ 40 ms.
- *Engine:* **Blockgrößen-Invarianz bitgleich** (alle Blockgrößen × Raten), Allokations-Zähler, Stille → exakt 0
  und inaktive Stimmen nach Ausklang, Bend mit `bendRange`, Expression-Interpolation, Wavetable-Tausch während
  einer Note ohne Klick.
- *Golden-Audio:* jedes Factory-Preset rendert eine feste Notenfolge (gehaltener Ton, Legato-Lauf, Akkord bei
  Poly) bei 48 kHz; verglichen wird ein **Merkmals-Fingerabdruck** (RMS, Spitze, spektraler Schwerpunkt, Flachheit
  je 100 ms) mit `tests/data/golden/presets-0.4.json` und Toleranzen – keine Sample-Hashes, weil MSVC und clang
  nicht bitgleich rechnen. Zusätzlich: Spitze ≤ −1 dBFS, kein DC, keine Stille, kein NaN.
- *Plugin:* IDs eindeutig/unverändert, Version-Hint 4, Reihenfolge angehängt; **0.3.1-State-Fixtures laden** →
  `synthOn` aus, alle alten Werte gleich; State-Rundreise mit eingebetteter Nutzer-Tabelle; Preset-Rundreise und
  „Preset ändert keine Voice-/Capture-Parameter“; Store-Stresstest (Message-Thread tauscht 1000× Tabellen,
  Audio-Thread rendert; optional TSan-Lauf lokal); Editor: Synth-Seite für beide Varianten, **jeder
  Synth-Parameter hat ein Bedienelement**, bestehende Editor-Tests bleiben grün.
- *Benchmarks:* typisches Preset, schwerster Fall, Stille, Mip-Map-Bau einer 256-Frame-Tabelle (Ziel < 150 ms im
  Hintergrund), Instanz-Start (pluginval misst Ladezeit).

**FL-Testcheckliste für den Nutzer (0.4.0, kommt als `docs/SYNTH-TESTING.md` ins ZIP)**
1. Altes 0.3.1-Projekt öffnen: klingt wie vorher (Synth aus), Einstellungen da.
2. Neue Instanz VocalSynth FX auf dem Mikro-Insert: Synth an, singen → Synth hörbar ohne Routing; Mute Dry und
   Dry-Pegel; Latenzgefühl wie bisher (keine zusätzliche Verzögerung).
3. 10 Presets durchschalten (Lead, Bass, Pad, Vocal), Eindruck notieren („klingt gut/billig/zu leise“).
4. Stimm-Modulation: Preset „Vocal …“: lauter/leiser, hell/dunkel („ii“ vs. „uu“), Zischen – hört man es?
5. Eigene Wavetable importieren (z. B. eine selbst erzeugte oder eine mit klarer Lizenz), Position drehen;
   Projekt speichern, FL schließen, öffnen → Tabelle und Klang wieder da.
6. Cutoff und Wavetable-Position in FL automatisieren (Automation-Clip), Wiedergabe.
7. CPU in FL bei Puffer 128/256 mit Preset „Pad“ (Poly) und schwerstem Preset; DSP-Anzeige des Plugins ablesen.
8. Instrument-Variante mit Sidechain-Mikro spielt den Synth (wenn T3-Weg klappt; sonst überspringen).
9. Externer Synth über MIDI-Port weiterhin o.k., Capture/Drag & Drop unverändert.
10. Bypass/Entfernen bei klingender Note: nichts hängt; Puffergröße während der Wiedergabe ändern.
11. Fenster vergrößern/verkleinern (Skalierung), auf HiDPI-Monitor scharf?

**Abnahme**
- Synth ohne Routing hörbar in FX und Instrument; gemeldete Latenz 0; keine hängenden Stimmen.
- Alias-, Filter-, Invarianz- und Golden-Tests grün; CPU-Ziele aus 3.6 lokal erreicht.
- Alte Projekte unverändert; State und Presets rundreisefest; Import für Serum-artige 2048er-WAVs und
  Single-Cycles.
- Nutzer: mindestens die Hälfte der Presets „brauchbar“, Rest wird in 0.4.1 nachgestimmt.

**Aufwand:** groß, ca. 10–14k Zeilen (ca. 40 % Tests), 8–12 Arbeitstage, 4 Implementierungswellen.

**Agenten-Aufteilung (disjunkte Dateien, höchstens 2 gleichzeitig)**

| Phase | Agent | Dateien |
|---|---|---|
| 0 | Design + Gerüst | `docs/SYNTH-DESIGN.md`, alle neuen Header mit eingefrorenen Schnittstellen, `SynthParameters.h`, CMake, leere Testdateien, State-Fixtures 0.3.1 |
| 0 | Kritik | nur Review des Designs |
| 1 | A „Wavetable“ | `RealFft`, `Wavetable`, `WavetableBuilder`, `FactoryWavetables`, `WavReader`, `WavetableOscillator`, `SubOscillator`, `NoiseSource` + Tests/Benchmarks |
| 1 | B „Stimme“ | `SvfFilter`, `Envelope`, `Lfo`, `ModMatrix`, `SynthSettings`, `SynthVoice` (gegen Oszillator-Schnittstelle aus dem Gerüst), `VoiceAllocator` + Tests |
| 2 | C „Engine + FX + Expression“ | `SynthEngine`, `fx/*`, `ExpressionFrame`, `ExpressionTracker`, `VoicePipeline`-Überladung (nur neue Funktion + Tests) |
| 2 | D „Plugin-Daten“ | `SynthSettingsBuilder`, `WavetableStore`, `WavetableWorker`, `SynthState`, `PresetManager` + Plugin-Tests |
| 3 | E „UI-Fundament + Synth-Seite“ | `plugin/ui/*` (Theme, LookAndFeel, Knob, SectionPanel, WavetableView, PresetBar, SynthPanel) + `SynthPanelTests` |
| 3 | F „Klang“ | `FactoryPresets.*`, Golden-Fingerabdrücke, `docs/SYNTH-TESTING.md` (deutsch) |
| 4 | Integration | `PluginProcessor.*`, `PluginEditor.*`, `Parameters.h`, CMake, README/STATUS; **Hörprobe-ZIP** an den Nutzer mit Init + 8 Presets, bevor Review/Fixes laufen |
| 5 | Review ×2, Gegenprüfung, Fix | Befunde + Regressionstests |

Agent F arbeitet ohne Gehör: Presets entstehen aus bekannten Synthese-Rezepten und werden über Messwerte geprüft
(Pegel, Schwerpunkt pro Kategorie, Hüllkurvenform); das eigentliche Urteil kommt vom Nutzer (Hörprobe, 0.4.1).

### 4.2 Release 0.5.0 – Neues UI

**Ziel:** Ein UI, das ein FL-Nutzer ohne Anleitung versteht: klar gegliedert, skalierbar, schön, mit
Ersteinrichtung und Preset-Browser. Dazu der Vokal als Mod-Quelle.

**Features**
- **Design:** dunkles Theme mit Akzentfarbe, Tokens für Farben/Abstände/Schriftgrößen in `Theme.h`,
  einheitliche Widgets; Design-Entwurf (Bild/HTML-Mockup) wird **vor** der Umsetzung vom Nutzer freigegeben.
- **Skalierung:** Design-Fläche 1100 × 700, frei skalierbar 70–200 % mit festem Seitenverhältnis
  (`setResizable`, Constrainer, `AffineTransform` auf der Inhaltskomponente), Menü 75/100/125/150/200 %; Größe im
  State (`UI`) und als globaler Standard (Einstellungsdatei im App-Datenordner). Vektorgrafik → HiDPI scharf.
- **Kopfzeile (immer sichtbar):** Logo/Version/Format, Preset-Leiste, Synth an/aus, Synth-/Dry-Pegel, Eingangspegel
  mit Gate-Linie, Latenz-Plakette (ms, Warnung bei großem Puffer), DSP-Last, Seiten-Reiter.
- **Seiten:**
  - **Play** (bisher „Voice“): großer Tuner, **Pitch-Kurve** (letzte 5 s: gesungene Tonhöhe + gesendete Noten),
    Tonart/Skala, Stimmlage, Gate, Stickiness, Glide, Expression-Anzeigen (Level/Brightness/Noisiness) und
    **Vokal-XY**.
  - **Synth:** Osc A/B mit **Wavetable-Ansicht** (2D-Frame + 3D-Stapel, in ein Bild gecacht, nur die
    Positionsmarke bewegt sich), Sub/Noise, Filter mit **Frequenzgang-Kurve**, **Hüllkurven-Ansicht** (Punkte
    ziehen = ADSR-Parameter), **LFO-Ansicht**, **Mod-Matrix** (Tabelle + Ziehen einer Quelle auf einen Regler
    legt einen Slot an; Modulations-Ringe an den Reglern), FX-Leiste.
  - **Capture:** bisheriges `CapturePanel` im neuen Design (Funktion unverändert).
  - **Setup:** Routing (MIDI-Kanal, Ausgabeweg, **Routing-Hilfe** mit dem FL-Rezept aus `FL-ROUTING.md`,
    Kopfhörer-Hinweis, virtueller Port), erweiterte Stimm-Einstellungen (Algorithmus, Mindestlänge, Oktave,
    Bend-Modus/-Bereich, Velocity, Legato), Testmuster + Test-Ton (M1), Diagnose (Blockgrößen, MIDI-Zähler).
- **Preset-Browser** (Overlay): Kategorien, Tags, Suche, Favoriten, Factory (schreibgeschützt) + Nutzer-Ordner,
  Init/Speichern/Speichern unter/Umbenennen/Löschen, Vorhören durch einfaches Anklicken.
- **Ersteinrichtung** (beim ersten Öffnen pro Rechner, jederzeit über „?“): „Wie willst du spielen?“ →
  (A) eingebauter Synth – fertig; (B) externer Synth – Schritt-für-Schritt-Routing mit Port-Nummer;
  (C) nur aufnehmen und ziehen. Plus Puffer-Tipp (128/256) und Kopfhörer-Hinweis.
- **Vokal-Tracker:** `FormantTracker` – Dezimierung auf ca. 11 kHz, LPC Ordnung 10–12 (Autokorrelation,
  Levinson-Durbin), Hüllkurve über `RealFft`, Spitzensuche F1 (250–1000 Hz) und F2 (700–3000 Hz) mit
  Kontinuitäts-Verfolgung, nur bei stimmhaften Frames; Normierung auf eine Vokal-Ebene mit Standardbereichen je
  Stimmlage (veröffentlichte Formant-Mittelwerte, keine Daten-Assets). Mod-Quellen **Vowel X**, **Vowel Y**
  (an die Quellenliste angehängt). Kalibrierung erst in 0.6.

**Module (neu)**
- Core: `FormantTracker`, `Lpc` (Levinson-Durbin), Erweiterung `ExpressionTracker` (vowelX/Y).
- Plugin `plugin/ui/`: `EditorShell` (Skalierung, Kopfzeile, Seiten), `HeaderBar`, `PlayPage`, `SynthPage`
  (ersetzt `SynthPanel`), `CapturePage` (umhüllt `CapturePanel`), `SetupPage`, `PitchCurveView`, `TunerView`
  (aus `TunerDisplay`), `VowelXYView`, `ExpressionMeters`, `WavetableView` (3D-Ausbau), `EnvelopeView`,
  `LfoView`, `FilterResponseView`, `ModMatrixView`, `PresetBrowser`, `FirstRunOverlay`, `RoutingHelpView`,
  `UiSettings` (globale Einstellungen). Plugin: `AnalysisHistoryRing` (SPSC Audio → UI, ein Punkt pro Hop:
  f0, Konfidenz, Note, Pegel, Vokal), `PresetManager`-Ausbau (Tags, Favoriten, Suche).

**Parameter (Version-Hint 5):** wenige neue, z. B. `vowelSmoothing` (ms) und `vowelRange` (Auto/Low/High).
Skalierung und letzte Seite sind keine Parameter (State `UI`).

**Tests**
- Editor-Tests ziehen mit um (Tests werden angepasst, nie ersatzlos gelöscht): Seiten Play/Synth/Capture/Setup
  für beide Varianten; „jeder Parameter hat ein Bedienelement auf irgendeiner Seite“; Label-Abgleich mit
  `FL-ROUTING.md`/`LIESMICH.txt` (Doku im selben PR aktualisiert). Testzugriffe (`showPage`, `getCapturePanel`,
  Display-Tick) bleiben; Komponenten bekommen `componentID`s für stabile Suche.
- Layout-Tests bei 70/100/200 %: alles innerhalb, nichts überlappt (wie der vorhandene CapturePanel-Layout-Test).
- **UI-Leistung:** Editor mit allen Seiten in ein Bild rendern, Zeit pro Frame messen (Benchmark); Timer
  aktualisiert nur geänderte Bereiche (Zähler wie der vorhandene Meter-Repaint-Test).
- `FormantTracker`: synthetische Vokale (Formant-Synthese mit bekannten F1/F2) ± 10 % bei f0 100–400 Hz,
  Rauschen/Stille → keine Werte, Blockgrößen-Invarianz, CPU-Benchmark.
- Preset-Browser: Filter/Suche/Favoriten, Speichern/Umbenennen ohne Datenverlust, schreibgeschützte Factory.
- State-Fixtures 0.3.1 und 0.4.0 laden.

**UI-Leistungsbudget:** Timer 30 Hz (Tuner/Pitch-Kurve 60 Hz nur auf der sichtbaren Play-Seite), Malen im Mittel
< 2 ms und Spitze < 8 ms pro Frame bei 100 % auf einem Mittelklasse-Laptop, keine Vollbild-Repaints pro Tick,
Wavetable-3D-Bild nur bei Tabellenwechsel neu (< 20 ms), Audio-Thread unberührt (UI liest nur Atomics und SPSC).

**FL-Testcheckliste (0.5.0)**
1. Ersteinrichtung erscheint einmal; Weg A und B ausprobieren (Routing-Hilfe verständlich?).
2. Alle vier Seiten ansehen, Fenster auf 75 % und 150–200 % ziehen, auf zweitem Monitor/HiDPI.
3. Preset-Browser: suchen, Favorit, eigenes Preset speichern, nach Neustart wieder da.
4. Wavetable-Ansicht folgt der Position bei Automation und bei Vokal → Position.
5. „aa-ee-ii-oo-uu“ singen: Vokal-XY bewegt sich nachvollziehbar; Preset mit Vokal → Position.
6. CPU der FL-Anzeige mit offenem Fenster vs. geschlossenem Fenster (UI darf Audio nicht stören).
7. Projekte aus 0.3.1 und 0.4.x laden; Fenstergröße bleibt pro Projekt.

**Abnahme:** pluginval 10 mit GUI grün; UI-Budget eingehalten; Testnutzer findet ohne Doku: Synth an, Preset
wählen, Routing zu externem Synth, Take ziehen; Vokal-Erkennung auf synthetischen Vokalen ± 10 %; alle
bisherigen Funktionen erreichbar.

**Aufwand:** groß, ca. 8–10k Zeilen, 8–12 Arbeitstage.

**Agenten-Aufteilung**

| Phase | Agent | Dateien |
|---|---|---|
| 0 | Design + Mockup | `docs/UI-DESIGN.md`, Mockup zur Freigabe durch den Nutzer, Gerüst aller `ui/`-Klassen, Theme-Tokens |
| 1 | A „Shell + Widgets“ | `EditorShell`, `HeaderBar`, `UiSettings`, Widget-Ausbau, Layout-Tests |
| 1 | B „Vokal (Core)“ | `FormantTracker`, `Lpc`, `ExpressionTracker`-Ausbau + Tests/Benchmarks |
| 2 | C „Synth-Seite“ | `SynthPage`, `WavetableView` 3D, `EnvelopeView`, `LfoView`, `FilterResponseView`, `ModMatrixView` |
| 2 | D „Presets + Hilfe“ | `PresetBrowser`, `PresetManager`-Ausbau, `FirstRunOverlay`, `RoutingHelpView` |
| 3 | E „Play + Setup“ | `PlayPage`, `TunerView`, `PitchCurveView`, `VowelXYView`, `ExpressionMeters`, `SetupPage`, `AnalysisHistoryRing` |
| 3 | F „Capture + Tests“ | `CapturePage`, Anpassung `CapturePanel` an das Theme, Umzug der Editor-Tests |
| 4 | Integration, Review (UI-Thread-Grenze, Lebensdauer der Editor-Komponenten), Fix | `PluginEditor.*` (wird zur dünnen Hülle), Doku (`FL-ROUTING.md`, `LIESMICH.txt`, README) |

### 4.3 Release 0.6.0 – Expression & Akkorde (Rest von M5)

**Ziel:** Die Stimm-Expression steuert auch externe Synths (CC), der Vokal ist auf die eigene Stimme kalibriert,
und gesungene Töne können Akkorde spielen.

**Features**
- **Vokal-Kalibrierung:** Assistent „halte a, e, i, o, u je 2 s“, Referenzpunkte pro Projekt (State `VOWEL`,
  `version` 1) und als Datei exportierbar; Ausgabe zusätzlich als 3 Morph-Gewichte (neue Mod-Quellen
  angehängt).
- **Expression → CC:** bis 8 Zuordnungen (Quelle → CC-Nummer, Bereich, Kurve, Glättung), Ratenbegrenzung
  (Standard 100 Hz pro CC, nur bei Änderung), auf dem MIDI-Kanal der Noten; geht über `MidiOutputStage`, also
  auch in Capture/Export (CC-Export gibt es schon).
- **Akkord-Modus** (`ChordMapper`): diatonisch aus Tonart/Skala (Dreiklang, Septakkord, sus, Power) oder feste
  Form; Voicings aus einer Tabelle, Inversion, Spread. Gilt für MIDI-Out, internen Synth (wechselt automatisch auf
  Poly, wenn Akkorde an) und Capture.

**Module:** Core `ChordMapper`, `VowelCalibration`, `CcMapper` (Ratenbegrenzung, Kurven); `VoicePipeline`
bekommt die Akkord-Stufe hinter dem `NoteSegmenter` (Bend gilt für alle Akkordtöne); `TakeRenderer`
(Clean-MIDI) muss mehrstimmige Events verstehen – Risiko, siehe 5. Plugin: `CcMappingState`, UI
`VowelCalibrationWizard`, `CcMatrixView`, Akkord-Bereich auf Play.

**Parameter (Version-Hint 6):** `chordMode` (Off, Diatonic, Fixed), `chordType`, `chordVoicing`, `chordInversion`,
`ccNNSource/Number/Amount/Smoothing` (n = 1…8; Kurve und Bereich im State) – ca. 36.

**Tests:** Akkorde ohne hängende Noten bei jeder Blockaufteilung, Paarung je Akkordton, Skalenwechsel während
eines Akkords; CC-Rate ≤ Grenze, keine doppelten Werte; kalibrierte Vokale ≥ 85 % richtige Region (PLAN M5);
Capture exportiert Akkorde korrekt (Clean und Raw); State-Fixtures 0.3.1–0.5.x.

**FL-Checkliste:** CC an einen externen Synth (z. B. Vital: CC 74 → Cutoff) über den MIDI-Port; Kalibrierung mit
der eigenen Stimme; Akkorde intern und extern; Take mit Akkorden in ein Pattern ziehen.

**Abnahme:** PLAN-M5-Kriterien; keine hängenden Noten; CC kommt in FL an (T1-Weg).

**Aufwand:** mittel, ca. 4–6k Zeilen, 5–7 Arbeitstage. **Agenten:** A „Akkorde“ (`ChordMapper`, Pipeline-Stufe,
`TakeRenderer`-Anpassung), B „CC + Kalibrierung“ (`CcMapper`, `VowelCalibration`, `MidiOutputStage`-Anbindung),
danach C „UI“ (Assistent, CC-Matrix, Akkord-Bereich), Integration/Review/Fix.

### 4.4 Release 0.7.0 – Beatbox-Trigger und Drum-Sampler (Ausblick, eigenes Design später)

- M4 wie in PLAN.md: `OnsetDetector` (HFC + SuperFlux), `FeatureExtractor` (MFCC über `RealFft`),
  `TriggerClassifier` (kNN, Unbekannt-Schwelle, Fast/Accurate), Training-Thread, `ModelStore` (gleiches
  Tausch-/Retire-Muster wie `WavetableStore`), Profile im State.
- **Drum-Sampler** (M6-Rest): 8 Pads, Velocity-Layer, Choke-Gruppen; **Factory-Kit im Code synthetisiert**
  (808/909-artige Kick, Snare, Hats, Clap, Perc – keine Fremd-Samples), Import eigener Samples, Beatbox-Kit aus
  einem Take schneiden.
- UI: Trigger-Pads mit Konfidenz, Training-Assistent mit Konfusionsmatrix (neue Seite „Beat“ im 0.5-Design).
- Aufwand groß (10–15 Tage), hängt vom Beatbox-Korpus ab (PLAN 4.2) – der Nutzer müsste dafür Hits aufnehmen.

---

## 5. Risiken

| Risiko | W. / Auswirkung | Gegenmaßnahme |
|---|---|---|
| Agenten können nicht hören: Presets klingen „technisch richtig, musikalisch schwach“ | hoch / mittel | Rezept-basierte Presets, Messwert-Prüfungen, **Hörprobe-ZIP vor Review**, 0.4.1 für Nachstimmen nach Nutzer-Feedback |
| CPU mit Unison/Poly auf Laptops in FL | mittel / mittel | Budget 3.6 mit Benchmarks, getrennte DSP-Anzeige, Unison-Obergrenze im Poly-Modus, ausgeklungene Stimmen exakt aus |
| Aliasing durch Drive, schnelle Pitch-Modulation | mittel / niedrig | weiche, begrenzte Sättigung, Alias-Tests; HQ-Modus (Oversampling/ADAA) später |
| Blockgrößen-Invarianz wird durch Vektorisierung/Compiler gebrochen | mittel / mittel | Steuer-Takt an absoluten Positionen, Vektorisierung nur über Stimmen, nie über Zeit; plattformübergreifend nur Merkmals-Goldens |
| Wavetable-Tausch/Retire falsch → Absturz oder Speicherleck | niedrig / hoch | einfaches Epoch-Muster, Stresstest, TSan lokal, `audio-dsp-review` |
| Projekt-Größe/Speicher durch eingebettete Tabellen, viele Instanzen | mittel / niedrig | 16-Bit-Einbettung, Teilen per Hash, Factory-Tabellen nur bei Bedarf und prozessweit |
| ca. 130 neue Parameter: lange Liste im FL-Wrapper, pluginval-Laufzeit, Preset-Laden erzeugt viele Änderungsmeldungen (Automation-Aufnahme in FL **unbestätigt**) | mittel / niedrig | klare Namen mit Gruppen-Präfix, CI-Timeouts prüfen, Preset-Laden mit Gesten-Klammer; FL-Check |
| Editor-Umbau bricht Tests/Doku-Abgleich | mittel / mittel | Testzugriffe und `componentID`s, Tests im selben PR umziehen, Doku-Label-Test bleibt |
| Skalierung/DPI im FL-Wrapper (Fenstergröße, Skalierung pro Monitor) **unbestätigt** | mittel / mittel | Skalierung per Transform (unabhängig vom Host), Menü-Fallback, FL-Check in 0.4 und 0.5 |
| Akkorde mit Clean-MIDI/Capture (TakeRenderer ist einstimmig gedacht) | mittel / mittel | im 0.6-Design: Akkorde bei Export aus der Grundton-Spur neu erzeugen oder Clean für Akkorde abschalten |
| Vokal-Erkennung bei hohen Stimmen (F1 nahe f0) und Mikro-Klang | hoch / mittel | Stimmlagen-abhängige Bereiche, Kalibrierung in 0.6, Glättung, Konfidenz-Gate |
| Lizenzen: Fremd-Wavetables, Schriften, Formate | niedrig / hoch | alle Tabellen/Presets/Kits im Code erzeugt; nur OFL-Schrift mit Lizenzdatei; `clm`-Chunk lesen ist eine Dateikonvention, kein fremder Code; keine fremden Marken/Logos im UI |
| Umfang läuft aus dem Ruder | mittel / mittel | „Nicht in diesem Block“-Liste (Abschnitt 2), je Release ein eingefrorenes Gerüst, Zusatzwünsche in 0.x.1 |

Weiterhin offen aus PLAN.md: die **Lizenz des eigenen Codes** (Repo ist öffentlich). Vor 0.5 klären, spätestens
bevor externe Beiträge angenommen werden.

---

## 6. Offene Entscheidungen für den Nutzer

| # | Frage | Optionen | Empfehlung (Standard, wenn keine Antwort) |
|---|---|---|---|
| E1 | **Reihenfolge** | (a) Synth 0.4 → UI 0.5 → Expression/Akkorde 0.6 → Beatbox 0.7; (b) Beatbox vor Expression; (c) UI vor Synth | **(a)** – schnellster spürbarer Gewinn, UI wird nur einmal gebaut |
| E2 | **UI-Technik** | natives JUCE mit eigenem Design; WebView (HTML/JS) | **Nativ** – sicher in FL und pluginval, vorhandene Tests bleiben, kein Zusatz-Toolchain |
| E3 | **Synth-Umfang 0.4** | kleiner (1 Osc, ohne FX); wie geplant (2 WT-Osc + Sub/Noise, 1 Filter, 3 Hüllkurven, 2 LFOs, 8 Mod-Slots, 3 FX); größer (2 Filter, Warp/FM, Arpeggiator) | **Wie geplant**; Warp/FM, zweiter Filter, Arp später |
| E4 | **Synth bei neuen FX-Instanzen** | an (Dry an) / aus | **An, Dry an** – man hört sofort etwas; die Ersteinrichtung fragt nach externem Synth und schaltet dann aus. Alte Projekte: immer **aus** |
| E5 | **Instrument-Variante** | bleibt Test/Sidechain-Variante; wird vollwertiger Synth (gleiche Engine) | **Vollwertiger Synth** – kostet fast nichts, gleiche Engine |
| E6 | **Presets und Spielweise** | Anzahl 12 / 24 / 40; Standard Mono, Legato oder Poly | **24 Factory-Presets, Standard Legato** (Stimme ist einstimmig, Glide sauber); Poly in Pad-Presets und bei Akkorden |

---

## 7. Nächste Schritte

1. Nutzer: Kurztest 0.3.1 (STATUS.md) und Antworten zu E1–E6 (ohne Antwort gelten die Empfehlungen).
2. 0.3.1 nach `master` mergen, Branch `v0.4-synth` von `master`.
3. Design-Agent: `docs/SYNTH-DESIGN.md` + Gerüst + State-Fixtures 0.3.1; Kritik-Agent; dann Welle 1 (A/B).
4. Nach Welle 2/3: Hörprobe-ZIP an den Nutzer, parallel Integration und Review.
5. PLAN.md und STATUS.md auf die neue Reihenfolge anpassen, sobald E1 entschieden ist.
