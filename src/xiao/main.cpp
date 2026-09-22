#include <Arduino.h>
#include <Meshtastic.h>
#include "lage_db.h"
#include "mesh_security.h"

#define MT_RX_PIN 44
#define MT_TX_PIN 43
#define MT_BAUD   115200

#define LED_PIN 21

const unsigned long SEND_INTERVAL_MS = 300000; // 5 Minuten = 5 * 60 * 1000
unsigned long lastSendTime = 0;

const unsigned long HEARTBEAT_INTERVAL_MS = 500;
unsigned long lastHeartbeatToggle = 0;
bool heartbeatState = false;

// --- Zustandsmodell der Säule (Issue #10) ---
enum class SaeulenZustand {
  STANDBY,
  AKTIV,
  STROMAUSFALL,
  WARTUNG,
  SABOTAGE
};

SaeulenZustand aktuellerZustand = SaeulenZustand::STANDBY;

const char* zustandName(SaeulenZustand z) {
  switch (z) {
    case SaeulenZustand::STANDBY:      return "STANDBY";
    case SaeulenZustand::AKTIV:        return "AKTIV";
    case SaeulenZustand::STROMAUSFALL: return "STROMAUSFALL";
    case SaeulenZustand::WARTUNG:      return "WARTUNG";
    case SaeulenZustand::SABOTAGE:     return "SABOTAGE";
  }
  return "UNBEKANNT";
}

// --- Kurz-Codes für Zustandswechsel per Mesh-Nachricht ---
// Bewusst kurz und eindeutig gehalten (kein Alltagswort), um auch unter Stress
// oder auf kleiner Tastatur tippbar und nicht versehentlich auslösbar zu sein.
struct ZustandsBefehl {
  const char* code;
  SaeulenZustand zustand;
};

const ZustandsBefehl ZUSTANDS_BEFEHLE[] = {
  {"LGE",  SaeulenZustand::AKTIV},        // Lageoeffnung
  {"NOR",  SaeulenZustand::STANDBY},      // Normalbetrieb
  {"WTG",  SaeulenZustand::WARTUNG},      // Wartung
  {"SAB",  SaeulenZustand::SABOTAGE},     // Sabotage (Test-Trigger)
  {"SAUS", SaeulenZustand::STROMAUSFALL}, // Stromausfall (Test-Trigger, spaeter automatisch)
};

void ledOn()  { digitalWrite(LED_PIN, LOW); }
void ledOff() { digitalWrite(LED_PIN, HIGH); }

// Schnelles Blinken (fürs Senden und für den Erfolgs-Blitz)
void blinkFast(uint8_t times, uint16_t delayMs) {
  for (uint8_t i = 0; i < times; i++) {
    ledOn(); delay(delayMs);
    ledOff(); delay(delayMs);
  }
}

// Warte-Muster: kurzer Doppel-Blitz, dann Pause — klar unterscheidbar vom Heartbeat
void blinkWaiting() {
  ledOn(); delay(60);
  ledOff(); delay(80);
  ledOn(); delay(60);
  ledOff(); delay(600);
}

