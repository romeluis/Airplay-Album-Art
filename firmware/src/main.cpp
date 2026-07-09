// AirPlay Album Art display firmware. Full system design: ../../design.md
//
// Pure listener: connects to the Mac mini service over WebSocket, receives
// state JSON frames + binary 64x64 RGB565 art frames, composes the canvas
// (art + progress bar) and hands it to the active output driver. Progress is
// interpolated locally between state frames. Disconnected or idle -> black.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <WiFi.h>

#include "audio_analyzer.h"
#include "canvas.h"
#include "mode_button.h"
#include "secrets.h"

#if defined(OUTPUT_HUB75) && defined(OUTPUT_TFT_PREVIEW)
#error "Define only one output driver: OUTPUT_HUB75 or OUTPUT_TFT_PREVIEW"
#elif defined(OUTPUT_HUB75)
#include "hub75_output.h"
#elif defined(OUTPUT_TFT_PREVIEW)
#include "tft_preview_output.h"
#else
#error "Define an output driver: OUTPUT_HUB75 or OUTPUT_TFT_PREVIEW"
#endif

static constexpr uint16_t kWhite = 0xFFFF;
static constexpr uint16_t kBlack = 0x0000;
static constexpr int kArtIdLen = 16;
static constexpr size_t kArtFrameLen = kArtIdLen + CANVAS_PX * 2;
static constexpr uint32_t kWsReconnectMs = 2000;
static constexpr uint8_t kBaseBrightness = 96;
static constexpr uint32_t kVisualizerFrameMs = 33;

#ifdef AUDIO_REACTIVE
static constexpr uint8_t kModeButtonPin = 21;
#endif

#if defined(OUTPUT_HUB75)
static Hub75Output output;
#else
static TftPreviewOutput output;
#endif
static WebSocketsClient webSocket;

#ifdef AUDIO_REACTIVE
static AudioAnalyzer audio;
static ModeButton modeButton;

enum class DisplayMode { Art, ArtBass, SoundBars };
static DisplayMode displayMode = DisplayMode::Art;

static const char* displayModeName(DisplayMode mode) {
  switch (mode) {
    case DisplayMode::Art:
      return "art";
    case DisplayMode::ArtBass:
      return "art+bass";
    case DisplayMode::SoundBars:
      return "sound bars";
  }
  return "unknown";
}
#endif

// Latest artwork received from the server.
static uint16_t artBuf[CANVAS_PX];
static char artId[kArtIdLen + 1] = "";
static bool artSeparator = false;

// Playback state from the last state frame.
static bool wsConnected = false;
static bool playing = false;
static bool artPending = false;  // server is resolving artwork -> loading animation
static double duration = 0.0;
static double basePosition = 0.0;   // position at baseMillis
static uint32_t baseMillis = 0;
static double lastRxPosition = -1;  // last received (position, ts) pair --
static double lastRxTs = -1;        // keepalives repeat it; only rebase on change

static uint16_t canvas[CANVAS_PX];
static bool needsRedraw = true;

#ifdef AUDIO_REACTIVE
static void nextDisplayMode() {
  switch (displayMode) {
    case DisplayMode::Art:
      displayMode = DisplayMode::ArtBass;
      break;
    case DisplayMode::ArtBass:
      displayMode = DisplayMode::SoundBars;
      break;
    case DisplayMode::SoundBars:
      displayMode = DisplayMode::Art;
      break;
  }
  output.setBrightness(kBaseBrightness);
  needsRedraw = true;
  Serial.printf("mode: %s\n", displayModeName(displayMode));
}
#endif

static void handleState(const uint8_t* payload, size_t length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length) != DeserializationError::Ok) {
    Serial.println("state: bad JSON");
    return;
  }
  const char* type = doc["type"] | "";
  if (strcmp(type, "state") != 0) return;

  bool nowPlaying = doc["playing"] | false;
  if (!nowPlaying) {
    if (playing) needsRedraw = true;
    playing = false;
    return;
  }

  double position = doc["position"] | 0.0;
  double ts = doc["ts"] | 0.0;
  artSeparator = doc["separator"] | false;
  artPending = doc["art_pending"] | false;
  duration = doc["duration"] | 0.0;
  // Keepalives resend the same (position, ts) sample; rebasing on those would
  // rewind the interpolated position, so only rebase on a fresh sample.
  if (!playing || position != lastRxPosition || ts != lastRxTs) {
    basePosition = position;
    baseMillis = millis();
    lastRxPosition = position;
    lastRxTs = ts;
  }
  playing = true;
  needsRedraw = true;
}

