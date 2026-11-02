# Analyse, Stems und Controller – Aufbau, Messwerte, bekannte Grenzen

Stand: 2026-11-02. Alle Zahlen stammen aus den automatischen Tests (`FreeDJTests.exe`, 247 Prüfungen) oder aus
den genannten Messungen auf dem Entwicklungsrechner (Ryzen 7 5800X, RTX 3070 Ti 8 GB, 32 GB RAM, Windows 11).

## 1. Musik-Analyse (`src/Analysis/`)

| Datei | Aufgabe |
|---|---|
| `AudioAnalysis.*` | Dekodieren (Mono, 22,05 kHz), Onset-Hüllkurve, Tempo, Beatgrid, Downbeat, Tonart, Energie, Fingerprint |
| `AnalysisManager.*` | Hintergrund-Thread (niedrige Priorität), Warteschlange, Fortschritt, überspringt OneDrive-Dateien ohne lokale Kopie |
| `OnlineFile.cpp` | erkennt „Dateien bei Bedarf“-Platzhalter (Zugriff würde den Download auslösen) |

**Tempo.** Spektralfluss (1024er FFT, Hop 256) in zwei Bändern; die Kick (Bass ≤ 200 Hz) zählt dreifach, damit
Off-Beat-Hi-Hats das Raster nicht um eine halbe Periode verschieben. Grobes Tempo über Autokorrelation mit
Prior um 125 BPM, dann Feinsuche in 0,01-BPM-Schritten über den ganzen Track (Phasen- und Tempo-Kamm) mit
parabolischer Interpolation. Ausgabe: BPM mit zwei Nachkommastellen, erster Schlag, Downbeat (0..3),
Konfidenz, zweite Lesart (Halb-/Doppeltempo), Kennzeichen „Tempo ändert sich“ (Tempo in vier Abschnitten
einzeln geschätzt, mindestens drei müssen deutlichen Rhythmus haben, Spanne > 0,6 BPM).

**Tonart.** Chroma aus Spektralspitzen (110–3500 Hz) mit Stimmungskorrektur, Krumhansl-Kessler-Profile,
Ausgabe als Camelot-Code. Konfidenz berücksichtigt auch, wie „tonal“ das Material ist.

**Speicherung.** Datenbank-Schema 2 (automatisch aus Schema 1 aktualisiert): Fingerprint, Analyseversion,
Analysedatum, Konfidenz, Beatgrid, Downbeat, Energie, Lautheit, `manual_flags`. Manuell gesetzte Werte
(BPM, Tonart, Grid, Genre) werden von keiner Analyse und keinem Neu-Einlesen der Datei überschrieben; das
steckt direkt in den SQL-Anweisungen. Eine geänderte Datei wird automatisch neu analysiert; Tracks, die
fehlschlagen, werden nicht bei jedem Start erneut versucht. Beim Neustart wird genau das fortgesetzt, was
noch nicht analysiert ist.

**Gemessen (synthetische Drum-Tracks, bekannte Wahrheit).** 7 Tempi von 87,5 bis 174 BPM auf ±0,01 BPM
genau; Beatgrid auf 0,1–1,1 ms; Downbeat in 12 von 12 Fällen; Rauschen und Stille werden als unzuverlässig bzw.
ohne Tempo gemeldet; Tempo-Rampe 120→126 BPM wird erkannt, konstantes Tempo nicht fälschlich markiert; alle
24 Tonarten als Akkordfolgen richtig, auch 35 Cent verstimmt. 60 s Audio werden in etwa 0,11 s analysiert,
ein 4-Minuten-Song in etwa 0,5 s.

**Auf echter Musik (49 Songs, Vergleich mit den mir bekannten BPM).** Die meisten Songs stimmen auf die ganze
BPM (z. B. 128, 148, 117, 119, 131, 120, 90). Es gibt keine Referenzdatei, das ist kein Beweis.

