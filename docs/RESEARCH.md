# VocalSynth – Technische Recherche

Stand: 2026-10-14. Ziel: ein Open-Source-Plugin (VST3, Haupt-Host FL Studio unter Windows, AU/macOS optional), mit dem man per Stimme Synths spielt und Samples/MIDI erzeugt, vergleichbar mit Vochlea Dubler 2.

**Wie belastbar die Angaben sind:** Die Fakten stammen aus Websuchen, Review-Artikeln, Foren und Repositories, die ich direkt gelesen habe (JUCE, pamplejuce, clap-juce-extensions, nih-plug, SwiftF0, NeuralNote, RTNeural, anira, DPF, iPlug2; per `git clone` am 2026-10-14). Die Domains image-line.com, vochlea.com und kvraudio.com konnte ich in dieser Umgebung nicht direkt abrufen. Angaben zu diesen Seiten kommen deshalb aus Suchmaschinen-Auszügen. Alles, was nur auf Forenberichten beruht, ist mit **(unbestätigt)** markiert und muss vor der Architektur-Entscheidung in FL Studio selbst getestet werden (siehe Abschnitt 2.5).

---

## 1. Marktanalyse

### 1.1 Vochlea Dubler 2: Funktionen

| Bereich | Was Dubler 2 macht | Quelle |
|---|---|---|
| Architektur | **Standalone-App, kein Plugin.** Die App stellt einen eigenen virtuellen MIDI-Treiber bereit (Schalter im Settings-Zahnrad). Die DAW sieht Dubler wie ein USB-Keyboard. Unter Windows bringt Dubler einen eigenen „Dubler ASIO“-Treiber mit. | [10 Tips](https://vochlea.com/blog/10-tips-for-getting-started-with-dubler-2), [MIDI-Input-Hilfe](https://vochlea.com/learn/dubler-not-showing-as-a-midi-input) |
| MIDI Capture | Begleit-Plugin (VST3/AU), das als Instrument auf einer MIDI-Spur sitzt. Es verbindet sich mit der App, nimmt bei laufendem Transport auf und ist BPM-synchron. Ansicht „Raw“ oder „Clean“ (entfernt Ghost-Notes und Übergangsnoten). Clips werden per **Drag & Drop** in die DAW gezogen. Seit 2.2.0 kommen Session-Historie und automatische Tonart-Vorschläge dazu. | [Blog MIDI Capture reborn](https://vochlea.com/blog/the-midi-capture-companion-plugin-reborn), [Tutorial](https://vochlea.com/tutorials/midi-capture) |
| Pitch → MIDI | Monophones Pitch-Tracking, Key/Scale-Lock mit vielen Skalen, Tonart-Vorschlag, „Stickiness“ (Hysterese, kostet Latenz). | [SOS](https://www.soundonsound.com/reviews/vochlea-dubler-2), [MusicTech](https://musictech.com/reviews/software-instruments/vochlea-dubler-2-review/) |
| Akkorde | Chord-Library, Voicings, eigene Akkorde. Die gesungene Grundnote löst den Akkord aus. | [Synthtopia](https://www.synthtopia.com/?p=126354) |
| Pitch Bend | **IntelliBend** hält die Note fest, bis man bewusst wegbendet. **TruBend** folgt der Stimme 1:1. | [Synthtopia](https://www.synthtopia.com/?p=126354) |
| Trigger (Beatbox) | Bis zu **8 trainierbare Sounds** pro Profil (mehr über mehrere Profile). Im „Train“-Tab nimmt man bis zu 12 Beispiele pro Sound in einem Durchgang auf. Velocity-sensitiv. | [SOS News](https://www.soundonsound.com/node/4926821), [SOS Review](https://www.soundonsound.com/reviews/vochlea-dubler-2) |
| CC-Steuerung | Hüllkurve (Lautstärke) → CC und **Vokal-Erkennung**: Anteil von 3 Vokal-Komponenten, je auf ein CC mappbar. Pitch → CC. | [SOS News](https://www.soundonsound.com/node/4926821), [MusicTech](https://musictech.com/reviews/software-instruments/vochlea-dubler-2-review/) |
| Mikrofon | Jedes Mikro möglich. Empfohlen sind dynamische Mikros, keine Kondensatormikros. Das mitgelieferte USB-Mikro liefert laut Tests die saubersten Ergebnisse. | [Producer Hive](https://producerhive.com/buyer-guides/microphones/vochlea-dubler-2-review/) |
| Latenz | Kein offizieller ms-Wert gefunden. Vochlea empfiehlt ca. 128 Samples Puffer und ASIO. | [Studio-One-Guide](https://vochlea.com/learn/getting-started-with-studio-one), [FL-Guide](https://vochlea.com/learn/getting-started-with-fl-studio) |

**Preise (uneinheitlich, bitte im Shop prüfen):**
- Zum Launch 2021: Software 249 USD / 189 GBP, Studio Kit 2 (mit Mikro) 329 USD / 249 GBP.
- Aktuell: Studio Kit 2 (digital) im Vochlea-Shop für 179 GBP statt 245,99 GBP; Software bei Händlern ca. 98 GBP ([Bax](https://www.bax-shop.co.uk/audio-software/vochlea-dubler-2), [Shop](https://shop.vochlea.com/products/dubler-studio-kit-3)).
- Neu seit Juli 2026: **DubBox**, eine Standalone-App für Beatbox → Drum-Loops mit bis zu 8 eigenen Sounds, MIDI-/Audio-Export, 69 USD bzw. 39 USD für Dubler-Besitzer ([Shop](https://shop.vochlea.com/products/dubbox), [SOS](https://www.soundonsound.com/news/vochlea-announce-dubbox), [MusicRadar](https://www.musicradar.com/music-tech/beatbox-your-ideas-into-reality-with-vochleas-dubbox-an-app-that-converts-your-voice-into-drum-loops)).

**Stärken:** schneller Beatbox-Trigger-Workflow mit gutem Training-UI, Akkord- und Scale-Funktionen, kreative CC-Steuerung über Vokale, funktioniert in jeder DAW.

**Schwächen und Nutzerbeschwerden:**
- Pitch-Tracking reagiert „zu präzise“: leicht unsauberes Singen erzeugt falsche Noten oder ungewollte Bends ([SOS](https://www.soundonsound.com/reviews/vochlea-dubler-2)). „Stickiness“ kostet Latenz, schnelle Melodien leiden ([MusicTech](https://musictech.com/reviews/software-instruments/vochlea-dubler-2-review/)).
- Bei komplexen Melodien entsteht „Spaghetti-MIDI“ mit viel Nacharbeit. Melodyne gilt beim MIDI-Export als besser ([Producer Hive](https://producerhive.com/buyer-guides/microphones/vochlea-dubler-2-review/)).
- Latenz: mit Fremdmikros und Interface spürbar höher. Bei 128 Samples Puffer war es für einen Tester „fast unmöglich, im Takt zu bleiben“ ([Engadget](https://www.engadget.com/dubler-2-can-turn-any-microphone-into-a-midi-controller-090002585.html)). Ein KVR-Nutzer berichtet, die Latenz sei nach einem Update schlechter geworden ([KVR](https://www.kvraudio.com/forum/viewtopic.php?t=554947)).
- Die Trigger-Erkennung hält laut einzelnen Nutzern das 8-Sound-Versprechen nicht ([Trustpilot](https://pt.trustpilot.com/review/vochlea.com)).
- Das Setup über separate App + virtuellen MIDI-Treiber + ASIO ist fehleranfällig: der Treiber wird nicht erkannt, manche Mac-Apps erkennen ihn gar nicht ([Vochlea-Hilfe](https://vochlea.com/learn/dubler-not-showing-as-a-midi-input)).

**Folgerungen für VocalSynth:**
1. Als **echtes Plugin** gebaut entfällt der Setup-Schmerz mit separater App und virtuellem Treiber.
2. Transparente Hysterese- und Latenz-Regler.
3. Ein „Clean“-Postprocessing für aufgenommenes MIDI.
4. Ein besseres Few-Shot-Trigger-Training (siehe Abschnitt 3.3).

### 1.2 Vergleichbare Produkte

| Produkt | Typ | Echtzeit? | Notizen | Quelle |
|---|---|---|---|---|
| **imitone** | Standalone, Voice → MIDI | ja | eigener „Resonator“-Tracker, ca. 6,6 ms Latenz in einem älteren Test, Vibrato/Expression → CC1/CC7, 25–60 USD perpetual | [Ask.Audio](https://ask.audio/articles/review-imitone---mind-to-melody-software), [AlternativeTo](https://alternativeto.net/software/imitone/about) |
| **Jam Origin MIDI Guitar 2/3** | Plugin + Standalone | ja, polyphon | Gitarre → MIDI, patentierte polyphone Erkennung mit Vorhersage, ca. 150 USD | [SOS](https://www.soundonsound.com/reviews/jam-origin-midi-guitar), [KVR](https://www.kvraudio.com/forum/viewtopic.php?p=8553005) |
| **Celemony Melodyne** | Offline-Editor (ARA) | nein | Referenz für die Qualität von Audio → MIDI, lokal | [AudioCipher](https://audiocipher.com/post/audio-to-midi) |
| **Samplab 2** | Plugin (VST3/AU) | nein | KI-Audio → MIDI **in der Cloud**, Basic gratis, Premium ca. 9,99 USD/Monat | [Magnetic](https://magneticmag.com/2023/01/samplab-2-review/), [Rekkerd](https://rekkerd.org/samplab-plugin-turns-harmonic-audio-samples-into-midi) |
| **NeuralNote** | Plugin, Open Source (Apache-2.0) | nein (Transkription) | lässt sich als Referenz-Codebasis für JUCE + ML nutzen. Die aktuellen Modellgewichte sind CC BY-NC (nur nicht-kommerziell). | Repo [DamRsn/NeuralNote](https://github.com/DamRsn/NeuralNote) |
| **Dodo MIDI** | Voice → MIDI | ja | keine Akkorde, keine Drums | [LANDR](https://blog.landr.com/audio-to-midi/) |
| **Vochlea DubBox** | Standalone | ja (Trigger) | siehe Abschnitt 1.1 | s. o. |
| Browser-Tools (Selekt, Veena) | Web | teilweise | Hum/Beatbox → MIDI. Rankings stammen teils vom Hersteller selbst, also Marketing. | [Selekt](https://selektaudio.com/tools/voice-to-midi), [Veena](https://www.veena.studio/blog/best-ai-voice-to-instrument-tools) |

Zu Antares Auto-Tune mit MIDI-Out und zu Sonic Charge gab es keine relevanten Treffer. Für Voice → MIDI in Echtzeit spielen sie keine Rolle.

**Marktlücke:** Es gibt kein Open-Source-Plugin mit Echtzeit-Voice-to-MIDI, Beatbox-Triggern und Vokal-CC, das gut in FL Studio integriert ist.

---

## 2. FL Studio: Plugin-MIDI-Ausgabe (architekturkritisch)

### 2.1 Formate und Versionen

- **Aktuelle Version:** FL Studio 2026 (Release 7. Juli 2026, aktuell 2026.1.x; [AlternativeTo](https://alternativeto.net/news/2026/7/fl-studio-2026-brings-rebuilt-flex-fl-cloud-project-storage-and-new-transmitter-plugin/)). Die Versionsnummern laufen inzwischen nach Jahren: 2024 → 2025 → 2026.
- **VST3:** voll unterstützt. Seit FL 2025.2 kann die VST3-Version eines Plugins die VST2-Version in alten Projekten ersetzen ([FL 2025.2](https://www.image-line.com/fl-studio-news/fl-studio-2025-2)).
- **CLAP:** wird gehostet **seit FL Studio 2024** ([Rekkerd](https://rekkerd.org/image-line-launches-fl-studio-2024-incl-clap-plugin-support/), [Release Notes 24.1.1](https://forum.image-line.com/viewtopic.php?p=1955986)). Ob FL den CLAP-Note-Output in sein Port-System weiterleitet, ist nicht dokumentiert **(unbestätigt)**.
- **VST3-CC-Output:** Seit **FL 21.0.3** (April 2023) unterstützt der Wrapper den VST3-Eventtyp „Legacy MIDI CC Out“ ([Release Notes 21.0.3](https://forum.image-line.com/viewtopic.php?t=302819)). Das ist wichtig: VST3 kennt keine MIDI-CCs als Events, CC und Pitchbend als Output gehen nur über `LegacyMIDICCOutEvent`. JUCE 9 setzt CC- und Pitchbend-Output bei VST3 automatisch in diese Events um (geprüft im Quellcode: `juce_VST3Common.h`, `kLegacyMIDICCOutEvent`).
- **VST2:** keine Option. Steinberg lizenziert das VST2-SDK seit Jahren nicht mehr an neue Entwickler. Viele Foren-Workarounds („nimm die VST2-Version“) fallen damit weg.

### 2.2 Wie MIDI in FL zwischen Plugins fließt

FL hat **keine MIDI-Spuren** wie andere DAWs. Plugins tauschen MIDI über **nummerierte interne MIDI-Ports** aus (0–255) ([FL-Manual MIDI Out](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/plugins/MIDI%20Out.htm), [MIDI Settings](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/envsettings_midi.htm)):

1. Im Plugin-Wrapper (Zahnrad → „VST wrapper settings“) gibt es pro Plugin ein **MIDI Output Port** und ein **MIDI Input Port**.
2. Sender-Plugin: Output-Port auf z. B. 5 setzen. Empfänger-Synth im Channel Rack: Input-Port auf 5 setzen. Die Nummer ist frei wählbar, sollte aber eindeutig sein ([KVR](https://www.kvraudio.com/forum/viewtopic.php?t=380456), [CodeFN42 FAQ](https://www.codefn42.com/faq_routing_flstudio.html)).
3. Native FL-Synths (Sytrus, FLEX usw.) haben **keinen** Input-Port. Sie erreicht man nur über **Patcher**: dort im „From FL Studio“-Knoten „Outputs > Events > MIDI Port #“ hinzufügen und mit dem Event-Input des Synths verbinden ([KVR](https://www.kvraudio.com/forum/viewtopic.php?p=6629302), [IL-Forum April 2026](https://forum.image-line.com/viewtopic.php?p=2055182)).
4. In Patcher zeigen VST-Instrumente mit MIDI-Out einen **MIDI-Output-Knoten**, den man direkt an andere Plugins kabeln kann ([Scaler/FL-Guide](https://help.pluginboutique.com/hc/en-us/articles/6232371884052)).
5. **Aufnahme:** MIDI, das ein Plugin ausgibt, kann FL **nicht direkt in die Piano Roll aufnehmen**. Laut Scaler-Support braucht man Workarounds wie Patcher oder loopMIDI-Loopback ([Scaler](https://help.pluginboutique.com/hc/en-us/articles/6232371884052)). Nutzer berichten, dass FL die Noten zwar empfängt, aber nicht aufnimmt ([NI-Community](https://community.native-instruments.com/discussion/50027/how-to-send-midi-from-maschine-mk3-to-fl-studios-piano-roll)). **Folge für uns:** Wir brauchen eine eigene Capture-Funktion mit Drag & Drop (wie Dubler „MIDI Capture“ und Scaler).

### 2.3 Kann ein Effekt-Plugin im Mixer-Insert Audio empfangen und MIDI an Channel-Rack-Instrumente schicken?

**Kurzantwort:** Laut mehreren Forenberichten funktioniert das Port-Routing aus einem Mixer-Effekt heraus grundsätzlich: Output-Port im Effekt-Wrapper, Input-Port am Instrument ([KVR](https://www.kvraudio.com/forum/viewtopic.php?t=380456)). Für **VST3** gibt es aber widersprüchliche und negative Berichte. Für unser Ziel-Setup (FL 2026, JUCE-9-VST3-Effekt) ist das **(unbestätigt)** und muss getestet werden:

- Bei Scaler (JUCE-basiert) ging MIDI-Out-Routing in FL **nur mit der VST2-Version**. Ein Moderator schrieb, der VST3-Version fehle das entsprechende Settings-Menü ([Scaler-Forum](https://forum.scalermusic.com/t/midi-routing-fl-studio-vst3/60)). Die Berichte sind älter, aus der Zeit von FL 20/21.
- Ein JUCE-Arpeggiator als VST3 lud in FL nur als Effekt. Selbst über Patcher kam kein MIDI bei Sytrus an ([JUCE-Forum](https://forum.juce.com/t/arpeggiator-plugin-example-not-working-in-fl-studio/39381)).
- VST3 hat **keine Kategorie „MIDI-Effekt“**. Der übliche Weg ist ein „Instrument mit MIDI-Out“ ([JUCE-Forum](https://forum.juce.com/t/midi-generator-plugin-im-distributing-a-vst3i-could-i-distribute-a-vst3-midi-effect-too-for-ableton-cubase-fl-studio/65814)).
- Ein Nutzer berichtet von einem Effekt im Mixer ohne jede Wirkung: Nach Änderung des Output-Ports triggerte das Instrument nicht mehr ([itch.io](https://umlore.itch.io/probcurv/comments)).
- MIDI-Learn bzw. MIDI-Input für VST3-FX im Mixer scheint in FL problematisch zu sein ([HISE-Forum](https://forum.hise.audio/post/95394); dort gepostete Schritt-für-Schritt-Anleitungen waren KI-generiert und **nicht verlässlich**).
- **Audio an ein Generator-Plugin:** Einen Channel-Rack-Generator mit Mixer-Audio zu füttern (Sidechain an einen Generator), ist nicht sauber dokumentiert **(unbestätigt)**. Für Effekte ist Sidechain dagegen dokumentiert: „Sidechain to this track“ und Zuweisung im Wrapper-Tab „Processing“ ([Cherry-Audio-Forum](https://forums.cherryaudio.com/viewtopic.php?p=5018)).

### 2.4 Latenz und PDC in FL

- Automatische PDC ist Standard. Plugins melden ihre Latenz per `setLatencySamples()`. Pro Plugin lässt sich ein manueller Offset setzen (Wrapper > Settings > Latency). Automatische PDC funktioniert nicht bei Multi-Output-Plugins ([FL-Manual Mixer Track Properties](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/mixer_trackprops.htm)).
- **Wichtig für Live-Spiel:** PDC hilft beim Live-Singen nicht. Die Latenz zwischen Mikro und Synth ist ASIO-Input + Analysefenster + Synth + ASIO-Output. Für das Live-MIDI sollte das Plugin **0 Latenz melden**, sonst verschiebt FL die Spur. Die Analyse-Latenz kann man beim Capture-Export zeitlich herausrechnen (Noten um die bekannte Detektionsverzögerung nach vorn schieben).
- FL ruft `processBlock` mit **variablen Puffergrößen** auf, inklusive sehr kleiner Blöcke. Im Wrapper gibt es dazu Optionen wie „Use fixed size buffers“ **(unbestätigt, nicht verifiziert)**. Die Analyse muss deshalb intern eigene Hop-Größen puffern und darf nicht vom Host-Block abhängen.
- Seit FL 2026 werden eingehende MIDI-Events mit deutlich weniger Jitter aufgenommen ([FL-2026-Release-Notes](https://forum.image-line.com/viewtopic.php?p=2067080)).

### 2.5 Folgerungen und Pflicht-Spike vor der Architektur

Weil die Lage bei VST3-MIDI-Out in FL unklar ist, sollte VocalSynth **nicht allein** auf das Port-Routing setzen. Empfohlen werden **drei Ausgabewege**, abgesichert durch einen Testplan in Woche 1:

| Test | Aufbau in FL 2026 (Windows, ASIO) | Erwartung |
|---|---|---|
| T1 | VST3 als **Effekt** (`IS_SYNTH FALSE`, `NEEDS_MIDI_OUTPUT TRUE`) im Mixer-Insert, Output-Port 5 → VST3-Synth (z. B. Vital/Surge) Input-Port 5 | Noten, Pitchbend, CC kommen an? |
| T2 | wie T1, aber Empfänger ist ein nativer FL-Synth über Patcher | Patcher-Knoten sichtbar? |
| T3 | VST3 als **Instrument** (`IS_SYNTH TRUE`, MIDI-Out, plus Sidechain-Audio-Input) im Channel Rack, Mikro-Audio per Mixer-Sidechain | Audio kommt im Generator an? |
| T4 | ganzes Setup in **Patcher** auf dem Mikro-Insert: VocalSynth → MIDI-Kabel → Synth | robusteste Einzelspur-Lösung? |
| T5 | dieselben Tests mit dem **CLAP**-Build (clap-juce-extensions) | Note-Out in FL-Ports? |
| T6 | eingebaute Synth/Sampler-Engine im Plugin selbst (kein MIDI-Routing nötig) | funktioniert immer |
| T7 | virtueller MIDI-Port aus dem Plugin (JUCE `MidiOutput::createNewDevice`) → FL-MIDI-Input → normale Aufnahme in die Piano Roll | Aufnahme direkt möglich, mit etwas Jitter |

Die drei Ausgabewege:
1. **Plugin-MIDI-Out** über VST3/CLAP-Events für FL-Ports, Patcher und andere DAWs. Dieser Weg ist der Standard.
2. **Eingebaute Klangerzeugung**, ein einfacher Sampler/Synth als Fallback und für Beatbox → Drum-Samples. Dieser Weg funktioniert garantiert.
3. **Capture + Drag & Drop** von MIDI-Dateien und WAV in Piano Roll, Playlist oder Channel Rack. Optional kommt ein **virtueller MIDI-Port** dazu: Unter Windows erstellt JUCE 9 virtuelle Endpoints über **Windows MIDI Services**, das seit Februar 2026 in Windows 11 24H2/25H2 ausgerollt ist und Loopback/App-to-App ohne loopMIDI erlaubt ([Windows Blog](https://blogs.windows.com/windowsexperience/2026/02/17/making-music-with-MIDI-just-got-a-real-boost-in-windows-11/), [MS Known Issues](https://devblogs.microsoft.com/windows-music-dev/windows-midi-services-rollout-known-issues-and-workarounds/); JUCE-Quellcode `juce_Midi_windows.cpp`, `MidiVirtualDevice`). Unter macOS gibt es CoreMIDI-Virtual-Sources. Damit lässt sich die FL-Aufnahme von Plugin-MIDI umgehen. Achtung: Bei MIDI Services sind loopMIDI und loopBE laut Microsoft nicht zuverlässig sichtbar.

---

## 3. DSP- und ML-Algorithmen

### 3.1 Monophone Tonhöhenerkennung (f0)

Die Latenz hängt vor allem von der Fensterlänge ab: etwa 2 Perioden der tiefsten Frequenz plus der Hop. Bei 80 Hz (tiefe Männerstimme) ist eine Periode 12,5 ms lang, ein Fenster also ≥ 25 ms. Bei 44,1 kHz entspricht das ca. 1024–2048 Samples ([MIT-Report](https://fpga.mit.edu/videos/2024/team13/report.pdf), [DAFx14 Derrien](https://www.dafx14.fau.de/papers/dafx14_olivier_derrien_a_very_low_latency_pitch_.pdf)).

| Verfahren | Prinzip | Latenz | Genauigkeit | CPU | Bewertung für VocalSynth |
|---|---|---|---|---|---|
| **YIN** | Differenzfunktion + CMND + Schwelle + Parabel-Interpolation | ≈ Fenster (25–40 ms) + Hop | gut, mit Oktavfehlern | gering (FFT-YIN: O(N log N)) | **Basis-Kandidat** |
| **MPM (McLeod)** | normierte Quadrat-Differenz (NSDF), Key-Maxima | wie YIN, kommt mit wenig Perioden aus | ähnlich wie YIN, etwas schwächer bei tiefen Tönen | gering | **Basis-Kandidat**, laut [sevagh/pitch-detection](https://github.com/sevagh/pitch-detection) in der Praxis etwa gleichauf mit YIN |
| **pYIN** | YIN-Kandidaten + HMM/Viterbi | Viterbi braucht Lookahead (fixed-lag), +20–100 ms | robuster Voicing-Entscheid | mittel | nur mit kurzem Fixed-Lag oder für Capture-Nachbearbeitung |
| **SWIPE / SWIPE'** | Spektrum-Template | lange Fenster | sehr genau | hoch | eher offline |
| **CREPE** (full/tiny) | CNN auf 1024 Samples bei 16 kHz (64 ms Fenster) | ≥ 32–64 ms | sehr robust gegen Rauschen | hoch (full ≈ 22 M Parameter, tiny deutlich kleiner) | für Live zu träge und zu teuer |
| **SPICE** | selbstüberwachtes CNN (Google) | ähnlich wie CREPE | gut | mittel | wenig verbreitet |
| **RMVPE** | Mel-Spektrogramm + U-Net + BiGRU | kontextabhängig, für Offline gedacht | Stand der Technik (Gesang) | sehr hoch (ca. 90 M Parameter) | nur offline |
| **FCPE** | kontextbasiert, 10,6 M Parameter | GPU-optimiert | ≈ RMVPE | hoch | offline ([Review](https://www.themoonlight.io/fr/review/fcpe-a-fast-context-based-pitch-estimation-model)) |
| **SwiftF0** (2025, MIT) | kleines CNN auf Spektrogramm, 16 kHz, 16-ms-Frames, ca. 14 k Parameter | **Streaming braucht 176 ms Lookahead** | höchstes F1 im Pitch-Benchmark, gleichauf mit RMVPE | 180× Echtzeit auf 1 CPU-Kern | super für **Capture-Nachbearbeitung**, nicht live ([Repo](https://github.com/lars76/swift-f0), [Paper](https://arxiv.org/abs/2508.18440v1)) |

**Empfehlung:**
- **Live-Pfad:** YIN oder MPM (FFT-basiert), z. B. Fenster 1024 bei 48 kHz mit Downsampling auf 16–24 kHz, Hop 128–256 Samples. Voicing aus Confidence plus RMS-Gate. Den Frequenzbereich per Preset einschränken (Bass, Tenor, Sopran, Pfeifen), das senkt Fensterlänge und Oktavfehler.
- **Capture-Pfad:** Das Audio wird mit aufgenommen und nach dem Take mit SwiftF0 (ONNX) oder pYIN offline neu analysiert. Das ergibt sauberes „Clean MIDI“, also eine bessere Version dessen, was Dubler als „Clean“-Ansicht anbietet.

### 3.2 Onset-Erkennung (für Trigger und Noten-Neuanschlag)

| Verfahren | Eignung | Notizen |
|---|---|---|
| Energie / Hüllkurven-Differenz (Zeitbereich) | am schnellsten (< 3 ms) | anfällig für Fehlauslösungen, gut als erste Stufe |
| **HFC** (High Frequency Content) | sehr gut für perkussive Laute wie „ts“, „k“ und „p“ | billig |
| **Spectral Flux** (gleichgerichtet, log-komprimiert) | Allzweck-Standard | Hop 128–256 → 3–6 ms Auflösung |
| **SuperFlux** (Böck & Widmer 2013) | Max-Filter über Frequenz unterdrückt Vibrato-Fehlauslösungen | ideal bei Gesang mit Vibrato |
| Complex Domain | erkennt auch weiche, tonale Onsets | etwas teurer |

Peak-Picking im Online-Modus: adaptive Schwelle (Median/Mittelwert plus Delta) und eine Mindestpause (Refractory, ca. 30–50 ms). Lookahead höchstens 1–2 Frames.

### 3.3 Beatbox-Klassifikation (Few-Shot, nutzerspezifisch)

- **Literatur:** Nutzerspezifische Modelle schlagen allgemeine Modelle deutlich. Trainingssets haben typischerweise unter 100 Beispiele. Etabliert sind kNN auf MFCC plus spektralen Deskriptoren ([Ramires et al. / LVT](https://arxiv.org/pdf/2110.09223), [Deep Embeddings 2022](https://arxiv.org/pdf/2204.04646)). Eine Studie von 2025 fand MFCC (n = 22) als bestes Feature für kNN und SVM ([JCSSP 2025](https://thescipub.com/abstract/jcssp.2025.961.970)).
- **Zielkonflikt bei der Latenz:** Stowell et al. fanden, dass die Klassifikation besser wird, wenn das Analysefenster **ca. 23 ms nach dem Onset** beginnt ([arXiv 1811.02406](https://www.arxiv.org/pdf/1811.02406)). Für unsere Lösung bedeutet das:
  - Modus „Fast“: Klassifikation auf den ersten ~10–15 ms, Latenz ≈ 15 ms.
  - Modus „Accurate“: ~25–40 ms.
  - Hybrid: sofort Note-On für die wahrscheinlichste Klasse senden. Eine spätere Korrektur ist bei Drums nicht sinnvoll. Für die **Capture-Spur** wird die Klasse aber nachträglich korrigiert.
- **Vorgeschlagene Pipeline:** Onset (HFC + Flux) → Fenster 20–40 ms → Features: 13–20 MFCC + ΔMFCC, Spektral-Zentroid, -Flatness und -Rolloff, ZCR, Attack-Steilheit, Band-Energien → z-Normierung → **kNN (k = 3–5, Cosinus-Distanz)** oder lineare SVM. Dazu eine **„Unbekannt“-Klasse** über eine Distanzschwelle, damit Störgeräusche nicht triggern. Die Velocity kommt aus der Peak-Energie, kalibriert pro Klasse.
- **Ausbaustufe:** ein kleines CNN als Embedding-Netz, vortrainiert auf Datensätzen wie AVP/VPT, mit Prototypen-Klassifikation (Prototypical Networks), damit 3–5 Beispiele pro Sound reichen. Inferenz mit RTNeural (siehe 3.6).
- **UX:** Training wie bei Dubler: „Sound 5× wiederholen“, Konfusionsmatrix und Live-Konfidenz anzeigen, Profile speichern.

### 3.4 Vokal- und Formant-Tracking → CC

- **LPC:** Downsampling auf ca. 10–11 kHz, Pre-Emphasis, Hamming-Fenster 20–30 ms, Ordnung ≈ 2 + fs/1000 (ca. 12), Levinson-Durbin. Formanten über Nullstellen des Polynoms (Root-Finding) oder Peak-Picking der LPC-Hüllkurve. F1 und F2 werden über die Zeit geglättet (Median + One-Pole).
- **Mapping:** F1/F2 in eine 2D-Vokalebene (a, e, i, o, u). Daraus zwei CCs (z. B. „Offenheit“ ≈ F1, „Helligkeit“ ≈ F2) oder Morph-Gewichte für 3 Vokale wie bei Dubler. Die Kalibrierung erfolgt pro Nutzer: jeden Vokal einmal halten, die Referenzpunkte werden gespeichert.
- **Robuster und billiger:** Spektral-Zentroid oder MFCC 1–4 als „Brightness“-CC. Ein kNN- oder Gauß-Klassifikator auf MFCC liefert weiche Vokal-Gewichte.
- **Weitere CC-Quellen:**
  - Hüllkurve (RMS in dB, Attack/Release-Glättung) → CC/Velocity
  - Geräuschanteil (Spectral Flatness) → CC
  - Pitch-Abweichung → Pitchbend oder CC

### 3.5 Pitch → MIDI: Noten-Segmentierung

1. **Gate mit Hysterese:** Note-On, wenn RMS > Ton-Schwelle und Confidence > c_on über mindestens N Frames. Note-Off erst bei RMS < Off-Schwelle (z. B. 6 dB tiefer) oder Confidence < c_off.
2. **Glättung:** Median über 3–5 Frames gegen Oktavsprünge. Oktav-Fehlerkorrektur: einen Sprung um ±12 Halbtöne nur akzeptieren, wenn er stabil bleibt.
3. **Noten-Hysterese („Stickiness“):** Die neue Note nur übernehmen, wenn die Abweichung > 0,5 + h Halbtöne ist (h ≈ 0,1–0,4) **und** sie mindestens T_min (30–80 ms) anhält. Ein Regler steuert Latenz gegen Stabilität, und die UI zeigt die dadurch entstehende Latenz in ms an.
4. **Scale-Lock:** auf die nächste Skalenstufe quantisieren. Optional „Snap nur am Notenanfang, danach Bend“.
5. **Pitchbend (wie IntelliBend/TruBend):**
   - Bend-Residuum = f0 in Cents minus Notenmitte, Bend-Range einstellbar (±2 Halbtöne Standard, ±12/±48).
   - Modus „Anchored“: Totzone ±30–50 Cent, erst danach bendet es.
   - Modus „True“: 1:1.
   - Bend-Ausgabe mit ~200–500 Hz Rate und Glättung, sonst entsteht ein MIDI-Flood.
   - MPE optional später.
6. **Legato/Glide:** Bei Notenwechsel ohne Pause entweder überlappende Noten senden (Synths mit Legato/Portamento gleiten dann) oder „Retrigger“. Ein Schalter in der UI wählt zwischen beiden.
7. **Akkord-Modus:** Grundnote → Akkord aus Skalenstufen (diatonisch) oder fester Form. Voicings und Inversionen kommen aus einer Tabelle.
8. **Velocity:** aus der Onset-Energie der ersten 10–20 ms.

### 3.6 ML-Inferenz im Plugin

| Engine | Lizenz | Stärken | Schwächen | Einsatz |
|---|---|---|---|---|
| **RTNeural** | BSD-3 | header-only, Echtzeit-sicher (keine Allokation), Modelle zur Compile-Zeit, Eigen/xsimd | nur Dense, GRU, LSTM, Conv1D/2D und einfache Layer | **Live-Klassifikator/CNN im Audio- oder Analyse-Thread** ([Repo](https://github.com/jatinchowdhury18/RTNeural)) |
| **ONNX Runtime** | MIT | beliebige Modelle, bei stateless Modellen am schnellsten (laut anira-Paper) | große DLL (zweistelliger MB-Bereich), allokiert, nicht Echtzeit-sicher | **Hintergrund-Thread**, z. B. SwiftF0 offline |
| **LibTorch** | BSD | schnellste Engine für stateful Modelle (laut anira) | Binärgröße mehrere 100 MB | ungeeignet für die Distribution |
| **anira** | Apache-2.0 | Wrapper für ONNX, LibTorch und TFLite mit statischem Thread-Pool, entschärft Echtzeit-Verletzungen | zusätzliche Abhängigkeit | falls ONNX nah an Echtzeit laufen soll ([Paper](https://arxiv.org/pdf/2506.12665)) |

Benchmarks unter realer DAW-Last weichen von isolierten Messungen ab ([DAFx26](https://dafx.de/paper-archive/details/rBxXewYH3YxRTiGXqSabwQ)). Das eigene Modell sollte deshalb unter Last in FL gemessen werden. Ein Vergleich auf Embedded-Hardware fand, dass alle Engines Echtzeit schaffen, wenn sie richtig eingesetzt werden ([DAFx Embedded](https://www.dafx.de/paper-archive/details/qcQo4dIbC0k5_sjLxWUmKA)).

**Fazit:** Version 1 kommt ohne Neuronale Netze aus (YIN/MPM, kNN). Version 2 bringt ein RTNeural-Embedding-CNN für Trigger und optional SwiftF0 per ONNX für die Capture-Nachanalyse.

---

## 4. Tech-Stack

### 4.1 JUCE (aktuell JUCE 9)

- **Version:** JUCE **9.0.4** vom 12.10.2026. JUCE 9.0.0 erschien am 21.07.2026 (Git-Tags geprüft). Laut KVR sind Preise und Lizenz identisch mit JUCE 8, JUCE-8-Abos wurden automatisch auf 9 hochgestuft ([KVR](https://www.kvraudio.com/news/juce-9-now-available-67802)). Für uns relevant: 9.0.3 hat Windows-MIDI-Services auf die In-Box-API aktualisiert, 9.0.4 hat Fixes bei MIDI-Disconnects.
- **Lizenz:** JUCE ist **dual lizenziert unter AGPLv3 oder der kommerziellen JUCE-9-EULA** (`LICENSE.md` im Repo). Der GPL-Modus und die Splash-Screen-Pflicht sind seit JUCE 8 weggefallen ([JUCE-Forum](https://forum.juce.com/t/juce-8-is-available-now/61809)).
- **Kommerzielle Stufen (JUCE-8-Konditionen, für JUCE 9 übernommen):**
  - **Starter:** kostenlos bis 20.000 USD Jahresumsatz bzw. Funding
  - **Indie:** bis 300.000 USD, 40 USD/Monat oder 800 USD perpetual
  - **Pro:** unbegrenzt, 175 USD/Monat pro Nutzer oder 3.500 USD perpetual
  - Quelle: [EULA-Änderungen](https://forum.juce.com/t/amendments-to-the-juce-end-user-licence-agreement-for-juce-8/61265), [Revenue Limits](https://forum.juce.com/t/revenue-limits-for-juce-tiers/61058)
- **Für VocalSynth:** Ein Open-Source-Projekt unter **AGPLv3** ist ohne JUCE-Kosten möglich. AGPL ist mit der MIT-Lizenz des VST3-SDK und mit BSD/MIT/Apache-Bibliotheken kompatibel. Die Konsequenz: Das gesamte Plugin ist AGPLv3, und wer Binaries verteilt, muss den Quellcode bereitstellen. Wenn das Projekt eine permissive Lizenz wie MIT anstrebt, kommt JUCE nicht in Frage, oder jeder Distributor braucht eine JUCE-Lizenz.
- **CMake:**
  - `juce_add_plugin(... FORMATS VST3 AU Standalone NEEDS_MIDI_INPUT FALSE NEEDS_MIDI_OUTPUT TRUE IS_SYNTH <TRUE|FALSE> IS_MIDI_EFFECT FALSE VST3_AUTO_MANIFEST TRUE)`, Optionen geprüft in `docs/CMake API.md`.
  - Weil VST3 keinen MIDI-Effekt-Typ kennt, setzen wir **zwei Targets aus demselben Code** auf: „VocalSynth FX“ (Effekt mit Audio-In und MIDI-Out) und „VocalSynth Instrument“ (Synth mit Sidechain-Input und MIDI-Out). Welches in FL besser funktioniert, entscheidet der Spike in Abschnitt 2.5.
- **CLAP:** JUCE 9 hat **keinen nativen CLAP-Export**. Dafür gibt es [clap-juce-extensions](https://github.com/free-audio/clap-juce-extensions) (MIT, aktiv, letzter Commit 06.10.2026), das in pamplejuce bereits eingebunden ist.

### 4.2 Alternativen

| Framework | Sprache | Lizenz | Formate | Bewertung |
|---|---|---|---|---|
| **JUCE 9** | C++ | AGPLv3 / kommerziell | VST3, AU, AAX, LV2, Standalone (+ CLAP über Extension) | größtes Ökosystem, beste Doku, MIDI-Device-API und Drag & Drop eingebaut. **Empfohlen.** |
| **iPlug2** | C++ | liberal (WDL-artig) | VST2/3, AU, AAX, CLAP, WAM (Web) | permissiv, kleinere Community, aktiv (letzter Commit 08/2026) |
| **DPF** (DISTRHO) | C++ | ISC | VST2/3, CLAP, LV2, JACK | schlank, Linux-stark, GUI-Toolkit einfacher, letzter Commit 10/2025 |
| **nih-plug** | Rust | ISC, aber **VST3-Export über GPLv3-Bindings** | VST3, CLAP | modern und sicher. Für unser Projekt passen die GPL-Bindings zu einer Open-Source-Lizenz. Das ML- und DSP-Ökosystem in C++ ist breiter. Aktiv (09/2026). |
| **CLAP SDK** pur | C | MIT | CLAP | nur als Zusatzformat |

- **VST3-SDK:** **MIT-Lizenz seit VST 3.8 (29.10.2025)**, vorher Dual GPLv3/proprietär ([Steinberg-Pressemitteilung](https://ocl-steinberg-live.steinberg.net/_storage/asset/819253/storage/master/Press%20Release%20-%202025-10-29%20-%20VST%203.8%20-%20EN.pdf), [Sonicstate](https://sonicstate.com/news/2025/10/30/vst-3-now-available-under-mit-license)). JUCE bündelt das SDK in `juce_audio_processors_headless/format_types/VST3_SDK/LICENSE.txt` (MIT, geprüft).
- **ASIO-SDK:** jetzt GPLv3 oder proprietär. Für das Standalone-Target unter Windows ist das relevant, für das Plugin nicht.

### 4.3 Projekt-Template, Validierung, CI und Signierung

- **pamplejuce** ([Repo](https://github.com/sudara/pamplejuce), letzter Commit 16.08.2026) bringt mit:
  - C++23, **JUCE 9 als Submodule**, CPM, CMake ≥ 3.25, Catch2-Tests und Benchmarks
  - clap-juce-extensions, Melatonin Inspector
  - GitHub Actions für Linux, macOS und Windows
  - **pluginval** in der CI, macOS-Signierung und Notarisierung, Windows-Signierung über Azure
  - Workflows: `build_and_test.yml`, `nightly.yml`
  - Achtung: Die Tests laufen rot, bis die Signierung konfiguriert ist. Für ein Open-Source-Projekt sollte man sie per Bedingung „Signing nur wenn Secrets gesetzt“ abschwächen.
- **pluginval** (Tracktion, aktiv, Commit vom 12.10.2026): in der CI mit `--strictness-level 10` für VST3 und AU. Zusätzlich den Steinberg-`validator` aus dem VST3-SDK und `auval` auf macOS laufen lassen.
- **Windows-Signierung:** Azure Trusted Signing heißt jetzt **„Artifact Signing“**. Preise: 9,99 USD/Monat für bis zu 5.000 Signaturen, Premium 99,99 USD ([Microsoft](https://azure.microsoft.com/en-us/products/artifact-signing)). Laut JUCE-Forum fiel im Oktober 2025 die Pflicht zu drei Jahren Firmenhistorie weg; zugelassen sind Organisationen in USA, Kanada, EU und UK ([JUCE-Forum](https://forum.juce.com/t/azure-code-signing-for-plugin-developers-guide/60391?page=6)). Ob Einzelpersonen teilnehmen können, ist **(unbestätigt)**. Für frühe Open-Source-Releases geht es auch unsigniert, dann warnt SmartScreen.
- **macOS:** Ein Apple Developer Account (99 USD/Jahr) ist für Signierung und Notarisierung nötig. Ohne ihn blockiert Gatekeeper AU und VST3. Eine Anleitung gibt es bei [melatonin.dev](https://melatonin.dev/blog/how-to-code-sign-and-notarize-macos-audio-plugins-in-ci/).

---

## 5. Empfehlung

### 5.1 Stack

- **C++23 + JUCE 9 (AGPLv3) + CMake über das pamplejuce-Template.** Formate: VST3 (primär), CLAP (über clap-juce-extensions), AU und Standalone (zum Debuggen und für Live-Nutzung mit virtuellem MIDI-Port).
- **DSP:** eigene YIN/MPM-, Onset-, LPC- und MFCC-Implementierungen. FFT über `juce::dsp::FFT`, optional PFFFT. Test-Fixtures mit Referenzaufnahmen in Catch2.
- **ML:** Version 1 nutzt kNN und SVM ohne Abhängigkeiten. Version 2 bringt RTNeural (Trigger-Embedding) und optional ONNX Runtime im Hintergrund-Thread (SwiftF0 für Capture).
- **Lizenz des Projekts:** AGPLv3, die einfachste mit JUCE kompatible Lizenz. Alternative bei Wunsch nach permissiver Lizenz: iPlug2 oder DPF, mit mehr Eigenarbeit.

### 5.2 Architektur

```
Audio-Thread (processBlock, keine Allokation, keine Locks)
  ├─ Input-Konditionierung: DC-Filter, HPF 60 Hz, optionales Noise-Gate
  ├─ Ringpuffer (SPSC, lock-free) ──► Analyse-Hop-Framer (eigene Hopgröße, unabhängig vom Host-Block)
  ├─ Analyse innerhalb des Audio-Threads (deterministisch, ~<10 % Blockbudget):
  │     Onset (HFC/Flux) · YIN/MPM · RMS · LPC/MFCC · kNN
  │     → Event-Generator (Gate/Hysterese, Scale-Lock, Akkorde, Bend, CC-Glättung)
  ├─ MIDI-Out: sample-genau in den MidiBuffer (Note, PB, CC → VST3 LegacyCCOut)
  ├─ Optional: interne Klangerzeugung (Sampler für Trigger, einfacher Synth)
  └─ Capture: Audio + Events ──► lock-free FIFO ──►

Hintergrund-Thread(s)
  ├─ Capture-Writer: WAV schreiben (juce::AudioFormatWriter::ThreadedWriter), MIDI-Events puffern
  ├─ Offline-Nachanalyse eines Takes (pYIN/SwiftF0) → „Clean MIDI“, Latenz-Kompensation
  ├─ Trigger-Training: Features sammeln, Modell neu bauen → atomarer Pointer-Swap ins Audio-Thread-Modell
  └─ Optional: virtueller MIDI-Port (MidiOutput::createNewDevice), gespeist über FIFO

Message-/GUI-Thread
  ├─ UI: Tuner/Pitch-Kurve, Trigger-Pads mit Konfidenz, Vokal-XY, Latenzanzeige
  ├─ Parameter (AudioProcessorValueTreeState), Profile (ValueTree/JSON)
  └─ Drag & Drop: performExternalDragDropOfFiles() für .mid und .wav aus temporären Dateien
```

**Wichtige Designentscheidungen:**
- Die **Live-Analyse läuft im Audio-Thread**. Ein separater Analyse-Thread brächte Jitter und mindestens einen Block Latenz. Sie muss deshalb billig und allokationsfrei sein: Puffer vorallokieren und in `prepareToPlay` dimensionieren. Nur teure Arbeit (Training, Nachanalyse, Dateien) läuft im Hintergrund.
- **Lock-free:** SPSC-FIFOs (`juce::AbstractFifo` oder eigene), atomare Parameter, Modell-Updates per `std::atomic<std::shared_ptr>` oder Double-Buffer mit Freigabe außerhalb des Audio-Threads.
- **Latenz melden:** 0 für den Live-Betrieb. Die Detektionsverzögerung ist bekannt und wird beim Export herausgerechnet.
- **Sample-Rate:** intern auf 16 bzw. 24 kHz für Pitch und LPC heruntersamplen, über einen Polyphase-Halbband-Filter mit geringer Latenz.
- **Drag & Drop nach FL:** WAV- und MIDI-Dateien in einen temporären oder Projektordner schreiben und dann per `DragAndDropContainer::performExternalDragDropOfFiles` übergeben. FL nimmt MIDI-Drags in Patterns und WAV-Drags in Playlist oder Channel Rack an ([Scaler-Drag-Thread](https://forum.image-line.com/viewtopic.php?p=1999180)). Zusätzlich einen „Speichern unter“-Button anbieten.
- **Sample-Erzeugung:** Ein Ein-Tasten-Recorder schneidet automatisch an den Onsets, normalisiert und erzeugt einen Fade. So entsteht ein Kit aus eigenen Beatbox-Sounds als WAV-Ordner (Dubler kann das nicht).

### 5.3 Roadmap

1. **M0 (1–2 Wochen):** pamplejuce-Setup, CI mit pluginval, FL-Spike T1–T7 aus Abschnitt 2.5. Danach fällt die Entscheidung zwischen FX- und Instrument-Target sowie über das Routing-Rezept für die Doku.
2. **M1:** Pitch → MIDI (YIN/MPM, Gate, Hysterese, Scale-Lock, Pitchbend), Latenzanzeige.
3. **M2:** Onset + Beatbox-Trigger mit Training (kNN), interner Drum-Sampler.
4. **M3:** Capture (MIDI + WAV) mit Drag & Drop, Clean-Nachanalyse.
5. **M4:** Vokal/Formant → CC, Akkord-Modus, CLAP/AU, Signierung.
6. **M5:** Neuronale Netze (RTNeural-Embedding, SwiftF0 offline), MPE.

### 5.4 Hauptrisiken

| Risiko | Auswirkung | Gegenmaßnahme |
|---|---|---|
| **VST3-MIDI-Out in FL funktioniert nicht oder nur eingeschränkt** (Berichte zu Scaler und JUCE-Arpeggiator) | Kern-Use-Case gefährdet | Spike in M0. Fallbacks: interne Klangerzeugung, Patcher-Rezept, virtueller MIDI-Port, Capture mit Drag & Drop, CLAP-Build |
| Aufnahme von Plugin-MIDI in FL nicht möglich | Nutzer erwarten „Record“ | eigener Capture-Recorder mit Drag & Drop, virtueller Port → FL-MIDI-Input |
| Gesamtlatenz (ASIO-I/O + Fenster ≥ 25 ms) | fühlt sich träge an, besonders bei tiefen Stimmen | Range-Presets, MPM mit kurzen Fenstern, Onset-getriggerter Note-On mit vorläufiger Tonhöhe und anschließendem Bend/Korrektur, ASIO-Leitfaden in der Doku |
| Oktavfehler, „Spaghetti-MIDI“ | schlechte UX (wie die Kritik an Dubler) | Hysterese, Median, Oktav-Lock, Clean-Nachanalyse |
| Fehlklassifikation bei Triggern, Übersprechen aus Lautsprechern | falsche Drums | „Unbekannt“-Klasse, Kopfhörer-Hinweis, Konfidenzanzeige, Training mit mehreren Beispielen |
| FL ruft mit variablen oder winzigen Blöcken auf | Artefakte, CPU-Spitzen | eigener Hop-Framer, feste interne Blockgröße |
| AGPL-Pflichten (JUCE) | Einschränkung für Forks und kommerzielle Nutzung | Lizenz früh festlegen und in README und CONTRIBUTING dokumentieren |
| Kosten und Verfügbarkeit von Code-Signing (Azure nur für Organisationen?) | Warnungen unter Windows und macOS | zuerst unsignierte Releases mit Doku. Später Signierung über Verein oder Firma oder einen Sponsor |
| ML-Abhängigkeiten (ONNX-Größe, Modelllizenzen wie NeuralNote-Gewichte CC BY-NC) | Distributions- und Lizenzprobleme | nur MIT/BSD-Modelle (SwiftF0 ist MIT), RTNeural bevorzugen |
