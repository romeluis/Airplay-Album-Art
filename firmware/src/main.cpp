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

// Visualizer mode: iTunes-style particle swirl. Glowing particles orbit a
// slowly wandering attractor, leaving fading light trails (the canvas decays
// toward black each frame instead of clearing). A bass-driven core pulses at
// the center, beats fling the swarm outward in radial bursts (occasionally
// reversing the swirl direction), and the palette hue drifts continuously.

// Full-saturation HSV -> RGB565.
static uint16_t hsv565(uint8_t h, uint8_t v) {
  uint8_t region = h / 43;
  uint8_t rem = (uint8_t)((h - region * 43) * 6);
  uint8_t q = (uint8_t)((v * (255 - rem)) >> 8);
  uint8_t t = (uint8_t)((v * rem) >> 8);
  switch (region) {
    case 0: return rgb565(v, t, 0);
    case 1: return rgb565(q, v, 0);
    case 2: return rgb565(0, v, t);
    case 3: return rgb565(0, q, v);
    case 4: return rgb565(t, 0, v);
    default: return rgb565(v, 0, q);
  }
}

static constexpr int kParticleCount = 48;
struct Particle {
  float x, y, vx, vy;
  uint8_t hueOffset;
};
static Particle particles[kParticleCount];
static float visHueBase = 0.0f;
static float visSwirl = 1.0f;
static uint32_t visLastFrameMs = 0;
static uint32_t visLastBurstMs = 0;

// Per-frame trail decay (~0.81x): a particle's wake glows for ~1/4 second.
static inline uint16_t fade565(uint16_t c) {
  uint16_t r = (uint16_t)((((c >> 11) & 0x1F) * 13) >> 4);
  uint16_t g = (uint16_t)((((c >> 5) & 0x3F) * 13) >> 4);
  uint16_t b = (uint16_t)(((c & 0x1F) * 13) >> 4);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// Channel-wise max: overlapping glows brighten instead of stomping each other.
static inline void plotMax(int x, int y, uint16_t c) {
  if (x < 0 || x >= CANVAS_W || y < 0 || y >= CANVAS_H) return;
  uint16_t& px = canvas[y * CANVAS_W + x];
  px = (uint16_t)(max(px & 0xF800, c & 0xF800) | max(px & 0x07E0, c & 0x07E0) |
                  max(px & 0x001F, c & 0x001F));
}

static void spawnParticle(Particle& p, float cx, float cy) {
  float ang = random(0, 628) / 100.0f;
  float rad = 4.0f + random(0, 180) / 10.0f;
  p.x = cx + cosf(ang) * rad;
  p.y = cy + sinf(ang) * rad;
  float speed = 0.3f + random(0, 60) / 100.0f;  // tangential launch
  p.vx = -sinf(ang) * speed * visSwirl;
  p.vy = cosf(ang) * speed * visSwirl;
  p.hueOffset = (uint8_t)random(0, 64);
}

static void composeVisualizerAndShow(int fill) {
  const AudioLevels& levels = audio.levels();
  uint32_t now = millis();
  bool restart = now - visLastFrameMs > 500;  // mode just (re)entered
  visLastFrameMs = now;

  // Attractor wanders on a slow Lissajous path so the swarm's home drifts.
  float tsec = now / 1000.0f;
  float cx = 31.5f + 9.0f * sinf(tsec * 0.31f);
  float cy = 29.0f + 7.0f * sinf(tsec * 0.23f + 1.7f);

  if (restart) {
    memset(canvas, 0, sizeof(canvas));
    for (int i = 0; i < kParticleCount; i++) spawnParticle(particles[i], cx, cy);
  } else {
    for (int i = 0; i < CANVAS_PX; i++) {
      if (canvas[i]) canvas[i] = fade565(canvas[i]);
    }
  }

  visHueBase += 0.25f + levels.loudness * 1.5f;
  if (visHueBase >= 256.0f) visHueBase -= 256.0f;
  uint8_t hueBase = (uint8_t)visHueBase;

  bool burst = levels.loudness > 0.72f && now - visLastBurstMs > 220;
  if (burst) {
    visLastBurstMs = now;
    if (random(0, 4) == 0) visSwirl = -visSwirl;
  }

  float pull = 0.050f + 0.045f * levels.bass;
  float swirl = visSwirl * (0.10f + 0.14f * levels.loudness);

  for (int i = 0; i < kParticleCount; i++) {
    Particle& p = particles[i];
    float dx = cx - p.x;
    float dy = cy - p.y;
    float dist = sqrtf(dx * dx + dy * dy) + 0.01f;
    float nx = dx / dist;
    float ny = dy / dist;
    p.vx += nx * pull - ny * swirl;
    p.vy += ny * pull + nx * swirl;
    if (burst) {  // radial kick away from the center
      p.vx -= nx * (1.2f + random(0, 80) / 100.0f);
      p.vy -= ny * (1.2f + random(0, 80) / 100.0f);
    }
    p.vx *= 0.965f;
    p.vy *= 0.965f;
    p.x += p.vx;
    p.y += p.vy;

    if (p.x < -6 || p.x > CANVAS_W + 6 || p.y < -6 || p.y > CANVAS_H + 6) {
      spawnParticle(p, cx, cy);
      continue;
    }

    // Faster particles glow brighter.
    float speed = sqrtf(p.vx * p.vx + p.vy * p.vy);
    uint8_t v = (uint8_t)constrain(120.0f + speed * 220.0f, 0.0f, 255.0f);
    uint8_t hue = (uint8_t)(hueBase + p.hueOffset);
    uint16_t bright = hsv565(hue, v);
    uint16_t dim = hsv565(hue, v >> 1);
    int px = (int)lroundf(p.x);
    int py = (int)lroundf(p.y);
    plotMax(px, py, bright);
    plotMax(px - 1, py, dim);
    plotMax(px + 1, py, dim);
    plotMax(px, py - 1, dim);
    plotMax(px, py + 1, dim);
  }

  // Bass core: pulsing glow riding the attractor.
  int coreR = 1 + (int)lroundf(levels.bass * 5.0f);
  for (int dy = -coreR; dy <= coreR; dy++) {
    for (int dx = -coreR; dx <= coreR; dx++) {
      int d2 = dx * dx + dy * dy;
      if (d2 > coreR * coreR) continue;
      uint16_t c = d2 <= 1 ? kWhite
                           : hsv565(hueBase, (uint8_t)(255 - (200 * d2) / (coreR * coreR + 1)));
      plotMax((int)cx + dx, (int)cy + dy, c);
    }
  }

  drawProgressBar(fill, true);  // colourful background: keep the bar readable
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
            composeVisualizerAndShow(fill);
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
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.printf("\nwifi: connected, ip %s\n", WiFi.localIP().toString().c_str());

#ifdef AUDIO_REACTIVE
  webUiBegin();
#endif

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
  httpServer.handleClient();
#endif
  webSocket.loop();
  render();
  delay(10);
}
