#include "tft_preview_output.h"

void TftPreviewOutput::begin() {
  // Board power rails: the TFT (and I2C) are behind switchable power pins.
  pinMode(TFT_I2C_POWER, OUTPUT);
  digitalWrite(TFT_I2C_POWER, HIGH);
  pinMode(TFT_BACKLITE, OUTPUT);
  digitalWrite(TFT_BACKLITE, HIGH);
  delay(10);

  tft_.init(135, 240);
  tft_.setRotation(3);  // landscape, USB port on the left (Reverse TFT)
  tft_.setSPISpeed(40000000);
  tft_.fillScreen(ST77XX_BLACK);
}

void TftPreviewOutput::show(const uint16_t* canvas) {
  for (int y = 0; y < CANVAS_H; y++) {
    uint16_t* row0 = &scaled_[(y * kScale) * kOutSize];
    uint16_t* row1 = row0 + kOutSize;
    for (int x = 0; x < CANVAS_W; x++) {
      uint16_t v = canvas[y * CANVAS_W + x];
      row0[2 * x] = v;
      row0[2 * x + 1] = v;
    }
    memcpy(row1, row0, kOutSize * sizeof(uint16_t));
  }
  tft_.drawRGBBitmap(kOffsetX, kOffsetY, scaled_, kOutSize, kOutSize);
}

void TftPreviewOutput::showBlack() {
  tft_.fillScreen(ST77XX_BLACK);
}
