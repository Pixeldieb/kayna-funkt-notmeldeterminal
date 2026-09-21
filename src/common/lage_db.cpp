#include "lage_db.h"
#include <SPIFFS.h>
#include "sqlite3.h"

// ---------------------------------------------------------------------
// PLATTFORM-EINSCHRAENKUNG (Sqlite3Esp32 + SPIFFS), gefunden live
// 2026-09-18 -- beim Schreiben neuer Queries gegen `lagemeldungen` oder
// `ereignisse` unbedingt beachten:
//
// Jede Query, die SQLite zwingt, das Ergebnis VOLLSTAENDIG zu
// materialisieren/sortieren, schlaegt auf diesem SPIFFS-Setup zuverlaessig
// mit "disk I/O error" (SQLITE_IOERR) fehl, sobald die Tabelle ein paar
// Dutzend Zeilen hat -- reproduzierbar mit exakt denselben Zeilen, die
// eine andere Query-Form klaglos liest. Betroffen: (a) jede Query mit
// WHERE-Klausel (selbst triviales "WHERE 1=1"), UND (b) "ORDER BY ..."
// ganz ohne LIMIT. NICHT betroffen: "ORDER BY x DESC LIMIT n" ohne
// WHERE-Klausel -- SQLite kann das offenbar in einem einzigen Streaming-
// Durchlauf mit beschraenktem Speicher beantworten (kein vollstaendiges
// Sortieren noetig), und nur dieser Pfad hat sich als zuverlaessig
// erwiesen. Siehe lageDbGetRecentSummaries() (funktioniert: kein WHERE,
// hat LIMIT) vs. lageDbListSummary() vor diesem Fix (kaputt: erst WHERE,
// dann ohne WHERE aber mit ungebremstem ORDER BY -- beides schlug fehl,
// erst "kein ORDER BY, Filterung in der Callback-Funktion" hat funktioniert).
//
// Praktische Konsequenz: Filtern (nach Kategorie, Status, Absender, ...)
// gehoert in C++ nach einem einfachen "ORDER BY updated_at DESC LIMIT n"-
// Fetch (grosszuegiges n, dann in C++ weiter einschraenken), NIEMALS in
// eine SQL-WHERE-Klausel gegen diese Tabellen. Passt zum bereits
// dokumentierten Grund, warum `lagemeldungen` keinen PRIMARY KEY/UNIQUE
// hat (siehe lageDbBegin() unten) -- derselbe Bug-Bereich der Bibliothek.
// ---------------------------------------------------------------------

static sqlite3* db = nullptr;
static LageDbTimeFn g_timeFn = nullptr;
static LageDbMirrorFn g_mirrorFn = nullptr;

void lageDbSetTimeProvider(LageDbTimeFn fn) { g_timeFn = fn; }
void lageDbSetMirrorHook(LageDbMirrorFn fn) { g_mirrorFn = fn; }

static unsigned long lageDbNow() { return g_timeFn ? g_timeFn() : millis(); }

static bool execSimple(const char* sql) {
  char* errMsg = nullptr;
  int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
  if (rc != SQLITE_OK) {
    Serial.print("SQL-Fehler: ");
    Serial.println(errMsg);
    sqlite3_free(errMsg);
    return false;
  }
  return true;
}

