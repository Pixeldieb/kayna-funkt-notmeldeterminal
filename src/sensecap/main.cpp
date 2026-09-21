// ============================================================================
// SenseCAP Indicator (D1L) — display/touch bring-up.
//
// Confirmed working on real hardware (2026-09-17): ST7701S 480x480 RGB
// panel + FT6336U capacitive touch via the TCA9535 IO expander, LVGL on
// top. Needed, hardware-specific build settings (see platformio.ini):
// flash_mode=dio, memory_type=dio_opi (octal PSRAM), 8MB partition table.
// The panel is mounted upside down relative to its natural framebuffer
// orientation — corrected in lvgl_disp_flush()/touch.cpp, not here.
//
// Meshtastic: real onboard SX1262 (see lora_radio.h) speaking the actual
// Meshtastic-protocol-compatible packet format (see meshtastic_proto.h) on
// the public default primary channel — not a raw/incompatible test signal.
// lage_db still has the 3 seeded example entries baked in for first boot
// alongside whatever real mesh traffic writes in.
// ============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <esp32s3/rom/cache.h>
#include <driver/uart.h>
#include <time.h>

// LVGL must be included before Arduino_GFX to avoid conflicts.
#define LV_CONF_INCLUDE_SIMPLE
#include <lvgl.h>
#include <Arduino_GFX_Library.h>

#include "io_expander.h"
#include "lage_db.h"
#include "mesh_security.h"
#include "lora_radio.h"
#include "meshtastic_proto.h"
#include "sd_mirror.h"
#include "station_config.h"
#include "touch.h"
#include "ui_model.h"
#include "wall_clock.h"

// Display/touch pin & timing profile. To bring up a different panel,
// write a new file under display_profiles/ with the same DISPLAY_* macro
// names and swap this include (or comment it out for none) — nothing
// else in this file should need to change.
#include "display_profiles/sensecap_indicator_d1l.h"

#define SCREEN_WIDTH  DISPLAY_SCREEN_WIDTH
#define SCREEN_HEIGHT DISPLAY_SCREEN_HEIGHT

#define I2C_SDA_PIN DISPLAY_I2C_SDA_PIN
#define I2C_SCL_PIN DISPLAY_I2C_SCL_PIN

#define GFX_BL DISPLAY_BL_PIN

#define LCD_SPI_SCK  DISPLAY_SPI_SCK_PIN
#define LCD_SPI_MOSI DISPLAY_SPI_MOSI_PIN

#define LCD_DE     DISPLAY_LCD_DE_PIN
#define LCD_VSYNC  DISPLAY_LCD_VSYNC_PIN
#define LCD_HSYNC  DISPLAY_LCD_HSYNC_PIN
#define LCD_PCLK   DISPLAY_LCD_PCLK_PIN
#define LCD_R0  DISPLAY_LCD_R0_PIN
#define LCD_R1  DISPLAY_LCD_R1_PIN
#define LCD_R2  DISPLAY_LCD_R2_PIN
#define LCD_R3  DISPLAY_LCD_R3_PIN
#define LCD_R4  DISPLAY_LCD_R4_PIN
#define LCD_G0  DISPLAY_LCD_G0_PIN
#define LCD_G1  DISPLAY_LCD_G1_PIN
#define LCD_G2  DISPLAY_LCD_G2_PIN
#define LCD_G3  DISPLAY_LCD_G3_PIN
#define LCD_G4  DISPLAY_LCD_G4_PIN
#define LCD_G5  DISPLAY_LCD_G5_PIN
#define LCD_B0  DISPLAY_LCD_B0_PIN
#define LCD_B1  DISPLAY_LCD_B1_PIN
#define LCD_B2  DISPLAY_LCD_B2_PIN
#define LCD_B3  DISPLAY_LCD_B3_PIN
#define LCD_B4  DISPLAY_LCD_B4_PIN

#define LVGL_BUFFER_LINES DISPLAY_LVGL_BUFFER_LINES

// Talking to esp_lcd's RGB panel driver directly instead of going through
// Arduino_ESP32RGBPanel/Arduino_RGB_Display: that wrapper doesn't expose
// on_frame_trans_done, and we need it (see lvgl_disp_flush) to gate our
// writes to just-after a frame finishes, instead of at a random point
// while the DMA is scanning the same buffer out to the panel. The
// esp_rgb_panel_t struct and the __containerof trick to reach ->fb are
// copied from Arduino_ESP32RGBPanel.h (not part of the public esp_lcd
// API, but that header already re-declares it, so it's available to us
// too via the Arduino_GFX_Library.h include above).
static esp_lcd_panel_handle_t rgb_panel_handle = nullptr;
static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;
static lv_color_t *lvgl_buf1 = nullptr;
static lv_color_t *lvgl_buf2 = nullptr;
static uint16_t *lcd_framebuffer = nullptr;

