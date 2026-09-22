# SenseCAP Screenshots — Dev-Tooling, NICHT für `main`

Dieser gesamte Ordner plus die zugehörigen Firmware-Serial-Befehle (`screenshot`,
`goto <screen>` in `src/sensecap/main.cpp`, `ui_model_test_goto_screen()` in
`src/sensecap/ui_model.cpp/.h`) existieren **nur auf diesem Branch**
(`dev/screenshot-tooling`). Sie dürfen **nicht** nach `main` gemergt werden.

## Warum nicht auf main

- `goto <screen>` springt ohne Touch direkt zu jedem Screen — **inklusive der
  PIN-geschützten Einstellungen-Seite**, ohne die PIN einzugeben. Das ist für ein
  Notfall-Terminal ein echtes Sicherheitsproblem, sobald es in einer echten
  Firmware landet, die im Feld läuft.
- `screenshot` dumpt den kompletten Framebuffer roh über Serial (~40s bei 115200
  Baud) und blockiert dabei das UI vollständig — reines Debug-Werkzeug, kein
  Feature für den Produktivbetrieb.

## Wie man Screenshots aufnimmt

Echte Screenshots des laufenden Geräts, kein Mockup — der Framebuffer wird direkt
abgegriffen (`lcd_framebuffer` in `main.cpp`).

```bash
# Aktuell sichtbaren Screen aufnehmen:
python3 scripts/capture_screenshot.py --port /dev/cu.usbserial-XXX --out out.png

# Direkt zu einem Screen springen und danach aufnehmen (kein Touch noetig):
python3 scripts/capture_screenshot.py --port /dev/cu.usbserial-XXX --goto settings --out out.png
```

Bekannte Screen-Namen für `--goto` (siehe `ui_model_test_goto_screen()` für die
aktuelle Liste): `main`, `fire`, `police`, `ambulance`, `info`, `confirm`,
`situation`, `crisis`, `emergency_details`, `transmission`, `transmission_failed`,
`history`, `language`, `pin_entry`, `settings`, `setup_bundesland`,
`setup_landkreis`, `setup_leitstelle`.

**Achtung:** `confirm`/`emergency_details`/`transmission*` zeigen Inhalte, die vom
zuletzt durchlaufenen echten Flow gesetzt wurden (Abteilungsname, VG-Nummer, ...).
Direkt dorthin springen kann veraltete oder leere Labels zeigen — gut genug für
einen Layout-Screenshot, nicht zur Inhalts-Verifikation.

## Aktuelle Screenshots

| Datei | Screen |
|---|---|
| `hauptmenue.png` | Hauptmenü |
| `feuerwehr-kategorien.png` | Feuerwehr-Unterkategorien |
| `notfall-bestaetigen.png` | Halte-Bestätigung vor dem Senden |
| `uebertragung-fehlgeschlagen.png` | Echter Fehlschlag-Screen (kein Meshtastic-Link) |
| `einstellungen.png` | Einstellungen-Seite |
