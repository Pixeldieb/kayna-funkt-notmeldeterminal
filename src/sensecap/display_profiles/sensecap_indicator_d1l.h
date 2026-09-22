#pragma once

// ============================================================================
// Display profile: Seeed SenseCAP Indicator (D1L)
// ============================================================================
//
// Panel: ST7701S, 480x480 RGB parallel (RGB666 over the bus, 3-wire 9-bit
// software SPI for the register init only). Touch: FT6336U (I2C).
// Both gated through a TCA9535 IO expander (see io_expander.h/.cpp).
//
// This is the only profile so far, but main.cpp only ever refers to the
// macros below, never these exact names — to bring up a different panel,
// write a new file here with the same macro names and swap the #include
// in main.cpp (or comment it out and uncomment another). No other file
// should need to change.
//
// Confirmed on real hardware (2026-09-17). Required, non-obvious
// platformio.ini settings for this exact panel/PSRAM combination:
//   board_build.flash_mode = dio          (qio bootloops on this unit)
//   board_build.arduino.memory_type = dio_opi  (qio_opi/qio_qspi bootloop;
//                                                PSRAM here is octal)
//   board_build.partitions = default_8MB.csv   (this unit's flash is 8MB,
//                                                default_16MB.csv bootloops)
//   platform = espressif32@5.4.0          (Arduino_GFX's ESP32-S3 RGB panel
//                                           driver needs ESP-IDF 4.x)
// See platformio.ini for the full env and git history for the bring-up
// story (bootloop -> PSRAM error -> upside-down panel, in that order).

#define DISPLAY_SCREEN_WIDTH  480
#define DISPLAY_SCREEN_HEIGHT 480

// The panel is mounted so the natural framebuffer orientation appears
// upside down on this unit. main.cpp's flush callback and touch.cpp both
// apply a 180° correction gated on this flag — flip it here, not in the
// logic, if a differently-mounted unit of the same panel needs it undone.
#define DISPLAY_ROTATE_180 1

// --- I2C bus: touch controller + IO expander ---
#define DISPLAY_I2C_SDA_PIN 39
#define DISPLAY_I2C_SCL_PIN 40

// --- Backlight (active HIGH) ---
#define DISPLAY_BL_PIN 45

// --- ST7701 software SPI (register init only, not pixel data) ---
#define DISPLAY_SPI_SCK_PIN  41
#define DISPLAY_SPI_MOSI_PIN 48

// --- RGB parallel panel bus (pixel data) ---
#define DISPLAY_LCD_DE_PIN    18
#define DISPLAY_LCD_VSYNC_PIN 17
#define DISPLAY_LCD_HSYNC_PIN 16
#define DISPLAY_LCD_PCLK_PIN  21
#define DISPLAY_LCD_R0_PIN  4
#define DISPLAY_LCD_R1_PIN  3
#define DISPLAY_LCD_R2_PIN  2
#define DISPLAY_LCD_R3_PIN  1
#define DISPLAY_LCD_R4_PIN  0
#define DISPLAY_LCD_G0_PIN  10
#define DISPLAY_LCD_G1_PIN  9
#define DISPLAY_LCD_G2_PIN  8
#define DISPLAY_LCD_G3_PIN  7
#define DISPLAY_LCD_G4_PIN  6
#define DISPLAY_LCD_G5_PIN  5
#define DISPLAY_LCD_B0_PIN  15
#define DISPLAY_LCD_B1_PIN  14
#define DISPLAY_LCD_B2_PIN  13
#define DISPLAY_LCD_B3_PIN  12
#define DISPLAY_LCD_B4_PIN  11

// hsync/vsync: polarity, front_porch, pulse_width, back_porch
#define DISPLAY_HSYNC_POLARITY     1
#define DISPLAY_HSYNC_FRONT_PORCH  10
#define DISPLAY_HSYNC_PULSE_WIDTH  8
#define DISPLAY_HSYNC_BACK_PORCH   50
#define DISPLAY_VSYNC_POLARITY     1
#define DISPLAY_VSYNC_FRONT_PORCH  10
#define DISPLAY_VSYNC_PULSE_WIDTH  8
#define DISPLAY_VSYNC_BACK_PORCH   20

// LVGL draw buffer height, in lines. 480 = full-screen buffer; needs PSRAM.
#define DISPLAY_LVGL_BUFFER_LINES 480
