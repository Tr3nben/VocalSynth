# Aufnahme und Drag & Drop testen (Meilenstein 3, Version 0.3.0)

Mit **VocalSynth 0.3.0** nimmt VocalSynth selbst auf, was du singst: Das Plugin hört immer mit und legt jeden
Take in einer Liste ab. Danach ziehst du den Take als **`.mid`** in einen Kanal (Channel Rack / Piano Roll) und auf
Wunsch die trockene Stimme als **`.wav`** in die Playlist. Grund: FL Studio nimmt MIDI, das ein Plugin ausgibt,
nicht in die Piano Roll auf (M1-Ergebnis).

Die Noten landen dort, wo du sie gesungen hast: Die Erkennungs-Verzögerung der Tonhöhen-Analyse und die
Verzögerung der Soundkarte (Eingangs- + Ausgangspuffer, bei ASIO 2048 ca. 85 ms) werden in den Dateien
herausgerechnet. Das Timing der Aufnahme ist also besser als das, was du live hörst.

Die Bedienelemente im Plugin sind englisch beschriftet, Menünamen von FL so, wie sie in der englischen
Oberfläche heißen. Wo ein Schritt nicht sicher belegt ist, steht **(unbestätigt)** dabei.

Zeitbedarf: ca. 60–90 Minuten für die ganze Checkliste (Kurztest: Abschnitt 4, ca. 15 Minuten).

> **Wichtig:** Takes werden **nicht** im FL-Projekt gespeichert (deine Entscheidung 4). Beim Schließen des
> Projekts oder von FL sind sie weg. Was du behalten willst: ziehen (Drag & Drop) oder mit *Save as...* speichern.
> Gezogene WAVs liegen dauerhaft in `Dokumente\VocalSynth\Takes` – dort löscht VocalSynth nie etwas.

---

## Inhalt