static bool touch_available = false;
static int16_t touch_last_x = 0;
static int16_t touch_last_y = 0;
static unsigned long lvgl_last_tick = 0;

// Bumped from an ISR (frame_trans_done), so: volatile, and the ISR side
// only ever increments it — no other shared state, no locking needed.
static volatile uint32_t g_frame_done_count = 0;

static bool IRAM_ATTR on_frame_trans_done(esp_lcd_panel_handle_t panel,
                                           esp_lcd_rgb_panel_event_data_t *edata,
                                           void *user_ctx) {
  g_frame_done_count++;
  return false; // no high-priority task to wake
}

// ============================================================================
// ST7701S bring-up: 3-wire 9-bit software SPI for the register init only.
// Pixel data afterwards goes over the separate RGB parallel bus.
// ============================================================================

static void spi_write_9bit(uint8_t dc, uint8_t data) {
  digitalWrite(LCD_SPI_SCK, LOW);
  digitalWrite(LCD_SPI_MOSI, dc ? HIGH : LOW);
  digitalWrite(LCD_SPI_SCK, HIGH);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(LCD_SPI_SCK, LOW);
    digitalWrite(LCD_SPI_MOSI, (data >> i) & 1 ? HIGH : LOW);
    digitalWrite(LCD_SPI_SCK, HIGH);
  }
}

static void lcd_write_cmd(uint8_t cmd) {
  ioexp_write_pin(IOEXP_LCD_CS, LOW);
  spi_write_9bit(0, cmd);
  ioexp_write_pin(IOEXP_LCD_CS, HIGH);
}

static void lcd_write_data(uint8_t data) {
  ioexp_write_pin(IOEXP_LCD_CS, LOW);
  spi_write_9bit(1, data);
  ioexp_write_pin(IOEXP_LCD_CS, HIGH);
}

