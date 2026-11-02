# Status

**Erledigt**
- Repo auf GitHub (Stxtement/freedj, Branch `main`), JUCE und TagLib als Submodule.
- README.md mit Funktionen, Bauanleitung, Abhängigkeiten und Lizenzhinweisen (deutsch).
- BPM-Fehler („19 Smile“, Deck 122 vs. Bibliothek 95,50): Original- und Wiedergabe-BPM getrennt, SYNC Half-/Double-Time-fähig mit ±16 %-Grenze (docs/ANALYSE_STEMS_CONTROLLER.md, Abschnitt 6).
- Smart Matching (grün/blau in der Bibliothek, Schalter + Modus Auto/Deck 1/Deck 2/Beide; Abschnitt 5).
- KI-Genre-Erkennung (Discogs-EffNet, DB-Schema 3, Info-Panel, Kontextmenü „Detect genre (AI)“, geprüfter Modell-Download; Abschnitt 4).
- Titelzeile in der App heißt jetzt „FREE DJ“ (Release-Build erfolgreich, Tests danach nicht neu gelaufen).
- Tests: 359 Prüfungen im Release bestanden, Debug-Lauf ohne Assertions.

**Offen**
- Genre-Modell (Discogs-EffNet, CC BY-NC-SA) lizenzrechtlich klären, falls kommerziell genutzt.
- Lizenzen der in JUCE enthaltenen Codecs ggf. in eine `THIRD_PARTY.md` auslagern.
- DDJ-FLX4-Standard-Mapping ist geraten, nicht an der Hardware geprüft.
- Stimmung/Tanzbarkeit/Vocal-Anteil per KI nicht umgesetzt; Afro/Melodic House fehlen im Discogs-Vokabular.
- Nichts committet.

**Als Nächstes**
- Weiterentwicklung laut docs/PLAN.md (Effekte, Pads, Keylock, Aufnahme).
