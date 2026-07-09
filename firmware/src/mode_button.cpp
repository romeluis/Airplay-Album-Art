#include "mode_button.h"

void ModeButton::begin(uint8_t pin) {
  pin_ = pin;
  pinMode(pin_, INPUT_PULLUP);
  stablePressed_ = digitalRead(pin_) == LOW;
  lastReading_ = stablePressed_;
  lastChangeMs_ = millis();
}

bool ModeButton::update() {
  bool reading = digitalRead(pin_) == LOW;
  uint32_t now = millis();

  if (reading != lastReading_) {
    lastReading_ = reading;
    lastChangeMs_ = now;
  }

  if (now - lastChangeMs_ < kDebounceMs || reading == stablePressed_) {
    return false;
  }

  stablePressed_ = reading;
  return stablePressed_;
}
