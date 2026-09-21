# XIAO ESP32-S3 + XIAO nRF52 — Board-Notizen (Historie)

Der ursprüngliche Hardware-Weg dieses Projekts, bevor SenseCAP Indicator
dazukam. Funktioniert weiterhin, ist aber bewusst zurückgestellt (Entscheidung
2026-09-18): der Handshake mit dem externen Funkgerät ist im Feldtest nicht
immer stabil ([Issue #35](https://github.com/Pixeldieb/kayna-funkt-notmeldeterminal/issues/35)),
und SenseCAP braucht dieses zweite Gerät gar nicht erst — siehe Haupt-README.
Bleibt hier als funktionierende Referenz und für den Fall, dass dieser Weg
später wieder gebraucht wird.

**Zwei unterschiedliche Boards, beide "XIAO" genannt — bewusst genau
unterschieden:**

| Board | Rolle |
|---|---|
| **Seeed XIAO ESP32-S3** ("Interface-Board") | Führt den Code aus `src/xiao/` aus, speichert Lagemeldungen in der lokalen Datenbank. **Hat kein eigenes LoRa-Funkmodul.** |
| **Seeed XIAO nRF52** ("Funkgerät") | Läuft mit unveränderter, offizieller Meshtastic-Firmware. Übernimmt das Funken (LoRa) ins Mesh-Netzwerk. |

Das XIAO-ESP32-S3-Interface-Board spricht mit dem XIAO-nRF52-Funkgerät über eine
serielle Verbindung (UART) — nicht über LoRa direkt.

## Hardware

| Teil | Wofür | Mehr Infos |
|---|---|---|
| Seeed XIAO ESP32-S3 (Interface-Board) | Führt den Code aus | [Seeed Wiki: Getting Started](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/) · [Pin-Belegung](https://wiki.seeedstudio.com/xiao_esp32s3_pin_multiplexing/) |
| Seeed XIAO nRF52 (Funkgerät) + Meshtastic-Firmware | Funkt ins Mesh-Netzwerk | [Meshtastic-Firmware flashen](https://flasher.meshtastic.org/) |
| Breadboard | Verbindung beider Boards | – |
| 2× USB-C-Kabel (Datenkabel!) | Strom + Programmieren | – |
| Jumper-Kabel | Für die Verkabelung | – |

> ⚠️ **Achtung bei den USB-Kabeln:** Manche USB-C-Kabel sind reine Ladekabel ohne Datenleitungen. Häufigste Ursache für Upload-Fehler.

## Verkabelung

![Verkabelung XIAO ESP32-S3 zu XIAO nRF52](../../assets/wiring-diagram.svg)

TX/RX **gekreuzt**, GND gemeinsam:

| XIAO ESP32-S3 (Interface-Board) | XIAO nRF52 (Funkgerät) |
|---|---|
| D6 (TX, GPIO43) | → RX |
| D7 (RX, GPIO44) | ← TX |
| GND | ↔ GND (Pflicht, auch bei getrennter USB-Stromversorgung) |

## Software-Setup

**1. Entwicklungsumgebung:** [VS Codium](https://vscodium.com/) + [PlatformIO-Erweiterung](https://platformio.org/install/ide?install=vscode)

**2. platformio.ini** (Auszug, siehe Repo für den vollständigen Eintrag):

```ini
[env:seeed_xiao_esp32s3]
platform = espressif32
board = seeed_xiao_esp32s3
framework = arduino
monitor_speed = 115200
lib_deps =
    https://github.com/meshtastic/Meshtastic-arduino.git
    siara-cc/Sqlite3Esp32
```

**3. Firmware flashen:**

```bash
pio run -e seeed_xiao_esp32s3 --target upload
```

## Das XIAO-nRF52-Funkgerät per CLI vorbereiten

Einmalig **per USB direkt am Rechner** (nicht über D6/D7):

```bash
pip3 install --upgrade meshtastic

meshtastic --set lora.region EU_868
meshtastic --set lora.tx_enabled true

meshtastic --set serial.enabled true
meshtastic --set serial.mode PROTO
meshtastic --set serial.rxd 7
meshtastic --set serial.txd 6
meshtastic --set serial.baud BAUD_115200

meshtastic --reboot
```

📖 Details: [meshtastic.org/docs/software/python/cli](https://meshtastic.org/docs/software/python/cli/)

Eine ausführliche, einfach gehaltene Anleitung, wie man ein gewöhnliches
Meshtastic-Gerät (z.B. genau dieses XIAO nRF52) als Testgegenstelle aufsetzt —
auch nützlich, um unabhängig von diesem Interface-Board mit dem SenseCAP
Indicator zu testen — steht im Wiki:
**[Meshtastic-Testknoten einrichten](https://github.com/Pixeldieb/kayna-funkt-notmeldeterminal/wiki/Meshtastic-Testknoten-einrichten)**.

## Wie der Code funktioniert

| Funktion | Aufgabe |
|---|---|
| `mt_serial_init(...)` | Baut beim Start die Verbindung zum XIAO-nRF52-Funkgerät auf |
| `mt_loop(millis())` | Muss **jeden Durchlauf** aufgerufen werden — hält Verbindung + eingehende Nachrichten am Laufen |
| `mt_send_text(...)` | Sendet alle 5 Minuten eine Test-Nachricht als Broadcast |
| `set_text_message_callback(...)` | Registriert `onTextMessage()`, wird bei jeder eingehenden Textnachricht aufgerufen |

**LED-Status** (eingebaute LED, GPIO21):

| Muster | Bedeutung |
|---|---|
| Doppel-Blitz + Pause | Verbindungsaufbau (Handshake) mit dem Funkgerät läuft |
| 5× schnelles Blinken (einmalig) | Verbindung erfolgreich hergestellt |
| Langsames Blinken (500ms) | Normalbetrieb |
| 6× schnelles Blinken | Test-Nachricht wird gesendet |
| 3× schnelles Blinken | Lagemeldung wurde gespeichert/aktualisiert |

## Zustandsmodell der Säule

Nur auf diesem Hardware-Weg vorhanden (SenseCAP hat dieses Konzept noch nicht,
siehe [Issue #10](https://github.com/Pixeldieb/kayna-funkt-notmeldeterminal/issues/10)).
Fünf Betriebszustände, Umschalten per Kurz-Code als Mesh-Broadcast (Textnachricht):

| Code | Zustand | Bedeutung |
|---|---|---|
| `LGE` | `AKTIV` | Lageöffnung — Notfallbetrieb |
| `NOR` | `STANDBY` | Normalbetrieb (Startzustand nach Boot) |
| `WTG` | `WARTUNG` | Wartungsmodus |
| `SAB` | `SABOTAGE` | Sabotage erkannt (aktuell manueller Test-Trigger) |
| `SAUS` | `STROMAUSFALL` | Stromausfall-Betrieb (aktuell manueller Test-Trigger, spätere Ausbaustufe: automatische Erkennung) |

Codes bewusst kurz und eindeutig gehalten — keine Alltagswörter wie "aus", um
versehentliches Auslösen und Tippfehler zu vermeiden, und schnell tippbar auch
unter Stress oder auf kleiner Tastatur.

Aktuellen Zustand abfragen: Serial-Befehl `status`. Zustandswechsel sind über
`src/common/mesh_security` genauso durch die Absender-Allowlist abgesichert wie
neue Lagemeldungen.

## Serial-CLI zum Prüfen

Im seriellen Monitor des XIAO-ESP32-S3-Interface-Boards eintippen:

| Befehl | Zeigt |
|---|---|
| `liste` | Alle aktuellen Lagemeldungen |
| `liste kategorie <X>` | Gefiltert nach Kategorie |
| `liste status <X>` | Gefiltert nach Status |
| `detail <ID>` | Eine Lagemeldung inkl. kompletter Änderungshistorie |
| `status` | Aktueller Zustand der Säule (siehe oben) |
| `allow add\|revoke <hex-node-id>` / `allow list` | Absender-Allowlist verwalten |
| `events` | Lokales Ereignisprotokoll |
| `help` | Befehlsübersicht |

> ⏱️ **Bekannte Einschränkung:** Zeitstempel basieren auf `millis()`
> (Geräte-Uptime seit letztem Reboot), keine echte Wanduhrzeit — dieses Board hat
> (anders als SenseCAP seit 2026-09-21) noch keine Uhrzeit-Quelle.

## Testen, ob alles funktioniert

1. Seriellen Monitor öffnen: `pio run -e seeed_xiao_esp32s3 --target monitor`
2. `>>> VERBUNDEN mit der Node` sollte erscheinen
3. Von einem zweiten Gerät im Mesh eine Lagemeldung senden (siehe oben)
4. Im Monitor sollte erscheinen: `>>> Neue Lagemeldung angelegt, ID X`
5. `liste` eintippen → sollte die neue Meldung zeigen

**Automatisierter Funktionstest:** [`scripts/test_mesh_functions.sh`](../../scripts/test_mesh_functions.sh)
sendet nacheinander alle aktuell unterstützten Testnachrichten (Zustandswechsel-Codes
+ Lagemeldungen) über einen zweiten, per USB angeschlossenen Meshtastic-Node.

```bash
./scripts/test_mesh_functions.sh --port /dev/cu.usbmodem2101
```

Seriellen Monitor des Interface-Boards währenddessen offen halten und nach jeder
gesendeten Nachricht mit `status`, `liste` bzw. `detail <ID>` gegenprüfen. Optional
Wartezeit zwischen den Nachrichten anpassen: `--delay <Sekunden>` (Default: 3).

## Troubleshooting

| Problem | Ursache | Fix |
|---|---|---|
| Upload schlägt fehl (`serial noise`, Timeout) | Anderes Terminal/Monitor blockiert den Port | Alle anderen Terminals schließen |
| Rote LED leuchtet dauerhaft bei USB | Normal — eingebaute Charge-LED | Kein Fehler |
| `SQL-Fehler: disk I/O error` beim Start | **Bekannter Bug** der `Sqlite3Esp32`-Bibliothek: `PRIMARY KEY`/`UNIQUE`-Constraints lösen auf SPIFFS zuverlässig I/O-Fehler aus ([Issue #18](https://github.com/siara-cc/esp32_arduino_sqlite3_lib/issues/18)) | Behoben: Schema nutzt keine expliziten Constraints mehr, IDs laufen über SQLites eingebaute `rowid` |
| Monitor zeigt nichts/Datenmüll | `monitor_speed` in `platformio.ini` passt nicht zu `Serial.begin()` im Code | `monitor_speed = 115200` setzen |
| Funkgerät sendet laut CLI erfolgreich, App zeigt nichts | App noch per USB verbunden (Port-Konflikt) oder Bluetooth-Kopplung verloren | Funkgerät nur per Strom + D6/D7 betreiben, App per **Bluetooth** verbinden |