static void st7701_init(void) {
  Serial.println("[LCD] ST7701 init sequence...");

  lcd_write_cmd(0xFF);
  lcd_write_data(0x77); lcd_write_data(0x01); lcd_write_data(0x00);
  lcd_write_data(0x00); lcd_write_data(0x10);

  lcd_write_cmd(0xC0); lcd_write_data(0x3B); lcd_write_data(0x00);
  lcd_write_cmd(0xC1); lcd_write_data(0x0D); lcd_write_data(0x02);
  lcd_write_cmd(0xC2); lcd_write_data(0x31); lcd_write_data(0x05);
  lcd_write_cmd(0xCD); lcd_write_data(0x08);

  lcd_write_cmd(0xB0);
  lcd_write_data(0x00); lcd_write_data(0x11); lcd_write_data(0x18); lcd_write_data(0x0E);
  lcd_write_data(0x11); lcd_write_data(0x06); lcd_write_data(0x07); lcd_write_data(0x08);
  lcd_write_data(0x07); lcd_write_data(0x22); lcd_write_data(0x04); lcd_write_data(0x12);
  lcd_write_data(0x0F); lcd_write_data(0xAA); lcd_write_data(0x31); lcd_write_data(0x18);

  lcd_write_cmd(0xB1);
  lcd_write_data(0x00); lcd_write_data(0x11); lcd_write_data(0x19); lcd_write_data(0x0E);
  lcd_write_data(0x12); lcd_write_data(0x07); lcd_write_data(0x08); lcd_write_data(0x08);
  lcd_write_data(0x08); lcd_write_data(0x22); lcd_write_data(0x04); lcd_write_data(0x11);
  lcd_write_data(0x11); lcd_write_data(0xA9); lcd_write_data(0x32); lcd_write_data(0x18);

  lcd_write_cmd(0xFF);
  lcd_write_data(0x77); lcd_write_data(0x01); lcd_write_data(0x00);
  lcd_write_data(0x00); lcd_write_data(0x11);

  lcd_write_cmd(0xB0); lcd_write_data(0x60);
  lcd_write_cmd(0xB1); lcd_write_data(0x32);
  lcd_write_cmd(0xB2); lcd_write_data(0x07);
  lcd_write_cmd(0xB3); lcd_write_data(0x80);
  lcd_write_cmd(0xB5); lcd_write_data(0x49);
  lcd_write_cmd(0xB7); lcd_write_data(0x85);
  lcd_write_cmd(0xB8); lcd_write_data(0x21);
  lcd_write_cmd(0xC1); lcd_write_data(0x78);
  lcd_write_cmd(0xC2); lcd_write_data(0x78);
  delay(20);

  lcd_write_cmd(0xE0);
  lcd_write_data(0x00); lcd_write_data(0x1B); lcd_write_data(0x02);

  lcd_write_cmd(0xE1);
  lcd_write_data(0x08); lcd_write_data(0xA0); lcd_write_data(0x00); lcd_write_data(0x00);
  lcd_write_data(0x07); lcd_write_data(0xA0); lcd_write_data(0x00); lcd_write_data(0x00);
  lcd_write_data(0x00); lcd_write_data(0x44); lcd_write_data(0x44);

  lcd_write_cmd(0xE2);
  lcd_write_data(0x11); lcd_write_data(0x11); lcd_write_data(0x44); lcd_write_data(0x44);
  lcd_write_data(0xED); lcd_write_data(0xA0); lcd_write_data(0x00); lcd_write_data(0x00);
  lcd_write_data(0xEC); lcd_write_data(0xA0); lcd_write_data(0x00); lcd_write_data(0x00);

  lcd_write_cmd(0xE3);
  lcd_write_data(0x00); lcd_write_data(0x00); lcd_write_data(0x11); lcd_write_data(0x11);

  lcd_write_cmd(0xE4);
  lcd_write_data(0x44); lcd_write_data(0x44);

  lcd_write_cmd(0xE5);
  lcd_write_data(0x0A); lcd_write_data(0xE9); lcd_write_data(0xD8); lcd_write_data(0xA0);
  lcd_write_data(0x0C); lcd_write_data(0xEB); lcd_write_data(0xD8); lcd_write_data(0xA0);
  lcd_write_data(0x0E); lcd_write_data(0xED); lcd_write_data(0xD8); lcd_write_data(0xA0);
  lcd_write_data(0x10); lcd_write_data(0xEF); lcd_write_data(0xD8); lcd_write_data(0xA0);

  lcd_write_cmd(0xE6);
  lcd_write_data(0x00); lcd_write_data(0x00); lcd_write_data(0x11); lcd_write_data(0x11);

  lcd_write_cmd(0xE7);
  lcd_write_data(0x44); lcd_write_data(0x44);

  lcd_write_cmd(0xE8);
  lcd_write_data(0x09); lcd_write_data(0xE8); lcd_write_data(0xD8); lcd_write_data(0xA0);
  lcd_write_data(0x0B); lcd_write_data(0xEA); lcd_write_data(0xD8); lcd_write_data(0xA0);
  lcd_write_data(0x0D); lcd_write_data(0xEC); lcd_write_data(0xD8); lcd_write_data(0xA0);
  lcd_write_data(0x0F); lcd_write_data(0xEE); lcd_write_data(0xD8); lcd_write_data(0xA0);

  lcd_write_cmd(0xEB);
  lcd_write_data(0x02); lcd_write_data(0x00); lcd_write_data(0xE4); lcd_write_data(0xE4);
  lcd_write_data(0x88); lcd_write_data(0x00); lcd_write_data(0x40);

  lcd_write_cmd(0xEC);
  lcd_write_data(0x3C); lcd_write_data(0x00);

  lcd_write_cmd(0xED);
  lcd_write_data(0xAB); lcd_write_data(0x89); lcd_write_data(0x76); lcd_write_data(0x54);
  lcd_write_data(0x02); lcd_write_data(0xFF); lcd_write_data(0xFF); lcd_write_data(0xFF);
  lcd_write_data(0xFF); lcd_write_data(0xFF); lcd_write_data(0xFF); lcd_write_data(0x20);
  lcd_write_data(0x45); lcd_write_data(0x67); lcd_write_data(0x98); lcd_write_data(0xBA);

  lcd_write_cmd(0xFF);
  lcd_write_data(0x77); lcd_write_data(0x01); lcd_write_data(0x00);
  lcd_write_data(0x00); lcd_write_data(0x00);

  lcd_write_cmd(0x3A); lcd_write_data(0x60); // RGB666
  lcd_write_cmd(0x36); lcd_write_data(0x00); // RGB order
  lcd_write_cmd(0x21);                       // display inversion on

  lcd_write_cmd(0x11); delay(120); // sleep out
  lcd_write_cmd(0x29); delay(20);  // display on

  Serial.println("[LCD] ST7701 init done");
}

// ============================================================================
// LVGL glue
// ============================================================================

