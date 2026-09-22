# UI-Dokumentation

Diese Dokumentation beschreibt die Benutzeroberfläche des ESP32-Notfallterminals.

Die UI ist so dokumentiert, dass sowohl Menschen als auch KI-Agenten die
Menüstruktur, Seiten, Bedienelemente und Bedienabläufe eindeutig verstehen
können.

---

## Dateien

| Datei | Zweck |
|---|---|
| `ui.yaml` | Technische Quelle der Wahrheit für die komplette UI |
| `flows.yaml` | Beschreibt konkrete Bedien- und Notfallabläufe |
| `README.md` | Erklärung der UI-Struktur und Konventionen |

### Quelle der Wahrheit

`ui.yaml` ist die maßgebliche Datei für:

- vorhandene Seiten
- Seiten-IDs
- UI-Elemente
- Button-IDs
- Navigation
- Aktionen
- Zustände
- Hardware-Anzeigen

`flows.yaml` ergänzt diese Informationen um konkrete Bedienabläufe.

---

## 1. Grundprinzip

Das Terminal verfügt über ein Hauptmenü, das nur während eines
Stromausfalls verfügbar ist.

Im aktiven Zustand signalisiert eine leicht pulsierende Signal-LED,
dass das Terminal verfügbar ist.

Das Hauptmenü besteht aus vier Bereichen:

```
Hauptmenü
├── Feuerwehr
├── Polizei
├── Krankenwagen
└── Info
```

## 2. Hauptmenü

Page ID: `main_menu`

Das Hauptmenü ist nur verfügbar, wenn:

```
system.power_failure == true
```

Bei aktiviertem Hauptmenü pulsiert die `signal_led` leicht.

### Auswahl

| Anzeige | ID | Ziel |
|---|---|---|
| Feuerwehr | `fire_department` | `fire_department_menu` |
| Polizei | `police` | `police_menu` |
| Krankenwagen | `ambulance` | `ambulance_menu` |
| Info | `information` | `information_menu` |

Die vier Punkte belegen `sel_1`–`sel_4`. Als Root-Seite hat `main_menu`
keinen leeren Kontext-Bereich — alle drei Kontext-Slots sind hier inaktiv.

## 3. Feuerwehr

Page ID: `fire_department_menu`

```
Feuerwehr
├── Brand
├── Chemie
├── Verkehrsunfall
└── Andere
```

Die vier Auswahlpunkte belegen die vier Auswahl-Slots (`sel_1`–`sel_4`,
siehe Abschnitt 19). „Zurück" ist kein fünfter Menüpunkt mehr, sondern
liegt auf dem Kontext-Slot `ctx_2` unterhalb des Displays.

Jede Auswahl führt zur Seite `emergency_confirmation`. Dabei werden die
Informationen zum gewählten Notfall an die Bestätigungsseite übergeben.

Beispiel:

```
Feuerwehr
└── Brand
    └── Notfall bestätigen
```

## 4. Polizei

Page ID: `police_menu`

```
Polizei
├── Einbruch
├── Diebstahl
├── Sicherheit
└── Andere
```

Slots wie bei Feuerwehr: `sel_1`–`sel_4` für die Auswahl, `ctx_2` für
„Zurück". Jede Auswahl führt zu `emergency_confirmation`.

## 5. Krankenwagen

Page ID: `ambulance_menu`

```
Krankenwagen
├── Verletzung
├── Notarzt
├── Transport
└── Andere
```

Slots wie bei Feuerwehr: `sel_1`–`sel_4` für die Auswahl, `ctx_2` für
„Zurück". Jede Auswahl führt zu `emergency_confirmation`.

## 6. Notfall bestätigen

Page ID: `emergency_confirmation`

Vor dem Absenden eines Notfalls wird immer eine Bestätigungsseite mit
Warnhinweis angezeigt.

```
Notfall bestätigen

⚠ Nur bei Notfall benutzen.
  Missbrauch wird strafrechtlich verfolgt.

  ctx_1        ctx_2         ctx_3
              [Abbruch]
[——————— Bestätigung (halten) ———————]
```

**Abbruch** — ID: `cancel`, Slot `ctx_2`, Aktion: `back`. Einfacher Druck
genügt. Der Notfall wird nicht ausgelöst.

