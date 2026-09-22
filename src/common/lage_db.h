#pragma once
#include <Arduino.h>

// created_at/updated_at default to millis() (uptime) -- fine for relative
// ordering, but not a real date/time, and this file is shared across
// boards (some have no real clock at all, e.g. src/xiao). A board with
// access to real wall-clock time (see e.g. sensecap/wall_clock.h) can
// install a provider returning real epoch seconds instead, so timestamps
// read back out are actually meaningful. Optional; defaults to millis().
typedef unsigned long (*LageDbTimeFn)();
void lageDbSetTimeProvider(LageDbTimeFn fn);

// Optional, same pattern: a board with somewhere else to keep a backup
// copy (see sensecap/sd_mirror.h -- the RP2040's SD card, reachable only
// from the SenseCAP board, not XIAO) can install a hook called after every
// successful write to lagemeldungen or ereignisse. write-only, best-effort:
// never blocks or fails the real write if unset or if the mirror target is
// unavailable -- SPIFFS/SQLite stays the authoritative store either way.
// table is "lagemeldungen" or "ereignisse"; line is a single already-
// formatted text line (semicolon-separated, no embedded newline).
typedef void (*LageDbMirrorFn)(const char* table, const String& line);
void lageDbSetMirrorHook(LageDbMirrorFn fn);

bool lageDbBegin();
int  lageDbCreate(const String& kategorie, const String& status, const String& text, const String& fromNode);
bool lageDbUpdate(int id, const String& kategorie, const String& status, const String& text, const String& fromNode);
void lageDbListSummary(const String& filterKategorie = "", const String& filterStatus = "");
void lageDbShowDetail(int id);

// Structured read access for UIs (displays, LVGL, ...) that can't just
// print to Serial like the CLI functions above.
struct LageMeldungSummary {
  int id;
  String kategorie;
  String status;
  String text;
  unsigned long updatedAt;
  // "!<hex-node-id>" for a report actually received over the mesh,
  // "!lokal-touch" for one this station triggered itself (see
  // do_trigger_emergency() in ui_model.cpp) -- the two must never be shown
  // interchangeably as "empfangene offizielle Meldungen" (Testprotokoll C1,
  // 2026-09-18).
  String fromNode;
};

// Fills `out` (capacity `maxCount`) with the most recently updated
// Lagemeldungen, newest first (no filtering by source -- see the .cpp for
// why a WHERE-based filter was tried and reverted: it made sqlite3_step()
// fail with a disk I/O error on this platform). Returns how many were
// written. Callers that only want externally-received reports (not this
// station's own "!lokal-touch" entries) must fetch a generously large
// maxCount and filter in C++ -- see build_info_list_page() in
// src/sensecap/ui_model.cpp.
int lageDbGetRecentSummaries(LageMeldungSummary* out, int maxCount);

// Issue #33: local, append-only log of operationally relevant events
// (activations, sabotage alarms, errors, security rejections, ...) for
// post-incident review, independent of whatever the Leitstelle did or did
// not receive over the mesh. Separate table from lagemeldungen -- this is
// about the STATION's own operational history, not the content of reports.
// "kategorie" is a short free-form tag, not an enum, so callers can log new
// kinds of events without a header change (e.g. "sicherheit", "system",
// "sabotage", "aktivierung", "fehler") -- see EVENT_LOG_CLI section of the
// respective board's README for the tags actually in use.
void eventLog(const String& kategorie, const String& text);

struct EventLogEntry {
  int id;
  unsigned long zeit;
  String kategorie;
  String text;
};

// Fills `out` (capacity `maxCount`) with the most recent events, newest
// first. Returns how many were written.
int eventLogGetRecent(EventLogEntry* out, int maxCount);