# SenseCAP Indicator (D1L) — Board-Notizen

Aktuelles Hauptziel des Projekts, neben dem historischen (zurückgestellten)
XIAO-ESP32-S3-Interface-Board + XIAO-nRF52-Funkgerät (siehe Haupt-README).
Touchscreen-Terminal mit Display, Datenbank und (angefangener) Meshtastic-Anbindung
auf **einem** Board statt der ESP32↔nRF52-Zwei-Board-Lösung.

Build/Flash: `pio run -e sensecap_indicator -t upload`

---

## 1. Status (Stand 2026-09-18)

| Teil | Status |
|---|---|
| Display (ST7701S 480x480 RGB) + Touch (FT6336U) | ✅ läuft stabil (LVGL-Refresh-Rate am 2026-09-18 von 10ms/"100fps" auf 33ms/"30fps" korrigiert, siehe Abschnitt 5.6 — vorher hat LVGL schneller geflusht als `full_refresh=1` + das Panel synchron liefern konnten) |
| Menü/Notfall-Flow (LVGL, `ui_model.cpp`) | ✅ läuft, inkl. Halte-Bestätigung (kreisrunder Fortschrittsring, kein Balken mehr), CLI-artigem 2-Stationen-Sende-Screen, Erfolg/Fehlschlag-Screens |
| Lagemeldungen in SQLite (`lage_db`, wiederverwendet von der XIAO-Säule) | ✅ läuft — siehe Abschnitt 5.6 für eine wichtige Plattform-Einschränkung (Query-Form) die am 2026-09-18 gefunden wurde |
| Uhrzeit | ⚠️ kein RTC-Chip, kein NTP/GPS auf diesem Board — läuft frei mit dem ESP32-S3-eigenen Quarz und driftet daher unkorrigiert (typisch einige Sekunden/Tag, nach mehreren Tagen spürbar). 3-stufige Quelle: (1) echte Mesh-Position-Pakete mit `time`-Feld, falls je eines ankommt, (2) letzter bekannter Stand aus NVS (überlebt Neustarts monoton), (3) Build-Zeitpunkt als letzter Fallback. Seit 2026-09-21 zusätzlich per Touchscreen stellbar (Einstellungen-Seite, Feld "Uhrzeit", `wallClockNowYMDHMS`-Vorbefüllung) — vorher ging das nur per Serial-Kommando `settime YYYY-MM-DD HH:MM:SS`, ohne Laptop im Feld also gar nicht korrigierbar. Ein echter I²C-RTC-Baustein (z.B. über den Grove-Port, der am RP2040 hängt) würde das strukturell lösen, ist für den Prototyp aber bewusst zurückgestellt (siehe Wiki, Hardware-Bring-up). |
| Meshtastic-Anbindung | ✅ **echtes Meshtastic-Protokoll**, live gegen ein reales Gerät verifiziert: Broadcast bidirektional, Direktnachricht mit echtem **Anwendungs-ACK** (nicht nur Transport-ACK — siehe Abschnitt 5.6, B5-Fix) an eine konfigurierbare, persistente Leitstelle. Details siehe Abschnitt 5. |
| Absender-Allowlist / Ratenbegrenzung (Issues #1, #3) | ✅ eingebaut (`src/common/mesh_security.h`) — sicherer Default: leere Allowlist verwirft alle eingehenden Lagemeldungen, `allow add <hex-node-id>` zum Freischalten |
| Heartbeat an Leitstelle (Issue #13) | ✅ periodischer Status-Broadcast (alle 15 Min., war 2 Min. bis 2026-09-18 — siehe Fix-Plan), ehrlich ohne Akku-/Sabotage-Werte (keine Sensorik vorhanden) |
| Standby-Screen, Alarm-Blinken, Batterie/Solar/Netz-Symbol | ❌ noch nicht begonnen (kein ADC/Batterie-Hardware vorhanden, bewusst keine Fake-Anzeige) |
| Lokaler Betreiber / Onboarding | ⚠️ Datenstruktur (`station_config.h`) + jetzt eine **PIN-geschützte Einstellungen-Seite** im Hauptmenü (Testmodus-Umschalter, Leitstelle-Node-ID, Ort als Freitext, seit 2026-09-21 zusätzlich Uhrzeit) — PIN aktuell Platzhalter `1234`, siehe `station_config.h`. Sprach-Button vorhanden, zeigt ehrlich "noch nicht umgesetzt" (keine echte Übersetzung). |

---

## 2. Hardware-Bring-up — was hart erkämpft war

Diese Einstellungen in `platformio.ini` (`env:sensecap_indicator`) sind **nicht optional**,
sondern durch Trial-and-Error an echter Hardware gefunden (siehe Git-Historie für die
volle Fehlersuche-Geschichte):

- `board_build.flash_mode = dio`, `board_build.arduino.memory_type = dio_opi` —
  jede QIO-Variante bootloopt (ROM meldet unabhängig von unserer Konfiguration `mode:DIO`,
  das PSRAM auf dieser Einheit ist Octal, nicht Quad).
- `board_build.partitions = default_8MB.csv` — Flash ist 8 MB
  (bestätigt via `esptool.py flash_id`), `default_16MB.csv` bootloopt.
- `platform = espressif32@5.4.0` (ESP-IDF 4.x) — `Arduino_GFX`s RGB-Panel-Treiber
  verträgt sich nicht mit ESP-IDF 5.x.
- Panel ist physisch **auf dem Kopf montiert** — 180°-Korrektur in
  `main.cpp` (`lvgl_disp_flush`) und `touch.cpp`, nicht im Treiber.

### Rendering-Glitches (das eigentliche Kernproblem)

Der Panel-Treiber dieser ESP-IDF-Version hat **keinen Doppelpuffer** — ein einzelner
PSRAM-Framebuffer wird kontinuierlich per DMA gescannt, während wir gleichzeitig
hineinschreiben. Behoben durch drei zusammenwirkende Maßnahmen (jede für sich war
nicht ausreichend):

1. **Cache-Writeback** nach jedem Schreiben (`Cache_WriteBack_Addr`) — PSRAM läuft über
   den CPU-Cache, die DMA liest direkt aus dem PSRAM und sah sonst veraltete Daten.
2. **Frame-Sync**: eigener `esp_lcd_new_rgb_panel`-Aufruf (nicht über `Arduino_GFX`,
   die das nicht exponiert) mit `on_frame_trans_done`-Callback — Schreibvorgänge warten
   auf den Beginn eines neuen Frames, statt an einem zufälligen Punkt mitten im Scan zu starten.
3. **PCLK gesenkt** (12→6 MHz): Ein voller Bildschirm-Kopiervorgang brauchte ~26-29ms,
   eine Bildperiode bei 12 MHz nur ~23ms — die Anzeige-Hardware hat uns also strukturell
   überholt, egal wie gut synchronisiert. Bei 6 MHz (~44ms Periode) reicht die Zeit.
   Kosten: niedrigere Bildwiederholrate (~22Hz), für ein Status-Menü unproblematisch.
4. Kopierrichtung im Flush-Loop an die physische Scan-Richtung angepasst (beide
   räumen in dieselbe Richtung, statt sich entgegenzulaufen).

**Falls nach dem nächsten LVGL-/Bibliotheks-Update wieder Glitches auftauchen:** zuerst
`disp_drv.full_refresh` prüfen (muss `0` sein, siehe Kommentar in `main.cpp`) und die
vier Punkte oben der Reihe nach neu verifizieren.

---

## 3. Architektur

- `main.cpp` — Display/Touch-Bring-up, LVGL-Glue, `setup()`/`loop()`.
- `display_profiles/sensecap_indicator_d1l.h` — alle Pin-/Timing-Konstanten für **dieses**
  Display. Neues Display = neue Profildatei mit denselben Makronamen + Include tauschen,
  sonst nichts anfassen.
- `io_expander.h/.cpp` — TCA9535-Treiber (I2C). Gated: LCD CS/RST, Touch-RST,
  LoRa NSS/RST/BUSY/DIO1.
- `touch.h/.cpp` — FT6336U (Single- und Dual-Touch-Lesen; Dual wird aktuell nirgends
  mehr gebraucht, das Panel hat ohnehin kein echtes Multitouch, siehe unten).
- `ui_model.h/.cpp` — das komplette Menü/Notfall-Flow (Hand-Nachbau von `ui/ui.yaml`,
  siehe `ui/README.md` Abschnitt 19 für das hardware-neutrale Bedienkonzept).
- `station_config.h/.cpp` — lokaler Betreiber/Stations-ID, Ansatzpunkt für ein
  künftiges Onboarding (noch keine Eingabe-UI).
- `wall_clock.h/.cpp` — echte Uhrzeit (kein RTC, wird beim Flashen aus der Build-Zeit
  gesetzt, sonst per `settime`).
- `lora_radio.h/.cpp` — Hardware-Bring-up des eingebauten SX1262 (Reset-Timing-Fix,
  RadioLibHal über den IO-Expander). Kennt nur den Chip, keine Meshtastic-Semantik.
- `meshtastic_proto.h/.cpp` — die eigentliche Meshtastic-Protokollschicht: Paketheader,
  AES128-CTR-Verschlüsselung, Protobuf-Encode/Decode, Kanal-Hash, Sende-/Empfangspfad.
  Siehe Abschnitt 5. (`meshtastic_bridge.h/.cpp`, die externe-Node-Anbindung "Weg 2",
  wurde entfernt — siehe Abschnitt 5, warum das endgültig keine Option auf diesem Board ist.)

---

## 4. Kein Multitouch

Die Halte-Bestätigung (`emergency_confirmation`) sollte ursprünglich zwei
Kontext-Tasten gleichzeitig gehalten verlangen (ui.yaml-Vorlage). Das Panel liefert
aber nachweislich keine zwei simultanen Touch-Punkte. Umgesetzt stattdessen als
**eine** Taste, 3 Sekunden halten (`confirm_btn_press_cb`, LVGL PRESSED/PRESSING/
RELEASED-Events, kein eigenes Touch-Polling mehr nötig).

---

## 5. Meshtastic-Anbindung

Siehe **[Issue #36](https://github.com/Pixeldieb/kayna-funkt-notmeldeterminal/issues/36)**
für den vollen Verlauf, und das [Wiki](https://github.com/Pixeldieb/kayna-funkt-notmeldeterminal/wiki/SenseCAP-Meshtastic)
für die kompakte Status-Zusammenfassung. Kurzfassung, Stand 2026-09-18 (autonome
Nachtsession + Live-Test-Folgesession am selben Tag):

### 5.1 CHIP_NOT_FOUND gelöst (Weg 1, `lora_radio.cpp`)

Realer Schaltplan zum Board gefunden (öffentliches Referenzprojekt
[ril3y/sensecap-indicator-d1l](https://github.com/ril3y/sensecap-indicator-d1l),
`SENSECAP_INDICATOR_PINOUT_SCHEMATIC.md`) — bestätigt unsere IO-Expander-Pinbelegung
(NSS=IO0, RST=IO1, BUSY=IO2, DIO1=IO3) exakt. Ursache für `CHIP_NOT_FOUND` war nicht
die vermutete BUSY-über-I2C-Geschwindigkeit, sondern fehlende Settle-Zeit:
`SX126x::reset()` pulst RST und hämmert danach *ohne jede Wartezeit* sofort `standby()`
über SPI. Fix in `lora_radio_wake_chip()`: eigener Reset + 20ms Settle-Zeit + ein rohes
`GET_STATUS` als Lebenszeichen-Check, **bevor** RadioLib übernimmt.

Der Schaltplan zeigt außerdem: der RP2040-Co-Prozessor hat **keine** Hardware-Verbindung
zum SX1262 (reiner Sensor-Co-Prozessor über ein festes COBS-Binärprotokoll) — die früher
angedachte "RP2040 als Relay"-Ausweichoption für Weg 2 ist damit hinfällig, und Weg 2
(externe Node über UART) bleibt aus GPIO-Mangel verworfen. `meshtastic_bridge.h/.cpp`
wurde entfernt.

### 5.2 Echtes Meshtastic-Protokoll (nicht nur rohes LoRa)

`meshtastic_proto.h/.cpp` implementiert das tatsächliche Meshtastic-Wire-Format —
kein eigenes/inkompatibles Format. Jede Konstante ist direkt aus `meshtastic/firmware`s
eigenem Quellcode gelesen (nicht aus dem Gedächtnis rekonstruiert):

| Was | Wert | Quelle |
|---|---|---|
| Paketheader (16 Byte: to/from/id/flags/channel/next_hop/relay_node) | exakter Byte-Layout | `RadioInterface.h` (`PacketHeader`) |
| Verschlüsselung | AES128-CTR, Nonce = 8B PacketID (LE) + 4B NodeNum (LE) + 4B Zähler | `CryptoEngine.cpp/.h` |
| Default-Kanal-PSK | `d4 f1 bb 3a 20 29 07 59 f0 bc ff ab cf 4e 69 01` (öffentlich, kein Geheimnis) | `Channels.h` (`defaultpsk`) |
| Kanal-Hash | `xorHash(name) ^ xorHash(psk)` | `Channels.cpp` (`generateHash`) |
| Payload | Protobuf-`Data`-Message (Portnum + Bytes) | `mesh.pb.h`, vendored in `lib/meshtastic_proto/` |
| Frequenz (Region EU_868 + Preset LONG_FAST) | **869.525 MHz**, genau ein Kanal-Slot (kein Hopping — `numChannels = floor(0.25/0.25) = 1`) | `RadioInterface.cpp` (`applyModemConfig`, `regions[]`) |
| BW/SF/CR/Sync/Präambel | 250kHz / SF11 / 4:5 / `0x2b` / 16 Symbole | `MeshRadio.h`, `RadioLibInterface.h`, `RadioInterface.h` |

Protobuf: nicht händisch kodiert, sondern nanopb 0.4.9.1 + Meshtastics eigene generierte
`.pb.h/.cpp` vendored in `lib/meshtastic_proto/` (siehe dessen README für Provenienz) —
das eliminiert praktisch jedes Risiko eines Feldnummern-/Wire-Type-Fehlers.

**Zwei Kanäle gleichzeitig** (beide auf derselben Frequenz — Region+Preset legen die
Frequenz fest, unabhängig vom Kanalnamen/-schlüssel, genau wie bei echter Firmware mit
mehreren Kanälen auf einem Radio):

| Kanal | Name/PSK | Zweck |
|---|---|---|
| Öffentlich | `"LongFast"`, Standard-PSK (Index 1) — der Werkskanal jedes Meshtastic-Geräts | NodeInfo-Austausch, generische ACK-Antworten, `mesh send`-Testkommando |
| Privat | `"kayna-funkt"`, kurzer Preset-Schlüssel (Index 5, offiziell in `channel.proto` dokumentiertes Kurz-Schema) | Notmeldungen als Direktnachricht — siehe 5.5 warum nicht der öffentliche Kanal |

**Verifiziert, nicht nur behauptet** (Ehrlichkeits-Grundsatz dieses Projekts): erst per
HF-Selbstempfang-Roundtrip und unabhängiger Krypto-Gegenrechnung in Python
(pycryptodome, komplett andere Implementierung als das on-device mbedtls — Ergebnis
byte-identisch), dann **live gegen ein reales Meshtastic-Gerät** (siehe 5.5).

### 5.3 Was zum Testen bereitsteht

- Serial-Kommando `mesh send <text>` — sendet eine echte Meshtastic-Textnachricht als
  Broadcast auf dem öffentlichen Kanal.
- Serial-Kommando `dispatch set <hex-node-id>` — legt die Leitstellen-Node-Nummer fest
  (persistent in NVS, siehe 5.5).
- Serial-Kommando `test emergency` — löst nur den reinen Funk-/Protokoll-Sendepfad aus
  (ruft `meshtastic_send_emergency()` direkt auf), ohne die UI-Statusmaschine
  (`g_transmission_pending`) zu durchlaufen. `test emergency ui` löst stattdessen
  denselben Pfad wie ein echter Touchscreen-Halte-Vorgang aus (VG-Nummerierung,
  Stufen-Anzeige, Erfolg/Fehlschlag-Screens) — für Tests ohne Touch-Zugriff.
- Der Notfall-Bestätigungs-Flow (`ui_model.cpp` → `meshtastic_send_emergency()`) sendet
  echte Meshtastic-Direktnachrichten im bekannten `LAGE:...`-Format an die konfigurierte
  Leitstelle, mit 2 echten Stationen ("Notmeldung gesendet" / "Von der Leitstelle
  bestätigt") und wartet bis zu 8s auf ein echtes Anwendungs-ACK (siehe 5.6), bevor der
  Erfolgs-Screen gezeigt wird.
- Empfang läuft mit: eingehende Textnachrichten (auch von echten Fremdgeräten auf einem
  der beiden Kanäle) werden dekodiert, geloggt, und `LAGE:`-formatierte Nachrichten in
  `lage_db` übernommen (`ui_model_notify_rx()` für die Statusleiste).
- ONLINE/OFFLINE in der Statusleiste zeigt einen echten Zustand: `true` sobald
  `lora_radio_begin()` **und** `meshtastic_proto_begin()` beim Boot erfolgreich waren.

### 5.4 Bewusst nicht gemacht

- Kein Routing/Rebroadcast (Store-and-Forward über mehrere Hops) — wir senden/empfangen
  nur direkt, wie ein einfacher Leaf-Node.
- Kein DIO1-Hardware-Interrupt (der Pin hängt am IO-Expander, nicht an einem echten
  ESP32-GPIO) — `meshtastic_proto_loop()` pollt stattdessen `getIrqStatus()` per SPI
  jede `loop()`-Iteration, unkritisch für die Latenz dieses Projekts.
- Kein PKI/Curve25519 — nur Kanal-PSK-Verschlüsselung (deshalb `PRIVATE_APP` statt
  `TEXT_MESSAGE_APP` für Direktnachrichten, siehe 5.5).
- Nur eine feste Leitstellen-Node-Nummer, kein Broadcast *und* Bestätigung gleichzeitig
  (würde zwei Pakete pro Notmeldung bedeuten) — siehe Code-Kommentar bei
  `meshtastic_send_emergency()`.
- Node-Nummer wird aus der ESP32-MAC abgeleitet, nicht mit Meshtastics eigenem Algorithmus
  nachgebildet — für die Protokoll-Kompatibilität irrelevant (jede stabile, von 0 und
  Broadcast verschiedene 32-Bit-Zahl funktioniert).

### 5.5 Live-Test-Ergebnisse (2026-09-18, Folgesession am selben Tag)

Mit einem echten Meshtastic-Gerät getestet. Ergebnis: Broadcast funktioniert sofort
bidirektional (Text hin und zurück, auch auf dem neu angelegten privaten Kanal). Für
Direktnachrichten mit Zustellbestätigung mussten zwei echte, live gefundene Blocker
gelöst werden:

1. **"Rejecting legacy DM"**: moderne Meshtastic-Firmware (`Router.cpp`) lehnt
   nicht-PKI-verschlüsselte Direktnachrichten auf Portnum `TEXT_MESSAGE_APP` grundsätzlich
   ab — eine bewusste Sicherheitsmaßnahme, kein Bug, unabhängig davon ob Kanal/PSK/Hash
   korrekt sind. Fix: Notmeldungen laufen über `PRIVATE_APP` (Portnum 256, offiziell in
   `portnums.proto` für genau solche eigenen Anwendungen reserviert), das ist von dieser
   Prüfung ausgenommen.
2. **Broadcasts werden nie bestätigt**: Router.cpp entfernt `want_ack` explizit für jeden
   Broadcast. Für einen echten Zustellungs-Faktor auf der Notmeldesäule (Nutzeranforderung)
   mussten Notmeldungen von Broadcast auf Direktnachricht an eine feste, konfigurierbare
   Leitstellen-Node-Nummer umgestellt werden — siehe den neuen privaten Kanal in 5.2.

Nach beiden Fixes: reproduzierbar echtes `ROUTING_APP`-ACK (`error_reason=NONE`) vom
Testgerät erhalten, geloggt als "Notmeldung von der Leitstelle bestaetigt!".

Dabei zwei weitere reale Bugs gefunden und sofort gefixt:

- **Selbstecho-Endlosschleife**: die eigene NodeInfo-Broadcast kam per HF-Selbstkopplung
  zurück, das Gerät hielt sich selbst für einen fremden Absender und antwortete sich
  selbst — Endlosschleife, die den Kanal zugespammt hat. Fix: `header.from == eigene
  Node-Nummer` wird jetzt ganz am Anfang von `handleReceivedPacket()` verworfen.
- **`lageDbBegin()` fehlte komplett** auf diesem Board (im Gegensatz zu `src/xiao/main.cpp`)
  — die SQLite-Datenbank war seit Board-Einführung nie geöffnet, jeder Schreib-/Lesezugriff
  ist still fehlgeschlagen. Symptom: Notmeldungshistorie zeigte immer ein leeres Feld ohne
  Absturz (SQLite gibt bei Null-Handle nur einen Fehlercode zurück statt zu asserten) — sah
  nach Rendering-Bug aus, war aber schlicht "hat nie etwas gespeichert".

Außerdem gefunden: die Leitstellen-Node-Nummer war nur im RAM, jeder Neustart/Neuflash
setzte sie zurück auf 0 ohne UI-Hinweis — vermutlich Ursache eines zwischenzeitlich
gemeldeten Fehlschlags. Jetzt persistent in NVS (`station_config_set_dispatch_node()`).

### 5.6 Testprotokoll + Fix-Runde (2026-09-18, Folgesession)

Nutzer ist strukturiert jeden UI-Bereich durchgegangen (Testprotokoll A-G), daraus ein
konsolidierter Fix-Plan, live gegen echte Hardware umgesetzt und getestet. Wichtigste
Funde:

- **B5 (kritisch): Falscher Erfolg möglich gewesen.** Das `ROUTING_APP`-Transport-ACK
  aus 5.5 bestätigt nur "ein Paket kam irgendwo an", nicht dass die Leitstelle die
  Notmeldung inhaltlich akzeptiert hat — und wird sogar VOR jeder
  Sicherheits-/Inhaltsprüfung der Gegenseite gesendet. Live reproduziert: bei zu vielen
  Nodes im Mesh kam ein Transport-ACK von einer Nicht-Leitstelle zustande, Erfolg wurde
  fälschlich angezeigt. Fix: eigenes **Anwendungs-ACK** (`LAGE:ACK:<hex packetId>`),
  das die empfangende Seite erst NACH erfolgreicher `meshSecurityCheck()` + DB-Speicherung
  sendet (`sendApplicationAck()` in `meshtastic_proto.cpp`), plus Prüfung dass das ACK
  wirklich vom konfigurierten `dispatchNodeNum` kommt. Das alte Transport-ACK wird nur
  noch geloggt, nie mehr für Erfolg gewertet. Live end-to-end verifiziert (echtes ACK
  vom Testknoten via eines kleinen Python-Leitstelle-Simulators, siehe unten).
- **Plattform-Falle: SQLite/SPIFFS bricht bei bestimmten Query-Formen.** Jede Query mit
  WHERE-Klausel (selbst trivial `WHERE 1=1`) ODER `ORDER BY` ohne `LIMIT` schlägt auf
  diesem Sqlite3Esp32+SPIFFS-Setup mit `SQLITE_IOERR` ("disk I/O error") fehl, sobald
  die Tabelle ein paar Dutzend Zeilen hat — nur `ORDER BY x DESC LIMIT n` ganz ohne
  WHERE ist zuverlässig. Ausführlich als Kommentar am Anfang von `src/common/lage_db.cpp`
  dokumentiert; jede neue Query gegen `lagemeldungen`/`ereignisse` muss das beachten
  (Filtern gehört nach C++, nicht in SQL).
- **Lageinformationen zeigte fast nichts an**, weil `lageDbGetRecentSummaries()` nur die
  10 zuletzt geänderten Zeilen der GESAMTEN Tabelle holte und danach erst nach "wirklich
  empfangen" filterte — bei viel lokaler Testaktivität (Halte-Geste, `test emergency`)
  füllten lokale Einträge das ganze 10er-Fenster. Fix: großzügiger Rohabruf (50 Zeilen),
  Filterung + Anzeige-Limit (10) danach in C++.
- **LVGL-Bildwiederholrate falsch konfiguriert**: `LV_DISP_DEF_REFR_PERIOD` stand auf
  10ms ("~100fps"), aber `disp_drv.full_refresh=1` (siehe Abschnitt 2) bedeutet jede
  Änderung löst einen kompletten Flush aus, der auf die reale Panel-Framerate von ~23ms
  synchronisiert wartet — LVGL hat also ständig schneller geflusht als möglich, was das
  Sync-Fenster für Tearing/Flackern offen gehalten hat. Jetzt 33ms (~30fps, siehe
  `include/lv_conf.h`). Nebenfund: die Statusleiste hat bei jedem Tick (~50-100x/Sek.)
  unconditional ihre Farbe neu gesetzt, auch ohne Änderung — jetzt nur noch bei echter
  Zustandsänderung.
- **Neu: Kontakt-Protokollierung.** Jeder Kontakt vom konfigurierten Leitstellen-Node
  wird jetzt ins Ereignisprotokoll geschrieben (`Kontakt von Leitstelle !xxxxxxxx`,
  auf 1 Eintrag/2s gedrosselt) — Grundlage für eine künftige echte Krisenstab-Anzeige.
- **UI-Überarbeitung**: Halte-Bestätigung als kreisrunder Ring statt Balken (mit
  ruhigem, langsamem Blau-Rot-Wechsel statt hektischer Sinus-Animation — nur der kleine
  Button pulsiert, nicht der ganze Screen), CLI-artiger 2-Stationen-Sende-Screen
  ("Notmeldung gesendet" / "Von der Leitstelle bestätigt" — die alte, nur zeitbasierte
  "Verbindung zur Leitstelle"-Zwischenstation wurde entfernt, da LoRa/Meshtastic keinen
  echten Verbindungsaufbau-Schritt hat, den man dort hätte zeigen können), Popup-Tastatur
  in den Einstellungen mit Live-Vorschauzeile, unterschiedliche Hintergrundfarben pro
  Bereich (Feuerwehr rot, Polizei grün, Krankenwagen teal, Einstellungen dunkel).
- **Testwerkzeug**: `scratchpad/leitstelle_sim.py` (nicht Teil der Firmware) — ein
  Python-Skript, das über den zweiten, unabhängigen Meshtastic-Testknoten läuft und
  echte `LAGE:NEU`-Nachrichten mit einem echten Anwendungs-ACK beantwortet, für
  End-to-End-Tests ohne eine echte Leitstellen-Software. Serial-Kommando
  `test emergency ui` (im Unterschied zum älteren `test emergency`) durchläuft dabei
  denselben Code-Pfad wie ein echter Touchscreen-Halte-Vorgang (VG-Nummerierung,
  Stufen-Anzeige, Erfolg/Fehlschlag-Screens inklusive), nicht nur die reine
  Funkübertragung.

---

## 6. Bekannte Platzhalter (bewusst, nicht vergessen)

- Lageinformationen zeigt jetzt nur noch echte **empfangene** Meldungen (gefiltert nach
  `fromNode`, nicht mehr die eigenen lokal ausgelösten Notmeldungen). Systeminfo
  Krisenstab zeigt ehrlich "noch nicht angebunden" statt Platzhalter-Daten — es gibt
  noch keine echte Anbindung an ein Krisenstab-System. Die frühere automatische
  Seed-Befüllung (3 Beispieleinträge beim ersten Boot) wurde entfernt; auf Geräten,
  die vor diesem Fix schon mal geflasht wurden, können die alten `!seed0001..3`-Zeilen
  noch in der Datenbank stehen (SPIFFS-persistiert), werden aber von der Filterung
  korrekt ausgeblendet.
- Status-Leiste: TX/RX-Punkte sind verdrahtet und blinken bei echtem Funkverkehr
  (`ui_model_notify_tx/rx`). ONLINE/OFFLINE hängt an `ui_model_set_connected`, gesetzt
  in `main.cpp` sobald Radio+Protokoll beim Boot erfolgreich starten (siehe Abschnitt 5).
  `testconnect on|off` über Serial kann das zu Testzwecken übersteuern.
  **Wichtige Einschränkung (2026-09-18 im Testprotokoll gefunden):**
  `ui_model_set_connected(true)` wird genau **einmal** beim Boot aufgerufen und danach
  nie wieder automatisch neu bewertet — ONLINE bedeutet also "Radio hat beim Start
  erfolgreich initialisiert", **nicht** "ist gerade aktiv mit dem Mesh verbunden" oder
  "Leitstelle ist gerade erreichbar". Faellt das Radio spaeter aus, oder war nie ein
  Mesh-Partner in Reichweite, bleibt die Anzeige trotzdem dauerhaft ONLINE. Kandidat
  fuer einen echteren Ansatz: ONLINE nur solange zeigen, wie kuerzlich (z.B. letzte
  N Minuten) tatsaechlich ein Paket empfangen wurde oder ein Heartbeat/ACK bestaetigt
  wurde — nicht umgesetzt, siehe Fix-Plan.
- `do_trigger_emergency` speichert die Meldung immer in `lage_db` (Status wandert
  „wird übermittelt" → „übermittelt"/„fehlgeschlagen") — unabhängig davon, ob wirklich
  etwas gesendet wurde.
- Vorgangsnummer (`VG-<DB-ID>`) und Zeitstempel in der Notmeldungshistorie sind echt
  (DB-Rowid bzw. echte Wall-Clock-Sekunden, siehe `lageDbSetTimeProvider` in
  `main.cpp`) — vorausgesetzt die Uhr wurde gestellt (per Touchscreen-Feld oder
  Serial-Kommando, siehe Uhrzeit-Zeile oben) und ist nicht schon wieder
  weggedriftet (kein RTC-Chip, siehe dort).
- Leitstellen-Node-Nummer (`dispatch set`) ist persistent in NVS, seit 2026-09-18 auch
  per Touchscreen setzbar (Einstellungen-Seite, PIN-geschützt) — kein Dropdown mit
  "zertifizierten" Leitstellen, da es noch keine echte Liste davon gibt, nur ein
  Freitextfeld für die Node-ID.