1. [Installation](#1-installation)
2. [Der Reiter „Capture“](#2-der-reiter-capture)
3. [Wie die Aufnahme funktioniert](#3-wie-die-aufnahme-funktioniert)
4. [Kurztest (15 Minuten)](#4-kurztest-15-minuten)
5. [Test-Checkliste](#5-test-checkliste)
6. [Was du zurückmelden sollst](#6-was-du-zurückmelden-sollst)
7. [Bekannte Einschränkungen von 0.3.0](#7-bekannte-einschränkungen-von-030)

---

## 1. Installation

1. Im [Actions-Tab](https://github.com/Tr3nben/VocalSynth/actions) den neuesten grünen Lauf öffnen und unten bei
   „Artifacts“ **`VocalSynth-0.3.0-Windows-<Commit>.zip`** herunterladen. Die Commit-Kennung aus dem Dateinamen
   notieren, sie gehört in deine Rückmeldung.
2. ZIP komplett entpacken, **`Install-VocalSynth.bat`** doppelklicken, die Administrator-Abfrage bestätigen und mit
   Enter in den Standard-Ordner installieren. Die vorige Version (0.2.1) wird überschrieben; deine FL-Projekte und
   Einstellungen bleiben erhalten (alte Projekte laden mit den Aufnahme-Standardwerten).
3. FL Studio öffnen: *Options → Manage plugins → Find installed plugins*.
4. Nach dem Laden prüfen: Oben im Plugin-Fenster steht **„Version 0.3.0 – M3 capture“** und es gibt drei Reiter:
   *Voice*, *Test & Routing* und neu **Capture**. Steht dort noch 0.2.1, hat FL die alte Datei geladen (FL neu
   starten, Plugin-Manager erneut suchen lassen).

Routing wie in 0.2.x ([M2-TESTING.md](M2-TESTING.md), Abschnitt 2): **VocalSynth FX auf dem Mikro-Insert →
MIDI Output Port → Synth (z. B. Vital) mit gleichem Input Port.** Für die Aufnahme braucht es kein Routing:
VocalSynth nimmt auch auf, wenn kein Synth angeschlossen ist. Kopfhörer benutzen.

---

## 2. Der Reiter „Capture“

Links:

| Element | Bedeutung |
|---|---|
| Statuszeile | „Synced 120.0 BPM 4/4 – recording take 3 (bar 17)“ (Transport läuft), „Free – waiting for a note“ / „Free – recording take 4“ (Transport gestoppt), „Capture off“. Bei Datenverlust erscheint orange „overflow: …“ – das soll nie passieren. |
| Speicherzeile | „Audio history … MB · takes … MB (n)“: Arbeitsspeicher dieser Instanz für den Rückblick und die Takes. |
| Take-Liste | Neuester Take oben: Nummer, Takt (Synced) oder Uhrzeit (Free), Länge (Takte oder Sekunden), Anzahl Noten, Hinweise *Grab*, *incomplete* (Daten verloren), *audio truncated*. Grünes Quadrat = Synced, graues = Free. Ein neuer Take wird automatisch ausgewählt. |
| Vorschau | Mini-Piano-Roll des ausgewählten Takes in der gewählten Version (Clean/Raw), darunter die Wellenform der Stimme. |
| Hinweis | „Takes are not saved with the project – drag or save what you want to keep.“ |

Rechts:

| Element | Bedeutung |
|---|---|
| **MIDI** (Ziehfläche) | Den ausgewählten Take als `.mid` in FL ziehen: auf einen Kanal im Channel Rack oder in eine offene Piano Roll. **Shift** beim Loslassen überspringt FLs MIDI-Import-Dialog, **Alt** erzwingt ihn. |
| **WAV** (Ziehfläche) | Die trockene Stimme des Takes als `.wav` in die Playlist ziehen. Zeigt „preparing...“, solange die Datei im Hintergrund geschrieben wird (bei langen Takes einige Sekunden). Beim Ziehen wird die Datei nach `Dokumente\VocalSynth\Takes` verschoben, weil FL WAVs nur verlinkt; danach „saved in Takes folder“. |
| *Save as...* | Ausgewählten Take als `.mid` oder `.wav` an einem Ort deiner Wahl speichern. |
| *New take* | Beendet den laufenden Take genau jetzt und beginnt den nächsten (auch mitten in einer gehaltenen Note). |
| *Grab last …* | Macht nachträglich einen Take aus dem, was du gerade gesungen hast – auch wenn kein Take lief. Transport läuft: die letzten N Takte (1–32, Auswahl daneben, Standard 8). Gestoppt: die letzten N Sekunden (10–120, Standard 30). |
| *Clear* | Löscht alle Takes und den Rückblick (fragt vorher). Schon gezogene WAVs bleiben. |
| *Version* | **Clean** (Standard): Geisternoten, Übergangsnoten und Oktavsprünge entfernt. **Raw**: genau das, was live gesendet wurde (zeitlich korrigiert). |
| *Bends* | Pitchbend in der `.mid`: **As plugin** (Standard: nur wenn *Bend Mode* beim Singen an war), *Off*, *On*. Mit Bends wird der Bend-Bereich (RPN 0 = *Bend Range*) an den Anfang geschrieben. |
| *CC* | Controller-Daten mitexportieren (nur das Testmuster sendet welche). |
| *Quantize* + Stärke + *Ends* | Notenanfänge auf das Raster ziehen (Off, 1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32), Stärke 0–100 %, *Ends* = Notenenden auch. Nur für Synced-Takes; Free-Takes werden nie quantisiert. Standard: aus. |
| *Offset* | Ausgleich der Soundkarten-Verzögerung (Plugin-Parameter): **Auto** (Standard) = 2 × Audio-Puffer, daneben steht der Wert in ms und der größte Block, den FL tatsächlich schickt („FL block“). *Manual* = Wert im Regler *Manual* (0–300 ms). *Off* = kein Ausgleich. |
| *Fine* | Feinkorrektur zu *Auto* (−100 … +100 ms): Liegen die Noten noch zu spät, Wert erhöhen. |
| *Capture* / *Capture audio* | Aufnahme an/aus; Stimme als WAV mit aufnehmen an/aus (beide Standard an). |
| *Free gap* | Pause, nach der bei gestopptem Transport ein neuer Take beginnt (0,5–30 s, Standard 3 s). |

Die Exportoptionen werden im Projekt gespeichert und gelten für jedes Ziehen, bis du sie änderst. Ändern
zeichnet die Vorschau sofort neu; eine schon gezogene Datei ändert sich nicht (neu ziehen).

Dateinamen: `VocalSynth_<Datum>_<Uhrzeit>_Take03_Bar017_Clean.mid` (Synced: Taktnummer im Namen),
`VocalSynth_<…>_Take04_Clean.mid` (Free: ohne Takt), mit Quantisierung z. B. `_Clean_Q16` oder `_Clean_Q16-50`
(50 %), die Stimme `…_Voice.wav`.

---

## 3. Wie die Aufnahme funktioniert

- **Immer an:** Solange *Capture* an ist, hört VocalSynth mit – auch bei geschlossenem Plugin-Fenster und
  gestopptem Transport. Die Stimme wird 10 Minuten zurück im Arbeitsspeicher gehalten (ca. 115 MB pro Instanz bei
  48 kHz, wächst in den ersten 10 Minuten langsam auf diesen Wert).
- **Synced (Transport läuft):** Ein Take beginnt mit *Play* und endet mit *Stop*. Loops im Pattern-Modus teilen den
  Take **nicht**: 8 Takte über zwei Durchläufe eines 4-Takt-Loops sind ein Take mit 8 Takten. Der Take erscheint ca.
  eine halbe Sekunde nach *Stop* in der Liste (so lange wird noch auf verspätete Noten gewartet).
- **Free (Transport gestoppt):** Ein Take beginnt mit der ersten Note und endet nach 3 s Pause (*Free gap*).
- **Takt:** Die `.mid` beginnt am Anfang des Takts, in dem deine erste Note liegt (deine Entscheidung 1). Die
  Taktnummer steht in der Liste und im Dateinamen. **Lege das Pattern in der Playlist an diesen Takt**, dann sitzen
  die Noten auf dem Raster. Die WAV beginnt am selben Punkt: an denselben Takt gelegt, laufen Stimme und Pattern
  zusammen.
- **Tempo:** Beim Ziehen in ein bestehendes Projekt übernimmt FL das Tempo der Datei nicht; die Noten werden nach
  Schlägen platziert. Synced-Takes passen deshalb bei jedem Projekttempo. Free-Takes sind in Sekunden beim
  aktuellen BPM-Wert von FL berechnet: Sie klingen nur bei diesem Tempo wie gesungen.

---

## 4. Kurztest (15 Minuten)

1. Version prüfen: **„Version 0.3.0 – M3 capture“**, Reiter **Capture** vorhanden.
2. Projekt 120 BPM, Metronom an, VocalSynth FX auf dem Mikro-Insert, ASIO 2048. Reiter *Capture* öffnen.
   Statuszeile: „Free – waiting for a note“.
3. *Play* drücken (Statuszeile „Synced 120.0 BPM 4/4 – recording take 1 (bar 1)“), 4 Takte lang Viertelnoten
   auf den Schlag singen, *Stop*. Nach ca. einer halben Sekunde erscheint **ein** Take mit der richtigen
   Taktnummer.
4. **MIDI** auf den Vital-Kanal im Channel Rack ziehen, das Pattern an den Takt des Takes legen: Liegen die Noten
   auf den Schlägen?
5. **WAV** in die Playlist an denselben Takt ziehen: Laufen Stimme und Pattern zusammen?

---

## 5. Test-Checkliste

Erwartetes Ergebnis jeweils in *kursiv*. Bitte notiere bei jedem Punkt o.k. / teilweise / nein.

1. **Erster Take.** Projekt 120 BPM, Metronom an, VocalSynth FX auf dem Mikro-Insert, ASIO **2048** (deine
   Einstellung) und zum Vergleich **256**. *Play*, 4 Takte Viertelnoten auf den Schlag singen, *Stop*.
   *Ein Take erscheint mit der richtigen Taktnummer (z. B. „Bar 1“ oder der Takt, an dem du gestartet hast).*
2. **MIDI ziehen.** *MIDI* (Clean, Quantize Off) auf den Vital-Kanal im Channel Rack ziehen und einmal in eine
   offene Piano Roll; das Pattern an den Takt des Takes legen. Einmal mit **Shift** beim Loslassen (kein
   Import-Dialog) und einmal mit **Alt** (Dialog).
   *Die Noten liegen bei beiden Puffergrößen (Offset Auto) innerhalb weniger ms auf den Schlägen.* Bitte notiere
   den Wert hinter „Auto =“ und „FL block“ bei 2048 und 256. Liegen die Noten deutlich zu spät oder zu früh:
   *Fine* verstellen, bis es passt, und den Wert notieren.
3. **Varianten.** Denselben Take mit *Quantize* 1/16 bei 100 % und 50 %, mit *Version* Raw und mit Bends (vorher
   *Bend Mode* = True singen) ziehen.
   *1/16 bei 100 %: alle Notenanfänge exakt auf dem Raster; 50 %: halb so weit. Raw: eventuell zusätzliche kurze
   Noten.* Frage: Übernimmt FL die Bends, und mit welchem Bend-Bereich klingen sie (±2 oder wie *Bend Range*)?
4. **WAV ziehen.** *WAV* in die Playlist an denselben Takt.
   *Stimme und Synth-Pattern laufen zusammen.* Projekt speichern, schließen, wieder öffnen: *Der Audio-Clip ist
   noch da* (die Datei liegt in `Dokumente\VocalSynth\Takes`).
5. **Tempo und Taktart.** Tempo **90** und **140 BPM**; ein Take mit Tempowechsel in der Mitte (Tempo-Automation);
   Taktart **3/4**.
   *Die Taktnummer stimmt, die Noten liegen auch hier auf dem Raster.* Bei einem Taktartwechsel weiter vorne im
   Song kann die Taktnummer abweichen (siehe 7).
6. **Pattern-Loop.** Pattern-Modus mit einem 4-Takt-Loop, 8 Takte über zwei Durchläufe singen.
   *Ein Take mit 8 Takten, die zweite Hälfte schließt nahtlos an.*
7. **Free-Modus.** Transport gestoppt: zwei Phrasen mit 5 s Pause singen.
   *Zwei Takes (Uhrzeit statt Takt).* Einen davon in ein Pattern ziehen.
8. **Grab.** Etwas singen, ohne auf das Plugin zu achten, dann *Grab last 8 bars* (Transport läuft) bzw.
   *Grab last 30 s* (gestoppt).
   *Ein Take mit dem Hinweis „Grab“ erscheint und enthält, was du gerade gesungen hast.*
9. **New take und Clear.** *New take* mitten in einer gehaltenen Note drücken; danach *Clear* (mit Rückfrage).
   *Die Note endet im alten Take am Schnitt und beginnt im neuen dort neu. Clear leert die Liste; schon gezogene
   WAVs bleiben in FL.*
10. **Langer Take.** 10 Minuten am Stück aufnehmen (Transport läuft), danach WAV ziehen.
    *Keine Aussetzer, kein „overflow“ in der Statuszeile, das Fenster reagiert flüssig; das WAV ist nach einigen
    Sekunden „ready“.*
11. **CPU und RAM.** CPU in FLs Performance-Anzeige mit und ohne *Capture* (VocalSynth allein, ohne Vital), Wert von
    *VocalSynth DSP* im Reiter *Voice*; die Speicherzeile im Reiter *Capture* nach 10 Minuten.
    *Capture kostet kaum CPU (< 1 % des Blocks); RAM pro Instanz ca. 115–130 MB nach 10 Minuten bei 48 kHz.*
12. **Zwei Instanzen und Aufräumen.** Zwei VocalSynth-Instanzen gleichzeitig (z. B. FX auf zwei Inserts); jede hat
    ihre eigene Take-Liste. FL schließen und im Explorer `%TEMP%\VocalSynth\Capture` öffnen.
    *Der Ordner ist leer (oder enthält nur Ordner anderer, noch laufender Instanzen).*

---

## 6. Was du zurückmelden sollst

Bitte als Liste oder Tabelle (Deutsch ist perfekt):

1. **Umgebung:** Commit-Kennung aus dem ZIP-Namen, FL-Version, Audio-Interface, Samplerate, ASIO-Puffer, VST3 oder
   CLAP, welcher Synth.
2. **Checkliste:** Pro Nummer aus Abschnitt 5 o.k. / teilweise / nein, mit kurzer Bemerkung.
3. **Timing (am wichtigsten):** Bei 2048 und 256: Wert hinter „Auto =“ und „FL block“, ob die Noten auf dem Schlag
   liegen, und falls nicht: um wie viel zu früh/spät (in FLs Piano Roll ablesbar, z. B. „ca. 1/32 zu spät“) und
   welcher *Fine*-Wert es behoben hat.
4. **Drag & Drop:** Was ging, was nicht (Channel Rack, Piano Roll, Playlist, Shift/Alt, Import-Dialog), und ob FL
   die Bends übernimmt und mit welchem Bereich.
5. **Clean oder Raw:** Welche Version war musikalisch brauchbarer? Fehlten in Clean Noten, die du gesungen hast,
   oder gab es in Raw störende Zusatznoten?
6. **Abstürze, Aussetzer** (Knackser), Overflow-Meldung, CPU- und RAM-Werte aus Punkt 11.
7. **Screenshots:** Reiter *Capture* mit Take-Liste, die Piano Roll nach dem Ziehen (mit Raster), die Playlist mit
   Pattern und WAV am selben Takt.
8. Optional, sehr hilfreich: ein oder zwei Takes als `.mid` **und** `.wav` (aus `Dokumente\VocalSynth\Takes`) –
   damit lassen sich Clean und das Timing an deiner echten Stimme nachmessen.

---

## 7. Bekannte Einschränkungen von 0.3.0

- **Takes gehen beim Schließen verloren** (nicht im Projekt gespeichert, Entscheidung 4). Ziehen oder speichern.
- **Auto-Offset** nimmt an, dass deine Stimme einen Eingangs- und einen Ausgangspuffer zu spät ankommt
  (2 × Puffergröße, die FL dem Plugin beim Start meldet). Meldet FL eine andere Größe als den ASIO-Puffer, steht
  bei „FL block“ ein kleinerer Wert und ein Hinweis; dann *Manual* oder *Fine* benutzen. Eine Kalibrierung
  (zum Metronom klatschen) kommt später (Entscheidung 3).
- **Taktnummer bei Taktartwechseln:** Wechselt die Taktart weiter vorne im Song, kann die angezeigte Taktnummer
  abweichen (sie zählt mit der aktuellen Taktart ab Songanfang). Die Noten liegen trotzdem richtig zum Taktanfang.
- **Free-Takes** werden mit dem BPM-Wert von FL beim Singen in Ticks umgerechnet; in einem Projekt mit anderem
  Tempo werden sie gestreckt oder gestaucht.
- Eine Note, die bis zu 1/32 Schlag **vor** dem Taktstrich beginnt, zählt zum neuen Takt und rutscht auf dessen
  Anfang (höchstens 15,6 ms bei 120 BPM).
- Ein Tempowechsel innerhalb eines sehr großen Audio-Blocks wird erst am nächsten Block bemerkt (bei 2048 Samples
  höchstens ca. 43 ms); die Abweichung bleibt sehr klein.
- **Speicher:** 10 Minuten Stimme kosten ca. 115 MB pro Instanz (48 kHz), bei 96 kHz ca. 230 MB, bei 192 kHz
  höchstens 256 MB. Mit *Capture audio* aus bleibt es bei wenigen MB.
- MIDI und WAV lassen sich nicht in einem Zug ziehen (zweimal ziehen). Takes bearbeiten (Noten verschieben,
  mehrere Takes kombinieren) gibt es im Plugin nicht – dafür ist FLs Piano Roll da.
