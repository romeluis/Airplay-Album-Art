#pragma once

#include <Arduino.h>

class ModeButton {
 public:
  void begin(uint8_t pin);
  bool update();

 private:
  static constexpr uint32_t kDebounceMs = 30;

  uint8_t pin_ = 0;
  bool stablePressed_ = false;
  bool lastReading_ = false;
  uint32_t lastChangeMs_ = 0;
};
