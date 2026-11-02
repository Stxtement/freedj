# Bestandsanalyse und Implementierungsplan

Stand: 2026-11-02, nach Phase 1+2. Quelle für den Funktionsumfang: Screenshot des VirtualDJ-Performance-Layouts
und der Erweiterungsauftrag. Es wird nichts von VirtualDJ kopiert (Code, Grafiken, Logo); nur Funktionsumfang
und Bedienkonzept dienen als Vorbild.

## 1. Bestand (geprüft: Build, 26 Engine-Tests, Debug-Lauf ohne Asserts/Leaks)

| Bereich | Stand | Nachweis |
|---|---|---|
| Deck-Engine (Laden, Play, Cue, Seek, Pitch ±50 %, Scratch, Reverse-fähig, Loop, 4 Hot Cues) | fertig | Tests |
| Gain, 3-Band-EQ, Filter | fertig | Tests (-23 dB / -62 dB) |
| Mixer: Fader, Crossfader, Master, Booth, Kopfhörer-Cue, VU | fertig | Tests |
| UI: 2 Decks, Mixer, Jogwheel, Waveforms, Datei-Browser | fertig, UI nur per Screenshot geprüft | Screenshot |
| Audio-Setup, Settings speichern | fertig | manuell gestartet |
| Tap-BPM, Sync (nur Tempo) | vorhanden, ohne Beat-Phase | - |

Bekannte Lücken und Fehler:
- Kein Keylock (Tempo ändert Tonhöhe), keine automatische BPM/Key/Beatgrid-Erkennung, kein Quantize.
- Keine Bibliothek: nur ein Dateibaum. Kein SQLite, keine Tags, keine Cover.
- Keine MIDI-Schicht. Aktionen sind noch direkt an UI-Callbacks gebunden (kein zentrales Aktionssystem).
- Keine Effekte, kein Slip, keine Pads, kein Sideview, keine Aufnahme, kein Mikrofon, keine Layouts.
- Kopfhörer-Routing ist nur auf Mehrkanal-Geräten (4+ Ausgänge) möglich; mit dem DDJ-FLX4 ungetestet.
- Kein Git-Commit vorhanden.

## 2. Reihenfolge (nach deiner Priorität: Stabilität, Audioqualität, Hardware, Funktionsumfang)

| Schritt | Inhalt | Abhängigkeiten |
|---|---|---|
| A | Bibliothek: SQLite, TagLib, Scanner, Songliste (zehntausende Zeilen), Seitenleiste, Playlists, Suche, Info-Panel | - |
| B | Aktionssystem + MIDI + DDJ-FLX4-Mapping inkl. LED-Feedback und Jogwheels | Controller angeschlossen zum Prüfen |
| C | Analyse (BPM, Beatgrid, Key), Quantize, Sync mit Beat-Phase, Keylock (Time-Stretch) | A (Ergebnisse in DB) |
| D | Effekt-Engine pro Deck (Echo, Reverb, Flanger, Phaser, Filter, Noise, Beat Repeat, Roll, Brake, Backspin) | B für Steuerung |
| E | Performance-Pads: Hot Cue, Loop, Roll, Slicer, Sampler, Scratch, Beat Jump, Stems, FX | B, C, D |
| F | Sideview: Sidelist, Remixes, Sampler, Automix, Karaoke, Clone | A, C |
| G | Stems (vorberechnet, siehe 3) | A, E |
| H | Layouts, Aufnahme, Mikrofon, History, Backup, Editoren | alle |

Jeder Schritt endet mit: Build, automatische Tests, Debug-Lauf, Doku. Was nicht getestet wurde, wird als
ungetestet markiert.

## 3. Stems: Machbarkeit (Analyse, noch nichts implementiert)

- Hochwertige Trennung braucht ein neuronales Netz (z. B. Demucs/HTDemucs als ONNX). Das Modell ist nicht im
  Repository und muss separat heruntergeladen werden (Lizenz prüfen).
- Echtzeit-Trennung während des Spielens ist auf einer CPU nicht stabil machbar. Der realistische Weg ist
  Vorberechnung im Hintergrund beim Analysieren und Ablage der Stems als Dateien im lokalen Cache.
  Danach ist die Wiedergabe latenzfrei, weil die Decks nur noch mehrere Spuren mit eigener Lautstärke mischen.
- Die Standardmodelle liefern Vocals, Drums, Bass, Other. Kick und Hi-Hat müssen aus der Drum-Spur zusätzlich
  getrennt werden (zweites Modell oder Frequenz-/Transientenfilter, Qualität geringer). Das ist die größte
  Qualitätslücke und wird bei der Umsetzung offen so ausgewiesen.
- Speicherbedarf: etwa 5 Spuren pro Track, bei FLAC-Kompression grob das 3- bis 5-Fache des Originals.

## 4. Technische Entscheidungen für Schritt A

- SQLite 3.54.1 (Amalgamation, geprüft per SHA3-256) im WAL-Modus. Alle Schreibzugriffe über eine
  Hintergrund-Verbindung in Transaktionen, Lesen über einen In-Memory-Cache der Tracks. So gibt es
  keine Datenbankabfragen pro Zeile beim Scrollen.
- TagLib 2.x (LGPL/MPL, statisch gelinkt) für ID3, Vorbis, MP4, FLAC, Cover.
- Songliste: virtuelles `TableListBox`; sortiert und gefiltert wird auf einem Index-Array, nicht auf Kopien.
- Scanner: eigener Thread, vergleicht Größe und Änderungszeit, liest nur neue oder geänderte Dateien,
  markiert fehlende Dateien, meldet Fortschritt.
