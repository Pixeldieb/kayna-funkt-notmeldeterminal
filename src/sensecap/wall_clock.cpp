#include "wall_clock.h"
#include <Preferences.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

namespace {
bool g_is_set = false;
constexpr char kPrefsNamespace[] = "wallclock";
constexpr char kPrefsKeyEpoch[] = "epoch";

void persistEpoch(time_t epoch) {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/false);
  prefs.putULong64(kPrefsKeyEpoch, (uint64_t)epoch);
  prefs.end();
}

void setEpochInternal(time_t epoch) {
  struct timeval tv = {epoch, 0};
  settimeofday(&tv, nullptr);
  g_is_set = true;
  persistEpoch(epoch);
}
}

bool wallClockIsSet() { return g_is_set; }

void wallClockSet(int year, int month, int day, int hour, int minute, int second) {
  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_sec = second;
  time_t epoch = mktime(&t);
  setEpochInternal(epoch);

  Serial.printf("[CLOCK] gestellt auf %04d-%02d-%02d %02d:%02d:%02d\n",
                year, month, day, hour, minute, second);
}

void wallClockSetFromEpoch(uint32_t epochSeconds, const char *sourceLabel) {
  // Build time is a lower bound for "plausible now" -- this firmware
  // cannot be running before it was compiled. Guards against a zero or
  // garbage field silently moving the clock backwards/to 1970.
  static const time_t kBuildEpochFloor = []() {
    struct tm t = {};
    char mon_str[4] = {0};
    int day, year, hour, minute, second;
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    sscanf(__DATE__, "%3s %d %d", mon_str, &day, &year);
    sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second);
    int month = 1;
    for (int i = 0; i < 12; i++) {
      if (strncmp(mon_str, months[i], 3) == 0) { month = i + 1; break; }
    }
    t.tm_year = year - 1900; t.tm_mon = month - 1; t.tm_mday = day;
    t.tm_hour = hour; t.tm_min = minute; t.tm_sec = second;
    return mktime(&t);
  }();

  if ((time_t)epochSeconds < kBuildEpochFloor) {
    Serial.printf("[CLOCK] Zeit von %s ignoriert (0x%08x liegt vor Build-Zeit)\n", sourceLabel,
                  (unsigned)epochSeconds);
    return;
  }

  setEpochInternal((time_t)epochSeconds);
  struct tm t;
  time_t epoch = (time_t)epochSeconds;
  localtime_r(&epoch, &t);
  Serial.printf("[CLOCK] gestellt auf %04d-%02d-%02d %02d:%02d:%02d (Quelle: %s)\n", t.tm_year + 1900,
                t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, sourceLabel);
}

String wallClockNowHMS() {
  if (!g_is_set) return "--:--:--";
  time_t now;
  time(&now);
  struct tm t;
  localtime_r(&now, &t);
  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  return String(buf);
}

String wallClockNowYMDHMS() {
  if (!g_is_set) return "";
  time_t now;
  time(&now);
  struct tm t;
  localtime_r(&now, &t);
  char buf[20];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
           t.tm_hour, t.tm_min, t.tm_sec);
  return String(buf);
}

void wallClockSetFromBuildTime() {
  // __DATE__ = "Mmm dd yyyy" (day is space-padded, e.g. "Sep  7 2026"),
  // __TIME__ = "hh:mm:ss" — both standard, compiler-filled, in whatever
  // local time/timezone the machine running the compiler was set to.
  static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char mon_str[4] = {0};
  int day, year, hour, minute, second;
  sscanf(__DATE__, "%3s %d %d", mon_str, &day, &year);
  sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second);

  int month = 1;
  for (int i = 0; i < 12; i++) {
    if (strncmp(mon_str, months[i], 3) == 0) { month = i + 1; break; }
  }
  struct tm t = {};
  t.tm_year = year - 1900; t.tm_mon = month - 1; t.tm_mday = day;
  t.tm_hour = hour; t.tm_min = minute; t.tm_sec = second;
  time_t buildEpoch = mktime(&t);

  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/true);
  time_t persistedEpoch = (time_t)prefs.getULong64(kPrefsKeyEpoch, 0);
  prefs.end();

  if (persistedEpoch > buildEpoch) {
    // Resuming from the last known time this board had (see file comment):
    // not accurate (doesn't know how long it was powered off), but
    // guarantees the clock keeps moving forward across reboots instead of
    // jumping back to this same build stamp every time.
    setEpochInternal(persistedEpoch);
    struct tm pt;
    localtime_r(&persistedEpoch, &pt);
    Serial.printf("[CLOCK] gestellt auf %04d-%02d-%02d %02d:%02d:%02d (aus letztem bekannten Stand, "
                  "vorlaeufig — mit 'settime' korrigierbar)\n",
                  pt.tm_year + 1900, pt.tm_mon + 1, pt.tm_mday, pt.tm_hour, pt.tm_min, pt.tm_sec);
    return;
  }

  wallClockSet(year, month, day, hour, minute, second);
  Serial.println("[CLOCK] (aus Build-Zeitpunkt, vorlaeufig — mit 'settime' korrigierbar)");
}

void wallClockHandleSerialLine(const String &line) {
  if (!line.startsWith("settime ")) return;

  int year, month, day, hour, minute, second;
  int n = sscanf(line.c_str() + 8, "%d-%d-%d %d:%d:%d", &year, &month, &day,
                 &hour, &minute, &second);
  if (n != 6) {
    Serial.println("[CLOCK] Format: settime YYYY-MM-DD HH:MM:SS");
    return;
  }
  wallClockSet(year, month, day, hour, minute, second);
}