bool lageDbBegin() {
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount fehlgeschlagen!");
    return false;
  }

  int rc = sqlite3_open("/spiffs/lage.db", &db);
  if (rc != SQLITE_OK) {
    Serial.println("DB konnte nicht geoeffnet werden!");
    return false;
  }

  // WICHTIG: kein PRIMARY KEY / UNIQUE hier -- loest auf SPIFFS zuverlaessig
  // "disk I/O error" aus (bekannter Bug der Sqlite3Esp32-Bibliothek, siehe
  // https://github.com/siara-cc/esp32_arduino_sqlite3_lib/issues/18 ).
  // Stattdessen nutzen wir SQLites eingebaute rowid als ID.
  execSimple(
    "CREATE TABLE IF NOT EXISTS lagemeldungen ("
    " kategorie TEXT,"
    " status TEXT,"
    " text TEXT,"
    " from_node TEXT,"
    " created_at INTEGER,"
    " updated_at INTEGER);"
  );
  execSimple(
    "CREATE TABLE IF NOT EXISTS lage_historie ("
    " lagemeldung_id INTEGER,"
    " changed_at INTEGER,"
    " alter_text TEXT,"
    " neuer_text TEXT,"
    " alter_status TEXT,"
    " neuer_status TEXT,"
    " from_node TEXT);"
  );
  // Issue #33: separate von lagemeldungen -- das hier ist die Betriebs-
  // historie der Saeule selbst (Aktivierungen, Sicherheits-Ablehnungen,
  // Fehler, ...), nicht der Inhalt der Lagemeldungen.
  execSimple(
    "CREATE TABLE IF NOT EXISTS ereignisse ("
    " zeit INTEGER,"
    " kategorie TEXT,"
    " text TEXT);"
  );
  return true;
}

int lageDbCreate(const String& kategorie, const String& status, const String& text, const String& fromNode) {
  sqlite3_stmt* stmt;
  const char* sql = "INSERT INTO lagemeldungen (kategorie, status, text, from_node, created_at, updated_at) "
                     "VALUES (?, ?, ?, ?, ?, ?);";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;

  unsigned long now = lageDbNow();
  sqlite3_bind_text(stmt, 1, kategorie.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, status.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, text.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, fromNode.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 5, now);
  sqlite3_bind_int64(stmt, 6, now);

  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) return -1;

  int newId = (int)sqlite3_last_insert_rowid(db); // funktioniert auch ohne PRIMARY KEY
  if (g_mirrorFn) {
    String line = String(newId) + ";" + kategorie + ";" + status + ";" + fromNode + ";" + String(now) + ";" + text;
    g_mirrorFn("lagemeldungen", line);
  }
  return newId;
}