**Bestätigung** — ID: `confirm`, Slots `ctx_1` + `ctx_3` gemeinsam,
Elementtyp `hold_button`. Beide Slots müssen gleichzeitig für
`hold.duration_ms` (Default 1500 ms, siehe Abschnitt 19) gehalten werden,
bevor `trigger_emergency` ausgeführt wird. Damit kann ein einzelner
versehentlicher Tastendruck/Touch keine Notfallmeldung auslösen.

## 7. Notfallübertragung

Page ID: `emergency_transmission`

Nach der Bestätigung:

1. Die Warn-LED wird aktiviert.
2. Eine Meshtastic-Nachricht wird erzeugt.
3. Die Nachricht wird über Meshtastic übertragen.
4. Der Übertragungsstatus wird angezeigt.

Bei erfolgreicher Übertragung: `emergency_details`
Bei fehlgeschlagener Übertragung: `emergency_transmission_failed`

## 8. Erfolgreiche Übertragung

Page ID: `emergency_details`

Nach erfolgreicher Übertragung werden die Notfalldetails auf dem Display
angezeigt. Diese Informationen sollen vom Benutzer notiert werden können.

Angezeigte Felder (aus `emergency.last_transmission`):

| Feld | Bedeutung |
|---|---|
| `case_number` | Vorgangsnummer |
| `timestamp` | Zeitstempel |
| `station_id` | Notfallsäule |
| `location` | Ort |

„Zurück" (ID `acknowledge`) liegt auf Kontext-Slot `ctx_2` und navigiert
zu `main_menu`.

## 9. Fehlgeschlagene Übertragung

Page ID: `emergency_transmission_failed`

Bei einem Übertragungsfehler werden zwei Möglichkeiten angeboten:

```
Übertragung fehlgeschlagen

 ctx_1            ctx_2
[Erneut senden]  [Abbruch]
```

**Erneut senden** — Slot `ctx_1`, startet die Übertragung erneut.
**Abbruch** — Slot `ctx_2`, kehrt zum Hauptmenü zurück.

## 10. Info

Page ID: `information_menu`

```
Info
├── Lageinformationen anfordern
└── Systeminformationen Krisenstab
```

Nur zwei Auswahlpunkte, belegen `sel_1`/`sel_2`. `sel_3`/`sel_4` sind auf
dieser Seite ungenutzt. „Zurück" liegt wie bei den anderen Menüs auf
`ctx_2`.

## 11. Lageinformationen

Page ID: `situation_information`

Zeigt aktuelle Lageinformationen und letzte empfangene Meldungen. Die
beiden rechten Bedienelemente dienen zum Scrollen.

```
┌──────────────────────────────┐
│ Lageinformationen            │
│                              │
│ Letzte Meldungen             │
│                              │
│ Meldung 1                    │
│ Meldung 2                    │
│ Meldung 3                    │
│                              │
│  ctx_1    ctx_2    ctx_3     │
│ [Hoch]  [Zurück]  [Runter]   │
└──────────────────────────────┘
```

Die Meldungen werden aus `situation_information.messages` geladen.

## 12. Systeminformationen Krisenstab

Page ID: `crisis_staff_information`

Zeigt Informationen und letzte Meldungen für den Krisenstab. Auch hier
dienen die beiden rechten Bedienelemente zum Scrollen.

```
┌──────────────────────────────┐
│ Systeminformationen          │
│ Krisenstab                   │
│                              │
│ Letzte Meldungen             │
│                              │
│ Meldung 1                    │
│ Meldung 2                    │
│ Meldung 3                    │
│                              │
│  ctx_1    ctx_2    ctx_3     │
│ [Hoch]  [Zurück]  [Runter]   │
└──────────────────────────────┘
```

Die Meldungen werden aus `crisis_staff_information.messages` geladen.

## 13. Navigationsregeln

### Zurück

Wenn eine Seite mit:

```yaml
action:
  type: back
```

definiert ist, wird der vorherige Navigationszustand wiederhergestellt.
Die Firmware sollte dafür einen Navigations-Stack verwenden.

Beispiel:

```
main_menu
→ fire_department_menu
→ emergency_confirmation
```

Nach Abbruch:

```
emergency_confirmation
→ fire_department_menu
```

