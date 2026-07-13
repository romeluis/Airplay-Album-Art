#pragma once

#include <Arduino.h>

struct AudioLevels {
  float rms = 0.0f;
  float bass = 0.0f;
  float loudness = 0.0f;  // beat pulse: rms vs recent average, 0..1
  uint8_t bands[16] = {};
};

class AudioAnalyzer {
 public:
  bool begin();
  void update();
  const AudioLevels& levels() const { return levels_; }

 private:
  static constexpr int kSampleRate = 16000;
  static constexpr int kBlockSamples = 256;
  static constexpr int kBandCount = 16;

  void analyzeBlock();
  void decay();

  bool ready_ = false;
  int sampleCount_ = 0;
  float samples_[kBlockSamples] = {};
  float dc_ = 0.0f;
  float agc_ = 0.04f;
  float avgRms_ = 0.01f;
  AudioLevels levels_;
};
