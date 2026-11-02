# Status

**Erledigt**
- Repo auf GitHub (Stxtement/freedj, Branch `main`), JUCE und TagLib als Submodule.
- README.md mit Funktionen, Bauanleitung, Abhängigkeiten und Lizenzhinweisen (deutsch).
- BPM-Fehler („19 Smile“, Deck 122 vs. Bibliothek 95,50): Original- und Wiedergabe-BPM getrennt, SYNC Half-/Double-Time-fähig mit ±16 %-Grenze (docs/ANALYSE_STEMS_CONTROLLER.md, Abschnitt 6).
- Smart Matching (grün/blau in der Bibliothek, Schalter + Modus Auto/Deck 1/Deck 2/Beide; Abschnitt 5).
- KI-Genre-Erkennung (Discogs-EffNet, DB-Schema 3, Info-Panel, Kontextmenü „Detect genre (AI)“, geprüfter Modell-Download; Abschnitt 4).
- Titelzeile in der App heißt jetzt „FREE DJ“ (Release-Build erfolgreich, Tests danach nicht neu gelaufen).
- Analyse-Persistenz geprüft: Ergebnisse stehen in SQLite und bleiben nach Neustart erhalten (echte Bibliothek: 953 von 2957 analysiert, 1993 noch offen, 11 fehlgeschlagen). Änderungen: nur-Zeitstempel-Änderung behält die Analyse (Fingerprint-Vergleich im Scanner), fehlgeschlagene Analysen werden pro Programmlauf einmal wiederholt, Analyse-Fortschritt in der Statuszeile, Stems werden im Hintergrund vorberechnet (nur bei Stillstand, bis 80 % des 20-GB-Caches). Quellcode kompiliert; Free DJ.exe nicht neu gelinkt (Datei gesperrt durch laufende App).
- Beat-Sync neu (DeckEngine::setSyncMaster, DeckComponent::syncToOtherDeck): SYNC gleicht Tempo exakt an, richtet die Beats aus und führt die Phase im Audio-Thread nach (max. ±2 % Tempo), das Deck folgt danach dem Tempo des anderen Decks. Endet bei Pitch-Fader, Nudge, Scratch, Trackwechsel. Kompiliert, NICHT getestet (Nutzer testet selbst); Bar-Ausrichtung (Downbeat) fehlt noch.
- Sync überarbeitet + AUTO-Sync (Knopf „AUTO“ je Deck, wird gespeichert): Nudgen beendet den Sync nicht mehr, sondern verschiebt die gehaltene Phase; ein gestopptes Deck startet beim PLAY auf dem Beat des anderen; Tempo-Korrektur max. ±1 %; Deck behält beim Ende des Syncs das zuletzt synchrone Tempo. Kompiliert, nicht getestet.
- GUI-Umbau: Decks zweispaltig (großer Platter + Pitch links/rechts, alle Tasten gestapelt daneben, Höhen skalieren mit dem Fenster), Mixer mit sichtbaren Kanal-Fadern, Pegelanzeigen und CUE-Tasten, kompakte FX-Leiste in Deck-Farben, mehr Platz für die Bibliothek (8 statt 5 Zeilen im Standardfenster), kürzere Statuszeile (Details im Tooltip). Zweite Instanz mit eigenem Datenordner (PATHBOARD_DATA_DIR) ist jetzt erlaubt (für Tests). Per Screenshot geprüft, nicht mit geladenen Tracks.
- Effekte (src/Effects/): 8 Effekte (Echo, Reverb, Flanger, Phaser, Filter, Trance Gate, Bit Crush, Beat Roll), je Deck 2 Slots vor dem Kanalfader, tempo- und beatgrid-synchron, Leiste „DECK n FX“ unter den Decks, Controller-Aktionen `deckN.fxM.*` und `fx.*` (Beat-FX-Bereich: on/off, Effektwahl, Depth, Deck-Ziel). 35 neue Tests, alle 398 Prüfungen bestehen. FLX4-Standardzuordnung der FX-Tasten ist GERATEN (Kanal 5), Effekt-Einstellungen werden nicht gespeichert.
- Beatmatching: die Ausrichtung auf den Beat des anderen Decks passiert jetzt sample-genau im Audio-Thread (Sprung auf den nächsten Master-Beat beim SYNC bzw. PLAY), statt über Positionen aus dem UI-Thread. Kompiliert und gelinkt, nicht getestet.
- Spotify-Einbindung (src/Spotify/, Knopf „SPOTIFY“): Anmeldung per OAuth-PKCE mit eigener Client-ID (Redirect `http://127.0.0.1:8898/callback`), liest Playlists und Liked Songs, gleicht sie mit lokalen Dateien ab (Titel + Interpret + Dauer), erzeugt aus den Treffern eine Bibliotheks-Playlist. Kein Spotify-Audio (nicht möglich). Kompiliert, nicht getestet, keine Unit-Tests dafür.
- Tests: 359 Prüfungen im Release bestanden, Debug-Lauf ohne Assertions.

**Offen**
- Genre-Modell (Discogs-EffNet, CC BY-NC-SA) lizenzrechtlich klären, falls kommerziell genutzt.
- Lizenzen der in JUCE enthaltenen Codecs ggf. in eine `THIRD_PARTY.md` auslagern.
- DDJ-FLX4-Standard-Mapping ist geraten, nicht an der Hardware geprüft.
- Stimmung/Tanzbarkeit/Vocal-Anteil per KI nicht umgesetzt; Afro/Melodic House fehlen im Discogs-Vokabular.
- Nichts committet.

**Als Nächstes**
- Weiterentwicklung laut docs/PLAN.md (Effekte, Pads, Keylock, Aufnahme).