// This unit's panel is mounted/wired so that the natural framebuffer
// orientation appears upside down — confirmed by looking at the real
// screen. We correct for it here (180° rotation), not via LVGL's own
// disp_drv.rotated, so widget code never has to think about orientation.
// touch.cpp applies the matching rotation to touch coordinates.
static void lvgl_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  if (lcd_framebuffer != nullptr) {
    // Wait for a frame to just finish (the DMA scan is right back at the
    // top, about to start over) before writing — starting our copy here
    // instead of at a random moment gives it the whole ~23ms frame
    // period to finish before the scan comes back around to the rows we
    // touch. Bounded wait: if the callback ever stops firing for some
    // reason, don't hang the UI forever, just skip the sync this once.
    uint32_t start_count = g_frame_done_count;
    uint32_t waited_us = 0;
    while (g_frame_done_count == start_count && waited_us < 70000) {
      delayMicroseconds(100);
      waited_us += 100;
    }
    uint16_t *src = (uint16_t *)color_p;
    int32_t rot_x1 = SCREEN_WIDTH - 1 - area->x2;
    int32_t rot_y1 = SCREEN_HEIGHT - 1 - area->y2;
    // Per-row pointers hoisted out of the inner loop (was recomputing
    // dy*SCREEN_WIDTH and sy*w for every single pixel) — this copy
    // blocks the CPU while the RGB peripheral keeps scanning the same
    // buffer via DMA in the background, so its wall-clock time directly
    // affects how much visible tearing/corruption shows up.
    //
    // Iterating sy from h-1 down to 0 (instead of 0 up to h-1) makes
    // the *destination* row (rot_y1 + (h-1-sy)) count up from rot_y1
    // instead of down from it — i.e. we write the raw framebuffer
    // top-to-bottom, the same direction the panel scans it out. We
    // start right after a frame-done event (scan restarting at row 0),
    // so writing in the same direction means we're always staying
    // ahead of the scan instead of the two of us converging on each
    // other from opposite ends, which was almost guaranteed to collide
    // somewhere in the middle for any reasonably large area.
    for (uint32_t sy = h; sy-- > 0;) {
      uint16_t *dst_row = &lcd_framebuffer[(rot_y1 + (h - 1 - sy)) * SCREEN_WIDTH + rot_x1];
      const uint16_t *src_row = &src[sy * w];
      for (uint32_t sx = 0; sx < w; sx++) {
        dst_row[w - 1 - sx] = src_row[sx];
      }
    }
    // PSRAM is accessed through the CPU cache; the LCD peripheral's DMA
    // reads straight from PSRAM, bypassing that cache. Without writing
    // the cache back explicitly, the DMA can read stale/half-written
    // data while it's still sitting in cache — exactly the "chaos"/
    // rolling-glitch look we've been chasing through several other
    // fixes. Arduino_GFX's own draw16bitRGBBitmap() does this after
    // every write (see Arduino_RGB_Display.cpp); our direct-framebuffer
    // path never did. Same address/size formula as its rotation-0 case.
    Cache_WriteBack_Addr((uint32_t)&lcd_framebuffer[rot_y1 * SCREEN_WIDTH + rot_x1],
                          (SCREEN_WIDTH * (h - 1) + w) * sizeof(uint16_t));
  }
  // No fallback path anymore (that went through Arduino_RGB_Display,
  // which we no longer create — see init_display_hw). lcd_framebuffer
  // is only ever null if esp_lcd_new_rgb_panel's PSRAM allocation
  // failed, in which case there's nothing sensible left to draw to.
  lv_disp_flush_ready(disp);
}

static void lvgl_touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  int16_t x, y;
  if (touch_available && touch_read_primary(&x, &y)) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x;
    data->point.y = y;
    touch_last_x = x;
    touch_last_y = y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
    data->point.x = touch_last_x;
    data->point.y = touch_last_y;
  }
}

