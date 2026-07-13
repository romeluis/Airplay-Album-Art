#ifdef OUTPUT_HUB75

#include "hub75_output.h"

#include <Arduino.h>

static constexpr int kPanelChain = 1;
static constexpr uint8_t kBrightness = 96;

// P3(2121)64x64-32S-6.0 signal -> ESP32-S3-DevKitC-1 GPIO.
// Matrix signals occupy contiguous header pins: left rail GPIO4..GPIO18,
// right rail GPIO1..GPIO39 mirroring the HUB75E connector's right column
// (G1, G2, E, B, D, LAT). TX(43)/RX(44) stay free.
static constexpr int kR1Pin = 4;    // left rail
static constexpr int kG1Pin = 1;    // right rail
static constexpr int kB1Pin = 5;    // left rail
static constexpr int kR2Pin = 6;    // left rail
static constexpr int kG2Pin = 2;    // right rail
static constexpr int kB2Pin = 7;    // left rail
static constexpr int kAPin = 15;    // left rail
static constexpr int kBPin = 41;    // right rail
static constexpr int kCPin = 16;    // left rail
static constexpr int kDPin = 40;    // right rail; panel silkscreen may label this as GND/GRD
static constexpr int kEPin = 42;    // right rail, between G2 and B
static constexpr int kLatPin = 39;  // right rail
static constexpr int kOePin = 18;   // left rail
static constexpr int kClkPin = 17;  // left rail

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
