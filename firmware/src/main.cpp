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

static void composeAndShow(int fill) {
  memcpy(canvas, artBuf, sizeof(canvas));
  if (fill >= 0) {
    // Light art: black out the whole bottom 3 rows so the white bar (and its
    // empty track) stays readable, then draw the fill on top.
    if (artSeparator) {
      memset(&canvas[(CANVAS_H - 3) * CANVAS_W], 0, 3 * CANVAS_W * sizeof(uint16_t));
    }
    for (int x = 0; x < fill; x++) {
      canvas[(CANVAS_H - 1) * CANVAS_W + x] = kWhite;
      canvas[(CANVAS_H - 2) * CANVAS_W + x] = kWhite;
    }
  }
  output.show(canvas);
}

static void render() {
  static bool wasBlack = false;
  static int lastFill = -2;

  bool black = !wsConnected || !playing;
  if (black) {
    if (!wasBlack) {
      output.showBlack();
      wasBlack = true;
      lastFill = -2;
    }
    needsRedraw = false;
    return;
  }

  int fill = currentFill();
  if (needsRedraw || wasBlack || fill != lastFill) {
    composeAndShow(fill);
    wasBlack = false;
    lastFill = fill;
    needsRedraw = false;
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