static bool init_display_hw(void) {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000);

  if (!ioexp_begin()) {
    Serial.println("[ERROR] IO expander (TCA9535) not detected at 0x20");
    return false;
  }
  Serial.println("[OK] IO expander detected");

  ioexp_set_pin_output(IOEXP_LCD_CS);
  ioexp_set_pin_output(IOEXP_LCD_RST);
  ioexp_write_pin(IOEXP_LCD_CS, HIGH);
  ioexp_write_pin(IOEXP_LCD_RST, HIGH);

  pinMode(LCD_SPI_SCK, OUTPUT);
  pinMode(LCD_SPI_MOSI, OUTPUT);
  digitalWrite(LCD_SPI_SCK, HIGH);
  digitalWrite(LCD_SPI_MOSI, LOW);

  Serial.println("[LCD] Resetting panel...");
  ioexp_write_pin(IOEXP_LCD_RST, LOW);
  delay(20);
  ioexp_write_pin(IOEXP_LCD_RST, HIGH);
  delay(150);

  st7701_init();

  // Same esp_lcd_rgb_panel_config_t fields Arduino_ESP32RGBPanel::
  // getFrameBuffer() builds (verified against its source) — just with
  // on_frame_trans_done wired up, which that wrapper never sets.
  esp_lcd_rgb_panel_config_t panel_config = {};
  panel_config.clk_src = LCD_CLK_SRC_PLL160M;
  // Measured (see [DIAG] logs): a full-screen copy+cache-writeback takes
  // ~29ms, but at 12MHz pclk the panel completes a full frame every
  // ~22.7ms (measured via on_frame_trans_done: ~44/s) — we are, on
  // paper, guaranteed to be too slow no matter how well-synchronized the
  // write start is. Slowing the pixel clock stretches the frame period
  // so our write actually fits inside it: at 6MHz, frame period ≈ 45ms,
  // comfortably longer than the ~29ms copy (with real margin for
  // PSRAM/cache jitter). Trade-off: refresh rate drops to ~22Hz — still
  // plenty for a static menu UI, not for anything resembling video.
  panel_config.timings.pclk_hz = 6000000;
  panel_config.timings.h_res = SCREEN_WIDTH;
  panel_config.timings.v_res = SCREEN_HEIGHT;
  panel_config.timings.hsync_pulse_width = DISPLAY_HSYNC_PULSE_WIDTH;
  panel_config.timings.hsync_back_porch = DISPLAY_HSYNC_BACK_PORCH;
  panel_config.timings.hsync_front_porch = DISPLAY_HSYNC_FRONT_PORCH;
  panel_config.timings.vsync_pulse_width = DISPLAY_VSYNC_PULSE_WIDTH;
  panel_config.timings.vsync_back_porch = DISPLAY_VSYNC_BACK_PORCH;
  panel_config.timings.vsync_front_porch = DISPLAY_VSYNC_FRONT_PORCH;
  panel_config.timings.flags.hsync_idle_low = (DISPLAY_HSYNC_POLARITY == 0) ? 1 : 0;
  panel_config.timings.flags.vsync_idle_low = (DISPLAY_VSYNC_POLARITY == 0) ? 1 : 0;
  panel_config.timings.flags.de_idle_high = 0;
  panel_config.timings.flags.pclk_active_neg = 0;
  panel_config.timings.flags.pclk_idle_high = 0;
  panel_config.data_width = 16; // RGB565 over the parallel bus
  panel_config.sram_trans_align = 8;
  panel_config.psram_trans_align = 64;
  panel_config.hsync_gpio_num = LCD_HSYNC;
  panel_config.vsync_gpio_num = LCD_VSYNC;
  panel_config.de_gpio_num = LCD_DE;
  panel_config.pclk_gpio_num = LCD_PCLK;
  panel_config.data_gpio_nums[0] = LCD_B0;
  panel_config.data_gpio_nums[1] = LCD_B1;
  panel_config.data_gpio_nums[2] = LCD_B2;
  panel_config.data_gpio_nums[3] = LCD_B3;
  panel_config.data_gpio_nums[4] = LCD_B4;
  panel_config.data_gpio_nums[5] = LCD_G0;
  panel_config.data_gpio_nums[6] = LCD_G1;
  panel_config.data_gpio_nums[7] = LCD_G2;
  panel_config.data_gpio_nums[8] = LCD_G3;
  panel_config.data_gpio_nums[9] = LCD_G4;
  panel_config.data_gpio_nums[10] = LCD_G5;
  panel_config.data_gpio_nums[11] = LCD_R0;
  panel_config.data_gpio_nums[12] = LCD_R1;
  panel_config.data_gpio_nums[13] = LCD_R2;
  panel_config.data_gpio_nums[14] = LCD_R3;
  panel_config.data_gpio_nums[15] = LCD_R4;
  panel_config.disp_gpio_num = GPIO_NUM_NC;
  panel_config.on_frame_trans_done = on_frame_trans_done;
  panel_config.user_ctx = nullptr;
  panel_config.flags.disp_active_low = 0;
  panel_config.flags.relax_on_idle = 0;
  panel_config.flags.fb_in_psram = 1;

  if (esp_lcd_new_rgb_panel(&panel_config, &rgb_panel_handle) != ESP_OK) {
    Serial.println("[ERROR] esp_lcd_new_rgb_panel failed");
    return false;
  }
  if (esp_lcd_panel_reset(rgb_panel_handle) != ESP_OK ||
      esp_lcd_panel_init(rgb_panel_handle) != ESP_OK) {
    Serial.println("[ERROR] RGB panel reset/init failed");
    return false;
  }
  Serial.printf("[OK] RGB display: %dx%d\n", SCREEN_WIDTH, SCREEN_HEIGHT);

  esp_rgb_panel_t *rgb_panel = __containerof(rgb_panel_handle, esp_rgb_panel_t, base);
  lcd_framebuffer = (uint16_t *)rgb_panel->fb;
  Serial.println(lcd_framebuffer ? "[OK] direct framebuffer access" : "[WARN] no direct framebuffer, using slow path");

  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  if (lcd_framebuffer) {
    memset(lcd_framebuffer, 0, (size_t)SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t));
    Cache_WriteBack_Addr((uint32_t)lcd_framebuffer, (size_t)SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t));
  }

  touch_available = touch_init();
  return true;
}

