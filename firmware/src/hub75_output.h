// Native 1:1 output for a 64x64 HUB75E RGB LED matrix. The default pin map is
// for the Adafruit Feather ESP32-S3 Reverse TFT headers, with the built-in TFT
// unused.
#pragma once

#include "output.h"

#ifdef OUTPUT_HUB75
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

class Hub75Output : public DisplayOutput {
 public:
  Hub75Output();

  void begin() override;
  void show(const uint16_t* canvas) override;
  void showBlack() override;
  void setBrightness(uint8_t brightness) override;

 private:
  MatrixPanel_I2S_DMA matrix_;
};
#endif
