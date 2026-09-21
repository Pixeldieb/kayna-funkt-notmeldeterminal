#pragma once
#include <Arduino.h>

// Real wall-clock time, settable over Serial (no RTC battery or NTP/WiFi
// on this board yet). Previously the status bar showed millis()-based
// uptime instead, which looked like a real clock but wasn't one — this
// replaces that.
//
// Testprotokoll 2026-09-18 (A1/A4/F2) found the old build-time-only
// approach did two things wrong: it was off by however long the board sat
// unflashed before this boot (not "normally well under a minute" as
// originally assumed), and worse, it jumps back to the SAME stale value on
// every reboot, which corrupted the local event log's chronological order
// across restarts. Fixed with two real time sources, in priority order:
//  1. A real Meshtastic Position packet's `time`/`timestamp` field, if any
//     node on the mesh ever sends one (see meshtastic_proto.cpp) — a phone
//     or GPS-synced node broadcasts this specifically so devices without
//     their own clock can pick it up; this is genuine wall-clock time, not
//     a guess.
//  2. The last wall-clock value this board ever knew, persisted to NVS
//     (Preferences) on every successful set and reloaded at boot — not
//     accurate (doesn't account for time elapsed while powered off), but
//     guarantees the clock only ever moves forward across reboots, which
//     is what the event log's ordering actually depends on.
// Falls back to build time only if neither of those is available yet.
// `settime` always overrides whatever any of this set.

bool wallClockIsSet();

// Sets the clock. Call once, e.g. from a serial command. Persists to NVS.
void wallClockSet(int year, int month, int day, int hour, int minute, int second);

// Sets the clock from a Unix epoch (seconds since 1970), e.g. from a real
// Meshtastic Position packet's `time`/`timestamp` field. Ignored if
// `epochSeconds` is implausibly small (before this firmware was built) —
// a zero/uninitialized field must never move the clock backwards. Persists
// to NVS like wallClockSet().
void wallClockSetFromEpoch(uint32_t epochSeconds, const char *sourceLabel);

// "HH:MM:SS", or "--:--:--" if wallClockIsSet() is false.
String wallClockNowHMS();

// "YYYY-MM-DD HH:MM:SS" (same format wallClockSet()/wallClockHandleSerialLine()
// accept back), or "" if wallClockIsSet() is false. For prefilling a UI field
// with the board's current idea of the time before the user corrects it.
String wallClockNowYMDHMS();

// Resolves the boot-time clock: prefers the last value persisted to NVS
// (from a previous boot) if it's newer than this firmware's own build
// time, otherwise falls back to __DATE__/__TIME__ (the build machine's
// local time when it was compiled) — call once at boot, before any mesh
// packets have had a chance to arrive. See file comment above.
void wallClockSetFromBuildTime();

// Feed one line read from Serial. Recognizes:
//   settime YYYY-MM-DD HH:MM:SS
// Anything else is ignored (so main.cpp can just forward every line here
// without needing its own command parser for this).
void wallClockHandleSerialLine(const String &line);
