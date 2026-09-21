<div align="center">

# kayna-funkt Notmeldeterminal (vibe coded prototype)

![Platform](https://img.shields.io/badge/platform-ESP32--S3-10537E?style=flat-square)
![Framework](https://img.shields.io/badge/framework-Arduino%20%2F%20PlatformIO-04A098?style=flat-square)
![Node](https://img.shields.io/badge/mesh-Meshtastic-62B22E?style=flat-square)
![Status](https://img.shields.io/badge/status-prototype%20%E2%80%94%20kayna--funkt-lightgrey?style=flat-square)
![AI Slopmaker](https://img.shields.io/badge/Anthropic%20Claude%20Sonnet%205%20High)

Ein Meshtastic-basiertes Notmeldeterminal für abgesetzte Standorte ohne
Mobilfunk-/Internetanbindung — Teil des Notmeldestellen-Projekts **kayna-funkt**.

Das Projekt baut in erster Linie ein **Interface**: eine Sammlung Logik
(Lagemeldungen strukturiert speichern, Absender prüfen, echte Zustellbestätigung),
die unabhängig von der konkreten Hardware funktioniert. Welches Gerät die
Notmeldungen tatsächlich sendet, kann wechseln und soll in Zukunft erweiterbar
bleiben — aktuell laufen zwei Hardware-Wege, weitere sind ausdrücklich möglich.
Frühere Titel dieses Repos ("ESP32-S3 Interface for Meshtastic") beschrieben nur
noch den ersten der beiden — siehe [Wiki](https://github.com/Pixeldieb/kayna-funkt-notmeldeterminal/wiki)
für den aktuellen Gesamtüberblick in einfacher Sprache.

</div>

---

## 📋 Inhalt

- [Was macht dieses Projekt?](#-was-macht-dieses-projekt)
- [Architektur: ein Interface, mehrere Hardware-Ziele](#-architektur-ein-interface-mehrere-hardware-ziele)
- [Lagemeldungen (das gemeinsame Interface)](#-lagemeldungen-das-gemeinsame-interface)
- [SenseCAP Indicator — aktuelles Hauptziel](#-sensecap-indicator--aktuelles-hauptziel)
- [Historie: der erste Hardware-Weg (XIAO ESP32-S3 + XIAO nRF52)](#-historie-der-erste-hardware-weg-xiao-esp32-s3--xiao-nrf52)
- [Weiterführende Links](#-weiterführende-links)
- [Changelog](#-changelog)
- [Lizenz](#-lizenz)

---

## 🧭 Was macht dieses Projekt?

**Ziel:** Eingehende, speziell markierte Nachrichten ("Lagemeldungen") sollen
nicht nur als lose Chat-Nachricht im Mesh verpuffen, sondern strukturiert, mit
Änderungshistorie und mit einer echten Zustellbestätigung auf dem Gerät
gespeichert werden.

Diese Logik lebt in `src/common/` und ist absichtlich hardware-neutral:

| Modul | Aufgabe |
|---|---|
| `lage_db` | Lagemeldungen + Änderungshistorie in SQLite, plus optionaler Spiegel-Haken für ein Backup-Medium |
| `mesh_security` | Absender-Allowlist + Ratenbegrenzung — sicherer Default: leere Allowlist verwirft alles |
| `cobs` | Byte-Framing für interne UART-Verbindungen (z.B. zwischen zwei Chips auf einem Board) |

Jedes unterstützte Gerät bringt nur noch sein eigenes, kleines
hardware-spezifisches Stück Code mit (Funkmodul ansprechen, Bildschirm zeichnen,
Tasten lesen) und benutzt diese gemeinsame Logik. Das ist bewusst so gebaut,
damit sich später ein weiteres Gerät (z.B. ein reguläres Produktivgerät mit
anderer Hardware) anschließen lässt, ohne die Kernlogik neu zu schreiben.

---

## 🧩 Architektur: ein Interface, mehrere Hardware-Ziele

```
src/common/         <- Interface: hardware-neutrale Logik (s.o.)
src/sensecap/        <- Hardware-Ziel 1 (aktueller Fokus): eigenständiges Touchscreen-Terminal
src/sensecap_rp2040/ <- selbes Board, zweiter Chip: Micro-SD-Backup-Spiegel
src/xiao/            <- Hardware-Ziel 2 (Historie): serieller Prototyp, siehe unten
```

Jedes `[env:...]` in `platformio.ini` ist ein eigenständiges Firmware-Ziel, das
gegen dieselbe `src/common/`-Logik baut. Ein neues Hardware-Ziel hinzuzufügen
heißt: neuer Ordner unter `src/`, neue `[env:...]`-Sektion, Wiederverwendung von
`src/common/` — keine Änderung an der Kernlogik selbst nötig.

---

## 🚨 Lagemeldungen (das gemeinsame Interface)

Eingehende Nachrichten mit dem Prefix `LAGE:` werden erkannt, geparst und in
einer lokalen **SQLite-Datenbank** gespeichert — alle anderen Mesh-Nachrichten
werden ignoriert. Dieses Format und Datenmodell sind auf beiden aktuellen
Hardware-Zielen identisch (`src/common/lage_db`).

### Nachrichtenformat

```
LAGE:<ID|NEU>;<Kategorie>;<Status>;<Text>
```

**Neue Lagemeldung anlegen:**
```
LAGE:NEU;Brand;offen;Kellerbrand Mehrfamilienhaus Hauptstraße 12
```

**Bestehende Lagemeldung aktualisieren** (ID aus vorheriger Anlage, z.B. `1`):
```
LAGE:1;Brand;in Bearbeitung;Feuerwehr löscht, Nachbargebäude evakuiert
```

Trennzeichen ist bewusst **Semikolon** (`;`) statt Pipe — auf jeder Tastatur ohne
Umschalt-Kombination erreichbar, wichtig im Feldeinsatz.

### Datenmodell

| Tabelle | Zweck |
|---|---|
| `lagemeldungen` | Aktueller Stand jeder Lagemeldung (Kategorie, Status, Text, Absender, Zeitstempel) |
| `lage_historie` | Jede Änderung wird **vor** dem Überschreiben protokolliert (alter/neuer Text, alter/neuer Status, Zeitpunkt) — volle Nachvollziehbarkeit |

IDs laufen über SQLites eingebaute `rowid`, keine expliziten `PRIMARY KEY`/`UNIQUE`-Constraints
(bekannter Bibliotheks-Bug, siehe Changelog 2026-09-15).

---

## 📺 SenseCAP Indicator — aktuelles Hauptziel

Ein Seeed SenseCAP Indicator (D1L) mit 480×480-Touchscreen: Menü, Notfall-Flow,
Datenbank und Funkverbindung laufen alle auf **einem** Board — im Unterschied
zum historischen Zwei-Board-Weg weiter unten braucht dieses Ziel kein externes
Funkgerät.

Build/Flash: `pio run -e sensecap_indicator -t upload`

Ausführliche Doku (Hardware-Bring-up-Story, bekannte Gotchas, Architektur,
aktueller Stand der Meshtastic-Anbindung): **[src/sensecap/README.md](src/sensecap/README.md)**.

Kurzstand: Display/Touch/Menü/Datenbank laufen stabil. Meshtastic-Anbindung läuft
über den eingebauten SX1262 mit echtem Meshtastic-Protokoll (Paketformat,
Verschlüsselung, Kanal-Hash — kein rohes/inkompatibles Signal) und ist gegen ein
reales Meshtastic-Gerät verifiziert: Broadcast bidirektional, Direktnachricht mit
einem echten **Anwendungs-ACK** (`LAGE:ACK:<id>`, nicht nur ein Transport-ACK —
ein Transport-ACK bestätigt nur, dass ein Paket irgendwo ankam, nicht dass die
Leitstelle es inhaltlich akzeptiert hat; siehe Changelog 2026-09-18 "B5") an eine
konfigurierbare Leitstelle.

Details im [Wiki](https://github.com/Pixeldieb/kayna-funkt-notmeldeterminal/wiki/SenseCAP-Meshtastic)
und in [Issue #36](https://github.com/Pixeldieb/kayna-funkt-notmeldeterminal/issues/36).

---

## 🕰️ Historie: der erste Hardware-Weg (XIAO ESP32-S3 + XIAO nRF52)

Der Ausgangspunkt dieses Projekts, bevor SenseCAP Indicator dazukam. Funktioniert
weiterhin, ist aber bewusst zurückgestellt (Entscheidung 2026-09-18): der
Handshake mit dem externen Funkgerät ist im Feldtest nicht immer stabil
([Issue #35](https://github.com/Pixeldieb/kayna-funkt-notmeldeterminal/issues/35)),
und SenseCAP braucht dieses zweite Gerät gar nicht erst. Bleibt hier als
funktionierende Referenz und für den Fall, dass dieser Weg später wieder
gebraucht wird.

**Zwei unterschiedliche Boards, beide "XIAO" genannt — bewusst genau
unterschieden:**

| Board | Rolle |
|---|---|
| **Seeed XIAO ESP32-S3** ("Interface-Board") | Führt den Code aus `src/xiao/` aus, speichert Lagemeldungen in der lokalen Datenbank. **Hat kein eigenes LoRa-Funkmodul.** |
| **Seeed XIAO nRF52** ("Funkgerät") | Läuft mit unveränderter, offizieller Meshtastic-Firmware. Übernimmt das Funken (LoRa) ins Mesh-Netzwerk. |

Das XIAO-ESP32-S3-Interface-Board spricht mit dem XIAO-nRF52-Funkgerät über eine
serielle Verbindung (UART) — nicht über LoRa direkt.

### Hardware

| Teil | Wofür | Mehr Infos |
|---|---|---|
| Seeed XIAO ESP32-S3 (Interface-Board) | Führt den Code aus | [Seeed Wiki: Getting Started](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/) · [Pin-Belegung](https://wiki.seeedstudio.com/xiao_esp32s3_pin_multiplexing/) |
| Seeed XIAO nRF52 (Funkgerät) + Meshtastic-Firmware | Funkt ins Mesh-Netzwerk | [Meshtastic-Firmware flashen](https://flasher.meshtastic.org/) |
| Breadboard | Verbindung beider Boards | – |
| 2× USB-C-Kabel (Datenkabel!) | Strom + Programmieren | – |
| Jumper-Kabel | Für die Verkabelung | – |

> ⚠️ **Achtung bei den USB-Kabeln:** Manche USB-C-Kabel sind reine Ladekabel ohne Datenleitungen. Häufigste Ursache für Upload-Fehler.

### Verkabelung

TX/RX **gekreuzt**, GND gemeinsam:

| XIAO ESP32-S3 (Interface-Board) | XIAO nRF52 (Funkgerät) |
|---|---|
| D6 (TX, GPIO43) | → RX |
| D7 (RX, GPIO44) | ← TX |
| GND | ↔ GND (Pflicht, auch bei getrennter USB-Stromversorgung) |

### Software-Setup

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

### Das XIAO-nRF52-Funkgerät per CLI vorbereiten

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

Eine ausführliche, in einfacher Sprache gehaltene Anleitung, wie man ein
gewöhnliches Meshtastic-Gerät (z.B. genau dieses XIAO nRF52) als Testgegenstelle
aufsetzt — auch nützlich, um unabhängig von diesem Interface-Board mit dem
SenseCAP Indicator zu testen — steht im Wiki:
**[Meshtastic-Testknoten einrichten](https://github.com/Pixeldieb/kayna-funkt-notmeldeterminal/wiki/Meshtastic-Testknoten-einrichten)**.

### Wie der Code funktioniert (XIAO ESP32-S3)

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

### Zustandsmodell der Säule

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

### Serial-CLI zum Prüfen

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

### Testen, ob alles funktioniert

1. Seriellen Monitor öffnen: `pio run -e seeed_xiao_esp32s3 --target monitor`
2. `>>> VERBUNDEN mit der Node` sollte erscheinen
3. Von einem zweiten Gerät im Mesh eine Lagemeldung senden (siehe oben)
4. Im Monitor sollte erscheinen: `>>> Neue Lagemeldung angelegt, ID X`
5. `liste` eintippen → sollte die neue Meldung zeigen

**Automatisierter Funktionstest:** [`scripts/test_mesh_functions.sh`](./scripts/test_mesh_functions.sh)
sendet nacheinander alle aktuell unterstützten Testnachrichten (Zustandswechsel-Codes
+ Lagemeldungen) über einen zweiten, per USB angeschlossenen Meshtastic-Node.

```bash
./scripts/test_mesh_functions.sh --port /dev/cu.usbmodem2101
```

Seriellen Monitor des Interface-Boards währenddessen offen halten und nach jeder
gesendeten Nachricht mit `status`, `liste` bzw. `detail <ID>` gegenprüfen. Optional
Wartezeit zwischen den Nachrichten anpassen: `--delay <Sekunden>` (Default: 3).

### Troubleshooting

| Problem | Ursache | Fix |
|---|---|---|
| Upload schlägt fehl (`serial noise`, Timeout) | Anderes Terminal/Monitor blockiert den Port | Alle anderen Terminals schließen |
| Rote LED leuchtet dauerhaft bei USB | Normal — eingebaute Charge-LED | Kein Fehler |
| `SQL-Fehler: disk I/O error` beim Start | **Bekannter Bug** der `Sqlite3Esp32`-Bibliothek: `PRIMARY KEY`/`UNIQUE`-Constraints lösen auf SPIFFS zuverlässig I/O-Fehler aus ([Issue #18](https://github.com/siara-cc/esp32_arduino_sqlite3_lib/issues/18)) | Behoben: Schema nutzt keine expliziten Constraints mehr, IDs laufen über SQLites eingebaute `rowid` |
| Monitor zeigt nichts/Datenmüll | `monitor_speed` in `platformio.ini` passt nicht zu `Serial.begin()` im Code | `monitor_speed = 115200` setzen |
| Funkgerät sendet laut CLI erfolgreich, App zeigt nichts | App noch per USB verbunden (Port-Konflikt) oder Bluetooth-Kopplung verloren | Funkgerät nur per Strom + D6/D7 betreiben, App per **Bluetooth** verbinden |

---

## 🔗 Weiterführende Links

- [Projekt-Wiki](https://github.com/Pixeldieb/kayna-funkt-notmeldeterminal/wiki) — Status-Dashboard über beide Boards und alle Themenbereiche, in einfacher Sprache
- [Seeed XIAO ESP32-S3 – Getting Started](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
- [Meshtastic – Offizielle Dokumentation](https://meshtastic.org/docs/)
- [Meshtastic – Serial Module Konfiguration](https://meshtastic.org/docs/configuration/module/serial/)
- [Meshtastic-Arduino Library (GitHub)](https://github.com/meshtastic/Meshtastic-arduino)
- [Sqlite3Esp32 Library (GitHub)](https://github.com/siara-cc/esp32_arduino_sqlite3_lib)
- [PlatformIO Dokumentation](https://docs.platformio.org/)

---

## 📝 Changelog

### 2026-09-22

- **Diese README neu geordnet**: Projekt wird jetzt zuerst als hardware-neutrales
  Interface (`src/common/`) beschrieben, SenseCAP Indicator als aktuelles
  Hauptziel nach oben geholt, der XIAO-Weg klar als Historie abgesetzt (vorher
  stand die README noch komplett aus XIAO-Sicht, ein Rest aus der Umbenennung).
  Dabei außerdem korrigiert: die Zustellbestätigungs-Beschreibung für SenseCAP
  nannte noch das alte Transport-ACK (`ROUTING_APP`), das schon am 2026-09-18
  als Sicherheitslücke gefixt wurde (siehe unten, "B5") — beschrieb also einen
  längst überholten Stand als aktuell.
- **XIAO-Namensverwechslung aufgelöst**: "XIAO-Board" meinte bisher uneinheitlich
  zwei verschiedene physische Geräte — das XIAO-ESP32-S3-Interface-Board (kein
  eigenes Funkmodul) und das XIAO-nRF52-Funkgerät (echte Meshtastic-Firmware).
  Ab jetzt überall ausgeschrieben.

### 2026-09-18

- **SenseCAP Indicator: echtes Meshtastic-Protokoll** über den eingebauten SX1262
  (nicht mehr nur rohes LoRa) — Paketheader, AES128-CTR-Verschlüsselung, Protobuf-
  Payload und Kanal-Hash exakt nach `meshtastic/firmware`s eigenem Quellcode
  nachgebaut (Frequenz/BW/SF/CR/Sync/Präambel ebenso). Protobuf-Definitionen nicht
  handkodiert, sondern nanopb + Meshtastics eigene generierte Header vendored
  (`lib/meshtastic_proto/`). Details: [src/sensecap/README.md](src/sensecap/README.md)
  Abschnitt 5, [Issue #36](https://github.com/Pixeldieb/kayna-funkt-notmeldeterminal/issues/36).
- Die alte externe-Node-Bridge (`meshtastic_bridge.h/.cpp`, Weg 2) für dieses Board
  entfernt — endgültig nicht machbar (kein freier GPIO, RP2040-Co-Prozessor hat keine
  Hardware-Verbindung zum SX1262, siehe Board-README).
- **Live gegen ein echtes Meshtastic-Gerät verifiziert** (Folgesession, selber Tag):
  Broadcast bidirektional bestätigt, danach zwei reale Blocker gefunden und behoben —
  moderne Firmware lehnt nicht-PKI-Direktnachrichten auf `TEXT_MESSAGE_APP` ab
  ("legacy DM", Fix: eigener `PRIVATE_APP`-Portnum), und Broadcasts werden nie
  bestätigt (Fix: eigener privater Kanal + Direktnachricht mit ACK an eine
  konfigurierbare Leitstelle — siehe "B5" direkt unten für den entscheidenden
  Nachfolge-Fund am selben Tag). NodeInfo-Austausch ergänzt. Nebenbei einen
  unabhängigen, bis dahin unbemerkten Bug gefunden: `lageDbBegin()` fehlte auf
  diesem Board komplett, die Notmeldungshistorie lief seit Board-Einführung ins
  Leere. Details im [Wiki](https://github.com/Pixeldieb/kayna-funkt-notmeldeterminal/wiki/SenseCAP-Meshtastic).
- **B5 (kritisch): Transport-ACK ≠ echte Zustellung.** Das anfängliche
  `ROUTING_APP`-ACK von oben bestätigt nur, dass ein Paket auf Transport-Ebene
  irgendwo ankam — nicht, dass die Leitstelle es inhaltlich akzeptiert hat. Es
  wird sogar gesendet, *bevor* die empfangende Seite überhaupt ihre
  Sicherheitsprüfung durchläuft. Live reproduziert: ein fremder Node im Mesh
  konnte fälschlich als "Leitstelle bestätigt" durchgehen. Fix: ein echtes
  Anwendungs-ACK (`LAGE:ACK:<hex packetId>`), das die empfangende Seite erst nach
  erfolgreicher `meshSecurityCheck()` *und* DB-Speicherung sendet, mit Prüfung
  dass es wirklich vom konfigurierten Leitstellen-Node kommt. Details:
  [src/sensecap/README.md](src/sensecap/README.md) Abschnitt 5.6,
  [Wiki/Sicherheit](https://github.com/Pixeldieb/kayna-funkt-notmeldeterminal/wiki/Sicherheit).
- **Sicherheitslücken aus der Codebase-Analyse geschlossen** ([#1](https://github.com/Pixeldieb/kayna-funkt-notmeldeterminal/issues/1),
  [#3](https://github.com/Pixeldieb/kayna-funkt-notmeldeterminal/issues/3)): eingehende
  Lagemeldungen (und auf dem XIAO-ESP32-S3-Interface-Board die Zustands-Kurzcodes)
  wurden von jedem Absender ungeprüft übernommen. Neues gemeinsames Modul
  `src/common/mesh_security.h` mit Absender-Allowlist (sicherer Default: leer =
  alles verwerfen, nicht alles erlauben) und Ratenbegrenzung, in beide Boards
  eingebaut.
- **Heartbeat-Telemetrie** ([#13](https://github.com/Pixeldieb/kayna-funkt-notmeldeterminal/issues/13)):
  SenseCAP-Board sendet periodisch einen Status-Broadcast, damit eine Leitstelle
  eine ausgefallene Station am ausbleibenden Lebenszeichen erkennt — ehrlich ohne
  Akku-/Sabotage-Werte, da dafür noch keine Sensorik existiert.

### 2026-09-17

- **Zweites Board: Seeed SenseCAP Indicator (D1L)** — eigenständiges Touchscreen-
  Terminal (480×480, ST7701S/FT6336U), `env:sensecap_indicator` in `platformio.ini`.
  Menü/Notfall-Flow (Auswahl → Bestätigung mit Halte-Geste → Senden → Erfolg/
  Fehlschlag), Lagemeldungen in SQLite (wiederverwendet vom XIAO-ESP32-S3-Interface-
  Board), Statusleiste, Notmeldungshistorie. Details: [src/sensecap/README.md](src/sensecap/README.md).
- Umfangreiches Hardware-Bring-up nötig: Bootloop-Ursachen (PSRAM-/Flash-Modus,
  Partitionstabelle), auf dem Kopf montiertes Panel, und Rendering-Glitches durch
  fehlenden Doppelpuffer (behoben über Cache-Writeback + Frame-Sync-Callback +
  reduzierten Pixeltakt) — siehe Board-README für die volle Fehlersuche-Geschichte.
- Meshtastic-Anbindung für das neue Board: eingebautes SX1262 sendet/empfängt jetzt
  zuverlässig rohes LoRa (Reset-Settle-Timing-Fix, nicht das vermutete BUSY-Timing).
  Externe Node über UART bleibt aus GPIO-Mangel verworfen. Echte Meshtastic-Protokoll-
  Kompatibilität (Verschlüsselung/Routing/Kanäle) ist der jetzt eigentliche offene
  Punkt — strategische Scope-Frage, dokumentiert in [Issue #36](https://github.com/Pixeldieb/kayna-funkt-notmeldeterminal/issues/36).
- `src/` neu strukturiert für mehrere Boards: `src/xiao/`, `src/common/`
  (gemeinsam genutztes `lage_db`), `src/sensecap/`.

### 2026-09-16

- **Zustandsmodell der Säule ergänzt** (STANDBY/AKTIV/STROMAUSFALL/WARTUNG/SABOTAGE), abfragbar per neuem Serial-Befehl `status`
- Zustandswechsel per Kurz-Code als Mesh-Broadcast (`LGE`, `NOR`, `WTG`, `SAB`, `SAUS`) — bewusst kurz und ohne Alltagswörter, um Tippfehler/versehentliches Auslösen zu vermeiden
- Feature-Roadmap strukturiert: Aufteilung in drei Repos (Mesh-Brain hier, [notfallbox-update-station](https://github.com/Pixeldieb/notfallbox-update-station) für Captive Portal/OTA, privates `notmeldestelle-leitstelle-spec` für die Leitstellen-Schnittstelle), Milestones „MVP Feldtest Kayna/Zeitz" und „Post-Pilot"
- `upload_speed` in `platformio.ini` auf 115200 gesenkt (460800 war beim Flashen über USB unzuverlässig)

### 2026-09-15

- Grundgerüst: ESP32-S3 ↔ Meshtastic-Node über D6/D7 (UART, gekreuzt), Verbindungsaufbau mit vollständigem Handshake-Wait
- LED-Statusanzeige (Warte-/Erfolgs-/Sende-/Heartbeat-Muster) über die eingebaute LED
- Test-Sendung im festen Intervall (zuletzt: alle 5 Minuten)
- Meshtastic-Node per CLI vollständig eingerichtet (Region, Serial-Modul, Owner, Kanal-Check)
- **Lagemeldungen-Feature ergänzt:** eingehende `LAGE:`-Nachrichten werden geparst und gespeichert
  - Nachrichtenformat zunächst mit Pipe (`|`), auf Anwenderwunsch auf Semikolon (`;`) umgestellt
  - Speicherung zunächst als JSON-Datei (LittleFS + ArduinoJson) prototypisch umgesetzt
  - Auf Wunsch durch echte relationale Datenbank ersetzt: SQLite (`Sqlite3Esp32`) mit zwei Tabellen (`lagemeldungen`, `lage_historie`) für Filterung, Kategorien und vollständige Änderungshistorie
  - Serial-CLI-Befehle (`liste`, `liste kategorie`, `liste status`, `detail`) zum Prüfen ohne zusätzliches Interface
  - Bug behoben: `disk I/O error` durch `PRIMARY KEY`/`UNIQUE`-Constraints auf SPIFFS (bekannter Library-Bug) — Schema auf implizite `rowid` umgestellt
- Wissensdatenbank-Artikel in Odoo Knowledge angelegt und laufend um Node-Vorbereitung (CLI-Checks, Einstellungen, Firmware-Update-Weg für XIAO nRF52840) erweitert

---

## 📄 Lizenz

Siehe [LICENSE](./LICENSE).
