#pragma once
#include <Arduino.h>

// Local operator / station provisioning data. Hardcoded for now — this is
// deliberately its own small module (not scattered constants in ui_model.cpp)
// so a future onboarding flow has one obvious place to write real values
// into: whoever sets up a new station without prior context needs to fill
// in exactly these fields (station ID, who runs it, who to call if
// something goes wrong) before the terminal is really ready for field use.
struct StationConfig {
  String stationId;
  String operatorName;  // "lokaler Betreiber"
  String localContact;  // who/what to call if a transmission keeps failing

  // Meshtastic node number of the "Leitstelle" (dispatch) that emergency
  // reports are sent to as a direct message (so we can get a real
  // delivery ACK -- Meshtastic never acks broadcasts, see
  // meshtastic_proto.cpp). 0 = not configured yet; set via the
  // "dispatch set <hex-node-id>" serial command (main.cpp) until there's a
  // real onboarding UI for it. Persisted to NVS (see
  // station_config_set_dispatch_node()) -- found live 2026-09-18 that an
  // in-memory-only value was a real problem: it silently reset to 0 on
  // every reboot/reflash during testing, causing sends that looked
  // identical to previously-working ones to fail with no code change.
  uint32_t dispatchNodeNum = 0;

  // Manual location text (Testprotokoll B3, 2026-09-18: emergency reports
  // had no location at all). This board has no GPS hardware, so there is
  // no automatic option -- see station_config_set_location(). Empty = not
  // set yet. Persisted to NVS like dispatchNodeNum.
  String locationText = "";

  // Settings-screen PIN gate (Testprotokoll A1, 2026-09-18: main menu
  // needed a code-protected settings entry). Hardcoded placeholder like
  // stationId/operatorName above -- a real onboarding flow should make
  // this operator-settable instead.
  String settingsPin = "1234";
};

// Single shared instance. Other fields are hardcoded for now (see the
// struct comment) — callers should go through this accessor either way so
// a future onboarding flow's load/save doesn't need to touch call sites.
StationConfig &station_config();

// Sets dispatchNodeNum (both in the live StationConfig and persisted to
// NVS, so it survives the next reboot/reflash) — use this instead of
// assigning station_config().dispatchNodeNum directly.
void station_config_set_dispatch_node(uint32_t nodeNum);

// Sets locationText (live + persisted to NVS), same pattern as
// station_config_set_dispatch_node().
void station_config_set_location(const String &text);