bool lageDbUpdate(int id, const String& kategorie, const String& status, const String& text, const String& fromNode) {
  sqlite3_stmt* sel;
  const char* selSql = "SELECT text, status FROM lagemeldungen WHERE rowid = ?;";
  if (sqlite3_prepare_v2(db, selSql, -1, &sel, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int(sel, 1, id);

  if (sqlite3_step(sel) != SQLITE_ROW) {
    sqlite3_finalize(sel);
    return false; // ID existiert nicht
  }
  String alterText = String((const char*)sqlite3_column_text(sel, 0));
  String alterStatus = String((const char*)sqlite3_column_text(sel, 1));
  sqlite3_finalize(sel);

  unsigned long now = lageDbNow();

  sqlite3_stmt* hist;
  const char* histSql = "INSERT INTO lage_historie "
    "(lagemeldung_id, changed_at, alter_text, neuer_text, alter_status, neuer_status, from_node) "
    "VALUES (?, ?, ?, ?, ?, ?, ?);";
  sqlite3_prepare_v2(db, histSql, -1, &hist, nullptr);
  sqlite3_bind_int(hist, 1, id);
  sqlite3_bind_int64(hist, 2, now);
  sqlite3_bind_text(hist, 3, alterText.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(hist, 4, text.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(hist, 5, alterStatus.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(hist, 6, status.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(hist, 7, fromNode.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(hist);
  sqlite3_finalize(hist);

  sqlite3_stmt* upd;
  const char* updSql = "UPDATE lagemeldungen SET kategorie=?, status=?, text=?, from_node=?, updated_at=? WHERE rowid=?;";
  sqlite3_prepare_v2(db, updSql, -1, &upd, nullptr);
  sqlite3_bind_text(upd, 1, kategorie.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(upd, 2, status.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(upd, 3, text.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(upd, 4, fromNode.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(upd, 5, now);
  sqlite3_bind_int(upd, 6, id);
  int rc = sqlite3_step(upd);
  sqlite3_finalize(upd);

  bool ok = rc == SQLITE_DONE;
  if (ok && g_mirrorFn) {
    String line = String(id) + ";" + kategorie + ";" + status + ";" + fromNode + ";" + String(now) + ";" + text;
    g_mirrorFn("lagemeldungen", line);
  }
  return ok;
}

struct ListFilter {
  const String* kategorie;
  const String* status;
};

// Column order fixed to match the WHERE-less SELECT below: id, kategorie,
// status, updated_at, from_node, text.
static int printRowCallback(void* data, int argc, char** argv, char** colNames) {
  ListFilter* filter = (ListFilter*)data;
  if (filter) {
    if (filter->kategorie->length() > 0 && (argc < 2 || !argv[1] || *filter->kategorie != argv[1])) return 0;
    if (filter->status->length() > 0 && (argc < 3 || !argv[2] || *filter->status != argv[2])) return 0;
  }
  for (int i = 0; i < argc; i++) {
    Serial.print(colNames[i]);
    Serial.print("=");
    Serial.print(argv[i] ? argv[i] : "NULL");
    Serial.print("  ");
  }
  Serial.println();
  return 0;
}

void lageDbListSummary(const String& filterKategorie, const String& filterStatus) {
  // No WHERE and no ORDER BY -- turns out that wasn't enough on its own
  // (still hit "disk I/O error" with WHERE removed but ORDER BY kept, once
  // the table had ~40 rows). SQLite can answer "ORDER BY x LIMIT n" with a
  // single streaming pass (bounded memory, no full sort), but a plain
  // "ORDER BY x" with no LIMIT has to fully materialize/sort the result --
  // that full-materialization path is what actually breaks here, not the
  // WHERE clause itself (see lageDbGetRecentSummaries()'s working query,
  // which also has no WHERE but does have LIMIT). This is a debug/inspect
  // command, not the real UI (that goes through lageDbGetRecentSummaries),
  // so natural rowid order (i.e. no sort at all -- free) is an acceptable
  // trade for actually working; filtering (kategorie/status) still happens
  // in the callback.
  const char* sql = "SELECT rowid AS id, kategorie, status, updated_at, from_node, text FROM lagemeldungen;";

  Serial.println("--- Uebersicht Lagemeldungen (rowid-Reihenfolge) ---");
  ListFilter filter{&filterKategorie, &filterStatus};
  char* errMsg = nullptr;
  sqlite3_exec(db, sql, printRowCallback, &filter, &errMsg);
  if (errMsg) { Serial.println(errMsg); sqlite3_free(errMsg); }
}

int lageDbGetRecentSummaries(LageMeldungSummary* out, int maxCount) {
  // No WHERE clause here on purpose -- found live (2026-09-18) that adding
  // one ("... WHERE from_node NOT LIKE ... ORDER BY ... LIMIT ?") made
  // sqlite3_step() fail partway through with SQLITE_IOERR ("disk I/O
  // error") on THIS exact table/platform, even though the identical query
  // without the WHERE clause reads the very same rows successfully. Looks
  // like another instance of the SPIFFS/Sqlite3Esp32 fragility already
  // documented above (lageDbBegin()'s comment on why there's no PRIMARY
  // KEY/UNIQUE either) -- exact trigger not fully understood, but
  // reproducible: filtering belongs in C++ after a plain fetch, not in
  // SQL, on this platform. See build_info_list_page() in ui_model.cpp for
  // where the "only genuinely received" filtering actually happens now.
  const char* sql = "SELECT rowid, kategorie, status, text, updated_at, from_node "
                     "FROM lagemeldungen ORDER BY updated_at DESC LIMIT ?;";
  sqlite3_stmt* stmt;
  int prc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
  if (prc != SQLITE_OK) {
    Serial.printf("[DB] lageDbGetRecentSummaries: prepare fehlgeschlagen (rc=%d): %s\n", prc, sqlite3_errmsg(db));
    return 0;
  }
  sqlite3_bind_int(stmt, 1, maxCount);

  int count = 0;
  int src;
  while (count < maxCount && (src = sqlite3_step(stmt)) == SQLITE_ROW) {
    out[count].id = sqlite3_column_int(stmt, 0);
    out[count].kategorie = String((const char*)sqlite3_column_text(stmt, 1));
    out[count].status = String((const char*)sqlite3_column_text(stmt, 2));
    out[count].text = String((const char*)sqlite3_column_text(stmt, 3));
    out[count].updatedAt = (unsigned long)sqlite3_column_int64(stmt, 4);
    out[count].fromNode = String((const char*)sqlite3_column_text(stmt, 5));
    count++;
  }
  if (count == 0 && src != SQLITE_DONE) {
    Serial.printf("[DB] lageDbGetRecentSummaries: step beendet mit rc=%d: %s\n", src, sqlite3_errmsg(db));
  }
  sqlite3_finalize(stmt);
  return count;
}

void eventLog(const String& kategorie, const String& text) {
  Serial.printf("[EVENT] %s: %s\n", kategorie.c_str(), text.c_str());
  unsigned long now = lageDbNow();
  if (g_mirrorFn) {
    String line = String(now) + ";" + kategorie + ";" + text;
    g_mirrorFn("ereignisse", line);
  }
  if (!db) return; // z.B. lageDbBegin() fehlgeschlagen -- nicht crashen, nur nicht persistieren
  sqlite3_stmt* stmt;
  const char* sql = "INSERT INTO ereignisse (zeit, kategorie, text) VALUES (?, ?, ?);";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
  sqlite3_bind_int64(stmt, 1, now);
  sqlite3_bind_text(stmt, 2, kategorie.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, text.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

int eventLogGetRecent(EventLogEntry* out, int maxCount) {
  // ORDER BY zeit alone isn't stable: the board's clock is build-time-derived
  // (see wall_clock.cpp) and restarts from roughly the same value on every
  // boot, so events from different actual boot cycles can share a "zeit" --
  // found live 2026-09-18 (Testprotokoll F2) mixing multiple boots' entries
  // into an unpredictable order. rowid always increases with insertion order
  // regardless of "zeit", so it's the real tie-breaker for a trustworthy history.
  const char* sql = "SELECT rowid, zeit, kategorie, text FROM ereignisse ORDER BY zeit DESC, rowid DESC LIMIT ?;";
  sqlite3_stmt* stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_int(stmt, 1, maxCount);

  int count = 0;
  while (count < maxCount && sqlite3_step(stmt) == SQLITE_ROW) {
    out[count].id = sqlite3_column_int(stmt, 0);
    out[count].zeit = (unsigned long)sqlite3_column_int64(stmt, 1);
    out[count].kategorie = String((const char*)sqlite3_column_text(stmt, 2));
    out[count].text = String((const char*)sqlite3_column_text(stmt, 3));
    count++;
  }
  sqlite3_finalize(stmt);
  return count;
}

void lageDbShowDetail(int id) {
  Serial.print("--- Detail Lagemeldung ");
  Serial.print(id);
  Serial.println(" ---");

  String sql = "SELECT rowid AS id, kategorie, status, text, from_node, created_at, updated_at "
               "FROM lagemeldungen WHERE rowid = " + String(id) + ";";
  char* errMsg = nullptr;
  sqlite3_exec(db, sql.c_str(), printRowCallback, nullptr, &errMsg);
  if (errMsg) { Serial.println(errMsg); sqlite3_free(errMsg); }

  Serial.println("--- Historie ---");
  String histSql = "SELECT changed_at, alter_status, neuer_status, neuer_text, from_node "
                    "FROM lage_historie WHERE lagemeldung_id = " + String(id) + " ORDER BY changed_at ASC;";
  sqlite3_exec(db, histSql.c_str(), printRowCallback, nullptr, &errMsg);
  if (errMsg) { Serial.println(errMsg); sqlite3_free(errMsg); }
}