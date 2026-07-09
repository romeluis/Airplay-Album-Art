#ifdef OUTPUT_HUB75

#include "hub75_output.h"

#include <Arduino.h>

static constexpr int kPanelChain = 1;
static constexpr uint8_t kBrightness = 96;

#if defined(HUB75_PINMAP_S3_DEVKIT)
// P3(2121)64x64-32S-6.0 signal -> ESP32-S3 dev board GPIO.
static constexpr int kR1Pin = 4;
static constexpr int kG1Pin = 5;
static constexpr int kB1Pin = 6;
static constexpr int kR2Pin = 7;
static constexpr int kG2Pin = 15;
static constexpr int kB2Pin = 16;
static constexpr int kAPin = 17;
static constexpr int kBPin = 18;
static constexpr int kCPin = 8;
static constexpr int kDPin = 9;    // panel silkscreen may label this as GND/GRD
static constexpr int kEPin = 10;
static constexpr int kLatPin = 11;
static constexpr int kOePin = 12;
static constexpr int kClkPin = 13;
#else
// P3(2121)64x64-32S-6.0 signal -> Feather ESP32-S3 Reverse TFT GPIO/header label.
static constexpr int kR1Pin = 5;    // D5
static constexpr int kG1Pin = 6;    // D6
static constexpr int kB1Pin = 9;    // D9
static constexpr int kR2Pin = 10;   // D10
static constexpr int kG2Pin = 11;   // D11
static constexpr int kB2Pin = 12;   // D12
static constexpr int kAPin = 13;    // D13
static constexpr int kBPin = 14;    // A4
static constexpr int kCPin = 15;    // A3
static constexpr int kDPin = 16;    // A2; panel silkscreen may label this as GND/GRD
static constexpr int kEPin = 17;    // A1
static constexpr int kLatPin = 18;  // A0
static constexpr int kOePin = 38;   // RX
static constexpr int kClkPin = 39;  // TX
#endif

static HUB75_I2S_CFG makeMatrixConfig() {
  HUB75_I2S_CFG::i2s_pins pins = {
      kR1Pin,  kG1Pin, kB1Pin, kR2Pin,  kG2Pin, kB2Pin, kAPin,
      kBPin,   kCPin,  kDPin,  kEPin,   kLatPin, kOePin, kClkPin,
  };

  HUB75_I2S_CFG config(CANVAS_W, CANVAS_H, kPanelChain, pins);
  config.double_buff = false;
  config.i2sspeed = HUB75_I2S_CFG::HZ_8M;
  config.min_refresh_rate = 60;
  config.setPixelColorDepthBits(8);
  return config;
}

Hub75Output::Hub75Output() : matrix_(makeMatrixConfig()) {}

void Hub75Output::begin() {
#ifdef TFT_CS
  pinMode(TFT_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
#endif
#ifdef TFT_BACKLITE
  pinMode(TFT_BACKLITE, OUTPUT);
  digitalWrite(TFT_BACKLITE, LOW);
#endif
#ifdef TFT_I2C_POWER
  pinMode(TFT_I2C_POWER, OUTPUT);
  digitalWrite(TFT_I2C_POWER, LOW);
#endif

  if (!matrix_.begin()) {
    Serial.println("hub75: matrix begin failed");
    return;
  }

  matrix_.setBrightness8(kBrightness);
  matrix_.clearScreen();
}

void Hub75Output::show(const uint16_t* canvas) {
  for (int y = 0; y < CANVAS_H; y++) {
    for (int x = 0; x < CANVAS_W; x++) {
      matrix_.drawPixel(x, y, canvas[y * CANVAS_W + x]);
    }
  }
}

void Hub75Output::showBlack() {
  matrix_.clearScreen();
}

void Hub75Output::setBrightness(uint8_t brightness) {
  matrix_.setBrightness8(brightness);
}

#endif
