#include "mesh_security.h"
#include <Preferences.h>
#include <limits.h>
#include "lage_db.h"

namespace {

constexpr int MAX_ALLOWED = 8;
constexpr int MAX_TRACKED_SENDERS = 8;
// Issue #3's own suggestion: "minimaler Zeitabstand zwischen zwei
// akzeptierten NEU-Meldungen derselben Node". 60s is generous enough that a
// legitimate operator resubmitting a correction isn't blocked for long, but
// stops a single flooding sender from writing more than one row a minute.
constexpr unsigned long MIN_NEU_INTERVAL_MS = 60000;

const char *kPrefsNamespace = "meshsec";

uint32_t g_allowed[MAX_ALLOWED] = {0};
bool g_loaded = false;

struct SenderRateState {
  uint32_t nodeNum = 0;
  unsigned long lastNeuAcceptedAt = 0;
};
SenderRateState g_rateState[MAX_TRACKED_SENDERS];

void load() {
  if (g_loaded) return;
  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/true);
  int count = 0;
  for (int i = 0; i < MAX_ALLOWED; i++) {
    char key[8];
    snprintf(key, sizeof(key), "allow%d", i);
    g_allowed[i] = prefs.getUInt(key, 0);
    if (g_allowed[i] != 0) count++;
  }
  prefs.end();
  g_loaded = true;

  if (count == 0) {
    Serial.println("[SECURITY] Keine Absender-Allowlist konfiguriert -- ALLE eingehenden "
                    "Lagemeldungen werden verworfen, bis mindestens ein Absender freigeschaltet ist. "
                    "'allow add <hex-node-id>' zum Freischalten (siehe Issue #1).");
  } else {
    Serial.printf("[SECURITY] Absender-Allowlist geladen: %d Node(s) freigeschaltet.\n", count);
  }
}

void persist() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/false);
  for (int i = 0; i < MAX_ALLOWED; i++) {
    char key[8];
    snprintf(key, sizeof(key), "allow%d", i);
    prefs.putUInt(key, g_allowed[i]);
  }
  prefs.end();
}

bool isAllowed(uint32_t nodeNum) {
  for (int i = 0; i < MAX_ALLOWED; i++) {
    if (g_allowed[i] == nodeNum) return true;
  }
  return false;
}

// Finds (or evicts the least-recently-used slot for) this sender's rate
// tracking entry. Not persisted -- restarting the device resetting rate
// tracking is an acceptable trade-off for a small embedded table, the
// allowlist (the actual security boundary) is what's persisted.
SenderRateState &rateSlotFor(uint32_t nodeNum) {
  int oldestIdx = 0;
  unsigned long oldestAt = ULONG_MAX;
  for (int i = 0; i < MAX_TRACKED_SENDERS; i++) {
    if (g_rateState[i].nodeNum == nodeNum) return g_rateState[i];
    if (g_rateState[i].lastNeuAcceptedAt < oldestAt) {
      oldestAt = g_rateState[i].lastNeuAcceptedAt;
      oldestIdx = i;
    }
  }
  g_rateState[oldestIdx].nodeNum = nodeNum;
  g_rateState[oldestIdx].lastNeuAcceptedAt = 0;
  return g_rateState[oldestIdx];
}

} // namespace

void meshSecurityInit() { load(); }

bool meshSecurityCheck(uint32_t fromNode, bool isNewReport) {
  load(); // safe to call repeatedly, no-ops after the first real load

  if (!isAllowed(fromNode)) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Absender !%08x nicht auf der Allowlist", (unsigned)fromNode);
    Serial.printf("[SECURITY] Lagemeldung abgelehnt: %s.\n", msg);
    eventLog("sicherheit", msg);
    return false;
  }

  if (isNewReport) {
    SenderRateState &state = rateSlotFor(fromNode);
    unsigned long now = millis();
    if (state.lastNeuAcceptedAt != 0 && (now - state.lastNeuAcceptedAt) < MIN_NEU_INTERVAL_MS) {
      char msg[80];
      snprintf(msg, sizeof(msg), "Ratenlimit fuer !%08x (letzte vor %lums)", (unsigned)fromNode,
               now - state.lastNeuAcceptedAt);
      Serial.printf("[SECURITY] Neue Lagemeldung abgelehnt: %s.\n", msg);
      eventLog("sicherheit", msg);
      return false;
    }
    state.lastNeuAcceptedAt = now;
  }

  return true;
}

bool meshSecurityAllow(uint32_t nodeNum) {
  load();
  if (isAllowed(nodeNum)) return true;
  for (int i = 0; i < MAX_ALLOWED; i++) {
    if (g_allowed[i] == 0) {
      g_allowed[i] = nodeNum;
      persist();
      char msg[48];
      snprintf(msg, sizeof(msg), "!%08x zur Allowlist hinzugefuegt", (unsigned)nodeNum);
      eventLog("system", msg);
      return true;
    }
  }
  Serial.println("[SECURITY] Allowlist voll (max. 8 Nodes) -- 'allow revoke' fuer einen bestehenden Eintrag.");
  return false;
}

bool meshSecurityRevoke(uint32_t nodeNum) {
  load();
  for (int i = 0; i < MAX_ALLOWED; i++) {
    if (g_allowed[i] == nodeNum) {
      g_allowed[i] = 0;
      persist();
      char msg[48];
      snprintf(msg, sizeof(msg), "!%08x von der Allowlist entfernt", (unsigned)nodeNum);
      eventLog("system", msg);
      return true;
    }
  }
  return false;
}

void meshSecurityListAllowed() {
  load();
  Serial.println("[SECURITY] Allowlist:");
  bool any = false;
  for (int i = 0; i < MAX_ALLOWED; i++) {
    if (g_allowed[i] != 0) {
      Serial.printf("  !%08x\n", (unsigned)g_allowed[i]);
      any = true;
    }
  }
  if (!any) Serial.println("  (leer)");
}