## 14. IDs

Jede Seite und jedes interaktive Element besitzt eine eindeutige ID.

Beispiele:

```
main_menu
fire_department_menu
police_menu
ambulance_menu
information_menu

fire_department
police
ambulance
information

confirm
cancel
```

IDs sind technische Bezeichner und sollten stabil bleiben. Die sichtbaren
Texte dürfen sich dagegen ändern.

Beispiel:

```yaml
id: fire_department
label: "Feuerwehr"
```

Die Beschriftung kann geändert werden, ohne dass sich die technische ID
ändert.

## 15. Aktionen

Die UI verwendet standardisierte Aktionstypen.

| Aktion | Bedeutung |
|---|---|
| `navigate` | Zu einer anderen Seite wechseln |
| `back` | Zur vorherigen Seite zurückkehren |
| `execute` | Eine Firmware-Funktion ausführen |
| `scroll` | Inhalt scrollen |
| `view` | Inhalt anzeigen |
| `hold_button` (Elementtyp) | Element muss über zwei Slots gleichzeitig für eine Mindestdauer gehalten werden, bevor die hinterlegte `action` ausgeführt wird (siehe Abschnitt 19) |

## 16. KI-Agenten

KI-Agenten sollen bei Änderungen an der UI zuerst `ui.yaml` lesen.

### Neue Seite

Beim Hinzufügen einer Seite müssen mindestens angegeben werden:

```yaml
- id: unique_page_id
  title: "Seitentitel"
  elements: []
```

### Neues interaktives Element

Jedes interaktive Element benötigt:

```yaml
- id: unique_element_id
  type: button
  label: "Anzeigename"
  action:
    type: ...
```

### Neue Navigation

Navigation muss über eine explizite Aktion dokumentiert werden:

```yaml
action:
  type: navigate
  target: target_page
```

### Keine implizite Navigation

Die Firmware soll nicht voraussetzen, dass ein Agent aus einem
Button-Label die Zielseite erraten muss.

Nicht:

```yaml
label: "WLAN"
```

Sondern:

```yaml
id: wifi_settings
label: "WLAN"
action:
  type: navigate
  target: wifi_settings
```

## 17. Stabilität der Dokumentation

Die UI-Dokumentation wird als Schnittstelle zwischen Firmware, Entwicklung
und KI-Agenten betrachtet.

Daher gilt: IDs sind stabiler als sichtbare Beschriftungen.

Eine Änderung wie:

```
"Feuerwehr" → "Feuerwehr / Brandbekämpfung"
```

ändert nicht die ID `fire_department`.

Eine Änderung der ID muss dagegen alle Referenzen in `ui.yaml`,
`flows.yaml`, Firmware, Tests und eventuell weiteren
Dokumentationsdateien berücksichtigen.

## 18. Bedienbaum

Die vollständige Menüstruktur lässt sich vereinfacht so darstellen:

```
Hauptmenü
│
├── Feuerwehr
│   ├── Brand
│   │   └── Notfall bestätigen
│   │       ├── Abbruch
│   │       └── Bestätigung
│   │           └── Meshtastic
│   │               └── Notfalldetails
│   │
│   ├── Chemie
│   │   └── Notfall bestätigen
│   │
│   ├── Verkehrsunfall
│   │   └── Notfall bestätigen
│   │
│   └── Andere
│       └── Notfall bestätigen
│
├── Polizei
│   ├── Einbruch
│   │   └── Notfall bestätigen
│   ├── Diebstahl
│   │   └── Notfall bestätigen
│   ├── Sicherheit
│   │   └── Notfall bestätigen
│   └── Andere
│       └── Notfall bestätigen
│
├── Krankenwagen
│   ├── Verletzung
│   │   └── Notfall bestätigen
│   ├── Notarzt
│   │   └── Notfall bestätigen
│   ├── Transport
│   │   └── Notfall bestätigen
│   └── Andere
│       └── Notfall bestätigen
│
└── Info
    ├── Lageinformationen anfordern
    │   └── Lageinformationen
    │       ├── Scroll hoch
    │       ├── Scroll runter
    │       └── Zurück
    │
    └── Systeminformationen Krisenstab
        └── Lageinformationen
            ├── Scroll hoch
            ├── Scroll runter
            └── Zurück
```

