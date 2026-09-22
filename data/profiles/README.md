# Deployment-Profile

Eine Profil-Datei fasst alle `StationConfig`-Werte (siehe
`src/sensecap/station_config.h`) einer Station an einem Ort zusammen, statt
sie am Gerät durch den Ersteinrichtungs-Assistenten zu klicken. Aus einem
Profil erzeugt `scripts/build_profile_firmware.py` ein fertiges, flashbares
Firmware-Image inklusive Web-Flasher-Manifest (siehe `webflash/README.md`).

- `profiles.example.json` — committed, nur fiktive Beispiel-/Prototypwerte.
  Dieses Repo ist öffentlich; echte Einsatzdaten (Leitstellen-Node-IDs,
  echte Standorte/Kontakte der 200+ Feldgeräte) gehören **nicht** hierher.
- `profiles.local.json` — vom Nutzer lokal angelegt, per `.gitignore`
  ausgeschlossen. Gleiches Schema wie `profiles.example.json`. Für echte
  Stationen: `--profiles-file data/profiles/profiles.local.json` an beide
  Skripte übergeben.

## Schema

```json
{
  "profiles": [
    {
      "name": "eindeutiger-profilname",
      "board_env": "sensecap_indicator",
      "stationId": "STATION-042",
      "operatorName": "Name des Betreibers",
      "localContact": "Ansprechpartner/Telefonnummer",
      "settingsPin": "1234",
      "dispatchNodeNum": "0xce0ffa28",
      "locationText": "Ort/Beschreibung"
    }
  ]
}
```

`board_env` muss einem `[env:...]`-Namen in `platformio.ini` entsprechen. Ein
neues Hardware-Target braucht nur einen neuen `platformio.ini`-Env plus
Profile mit passendem `board_env` — keine Änderung an den Skripten.

`dispatchNodeNum` weglassen oder `null` setzen, wenn die Station ihre
Leitstelle noch nicht kennt — dann läuft beim ersten Boot weiterhin der
normale Ersteinrichtungs-Assistent (Ort + Leitstelle) statt automatisch
fertig eingerichtet zu sein.
