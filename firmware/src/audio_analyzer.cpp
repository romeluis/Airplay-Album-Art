#include "audio_analyzer.h"

#ifdef AUDIO_REACTIVE

#include <driver/i2s.h>
#include <math.h>

static constexpr i2s_port_t kI2sPort = I2S_NUM_1;
// INMP441 on three physically adjacent DevKitC-1 left-rail pins (GPIO9/10/11,
// below the matrix block; GPIO8 and the strapping pins GPIO3/GPIO46 in between
// are left free). VDD -> 3V3, GND -> GND, L/R -> GND.
static constexpr int kMicSckPin = 9;
static constexpr int kMicWsPin = 10;
static constexpr int kMicSdPin = 11;

static constexpr float kPi = 3.14159265358979323846f;
static constexpr float kBandHz[16] = {
    63.0f,  100.0f, 160.0f, 250.0f, 400.0f, 630.0f, 900.0f, 1250.0f,
    1700.0f, 2300.0f, 3000.0f, 3800.0f, 4800.0f, 6000.0f, 7000.0f, 7800.0f,
};

bool AudioAnalyzer::begin() {
  i2s_config_t config = {};
  config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = kSampleRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 4;
  config.dma_buf_len = 128;
  config.use_apll = false;
  config.tx_desc_auto_clear = false;
  config.fixed_mclk = 0;
  config.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  config.bits_per_chan = I2S_BITS_PER_CHAN_32BIT;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = kMicSckPin;
  pins.ws_io_num = kMicWsPin;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = kMicSdPin;

  if (i2s_driver_install(kI2sPort, &config, 0, nullptr) != ESP_OK) {
    Serial.println("audio: i2s driver install failed");
    return false;
  }
  if (i2s_set_pin(kI2sPort, &pins) != ESP_OK) {
    Serial.println("audio: i2s pin setup failed");
    i2s_driver_uninstall(kI2sPort);
    return false;
  }
  i2s_zero_dma_buffer(kI2sPort);
  ready_ = true;
  return true;
}

void AudioAnalyzer::update() {
  if (!ready_) {
    decay();
    return;
  }

  int32_t raw[64];
  size_t bytesRead = 0;
  esp_err_t err = i2s_read(kI2sPort, raw, sizeof(raw), &bytesRead, 0);
  if (err != ESP_OK || bytesRead == 0) {
    decay();
    return;
  }

  int count = bytesRead / sizeof(raw[0]);
  for (int i = 0; i < count; i++) {
    float sample = raw[i] / 2147483648.0f;
    dc_ += (sample - dc_) * 0.001f;
    samples_[sampleCount_++] = sample - dc_;
    if (sampleCount_ >= kBlockSamples) {
      analyzeBlock();
      sampleCount_ = 0;
    }
  }
}

void AudioAnalyzer::analyzeBlock() {
  float rmsSum = 0.0f;
  for (int i = 0; i < kBlockSamples; i++) {
    rmsSum += samples_[i] * samples_[i];
  }

  float rms = sqrtf(rmsSum / kBlockSamples);
  levels_.rms = levels_.rms * 0.82f + rms * 0.18f;

  // Beat emphasis: instantaneous rms against its slow running average.
  // Compressed music keeps rms pinned near its peak, so peak-relative
  // loudness barely moves — the ratio against the average still spikes on
  // hits. Instant attack, exponential decay (~0.5s) between beats.
  avgRms_ = avgRms_ * 0.99f + rms * 0.01f;
  float ratio = rms / fmaxf(avgRms_, 0.0015f);
  float pulse = rms > 0.0015f ? constrain((ratio - 0.85f) / 0.9f, 0.0f, 1.0f) : 0.0f;
  levels_.loudness = fmaxf(pulse, levels_.loudness * 0.88f);

  float magnitudes[kBandCount];
  float maxMag = 0.0f;
  for (int band = 0; band < kBandCount; band++) {
    float k = 0.5f + (kBlockSamples * kBandHz[band]) / kSampleRate;
    float omega = (2.0f * kPi * k) / kBlockSamples;
    float coeff = 2.0f * cosf(omega);
    float q0 = 0.0f;
    float q1 = 0.0f;
    float q2 = 0.0f;
    for (int i = 0; i < kBlockSamples; i++) {
      q0 = coeff * q1 - q2 + samples_[i];
      q2 = q1;
      q1 = q0;
    }
    float power = q1 * q1 + q2 * q2 - q1 * q2 * coeff;
    float mag = sqrtf(fmaxf(power, 0.0f)) / kBlockSamples;
    magnitudes[band] = mag;
    if (mag > maxMag) maxMag = mag;
  }

  agc_ = fmaxf(maxMag, agc_ * 0.96f);
  if (agc_ < 0.02f) agc_ = 0.02f;

  float bass = (magnitudes[0] + magnitudes[1] + magnitudes[2] + magnitudes[3]) / (4.0f * agc_);
  bass = constrain(bass, 0.0f, 1.0f);
  levels_.bass = levels_.bass * 0.72f + bass * 0.28f;

  bool gated = levels_.rms > 0.0015f;
  for (int band = 0; band < kBandCount; band++) {
    float normalized = gated ? magnitudes[band] / (agc_ * 0.8f) : 0.0f;
    normalized = powf(constrain(normalized, 0.0f, 1.0f), 0.65f);
    uint8_t target = (uint8_t)lroundf(normalized * 63.0f);
    uint8_t current = levels_.bands[band];
    levels_.bands[band] = target > current ? target : (uint8_t)max(0, current - 3);
  }
}

void AudioAnalyzer::decay() {
  levels_.rms *= 0.96f;
  levels_.bass *= 0.94f;
  levels_.loudness *= 0.92f;
  for (int i = 0; i < kBandCount; i++) {
    levels_.bands[i] = levels_.bands[i] > 0 ? levels_.bands[i] - 1 : 0;
  }
}

#else

bool AudioAnalyzer::begin() {
  return false;
}

void AudioAnalyzer::update() {
  decay();
}

void AudioAnalyzer::analyzeBlock() {}

void AudioAnalyzer::decay() {
  levels_.rms = 0.0f;
  levels_.bass = 0.0f;
  levels_.loudness = 0.0f;
  memset(levels_.bands, 0, sizeof(levels_.bands));
}

#endif
