#include "lora_radio.h"

#include <RadioLib.h>
#include <SPI.h>
#include <string.h>

#include "io_expander.h"

// SPI bus shared with the ST7701's software-SPI init (main.cpp) — that
// one bit-bangs plain digitalWrite and only runs once at boot, so it
// doesn't conflict with the real SPI peripheral claiming these same
// pins afterwards for LoRa.
#define LORA_SCK_PIN  41
#define LORA_MISO_PIN 47
#define LORA_MOSI_PIN 48

namespace {

// NSS/RST/BUSY/DIO1 are all TCA9535 IO-expander bits, not real GPIOs
// (see io_expander.h). RadioLibHal's pin arguments are just plain
// uint32_t with platform-defined meaning, so we encode "this is an
// IO-expander bit, not a GPIO number" with an offset comfortably above
// any real ESP32 GPIO (0-48).
constexpr uint32_t VPIN_BASE = 1000;
constexpr uint32_t VPIN_NSS = VPIN_BASE + IOEXP_LORA_NSS;
constexpr uint32_t VPIN_RST = VPIN_BASE + IOEXP_LORA_RST;
constexpr uint32_t VPIN_BUSY = VPIN_BASE + IOEXP_LORA_BUSY;
constexpr uint32_t VPIN_DIO1 = VPIN_BASE + IOEXP_LORA_DIO1;

// Custom RadioLib hardware abstraction: redirects every control line to
// the IO expander instead of real GPIOs. The one real known risk here
// (see lora_radio.h): BUSY is polled by RadioLib during SPI transactions
// expecting a fast direct GPIO read; ours goes over I2C instead, which
// is much slower and could make transactions unreliable or very slow.
// This is the actual unknown this experiment is meant to test.
class SenseCapLoraHal : public RadioLibHal {
 public:
  SenseCapLoraHal() : RadioLibHal(INPUT, OUTPUT, LOW, HIGH, RISING, FALLING) {}

  void pinMode(uint32_t pin, uint32_t mode) override {
    if (pin < VPIN_BASE) { ::pinMode(pin, mode); return; }
    if (mode == OUTPUT) ioexp_set_pin_output(pin - VPIN_BASE);
    else ioexp_set_pin_input(pin - VPIN_BASE);
  }
  void digitalWrite(uint32_t pin, uint32_t value) override {
    if (pin < VPIN_BASE) { ::digitalWrite(pin, value); return; }
    ioexp_write_pin(pin - VPIN_BASE, value != 0);
  }
  uint32_t digitalRead(uint32_t pin) override {
    if (pin < VPIN_BASE) return ::digitalRead(pin);
    return ioexp_read_pin(pin - VPIN_BASE) ? HIGH : LOW;
  }
  // DIO1 is behind the IO expander, so it can't be a real hardware
  // interrupt source. No-op: this Hal only supports RadioLib's
  // blocking/polling calls (begin()/transmit()), not interrupt-driven
  // startReceive() — that would need GPIO42 (the expander's own INT
  // line, a real GPIO) wired up separately to know something changed,
  // then reading the expander to see if it was DIO1. Not done yet.
  void attachInterrupt(uint32_t, void (*)(void), uint32_t) override {}
  void detachInterrupt(uint32_t) override {}
  void delay(RadioLibTime_t ms) override { ::delay(ms); }
  void delayMicroseconds(RadioLibTime_t us) override { ::delayMicroseconds(us); }
  RadioLibTime_t millis() override { return ::millis(); }
  RadioLibTime_t micros() override { return ::micros(); }
  long pulseIn(uint32_t pin, uint32_t state, RadioLibTime_t timeout) override {
    return (pin < VPIN_BASE) ? ::pulseIn(pin, state, timeout) : 0;
  }
  void spiBegin() override { SPI.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, -1); }
  void spiBeginTransaction() override { SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0)); }
  void spiTransfer(uint8_t *out, size_t len, uint8_t *in) override {
    for (size_t i = 0; i < len; i++) in[i] = SPI.transfer(out[i]);
  }
  void spiEndTransaction() override { SPI.endTransaction(); }
  void spiEnd() override { SPI.end(); }
};

