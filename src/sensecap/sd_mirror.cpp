#include "sd_mirror.h"

#include <string.h>

#include "cobs.h"

namespace {

// UART link to the RP2040 co-processor -- see src/sensecap_rp2040/main.cpp
// for the receiving end.
//
// GPIO43/44 (a third-party reverse-engineered schematic's claim for this
// link) turned out to be WRONG -- found live 2026-09-21: those are this
// board's own Serial/UART0 pins (Serial.begin(115200) in setup(), no
// custom pins given -- ESP32-S3's hardware-default UART0, also documented
// in platformio.ini's "Classic UART0 console" comment). Using them for a
// second UART corrupted both: the debug console and the RP2040 link
// garbled into each other. Seeed's OWN official wiki ("Develop both chips
// with Arduino" -- SenseCAP_Indicator_Arduino.md) says the actual link is
// "pin 20 and pin 19 on the ESP32S3", which also matches this board's own
// free-GPIO budget (everything else is claimed by the display/I2C/LoRa/
// backlight/button, see src/sensecap/display_profiles -- 19 and 20 were
// the only ones left unaccounted for).
constexpr int kUartTxPin = 19;
constexpr int kUartRxPin = 20;
constexpr uint32_t kUartBaud = 115200;

constexpr uint8_t kPktMirrorLage = 0xE0;
constexpr uint8_t kPktMirrorEvent = 0xE1;
constexpr uint8_t kPktPing = 0xE2;
constexpr uint8_t kPktPong = 0xE3;

constexpr size_t kMaxLineLen = 500; // truncation point, see sendPacket()

void sendRaw(const uint8_t *raw, size_t rawLen) {
  uint8_t encoded[cobsMaxEncodedLength(kMaxLineLen + 1)];
  size_t n = cobsEncode(raw, rawLen, encoded);
  Serial1.write(encoded, n);
  Serial1.write((uint8_t)0);
}

void sendPacket(uint8_t type, const String &line) {
  uint8_t raw[kMaxLineLen + 1];
  size_t bodyLen = line.length();
  if (bodyLen > kMaxLineLen) bodyLen = kMaxLineLen; // defensive truncation, never block/grow unbounded
  raw[0] = type;
  memcpy(raw + 1, line.c_str(), bodyLen);
  sendRaw(raw, bodyLen + 1);
}

} // namespace

void sdMirrorBegin() { Serial1.begin(kUartBaud, SERIAL_8N1, kUartRxPin, kUartTxPin); }

void sdMirrorSend(const char *table, const String &line) {
  if (strcmp(table, "lagemeldungen") == 0) {
    sendPacket(kPktMirrorLage, line);
  } else if (strcmp(table, "ereignisse") == 0) {
    sendPacket(kPktMirrorEvent, line);
  }
}

bool sdMirrorPing(uint32_t timeoutMs, bool *outSdOk) {
  while (Serial1.available()) Serial1.read(); // drop any stale bytes first

  uint8_t ping = kPktPing;
  sendRaw(&ping, 1);

  uint8_t rxBuf[16];
  size_t rxLen = 0;
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (Serial1.available()) {
      uint8_t b = (uint8_t)Serial1.read();
      if (b == 0) {
        if (rxLen == 0) continue;
        uint8_t decoded[16];
        size_t n = cobsDecode(rxBuf, rxLen, decoded);
        if (n >= 2 && decoded[0] == kPktPong) {
          if (outSdOk) *outSdOk = decoded[1] != 0;
          return true;
        }
        rxLen = 0;
      } else if (rxLen < sizeof(rxBuf)) {
        rxBuf[rxLen++] = b;
      } else {
        rxLen = 0; // overflow, drop this frame
      }
    }
  }
  return false; // no reply within timeoutMs -- link down or RP2040 not flashed with this firmware
}
