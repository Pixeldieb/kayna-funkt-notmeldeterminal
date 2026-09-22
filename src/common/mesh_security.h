#pragma once
#include <Arduino.h>

// Sender-Allowlist + Ratenbegrenzung fuer eingehende Lagemeldungen (Issues #1
// und #3). Beide Boards (xiao, sensecap) verarbeiten Mesh-Nachrichten mit
// LAGE:-Praefix bisher von JEDEM Absender ungeprueft -- jeder Teilnehmer im
// selben Kanal kann bestehende Meldungen ueberschreiben, beliebig neue
// anlegen, oder (auf dem xiao-Board) sogar den Betriebszustand der Saeule
// per Kurz-Code umschalten. In einem Notfall-/Katastrophenszenario ist eine
// manipulierte oder ueberflutete Lagemeldung kein theoretisches Risiko.
//
// Sicherer Default: eine LEERE Allowlist bedeutet "alle eingehenden
// Lagemeldungen ablehnen", nicht "alle erlauben" -- ein neu geflashtes oder
// frisch zurueckgesetztes Geraet ist damit sicher, auch wenn niemand die
// Allowlist explizit befuellt hat. Klar geloggt (Serial), nicht stillschweigend.

void meshSecurityInit(); // einmal in setup() aufrufen (laedt Allowlist aus NVS)

// true = Absender ist freigeschaltet UND (falls isNewReport) das
// Ratenlimit fuer diesen Absender wurde nicht verletzt. Loggt intern schon
// den genauen Ablehnungsgrund (nicht freigeschaltet / Ratenlimit), damit
// Manipulationsversuche auf Serial sichtbar sind statt nur stillschweigend
// verworfen zu werden.
bool meshSecurityCheck(uint32_t fromNode, bool isNewReport);

// Persistente Verwaltung der Allowlist (max. 8 Einträge, siehe .cpp).
bool meshSecurityAllow(uint32_t nodeNum);
bool meshSecurityRevoke(uint32_t nodeNum);
void meshSecurityListAllowed();
