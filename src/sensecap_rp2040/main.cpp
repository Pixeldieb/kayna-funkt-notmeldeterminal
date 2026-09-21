// SenseCAP Indicator's RP2040 sensor co-processor -- replaces Seeed's stock
// sensor-telemetry firmware (never read by our ESP32-S3 code, see
// platformio.ini's sensecap_rp2040 env comment for why that's safe to do).
// Job: mount the Micro-SD card (the only place it's reachable from -- see
// that same comment for the hardware reason) and append whatever lines the
// ESP32-S3 sends over the COBS-framed UART link to two log files. This is a
// write-only mirror: SPIFFS/SQLite on the ESP32-S3 stays authoritative,
// this SD card is a backup, not a replacement store (yet).
//
// Flashing this (unlike the ESP32 boards in this repo): hold BOOTSEL while
// plugging in USB, the RP2040 enumerates as a USB mass-storage drive, then
// `pio run -e sensecap_rp2040 -t upload` (or drag the built .uf2 onto that
// drive by hand).
#include <Arduino.h>
#include <SDFS.h>
#include <SPI.h>

#include "cobs.h"

// Seeed's own "MicroSD" RP2040 tutorial uses the classic Arduino "SD"
// library (SD.begin(cs, clock, SPI1)) -- that exact 3-argument overload
// doesn't exist in the version PlatformIO's Library Dependency Finder
// resolves for this core (only begin(uint8_t) / begin(uint32_t, uint8_t),
// both hard-wired to the default SPI/spi0 instance internally via Sd2Card,
// found live 2026-09-21 as a build error). GP10-13 are hardwired to SPI1's
// alternate function on the RP2040 (fixed in silicon, not just a default),
// so the default SPI object can't be remapped onto them either. Using
// arduino-pico's own SDFS wrapper instead, whose SDFSConfig constructor
// takes an explicit HardwareSPI& (SPI1) -- this is the API arduino-pico's
// own docs/examples actually use for a non-default SPI bus.
using namespace sdfs;

namespace {

// UART link to the ESP32-S3. GP16/17 per a third-party reverse-engineered
// schematic -- NOT independently confirmed on this side yet (the matching
// ESP32-S3 pin claim from the same source turned out to be wrong, see
// src/sensecap/sd_mirror.cpp's comment; the ESP32 side is now fixed to
// Seeed's own officially documented GPIO19/20 instead). If "sd status" on
// the ESP32-S3 (see main.cpp there) never gets a PONG after re-flashing
// both sides, this is the pin assignment to double-check/sweep next --
// nothing else on the RP2040 side is known to use GP16/17.
constexpr int kUartTxPin = 16;
constexpr int kUartRxPin = 17;
constexpr uint32_t kUartBaud = 115200;

// SD card over SPI1, pins from Seeed's own RP2040 "MicroSD" dev tutorial.
constexpr int kSdSckPin = 10;
constexpr int kSdMosiPin = 11;
constexpr int kSdMisoPin = 12;
constexpr int kSdCsPin = 13;

// Our own packet types, deliberately outside Seeed's documented 0xA0-0xBF
// ranges (ESP32<->RP2040 stock sensor protocol) -- Seeed's own docs say
// "the following commands are for reference only, you can also define your
// own commands", so this coexists cleanly if that stock protocol is ever
// wanted back on some other packet type range.
constexpr uint8_t kPktMirrorLage = 0xE0;  // ESP32->RP2040: append line to /lage_mirror.log
constexpr uint8_t kPktMirrorEvent = 0xE1; // ESP32->RP2040: append line to /ereignisse_mirror.log
constexpr uint8_t kPktPing = 0xE2;        // ESP32->RP2040: are you there / is the SD ok?
constexpr uint8_t kPktPong = 0xE3;        // RP2040->ESP32: reply to ping, payload[0] = 1 if SD mounted ok

bool g_sdOk = false;

void sendPong() {
  uint8_t payload[2] = {kPktPong, (uint8_t)(g_sdOk ? 1 : 0)};
  uint8_t encoded[cobsMaxEncodedLength(sizeof(payload))];
  size_t n = cobsEncode(payload, sizeof(payload), encoded);
  Serial1.write(encoded, n);
  Serial1.write((uint8_t)0);
}

void appendLine(const char *filename, const uint8_t *data, size_t len) {
  if (!g_sdOk) return;
  fs::File f = SDFS.open(filename, "a");
  if (!f) return;
  f.write(data, len);
  f.write((const uint8_t *)"\n", 1);
  f.close();
}

void handlePacket(const uint8_t *payload, size_t len) {
  if (len < 1) return;
  uint8_t type = payload[0];
  const uint8_t *body = payload + 1;
  size_t bodyLen = len - 1;
  switch (type) {
    case kPktMirrorLage:
      appendLine("/lage_mirror.log", body, bodyLen);
      break;
    case kPktMirrorEvent:
      appendLine("/ereignisse_mirror.log", body, bodyLen);
      break;
    case kPktPing:
      sendPong();
      break;
    default:
      break; // unknown packet type, ignore rather than guess
  }
}

// Line-buffered COBS frame reader: bytes accumulate until a 0x00
// delimiter, then decode+dispatch. No length prefix needed -- COBS framing
// is self-delimiting. 600 bytes is comfortably above the ~512-byte lines
// sd_mirror.cpp on the ESP32-S3 side caps outgoing lines at.
uint8_t g_rxBuf[600];
size_t g_rxLen = 0;

void pollUart() {
  while (Serial1.available()) {
    uint8_t b = (uint8_t)Serial1.read();
    if (b == 0) {
      if (g_rxLen > 0) {
        uint8_t decoded[600];
        size_t n = cobsDecode(g_rxBuf, g_rxLen, decoded);
        if (n > 0) handlePacket(decoded, n);
      }
      g_rxLen = 0;
    } else if (g_rxLen < sizeof(g_rxBuf)) {
      g_rxBuf[g_rxLen++] = b;
    } else {
      g_rxLen = 0; // overflow: drop the malformed frame, wait for the next delimiter
    }
  }
}

} // namespace

void setup() {
  Serial1.setTX(kUartTxPin);
  Serial1.setRX(kUartRxPin);
  Serial1.begin(kUartBaud);

  SPI1.setSCK(kSdSckPin);
  SPI1.setTX(kSdMosiPin);
  SPI1.setRX(kSdMisoPin);
  SDFS.setConfig(SDFSConfig(kSdCsPin, SD_SCK_MHZ(1), SPI1));
  g_sdOk = SDFS.begin();
}

void loop() {
  pollUart();
}