**Bekannte Grenzen.**
- Triolen-Rhythmen werden als 2/3 oder 4/3 des Tempos gelesen (*Hey Ya!* 106 statt ~159, *Hot in Here* 142,8
  statt ~107). Beide mit mittlerer Konfidenz (0,59/0,60), also als unsicher erkennbar.
- Tonart: ohne Referenz nicht geprüft, bei Pop mit viel Gesang oft niedrige Konfidenz.
- Energie 1–10 ist eine grobe Lautheits-Heuristik, nicht validiert.
- Das Raster ist konstant. Bei Tracks mit Tempowechsel wird nur gekennzeichnet, nicht gefolgt.
- **Genre-Erkennung ist nicht umgesetzt.** Dafür braucht es ein trainiertes Modell; ehrlich geht das nicht mit
  Heuristiken. Das vorhandene Genre-Feld (aus den Datei-Tags) ist editierbar und wird nie überschrieben.

## 2. KI-Stem-Trennung (`src/Stems/`)

**Modell.** HTDemucs (Meta AI Research, MIT-Lizenz) als ONNX-Export `ferrite-stems/htdemucs-onnx-dml`
(372 MB, SHA-256 `9e06fef2…20ab`, wird beim Laden geprüft). Vier Stems: drums, bass, other, vocals.
Alternativen (kurz geprüft): Standard-Exporte mit entrollter iSTFT (24 700 Operationen, laufen auf DirectML
nicht stabil), MDX-Net-Modelle (Lizenzlage unklar). Kick und Hi-Hat einzeln gibt es mit diesem Modell nicht.

**Ausführung.** ONNX Runtime 1.22.1 mit DirectML (GPU) oder CPU.
- Auf DirectML liefert dieses Modell bei jeder Graph-Optimierungsstufe > 0 **falsche Werte** (ORT 1.22 und 1.24
  gemessen); nur Stufe 0 ist korrekt (ORT 1.24 ist auch dort falsch). Daher: ORT 1.22.1, Stufe 0 auf der GPU.
- Jedes Segment wird auf Plausibilität geprüft (Summe der Stems ≈ Eingang). Bei Unsinn wechselt der
  Separator selbständig auf die CPU und wiederholt den Auftrag (getestet mit absichtlich kaputter Konfiguration:
  Ergebnis danach identisch zur CPU-Referenz).
- Abweichung GPU gegen CPU: −76 bis −81 dB (nicht hörbar).

**Qualitätsmodi.** Gleiches Netz, unterschiedlicher Aufwand: FAST (10 % Überlappung), BALANCED (25 %),
HIGH (50 % und zwei zeitversetzte Läufe gemittelt). Auf der GPU, 20 s Audio: FAST ~1,0 s, BALANCED ~1,3 s,
HIGH ~3,8 s (ca. 20×, 15×, 5× Echtzeit). Auf der CPU ca. 2× Echtzeit. FAST und HIGH unterscheiden sich bei den
Vocals um −20 dB.

**Cache (`StemCache`).** Pro Datei-Fingerprint und Qualität vier 24-Bit-FLAC-Dateien plus `meta.json`
(Modell-ID, Länge, Dateigrößen). Ungültig bei anderem Modell, anderem Inhalt, fehlenden oder beschädigten
Dateien; ein BALANCED-Eintrag bedient auch FAST. Geschrieben wird in einen Temp-Ordner und dann umbenannt.
Limit 20 GB, älteste Einträge zuerst. Ein Song braucht grob 70–110 MB.

**Wiedergabe (`DeckEngine`).** Sind alle Stems an, spielt das Original. Sobald ein Stem aus ist, wird das
Original in 5 ms ausgeblendet und danach **gar nicht mehr gelesen**; ein stummer Stem wird ebenfalls nicht
gelesen und ist exakt 0,0. Alle Stems werden am selben Abspielzeiger gelesen (keine Phasenverschiebung).
Nichts davon läuft im Audio-Callback außer dem Mischen; Trennung, Dekodieren und Cache-Zugriff liegen im
Hintergrund, die Übergabe an das Deck geschieht per `shared_ptr` unter dem Try-Lock.

