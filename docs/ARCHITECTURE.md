# Free DJ – Architektur

Stand: Phase 1 + 2 (Oberfläche, Audio-Engine, Mixer). Diese Datei wird mit jeder Phase fortgeschrieben.

## Technologie

| Bereich | Wahl | Begründung |
|---|---|---|
| Sprache / Framework | C++20, JUCE 9 | Echtzeit-Audio, native GUI, MIDI, plattformübergreifend |
| Build | CMake + MSVC (`F:\BuildTools`) | JUCE-Standard, reproduzierbar |
| Audio-APIs (Windows) | WASAPI (shared/exclusive), DirectSound | Exclusive Mode für niedrige Latenz. ASIO braucht das Steinberg-SDK (später optional) |
| Dateiformate | WAV, AIFF, FLAC, OGG, MP3 (JUCE) und AAC/M4A/WMA (Windows Media Foundation) | |

## Module

```
src/
  Main.cpp                 App-Start, Fenster, Settings-Datei (ApplicationProperties)
  MainComponent.*          Hauptlayout, Audio-Setup-Dialog, Statuszeile, Settings speichern
  Audio/
    AudioEngine.*          Besitzt AudioDeviceManager, FormatManager, beide Decks, Mixer
    DeckEngine.*           Ein Deck: Laden, Wiedergabe, Pitch, Scratch, Gain/EQ/Filter, Cues, Loops
  Mixer/
    MixerEngine.*          Audio-Callback: Decks abholen, Fader, Crossfader, Master/Kopfhörer/Booth
    MixerComponent.*       Mixer-UI inkl. VU-Meter
  Decks/
    DeckComponent.*        Deck-UI (Transport, Pitch, Hot Cues, Loops, Tap, Sync, Drag & Drop)
    JogWheel.*             Virtueller Teller (Scratch innen, Pitch-Bend außen)
    ScratchDriver.h        Wandelt Bewegungen (Maus/MIDI) in eine Scratch-Geschwindigkeit um
  Waveform/
    WaveformDisplay.*      Overview (Seek) und scrollende Waveform (Scratch, Zoom)
  Library/
    FileBrowserPanel.*     Ordner-Browser (wird in Phase 3 durch die Bibliothek ersetzt)
  UI/
    DJLookAndFeel.*        Farben, Knobs, Fader, Buttons
tests/
  EngineTests.cpp          Offline-Tests der Audio-Engine (ohne Soundkarte)
```

## Signalfluss

```
Datei ──(Loader-Thread: dekodieren)──► RAM-Puffer (float, stereo, Original-Samplerate)
   │
   ▼  Audio-Thread, pro Sample
Variable-Rate-Reader (Catmull-Rom-Interpolation, Rate auch negativ)
   → Gain (Trim) → 3-Band-EQ (Low-Shelf 250 Hz / Peak 1 kHz / High-Shelf 4 kHz)
   → Filter (LP ◄ 0 ► HP, State-Variable-TPT)
   → Channel-Fader → Crossfader ─┬─► Master-Bus → Master-Volume → Ausgang 1/2
                                 └─► Booth-Volume → Ausgang 5/6
   Deck (pre-fader) bei aktivem CUE → Cue-Bus ─(Mix mit Master)─► Kopfhörer → Ausgang 3/4
```

Warum ganze Tracks im RAM: Scratchen, Rückwärtslauf, Backspin, Brake, Slip-Mode und sample-genaue Loops
brauchen wahlfreien Zugriff mit beliebiger Geschwindigkeit. Ein 5-Minuten-Track belegt ca. 100 MB.

## Thread-Modell und Regeln

| Thread | Aufgaben |
|---|---|
| Audio-Thread | `MixerEngine::audioDeviceIOCallbackWithContext` → `DeckEngine::getNextAudioBlock` |
| Message-Thread | UI, Tastatur, (ab Phase 5) MIDI-Aktionen |
| Loader-Thread (1 pro Deck) | Dekodieren, Waveform-Thumbnail erzeugen, alten Track freigeben |
| Scan-Thread | Ordnerinhalt für den Browser |

Regeln für den Audio-Thread (verbindlich für jeden neuen Code):

1. Keine Speicherallokation. Puffer und Filterkoeffizienten werden in `prepareToPlay` bzw.
   `audioDeviceAboutToStart` vorbelegt.
2. Kein blockierendes Lock. Einziges Lock ist `DeckEngine::trackLock`, auf dem Audio-Thread nur per
   `ScopedTryLock`. Es wird nur kurz für den Tausch des Track-Zeigers gehalten. Schlägt der Try-Lock
   fehl, liefert das Deck für einen Block Stille.
3. Kein Freigeben großer Objekte. Der alte Track wird auf dem Loader-Thread zerstört.
4. Steuerwerte (Play, Pitch, EQ, Fader, Seek, Loop …) laufen über `std::atomic`. Seeks werden als
   `pendingSeekSamples` übergeben und vom Audio-Thread am Blockanfang übernommen.
5. Geschwindigkeitsänderungen werden geglättet (~5 ms). Dadurch klicken Start, Stopp und Scratch nicht.

Ladevorgänge haben eine Generationsnummer: Ein neuer Ladeauftrag bricht einen laufenden ab, und ein
veralteter Auftrag kann nie einen neueren Track überschreiben.

## Fehlerbehandlung

- Fehlende, beschädigte oder nicht unterstützte Dateien liefern eine Fehlermeldung am Deck und keinen Absturz.
- MP3 und ähnliche Formate schätzen ihre Länge. Endet der Datenstrom früher, gilt das als Dateiende; die
  angehängte Stille wird abgeschnitten.
- Ein laufendes Deck wird nie durch Laden überschrieben (Schutz des Live-Mixes).
- Ein fehlendes Audiogerät wird gemeldet. Die Geräteauswahl wird in den Settings gespeichert.

## Tests

`FreeDJTests.exe` rendert echtes Audio durch Deck und Mixer und misst das Ergebnis: Pegel,
Frequenz (Pitch), Position (Loop, Scratch, Seek), Dämpfung (EQ, Filter), Crossfader, Kopfhörer-Routing
und VU-Werte. Mit Dateipfaden als Argumenten werden zusätzlich echte Bibliotheksdateien dekodiert.
Der Debug-Build läuft ohne JUCE-Assertions und ohne Leak-Meldungen.

```
cmake --build build --config Release --target FreeDJTests
build\FreeDJTests_artefacts\Release\FreeDJTests.exe [optionale Audiodateien ...]
```

## Geplante Erweiterungen (Architektur-Vorgriff)

- **Steuerungsschicht (Phase 5):** Alle Funktionen werden als benannte Aktionen/Parameter registriert
  (z. B. `deck1.play`, `deck1.jog.scratch`, `mixer.crossfader`). UI, Tastatur und MIDI greifen auf
  dieselbe Schicht zu. Mappings sind Daten (XML/JSON), sodass weitere Controller ohne Codeänderung
  hinzukommen. Für den Pioneer DDJ-FLX4 gibt es ein mitgeliefertes Mapping inklusive LED-Feedback;
  die Jogwheels gehen direkt in `JogWheel::touch/spin/bend`.
- **Keylock (Phase 6/7):** Ein Time-Stretcher sitzt zwischen dem Variable-Rate-Reader und dem EQ.
- **Bibliothek (Phase 3):** SQLite-Datenbank und Hintergrund-Scanner; die Analyse (BPM, Key, Beatgrid)
  läuft in einem eigenen Thread-Pool und speichert ihre Ergebnisse in der Datenbank.