SenseCapLoraHal g_hal;
Module g_module(&g_hal, VPIN_NSS, VPIN_DIO1, VPIN_RST, VPIN_BUSY);
SX1262 g_radio(&g_module);
bool g_ready = false;

// Root cause of the long-standing CHIP_NOT_FOUND (Issue #36): SX126x::reset()
// pulses RST and, with no settling delay at all, immediately starts hammering
// standby() over SPI. On real hardware the chip needs a short settle time
// after RST release before it answers reliably -- normally invisible because
// a plain GPIO reset+first-SPI-command happens with enough incidental
// latency, but with RST/BUSY routed through this I2C IO-expander, RadioLib's
// own reset()-then-standby() sequence ran too close together and consistently
// got back all-zero SPI status bytes (confirmed via RADIOLIB_DEBUG_SPI: every
// GET_STATUS during the retry loop returned 0x00 0x00 for a full 10s before
// giving up). Doing our own reset with an explicit settle delay before
// handing control to RadioLib makes it succeed on the first try, every time
// (verified across repeated power cycles).
//
// The GET_STATUS probe itself is a cheap sanity check, not the fix: it just
// confirms the chip actually answered (a real status byte is never 0x00).
bool lora_radio_wake_chip() {
  ioexp_set_pin_output(IOEXP_LORA_NSS);
  ioexp_write_pin(IOEXP_LORA_NSS, true);
  ioexp_set_pin_output(IOEXP_LORA_RST);
  ioexp_set_pin_input(IOEXP_LORA_BUSY);
  ioexp_set_pin_input(IOEXP_LORA_DIO1);

  ioexp_write_pin(IOEXP_LORA_RST, false);
  delay(2);
  ioexp_write_pin(IOEXP_LORA_RST, true);
  delay(20); // settle time RadioLib's own reset() doesn't wait for

  SPI.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, -1);
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  ioexp_write_pin(IOEXP_LORA_NSS, false);
  uint8_t status1 = SPI.transfer(0xC0); // GET_STATUS
  uint8_t status2 = SPI.transfer(0x00);
  ioexp_write_pin(IOEXP_LORA_NSS, true);
  SPI.endTransaction();

  bool alive = (status1 != 0x00) || (status2 != 0x00);
  Serial.printf("[LORA] Wake-Probe: GET_STATUS -> 0x%02X 0x%02X (%s)\n", status1, status2,
                alive ? "Chip antwortet" : "keine Antwort");
  return alive;
}

} // namespace

bool lora_radio_begin() {
  if (!lora_radio_wake_chip()) {
    Serial.println("[LORA] Kein SX1262 auf dem Bus erreichbar, breche ab");
    return false;
  }
  // Generic bring-up only, arbitrary starting parameters -- SX1262::begin()
  // just needs *a* valid config to leave the chip in a known, working state.
  // meshtastic_proto_begin() reconfigures frequency/BW/SF/CR/sync word/
  // preamble/power to the exact values real Meshtastic firmware uses right
  // after this, the same two-phase pattern RadioLib's own examples use.
  int16_t state = g_radio.begin(868.0);
  g_ready = (state == RADIOLIB_ERR_NONE);
  Serial.printf("[LORA] begin() -> %d (%s)\n", state, g_ready ? "OK" : "FEHLER");
  return g_ready;
}

bool lora_radio_ready() { return g_ready; }

SX1262 &lora_radio_instance() { return g_radio; }

bool lora_radio_send_test(const char *text) {
  if (!g_ready) {
    Serial.println("[LORA] send_test: Radio nicht bereit");
    return false;
  }
  Serial.printf("[LORA] Sende (roh, nicht Meshtastic-kompatibel): \"%s\"\n", text);
  int16_t state = g_radio.transmit((uint8_t *)text, strlen(text));
  Serial.printf("[LORA] transmit() -> %d\n", state);
  return state == RADIOLIB_ERR_NONE;
}
