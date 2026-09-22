# Standort-Presets

Strukturierte Listen von Verwaltungsbezirken für ein Dropdown/Auswahlfeld am
Terminal, gedacht als Ersatz oder Ergänzung für das bisherige Freitext-Feld
"Ort" (siehe `SenseCAP-Einstellungen` im Wiki). Eine Datei pro Land/Locale,
damit sich künftig weitere Länder ergänzen lassen, ohne die bestehenden
Presets anzufassen oder umzustrukturieren.

## Warum Kfz-Kennzeichen als System

Für Deutschland orientiert sich die Liste bewusst an den amtlichen
Kfz-Unterscheidungszeichen (z.B. "M" für München, "BOT" für Bottrop) statt an
einer eigenen Codierung: Das System ist bereits jedem bekannt, kompakt (1-3
Buchstaben), deckt lückenlos alle 401 Landkreise und kreisfreien Städte ab,
und ist offiziell/stabil (Änderungen nur bei echten Gebietsreformen, per
Bundesanzeiger veröffentlicht). Für ein Notmeldeterminal, bei dem der
Standort schnell und eindeutig kommuniziert werden soll, ist das die
naheliegende Wahl.

## Datei: `districts_de.json`

Ein Land = eine Datei, benannt nach Locale (`de-DE`). Felder:

| Feld | Bedeutung |
|---|---|
| `locale` | Sprach-/Ländercode (BCP-47), bestimmt welches Preset zur UI-Sprache passt |
| `label` | Anzeigename der Preset-Quelle (nicht für die UI selbst gedacht) |
| `system` | Welches offizielle System die Codes referenzieren |
| `source` | Woher die Daten stammen (Nachvollziehbarkeit/Nachprüfbarkeit) |
| `stand` | Datum, an dem die Liste zuletzt gegen die Quelle abgeglichen wurde |
| `count` | Anzahl der Einträge in `districts` (Prüfsumme gegen Kopierfehler) |
| `states` | Die 16 Bundesländer: `code` (ISO 3166-2:DE, z.B. "BY") + `name` |
| `districts` | Die eigentliche Liste, siehe unten |

Jeder Eintrag in `districts`:

| Feld | Bedeutung |
|---|---|
| `id` | Amtlicher Gemeindeschlüssel des Kreises (5-stellig, stabile eindeutige ID -- als Schlüssel verwenden, nicht `name` oder `kennzeichen`) |
| `name` | Name des Landkreises/der kreisfreien Stadt |
| `type` | `"landkreis"` oder `"kreisfreie_stadt"` |
| `kennzeichen` | Das primäre/aktuelle Unterscheidungszeichen -- dieses Feld für die Dropdown-Anzeige verwenden |
| `kennzeichenAlle` | Alle für diesen Kreis gültigen Zeichen, inkl. wiedereingeführter historischer Zeichen abgetrennter Altkreise. `kennzeichen` ist immer `kennzeichenAlle[0]` |
| `bundesland` | Bundesland-Code, verweist auf `states[].code` |

Zwei Kreise können denselben `kennzeichen`-Wert tragen (z.B. "A" für sowohl
Stadt Augsburg als auch Landkreis Augsburg -- historisch geteilte Zeichen,
unterschieden über den Nummernkreis auf dem echten Kennzeichen, nicht über
den Buchstaben). Für die Dropdown-Anzeige deshalb immer `name` **und**
`kennzeichen` gemeinsam zeigen, nie nur den Code.

Städteregion Aachen, Region Hannover und Regionalverband Saarbrücken
("Kommunalverbände besonderer Art") sind mit `type: "landkreis"` gelistet,
nicht doppelt als kreisfreie Stadt -- deckt sich mit der amtlichen Zählung
(294 Landkreise + 107 kreisfreie Städte = 401).

## Für die Firmware-Integration (noch nicht umgesetzt)

Diese Datei ist als Rohdaten/Quelle gedacht, nicht zwingend als Laufzeitformat
für das SenseCAP-Board: 85 KB rohes JSON zur Laufzeit zu parsen ist für den
ESP32-S3 unnötig teuer. Empfehlung für die spätere Einbindung: aus dieser
JSON-Datei zur Build-Zeit ein kompaktes `PROGMEM`-C-Array generieren (Skript
in `scripts/`, analog zu anderen generierten Headern im Projekt), statt
ArduinoJson zur Laufzeit über die volle Liste laufen zu lassen. Diese Datei
bleibt dabei die Quelle der Wahrheit; das generierte Array wird aus ihr
abgeleitet, nicht von Hand gepflegt.

## Weitere Länder ergänzen

Neue Datei `districts_<locale>.json` nach demselben Schema anlegen. `system`
kann für andere Länder ein anderes offizielles Referenzsystem sein (nicht
zwingend Kfz-Kennzeichen, falls es dort kein vergleichbar bekanntes System
gibt) -- Hauptsache stabil, offiziell und den Nutzern bereits geläufig.
Welches Preset die UI lädt, richtet sich nach der eingestellten Sprache
(siehe Sprache-Knopf im Hauptmenü, aktuell nur Deutsch implementiert).
