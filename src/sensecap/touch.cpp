#include "touch.h"
#include "io_expander.h"
#include <Wire.h>

namespace {
constexpr uint8_t FT6336U_ADDR = 0x48;
constexpr uint8_t REG_TD_STATUS = 0x02;
constexpr uint8_t REG_P1_XH = 0x03;
constexpr uint8_t REG_P2_XH = 0x09;
constexpr int16_t SCREEN_WIDTH = 480;
constexpr int16_t SCREEN_HEIGHT = 480;

// Reads one point's 4 registers (XH,XL,YH,YL) starting at `reg_base`
// (REG_P1_XH or REG_P2_XH) and returns rotated screen coordinates.
void read_point_regs(uint8_t reg_base, int16_t *x, int16_t *y) {
  uint8_t buf[4];
  Wire.beginTransmission(FT6336U_ADDR);
  Wire.write(reg_base);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)FT6336U_ADDR, (uint8_t)4);
  for (int i = 0; i < 4 && Wire.available(); i++) buf[i] = Wire.read();

  int16_t raw_x = ((buf[0] & 0x0F) << 8) | buf[1];
  int16_t raw_y = ((buf[2] & 0x0F) << 8) | buf[3];
  // 180° rotation to match the panel's mounted orientation.
  *x = SCREEN_WIDTH - 1 - raw_x;
  *y = SCREEN_HEIGHT - 1 - raw_y;
}
} // namespace

bool touch_init() {
  ioexp_set_pin_output(IOEXP_TP_RST);
  ioexp_write_pin(IOEXP_TP_RST, LOW);
  delay(10);
  ioexp_write_pin(IOEXP_TP_RST, HIGH);
  delay(300);

  Wire.beginTransmission(FT6336U_ADDR);
  if (Wire.endTransmission() == 0) {
    Serial.println("[TOUCH] FT6336U detected");
    return true;
  }
  Serial.println("[TOUCH] FT6336U NOT detected");
  return false;
}

bool touch_read_primary(int16_t *x, int16_t *y) {
  Wire.beginTransmission(FT6336U_ADDR);
  Wire.write(REG_TD_STATUS);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom((uint8_t)FT6336U_ADDR, (uint8_t)1);
  uint8_t status = Wire.available() ? Wire.read() : 0;
  uint8_t num_points = status & 0x0F;
  if (num_points == 0 || num_points > 2) return false;

  read_point_regs(REG_P1_XH, x, y);
  return true;
}

int touch_read_points(TouchPoint points[2]) {
  Wire.beginTransmission(FT6336U_ADDR);
  Wire.write(REG_TD_STATUS);
  if (Wire.endTransmission(false) != 0) return 0;
  Wire.requestFrom((uint8_t)FT6336U_ADDR, (uint8_t)1);
  uint8_t status = Wire.available() ? Wire.read() : 0;
  uint8_t num_points = status & 0x0F;
  if (num_points == 0 || num_points > 2) return 0;

  read_point_regs(REG_P1_XH, &points[0].x, &points[0].y);
  if (num_points == 2) {
    read_point_regs(REG_P2_XH, &points[1].x, &points[1].y);
  }
  return num_points;
}