Getestet: Vocal aus → Vocal-Ton 155 dB leiser, Marker-Ton, der nur im Original steckt, ist exakt weg;
alle Stems aus → Ausgabe exakt 0; kein Klick beim Umschalten; Mute vollständig 6 ms nach dem Befehl;
Zeitgleichheit bei 44,1 und 48 kHz Quellen; Resampler 0,2 Samples; Kosten ca. 0,25 % eines Kerns.

**Echte Trennung (je 20 s aus fünf Songs der Bibliothek, BALANCED).**

| Genre | Rekonstruktion | Vocal-Anteil | Vocal-Rest im Instrumental* | Instrumenten-Rest im Vocal-Stem* |
|---|---|---|---|---|
| Pop / Dance | 32,4 dB | 25 % | −62,8 dB | −45,9 dB |
| Rock | 27,8 dB | 8 % | −43,5 dB | −32,4 dB |
| Pop / Electro | 30,1 dB | 23 % | −64,2 dB | −41,4 dB |
| Hip-Hop | 33,1 dB | 25 % | −48,7 dB | −32,5 dB |
| Dance | 27,9 dB | 49 % | −54,9 dB | −48,2 dB |
| Eigenproduktion (ohne Gesang) | 35,4 dB | 0,00007 % | −62,6 dB | nicht definiert |

\* Gemessen durch erneutes Trennen: Das Instrumental wird noch einmal getrennt, der dabei gefundene
Gesang ist der „Vocal-Rest“. **Das ist eine Untergrenze**: Was das Modell schon beim ersten Mal nicht als
Gesang erkennt, findet es beim zweiten Mal auch nicht. Es ersetzt kein Hören und keine Messung mit
Referenz-Stems (z. B. MUSDB18); beides habe ich nicht gemacht.

**Bekannte Grenzen.** Hall und Echo des Gesangs bleiben teilweise im Instrumental; mehrstimmiger Chor, Gesang
mit starken Effekten und Samples mit Stimmen sind die typischen Problemfälle von Demucs. Die Stems sind
nicht „sauberer als das Modell“: Es bleibt hörbares Restmaterial, besonders bei Zischlauten. Nicht getestet
wurden House/Tech House/Techno mit echtem Gesang, Hip-Hop-Features mit mehreren Stimmen und stark verhallte
Songs: dafür liegt in meiner Testmenge kein passendes lokales Material. Der erste Aufruf einer Sitzung braucht
~6 s (Modell laden).

## 3. Controller (`src/Controller/`)

| Datei | Aufgabe |
|---|---|
| `ActionRegistry.h` | Funktionen der App als benannte Aktionen (`deck1.play`, `mixer.crossfader`, …) |
| `ControllerMapping.*` | Zuordnung MIDI-Nachricht → Aktion als Daten (JSON), Standard-Mapping für das DDJ-FLX4 |
| `ControllerManager.*` | MIDI-Ein-/Ausgang, Decodieren, Lernen, Monitor, LEDs, automatisches (Wieder-)Verbinden |
| `ControllerSettingsComponent.*` | Einstellungsseite (Knopf CONTROLLER) |

MIDI kommt im Treiber-Thread an, wird in eine Warteschlange gelegt und im Message-Thread bearbeitet, nie im
Audio-Thread. 14-Bit-Regler (MSB + LSB), 7-Bit-Regler, Tasten (Drücken und Loslassen getrennt) und zwei
Arten von Endlos-Drehgebern (Jogwheel: 64 = still; Browse-Knopf: ±Schritte) werden unterstützt.
In der Einstellungsseite kann jede Funktion mit **Learn** einem beliebigen Bedienelement zugewiesen werden;
Typ, Invertierung, Jogwheel- und Pitch-Bend-Empfindlichkeit sind einstellbar; Mapping laden/speichern;
jede Änderung wird sofort in `controller-mapping.json` gespeichert.

