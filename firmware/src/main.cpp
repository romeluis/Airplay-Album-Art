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

#ifdef AUDIO_REACTIVE
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#endif

#include "audio_analyzer.h"
#include "canvas.h"
#include "mode_button.h"
#include "secrets.h"
#include "visualizer.h"

#ifdef OUTPUT_HUB75
#include "hub75_output.h"
#else
#error "Define the output driver: OUTPUT_HUB75"
#endif

static constexpr uint16_t kWhite = 0xFFFF;
static constexpr uint16_t kBlack = 0x0000;
static constexpr int kArtIdLen = 16;
static constexpr size_t kArtFrameLen = kArtIdLen + CANVAS_PX * 2;
static constexpr uint32_t kWsReconnectMs = 2000;
static constexpr uint8_t kBaseBrightness = 96;
static constexpr uint32_t kVisualizerFrameMs = 33;

static constexpr uint16_t gray565(uint8_t g) {
  return (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
}

static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Base display brightness; web-adjustable and persisted to NVS.
static uint8_t baseBrightness = kBaseBrightness;

#ifdef AUDIO_REACTIVE
static constexpr uint8_t kArtBassMinBrightness = 24;
static constexpr uint8_t kArtBassMaxBrightness = 255;

// DevKitC-1 right rail; button to GND, INPUT_PULLUP + active-low read.
static constexpr uint8_t kModeButtonPin = 21;
#endif

static Hub75Output output;
static WebSocketsClient webSocket;

#ifdef AUDIO_REACTIVE
static AudioAnalyzer audio;
static ModeButton modeButton;
static Visualizer visualizer;

enum class DisplayMode { Art, ArtBass, SoundBars, Visualizer };
static DisplayMode displayMode = DisplayMode::Art;

static const char* displayModeName(DisplayMode mode) {
  switch (mode) {
    case DisplayMode::Art:
      return "art";
    case DisplayMode::ArtBass:
      return "art+bass";
    case DisplayMode::SoundBars:
      return "sound bars";
    case DisplayMode::Visualizer:
      return "visualizer";
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
static void setDisplayMode(DisplayMode mode) {
  displayMode = mode;
  output.setBrightness(baseBrightness);
  needsRedraw = true;
  Serial.printf("mode: %s\n", displayModeName(displayMode));
}

static void nextDisplayMode() {
  switch (displayMode) {
    case DisplayMode::Art:
      setDisplayMode(DisplayMode::ArtBass);
      break;
    case DisplayMode::ArtBass:
      setDisplayMode(DisplayMode::SoundBars);
      break;
    case DisplayMode::SoundBars:
      setDisplayMode(DisplayMode::Visualizer);
      break;
    case DisplayMode::Visualizer:
      setDisplayMode(DisplayMode::Art);
      break;
  }
}

// Sound-bar colour; web-adjustable and persisted to NVS.
static uint8_t barR = 255, barG = 120, barB = 0;
static uint16_t barColor = rgb565(255, 120, 0);

static void setBarColor(uint8_t r, uint8_t g, uint8_t b) {
  barR = r;
  barG = g;
  barB = b;
  barColor = rgb565(r, g, b);
  needsRedraw = true;
}

// --- Web UI: mode switching, brightness, bar colour + restart from a browser ---

static WebServer httpServer(80);
static Preferences prefs;

static const char kWebPage[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Album Art Display</title>
<style>
body{font-family:-apple-system,system-ui,sans-serif;background:#111;color:#eee;
display:flex;flex-direction:column;gap:12px;align-items:center;padding:32px 16px;margin:0}
h1{font-size:20px;font-weight:600;margin:0 0 12px}
button{width:min(280px,90vw);padding:14px;font-size:17px;border-radius:12px;
border:1px solid #444;background:#222;color:#eee;cursor:pointer}
button.active{background:#0a84ff;border-color:#0a84ff;font-weight:600}
button.danger{margin-top:20px;background:#2a1214;border-color:#7f1d1d;color:#f88}
label{width:min(280px,90vw);display:flex;align-items:center;gap:12px;
font-size:15px;color:#aaa;padding:4px 2px}
input[type=range]{flex:1;accent-color:#0a84ff}
input[type=color]{margin-left:auto;width:48px;height:34px;border:none;padding:0;
background:none;cursor:pointer}
</style></head><body>
<h1>Album Art Display</h1>
<button data-m="art">Art</button>
<button data-m="artbass">Art + Bass</button>
<button data-m="bars">Sound Bars</button>
<button data-m="visualizer">Visualizer</button>
<label>Brightness<input type="range" id="bri" min="8" max="255"></label>
<label>Bar colour<input type="color" id="barc"></label>
<button class="danger" id="restart">Restart device</button>
<script>
const bs=[...document.querySelectorAll('button[data-m]')];
const bri=document.getElementById('bri'),barc=document.getElementById('barc');
async function refresh(){try{const s=await(await fetch('/status')).json();
bs.forEach(b=>b.classList.toggle('active',b.dataset.m===s.mode));
if(document.activeElement!==bri)bri.value=s.brightness;
if(document.activeElement!==barc)barc.value=s.barcolor;}catch(e){}}
bs.forEach(b=>b.onclick=async()=>{await fetch('/mode?set='+b.dataset.m,{method:'POST'});refresh();});
let bt,ct;
bri.oninput=()=>{clearTimeout(bt);bt=setTimeout(()=>fetch('/brightness?set='+bri.value,{method:'POST'}),100);};
barc.oninput=()=>{clearTimeout(ct);ct=setTimeout(()=>fetch('/barcolor?set='+barc.value.slice(1),{method:'POST'}),100);};
document.getElementById('restart').onclick=async()=>{
if(confirm('Restart the display?'))await fetch('/restart',{method:'POST'});};
refresh();setInterval(refresh,3000);
</script></body></html>)HTML";

static const char* displayModeSlug(DisplayMode mode) {
  switch (mode) {
    case DisplayMode::Art:
      return "art";
    case DisplayMode::ArtBass:
      return "artbass";
    case DisplayMode::SoundBars:
      return "bars";
    case DisplayMode::Visualizer:
      return "visualizer";
  }
  return "art";
}

static bool displayModeFromSlug(const String& slug, DisplayMode& out) {
  if (slug == "art") out = DisplayMode::Art;
  else if (slug == "artbass") out = DisplayMode::ArtBass;
  else if (slug == "bars") out = DisplayMode::SoundBars;
  else if (slug == "visualizer") out = DisplayMode::Visualizer;
  else return false;
  return true;
}

static void webUiBegin() {
  httpServer.on("/", HTTP_GET, []() {
    httpServer.send_P(200, "text/html", kWebPage);
  });
  httpServer.on("/status", HTTP_GET, []() {
    char json[80];
    snprintf(json, sizeof(json),
             "{\"mode\":\"%s\",\"brightness\":%u,\"barcolor\":\"#%02x%02x%02x\"}",
             displayModeSlug(displayMode), baseBrightness, barR, barG, barB);
    httpServer.send(200, "application/json", json);
  });
  httpServer.on("/mode", HTTP_POST, []() {
    DisplayMode mode;
    if (!httpServer.hasArg("set") || !displayModeFromSlug(httpServer.arg("set"), mode)) {
      httpServer.send(400, "text/plain", "unknown mode");
      return;
    }
    setDisplayMode(mode);
    httpServer.send(200, "text/plain", "ok");
  });
  httpServer.on("/brightness", HTTP_POST, []() {
    if (!httpServer.hasArg("set")) {
      httpServer.send(400, "text/plain", "missing set");
      return;
    }
    long v = httpServer.arg("set").toInt();
    baseBrightness = (uint8_t)constrain(v, 8L, 255L);  // floor: never fully dark
    needsRedraw = true;
    prefs.putUChar("bright", baseBrightness);
    httpServer.send(200, "text/plain", "ok");
  });
  httpServer.on("/barcolor", HTTP_POST, []() {
    String hex = httpServer.arg("set");
    if (hex.startsWith("#")) hex.remove(0, 1);
    char* end = nullptr;
    long rgb = strtol(hex.c_str(), &end, 16);
    if (hex.length() != 6 || end == nullptr || *end != '\0') {
      httpServer.send(400, "text/plain", "want RRGGBB");
      return;
    }
    setBarColor((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    prefs.putUInt("barcolor", (uint32_t)rgb);
    httpServer.send(200, "text/plain", "ok");
  });
  httpServer.on("/restart", HTTP_POST, []() {
    httpServer.send(200, "text/plain", "restarting");
    delay(100);  // let the response flush before rebooting
    ESP.restart();
  });
  httpServer.begin();

  Serial.printf("web: http://%s/\n", WiFi.localIP().toString().c_str());
  if (MDNS.begin("albumart")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("web: http://albumart.local/");
  }
}

#ifdef SERVER_MDNS_NAME
static String wsHost = SERVER_HOST;

// DHCP can move the server: resolve its Bonjour name to an IP at boot (and
// again while disconnected), falling back to the static SERVER_HOST.
// Requires MDNS.begin() to have run (webUiBegin).
static String resolveServerHost() {
  IPAddress ip = MDNS.queryHost(SERVER_MDNS_NAME);
  if (ip != IPAddress()) {
    Serial.printf("ws: %s.local -> %s\n", SERVER_MDNS_NAME, ip.toString().c_str());
    return ip.toString();
  }
  Serial.printf("ws: mDNS lookup of %s.local failed, using %s\n",
                SERVER_MDNS_NAME, SERVER_HOST);
  return SERVER_HOST;
}
#endif
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
      for (int dx = 0; dx < 4; dx++) {
        canvas[py * CANVAS_W + x0 + dx] = barColor;
      }
    }

    if (barPeaks[band] > 0) {
      int peakY = 60 - barPeaks[band];
      if (peakY >= 0 && peakY < CANVAS_H) {
        for (int dx = 0; dx < 4; dx++) {
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
        output.setBrightness(baseBrightness);
        output.showBlack();
      }
      needsRedraw = false;
      break;
    case Mode::Loading: {
      output.setBrightness(baseBrightness);
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
        // Loudness-driven pulse: dim floor to full brightness, with a gamma
        // curve so quiet passages sit near the floor and hits slam to max.
        float loud = audio.levels().loudness;
        float curved = powf(constrain(loud, 0.0f, 1.0f), 1.6f);
        // Pulse range scales with the web-set base brightness (24..255 at
        // the default base of 96).
        float scale = (float)baseBrightness / kBaseBrightness;
        float lo = kArtBassMinBrightness * scale;
        float hi = min(255.0f, kArtBassMaxBrightness * scale);
        uint8_t bright = (uint8_t)lroundf(lo + curved * (hi - lo));
        output.setBrightness(bright);
      } else {
        output.setBrightness(baseBrightness);
      }

      if (displayMode == DisplayMode::SoundBars || displayMode == DisplayMode::Visualizer) {
        uint32_t now = millis();
        if (modeChanged || displayModeChanged || needsRedraw || fill != lastFill ||
            now - lastVisualizerMs >= kVisualizerFrameMs) {
          if (displayMode == DisplayMode::SoundBars) {
            composeBarsAndShow(fill);
          } else {
            visualizer.render(canvas, audio.levels());
            drawProgressBar(fill, true);  // colourful background: keep bar readable
            output.show(canvas);
          }
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

// Boot splash: WiFi symbol at center — arcs pulse upward while connecting
// (shows the device is on), all-green once connected. The normal render loop
// paints over it afterwards.
static void drawWifiSplash(int step, bool connected) {
  memset(canvas, 0, sizeof(canvas));
  constexpr float cx = 31.5f;
  constexpr int cy = 44;
  const uint16_t dim = gray565(36);
  const uint16_t lit = connected ? rgb565(60, 220, 90) : rgb565(10, 132, 255);

  for (int dy = 0; dy < 2; dy++) {  // base dot, always bright
    for (int dx = 0; dx < 2; dx++) {
      canvas[(cy + dy) * CANVAS_W + 31 + dx] = lit;
    }
  }

  int litArcs = connected ? 3 : step % 4;  // 0..3 arcs, cycling upward
  static constexpr float kArcR[3] = {7.0f, 12.0f, 17.0f};
  for (int a = 0; a < 3; a++) {
    uint16_t c = a < litArcs ? lit : dim;
    int steps = 14 + (int)(kArcR[a] * 2.0f);
    for (int s = 0; s <= steps; s++) {
      float th = -0.85f + 1.7f * s / steps;  // ~±49° off vertical
      int x = (int)lroundf(cx + kArcR[a] * sinf(th));
      int y = cy - (int)lroundf(kArcR[a] * cosf(th));
      canvas[y * CANVAS_W + x] = c;
      canvas[(y + 1) * CANVAS_W + x] = c;  // 2px thick
    }
  }
  output.show(canvas);
}

void setup() {
  Serial.begin(115200);

  // On a cold power-on the 5V rail is still settling under the panel's inrush
  // and the matrix DMA init can wedge (screen stays dark until a manual
  // reset). Self-reset once instead: a software restart with power already
  // stable behaves exactly like pressing the reset button.
  if (esp_reset_reason() == ESP_RST_POWERON) {
    Serial.println("boot: cold power-on, settling then self-resetting");
    delay(500);
    esp_restart();
  }

#ifdef AUDIO_REACTIVE
  // Web-adjustable settings persisted in NVS; loaded before the first paint.
  prefs.begin("display", false);
  baseBrightness = prefs.getUChar("bright", kBaseBrightness);
  uint32_t rgb = prefs.getUInt("barcolor", 0xFF7800UL);
  setBarColor((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
#endif

  output.begin();
  output.setBrightness(baseBrightness);

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
  int splashStep = 0;
  while (WiFi.status() != WL_CONNECTED) {
    drawWifiSplash(splashStep++, false);
    delay(250);
    Serial.print(".");
  }
  Serial.printf("\nwifi: connected, ip %s\n", WiFi.localIP().toString().c_str());
  drawWifiSplash(0, true);  // brief green confirmation, then normal display
  delay(600);
  output.showBlack();

#ifdef AUDIO_REACTIVE
  webUiBegin();
#endif

#if defined(AUDIO_REACTIVE) && defined(SERVER_MDNS_NAME)
  wsHost = resolveServerHost();
  webSocket.begin(wsHost, SERVER_PORT, SERVER_PATH);
#else
  webSocket.begin(SERVER_HOST, SERVER_PORT, SERVER_PATH);
#endif
  webSocket.onEvent(onWsEvent);
  webSocket.setReconnectInterval(kWsReconnectMs);
  // Detect a silently dead server (Mac mini powered off) and reconnect.
  webSocket.enableHeartbeat(15000, 3000, 2);
}

void loop() {
#ifdef AUDIO_REACTIVE
  audio.update();
  if (modeButton.update()) nextDisplayMode();
  httpServer.handleClient();
#ifdef SERVER_MDNS_NAME
  // Server may have rebooted onto a new IP: re-resolve after a minute of
  // failed reconnects.
  static uint32_t lastResolveMs = 0;
  if (!wsConnected && millis() - lastResolveMs > 60000) {
    lastResolveMs = millis();
    String host = resolveServerHost();
    if (host != wsHost) {
      wsHost = host;
      webSocket.begin(wsHost, SERVER_PORT, SERVER_PATH);
    }
  }
#endif
#endif
  webSocket.loop();
  render();
  delay(10);
}
