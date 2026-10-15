# FL-Studio-Routing-Test (Meilenstein 1)

Diese Anleitung führt dich durch die Tests T1–T7 aus [RESEARCH.md, Abschnitt 2.5](RESEARCH.md#25-folgerungen-und-pflicht-spike-vor-der-architektur). Mit dem Testbuild **VocalSynth 0.1.0** findest du heraus, auf welchem Weg MIDI in FL Studio 2026 zuverlässig von VocalSynth zu einem Klangerzeuger kommt. Danach wird nach der Entscheidungsregel in Abschnitt 7 der Hauptweg für VocalSynth festgelegt.

**Wichtig:** Der Testbuild analysiert deine Stimme noch **nicht**. Er erzeugt selbst festes Test-MIDI (Tonleiter, Pitchbend-Sweep, CC-Sweep). Ein Mikrofon brauchst du nur für T3 (kommt Audio im Instrument an?) und für die Pegelanzeige.

Zeitbedarf: ca. 1,5–2 Stunden für alles. T1, T6 und T7 sind die wichtigsten; wenn die Zeit knapp ist, mach die zuerst.

Die Bedienelemente im Plugin sind englisch beschriftet. Menünamen in FL Studio sind hier so angegeben, wie sie in der englischen Oberfläche von FL heißen; je nach FL-Version können sie leicht abweichen. Wo ein Schritt nicht sicher belegt ist, steht **(unbestätigt)** dabei – genau das sollen die Tests ja klären. Wenn etwas anders aussieht als beschrieben, schreib es in die Spalte „Bemerkung“.

---

## Inhalt

1. [Vorbereitung](#1-vorbereitung)
2. [Das Testmenü im Plugin](#2-das-testmenü-im-plugin)
3. [So erkennst du, ob ein Test geklappt hat](#3-so-erkennst-du-ob-ein-test-geklappt-hat)
4. [Die Tests T1–T7](#4-die-tests-t1t7)
5. [Zusatzchecks](#5-zusatzchecks)
6. [Ergebnisse (zum Ausfüllen)](#6-ergebnisse-zum-ausfüllen)
7. [Entscheidungsregel](#7-entscheidungsregel)
8. [Bekannte Einschränkungen des Testbuilds](#8-bekannte-einschränkungen-des-testbuilds)

---

## 1. Vorbereitung

1. **Installieren:** ZIP `VocalSynth-0.1.0-Windows-<Commit>.zip` aus dem [Actions-Tab](https://github.com/Tr3nben/VocalSynth/actions) laden, entpacken, `Install-VocalSynth.bat` ausführen (Details in der `LIESMICH.txt` im ZIP). Danach in FL: *Options → Manage plugins → Find installed plugins*.
   - Merk dir die Commit-Kennung aus dem ZIP-Namen, sie gehört in die Ergebnistabelle.
   - Wenn eine ältere Version (M0) installiert war: Das Installationsskript überschreibt sie. Prüfe nach dem Laden, dass im Plugin-Fenster oben „Version 0.1.0“ steht.
2. **Audio-Einstellungen:** *Options → Audio settings*: ASIO-Treiber deines Interfaces (oder „FL Studio ASIO“), Puffer **128–256 Samples**. Notiere Samplerate und Puffergröße.
3. **Projekt-Tempo:** Stell FL auf **120 BPM**. Das ist auch der Standardwert des Plugin-Reglers „Test tempo (BPM)“. Das Testmuster läuft mit dem Plugin-Tempo, nicht mit dem FL-Tempo (siehe Abschnitt 8).
4. **Empfänger-Synth:** Für T1, T3, T4, T5 und T7 brauchst du einen VST3-Synth mit MIDI-Eingang. Gut geeignet und kostenlos:
   - **Vital** (vital.audio) oder **Surge XT** (surge-synthesizer.github.io). Beide zeigen links unten ein **Pitch-Rad und ein Mod-Rad**, die sich sichtbar bewegen, wenn Pitchbend bzw. CC 1 ankommt.
   - Für T2 brauchst du einen **nativen FL-Synth**, z. B. FLEX, Sytrus oder 3x Osc.
5. **Kopfhörer und Lautstärke:** Den Pitchbend-Sweep (voller Bereich, einmal pro Sekunde) findet man nach kurzer Zeit anstrengend. Fang leise an.
6. **„Smart disable“ ausschalten:** FL kann Plugins schlafen legen, wenn kein Audio anliegt. Dann läuft auch das Testmuster nicht. Im Plugin-Fenster von VocalSynth: Menü (Pfeil oben links) → *Smart disable* ausschalten, falls es aktiv ist. Für alle Plugins auf einmal gibt es *Tools → Macros → Switch smart disable for all plugins* **(unbestätigt, ob das in FL 2026 noch so heißt)**.
7. **Screenshots:** Mach bei jedem Test einen Screenshot der Wrapper-Einstellungen (Ports) und des VocalSynth-Fensters. Windows: `Win + Umschalt + S`.

### Wo finde ich die Wrapper-Einstellungen?

Jedes Plugin-Fenster in FL hat oben links eine kleine Werkzeugleiste. Das **Zahnrad-Symbol** („Plugin options“ / „Settings“) öffnet die *VST wrapper settings*. Dort gibt es einen Bereich **MIDI** mit den Feldern **Input port** und **Output port** (Wert mit der Maus hoch- oder runterziehen, oder Doppelklick zum Eintippen). Im Bereich **Processing** stehen u. a. „Use fixed size buffers“ und die Eingangs-Zuordnung für Sidechains.

---

## 2. Das Testmenü im Plugin

Beide Varianten, **VocalSynth FX** (Effekt) und **VocalSynth Instrument** (Generator), haben dasselbe Testmenü. Alle Einstellungen sind normale Plugin-Parameter. Du kannst sie also auch über FL automatisieren oder in der Parameterliste des Wrappers ändern, falls das Plugin-Fenster mal nicht aufgeht.

| Element im Plugin | Bedeutung |
|---|---|
| Kopfzeile | „VocalSynth FX - M1 routing test build“ bzw. „VocalSynth Instrument - …“, dazu **Version** und **Format** (VST3 oder CLAP). Damit beweist der Screenshot, welcher Build geladen war. |
| **Test Pattern** | Welches Test-MIDI erzeugt wird (Tabelle unten). Standard: **Off**. |
| **MIDI Channel** | MIDI-Kanal 1–16 für alle Testereignisse. Standard: 1. |
| **Test tempo (BPM)** | Tempo des Testmusters, 60–200 BPM. Standard: 120. |
| **CC Number** | Welcher Controller beim „CC Sweep“ gesendet wird, 1–119. Standard: **1 = Mod-Rad**. |
| **Monitor Tone** | Ein einfacher Sinuston im Plugin spielt das Muster selbst hörbar ab (Test T6). Braucht kein MIDI-Routing. |
| **Virtual MIDI Out** | Öffnet einen virtuellen MIDI-Port unter Windows (Test T7). Daneben steht der Status: `Port: off`, `Port: open: VocalSynth FX` (bzw. „… 2“ bei der zweiten Instanz) oder `Port: failed`. |
| **Mute Dry** (nur FX) | Schaltet das Original-Audio (dein Mikro) stumm. Der Monitor-Ton bleibt hörbar. |
| **Restart pattern** | Beendet alle laufenden Testnoten und startet das Muster von vorn. Nach jeder Änderung am Routing einmal drücken. |
| Zähler „sent“ | Wie viele Note-Ons, Note-Offs, Pitchbends und CCs das Plugin an FL übergeben hat, gezählt seit dem Laden des Plugins (nie zurückgesetzt). Wichtig ist, ob sie hochlaufen. |
| Pegelanzeige und **Input: N ch** | Ob Audio ankommt und auf wie vielen Kanälen (wichtig für T3). |
| Aktuelle Testnote, Samplerate | Zur Kontrolle, was das Plugin gerade spielt. |
| Blockgrößen-Histogramm | Wie groß die Audio-Blöcke sind, die FL dem Plugin schickt (min/max/letzter, Verteilung). Mit Reset-Knopf. |
| **Drag test MIDI** / **Drag test WAV** | Zum Herausziehen einer Test-`.mid` bzw. `.wav` nach FL (Zusatzcheck in Abschnitt 5). |

### Die Testmuster

Angaben bei 120 BPM. Das Muster läuft frei vor sich hin, sobald FL das Plugin verarbeitet – du musst in FL **nicht** auf Play drücken.

| Test Pattern | Was gesendet wird | Woran du es erkennst |
|---|---|---|
| **Off** | nichts | Stille |
| **Notes** | C-Dur-Tonleiter aufwärts von MIDI-Note 60 bis 72, eine Achtelnote pro Ton (4 Töne pro Sekunde), Velocity 100, dann wieder von vorn | Tonleiter „do-re-mi…“ |
| **Pitch Bend** | Note 60 wird gehalten und **jeden Takt neu angeschlagen**. Dazu Pitchbend im vollen Bereich: Mitte → ganz hoch → Mitte → ganz runter → Mitte, einmal pro Sekunde (2 Beats) | Ton jault rauf und runter; das Pitch-Rad im Synth bewegt sich |
| **CC Sweep** | Note 60 gehalten, jeden Takt neu angeschlagen. Dazu der Controller „CC Number“ von 0 → 127 → 0 in 2 Sekunden (1 Takt) | Mod-Rad (bei CC 1) wandert rauf und runter |
| **All** | Im Wechsel 4 Takte Notes, 2 Takte Pitch Bend, 2 Takte CC Sweep (zusammen 16 s), dann von vorn | alles nacheinander |

MIDI-Note 60 heißt in FL **C5**, in vielen anderen Programmen C4. Gemeint ist dieselbe Taste. Das Plugin-Fenster benennt Noten wie FL („Test note: C5 (60)“).

---

## 3. So erkennst du, ob ein Test geklappt hat

Jeder Test hat zwei Hälften:

1. **Sendet das Plugin?** Die Zähler „sent“ im VocalSynth-Fenster laufen hoch. Achte darauf, ob sie sich **bewegen**, nicht auf ihren Wert: Die Zähler zählen seit dem Laden des Plugins und werden nie zurückgesetzt. Wenn du dasselbe VocalSynth aus einem früheren Test weiterverwendest, stehen dort also schon Zahlen. Bleiben die Zähler stehen (egal auf welchem Wert, auch nicht 0): Test Pattern steht auf Off, das Plugin ist gebypasst oder FL hat es schlafen gelegt (Smart disable). Das ist dann kein Routing-Ergebnis – erst beheben.
2. **Kommt es an?** Der Ziel-Synth reagiert:
   - **Note:** du hörst die Tonleiter (Pattern „Notes“). Viele Synths zeigen gespielte Töne auch auf ihrer Bildschirm-Tastatur.
   - **Pitchbend:** das Pitch-Rad des Synths bewegt sich und der Ton jault (Pattern „Pitch Bend“). Wie weit er jault, hängt von der Bend-Range des Synths ab (meist ±2 Halbtöne).
   - **CC:** das Mod-Rad des Synths bewegt sich (Pattern „CC Sweep“, CC Number 1). Wenn dein Synth kein Mod-Rad anzeigt: einen Regler per MIDI-Learn zuweisen (Vital: Rechtsklick auf einen Regler → „Learn MIDI Assignment“; Surge XT: Rechtsklick → „MIDI Learn“). Der laufende Sweep wird dann gelernt, und der Regler bewegt sich mit.

Zähler laufen, Synth schweigt → FL leitet das MIDI auf diesem Weg **nicht** weiter. Genau das ist das Ergebnis, das wir suchen. Bitte nicht „reparieren“, sondern notieren.

Teilergebnisse sind wichtig: Es kann gut sein, dass Noten ankommen, Pitchbend oder CC aber nicht. Deshalb hat die Ergebnistabelle drei Spalten.

**Nach jeder Routing-Änderung:** einmal **Restart pattern** drücken. Gehaltene Töne (Pitch Bend, CC Sweep) werden zwar jeden Takt neu angeschlagen, aber so musst du nicht warten.

**Doppelte Noten vermeiden:** Wenn mehrere Wege gleichzeitig aktiv sind (z. B. Port-Routing aus T1 und der virtuelle Port aus T7), bekommt der Synth alles doppelt. Vor jedem Test die Ports des vorherigen Tests zurücksetzen (Input port des Synths auf „—“/aus) oder das vorherige VocalSynth entfernen.

---

## 4. Die Tests T1–T7

### T1 – VocalSynth FX im Mixer → VST3-Synth über MIDI-Port

Das ist der Wunschweg: Mikro-Insert mit VocalSynth als Effekt, MIDI geht über einen FL-Port an einen Synth im Channel Rack.

1. **Mixer öffnen** (`F9`). Einen freien Insert wählen, z. B. **Insert 1**. Rechts oben im Mixer unter *Input* (Audio-Eingang) dein Mikrofon auswählen.
2. Im Effekt-Slot-Bereich rechts auf einen freien Slot klicken → **VocalSynth FX** wählen. Achte darauf, die **VST3**-Variante zu nehmen (die CLAP-Variante kommt in T5). Im Plugin-Fenster muss oben „Format: VST3“ stehen.
3. Im VocalSynth-Fenster: Zahnrad (Wrapper-Einstellungen) → Bereich *MIDI* → **Output port = 5**.
4. **Channel Rack öffnen** (`F6`) → „+“ → **Vital** (oder Surge XT) hinzufügen. Das Synth-Fenster öffnen → Zahnrad → Bereich *MIDI* → **Input port = 5**.
5. Zurück zu VocalSynth: **Test Pattern = Notes**, dann **Restart pattern**.
   - Hörst du die Tonleiter aus dem Synth? → Spalte **Note**.
6. **Test Pattern = Pitch Bend** → Bewegt sich das Pitch-Rad, jault der Ton? → Spalte **Pitchbend**.
7. **Test Pattern = CC Sweep**, CC Number = 1 → Bewegt sich das Mod-Rad? → Spalte **CC**.
8. Optional: **MIDI Channel = 2** einstellen. Synths, die auf allen Kanälen hören (Vital, Surge XT im Standard), spielen weiter. Wenn es danach still ist, notier das – es ist kein Fehler, aber gut zu wissen.
9. Screenshot von beiden Wrapper-Einstellungen (Output port 5, Input port 5) und vom VocalSynth-Fenster mit den Zählern.

### T2 – VocalSynth FX → nativer FL-Synth über Patcher

Native FL-Synths (FLEX, Sytrus, 3x Osc …) haben laut Recherche **keinen** MIDI-Input-Port. Man erreicht sie über **Patcher**.

1. VocalSynth FX aus T1 bleibt auf Insert 1 mit **Output port 5**. Beim Vital aus T1 den **Input port zurück auf aus** stellen (sonst spielt er mit).
2. Channel Rack → „+“ → **Patcher** hinzufügen und öffnen.
3. Im Patcher: Rechtsklick auf eine freie Fläche → einen nativen Synth hinzufügen, z. B. **FLEX**.
4. Rechtsklick auf den Knoten **„From FL Studio“** → *Outputs* → *Events* → den Eintrag für **MIDI-Port 5** aktivieren (unbestätigt, wie der Eintrag in FL 2026 genau heißt). Am Knoten erscheint ein neuer Ausgang.
   - Gibt es diesen Menüpunkt nicht oder keinen Eintrag für Port 5 → in der Bemerkung notieren („Patcher-Knoten nicht sichtbar“).
5. Diesen neuen Ausgang mit dem **Event-Eingang** von FLEX verbinden (ziehen). Der Audio-Ausgang von FLEX muss mit **„To FL Studio“** verbunden sein (macht Patcher meist automatisch).
6. In VocalSynth nacheinander **Notes**, **Pitch Bend**, **CC Sweep** wählen (jeweils Restart pattern) und wie in T1 prüfen. Für CC: FLEX/Sytrus haben kein sichtbares Mod-Rad. Weise einen Regler per Rechtsklick → *Link to controller…* zu, oder notiere „CC nicht prüfbar“ **(unbestätigt, ob „Link to controller“ hier auf Port-MIDI reagiert)**.
7. Screenshot vom Patcher mit dem Kabel.

### T3 – VocalSynth Instrument mit Mikro über Sidechain

Hier ist VocalSynth ein **Generator** im Channel Rack. Die große Frage ist, ob das Mikro-Audio überhaupt beim Instrument ankommt.

1. Den T1/T2-Aufbau abbauen oder Ports auf aus stellen (VocalSynth FX auf Insert 1 entfernen oder Test Pattern = Off, damit nichts doppelt kommt).
2. Channel Rack → „+“ → **VocalSynth Instrument** (VST3). Kopfzeile: „VocalSynth Instrument - M1 routing test build“, „Format: VST3“.
3. Dem Instrument einen eigenen Mixer-Insert geben: im Channel Rack auf das kleine Zahlenfeld (Mixer-Track) links neben dem Kanalnamen ziehen, z. B. auf **Insert 2**.
4. Das Mikro liegt auf **Insert 1** (wie in T1). Im Mixer **Insert 1 auswählen**. Dann bei **Insert 2** unten auf den kleinen Routing-Pfeil **rechtsklicken** → **„Sidechain to this track“**.
5. Im VocalSynth-Instrument-Fenster: Zahnrad → Bereich *Processing* → bei den Eingängen (Input) **Insert 1** dem Eingang „Sidechain“ bzw. Input 1/2 zuordnen **(unbestätigt – genau das ist der Test)**.
6. **Prüfen Audio:** In das Mikro sprechen. Bewegt sich die Pegelanzeige im Instrument? Was steht bei **„Input: N ch“**?
   - Pegel bewegt sich → Audio kommt an. In die Bemerkung: „Audio-in ja“.
   - Pegel bleibt still oder „Input: 0 ch“ → Audio kommt nicht an. In die Bemerkung: „Audio-in nein“ und was du probiert hast.
7. **Prüfen MIDI:** Im Instrument-Fenster: Zahnrad → *MIDI* → **Output port = 6**. Beim Vital: **Input port = 6**. Dann Notes / Pitch Bend / CC Sweep wie in T1 prüfen.
8. Bonus: **Monitor Tone** an – der Sinuston muss auf Insert 2 zu hören sein (das ist schon ein Stück T6).

### T4 – Kompletter Aufbau in Patcher auf dem Mikro-Insert

Alles in einem Patcher: Mikro → VocalSynth FX → MIDI-Kabel → Synth. Das wäre die robusteste Einzelspur-Lösung.

1. Vorherige Aufbauten abbauen oder stummschalten (keine anderen Ports auf 5/6 aktiv).
2. Mixer → **Insert 1** (Mikro) → freier Effekt-Slot → **Patcher**.
3. Im Patcher: Rechtsklick → **VocalSynth FX** hinzufügen, außerdem **Vital** (bzw. einen anderen Synth).
4. Verbinden:
   - Audio von „From FL Studio“ → Audio-Eingang von VocalSynth FX
   - **MIDI-/Event-Ausgang von VocalSynth FX → Event-Eingang von Vital**
   - Audio-Ausgang von Vital → „To FL Studio“
5. Hat VocalSynth FX im Patcher **keinen** MIDI-Ausgang? Dann das VocalSynth-Fenster im Patcher öffnen → Zahnrad → *MIDI* → einen **Output port** setzen (z. B. 7) und schauen, ob der Ausgang dann erscheint. Notieren, was nötig war.
6. Notes / Pitch Bend / CC Sweep wie in T1 prüfen.
7. Screenshot vom Patcher.

### T5 – Dieselben Tests mit dem CLAP-Build

Der CLAP-Build ist technisch ein eigenes Plugin. Ob FL dessen MIDI in seine Ports weiterleitet, ist offen.

1. In FL die **CLAP**-Variante von VocalSynth FX laden (im Plugin-Browser bzw. Plugin-Manager ist das Format angegeben). Kontrolle: Kopfzeile im Plugin zeigt **„Format: CLAP“**.
2. Mindestens den **T1-Aufbau** wiederholen: CLAP-FX auf Insert 1, Output port 5 (falls der CLAP-Wrapper überhaupt ein MIDI-Port-Feld hat – wenn nicht, notieren), Vital Input port 5. Der Empfänger darf ruhig VST3 bleiben.
3. Wenn Zeit ist: auch T3 (CLAP-Instrument) und T4 (CLAP-FX im Patcher) wiederholen.
4. Jede Variante bekommt eine eigene Zeile in der Ergebnistabelle.

### T6 – Interner Ton, ganz ohne Routing

Das muss immer funktionieren und ist die Kontrolle, dass das Plugin selbst richtig läuft.

1. VocalSynth FX auf einem Insert (z. B. Insert 1). **Mute Dry** an, damit du nur den Testton hörst.
2. **Monitor Tone** an. **Test Pattern = Notes** → du hörst eine Sinus-Tonleiter direkt aus dem Mixer-Kanal.
3. **Pitch Bend** → der Sinuston bewegt sich um ±2 Halbtöne.
4. **CC Sweep** → der Sinuston bleibt gleich (der Testton reagiert absichtlich nicht auf CC). In der CC-Spalte „–“ eintragen.
5. Dasselbe kurz mit **VocalSynth Instrument** im Channel Rack: dort ist der Testton der einzige Ausgang.
6. Achte auf Knackser, Aussetzer oder verzerrten Klang und notiere sie.

Gut zu wissen (kein Fehler):
- Bei Pitch Bend und CC Sweep wird Note 60 jeden Takt neu angeschlagen. Am Testton hörst du das nicht – er klingt einfach weiter. Der Neuanschlag ist nur für externe Synths gedacht.
- Der Testton spielt auf jedem MIDI-Kanal gleich; die Einstellung **MIDI Channel** ändert an ihm nichts.
- Ist VocalSynth FX gebypasst, hörst du deine Stimme unverändert, auch wenn **Mute Dry** an ist. Bypass heißt „Plugin aus“.

### T7 – Virtueller MIDI-Port → normale Aufnahme in die Piano Roll

FL kann Plugin-MIDI nicht direkt aufnehmen. Ein virtueller MIDI-Port umgeht das: VocalSynth meldet sich bei Windows als MIDI-Gerät, und FL nimmt es wie ein Keyboard auf.

**Voraussetzung:** Windows 11 24H2 oder 25H2 mit **Windows MIDI Services** (wird seit Februar 2026 über Windows Update ausgerollt). Deine Windows-Version siehst du mit `Win + R` → `winver`.

1. Alle anderen Routings abbauen (Input ports der Synths auf aus), sonst kommen Noten doppelt.
2. VocalSynth FX auf Insert 1 (VST3). **Virtual MIDI Out** einschalten. Nach kurzer Zeit muss der Status **`open: VocalSynth FX`** zeigen.
   - Status **`failed`**: Windows konnte den Port nicht anlegen (meist fehlen Windows MIDI Services). Notieren – T7 ist dann auf diesem PC nicht möglich. Das Plugin versucht es alle paar Sekunden erneut; es darf nicht abstürzen.
3. FL: *Options → MIDI settings* (`F10`, Reiter MIDI). In der Liste **Input** sollte **„VocalSynth FX“** auftauchen.
   - Fehlt der Eintrag: Fenster schließen und neu öffnen bzw. „Rescan devices“/„Refresh device list“ (falls vorhanden). Notiere, ob FL den Port erst nach einem Neustart sieht – das wäre ein wichtiges Ergebnis, weil der Port nur existiert, solange das Plugin geladen ist.
4. Den Eintrag anklicken und **Enable** einschalten. *Controller type*: „(generic controller)“.
5. Im Channel Rack den **Vital-Kanal auswählen** (FL schickt MIDI-Eingänge an den ausgewählten Kanal). **Test Pattern = Notes** → Vital spielt die Tonleiter, ohne dass irgendein Port im Wrapper gesetzt ist. → Spalte Note.
6. **Aufnahme:** In der Transportleiste Aufnahme (●) aktivieren, als Aufnahmeart Noten/„Everything“ wählen, Play drücken, 2–4 Takte laufen lassen, Stopp. Piano Roll von Vital öffnen: Sind die Noten da?
   - Das Muster läuft frei und ist nicht mit FL synchronisiert. Die Noten liegen deshalb nicht genau auf dem FL-Raster, aber sie sollten **gleichmäßige Abstände** haben. Kleine Schwankungen (wenige Millisekunden) sind beim virtuellen Port normal (siehe Abschnitt 8).
7. Pitch Bend und CC Sweep ebenso aufnehmen. In der Piano Roll unten im Event-Bereich nachsehen (Pitch bzw. Mod wheel / CC). Notiere, was aufgenommen wurde.
8. **Mehrere Instanzen:** Ein zweites VocalSynth FX auf einen anderen Insert legen und dort Virtual MIDI Out einschalten. Status sollte **`open: VocalSynth FX 2`** zeigen, und in FL sollte ein zweiter Eingang auftauchen. Notieren.
9. Virtual MIDI Out wieder ausschalten: Der Synth darf keinen hängenden Ton behalten.

---

## 5. Zusatzchecks

Diese Checks sichern spätere Funktionen ab (Capture, Analyse). Ergebnisse in die zweite Tabelle in Abschnitt 6.

**Drag & Drop**

1. FL auf 120 BPM, VocalSynth auf **Test Pattern = Notes** (bei „Off“ wird ebenfalls die Tonleiter exportiert).
2. **„Drag test MIDI“** mit der Maus greifen und auf einen Kanal im Channel Rack bzw. in die Piano Roll ziehen. Erwartung: 4 Takte, 32 Noten, Tonleiter in Achteln.
3. **„Drag test WAV“** in die **Playlist** ziehen. Erwartung: ein Audio-Clip, 2 Sekunden Sinuston (440 Hz).
4. Notieren, ob der Clip im Takt richtig liegt und ob FL meckert.

**Blockgrößen** (die künftige Stimmanalyse muss mit allen Größen klarkommen)

1. Im VocalSynth-Fenster das Histogramm zurücksetzen, ca. 10 Sekunden warten (Play in FL ist egal, aber probier auch einmal mit laufendem Song).
2. Notieren: min, max, letzter Wert und welcher Balken am größten ist.
3. Zahnrad → *Processing* → **„Use fixed size buffers“** einschalten. Histogramm zurücksetzen, 10 Sekunden warten, wieder notieren.
4. In *Options → Audio settings* den ASIO-Puffer während des Betriebs ändern (z. B. 256 → 512 → 128). Das Plugin muss weiterlaufen, ohne Absturz und ohne hängende Töne.

**Gemeldete Latenz**

- VocalSynth meldet 0 Samples Latenz. In den Wrapper-Einstellungen (*Processing*, Bereich Latenz/PDC) sollte keine Verzögerung eingetragen sein, und andere Spuren dürfen sich durch das Laden von VocalSynth nicht verschieben. Notieren, was FL anzeigt.

**Hängende Noten** (einmal mit VST3, einmal mit CLAP)

Aufbau wie in T1 (bzw. T5), **Test Pattern = Pitch Bend**, damit ein Ton gehalten wird. Dann jeweils prüfen, ob der Vital-Ton **sofort aufhört** und das Pitch-Rad **in die Mitte** zurückgeht:

1. Effekt-Slot von VocalSynth **deaktivieren/bypassen** (grüne LED am Slot ausschalten).
2. Wieder einschalten, dann **Test Pattern = Off**.
3. Wieder Pitch Bend, dann VocalSynth aus dem Slot **entfernen**.
4. Wieder laden, Pitch Bend, dann ASIO-Puffer oder Samplerate ändern.

Hängt ein Ton: notieren, bei welchem Schritt. Beenden kannst du ihn, indem du den Synth-Kanal kurz entfernst und neu lädst oder das Projekt neu öffnest.

**Speichern und Laden**

- Nicht-Standard-Werte einstellen (z. B. Test Pattern = All, MIDI Channel = 3, Test tempo = 97.5, CC Number = 74, Monitor Tone an). Projekt speichern, FL schließen, Projekt neu öffnen. Stehen alle Werte noch so da?

---

## 6. Ergebnisse (zum Ausfüllen)

**Testumgebung**

| | |
|---|---|
| Datum | 2026-10-15 |
| FL-Studio-Version (*Help → About*) | |
| VocalSynth-Version und Commit (aus dem ZIP-Namen) | 0.1.0 / 00608dd |
| Windows-Version (`winver`) | |
| Audio-Interface, ASIO-Puffer, Samplerate | |
| Empfänger-Synth(s) mit Version | VST3-Synth (Vital o. ä.) |

**Routing-Tests**

Eintragen: **ja** / **nein** / **teils** (z. B. „nur manchmal“, „verzögert“) / **–** (nicht anwendbar oder nicht getestet).

| Test | Note | Pitchbend | CC | Bemerkung |
|---|---|---|---|---|
| T1 – FX im Mixer, Port 5 → VST3-Synth | ja | ja | ja | Alles kommt an. Primärer Weg (siehe Abschnitt 7). |
| T2 – FX → nativer Synth über Patcher | | | | |
| T3 – Instrument mit Sidechain (Audio-in: ja/nein) | | | | |
| T4 – alles in Patcher auf dem Mikro-Insert | | | | |
| T5 – CLAP, Aufbau T1 | | | | |
| T5 – CLAP, Aufbau T3 | | | | |
| T5 – CLAP, Aufbau T4 | | | | |
| T6 – interner Monitor-Ton (FX und Instrument) | ja | ja | – | CC hat beim Sinuston erwartungsgemäß keine hörbare Wirkung. |
| T7 – virtueller Port, live gespielt | | | | |
| T7 – virtueller Port, in Piano Roll aufgenommen | | | | |
| T7 – zweite Instanz („VocalSynth FX 2“) | | | | |

**Aufnahme**

| Check | Ergebnis | Bemerkung |
|---|---|---|
| Plugin-MIDI aus T1 direkt in die Piano Roll des Synths aufnehmen | nein | Wie in RESEARCH 2.2 erwartet: FL nimmt Plugin-MIDI nicht auf. Die eigene Aufnahme mit Drag & Drop (M3) bleibt nötig. |

T2–T5 und T7 wurden nicht getestet; sie sind nach dem Erfolg von T1 optional.

**Zusatzchecks**

| Check | Ergebnis | Bemerkung |
|---|---|---|
| Drag & Drop `.mid` in Channel Rack / Piano Roll | | |
| Drag & Drop `.wav` in die Playlist | | |
| Blockgrößen ohne „fixed size buffers“ (min / max / häufigster Bereich) | | |
| Blockgrößen mit „fixed size buffers“ (min / max / häufigster Bereich) | | |
| ASIO-Puffer im Betrieb ändern | | |
| Gemeldete Latenz 0, nichts verschoben | | |
| Hängende Noten VST3 (Bypass / Off / Entfernen / Puffer) | | |
| Hängende Noten CLAP (Bypass / Off / Entfernen / Puffer) | | |
| Speichern und Laden der Testparameter | | |
| Abstürze, Knackser, sonstiges | | |

Die ausgefüllten Tabellen kannst du direkt hier in die Datei eintragen oder als Text bzw. Screenshot zurückschicken.

---

## 7. Entscheidungsregel

Aus [PLAN.md, M1](PLAN.md#m1--fl-studio-routing-spike-gono-go-gate-12-wochen). Die erste Zeile, die auf die Ergebnisse passt, gilt.

| Ergebnis | Primärer Weg | Folge für den Plan |
|---|---|---|
| T1 o.k. (Note + PB + CC) | FX-Target im Mic-Insert, Port-Routing | Plan wie vorgesehen |
| T1 scheitert, T3 o.k. | Instrument-Target mit Sidechain | FX-Target bleibt sekundär |
| nur T4 (Patcher) o.k. | Patcher-Rezept als Standard | RoutingHelp in M7 aufwerten |
| T5 besser als VST3 | CLAP für FL priorisieren | CLAP-Tests in die CI-Pflicht |
| T1–T5 scheitern | interne Engine (T6) + Capture + T7 | **M6 rückt direkt hinter M2** |

Unabhängig davon: Klappt T7, wird der virtuelle Port als optionaler Weg zum direkten Aufnehmen weitergeführt. Capture mit Drag & Drop (M3) kommt in jedem Fall.

**Entscheidung** (nach den Tests ausfüllen)

- Primärer Weg:
- Begründung:
- Datum:

### Rezept für den gewählten Weg

*Wird nach den Tests ergänzt: die genaue Klickfolge für den Weg, der funktioniert hat, mit Screenshots. Diese Anleitung ersetzt dann den Abschnitt „Wie es in FL Studio funktionieren soll“ in der README.*

---

## 8. Bekannte Einschränkungen des Testbuilds

- **Keine Stimmanalyse.** Das MIDI ist ein festes Testmuster. Das Mikro wird nur angezeigt (Pegel) und beim FX-Plugin durchgereicht.
- **Eigenes Tempo, nicht mit FL synchronisiert.** Das Muster läuft mit „Test tempo (BPM)“ und einem eigenen Zähler, unabhängig von Play/Stopp und Songposition in FL. Aufgenommene Noten liegen deshalb nicht auf dem FL-Raster.
- **Virtueller Port mit leichtem Zeitversatz.** Die Nachrichten gehen in Paketen pro Audio-Block raus. Der Zeitversatz liegt in der Größenordnung eines Audio-Puffers (bei 256 Samples und 48 kHz ca. 5 ms) plus ca. 1 ms.
- **CLAP und Bypass.** Beim CLAP-Build ruft FL beim Bypass das Plugin einfach nicht mehr auf. VocalSynth kann dann keine Note-Offs mehr schicken; ob ein Ton hängen bleibt, hängt von FL ab. Genau deshalb wird der Hängende-Noten-Check auch mit CLAP gemacht.
- **CLAP und Automation.** Beim CLAP-Build teilt der Wrapper Audio-Blöcke an Automationspunkten. Das Histogramm zeigt dann kleinere Blöcke; das ist normal.
- **Monitor-Ton ignoriert CC.** Der Sinuston folgt nur Noten und Pitchbend.
- **Unsigniert.** Windows zeigt ggf. die SmartScreen-Warnung („Weitere Informationen → Trotzdem ausführen“).
