#pragma once
#include <Arduino.h>

// FT6336U capacitive touch controller (I2C). Coordinates returned by both
// functions below are already rotated 180° to match the panel's mounted
// orientation (see main.cpp's lvgl_disp_flush comment) — callers work in
// the same coordinate space LVGL/the visible screen uses.

struct TouchPoint {
  int16_t x;
  int16_t y;
};

bool touch_init();

// Single point, for LVGL's indev (tap/click driven UI).
bool touch_read_primary(int16_t *x, int16_t *y);

// Up to 2 simultaneous points, for the hold_confirm two-finger gesture.
// Returns how many points are currently pressed (0-2), filling `points`.
int touch_read_points(TouchPoint points[2]);