// --- Callback: wird von mt_loop() aufgerufen, wenn eine Textnachricht ankommt ---
void onTextMessage(uint32_t from, uint32_t to, uint8_t channel, const char* text) {
  String msg(text);

  // TEMPORAER: rohe Sicht auf jede eingehende Nachricht zur Fehlersuche (Issue Testkampagne)
  Serial.print("### RAW von !"); Serial.print(from, HEX);
  Serial.print(" ch="); Serial.print(channel);
  Serial.print(" len="); Serial.print(strlen(text));
  Serial.print(" msg=\""); Serial.print(msg); Serial.println("\"");

  for (const ZustandsBefehl& befehl : ZUSTANDS_BEFEHLE) {
    if (msg.equalsIgnoreCase(befehl.code)) {
      // Issue #1: ein Zustandswechsel (z.B. SABOTAGE/STROMAUSFALL) ist
      // sicherheitskritischer als eine einzelne Lagemeldung -- dieselbe
      // Allowlist-Pruefung gilt daher auch hier, nicht nur fuer LAGE:.
      if (!meshSecurityCheck(from, /*isNewReport=*/false)) return;
      Serial.print("!!! Befehl '"); Serial.print(befehl.code);
      Serial.print("' empfangen, Zustand wechselt von "); Serial.print(zustandName(aktuellerZustand));
      Serial.print(" auf "); Serial.println(zustandName(befehl.zustand));
      { // Issue #33: Zustandswechsel sind fuer die Nachbereitung relevant
        char logMsg[64];
        snprintf(logMsg, sizeof(logMsg), "%s -> %s (Befehl '%s')", zustandName(aktuellerZustand),
                 zustandName(befehl.zustand), befehl.code);
        eventLog("aktivierung", logMsg);
      }
      aktuellerZustand = befehl.zustand;
      return;
    }
  }

  if (!msg.startsWith("LAGE:")) return; // alles andere ignorieren

  String payload = msg.substring(5);
  int p1 = payload.indexOf(';');
  int p2 = payload.indexOf(';', p1 + 1);
  int p3 = payload.indexOf(';', p2 + 1);
  if (p1 < 0 || p2 < 0 || p3 < 0) {
    Serial.println("Ungueltiges LAGE-Format, erwartet: LAGE:<ID|NEU>;Kategorie;Status;Text");
    return;
  }

  String idPart = payload.substring(0, p1); idPart.trim();
  String kategorie = payload.substring(p1 + 1, p2); kategorie.trim();
  String status = payload.substring(p2 + 1, p3); status.trim();
  String content = payload.substring(p3 + 1); content.trim();

  bool istNeu = idPart.equalsIgnoreCase("NEU");
  if (!meshSecurityCheck(from, istNeu)) return; // Issues #1 (Allowlist) / #3 (Ratenlimit)

  char fromBuf[12];
  snprintf(fromBuf, sizeof(fromBuf), "!%08x", from);
  String fromNode(fromBuf);

  if (istNeu) {
    int newId = lageDbCreate(kategorie, status, content, fromNode);
    Serial.print(">>> Neue Lagemeldung angelegt, ID "); Serial.println(newId);
  } else {
    int id = idPart.toInt();
    if (lageDbUpdate(id, kategorie, status, content, fromNode)) {
      Serial.print(">>> Lagemeldung "); Serial.print(id); Serial.println(" aktualisiert");
    } else {
      Serial.print(">>> Update fehlgeschlagen, ID "); Serial.print(id); Serial.println(" nicht gefunden");
    }
  }
  blinkFast(3, 100);
}

// --- Wird aufgerufen, wenn der Config-Handshake mit der Node abgeschlossen ist ---
// mt_serial_loop() liefert in der Bibliothek IMMER true zurueck ("It's easy being
// a serial interface") - der urspruengliche while(!connected)-Loop lief daher sofort
// durch, ohne dass der echte want_config-Handshake je angestossen wurde. Dadurch hat
// die Node eingehende Mesh-Nachrichten nie an den ESP32 weitergeleitet.
bool configDone = false;

void onNodeReport(mt_node_t* node, mt_nr_progress_t progress) {
  if (progress == MT_NR_DONE || progress == MT_NR_INVALID) {
    configDone = true;
  }
}