static void handleArt(const uint8_t* payload, size_t length) {
  if (length != kArtFrameLen) {
    Serial.printf("art: unexpected frame length %u\n", (unsigned)length);
    return;
  }
  memcpy(artId, payload, kArtIdLen);
  artId[kArtIdLen] = '\0';
  const uint8_t* px = payload + kArtIdLen;  // RGB565 big-endian
  for (int i = 0; i < CANVAS_PX; i++) {
    artBuf[i] = (uint16_t)(px[2 * i] << 8) | px[2 * i + 1];
  }
  Serial.printf("art: %s\n", artId);
  needsRedraw = true;
}

static void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("ws: connected");
      wsConnected = true;
      needsRedraw = true;
      break;
    case WStype_DISCONNECTED:
      if (wsConnected) Serial.println("ws: disconnected");
      wsConnected = false;
      playing = false;
      needsRedraw = true;
      break;
    case WStype_TEXT:
      handleState(payload, length);
      break;
    case WStype_BIN:
      handleArt(payload, length);
      break;
    default:
      break;
  }
}

// Interpolated playback position, clamped to [0, duration].
static double currentPosition() {
  double pos = basePosition + (millis() - baseMillis) / 1000.0;
  if (duration > 0 && pos > duration) pos = duration;
  return pos < 0 ? 0 : pos;
}

// Progress bar fill in canvas pixels (0..CANVAS_W); -1 = no bar.
static int currentFill() {
  if (duration <= 0) return -1;
  int fill = (int)lround(currentPosition() / duration * CANVAS_W);
  return constrain(fill, 0, CANVAS_W);
}

static void drawProgressBar(int fill, bool separator) {
  if (fill < 0) return;
  // Light art: black out the whole bottom 3 rows so the white bar (and its
  // empty track) stays readable, then draw the fill on top.
  if (separator) {
    memset(&canvas[(CANVAS_H - 3) * CANVAS_W], 0, 3 * CANVAS_W * sizeof(uint16_t));
  }
  for (int x = 0; x < fill; x++) {
    canvas[(CANVAS_H - 1) * CANVAS_W + x] = kWhite;
    canvas[(CANVAS_H - 2) * CANVAS_W + x] = kWhite;
  }
}

static void composeAndShow(int fill) {
  memcpy(canvas, artBuf, sizeof(canvas));
  drawProgressBar(fill, artSeparator);
  output.show(canvas);
}

// Artwork loading: Apple-style spinner — 8 gray spokes radiating from the
// center, brightness rotating counter-clockwise (head bright, fading tail).
// Progress bar stays visible.
static constexpr uint32_t kSpinnerStepMs = 100;  // full revolution ~0.8s

