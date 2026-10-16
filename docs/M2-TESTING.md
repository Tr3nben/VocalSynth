# Singen → MIDI testen (Meilenstein 2, Version 0.2.1)

Mit **VocalSynth 0.2.x** wird deine Stimme zu MIDI: Du singst oder summst ins Mikrofon, und
VocalSynth schickt die erkannten Noten (und auf Wunsch Pitchbend) über den MIDI-Port von FL Studio an einen Synth.
Diese Anleitung erklärt die Einrichtung in FL Studio 2026, jedes Bedienelement und eine Checkliste. Am Ende steht,
was du zurückmelden sollst.

Es ist der Weg, der in M1 funktioniert hat (Test T1): **VocalSynth FX auf dem Mikro-Insert → MIDI Output Port →
VST3-Synth mit gleichem Input Port.** Die Bedienelemente im Plugin sind englisch beschriftet, Menünamen von FL so,
wie sie in der englischen Oberfläche heißen. Wo ein Schritt nicht sicher belegt ist, steht **(unbestätigt)** dabei.

Zeitbedarf: ca. 45–60 Minuten (nur die Neuerungen von 0.2.1: ca. 15 Minuten, siehe [Abschnitt 0](#0-was-ist-neu-in-021)).

> **Wichtig für die Latenz: ASIO-Puffer klein stellen (128 oder 256 Samples).** Ein Puffer von 2048 Samples
> allein kostet ca. **46 ms** (bei 44,1 kHz; 43 ms bei 48 kHz) – und er zählt beim Singen doppelt (Eingang und
> Ausgang). Mit dem MIDI-Port von FL kommt oft noch ein Puffer dazu. Details und Klickweg:
> [Abschnitt 1.1](#11-puffergröße-und-latenz).

> **Wichtig:** FL Studio nimmt MIDI, das ein Plugin ausgibt, **nicht** in die Piano Roll des Synths auf (in M1
> festgestellt). Du hörst die Noten live, kannst sie aber noch nicht aufnehmen. Die eigene Aufnahme mit Drag & Drop
> nach FL kommt in Meilenstein 3. Wenn du einen Take festhalten willst: Audio des Synths aufnehmen.

---

## Inhalt

0. [Was ist neu in 0.2.1](#0-was-ist-neu-in-021)
1. [Installation](#1-installation) – mit [1.1 Puffergröße und Latenz](#11-puffergröße-und-latenz)
2. [Routing in FL Studio](#2-routing-in-fl-studio)
3. [Das Plugin-Fenster](#3-das-plugin-fenster)
4. [Erste Schritte (5 Minuten)](#4-erste-schritte-5-minuten)
5. [Test-Checkliste](#5-test-checkliste)
6. [Was du zurückmelden sollst](#6-was-du-zurückmelden-sollst)
7. [Bekannte Einschränkungen von 0.2.x](#7-bekannte-einschränkungen-von-02x)

---

## 0. Was ist neu in 0.2.1

Antwort auf deine Rückmeldung zu 0.2.0 (danke!):

- **Ruhigeres Stimmgerät:** Notenname, Nadel, Hz-Wert und Lämpchen springen nicht mehr 20–30-mal pro Sekunde.
  Der Notenname zeigt die Note, die VocalSynth gerade sendet (bleibt stehen wie die MIDI-Note), die Nadel
  bewegt sich gedämpft wie bei einem Hardware-Stimmgerät, der Hz-Wert ändert sich höchstens 5-mal pro Sekunde.
  Am MIDI selbst ändert das nichts.
- **Latenz-Anzeige mit Puffer:** Im Reiter *Voice* steht jetzt neben der Analyse-Latenz auch dein Audio-Puffer
  und eine Schätzung der Gesamt-Latenz. Bei mehr als 512 Samples erscheint ein oranger Hinweis.
- **Eigene DSP-Last:** *VocalSynth DSP* zeigt, wie viel Rechenzeit VocalSynth selbst braucht – damit kannst du
  VocalSynth von Vital (oder einem anderen Synth) in der CPU-Anzeige von FL unterscheiden.
- **Gate-Standard −40 dB** (vorher −45). Dein Erfahrungswert **−35 … −40 dB** ist damit fast erreicht; alle
  Regler bleiben einstellbar.
- **Schnellere Notenanfänge** bei klaren Tonanfängen: im Tenor bei mittlerer Tonlage (A4/C5 in FL-Notennamen,
  220/262 Hz) ca. 10–11 statt ca. 20 ms, ab E4 (165 Hz) unter 15 ms. Am tiefsten Ton der Stimmlage (Tenor A3,
  110 Hz) bleibt es bei ca. 21 ms, weil die Analyse mindestens zwei Schwingungen braucht. Die Anzeige *Analysis*
  zeigt diesen ungünstigsten Fall (Tenor 23 ms statt 24 ms). Gleitest du von unten in den Ton (Schleifer), kann
  kurz (ca. 20 ms im Tenor) der Halbton darunter anklingen, danach kommt die richtige Note.

Kurztest für 0.2.1 (ca. 15 Minuten):

1. Version prüfen: oben im Plugin **„Version 0.2.1 – M2 pitch to MIDI“**.
2. ASIO-Puffer wie in [1.1](#11-puffergröße-und-latenz) auf **256** (dann **128**) stellen; die Zeile *Buffer*
   im Plugin muss den neuen Wert zeigen, der orange Hinweis verschwindet.
3. Tonleiter und einen Ton mit Vibrato singen: Ist das Stimmgerät jetzt angenehm ruhig, aber noch schnell genug?
   Ein paar Töne bewusst von unten anschleifen: Hörst du am Notenanfang einen Doppelanschlag oder Zwitschern?
4. Latenz-Gefühl mit dem Synth über den MIDI-Port bei 2048, 256 und 128 Samples vergleichen.
5. *VocalSynth DSP* (Reiter *Voice* unten oder *Test & Routing*) beim Singen ablesen und mit der CPU-Anzeige
   von FL vergleichen.

---

## 1. Installation

1. Im [Actions-Tab](https://github.com/Tr3nben/VocalSynth/actions) den neuesten grünen Lauf öffnen und unten bei
   „Artifacts“ **`VocalSynth-0.2.1-Windows-<Commit>.zip`** herunterladen. Die Commit-Kennung aus dem Dateinamen
   notieren, sie gehört in deine Rückmeldung.
2. ZIP komplett entpacken, **`Install-VocalSynth.bat`** doppelklicken, die Administrator-Abfrage bestätigen und mit
   Enter in den Standard-Ordner installieren (oder deinen eigenen VST3-Ordner angeben). Die vorige Version (0.2.0)
   wird dabei überschrieben; deine FL-Projekte und Einstellungen bleiben erhalten. Details: `LIESMICH.txt` im ZIP.
3. FL Studio öffnen: *Options → Manage plugins → Find installed plugins*.
4. Nach dem Laden prüfen: Oben im Plugin-Fenster steht **„Version 0.2.1 – M2 pitch to MIDI“**. Steht dort noch
   0.2.0, hat FL die alte Datei geladen (FL neu starten, Plugin-Manager erneut suchen lassen).

**Audio-Einstellungen** (*Options → Audio settings*): ASIO-Treiber deines Interfaces (oder „FL Studio ASIO“),
Puffer **128–256 Samples** (siehe 1.1). Notiere Samplerate und Puffergröße. Kopfhörer benutzen, damit der Synth
nicht ins Mikrofon zurückläuft (sonst „hört“ VocalSynth den Synth und spielt ihn nach).

### 1.1 Puffergröße und Latenz

Die Verzögerung zwischen Singen und Synth-Ton setzt sich aus drei Teilen zusammen:

| Teil | Wie groß | Was du tun kannst |
|---|---|---|
| **Analyse** in VocalSynth | je nach Tonhöhe: Tenor ca. 10–23 ms, Bass/Wide ca. 13–38 ms (Anzeige *Analysis* = ungünstigster Fall, tiefster Ton) | Passende *Voice Range* wählen (tiefere Lagen brauchen ein längeres Fenster). |
| **Audio-Puffer** (ASIO) | Puffer ÷ Samplerate, **zweimal**: einmal beim Eingang (Mikrofon), einmal beim Ausgang (Kopfhörer) | **Puffer verkleinern** – das bringt am meisten. |
| **MIDI-Port von FL** | wenn der Synth das MIDI über den Output/Input Port bekommt: oft **ein weiterer Puffer** **(unbestätigt)** | Puffer verkleinern. (Der Monitor-Ton im Plugin hat diesen Umweg nicht.) |

Beispiele bei 44,1 kHz (Tenor, Analyse wie angezeigt 23 ms; bei mittlerer Tonlage eher 11 ms):

| ASIO-Puffer | ein Puffer | Gesamt ca. (Analyse + 2 Puffer) | über den MIDI-Port ca. |
|---|---|---|---|
| **2048** Samples | 46 ms | 116 ms | 162 ms |
| 512 Samples | 12 ms | 46 ms | 58 ms |
| **256** Samples | 5,8 ms | 35 ms | 41 ms |
| **128** Samples | 2,9 ms | 29 ms | 32 ms |

Ab etwa 30–40 ms fühlt sich Singen → Synth „direkt“ an; 100 ms und mehr merkt man deutlich. Deshalb:
**128 oder 256 Samples** einstellen. Knackst es dann, den nächstgrößeren Wert nehmen (256 → 512).

**So stellst du den Puffer in FL Studio um:** *Options → Audio settings* → im Bereich *Input / output*:

- Mit **„FL Studio ASIO“**: den Regler **Buffer length** auf 128 oder 256 Samples ziehen.
- Mit dem **ASIO-Treiber deines Interfaces**: auf **Show ASIO panel** klicken und dort die Puffergröße
  („Buffer size“, „Latency“ o. ä.) einstellen. **(unbestätigt, je nach Interface anders benannt)**

Danach zeigt VocalSynth im Reiter *Voice* in der Zeile *Buffer* den neuen Wert (nach 1–2 Sekunden Audio).
Den Puffer siehst du dort immer so, wie FL ihn wirklich an das Plugin schickt (größter Block der letzten
1–2 Sekunden).

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
| **Stimmgerät** (großer Notenname) | Benannt wie in der Piano Roll von FL (**C5 = MIDI-Note 60**, A5 = 440 Hz). **Weiß:** die Note, die VocalSynth gerade sendet (in deiner gesungenen Oktave, also ohne *Octave*-Verschiebung); sie bleibt stehen, solange die MIDI-Note klingt. **Hellgrau:** keine Note wird gesendet, angezeigt wird die nächste Note, sobald sie ca. 0,1 s stabil ist. Nach dem Ende des Tons bleibt der Name ca. 0,25 s stehen und verblasst dann zu „-“. Daneben die Frequenz in **Hz** (ganze Hz, ab 1 kHz in kHz) und die Abweichung in **cents**, beide höchstens 5-mal pro Sekunde aktualisiert. |
| **Nadel** (−50 … +50 cents) | Abweichung von der angezeigten Note, gedämpft (Zeitkonstante 0,12 s, wie ein Hardware-Stimmgerät): Vibrato von ±50 cents zeigt sie als ca. ±12 cents. Bei einem Notenwechsel gleitet sie zur neuen Position. Grün: höchstens 10 cents daneben. Orange: weiter daneben. Steht sie am Rand, liegt dein Ton mehr als 50 cents neben der gesendeten Note (Stickiness oder Skala halten die Note). |
| **Confidence** | Wie sicher sich die Tonhöhenerkennung ist (Balken 0–100 %, steigt schnell, fällt langsamer). Bei Atem, Zischlauten und Rauschen bleibt er niedrig. |
| **Voiced** (Lämpchen) | Leuchtet, wenn VocalSynth deine Stimme als „gesungener Ton“ zählt (laut genug und sicher genug erkannt). Nur dann entstehen Noten. Geht sofort an und erst nach 0,15 s ohne Stimme aus (kein Flackern). |
| **Sounding note** | Die Note, die gerade als MIDI gesendet wird (mit Oktav-Verschiebung). |
| **Input-Pegel** mit **Gate-Linie** | Breiter Balken: Spitzenpegel am Eingang. Schmaler Balken unten: der Pegel, den das Gate sieht (grün = über dem Gate). Weiße Linie: die Gate-Schwelle. |
| **Analysis … · Buffer …** (Kasten unten rechts) | *Analysis*: Verzögerung der Analyse vom Tonbeginn bis zur Note-On, für Stimmlage und Samplerate. *Buffer*: der Audio-Puffer, den FL wirklich schickt, in Samples und ms. Die Verzögerung wird FL nicht gemeldet; das Audio wird nicht verzögert. |
| **Total ≈ … ms** | Schätzung, was du hörst: Analyse + Eingangs- + Ausgangspuffer (Treiber-Extras nicht eingerechnet). Dahinter *FL MIDI port ≈ +… ms*: so viel kommt ungefähr dazu, wenn der Synth das MIDI über den MIDI-Port bekommt. Orange, wenn der Puffer größer als 512 Samples ist; darunter dann der Hinweis *„Large audio buffer adds latency — try 128 or 256 samples in FL's audio settings“* (siehe [1.1](#11-puffergröße-und-latenz)). |
| **VocalSynth DSP** | Rechenzeit, die VocalSynth selbst pro Audio-Block braucht, in Prozent der Blockdauer: *avg* = Mittel über ca. 1 s, *peak* = langsamster Block (fällt über ein paar Sekunden zurück). Bei 100 % würde es knacksen. Die CPU-Anzeige von FL enthält dagegen alles (auch Vital) und rechnet anders – die Zahlen sind nicht direkt vergleichbar, zeigen aber, wer wie viel braucht. |

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
| **Gate** | −70 … −10 dB, **−40** (bis 0.2.0: −45) | Unter diesem Pegel entsteht keine Note. **Empfehlung: −35 … −40 dB** (dein Erfahrungswert aus dem 0.2.0-Test). Höher stellen, wenn Raumgeräusche oder Atem Noten auslösen; tiefer, wenn leises Singen nicht erkannt wird. Die Linie im Pegelbalken zeigt den Wert. |
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
- **VocalSynth DSP**: dieselbe DSP-Last-Anzeige wie im Reiter *Voice* (siehe 3.1).
- **Block sizes**: die Blockgrößen, die FL schickt; *max* entspricht normalerweise dem ASIO-Puffer.
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
| 8 | **Latenz-Gefühl** | Kurze, abgesetzte Töne („da da da“) im Takt eines Metronoms singen, ASIO-Puffer 2048, 256 und 128 (siehe 1.1), einmal mit dem Synth über den MIDI-Port, einmal mit *Monitor Tone*. | Fühlt sich das spielbar an? Notiere die Werte *Analysis*, *Buffer* und *Total* und deinen Eindruck (z. B. „merkbar, aber o.k.“). |
| 9 | **Stimmlagen** | *Voice Range* auf **Baritone** und **Wide** umstellen, ein paar Töne singen; bei Interesse auch *Whistle* mit Pfeifen. | Funktioniert weiter; die angezeigte Analyse-Latenz ändert sich (Wide/Bass länger als Tenor). Wide neigt eher zu Oktavfehlern. |
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
5. **Latenz:** angezeigte Werte (*Analysis*, *Buffer*, *Total*) und dein Eindruck bei Puffer 2048/256/128, mit
   dem Synth über den MIDI-Port und mit *Monitor Tone*.
6. **Abstürze oder Aussetzer** (Knackser), **CPU:** *VocalSynth DSP* (avg/peak) beim Singen und die CPU-Anzeige von
   FL mit und ohne Vital.
7. **Stimmgerät:** Ist die Anzeige jetzt ruhig genug, oder zu träge?
8. **Screenshots:** VocalSynth-Fenster (Reiter *Voice*), Wrapper-Einstellungen mit den Ports.
9. Optional, sehr hilfreich für die Feinabstimmung: ein paar kurze **Aufnahmen deiner Stimme** als WAV (trocken, ohne
   Effekte, z. B. eine Tonleiter, ein Ton mit Vibrato, eine kurze Melodie, 10 s Atmen/Raumgeräusch). Daraus wird
   der Testkorpus (`tests/corpus/`), mit dem YIN, MPM und die Schwellen gemessen werden.
   Zu deiner Frage: Ja, es gibt öffentliche Sammlungen mit sauber gesungenen Tonleitern, z. B. **VocalSet**
   (20 Sängerinnen und Sänger, Tonleitern, Arpeggien, lange Töne, Vibrato; frei nutzbar). Solche Daten eignen
   sich gut, um YIN/MPM und die Schwellen zu prüfen. Deine eigenen Aufnahmen bleiben trotzdem wertvoll, weil
   darin dein Mikrofon, dein Raum und deine Stimme stecken – aber sie sind keine Voraussetzung mehr.

---

## 7. Bekannte Einschränkungen von 0.2.x

- **Keine Aufnahme in FL:** FL nimmt Plugin-MIDI nicht in die Piano Roll auf. Capture mit Drag & Drop kommt in M3.
- **Monophon:** Es wird immer nur eine Note erkannt (eine Stimme). Akkorde gibt es erst mit dem Akkord-Modus (M5).
- **Latenz:** Die Analyse braucht mindestens etwa zwei Schwingungen des gesungenen Tons, daher je nach Tonhöhe
  Tenor ca. 10–23 ms, Bass/Wide ca. 13–38 ms (am tiefsten Ton der Stimmlage am längsten), plus zweimal der ASIO-Puffer (siehe 1.1). Die
  Verzögerung wird FL nicht gemeldet und nicht ausgeglichen. Die Anzeige *Total* ist eine Schätzung: Treiber-
  und Wandler-Latenz deines Interfaces kommen noch dazu.
- **Feinabstimmung:** Schwellen (Gate-Hysterese, Konfidenz, Oktavschutz) sind bisher nur mit synthetischen Signalen
  abgestimmt. Deine Rückmeldung und Aufnahmen entscheiden die Standardwerte (auch YIN vs. MPM).
- **Monitor-Ton:** ein einfacher Sinus, nur zur Kontrolle; Pitchbend über ±2 Halbtöne hinaus (bei großer *Bend
  Range*) folgt er nur bis ±2 Halbtöne.
- **Instrument-Variante:** funktioniert genauso (Mikrofon über den Sidechain-Eingang, siehe FL-ROUTING.md, T3), ist
  aber nicht der Hauptweg und wurde in M1 nicht weiter getestet.
