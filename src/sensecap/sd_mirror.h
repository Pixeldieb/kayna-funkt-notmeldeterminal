#pragma once
#include <Arduino.h>

// Sends a line to the RP2040 co-processor to be appended to a file on its
// Micro-SD card (see src/sensecap_rp2040/main.cpp) -- a write-only backup
// mirror, not the authoritative store (that's still SPIFFS/SQLite, see
// lage_db.h). Wired into lage_db's generic mirror hook (see
// lageDbSetMirrorHook) in main.cpp's setup(), not called directly from
// application code elsewhere.
//
// Best-effort by design: nothing here waits for or checks a reply, and a
// disconnected/dead RP2040 link never blocks or fails the real write in
// lage_db.cpp. This is a convenience backup, not a feature anything else
// depends on being reliable.
void sdMirrorBegin();

// Matches LageDbMirrorFn's signature (see lage_db.h) so it can be passed
// straight to lageDbSetMirrorHook().
void sdMirrorSend(const char *table, const String &line);

// Diagnostic only (serial "sd status" command, see main.cpp): sends a PING
// and waits up to timeoutMs for the RP2040's PONG reply. Returns true if a
// reply arrived at all (proves the UART link + RP2040 firmware are alive);
// *outSdOk reports whether the RP2040 said its SD card mounted correctly.
// Everything else in this file stays fire-and-forget on purpose -- this is
// the one exception, used only for a human to check the link, never on
// the hot path of an actual Lagemeldung/event write.
bool sdMirrorPing(uint32_t timeoutMs, bool *outSdOk);