static constexpr uint16_t gray565(uint8_t g) {
  return (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
}

static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Spoke unit directions, clockwise from 12 o'clock.
static constexpr float kSpokeDir[8][2] = {
    {0, -1}, {0.7071f, -0.7071f}, {1, 0},  {0.7071f, 0.7071f},
    {0, 1},  {-0.7071f, 0.7071f}, {-1, 0}, {-0.7071f, -0.7071f},
};
static constexpr uint16_t kSpinnerLevels[8] = {
    gray565(255), gray565(180), gray565(125), gray565(85),
    gray565(58),  gray565(40),  gray565(26),  gray565(16),
};
static constexpr int kSpokeInner = 6;
static constexpr int kSpokeOuter = 13;
static constexpr int kSpinnerCx = CANVAS_W / 2;
static constexpr int kSpinnerCy = 30;  // nudged up to clear the progress bar

static void composeLoadingAndShow(int step, int fill) {
  memset(canvas, 0, sizeof(canvas));
  for (int i = 0; i < 8; i++) {
    // (i + step): the bright head advances counter-clockwise over time.
    uint16_t level = kSpinnerLevels[(i + step) % 8];
    for (int t = kSpokeInner; t <= kSpokeOuter; t++) {
      int x = kSpinnerCx + (int)lroundf(kSpokeDir[i][0] * t) - 1;  // 2x2 blocks
      int y = kSpinnerCy + (int)lroundf(kSpokeDir[i][1] * t) - 1;
      for (int dy = 0; dy < 2; dy++) {
        for (int dx = 0; dx < 2; dx++) {
          canvas[(y + dy) * CANVAS_W + (x + dx)] = level;
        }
      }
    }
  }
  drawProgressBar(fill, false);  // black background needs no separator
  output.show(canvas);
}

#ifdef AUDIO_REACTIVE
static uint8_t barPeaks[16] = {};

static uint16_t barColor(int y) {
  if (y < 18) return rgb565(255, 255, 255);
  if (y < 36) return rgb565(40, 210, 255);
  return rgb565(40, 255, 120);
}

static void composeBarsAndShow(int fill) {
  const AudioLevels& levels = audio.levels();
  memset(canvas, 0, sizeof(canvas));

  for (int band = 0; band < 16; band++) {
    int height = constrain((int)levels.bands[band], 0, 60);
    if (height > barPeaks[band]) {
      barPeaks[band] = height;
    } else if (barPeaks[band] > 0) {
      barPeaks[band]--;
    }

    int x0 = band * 4;
    for (int y = 0; y < height; y++) {
      int py = 60 - y;
      uint16_t color = barColor(py);
      for (int dx = 0; dx < 3; dx++) {
        canvas[py * CANVAS_W + x0 + dx] = color;
      }
    }

    if (barPeaks[band] > 0) {
      int peakY = 60 - barPeaks[band];
      if (peakY >= 0 && peakY < CANVAS_H) {
        for (int dx = 0; dx < 3; dx++) {
          canvas[peakY * CANVAS_W + x0 + dx] = kWhite;
        }
      }
    }
  }

  drawProgressBar(fill, false);
  output.show(canvas);
}
#endif

enum class Mode { Black, Loading, Art };

static void render() {
  static Mode lastMode = Mode::Art;  // force initial black paint
  static int lastFill = -2;
  static int lastStep = -1;
#ifdef AUDIO_REACTIVE
  static DisplayMode lastDisplayMode = DisplayMode::Art;
  static uint32_t lastVisualizerMs = 0;
#endif

  Mode mode = (!wsConnected || !playing) ? Mode::Black
              : (artPending ? Mode::Loading : Mode::Art);
  bool modeChanged = mode != lastMode;
  lastMode = mode;
#ifdef AUDIO_REACTIVE
  bool displayModeChanged = displayMode != lastDisplayMode;
  lastDisplayMode = displayMode;
#endif

  switch (mode) {
    case Mode::Black:
      if (modeChanged) {
        output.setBrightness(kBaseBrightness);
        output.showBlack();
      }
      needsRedraw = false;
      break;
    case Mode::Loading: {
      output.setBrightness(kBaseBrightness);
      int step = (int)((millis() / kSpinnerStepMs) % 8);
      int fill = currentFill();
      if (modeChanged || needsRedraw || step != lastStep || fill != lastFill) {
        composeLoadingAndShow(step, fill);
        lastStep = step;
        lastFill = fill;
        needsRedraw = false;
      }
      break;
    }
    case Mode::Art: {
      int fill = currentFill();
#ifdef AUDIO_REACTIVE
      if (displayMode == DisplayMode::ArtBass) {
        uint8_t boost = (uint8_t)lroundf(audio.levels().bass * 110.0f);
        output.setBrightness(constrain(kBaseBrightness + boost, kBaseBrightness, 220));
      } else {
        output.setBrightness(kBaseBrightness);
      }

      if (displayMode == DisplayMode::SoundBars) {
        uint32_t now = millis();
        if (modeChanged || displayModeChanged || needsRedraw || fill != lastFill ||
            now - lastVisualizerMs >= kVisualizerFrameMs) {
          composeBarsAndShow(fill);
          lastVisualizerMs = now;
          lastFill = fill;
          needsRedraw = false;
        }
      } else
#endif
      {
        if (modeChanged || needsRedraw || fill != lastFill
#ifdef AUDIO_REACTIVE
            || displayModeChanged
#endif
        ) {
          composeAndShow(fill);
          lastFill = fill;
          needsRedraw = false;
        }
      }
      break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  output.begin();
  output.setBrightness(kBaseBrightness);

#ifdef AUDIO_REACTIVE
  modeButton.begin(kModeButtonPin);
  if (audio.begin()) {
    Serial.println("audio: ready");
  }
#endif

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("wifi: connecting to %s", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.printf("\nwifi: connected, ip %s\n", WiFi.localIP().toString().c_str());

  webSocket.begin(SERVER_HOST, SERVER_PORT, SERVER_PATH);
  webSocket.onEvent(onWsEvent);
  webSocket.setReconnectInterval(kWsReconnectMs);
  // Detect a silently dead server (Mac mini powered off) and reconnect.
  webSocket.enableHeartbeat(15000, 3000, 2);
}

void loop() {
#ifdef AUDIO_REACTIVE
  audio.update();
  if (modeButton.update()) nextDisplayMode();
#endif
  webSocket.loop();
  render();
  delay(10);
}
