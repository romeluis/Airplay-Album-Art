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

#include "canvas.h"
#include "secrets.h"
#include "tft_preview_output.h"

static constexpr uint16_t kWhite = 0xFFFF;
static constexpr uint16_t kBlack = 0x0000;
static constexpr int kArtIdLen = 16;
static constexpr size_t kArtFrameLen = kArtIdLen + CANVAS_PX * 2;
static constexpr uint32_t kWsReconnectMs = 2000;

static TftPreviewOutput output;
static WebSocketsClient webSocket;

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

// Artwork loading: spinner of 8 gray dots on a black canvas, brightness
// rotating around the ring (head bright, fading tail). Progress bar stays
// visible.
static constexpr uint32_t kSpinnerStepMs = 100;  // full revolution ~0.8s

static constexpr uint16_t gray565(uint8_t g) {
  return (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
}

// Dot offsets from center, clockwise from 12 o'clock (radius 14).
static constexpr int8_t kSpinnerPos[8][2] = {
    {0, -14}, {10, -10}, {14, 0}, {10, 10}, {0, 14}, {-10, 10}, {-14, 0}, {-10, -10},
};
static constexpr uint16_t kSpinnerLevels[8] = {
    gray565(255), gray565(180), gray565(125), gray565(85),
    gray565(58),  gray565(40),  gray565(26),  gray565(16),
};
static constexpr int kSpinnerCx = CANVAS_W / 2;
static constexpr int kSpinnerCy = 30;  // nudged up to clear the progress bar

static void composeLoadingAndShow(int step, int fill) {
  memset(canvas, 0, sizeof(canvas));
  for (int i = 0; i < 8; i++) {
    uint16_t level = kSpinnerLevels[(i - step + 8) % 8];
    int x0 = kSpinnerCx + kSpinnerPos[i][0] - 1;  // 2x2 dots
    int y0 = kSpinnerCy + kSpinnerPos[i][1] - 1;
    for (int dy = 0; dy < 2; dy++) {
      for (int dx = 0; dx < 2; dx++) {
        canvas[(y0 + dy) * CANVAS_W + (x0 + dx)] = level;
      }
    }
  }
  drawProgressBar(fill, false);  // black background needs no separator
  output.show(canvas);
}

enum class Mode { Black, Loading, Art };

static void render() {
  static Mode lastMode = Mode::Art;  // force initial black paint
  static int lastFill = -2;
  static int lastStep = -1;

  Mode mode = (!wsConnected || !playing) ? Mode::Black
              : (artPending ? Mode::Loading : Mode::Art);
  bool modeChanged = mode != lastMode;
  lastMode = mode;

  switch (mode) {
    case Mode::Black:
      if (modeChanged) output.showBlack();
      needsRedraw = false;
      break;
    case Mode::Loading: {
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
      if (modeChanged || needsRedraw || fill != lastFill) {
        composeAndShow(fill);
        lastFill = fill;
        needsRedraw = false;
      }
      break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  output.begin();

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
  webSocket.loop();
  render();
  delay(10);
}