static bool init_lvgl(void) {
  lv_init();

  size_t buf_size = SCREEN_WIDTH * LVGL_BUFFER_LINES * sizeof(lv_color_t);
  lvgl_buf1 = (lv_color_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  lvgl_buf2 = (lv_color_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!lvgl_buf1) {
    Serial.println("[ERROR] Failed to allocate LVGL draw buffer in PSRAM");
    return false;
  }

  lv_disp_draw_buf_init(&draw_buf, lvgl_buf1, lvgl_buf2, SCREEN_WIDTH * LVGL_BUFFER_LINES);

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCREEN_WIDTH;
  disp_drv.ver_res = SCREEN_HEIGHT;
  disp_drv.flush_cb = lvgl_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  // Back to full_refresh=1, now that lvgl_disp_flush waits for
  // on_frame_trans_done + does the cache writeback (it didn't do either
  // yet the first time this was tried, see git history). The two
  // fixes change the tradeoff: with partial refresh, a single screen
  // change (e.g. loading a menu) can produce many small flush_cb calls,
  // and EACH ONE now waits for its own fresh frame-done event — on a
  // busy screen that serializes into a slow, staggered redraw spread
  // across several frame periods, which is its own visible glitch.
  // full_refresh=1 collapses a whole screen's redraw into exactly one
  // flush call, so it waits/writes once instead of many times.
  disp_drv.full_refresh = 1;
  lv_disp_drv_register(&disp_drv);

  if (touch_available) {
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_read;
    lv_indev_drv_register(&indev_drv);
  }

  return true;
}

// Bisection switch for tracking down the rendering glitches: when 1,
// skip the whole ui_model (menus/status bar/icons/context bars) and
// show one static screen instead. Report what happens at each level
// before moving to the next — see the comment block on MINIMAL_LEVEL.
#define UI_MINIMAL_TEST 0

// 0 = solid color fill only, nothing else ever drawn or updated.
// 1 = + one static label, never updated after first paint.
// 2 = + a counter label updated once a second (tests: does *any*
//     periodic partial redraw glitch, even on an otherwise trivial UI).
// 3 = + touch: tapping anywhere increments the counter immediately
//     (tests: does touch-driven redraw glitch).
#define MINIMAL_LEVEL 3

#if UI_MINIMAL_TEST
static lv_obj_t *g_minimal_label = nullptr;
static int g_minimal_counter = 0;

static void build_minimal_ui() {
  lv_obj_t *scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x10537E), 0);

#if MINIMAL_LEVEL >= 1
  g_minimal_label = lv_label_create(scr);
  lv_label_set_text(g_minimal_label, "MINIMAL TEST");
  lv_obj_set_style_text_color(g_minimal_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(g_minimal_label, &lv_font_montserrat_32, 0);
  lv_obj_center(g_minimal_label);
#endif

#if MINIMAL_LEVEL >= 3
  lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(scr, [](lv_event_t *e) {
    g_minimal_counter++;
    lv_label_set_text_fmt(g_minimal_label, "TAP #%d", g_minimal_counter);
  }, LV_EVENT_CLICKED, nullptr);
#endif

  lv_scr_load(scr);
}
#endif

// ============================================================================
// Arduino entry points
// ============================================================================

