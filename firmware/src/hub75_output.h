// Native 1:1 output for a 64x64 HUB75E RGB LED matrix on the
// ESP32-S3-DevKitC-1 (pin map in hub75_output.cpp).
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
