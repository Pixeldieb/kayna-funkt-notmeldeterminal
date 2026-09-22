#include "station_config.h"
#include <Preferences.h>

namespace {
const char *kPrefsNamespace = "kaynafunkt";
const char *kDispatchKey = "dispatch";
const char *kLocationKey = "location";
const char *kSetupDoneKey = "setupdone";
bool g_loaded = false;
} // namespace

StationConfig &station_config() {
  // TODO(onboarding): replace with real provisioning (load from NVS,
  // filled in by a setup flow) — see station_config.h.
  //
  // Field-by-field assignment, not brace-init: a default member initializer
  // (dispatchNodeNum's "= 0") makes this compiler treat the struct as
  // non-aggregate, so a `StationConfig{...}` list would fail to compile
  // (same issue as CtxSlot in ui_model.cpp).
  static StationConfig cfg;
  if (!g_loaded) {
    cfg.stationId = "STATION-01";
    cfg.operatorName = "Unbekannter Betreiber";
    cfg.localContact = "Kein lokaler Kontakt hinterlegt";

    Preferences prefs;
    prefs.begin(kPrefsNamespace, /*readOnly=*/true);
    cfg.dispatchNodeNum = prefs.getUInt(kDispatchKey, 0);
    // getString() (unlike getUInt()) logs an ESP_LOGE "len fail: NOT_FOUND"
    // for a missing key even though the default is still returned
    // correctly -- checking isKey() first avoids that misleading error
    // line on a station's very first boot, before anyone has ever set a
    // location.
    cfg.locationText = prefs.isKey(kLocationKey) ? prefs.getString(kLocationKey, "") : "";
    cfg.setupCompleted = prefs.getBool(kSetupDoneKey, false);
    prefs.end();
    g_loaded = true;
  }
  return cfg;
}

void station_config_set_dispatch_node(uint32_t nodeNum) {
  station_config().dispatchNodeNum = nodeNum; // ensures cfg is loaded first

  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/false);
  prefs.putUInt(kDispatchKey, nodeNum);
  prefs.end();
}

void station_config_set_location(const String &text) {
  station_config().locationText = text; // ensures cfg is loaded first

  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/false);
  prefs.putString(kLocationKey, text);
  prefs.end();
}

void station_config_set_setup_completed(bool completed) {
  station_config().setupCompleted = completed; // ensures cfg is loaded first

  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/false);
  prefs.putBool(kSetupDoneKey, completed);
  prefs.end();
}