void setup() {
  delay(1500);
  Serial.begin(115200);
  Serial.println("\n=== SenseCAP Indicator: display/touch bring-up ===");

  if (!init_display_hw()) {
    Serial.println("[FATAL] display init failed, halting");
    while (1) delay(1000);
  }
  if (!init_lvgl()) {
    Serial.println("[FATAL] LVGL init failed, halting");
    while (1) delay(1000);
  }

  wallClockSetFromBuildTime(); // "vorerst" — see wall_clock.h; settime overrides

  // BUG FIX (found live 2026-09-18): this call was simply missing on this
  // board (present on src/xiao/main.cpp, never ported over here) -- without
  // it `db` in lage_db.cpp stays nullptr forever, so every lageDbCreate/
  // lageDbUpdate/lageDbGetRecentSummaries call has been silently failing.
  // Symptom that surfaced it: Notmeldungshistorie always showing empty,
  // with no crash (SQLite quietly errors out on a null handle instead of
  // asserting) -- looked like a rendering bug, was actually "never wrote
  // anything to persist in the first place".
  if (!lageDbBegin()) {
    Serial.println("[FEHLER] lage_db konnte nicht initialisiert werden");
  }
  // lage_db.cpp is shared with src/xiao (no real clock there), so it
  // defaults to millis(). This board has wall_clock.h (set from build time
  // above, refinable via `settime`) -- use real epoch seconds instead, so
  // Notmeldungshistorie shows an actual date/time, not just uptime.
  lageDbSetTimeProvider([]() -> unsigned long {
    time_t t;
    time(&t);
    return (unsigned long)t;
  });

  // Write-only SD-card backup via the RP2040 co-processor (2026-09-21) --
  // see sd_mirror.h for why it has to go through the RP2040 at all, and
  // src/sensecap_rp2040/main.cpp for the receiving end. Best-effort: if
  // that board isn't flashed with the mirror firmware yet, or the link is
  // down, this just silently never gets acknowledged -- SPIFFS/SQLite
  // above stays the real store either way.
  sdMirrorBegin();
  lageDbSetMirrorHook(sdMirrorSend);

  // Issues #1 (Allowlist) / #3 (Ratenlimit): eingehende Lagemeldungen wurden
  // bisher von jedem Absender ungeprueft uebernommen. Sicherer Default: eine
  // leere Allowlist verwirft ALLE eingehenden Lagemeldungen, bis mindestens
  // ein Absender per "allow add <hex-node-id>" freigeschaltet wurde.
  meshSecurityInit();

#if UI_MINIMAL_TEST
  build_minimal_ui();
  Serial.printf("[OK] MINIMAL TEST level %d\n", MINIMAL_LEVEL);
#else
  ui_model_build();
#endif
  // Onboard SX1262 (external node over UART is not an option on this
  // board: GPIO22-25 are rejected as invalid by uart_set_pin, GPIO26/27
  // hang the chip solid — likely SPI flash/PSRAM lines, do NOT probe that
  // range further — and the RP2040 co-processor has no hardware path to
  // the SX1262 either, see src/sensecap/README.md section 5). Real
  // Meshtastic-protocol framing on top of the radio, see meshtastic_proto.h.
  if (lora_radio_begin() && meshtastic_proto_begin()) {
    ui_model_set_connected(true); // real signal: radio initialized and listening
    Serial.printf("[OK] Meshtastic bereit, Node !%08x\n", (unsigned)meshtastic_proto_my_node_num());
    eventLog("system", "Boot: Meshtastic-Radio bereit");
  } else {
    Serial.println("[FEHLER] Meshtastic-Radio nicht bereit, bleibe OFFLINE");
    eventLog("fehler", "Boot: Meshtastic-Radio NICHT bereit");
  }
  lvgl_last_tick = millis();
  Serial.println("[OK] setup complete");
  Serial.println("Uhrzeit stellen: settime YYYY-MM-DD HH:MM:SS");
  Serial.println("Testnachricht senden: mesh send <text>");
  Serial.println("Leitstelle fuer Notmeldungen festlegen: dispatch set <hex-node-id, z.B. ce0ffa28>");
  Serial.println("Absender-Allowlist verwalten: allow add|revoke <hex-node-id>, allow list");
  Serial.println("Heartbeat sofort senden (laeuft sonst automatisch alle 15 Minuten): test heartbeat");
  Serial.println("Lokales Ereignisprotokoll ansehen: events");
  Serial.println("Nur zum Testen (Status-UI ohne echten Zustand): testconnect on|off\n");
}

