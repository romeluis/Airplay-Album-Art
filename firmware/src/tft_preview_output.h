// Letterboxed preview of the 64x64 canvas on the Reverse TFT Feather's
// built-in 240x135 ST7789: exact 2x nearest-neighbor scale to 128x128,
// centered (56px bars left/right, 3px top / 4px bottom).
#pragma once

#include <Adafruit_ST7789.h>

#include "output.h"

class TftPreviewOutput : public DisplayOutput {
 public:
  void begin() override;
  void show(const uint16_t* canvas) override;
  void showBlack() override;

 private:
  static constexpr int kScale = 2;
  static constexpr int kOutSize = CANVAS_W * kScale;  // 128
  static constexpr int kOffsetX = (240 - kOutSize) / 2;
  static constexpr int kOffsetY = (135 - kOutSize) / 2;

  Adafruit_ST7789 tft_{TFT_CS, TFT_DC, TFT_RST};
  uint16_t scaled_[kOutSize * kOutSize];
};
