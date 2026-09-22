#include "io_expander.h"
#include <Wire.h>

namespace {
constexpr uint8_t REG_INPUT_PORT0 = 0x00;
constexpr uint8_t REG_OUTPUT_PORT0 = 0x02;
constexpr uint8_t REG_CONFIG_PORT0 = 0x06;
constexpr uint8_t REG_CONFIG_PORT1 = 0x07;
uint8_t g_port0_cache = 0xFF;

void write_reg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(IOEXP_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

uint8_t read_reg(uint8_t reg) {
  Wire.beginTransmission(IOEXP_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)IOEXP_ADDR, (uint8_t)1);
  return Wire.read();
}
} // namespace

bool ioexp_begin() {
  Wire.beginTransmission(IOEXP_ADDR);
  if (Wire.endTransmission() != 0) return false;
  g_port0_cache = read_reg(REG_OUTPUT_PORT0);
  return true;
}

void ioexp_set_pin_output(uint8_t pin) {
  uint8_t reg = (pin < 8) ? REG_CONFIG_PORT0 : REG_CONFIG_PORT1;
  uint8_t bit = pin % 8;
  uint8_t val = read_reg(reg);
  val &= ~(1 << bit); // 0 = output on TCA9535
  write_reg(reg, val);
}

void ioexp_write_pin(uint8_t pin, bool level) {
  // Only port0 (pins 0-7) is used on this board (LCD_CS/RST, TOUCH_RST,
  // LoRa NSS/RST/BUSY/DIO1).
  uint8_t bit = pin % 8;
  if (level) {
    g_port0_cache |= (1 << bit);
  } else {
    g_port0_cache &= ~(1 << bit);
  }
  write_reg(REG_OUTPUT_PORT0, g_port0_cache);
}

void ioexp_set_pin_input(uint8_t pin) {
  uint8_t reg = (pin < 8) ? REG_CONFIG_PORT0 : REG_CONFIG_PORT1;
  uint8_t bit = pin % 8;
  uint8_t val = read_reg(reg);
  val |= (1 << bit); // 1 = input on TCA9535
  write_reg(reg, val);
}

bool ioexp_read_pin(uint8_t pin) {
  // Only port0 needed here (BUSY/DIO1 are both < 8).
  uint8_t val = read_reg(REG_INPUT_PORT0);
  return (val >> (pin % 8)) & 1;
}
