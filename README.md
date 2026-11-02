# Free DJ â€“ Freie und quelloffene DJ-Software

Free DJ ist eine kostenlose Open-Source-DJ-Software fÃ¼r alle, die Musik und Mixen lieben. Ob AnfÃ¤nger, Hobby-DJ oder Profi: Free DJ gibt dir die Werkzeuge, um Tracks zu mixen, ÃœbergÃ¤nge zu gestalten und Musik zu genieÃŸen â€“ ohne teure Abos oder EinschrÃ¤nkungen.

## âœ¨ Funktionen

- ðŸŽµ Mehrere Audiotracks laden und mixen
- ðŸŽšï¸ Virtueller DJ-Mixer mit LautstÃ¤rkereglern
- ðŸŽ§ KopfhÃ¶rer-VorhÃ¶ren und Cue-UnterstÃ¼tzung
- ðŸ”Š Equalizer und Audioeffekte
- â±ï¸ BPM-Erkennung und Beat-Synchronisation
- ðŸ¥ Stem-Trennung (Gesang, Schlagzeug, Bass, Rest) per KI
- ðŸŽ›ï¸ UnterstÃ¼tzung fÃ¼r DJ-Controller (frei belegbares Mapping)
- ðŸ·ï¸ Genre-Erkennung per KI
- ðŸ“‚ Musikbibliothek und Playlists
- ðŸŽ¨ Moderne, benutzerfreundliche OberflÃ¤che
- ðŸ†“ Komplett kostenlos und Open Source

## ðŸŽ¯ Unsere Mission

Wir wollen DJing fÃ¼r alle zugÃ¤nglich machen. Keine teuren Lizenzen, keine Abos â€“ nur Musik und KreativitÃ¤t.

## ðŸ¤ Open Source

Free DJ wird von der Community fÃ¼r die Community gebaut. Jeder ist willkommen mitzumachen: Fehler melden, Funktionen vorschlagen und das Projekt verbessern.

**Mix freely. Create endlessly. Be a DJ.**

---

## ðŸ› ï¸ Bauen

Voraussetzungen: Windows, CMake â‰¥ 3.22, Visual Studio (C++20).

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

## ðŸ“¦ Verwendete Bibliotheken und Modelle

| Komponente | Version | Zweck | Lizenz | Copyright / Quelle |
|---|---|---|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) | 9.0.4 | Audio-Engine, GUI, Audioformate, DSP | AGPL-3.0 (oder kommerzielle JUCE-Lizenz) | Â© Raw Material Software Limited |
| [TagLib](https://github.com/taglib/taglib) | Submodul | Lesen/Schreiben von Tags (ID3, MP4 usw.) | LGPL-2.1 oder MPL-1.1 | Â© Scott Wheeler, TagLib-Mitwirkende |
| [SQLite](https://sqlite.org) | 3.54.1 | Musikbibliothek-Datenbank | Public Domain | Die Autoren verzichten auf das Urheberrecht |
| [ONNX Runtime](https://github.com/microsoft/onnxruntime) | 1.22.1 | AusfÃ¼hrung der KI-Modelle | MIT | Â© Microsoft Corporation |
| [DirectML](https://github.com/microsoft/DirectML) | siehe `libs/onnxruntime-dml` | GPU-Beschleunigung der KI-Modelle | MIT (Microsoft-Redistributable) | Â© Microsoft Corporation |
| [HTDemucs](https://github.com/facebookresearch/demucs) (ONNX-Export [`ferrite-stems/htdemucs-onnx-dml`](https://huggingface.co/ferrite-stems/htdemucs-onnx-dml)) | â€“ | Stem-Trennung | MIT | Â© Meta Platforms, Inc. und Mitwirkende |
| [Discogs-EffNet](https://essentia.upf.edu/models.html) | bsdynamic-1 | Genre-Erkennung (400 Stile) | CC BY-NC-SA 4.0 âš ï¸ | Pablo Alonso, Music Technology Group, Universitat Pompeu Fabra |

Die vollstÃ¤ndigen Lizenztexte liegen in den jeweiligen Ordnern unter [`libs/`](libs/), z. B. `libs/onnxruntime-dml/LICENSE-onnxruntime.txt`, `LICENSE-directml.txt`, `ThirdPartyNotices-onnxruntime.txt` und `libs/taglib/COPYING.LGPL` / `COPYING.MPL`.

JUCE bringt zusÃ¤tzlich eigene Bibliotheken mit, die hier mitgenutzt werden: FLAC, Ogg Vorbis (jeweils BSD-Lizenz), zlib, libpng und libjpeg. Deren Lizenzen stehen im JUCE-Ordner unter `modules/juce_audio_formats/codecs/` und `modules/juce_graphics/image_formats/`. FÃ¼r Windows Media Format und WASAPI/DirectSound werden Betriebssystem-Schnittstellen von Windows genutzt.

### âš ï¸ Lizenzhinweise

- **JUCE steht unter AGPL-3.0.** Weil Free DJ JUCE einbindet, muss Free DJ als Ganzes ebenfalls unter **AGPL-3.0** verÃ¶ffentlicht werden, inklusive Quellcode. Wer Free DJ weitergibt, muss den Quellcode mitliefern.
- **TagLib** ist LGPL/MPL und wird hier statisch gelinkt. Durch die AGPL-Gesamtlizenz ist das unproblematisch, weil der Quellcode offen ist.
- **Das Discogs-EffNet-Modell ist nur fÃ¼r nicht-kommerzielle Nutzung lizenziert** (CC BY-NC-SA 4.0). Es liegt nicht im Repository. Wer es nutzt, muss die Lizenzbedingungen selbst einhalten. FÃ¼r kommerzielle Nutzung bietet die Universitat Pompeu Fabra eine separate Lizenz an.
- Die **Stem-Trennung (HTDemucs)** ist MIT-lizenziert und darf frei genutzt werden. Die Lizenz verlangt, den Copyright-Hinweis beizubehalten.
- Musik, die du mit Free DJ abspielst oder mixt, unterliegt weiterhin den Rechten der jeweiligen Urheber. Du bist selbst fÃ¼r die Rechte an Ã¶ffentlichen Auftritten verantwortlich.

## âš ï¸ Haftungsausschluss (Disclaimer)

Die Nutzung von Free DJ erfolgt **auf eigene Gefahr**. Die Software wird so bereitgestellt, â€žwie sie istâ€œ, ohne jede ausdrÃ¼ckliche oder stillschweigende GewÃ¤hrleistung, insbesondere ohne GewÃ¤hr fÃ¼r FunktionsfÃ¤higkeit, Fehlerfreiheit oder Eignung fÃ¼r einen bestimmten Zweck. Die Autoren und Mitwirkenden haften nicht fÃ¼r SchÃ¤den, die durch die Nutzung entstehen, zum Beispiel Datenverlust, Hardware-SchÃ¤den (etwa an Lautsprechern oder GehÃ¶r durch zu hohe LautstÃ¤rke), AusfÃ¤lle bei Auftritten oder Rechtsverletzungen durch abgespielte Inhalte.

Sichere vor wichtigen Auftritten deine Bibliothek und teste dein Setup vorher. Die Software ist ein Community-Projekt und kann Fehler enthalten.

## ðŸ“„ Lizenz

Free DJ steht unter der **GNU Affero General Public License v3.0 (AGPL-3.0)**. Den vollständigen Lizenztext findest du in der Datei [LICENSE](LICENSE).

Â© 2026 Fabio Coluccelli und die Free-DJ-Mitwirkenden.
