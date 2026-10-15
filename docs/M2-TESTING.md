# Singen → MIDI testen (Meilenstein 2, Version 0.2.0)

Mit **VocalSynth 0.2.0** wird deine Stimme zum ersten Mal zu MIDI: Du singst oder summst ins Mikrofon, und
VocalSynth schickt die erkannten Noten (und auf Wunsch Pitchbend) über den MIDI-Port von FL Studio an einen Synth.
Diese Anleitung erklärt die Einrichtung in FL Studio 2026, jedes Bedienelement und eine Checkliste. Am Ende steht,
was du zurückmelden sollst.

Es ist der Weg, der in M1 funktioniert hat (Test T1): **VocalSynth FX auf dem Mikro-Insert → MIDI Output Port →
VST3-Synth mit gleichem Input Port.** Die Bedienelemente im Plugin sind englisch beschriftet, Menünamen von FL so,
wie sie in der englischen Oberfläche heißen. Wo ein Schritt nicht sicher belegt ist, steht **(unbestätigt)** dabei.

Zeitbedarf: ca. 45–60 Minuten.

> **Wichtig:** FL Studio nimmt MIDI, das ein Plugin ausgibt, **nicht** in die Piano Roll des Synths auf (in M1
> festgestellt). Du hörst die Noten live, kannst sie aber noch nicht aufnehmen. Die eigene Aufnahme mit Drag & Drop
> nach FL kommt in Meilenstein 3. Wenn du einen Take festhalten willst: Audio des Synths aufnehmen.

---

## Inhalt