void loop() {
  unsigned long now = millis();
  lv_tick_inc(now - lvgl_last_tick);
  lvgl_last_tick = now;

  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    wallClockHandleSerialLine(line);
    // TEST-ONLY: see ui_model_set_connected's doc comment — this
    // does not simulate a real Meshtastic link, it just lets you check
    // the success screen renders correctly without the UI ever lying
    // about connectivity by default (it's OFFLINE/failed otherwise).
    if (line == "testconnect on") {
      ui_model_set_connected(true);
      Serial.println("[TEST] g_meshtastic_connected = true (nur zum Testen!)");
    } else if (line == "testconnect off") {
      ui_model_set_connected(false);
      Serial.println("[TEST] g_meshtastic_connected = false");
    } else if (line.startsWith("mesh send ")) {
      meshtastic_proto_send_text(line.substring(10).c_str());
    } else if (line.startsWith("dispatch set ")) {
      uint32_t nodeNum = strtoul(line.substring(13).c_str(), nullptr, 16);
      station_config_set_dispatch_node(nodeNum);
      Serial.printf("[OK] Leitstelle fuer Notmeldungen gesetzt: !%08x\n", (unsigned)nodeNum);
    } else if (line == "test emergency") {
      // Radio/protocol layer only -- calls meshtastic_send_emergency()
      // directly, same as a touchscreen send at the wire-protocol level
      // (direct message + ACK request on the private channel). Does NOT
      // go through ui_model's g_transmission_pending/ACK-wait state
      // machine, so it never triggers VG-numbering, the stage rows, the
      // "Notmeldung..." eventLog entries, or the success/failure screens
      // -- found live 2026-09-18 while trying to test the failure screen
      // this way and seeing nothing happen after the 8s timeout elapsed.
      // Use "test emergency ui" below for that.
      bool ok = meshtastic_send_emergency("fire_department", "test", "Serial-Testmeldung");
      Serial.printf("[TEST] meshtastic_send_emergency() -> %s (radio lokal), warte bis zu 8s auf ACK...\n",
                    ok ? "OK" : "FEHLER");
    } else if (line == "test emergency ui") {
      // The real thing: same do_trigger_emergency()/start_transmission()
      // path a touchscreen hold-confirm uses, so this actually exercises
      // VG-numbering, the stage rows, eventLog, and the success/failure
      // screens -- for testing those without physical touch access.
      ui_model_test_trigger_emergency("fire_department", "test", "UI-Testmeldung");
    } else if (line.startsWith("allow add ")) {
      uint32_t nodeNum = strtoul(line.substring(10).c_str(), nullptr, 16);
      if (meshSecurityAllow(nodeNum)) {
        Serial.printf("[SECURITY] !%08x freigeschaltet.\n", (unsigned)nodeNum);
      }
    } else if (line.startsWith("allow revoke ")) {
      uint32_t nodeNum = strtoul(line.substring(13).c_str(), nullptr, 16);
      Serial.printf("[SECURITY] !%08x %s.\n", (unsigned)nodeNum,
                    meshSecurityRevoke(nodeNum) ? "entfernt" : "war nicht auf der Allowlist");
    } else if (line == "allow list") {
      meshSecurityListAllowed();
    } else if (line == "test heartbeat") {
      meshtastic_send_heartbeat();
    } else if (line == "events") {
      // Issue #33: lokale Betriebshistorie, unabhaengig von der Leitstelle
      // einsehbar -- hier per Serial, da es noch keine eigene UI-Seite dafuer gibt.
      EventLogEntry events[20];
      int count = eventLogGetRecent(events, 20);
      Serial.printf("--- Ereignisprotokoll (%d) ---\n", count);
      for (int i = 0; i < count; i++) {
        Serial.printf("#%d [%lu] %s: %s\n", events[i].id, events[i].zeit, events[i].kategorie.c_str(),
                      events[i].text.c_str());
      }
    } else if (line == "liste") {
      // Testprotokoll F2 (2026-09-18): SenseCAP had no equivalent of
      // XIAO's "liste" for inspecting lagemeldungen directly -- flagged as
      // a gap then, needed now for debugging why Lageinformationen shows
      // nothing.
      lageDbListSummary();
    } else if (line.startsWith("liste kategorie ")) {
      lageDbListSummary(line.substring(17), "");
    } else if (line.startsWith("liste status ")) {
      lageDbListSummary("", line.substring(13));
    } else if (line.startsWith("detail ")) {
      lageDbShowDetail(line.substring(7).toInt());
    } else if (line == "sd status") {
      // Diagnostic for the SD-card mirror (2026-09-21, see sd_mirror.h) --
      // the actual writes are fire-and-forget, this is the one place that
      // waits for a reply, purely so a human can check the RP2040 link and
      // SD-card state on demand.
      bool sdOk = false;
      if (sdMirrorPing(1000, &sdOk)) {
        Serial.printf("[SD] RP2040 antwortet, SD-Karte %s\n", sdOk ? "gemountet" : "NICHT gemountet/Fehler");
      } else {
        Serial.println("[SD] Keine Antwort vom RP2040 (nicht geflasht, nicht verbunden, oder Firmware haengt)");
      }
    }
  }

  // Issue #13: periodic presence so a Leitstelle watching several stations
  // notices one going silent. Was 2 min during early testing; Testprotokoll
  // 2026-09-18 (D1/F1) found that too aggressive for EU868's duty-cycle
  // limit on a live mesh with several nodes -- raised to 15 min, the user's
  // stated minimum acceptable spacing.
  static unsigned long lastHeartbeatAt = 0;
  const unsigned long HEARTBEAT_INTERVAL_MS = 15UL * 60 * 1000;
  if (now - lastHeartbeatAt >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatAt = now;
    meshtastic_send_heartbeat();
  }

#if UI_MINIMAL_TEST
#if MINIMAL_LEVEL >= 2
  static unsigned long last_tick = 0;
  if (now - last_tick >= 1000) {
    last_tick = now;
    g_minimal_counter++;
    lv_label_set_text_fmt(g_minimal_label, "TICK %d", g_minimal_counter);
  }
#endif
#else
  ui_model_tick();
#endif
  meshtastic_proto_loop();
  lv_timer_handler();
  delay(10);
}
