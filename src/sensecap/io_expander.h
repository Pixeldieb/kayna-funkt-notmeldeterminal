#pragma once
#include <Arduino.h>

// TCA9535 IO expander on the SenseCAP Indicator's I2C bus. Gates the LCD
// chip-select/reset lines and the touch controller's reset line (see
// SENSECAP_INDICATOR_HARDWARE_REFERENCE in the project research notes).
// Call ioexp_begin() once, after Wire.begin(), before touching the LCD or
// touch controller.

#define IOEXP_ADDR 0x20
#define IOEXP_LCD_CS  4  // P04
#define IOEXP_LCD_RST 5  // P05
#define IOEXP_TP_RST  7  // P07

// SX1262 LoRa control lines — all behind this same expander, not real
// GPIOs. See lora_radio.h/.cpp for why that matters (RadioLib's BUSY
// polling expects fast direct GPIO reads; these go over I2C instead).
#define IOEXP_LORA_NSS   0  // P00, output (SPI chip select)
#define IOEXP_LORA_RST   1  // P01, output
#define IOEXP_LORA_BUSY  2  // P02, input
#define IOEXP_LORA_DIO1  3  // P03, input

bool ioexp_begin();
void ioexp_set_pin_output(uint8_t pin);
void ioexp_set_pin_input(uint8_t pin);
void ioexp_write_pin(uint8_t pin, bool level);
bool ioexp_read_pin(uint8_t pin);