1. [Installation](#1-installation)
2. [Routing in FL Studio](#2-routing-in-fl-studio)
3. [Das Plugin-Fenster](#3-das-plugin-fenster)
4. [Erste Schritte (5 Minuten)](#4-erste-schritte-5-minuten)
5. [Test-Checkliste](#5-test-checkliste)
6. [Was du zurückmelden sollst](#6-was-du-zurückmelden-sollst)
7. [Bekannte Einschränkungen von 0.2.0](#7-bekannte-einschränkungen-von-020)

---

## 1. Installation

1. Im [Actions-Tab](https://github.com/Tr3nben/VocalSynth/actions) den neuesten grünen Lauf öffnen und unten bei
   „Artifacts“ **`VocalSynth-0.2.0-Windows-<Commit>.zip`** herunterladen. Die Commit-Kennung aus dem Dateinamen
   notieren, sie gehört in deine Rückmeldung.
2. ZIP komplett entpacken, **`Install-VocalSynth.bat`** doppelklicken, die Administrator-Abfrage bestätigen und mit
   Enter in den Standard-Ordner installieren (oder deinen eigenen VST3-Ordner angeben). Die Version 0.1.0 wird dabei
   überschrieben. Details: `LIESMICH.txt` im ZIP.
3. FL Studio öffnen: *Options → Manage plugins → Find installed plugins*.
4. Nach dem Laden prüfen: Oben im Plugin-Fenster steht **„Version 0.2.0 – M2 pitch to MIDI“**. Steht dort noch
   0.1.0, hat FL die alte Datei geladen (FL neu starten, Plugin-Manager erneut suchen lassen).

**Audio-Einstellungen** (*Options → Audio settings*): ASIO-Treiber deines Interfaces (oder „FL Studio ASIO“),
Puffer **128–256 Samples**. Notiere Samplerate und Puffergröße. Kopfhörer benutzen, damit der Synth nicht ins
Mikrofon zurückläuft (sonst „hört“ VocalSynth den Synth und spielt ihn nach).

---

## 2. Routing in FL Studio

1. **Mikrofon auf einen Mixer-Insert:** Im Mixer einen freien Insert wählen (z. B. Insert 1) und oben rechts im
   Mixer bei *Audio input source* den Eingang deines Mikrofons wählen. Der Pegel des Inserts muss sich beim Singen
   bewegen.
2. **VocalSynth FX auf diesen Insert laden:** In einem freien Effekt-Slot des Inserts *VocalSynth FX* wählen
   (unter den Effekten, nicht unter den Generatoren).
3. **MIDI Output Port setzen:** Im VocalSynth-Fenster das Zahnrad oben links öffnen (*VST wrapper settings*) →
   Bereich **MIDI** → **Output port** auf z. B. **1** stellen.
4. **Synth laden:** Im Channel Rack einen VST3-Synth mit MIDI-Eingang laden, z. B. **Vital** oder **Surge XT**
   (beide kostenlos). Native FL-Synths (FLEX, Sytrus, 3x Osc) haben keinen Input-Port und gehen nur über Patcher
   (in M1 nicht getestet).
5. **Gleichen Input Port beim Synth:** Im Fenster des Synths Zahnrad → *VST wrapper settings* → **MIDI** →
   **Input port** auf denselben Wert (**1**).
6. **Pitchbend-Bereich des Synths:** Nur wichtig, wenn du *Bend Mode* benutzt. Der Wert **Bend Range** in
   VocalSynth muss zum Pitchbend-Bereich des Synths passen. Vital und Surge stehen meist auf ±2 Halbtöne
   **(unbestätigt, im Synth nachsehen)**. Wenn beide ±2 haben, passt es.
7. **Smart disable** für VocalSynth ausschalten, falls aktiv (Menü oben links im Plugin-Fenster → *Smart disable*).
   Sonst kann FL das Plugin schlafen legen.

Damit spielst du den Synth mit der Stimme. Der Synth muss nicht ausgewählt oder „armed“ sein.

---

## 3. Das Plugin-Fenster

Oben stehen Name, Version und Format (VST3 oder CLAP). Darunter zwei Reiter:

- **Voice**: der Hauptbereich für Singen → MIDI (Stimmgerät, Einstellungen, Pegel).
- **Test & Routing**: alles aus dem M1-Testbuild (Testmuster, MIDI-Kanal, Monitor-Ton, virtueller Port, Zähler,
  Blockgrößen, Test-Clips). Siehe [FL-ROUTING.md](FL-ROUTING.md).

Alle Einstellungen sind normale Plugin-Parameter: Sie werden mit dem FL-Projekt gespeichert und lassen sich
automatisieren.

### 3.1 Anzeigen im Reiter „Voice“

| Anzeige | Bedeutung |
|---|---|
| **Stimmgerät** (großer Notenname) | Die Note, die du gerade singst, benannt wie in der Piano Roll von FL (**C5 = MIDI-Note 60**, A5 = 440 Hz). Daneben die Frequenz in **Hz** und die Abweichung in **cents**. |
| **Nadel** (−50 … +50 cents) | Grün: höchstens 10 cents daneben. Orange: weiter daneben. Ohne Ton keine Nadel. |
| **Confidence** | Wie sicher sich die Tonhöhenerkennung ist (Balken 0–100 %). Bei Atem, Zischlauten und Rauschen bleibt er niedrig. |
| **Voiced** (Lämpchen) | Leuchtet, wenn VocalSynth deine Stimme als „gesungener Ton“ zählt (laut genug und sicher genug erkannt). Nur dann entstehen Noten. |
| **Sounding note** | Die Note, die gerade als MIDI gesendet wird (mit Oktav-Verschiebung). Kann vom Stimmgerät abweichen: Skala, Stickiness, Octave. |
| **Latency** | Rechnerische Verzögerung der Analyse vom Tonbeginn bis zur Note-On, für die gewählte Stimmlage und Samplerate (Tenor ca. 24 ms, Bass/Wide ca. 39 ms). Dazu kommen der ASIO-Puffer und der Synth. Die Verzögerung wird FL nicht gemeldet; das Audio wird nicht verzögert. |
| **Input-Pegel** mit **Gate-Linie** | Breiter Balken: Spitzenpegel am Eingang. Schmaler Balken unten: der Pegel, den das Gate sieht (grün = über dem Gate). Weiße Linie: die Gate-Schwelle. |

Steht *Source* auf *Test Pattern*, ist das Stimmgerät grau: Dann wird die Stimme nicht analysiert.

### 3.2 Einstellungen im Reiter „Voice“

| Element | Werte (Standard fett) | Was es macht |
|---|---|---|
| **Source** | **Voice**, Test Pattern | *Voice*: deine Stimme erzeugt das MIDI. *Test Pattern*: das feste Test-MIDI aus M1 (Muster im Reiter *Test & Routing*). Beim Umschalten werden alle Noten beendet. |
| **Voice Range** | Bass, Baritone, **Tenor**, Alto, Soprano, Whistle, Wide | Deine Stimmlage, wie „Input Type“ bei Auto-Tune. Begrenzt die Tonhöhensuche: Bass 65–330 Hz, Baritone 82–392 Hz, **Tenor 110–523 Hz** (A3–C6 in FL-Notennamen), Alto 147–698 Hz, Soprano 196–1047 Hz, Whistle 500–2500 Hz (Pfeifen), Wide 65–1047 Hz (alle Stimmen). Tiefere Lagen brauchen ein längeres Analysefenster und haben mehr Latenz. Töne außerhalb der Lage werden ignoriert. |
| **Algorithm** | YIN, **MPM** | Zwei Verfahren zur Tonhöhenerkennung. MPM ist Standard, weil es in unseren synthetischen Tests etwas schneller und sauberer war. Beide ausprobieren; sag uns, welches bei deiner Stimme sauberer ist. |
| **Key** | **C**, C#, …, B | Grundton der Skala. |
| **Scale** | **Chromatic**, Major, Natural Minor, Harmonic Minor, Major Pentatonic, Minor Pentatonic, Blues, Dorian, Mixolydian | Die gesungene Tonhöhe wird auf die nächste Note dieser Skala gerundet. *Chromatic* = jeder Halbton ist erlaubt. |
| **Bend Mode** | **Off**, Anchored, True | *Off*: nur Noten. *Anchored*: kleine Abweichungen (±30 cents) werden ignoriert, erst bewusstes Wegziehen erzeugt Pitchbend. *True*: Pitchbend folgt der Stimme 1:1 (Vibrato und Schleifer hörbar). |
| **Bend Range** | **±2**, ±12, ±24, ±48 | Pitchbend-Bereich in Halbtönen. Muss zur Einstellung im Synth passen, sonst stimmen die Bends nicht. |
| **Stickiness** | 0–100 %, **50** | Wie fest eine Note „klebt“, bevor zur nächsten gewechselt wird. Mehr = stabilere Noten (Vibrato bleibt eine Note), aber Notenwechsel kommen später (bei 50 % ca. 35 ms, bei 100 % ca. 60 ms). Weniger = schneller, aber mehr Wackel-Noten. |
| **Gate** | −70 … −10 dB, **−45** | Unter diesem Pegel entsteht keine Note. Höher stellen, wenn Raumgeräusche oder Atem Noten auslösen; tiefer, wenn leises Singen nicht erkannt wird. Die Linie im Pegelbalken zeigt den Wert. |
| **Min Note** | 20–250 ms, **60** | Mindestlänge einer Note. Kürzere Wackler werden der vorigen Note zugeschlagen. Verzögert den Notenbeginn nicht. |
| **Velocity Sens.** | 0–100 %, **70** | 0 % = jede Note mit Velocity 100. 100 % = Velocity folgt ganz der Lautstärke am Notenanfang. |
| **Octave** | −3 … +3, **0** | Verschiebt die MIDI-Noten um ganze Oktaven (z. B. +1, um mit Tenorstimme Lead-Sounds eine Oktave höher zu spielen). |
| **Legato** | **aus**, an | Aus: bei jedem Notenwechsel erst Note-Off, dann Note-On (Synth schlägt neu an). An: neue Note beginnt vor dem Ende der alten, damit Mono-Synths im Legato-/Glide-Modus gleiten. |

### 3.3 Reiter „Test & Routing“

Wie in M1 (siehe [FL-ROUTING.md, Abschnitt 2](FL-ROUTING.md#2-das-testmenü-im-plugin)). Für M2 wichtig:

- **MIDI channel**: Kanal 1–16, gilt für die Stimme **und** das Testmuster.
- **Monitor Tone**: eingebauter Sinuston, der die gesendeten Noten mitspielt, bei *Source = Voice* also deine
  gesungenen Noten (mit Pitchbend). Gut zum Testen ganz ohne Synth und Routing.
- **Mute Dry**: schaltet dein Mikrofon-Signal auf dem Insert stumm (der Monitor-Ton bleibt hörbar).
- **MIDI sent**: Zähler der gesendeten Note-On/Off, Pitchbends und CCs. Steigt der Zähler, aber der Synth bleibt
  stumm, liegt es am Routing; steigt er nicht, hat VocalSynth nichts erkannt.
- **Test pattern** / **Restart pattern**: nur bei *Source = Test Pattern*. Damit prüfst du schnell, ob das Routing
  noch stimmt.

---

## 4. Erste Schritte (5 Minuten)

1. Routing wie in Abschnitt 2. Im VocalSynth-Fenster Reiter **Voice**: *Source* = **Voice**,
   *Voice Range* = **Tenor**, alles andere auf Standard.
2. Einen langen Ton auf „a“ oder „u“ singen, mittlere Lautstärke, etwa A3 (220 Hz). Erwartung: Das Lämpchen
   *Voiced* leuchtet, das Stimmgerät zeigt **A4** (FL-Notenname von 220 Hz, MIDI-Note 57), der Synth spielt die
   Note, solange du singst, und hört auf, wenn du aufhörst.
3. Kommt nichts beim Synth an: Reiter *Test & Routing* → *MIDI sent* beobachten. Steigt *note-on*, ist das Routing
   schuld (Ports prüfen, siehe Abschnitt 2). Steigt nichts: Pegel und Gate-Linie im Reiter *Voice* ansehen – der
   schmale Balken muss beim Singen rechts von der weißen Linie liegen.
4. Ohne Synth testen: im Reiter *Test & Routing* **Monitor Tone** einschalten. Dann spielt VocalSynth deine Noten
   selbst als Sinuston (zusammen mit deiner Stimme; mit **Mute Dry** nur der Sinuston).

---

## 5. Test-Checkliste

Wenn nicht anders angegeben: *Source = Voice*, *Voice Range = Tenor*, *Algorithm = MPM*, Rest Standard. Ergebnis
pro Punkt notieren: **o.k.** / **teilweise** / **nein**, plus Bemerkung. Die Punkte 1–7 zuerst mit MPM, dann
die wichtigsten (1, 2, 3, 7) noch einmal mit **YIN**.

| Nr. | Test | So geht's | Erwartung |
|---|---|---|---|
| 1 | **Einzelne Töne** | Lange Töne (2–3 s) auf verschiedenen Höhen deines Bereichs singen, mit Pause dazwischen. | Pro Ton genau eine Note, richtige Tonhöhe (Stimmgerät = Sounding note), keine falsche Oktave, Note endet mit dem Ton. |
| 2 | **Tonleitern** | C-Dur auf und ab singen (legato und abgesetzt), dann *Key = C*, *Scale = Major* stellen und noch einmal, auch absichtlich etwas daneben. | Jede Stufe eine saubere Note, keine kurzen Zwischennoten. Mit *Major* landen schiefe Töne auf der nächsten Dur-Note. |
| 3 | **Vibrato** | Einen Ton mit kräftigem Vibrato halten. Dann *Bend Mode = True* und wiederholen. | Mit *Off*: eine einzige Note ohne Flattern. Mit *True*: weiter eine Note, das Vibrato hört man über Pitchbend. |
| 4 | **Legato** | Eine schnelle Melodie mit gebundenen Tönen singen, *Legato* aus und an. Am besten mit einem Mono-Synth mit Glide. | Aus: jeder Ton schlägt neu an. An: Töne gehen ineinander über (Glide), keine hängenden Noten. |
| 5 | **Bend-Modi** | Ton halten und langsam ca. einen Halbton hoch- und zurückziehen: *Off*, *Anchored*, *True*. Danach *Bend Range* = ±12 (im Synth auch ±12 einstellen) und *True* wiederholen. | *Off*: keine Bends. *Anchored*: kleine Wackler ohne Bend, deutliches Ziehen biegt den Ton. *True*: folgt genau. Mit ±12 in beiden stimmt die Tonhöhe weiterhin. Nach dem Notenende springt das Pitch-Rad des Synths zurück in die Mitte. |
| 6 | **Oktave** | *Octave* = +1, dann −1, während du einen Ton hältst und zwischen Tönen. | Die MIDI-Note wechselt um 12 Halbtöne, keine hängende Note. |
| 7 | **Atem und Geräusche** | Atmen, laut ausatmen, „sch“, „s“, „f“, „t“, „k“, Räuspern, Klatschen, Tippen am Tisch, Raum ganz still. | **Keine** Noten (oder höchstens ganz selten eine sehr kurze). Wenn doch: *Gate* höher stellen und notieren, ab welchem Wert es ruhig ist. |
| 8 | **Latenz-Gefühl** | Kurze, abgesetzte Töne („da da da“) im Takt eines Metronoms singen, ASIO-Puffer 128 und 256. | Fühlt sich das spielbar an? Notiere den Wert der Anzeige *Latency* und deinen Eindruck (z. B. „merkbar, aber o.k.“). |
| 9 | **Stimmlagen** | *Voice Range* auf **Baritone** und **Wide** umstellen, ein paar Töne singen; bei Interesse auch *Whistle* mit Pfeifen. | Funktioniert weiter; die angezeigte Latenz ändert sich (Wide/Bass ca. 39 ms). Wide neigt eher zu Oktavfehlern. |
| 10 | **Stickiness** | *Stickiness* auf 0 und auf 100, Tonleiter und Vibrato singen. | 0: schnellere Wechsel, eher Wackel-Noten. 100: sehr stabil, Wechsel spürbar später. |
| 11 | **Keine hängenden Noten** | Während eine Note klingt: Plugin per Bypass (Insert-Slot) aus- und einschalten, *Source* auf *Test Pattern* und zurück, *Voice Range*/*Key*/*Scale* ändern, Wiedergabe in FL starten/stoppen. | Keine Note bleibt hängen, das Pitch-Rad des Synths steht danach in der Mitte. |
| 12 | **Testmodus noch da** | *Source = Test Pattern*, im Reiter *Test & Routing* *Test pattern = Notes*. | Die Test-Tonleiter aus M1 läuft wie gewohnt. Danach zurück auf *Voice*. |
| 13 | **Speichern** | Einstellungen ändern (z. B. Baritone, D Minor, True), Projekt speichern, FL neu starten, Projekt laden. | Alle Einstellungen sind wieder da. |

---

## 6. Was du zurückmelden sollst

Bitte als Liste oder Tabelle (Deutsch ist perfekt):

1. **Umgebung:** Commit-Kennung aus dem ZIP-Namen, FL-Version, Audio-Interface, Samplerate, ASIO-Puffer, Mikrofon
   (dynamisch/Kondensator), welcher Synth, VST3 oder CLAP.
2. **Checkliste:** Pro Nummer aus Abschnitt 5 o.k. / teilweise / nein, mit kurzer Bemerkung. Bei Problemen: was du
   gesungen hast und was passiert ist (z. B. „bei G3 manchmal eine Oktave zu hoch“, „beim Ausatmen kurze Noten“).
3. **YIN oder MPM:** Welcher Algorithmus war bei deiner Stimme besser (falsche Oktaven, Wackel-Noten, Aussetzer)?
4. **Einstellungen, die bei dir am besten waren:** vor allem *Gate*, *Stickiness*, *Min Note*.
5. **Latenz:** angezeigter Wert und dein Eindruck bei Puffer 128/256.
6. **Abstürze oder Aussetzer** (Knackser, CPU-Anzeige von FL deutlich höher als vorher).
7. **Screenshots:** VocalSynth-Fenster (Reiter *Voice*), Wrapper-Einstellungen mit den Ports.
8. Optional, sehr hilfreich für die Feinabstimmung: ein paar kurze **Aufnahmen deiner Stimme** als WAV (trocken, ohne
   Effekte, z. B. eine Tonleiter, ein Ton mit Vibrato, eine kurze Melodie, 10 s Atmen/Raumgeräusch). Daraus wird
   der Testkorpus (`tests/corpus/`), mit dem YIN, MPM und die Schwellen gemessen werden.

---

## 7. Bekannte Einschränkungen von 0.2.0

- **Keine Aufnahme in FL:** FL nimmt Plugin-MIDI nicht in die Piano Roll auf. Capture mit Drag & Drop kommt in M3.
- **Monophon:** Es wird immer nur eine Note erkannt (eine Stimme). Akkorde gibt es erst mit dem Akkord-Modus (M5).
- **Latenz:** Die Analyse braucht ein Fenster von mindestens zwei Perioden des tiefsten Tons der Stimmlage, daher
  Tenor ca. 24 ms, Bass/Wide ca. 39 ms plus ASIO-Puffer. Die Verzögerung wird FL nicht gemeldet und nicht
  ausgeglichen.
- **Feinabstimmung:** Schwellen (Gate-Hysterese, Konfidenz, Oktavschutz) sind bisher nur mit synthetischen Signalen
  abgestimmt. Deine Rückmeldung und Aufnahmen entscheiden die Standardwerte (auch YIN vs. MPM).
- **Monitor-Ton:** ein einfacher Sinus, nur zur Kontrolle; Pitchbend über ±2 Halbtöne hinaus (bei großer *Bend
  Range*) folgt er nur bis ±2 Halbtöne.
- **Instrument-Variante:** funktioniert genauso (Mikrofon über den Sidechain-Eingang, siehe FL-ROUTING.md, T3), ist
  aber nicht der Hauptweg und wurde in M1 nicht weiter getestet.