**Wichtig: Das Standard-Mapping des DDJ-FLX4 ist geraten** (Nachrichtenaufbau der DDJ-400-Familie aus dem
Gedächtnis) und **nicht gegen dein Gerät geprüft**. Die Logik dahinter ist getestet (alle Nachrichtenarten,
Lernen, LEDs, Thread-Übergabe), ob `Note 0x0B` an deinem Pult wirklich „Play“ ist, ist es nicht. Mit dem
MIDI-Monitor und Learn korrigierst du abweichende Zuordnungen in wenigen Minuten. Unbekannt sind außerdem
die Auflösung des Jogwheels (Einstellung „ticks per turn“), die Richtung des Tempo-Faders (Invertieren) und
ob die LED-Antworten (Note-on mit Velocity 127/0 auf derselben Nummer) so am Pult ankommen.

Unter Windows kann ein MIDI-Gerät nur ein Programm gleichzeitig öffnen; läuft rekordbox, VirtualDJ oder
Serato, meldet die App, dass sie das Gerät nicht öffnen kann.

## 4. Genre-Erkennung (`src/Genre/`)

**Modell.** Discogs-EffNet (Music Technology Group, UPF; offizielle ONNX-Datei von essentia.upf.edu, 18 MB,
SHA-256 `a280825b…e43c` im Programm hinterlegt und beim Laden geprüft). **Lizenz CC BY-NC-SA 4.0: nur
nicht-kommerzielle Nutzung.** Es liefert 400 Discogs-Stile (Electronic---Tech House, Rock---Alternative Rock, …).
Das Modell wird erst nach Rückfrage heruntergeladen (Rechtsklick > „Detect genre (AI)“).

**Ablauf.** Datei → Mono 16 kHz (Sinc-Resampler, 0,6 s pro Song) → Mel-Spektrogramm-Patches (128×96) →
ONNX Runtime (CPU) → Mittel der Stil-Wahrscheinlichkeiten über bis zu 24 Patches. Ausgabe: Hauptgenre
(z. B. House), Untergenre (Tech House), Musikstil-Familie (Electronic), Konfidenz = Wahrscheinlichkeit des
stärksten Stils (die Stile sind unabhängig, die Werte ergeben zusammen nicht 1) und die nächsten Alternativen.
Gespeichert in eigenen Spalten (Schema 3); **das eigene Genre-Tag wird nie überschrieben**. In der Liste erscheint
ein automatisches Genre gedämpft und kursiv, und nur ab 25 % Konfidenz. Der Button „Use detected genre“ im Info-Panel
übernimmt es bewusst als eigenes.

**Vorverarbeitung gemessen, nicht geraten.** Die Modellbeschreibung nennt die Mel-Parameter nicht. Von 24 Varianten
lieferten nur Leistungsspektren sinnvolle Stile (Mr. Brightside → Alternative Rock, In Da Club → Gangsta); mit
Betragsspektrum wurden Rock und Hip-Hop als „Experimental/Noise“ gelesen. Die Standardwerte stehen in `MelOptions`.

**Getestet (4 lokale Songs, nur bekannte Genres):** Mr. Brightside → Rock / Alternative Rock (0,40), In Da Club →
Hip Hop / Gangsta (0,71), Get Busy → Reggae / Ragga (0,86), Teenage Dirtbag → Rock / Alternative Rock (0,27):
4 von 4 im richtigen Hauptgenre. Das ist eine kleine Stichprobe, keine Genauigkeitsmessung.

**Grenzen.** „Afro House“, „Melodic House“ und „Melodic Techno“ gibt es im Discogs-Vokabular nicht; das Modell
kann sie nicht ausgeben (aus Tags werden sie verstanden). Viele Pop-Songs liegen unter 25 % und bleiben ohne
Genre. Stimmung, Tanzbarkeit und Vocal-Anteil sind nicht umgesetzt.

## 5. Smart Matching (`src/Library/SmartMatch.*`, `LibraryBrowser`)

