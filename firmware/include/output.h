// Output driver interface: takes the finished 64x64 canvas and puts it on a
// physical display. Implementations: TftPreviewOutput (now), Hub75Output
// (when the matrix arrives). See ../../design.md "Rendering".
#pragma once

#include "canvas.h"

class DisplayOutput {
 public:
  virtual ~DisplayOutput() = default;
  virtual void begin() = 0;
  virtual void show(const uint16_t* canvas) = 0;  // CANVAS_PX RGB565 values
  virtual void showBlack() = 0;
  virtual void setBrightness(uint8_t brightness) {}
};
