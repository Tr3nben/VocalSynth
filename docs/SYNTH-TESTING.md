# Den eigenen Synth testen (Version 0.4.0 „Eigener Klang“)

Mit **VocalSynth 0.4** hat das Plugin einen **eigenen Wavetable-Synth**. Du singst, VocalSynth macht daraus Noten
(wie bisher) und spielt sie **direkt im Plugin** – ohne MIDI-Routing, ohne zweites Plugin. Zusätzlich hört der
Synth auf deine Stimme: **Lautstärke, Helligkeit, Hauchigkeit und Tonhöhen-Abweichung** (Vibrato) steuern den Klang.
Der Weg über den MIDI-Port zu externen Synths bleibt genau wie in 0.3.

Die Bedienelemente im Plugin sind englisch beschriftet, Menünamen von FL so, wie sie in der englischen
Oberfläche heißen. Wo ein Schritt nicht sicher belegt ist, steht **(unbestätigt)** dabei.

Zeitbedarf: Kurztest (Abschnitt 2 + Probehör-Auswahl) ca. 20 Minuten, ganze Checkliste ca. 60–90 Minuten.

---

## Inhalt

1. [Was ist neu](#1-was-ist-neu)
2. [Schnellstart: singen und hören](#2-schnellstart-singen-und-hören)
3. [Die Seite „Synth“ in zwei Minuten](#3-die-seite-synth-in-zwei-minuten)
4. [Preset-Tour: worauf du achten sollst](#4-preset-tour-worauf-du-achten-sollst)
5. [Test-Checkliste für FL Studio](#5-test-checkliste-für-fl-studio)
6. [Probleme melden](#6-probleme-melden)
7. [Bekannte Einschränkungen von 0.4](#7-bekannte-einschränkungen-von-04)

---

## 1. Was ist neu

- Neuer Reiter **„Synth“** (vierter Reiter nach *Voice*, *Test & Routing*, *Capture*): 2 Wavetable-Oszillatoren
  mit Unison, Sub, Rauschen, Filter mit Drive, 3 Hüllkurven, 2 LFOs, 8 Modulations-Slots, Chorus/Delay/Reverb.
- **24 Factory-Presets** (Init + 23 Klänge), eigene Presets speichern (Ordner `Dokumente\VocalSynth\Presets`).
- **Neu in 0.4.1:** **16 weitere Presets für Trap und R&B** (zusammen 40). Im Preset-Menü stehen sie bei ihrer
  Kategorie (Lead, Bass …) und zusätzlich unter **Genres → Trap** bzw. **Genres → R&B** (Abschnitt 4,
  „Neu in 0.4.1“).
- **14 eingebaute Wavetables** und **Import eigener WAV-Wavetables** (die Tabelle wird im Projekt gespeichert,
  das Projekt klingt also auch auf einem anderen Rechner gleich).
- **Neue Instanzen:** Synth **an**, trockene Stimme **an** – du hörst sofort etwas. Der Reiter *Voice* sagt es
  unten links („Internal synth: ON …“); wer weiter einen externen Synth über den MIDI-Port spielt, schaltet
  *Synth On* (Reiter *Synth*, Bereich *MASTER*) aus. **Alte 0.3-Projekte:** Synth **aus**, alles klingt wie vorher.
- Das Fenster ist skalierbar (75–200 %).

---

## 2. Schnellstart: singen und hören

1. **Kopfhörer** aufsetzen (sonst singt der Synth über die Lautsprecher ins Mikro zurück).
2. Mixer öffnen (F9), das Insert mit deinem Mikro wählen (z. B. **Insert 1**, Eingang = Mikro wie in
   `FL-ROUTING.md`, T1).
3. In einen freien Effekt-Slot dieses Inserts **VocalSynth FX** laden.
4. Singen. Du hörst deine Stimme **und** den Synth (Preset **Init**: eine einfache Säge).
5. Reiter **Synth** öffnen. Im Bereich **MASTER**:
   - **Synth On** – Synth an/aus.
   - **Volume** – Lautstärke des Synths (Standard −6 dB). Presets ändern diesen Regler nie.
   - **Dry** – Lautstärke deiner trockenen Stimme (0 dB = unverändert, ganz links = stumm).
     **Mute Dry** (Reiter *Test & Routing*) schaltet die Stimme wie bisher hart stumm.
   - Oben rechts neben **Save**: **Synth DSP** – wie viel Rechenzeit der Synth braucht (Durchschnitt und
     Spitze). Die Anzeige *VocalSynth DSP* im Reiter *Voice* zeigt das ganze Plugin (Analyse + Synth).
6. Mit den Pfeilen **< >** neben dem Preset-Namen durch die Presets schalten, oder auf den Namen klicken
   (Menü nach Kategorie und – ab 0.4.1 – nach Genre: *Trap*, *R&B*).

Tipp: Die meisten Presets sind auf **gleiche Lautheit** abgestimmt (±1 dB, Init ist etwas lauter). Wenn
ein Preset deutlich leiser oder lauter wirkt, ist das ein Befund – bitte notieren.

> Hinweis: „Reset to default“ von FL (Rechtsklick auf das Plugin) schaltet den Synth **an** (der Standardwert
> von *Synth On* ist „an“, wie bei einer neuen Instanz). Alte 0.3-Projekte laden trotzdem mit Synth **aus**.

---

## 3. Die Seite „Synth“ in zwei Minuten

- **Mode** (Bereich *VOICE*): **Legato** (Standard) – eine Stimme, beim Notenwechsel läuft der Ton ohne neuen
  Anschlag weiter; mit **Glide** > 0 gleitet er zur neuen Note (bei *Init* steht Glide auf 0, also kein Gleiten).
  **Mono** – jede Note setzt neu an (gut für „gezupfte“ Bässe). **Poly** – bis 16 Stimmen;
  beim Singen klingen alte Noten aus, während die neue schon spielt (Flächen, Keys).
- **Vier Anzeigen** (*EXPRESSION* im Bereich *VOICE*): **Level** (wie laut du singst), **Bright** (hell „ii“ ↔
  dunkel „uu“), **Noise** (Hauchen, Zischen, „sch“), **Pitch** (wie weit du neben der Note liegst, z. B. Vibrato).
  Diese vier sind in der **Mod-Matrix** als *Voice Level*, *Voice Brightness*, *Voice Noisiness*, *Voice Pitch*
  wählbar. Die Anzeigen bewegen sich nur im Modus *Voice* (nicht beim Test-Pattern).
- **OSC A / OSC B**: Tabelle wählen (Menü über der Wellenform), **Position** fährt durch die Tabelle.
  *Import WAV* lädt eine eigene Tabelle.
- **FILTER**, **ENV 1** (Lautstärke, mit **Gain** = Preset-Pegel), **ENV 2** (Filter), **ENV 3** (frei),
  **LFO 1/2**, **Mod Matrix** (8 Zeilen: Quelle → Ziel, Stärke), **FX** (Chorus, Delay, Reverb).

---

## 4. Preset-Tour: worauf du achten sollst

**Probehör-Auswahl** (kommt zuerst als Hörprobe-ZIP): **Init** und die 8 mit ★ markierten Presets. Wenn du nur
wenig Zeit hast: nur diese.

Bei allen Presets gilt: Sing einmal **leise und laut**, einmal **„ii“ und „uu“**, einmal **gehaucht**, einmal mit
**Vibrato**, und eine schnelle Tonfolge (Legato). Notiere pro Preset kurz: *gut / brauchbar / billig / zu leise /
zu laut / komisch* (+ ein Wort warum).

### Init
| Preset | Modus | Worauf achten |
|---|---|---|
| Init | Legato | Neutraler Startpunkt: Sägezahn, Filter offen, keine Effekte, keine Stimm-Modulation. Klingt „roh“ – das ist richtig. |

### Lead (einstimmige Melodie-Sounds)
| Preset | Modus | Worauf achten |
|---|---|---|
| ★ Voice Lead | Legato | Warme Säge mit kurzem Glide. **Lauter singen = heller** (Level → Filter), „ii“ macht ihn zusätzlich etwas heller. Leichter Chorus und Ping-Pong-Delay. |
| ★ Supersaw Lead | Legato | Breite, helle Supersaw (7 Stimmen) + Oktave darüber. **Helle Vokale** bringen die Oktave stärker. Klingt sie breit, aber nicht schwammig? |
| PWM Lead | Legato | Rechteck mit langsam wanderndem Pulsbreiten-Wackeln und einem **Vibrato, das erst nach ca. 0,7 s einsetzt**. Lauter = heller. |
| Sync Lead | Legato | Aggressiver Sync-Sound: zu Beginn jeder neu angesetzten Phrase ein „Zwitschern“, **helle Vokale** schärfen ihn. |
| Brass Lead | Legato | Blechbläser-artig: Filter öffnet sich nach dem Einsatz, **lauter singen = „blechiger“**. Ist der Einsatz weich genug? |
| Whistle | Legato | Pfeifen eine Oktave über deiner Stimme mit Gleiten. **Hauchen** gibt Luftgeräusch dazu, verzögertes Vibrato. |

### Bass (eine Oktave unter deiner Stimme)
| Preset | Modus | Worauf achten |
|---|---|---|
| Sub Bass | Legato | Tiefer, runder Bass. Auf Laptop-Lautsprechern kaum hörbar – **mit Kopfhörer** prüfen. Lauter = etwas mehr Knurren. |
| ★ Reese Bass | Legato | Zwei verstimmte Sägen, langsames Schweben. Lauter = offener. Klingt es fett, aber nicht matschig? |
| ★ Acid Bass | Mono | Resonantes „Quietschen“ bei **jeder** Note (Mono setzt jede Note neu an), Glide bei verbundenen Noten. Lauter und heller = mehr „Squelch“. Kratzt es unangenehm? |
| Pluck Bass | Mono | Gezupfter, kurzer Bass. Kurze, rhythmische Töne singen („dm dm dm“). |
| FM Bass | Legato | Knurriger FM-Bass. **„ii“ = mehr Knurren**, „uu“ = runder. |

### Pad (Flächen, Poly)
| Preset | Modus | Worauf achten |
|---|---|---|
| ★ Warm Pad | Poly | Weiche Fläche mit langsamem Einschwingen und langem Ausklang, Chorus und Hall. Beim Melodiesingen überlappen die Töne zu Akkorden. Ist der Einsatz zu langsam für dich? |
| ★ Choir Pad | Poly | Chor-artige Vokal-Fläche. **„ii“ ↔ „uu“** ändert den Vokal des Chors, lauter = etwas lauter, Hauchen = etwas Luft. |
| Glass Pad | Poly | Glockige, glitzernde Fläche, langsam schwebend, sehr langer Ausklang mit Delay und Hall. Helle Vokale = mehr Glitzern. |

### Pluck/Keys (Poly)
| Preset | Modus | Worauf achten |
|---|---|---|
| Poly Pluck | Poly | Kurzer, gezupfter Ton mit Delay. Gut für schnelle, kurze Noten. |
| FM E-Piano | Poly | E-Piano-artig: heller Anschlag, der weich wird. Leicht helle Vokale = etwas glockiger. |
| Drawbar Organ | Poly | Orgel mit schnellem Chorus (Leslie-artig). **Helle Vokale wechseln die Register** (andere Zugriegel-Mischung). Kein Einschwingen, kurzer Ausklang. |

### Vocal (der Klang folgt deiner Stimme am stärksten)
| Preset | Modus | Worauf achten |
|---|---|---|
| ★ Vowel Morph | Legato | Der Synth „spricht“ Vokale: **„uu“ → U, „oo“ → O, „ii“ → I**. Hörst du den Unterschied deutlich? |
| ★ Talking Saw | Legato | Filter-Wah aus der Stimme: **helle Vokale und lauteres Singen öffnen das Filter**. Sing „wa-wa-wa“ oder „ju-ju-ju“. |
| Breathy Voice | Legato | Weicher, hauchiger Klang. **Je mehr du hauchst, desto mehr Luft** im Synth. Klingt das natürlich oder nach Rauschen? |
| Vibrato Wah | Legato | **Dein eigenes Vibrato** öffnet und schließt das Filter („wah-wah“). Mit und ohne Vibrato singen und vergleichen. |

### FX
| Preset | Modus | Worauf achten |
|---|---|---|
| Space Drift | Poly | Atmosphäre: sich ständig verändernde Fläche, langes Delay und großer Hall. Hauchen gibt Rauschen dazu. Für Intros/Übergänge. |
| Glitch | Legato | Digitaler, rhythmischer Klang im Song-Tempo (1/16), Delay im Tempo. **Hauchen/Zischen** gibt mehr Delay. Mit laufendem Transport bei verschiedenen Tempi prüfen. |

### Neu in 0.4.1: Trap und R&B

Alle 16 sind gleich laut wie die anderen Presets (−18 LUFS) und reagieren auf deine Stimme. Im Menü:
**Genres → Trap** / **Genres → R&B** (rechts steht die Kategorie). Mit **< >** kommen sie nach *Glitch*.
Kurztest, wenn wenig Zeit ist: **808 Glide Bass**, **Trap Bell**, **Trap Flute**, **Neo-Soul Keys**,
**Smooth R&B Lead**, **Talkbox Lead**.

**Trap**
| Preset | Kategorie / Modus | Worauf achten |
|---|---|---|
| Trap Flute | Lead / Legato | Flöte eine Oktave über deiner Stimme, verzögertes Vibrato. **Hauchen = mehr Atemgeräusch**, lauter = etwas „überblasen“ (heller). |
| Brass Stab | Lead / Mono | Kurzer Blech-Stoß, **jede Note setzt neu an** (Mono). Kurze, rhythmische Silben singen („ta ta“); lauter = blechiger. |
| ★ 808 Glide Bass | Bass / Legato | 808 eine Oktave unter deiner Stimme mit **Glide 110 ms**: **sing einen Slide** (Ton hochziehen, ohne abzusetzen) – der 808 muss mitgleiten, ohne neu anzuschlagen. Neue Töne (nach einer Pause) haben einen kurzen „Punch“. **Lauter = mehr Verzerrung** – auch auf Laptop-/Handy-Lautsprechern prüfen: bleibt der Bass hörbar und sauber? |
| 808 Distorted | Bass / Legato | Wie oben, aber stark verzerrt (fast Rechteck), Glide 90 ms. Helle Vokale = offener. Ist es zu kratzig? |
| Dark Atmos | Pad / Poly | Dunkle, langsame Fläche (Einschwingen ca. 1 s), wandernd, großer Hall. Helle Vokale öffnen das Filter, Hauchen gibt Rauschen dazu. |
| ★ Trap Bell | Pluck/Keys / Poly | Helle, kurze Glocke eine Oktave höher (klingt auch bei gehaltenem Ton aus). **„ii“ = heller**, Hauchen = mehr Delay. Schnelle Melodien singen. |
| Dark Pluck | Pluck/Keys / Poly | Dunkler, weicher Zupf-Sound mit Delay. Helle Vokale = offener. |
| Glass Arp | Pluck/Keys / Poly | Gläserner, kurzer Pluck mit 1/16-Ping-Pong-Delay im Song-Tempo – schnelle, kurze Töne („ti-ti-ti“) singen. Helle Vokale = mehr Glas. |

**R&B**
| Preset | Kategorie / Modus | Worauf achten |
|---|---|---|
| ★ Smooth R&B Lead | Lead / Legato | Weiche Säge/Sinus-Mischung. **Dein Vibrato wird etwas breiter** (Voice Pitch → Pitch) – mit und ohne Vibrato singen. Lauter = heller. |
| Slow Jam Lead | Lead / Legato | 90er-Slow-Jam-Lead: **gleitet zwischen allen Tönen** (Glide 140 ms, auch nach Pausen), verzögertes Vibrato. Helle Vokale = näselnder (schmalerer Puls). |
| 90s R&B Bass | Bass / Legato | Runder, leicht resonanter Synth-Bass mit kurzem Filter-„Pluck“. Lauter = offener. |
| Silk Pad | Pad / Poly | Warme, weiche Fläche mit langsamem Einsatz und breitem Chorus. Helle Vokale = offener, lauter = etwas Glanz (Oktave). |
| Airy Choir | Pad / Poly | Luftiger Chor. **„ii“ ↔ „uu“** ändert den Vokal, **Hauchen = mehr Luft**. |
| ★ Neo-Soul Keys | Pluck/Keys / Poly | Rhodes-artiges E-Piano: Anschlag „bellt“ kurz, **Stereo-Tremolo** (wandert links/rechts). Helle Vokale = etwas mehr Biss. |
| Bell Keys | Pluck/Keys / Poly | Glockiges E-Piano mit Chorus, längerer Ausklang. „ii“ = heller. |
| ★ Talkbox Lead | Vocal / Legato | Talkbox-artig: **je heller du singst, desto mehr wandert der Vokal von A über E zu I** („uu“/„oo“ → A, „ii“ → I), lauter = offener. „wa-ji-wa“ oder „you-yeah“ singen. |

---

## 5. Test-Checkliste für FL Studio

Bitte die Punkte der Reihe nach, Ergebnis jeweils mit **ok / nicht ok / übersprungen** + kurzer Notiz.

1. **Altes Projekt:** Ein Projekt mit VocalSynth 0.3.1 öffnen. Klingt es wie vorher (Synth aus)? Sind alle
   Einstellungen (Voice, Test, Capture) noch da?
2. **Neue Instanz auf dem Mikro-Insert:** VocalSynth FX neu laden (Abschnitt 2). Synth ist an, singen → Synth
   hörbar **ohne** Routing. **Dry** ganz runter und wieder hoch, **Mute Dry** an/aus. Fühlt sich die Verzögerung
   wie bisher an (keine zusätzliche Latenz)?
3. **Presets durchschalten:** Mindestens 10 Presets aus Lead, Bass, Pad und Vocal (oder die Probehör-Auswahl),
   Eindruck pro Preset notieren (Abschnitt 4).
4. **Stimm-Modulation:** Preset **Vowel Morph** und **Talking Saw**: lauter/leiser, hell/dunkel („ii“ vs. „uu“),
   Zischen/Hauchen (**Breathy Voice**) – hört man es? Bewegen sich die vier Anzeigen (*EXPRESSION*)?
5. **Eigene Wavetable:** Bei OSC A *Import WAV* wählen und eine WAV laden (selbst erzeugt oder mit klarer
   Lizenz, z. B. ein Single-Cycle oder eine Serum-artige 2048er-Tabelle). Hat die Datei keine Serum-Kennung und
   lässt sich die Frame-Größe nicht sicher erkennen (z. B. Länge 4096, 16384 …), fragt VocalSynth nach der
   **Frame-Größe**: Serum-/Vital-Tabellen → **2048** (vorausgewählt), WaveEdit-Tabellen (64 × 256) → **256**,
   ein einzelner Zyklus mit 4096 Samples → **4096**. Klingt die Tabelle Oktaven zu hoch, war die Größe falsch –
   noch einmal mit einer kleineren Größe importieren. **Position** drehen – ändert sich der
   Klang? Projekt speichern, FL schließen, Projekt wieder öffnen → Tabelle und Klang wieder da?
6. **Automation:** In FL für **Filter Cutoff** und **Osc A Position** je einen Automation-Clip anlegen. Ein
   Rechtsklick auf einen VocalSynth-Regler öffnet **kein** FL-Menü (das ist so). FLs Weg: den Regler im Plugin
   kurz bewegen, dann im FL-Hauptmenü **Tools → Last tweaked → Create automation clip**. Alternativ
   (unbestätigt): im Plugin-Fenster oben links auf das Menü-Symbol des FL-Wrappers → *Browse parameters*, dann im
   Browser Rechtsklick auf den Parameter → *Create automation clip*. Abspielen und dabei singen (oder
   Test-Pattern). Folgt der Klang ohne Knacken?
7. **CPU:** Puffergröße in FL (Audio-Einstellungen) auf **128** und **256**. Preset **Warm Pad** (Poly, zugleich
   das rechenintensivste Preset) und **Space Drift** spielen, eine Melodie mit vielen überlappenden Tönen singen.
   Wert der Anzeige **Synth DSP** (Reiter *Synth*) und **VocalSynth DSP** (Reiter *Voice*) sowie die CPU-Anzeige
   von FL notieren. Knackser?
8. **Instrument-Variante:** VocalSynth Instrument mit Sidechain-Mikro wie in `FL-ROUTING.md`, T3. Spielt der
   Synth, wenn du singst? (Wenn T3 bei dir nicht klappt: überspringen.)
9. **Externer Synth und Capture:** Wie bisher einen externen Synth über den MIDI-Port spielen (`FL-ROUTING.md`,
   T1/T2) – geht es noch? Einen Take im Reiter *Capture* aufnehmen und per Drag & Drop in FL ziehen – unverändert?
10. **Bypass und Entfernen:** Eine Note lang halten (oder Test-Pattern laufen lassen) und dabei das Plugin
    **bypassen** (der Synth blendet in 10 ms aus – knackt es?), wieder einschalten, dann das Plugin aus dem Slot
    **entfernen**. Hängt irgendein Ton? Puffergröße **während der Wiedergabe** ändern – stürzt etwas ab, hängt
    etwas?
11. **Fenster-Skalierung:** Fenster an der Ecke unten rechts größer/kleiner ziehen bzw. im Skalierungs-Menü
    oben rechts (zeigt „100 %“) 75 / 100 / 125 / 150 / 200 % wählen. Wird die Größe mit dem Projekt gespeichert? Bleibt alles lesbar und **scharf** (besonders auf einem HiDPI-/4K-Monitor)?

---

## 6. Probleme melden

Am hilfreichsten ist eine kurze Notiz pro Problem mit:

- **Was du gemacht hast** (Schritte) und **was passiert ist** vs. **was du erwartet hast**.
- **Preset-Name** (und ob du etwas verändert hast – dann steht ein `*` hinter dem Namen), Play Mode.
- FL-Version, Audio-Treiber (z. B. ASIO), **Puffergröße** und **Abtastrate** (44,1 / 48 kHz), Wert der
  **Synth DSP**-Anzeige.
- Bei Klangproblemen: eine kurze **Aufnahme** (z. B. per Capture-Take als WAV + den Synth über FLs Recording
  aufnehmen) oder das **Projekt** (FL: *File → Export → Zipped loop package*, dann sind eigene Wavetables dabei).
- Bei Anzeige-Problemen: ein **Screenshot** (Windows: Win + Shift + S).
- Bei Abstürzen: wann genau (beim Laden, beim Preset-Wechsel, beim Import, beim Schließen …).

Für die Klang-Bewertung reicht die Tabelle aus Abschnitt 4 mit einem Wort pro Preset. Ziel für 0.4.0: mindestens
die Hälfte der Presets „brauchbar“; den Rest stimmen wir in 0.4.1 nach deinem Feedback nach.

---

## 7. Bekannte Einschränkungen von 0.4

- Der Synth spielt nur, was VocalSynth selbst aus deiner Stimme (oder dem Test-Pattern) macht – er ist kein
  Instrument, das du über die Piano Roll oder ein MIDI-Keyboard spielst.
- Akkorde kommen erst mit 0.6 (Poly hilft vorerst nur beim Überlappen ausklingender Töne).
- Kein „HQ-Modus“ (Oversampling): sehr hohe Töne mit viel Filter-Drive können leicht rau klingen.
- Sehr schnelle, abwärts gleitende Töne können ganz oben im Spektrum (über ca. 7 kHz) minimal knistern – bitte
  melden, falls du es hörst.
- Die Vokal-Erkennung (Vowel X/Y als Mod-Quellen) kommt erst in 0.5; *Brightness* ist bis dahin der beste Ersatz.
  Die Quellen-Liste der Mod-Matrix zeigt sie schon jetzt grau als „Vowel X (0.5)“, „Vowel Y (0.5)“ und
  „Vowel Weight 1–3 (0.6)“ (damit gespeicherte Automation später nicht verrutscht); in 0.4 bewirken sie nichts.
- VocalSynth meldet FL keine Ausklingzeit (Tail 0). Auf dem Mikro-Insert ist das egal. Beim **Export** am Songende
  kann ein langer Hall-/Release-Ausklang abgeschnitten werden – im Export-Dialog *Leave remainder* wählen
  (unbestätigt), und *Smart disable* für VocalSynth nicht einschalten.