// --- Serial-CLI zum Prüfen der Datenbank ---
void handleSerialCommand(const String& cmd) {
  if (cmd == "liste") {
    lageDbListSummary();
  } else if (cmd.startsWith("liste kategorie ")) {
    lageDbListSummary(cmd.substring(16), "");
  } else if (cmd.startsWith("liste status ")) {
    lageDbListSummary("", cmd.substring(13));
  } else if (cmd.startsWith("detail ")) {
    lageDbShowDetail(cmd.substring(7).toInt());
  } else if (cmd == "status") {
    Serial.print("Zustand: "); Serial.println(zustandName(aktuellerZustand));
  } else if (cmd.startsWith("allow add ")) {
    uint32_t nodeNum = strtoul(cmd.substring(10).c_str(), nullptr, 16);
    if (meshSecurityAllow(nodeNum)) {
      Serial.printf("[SECURITY] !%08x freigeschaltet.\n", (unsigned)nodeNum);
    }
  } else if (cmd.startsWith("allow revoke ")) {
    uint32_t nodeNum = strtoul(cmd.substring(13).c_str(), nullptr, 16);
    Serial.printf("[SECURITY] !%08x %s.\n", (unsigned)nodeNum,
                  meshSecurityRevoke(nodeNum) ? "entfernt" : "war nicht auf der Allowlist");
  } else if (cmd == "allow list") {
    meshSecurityListAllowed();
  } else if (cmd == "events") {
    // Issue #33: lokale Betriebshistorie, unabhaengig von der Leitstelle einsehbar.
    EventLogEntry events[20];
    int count = eventLogGetRecent(events, 20);
    Serial.printf("--- Ereignisprotokoll (%d) ---\n", count);
    for (int i = 0; i < count; i++) {
      Serial.printf("#%d [%lu] %s: %s\n", events[i].id, events[i].zeit, events[i].kategorie.c_str(),
                    events[i].text.c_str());
    }
  } else if (cmd == "help") {
    Serial.println("Befehle: liste | liste kategorie <X> | liste status <X> | detail <ID> | status | "
                    "allow add|revoke <hex-node-id> | allow list | events | help");
  } else if (cmd.length() > 0) {
    Serial.println("Unbekannter Befehl. 'help' fuer Uebersicht.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  pinMode(LED_PIN, OUTPUT);
  ledOff();

  lageDbBegin();
  meshSecurityInit(); // Issues #1/#3: sicherer Default, leere Allowlist verwirft alles

  Serial.println("Starte Meshtastic-Verbindung...");
  mt_serial_init(MT_RX_PIN, MT_TX_PIN, MT_BAUD);
  mt_request_node_report(onNodeReport); // stoesst den echten want_config-Handshake an

  // --- Handshake: warten, bis Config-Austausch abgeschlossen ist ---
  // LED blinkt im "Doppel-Blitz"-Muster, solange nicht verbunden.
  // Timeout-Fallback: falls die Node den Handshake nie mit "fertig" beantwortet
  // (z.B. Versions-Mismatch), nach 15s trotzdem weitermachen, statt fuer immer
  // haengen zu bleiben - sonst ist nicht mal die lokale Serial-CLI erreichbar.
  unsigned long handshakeStart = millis();
  const unsigned long HANDSHAKE_TIMEOUT_MS = 15000;
  while (!configDone) {
    mt_loop(millis());
    blinkWaiting();
    if (millis() - handshakeStart > HANDSHAKE_TIMEOUT_MS) {
      Serial.println(">>> WARNUNG: Config-Handshake nach 15s nicht abgeschlossen, mache trotzdem weiter.");
      eventLog("fehler", "Config-Handshake nach 15s nicht abgeschlossen");
      break;
    }
  }

  set_text_message_callback(onTextMessage); // NACH erfolgtem Handshake registrieren

  Serial.println(">>> VERBUNDEN mit der Node. Tippe 'help' fuer CLI-Befehle.");
  eventLog("system", "Boot: verbunden mit Meshtastic-Node");

  // --- Erfolg: 5x schnell blinken ---
  blinkFast(5, 100);

  lastHeartbeatToggle = millis();
}

void loop() {
  bool connected = mt_loop(millis()); // ruft bei Bedarf onTextMessage() auf

  // Verbindung mittendrin verloren -> zurück ins Warte-Muster, bis sie wieder da ist
  if (!connected) {
    blinkWaiting();
    return;
  }

  // Periodischer Testversand deaktiviert (hat die Handshake-Fehlersuche gestoert)

  if (millis() - lastHeartbeatToggle >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatToggle = millis();
    heartbeatState = !heartbeatState;
    heartbeatState ? ledOn() : ledOff();
  }

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    handleSerialCommand(cmd);
  }
}