## 19. Hardware-Bedienkonzept

Die UI-Definition ist hardware-neutral: `ui.yaml` beschreibt Seiten und
Elemente, nicht Pins oder Displaytreiber. Die physische Anbindung läuft
über zwei generische Rollen-Gruppen (Details: `hardware.input` in
`ui.yaml`).

### Auswahl-Slots (`sel_1`–`sel_4`)

Vier Slots, je einem Bildschirm-Quadranten zugeordnet (oben-links,
oben-rechts, unten-links, unten-rechts). Werden für Menüauswahl
verwendet — maximal 4 Auswahlpunkte pro Seite. Ein Seiten-Element bindet
sich über `slot: sel_1` (usw.) an einen Slot.

### Kontext-Slots (`ctx_1`–`ctx_3`)

Drei Slots unterhalb/um den Inhaltsbereich, Belegung ist pro Seite
unterschiedlich (`context_bar` in `ui.yaml`):

| Slot | Konvention |
|---|---|
| `ctx_1` | Kontextabhängig — z.B. „Vor", Scroll hoch, oder Teil einer Halte-Kombination |
| `ctx_2` | Feste Rolle über die gesamte UI: „Zurück"/„Abbruch", sofern auf der Seite vorhanden |
| `ctx_3` | Kontextabhängig — z.B. „Bestätigen", Scroll runter, oder Teil einer Halte-Kombination |

### Halte-Bestätigung (`hold_button` / `hold_confirm`)

Sicherheitskritische Aktionen (aktuell: Notfall auslösen) erfordern das
gleichzeitige Halten zweier Slots für eine Mindestdauer
(`hold.duration_ms`, Default 1500 ms) statt eines einzelnen Tastendrucks
— ein einzelner versehentlicher Druck/Touch kann so nichts auslösen.

### Persistente Statusleiste (`chrome.status_bar`)

Auf jeder Seite sichtbar, unabhängig von der Navigation: Online/Offline,
Notfallsäulennummer, Ort, Signalstärke. Darunter zeigt der Breadcrumb
(`chrome.breadcrumb`) den Navigationspfad (z.B. „Polizei > Einbruch").

### Hardware-neutral heißt: austauschbares Backend

Auf einem Touch-Display sind `sel_*`/`ctx_*` Touch-Zonen über den
gerenderten Kacheln/der Kontextleiste. Auf einem Board mit physischen
Tasten wären es GPIO-Pins. Die Seiten-Definitionen in `ui.yaml` ändern
sich in beiden Fällen nicht — nur die Firmware-Bindung der Slot-IDs auf
die jeweilige Eingabequelle.

**Aktuell evaluierte Displays** (siehe `hardware.displays` in `ui.yaml`):

| Display | Rolle |
|---|---|
| Seeed SenseCAP Indicator (D1L) | Aktuelles Zielboard: ESP32-S3 + RP2040, Farb-Touch 480×480, eingebautes LoRa (SX1262) mit vorinstallierter Meshtastic-Firmware auf dem RP2040, microSD. Kandidat, um Funk + Speicher + UI auf einem Board zu konsolidieren — RP2040 spricht intern per UART das gleiche Meshtastic-Serial-Protokoll, das dieses Projekt schon zu einer externen Node nutzt. Offen: ob/wie sich die ESP32-S3-Seite mit eigener Firmware bespielen lässt, während der RP2040 bei Stock-Meshtastic bleibt. |
| Seeed ePaper-Touchpanel | Nur für schnellen, funkunabhängigen Layout-Check des Menüs — kein LoRa/UART, kein Kandidat fürs finale Terminal. |

Das ursprünglich angedachte XIAO Round Display ist damit vorerst vom
Tisch.

## Hinweis zur weiteren Entwicklung

Die drei Dateien sind bewusst so getrennt:

- `ui.yaml` → Was existiert?
- `flows.yaml` → Wie benutzt man es?
- `README.md` → Wie ist das System aufgebaut und welche Regeln gelten?

Das ist für einen KI-Agenten eine gute Trennung, weil er beispielsweise bei
einer Änderung am „Brand“-Button gezielt `ui.yaml` durchsuchen kann,
während er für einen kompletten Notrufablauf `flows.yaml` heranzieht.