Schalter „Smart Match“ (an/aus, wird gemerkt) und Modus (Auto / Deck 1 / Deck 2 / Beide Decks) in der Browser-Leiste.
Bezug im Modus Auto: das spielende Deck, sonst das zuletzt geladene; „Beide“ verlangt, dass ein Song zu beiden
Decks passt (das schwächere Ergebnis zählt). Die Markierung folgt jedem Trackwechsel, Tempoänderung und Play/Pause;
Filter und Sortierung bleiben unverändert, nur die Zeilen werden getönt (Balken links). Der Tooltip nennt den Grund.

Bewertung (nur aus bekannten Werten, nichts wird ergänzt):
- **Tempo:** Abweichung ≤ 3 % = 1,0; ≤ 6 % = 0,7; ≤ 10 % = 0,4; sonst 0. Half-/Double-Time zählt (Faktor 0,95).
  Verglichen wird mit dem **hörbaren** Tempo des Bezugsdecks (inklusive Pitch).
- **Tonart (Camelot):** gleich 1,0; relative Dur/Moll oder ein Schritt 0,9; zwei Schritte 0,5; sonst 0.
- **Genre:** gleiches Genre 1,0; Haupt- und Untergenre 0,9; zwei Untergenres derselben Familie 0,75; verwandte
  Familien (z. B. House–Techno 0,5, Pop–Rock 0,4); sonst 0. Quelle: eigenes Tag, sonst automatisches Genre ≥ 25 %.
- **Grün:** Tempo ≥ 0,7 und Tonart ≥ 0,9 und Gesamt ≥ 0,85 und Genre bekannt-passend oder unbekannt.
  **Blau:** Tempo ≥ 0,4, Gesamt ≥ 0,6, Tonart unbekannt oder ≥ 0,5. **Keine Farbe:** unpassende Tonart, Tempo zu weit
  weg, unbekanntes Tempo, oder nur das Tempo ist bekannt.
- **Zuverlässigkeit:** Ein automatisch erkannter Wert zählt nur ab Konfidenz BPM 0,35 / Tonart 0,25. Von Hand
  gesetzte Werte und Werte aus den Datei-Tags zählen immer. Weil die Tonart-Erkennung bei Pop oft unsicher ist,
  bleiben dann viele Songs unmarkiert – das ist gewollt und kein Fehler.

## 6. BPM-Anzeige und Sync (Fehlerbericht „19 Smile“)

**Ursache.** Die Bibliothek zeigte korrekt das Original (95,50 BPM, Konfidenz 100 %, nichts manuell). Das Deck zeigte
die Wiedergabe-BPM (Original × Pitch) ohne Hinweis darauf. 122 BPM entstehen nur mit +27,75 % Pitch, und den
setzte SYNC: Es zog das Tempo auf die andere Deck-BPM und erweiterte dafür den Pitch-Bereich still bis ±50 %,
ohne Half-/Double-Time zu berücksichtigen. In der echten App nachgestellt (Smile auf Deck 1, 122-BPM-Track auf Deck 2).

**Korrektur.** `DeckEngine::getOriginalBpm()` und `getPlaybackBpm()` trennen die beiden Werte. Das Deck zeigt groß die
Wiedergabe-BPM und darunter „ORIG 95.50“ (gelb mit Pitch-Prozent, wenn gepitcht wird). SYNC rechnet vom Original
gegen das hörbare Tempo des anderen Decks, berücksichtigt Half-/Double-Time (95,5 → 191 BPM = 0 % Pitch) und lehnt ab,
wenn mehr als ±16 % nötig wären („TOO FAR“ auf dem Knopf, Tooltip mit Prozentwert); die Tonart würde sonst um
mehrere Halbtöne wandern.

## 7. Noch nicht umgesetzt

Stimmung/Tanzbarkeit/Vocal-Anteil, Effekte, Slicer/Sampler/Scratch-Pads, Sideview (Sidelist, Automix, Sampler, Karaoke),
Layouts, Mikrofon, Aufnahme, Keylock (Tempo ohne Tonhöhe), Quantize/Sync mit Beat-Phase, Beatgrid-Editor mit
Tempo-Kurve, Einstellung des Stem-Cache-Limits und des Modellordners in der Oberfläche, ASIO.
