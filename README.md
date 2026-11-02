# Free DJ – Freie und quelloffene DJ-Software

Free DJ ist eine kostenlose Open-Source-DJ-Software für alle, die Musik und Mixen lieben. Ob Anfänger, Hobby-DJ oder Profi: Free DJ gibt dir die Werkzeuge, um Tracks zu mixen, Übergänge zu gestalten und Musik zu genießen – ohne teure Abos oder Einschränkungen.

## ✨ Funktionen

- 🎵 Mehrere Audiotracks laden und mixen
- 🎚️ Virtueller DJ-Mixer mit Lautstärkereglern
- 🎧 Kopfhörer-Vorhören und Cue-Unterstützung
- 🔊 Equalizer und Audioeffekte
- ⏱️ BPM-Erkennung und Beat-Synchronisation
- 🥁 Stem-Trennung (Gesang, Schlagzeug, Bass, Rest) per KI
- 🎛️ Unterstützung für DJ-Controller (frei belegbares Mapping)
- 🏷️ Genre-Erkennung per KI
- 📂 Musikbibliothek und Playlists
- 🎨 Moderne, benutzerfreundliche Oberfläche
- 🆓 Komplett kostenlos und Open Source

## 🎯 Unsere Mission

Wir wollen DJing für alle zugänglich machen. Keine teuren Lizenzen, keine Abos – nur Musik und Kreativität.

## 🤝 Open Source

Free DJ wird von der Community für die Community gebaut. Jeder ist willkommen mitzumachen: Fehler melden, Funktionen vorschlagen und das Projekt verbessern.

**Mix freely. Create endlessly. Be a DJ.**

---

## 🛠️ Bauen

Voraussetzungen: Windows, CMake ≥ 3.22, Visual Studio (C++20).

```
git clone --recurse-submodules https://github.com/Stxtement/freedj.git
cd freedj
cmake -B build
cmake --build build --config Release
```

Die Submodule `libs/JUCE` und `libs/taglib` werden mit `--recurse-submodules` geladen.

Die KI-Modelle sind nicht im Repository (Ordner `models/`):

- **Stem-Modell** (`htdemucs-dml.onnx`, ca. 370 MB): Die App bietet den Download beim ersten Start an.
- **Genre-Modell** (`discogs-effnet-bsdynamic-1.onnx` samt `.json`): optional, separat von Essentia herunterladen.

---

## 📦 Verwendete Bibliotheken und Modelle

| Komponente | Version | Zweck | Lizenz | Copyright / Quelle |
|---|---|---|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) | 9.0.4 | Audio-Engine, GUI, Audioformate, DSP | AGPL-3.0 (oder kommerzielle JUCE-Lizenz) | © Raw Material Software Limited |
| [TagLib](https://github.com/taglib/taglib) | Submodul | Lesen/Schreiben von Tags (ID3, MP4 usw.) | LGPL-2.1 oder MPL-1.1 | © Scott Wheeler, TagLib-Mitwirkende |
| [SQLite](https://sqlite.org) | 3.54.1 | Musikbibliothek-Datenbank | Public Domain | Die Autoren verzichten auf das Urheberrecht |
| [ONNX Runtime](https://github.com/microsoft/onnxruntime) | 1.22.1 | Ausführung der KI-Modelle | MIT | © Microsoft Corporation |
| [DirectML](https://github.com/microsoft/DirectML) | siehe `libs/onnxruntime-dml` | GPU-Beschleunigung der KI-Modelle | MIT (Microsoft-Redistributable) | © Microsoft Corporation |
| [HTDemucs](https://github.com/facebookresearch/demucs) (ONNX-Export [`ferrite-stems/htdemucs-onnx-dml`](https://huggingface.co/ferrite-stems/htdemucs-onnx-dml)) | – | Stem-Trennung | MIT | © Meta Platforms, Inc. und Mitwirkende |
| [Discogs-EffNet](https://essentia.upf.edu/models.html) | bsdynamic-1 | Genre-Erkennung (400 Stile) | CC BY-NC-SA 4.0 ⚠️ | Pablo Alonso, Music Technology Group, Universitat Pompeu Fabra |

Die vollständigen Lizenztexte liegen in den jeweiligen Ordnern unter [`libs/`](libs/), z. B. `libs/onnxruntime-dml/LICENSE-onnxruntime.txt`, `LICENSE-directml.txt`, `ThirdPartyNotices-onnxruntime.txt` und `libs/taglib/COPYING.LGPL` / `COPYING.MPL`.

JUCE bringt zusätzlich eigene Bibliotheken mit, die hier mitgenutzt werden: FLAC, Ogg Vorbis (jeweils BSD-Lizenz), zlib, libpng und libjpeg. Deren Lizenzen stehen im JUCE-Ordner unter `modules/juce_audio_formats/codecs/` und `modules/juce_graphics/image_formats/`. Für Windows Media Format und WASAPI/DirectSound werden Betriebssystem-Schnittstellen von Windows genutzt.

### ⚠️ Lizenzhinweise

- **JUCE steht unter AGPL-3.0.** Weil Free DJ JUCE einbindet, muss Free DJ als Ganzes ebenfalls unter **AGPL-3.0** veröffentlicht werden, inklusive Quellcode. Wer Free DJ weitergibt, muss den Quellcode mitliefern.
- **TagLib** ist LGPL/MPL und wird hier statisch gelinkt. Durch die AGPL-Gesamtlizenz ist das unproblematisch, weil der Quellcode offen ist.
- **Das Discogs-EffNet-Modell ist nur für nicht-kommerzielle Nutzung lizenziert** (CC BY-NC-SA 4.0). Es liegt nicht im Repository. Wer es nutzt, muss die Lizenzbedingungen selbst einhalten. Für kommerzielle Nutzung bietet die Universitat Pompeu Fabra eine separate Lizenz an.
- Die **Stem-Trennung (HTDemucs)** ist MIT-lizenziert und darf frei genutzt werden. Die Lizenz verlangt, den Copyright-Hinweis beizubehalten.
- Musik, die du mit Free DJ abspielst oder mixt, unterliegt weiterhin den Rechten der jeweiligen Urheber. Du bist selbst für die Rechte an öffentlichen Auftritten verantwortlich.

## ⚠️ Haftungsausschluss (Disclaimer)

Die Nutzung von Free DJ erfolgt **auf eigene Gefahr**. Die Software wird so bereitgestellt, „wie sie ist“, ohne jede ausdrückliche oder stillschweigende Gewährleistung, insbesondere ohne Gewähr für Funktionsfähigkeit, Fehlerfreiheit oder Eignung für einen bestimmten Zweck. Die Autoren und Mitwirkenden haften nicht für Schäden, die durch die Nutzung entstehen, zum Beispiel Datenverlust, Hardware-Schäden (etwa an Lautsprechern oder Gehör durch zu hohe Lautstärke), Ausfälle bei Auftritten oder Rechtsverletzungen durch abgespielte Inhalte.

Sichere vor wichtigen Auftritten deine Bibliothek und teste dein Setup vorher. Die Software ist ein Community-Projekt und kann Fehler enthalten.

## 📄 Lizenz

Free DJ steht unter der **GNU Affero General Public License v3.0 (AGPL-3.0)**. Den vollständigen Lizenztext findest du in der Datei [LICENSE](LICENSE).

© 2026 Fabio Coluccelli und die Free-DJ-Mitwirkenden.
