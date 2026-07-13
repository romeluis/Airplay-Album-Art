#include "visualizer.h"

#ifdef AUDIO_REACTIVE

#include <math.h>

#include "canvas.h"

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

static constexpr float kPi = 3.14159265358979323846f;
static constexpr uint16_t kWhite = 0xFFFF;

static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

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

// Per-frame trail decay (~0.81x): a wake glows for ~1/4 second at 30fps.
static inline uint16_t fade565(uint16_t c) {
  uint16_t r = (uint16_t)((((c >> 11) & 0x1F) * 13) >> 4);
  uint16_t g = (uint16_t)((((c >> 5) & 0x3F) * 13) >> 4);
  uint16_t b = (uint16_t)(((c & 0x1F) * 13) >> 4);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// Canvas being composed this frame (set at Visualizer::render entry).
static uint16_t* cv = nullptr;

// Channel-wise max: overlapping glows brighten instead of stomping each other.
static inline void plotMax(int x, int y, uint16_t c) {
  if (x < 0 || x >= CANVAS_W || y < 0 || y >= CANVAS_H) return;
  uint16_t& px = cv[y * CANVAS_W + x];
  px = (uint16_t)(max(px & 0xF800, c & 0xF800) | max(px & 0x07E0, c & 0x07E0) |
                  max(px & 0x001F, c & 0x001F));
}

static void decayCanvas() {
  for (int i = 0; i < CANVAS_PX; i++) {
    if (cv[i]) cv[i] = fade565(cv[i]);
  }
}

// Band level clamped to the analyzer's nominal 0..60 range.
static inline int bandLevel(const AudioLevels& levels, int band) {
  int v = levels.bands[band];
  return v > 60 ? 60 : v;
}

// Bass core: pulsing glow at (cx, cy) — shared by swirl and starfield.
static void drawBassCore(float cx, float cy, float bass, uint8_t hue) {
  int coreR = 1 + (int)lroundf(bass * 5.0f);
  for (int dy = -coreR; dy <= coreR; dy++) {
    for (int dx = -coreR; dx <= coreR; dx++) {
      int d2 = dx * dx + dy * dy;
      if (d2 > coreR * coreR) continue;
      uint16_t c = d2 <= 1 ? kWhite
                           : hsv565(hue, (uint8_t)(255 - (200 * d2) / (coreR * coreR + 1)));
      plotMax((int)cx + dx, (int)cy + dy, c);
    }
  }
}

// Per-frame context handed to every effect.
struct FrameCtx {
  const AudioLevels& levels;
  uint32_t now;
  bool beat;
  uint8_t hue;  // shared drifting palette base
};

// ---------------------------------------------------------------------------
// Lookup tables (built once on first render)
// ---------------------------------------------------------------------------

static int8_t sinLut[256];          // sin scaled to -127..127
static uint8_t plasmaRad[CANVAS_PX];  // distance from center * 5, saturated
static uint8_t kalBand[32 * 32];    // kaleidoscope quadrant: radius -> band 0..15
static uint8_t kalAng[32 * 32];     // kaleidoscope quadrant: angle 0..255

static void buildLuts() {
  for (int i = 0; i < 256; i++) {
    sinLut[i] = (int8_t)lroundf(127.0f * sinf(i * 2.0f * kPi / 256.0f));
  }
  for (int y = 0; y < CANVAS_H; y++) {
    for (int x = 0; x < CANVAS_W; x++) {
      float r = hypotf(x - 31.5f, y - 31.5f);
      plasmaRad[y * CANVAS_W + x] = (uint8_t)min(255L, lroundf(r * 5.0f));
    }
  }
  // Top-left quadrant; the other three are mirrored at draw time.
  for (int y = 0; y < 32; y++) {
    for (int x = 0; x < 32; x++) {
      float dx = 31.5f - x;
      float dy = 31.5f - y;
      float r = hypotf(dx, dy);  // 0.7 .. 44.5
      int band = (int)((r - 2.0f) * 16.0f / 38.0f);
      kalBand[y * 32 + x] = (uint8_t)constrain(band, 0, 15);
      kalAng[y * 32 + x] = (uint8_t)lroundf(atan2f(dy, dx) * (255.0f / (kPi / 2.0f)));
    }
  }
}

// ---------------------------------------------------------------------------
// Effect 0: particle swirl — glowing particles orbit a wandering attractor,
// beats fling the swarm outward (occasionally reversing the swirl direction).
// ---------------------------------------------------------------------------

static constexpr int kParticleCount = 48;
struct Particle {
  float x, y, vx, vy;
  uint8_t hueOffset;
};
static Particle particles[kParticleCount];
static float swirlDir = 1.0f;

// Attractor wanders on a slow Lissajous path so the swarm's home drifts.
static void swirlCenter(uint32_t now, float& cx, float& cy) {
  float tsec = now / 1000.0f;
  cx = 31.5f + 9.0f * sinf(tsec * 0.31f);
  cy = 29.0f + 7.0f * sinf(tsec * 0.23f + 1.7f);
}

static void spawnParticle(Particle& p, float cx, float cy) {
  float ang = random(0, 628) / 100.0f;
  float rad = 4.0f + random(0, 180) / 10.0f;
  p.x = cx + cosf(ang) * rad;
  p.y = cy + sinf(ang) * rad;
  float speed = 0.3f + random(0, 60) / 100.0f;  // tangential launch
  p.vx = -sinf(ang) * speed * swirlDir;
  p.vy = cosf(ang) * speed * swirlDir;
  p.hueOffset = (uint8_t)random(0, 64);
}

static void swirlInit(uint32_t now) {
  float cx, cy;
  swirlCenter(now, cx, cy);
  for (int i = 0; i < kParticleCount; i++) spawnParticle(particles[i], cx, cy);
}

static void fxSwirl(const FrameCtx& ctx) {
  decayCanvas();
  float cx, cy;
  swirlCenter(ctx.now, cx, cy);

  if (ctx.beat && random(0, 4) == 0) swirlDir = -swirlDir;

  float pull = 0.050f + 0.045f * ctx.levels.bass;
  float swirl = swirlDir * (0.10f + 0.14f * ctx.levels.loudness);

  for (int i = 0; i < kParticleCount; i++) {
    Particle& p = particles[i];
    float dx = cx - p.x;
    float dy = cy - p.y;
    float dist = sqrtf(dx * dx + dy * dy) + 0.01f;
    float nx = dx / dist;
    float ny = dy / dist;
    p.vx += nx * pull - ny * swirl;
    p.vy += ny * pull + nx * swirl;
    if (ctx.beat) {  // radial kick away from the center
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
    uint8_t hue = (uint8_t)(ctx.hue + p.hueOffset);
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

  drawBassCore(cx, cy, ctx.levels.bass, ctx.hue);
}

// ---------------------------------------------------------------------------
// Effect 1: radial spectrum — 16 bands as 32 mirrored spokes radiating from
// the center; the whole fan rotates faster with loudness.
// ---------------------------------------------------------------------------

static float fanAngle = 0.0f;

static void fxRadial(const FrameCtx& ctx) {
  decayCanvas();
  fanAngle += 0.01f + 0.05f * ctx.levels.loudness;
  for (int s = 0; s < 32; s++) {
    int band = s < 16 ? s : 31 - s;  // mirror for symmetry
    int level = bandLevel(ctx.levels, band);
    float len = 4.0f + level * 25.0f / 60.0f;
    float ang = fanAngle + s * (2.0f * kPi / 32.0f);
    float ca = cosf(ang);
    float sa = sinf(ang);
    uint8_t hue = (uint8_t)(ctx.hue + band * 8);
    for (float t = 3.0f; t <= len; t += 1.0f) {
      uint8_t v = (uint8_t)(110 + (int)(145.0f * t / len));  // bright tip
      int x = (int)lroundf(31.5f + ca * t);
      int y = (int)lroundf(31.5f + sa * t);
      plotMax(x, y, hsv565(hue, v));
      plotMax(x + (sa > 0 ? -1 : 1), y, hsv565(hue, v >> 1));  // widen inward
    }
  }
}

// ---------------------------------------------------------------------------
// Effect 2: beat ripples — expanding colored rings launched on each beat,
// thickness driven by bass, over a pulsing center glow.
// ---------------------------------------------------------------------------

struct Ring {
  float r;
  float intensity;
  uint8_t hue;
  bool active;
};
static Ring rings[6];

static void ripplesInit() {
  for (Ring& ring : rings) ring.active = false;
}

static void fxRipples(const FrameCtx& ctx) {
  decayCanvas();

  if (ctx.beat) {
    // Reuse the weakest slot so a busy song can't starve new rings.
    Ring* slot = &rings[0];
    for (Ring& ring : rings) {
      if (!ring.active) { slot = &ring; break; }
      if (ring.intensity < slot->intensity) slot = &ring;
    }
    slot->active = true;
    slot->r = 2.0f;
    slot->intensity = 1.0f;
    slot->hue = (uint8_t)(ctx.hue + random(0, 48));
  }

  bool thick = ctx.levels.bass > 0.4f;
  for (Ring& ring : rings) {
    if (!ring.active) continue;
    ring.r += 0.8f + ctx.levels.loudness;
    ring.intensity *= 0.94f;
    if (ring.r > 46.0f || ring.intensity < 0.08f) {
      ring.active = false;
      continue;
    }
    uint8_t v = (uint8_t)(255.0f * ring.intensity);
    uint16_t c = hsv565(ring.hue, v);
    uint16_t cDim = hsv565(ring.hue, v >> 1);
    int steps = min(256, 10 + (int)(6.3f * ring.r));
    for (int s = 0; s < steps; s++) {
      float a = s * 2.0f * kPi / steps;
      float ca = cosf(a);
      float sa = sinf(a);
      plotMax((int)lroundf(31.5f + ca * ring.r), (int)lroundf(31.5f + sa * ring.r), c);
      if (thick) {
        plotMax((int)lroundf(31.5f + ca * (ring.r - 1.0f)),
                (int)lroundf(31.5f + sa * (ring.r - 1.0f)), cDim);
      }
    }
  }

  // Center glow keeps the effect alive through quiet passages.
  uint8_t glow = (uint8_t)(70 + ctx.levels.loudness * 150.0f);
  drawBassCore(31.5f, 31.5f, 0.2f + ctx.levels.bass * 0.5f, ctx.hue);
  plotMax(31, 31, hsv565(ctx.hue, glow));
  plotMax(32, 31, hsv565(ctx.hue, glow));
  plotMax(31, 32, hsv565(ctx.hue, glow));
  plotMax(32, 32, hsv565(ctx.hue, glow));
}

// ---------------------------------------------------------------------------
// Effect 3: starfield warp — stars fly outward with perspective; loudness is
// the warp speed, trails come from canvas decay, bass pulses the core.
// ---------------------------------------------------------------------------

struct Star {
  float dx, dy;  // unit direction from center
  float z;       // depth 1 (far) -> 0 (past the viewer)
  uint8_t hueOffset;
};
static constexpr int kStarCount = 48;
static Star stars[kStarCount];

static void spawnStar(Star& s) {
  float ang = random(0, 628) / 100.0f;
  s.dx = cosf(ang);
  s.dy = sinf(ang);
  s.z = 0.25f + random(0, 75) / 100.0f;
  s.hueOffset = (uint8_t)random(0, 80);
}

static void starfieldInit() {
  for (Star& s : stars) spawnStar(s);
}

static void fxStarfield(const FrameCtx& ctx) {
  decayCanvas();
  float speed = 0.004f + 0.022f * ctx.levels.loudness;
  for (Star& s : stars) {
    s.z -= speed;
    float r = 2.5f / s.z - 2.5f;  // perspective projection
    if (s.z <= 0.06f || r > 45.0f) {
      spawnStar(s);
      continue;
    }
    int x = (int)lroundf(31.5f + s.dx * r);
    int y = (int)lroundf(31.5f + s.dy * r);
    uint8_t v = (uint8_t)constrain(50.0f + 240.0f * (1.0f - s.z), 0.0f, 255.0f);
    uint8_t hue = (uint8_t)(ctx.hue + s.hueOffset);
    plotMax(x, y, hsv565(hue, v));
    if (v > 170) {  // near stars get a dim halo
      plotMax(x + 1, y, hsv565(hue, v >> 2));
      plotMax(x, y + 1, hsv565(hue, v >> 2));
    }
  }
  drawBassCore(31.5f, 31.5f, ctx.levels.bass, ctx.hue);
}

// ---------------------------------------------------------------------------
// Effect 4: plasma waves — classic sine plasma, integer math over LUTs; the
// phases travel faster with loudness and bass swells the radial component.
// ---------------------------------------------------------------------------

static float plasmaP1 = 0, plasmaP2 = 0, plasmaP3 = 0, plasmaP4 = 0;

static void fxPlasma(const FrameCtx& ctx) {
  plasmaP1 += 1.0f + ctx.levels.loudness * 6.0f;
  plasmaP2 += 1.6f + ctx.levels.loudness * 4.0f;
  plasmaP3 -= 1.2f + ctx.levels.bass * 8.0f;
  plasmaP4 += 0.7f + ctx.levels.loudness * 3.0f;
  int p1 = (int)plasmaP1, p2 = (int)plasmaP2, p3 = (int)plasmaP3, p4 = (int)plasmaP4;
  int boost = (int)(ctx.levels.loudness * 60.0f);

  int i = 0;
  for (int y = 0; y < CANVAS_H; y++) {
    int sy = sinLut[(y * 7 + p2) & 255];
    for (int x = 0; x < CANVAS_W; x++, i++) {
      int sum = sinLut[(x * 6 + p1) & 255] + sy +
                sinLut[((x + y) * 4 + p3) & 255] +
                sinLut[(plasmaRad[i] + p4) & 255];  // -508..508
      uint8_t hue = (uint8_t)(ctx.hue + (sum >> 2));
      int v = 150 + (sum >> 3) + boost;
      cv[i] = hsv565(hue, (uint8_t)constrain(v, 40, 255));
    }
  }
}

// ---------------------------------------------------------------------------
// Effect 5: kaleidoscope — one 32x32 quadrant of concentric band-driven arcs
// with rotating petals, mirrored 4-way.
// ---------------------------------------------------------------------------

static float kalRot = 0.0f;

static void fxKaleido(const FrameCtx& ctx) {
  kalRot += 0.8f + ctx.levels.loudness * 3.0f;
  int rot = (int)kalRot;
  int flash = ctx.beat ? 60 : 0;

  for (int y = 0; y < 32; y++) {
    for (int x = 0; x < 32; x++) {
      int q = y * 32 + x;
      int band = kalBand[q];
      int level = bandLevel(ctx.levels, band);
      int mod = sinLut[(kalAng[q] * 3 + rot) & 255];  // 3 petals per quadrant
      int v = ((14 + level * 225 / 60) * (160 + mod)) >> 8;
      v = min(255, v + flash);
      uint16_t c = hsv565((uint8_t)(ctx.hue + band * 12 + (mod >> 3)), (uint8_t)v);
      cv[y * CANVAS_W + x] = c;
      cv[y * CANVAS_W + (63 - x)] = c;
      cv[(63 - y) * CANVAS_W + x] = c;
      cv[(63 - y) * CANVAS_W + (63 - x)] = c;
    }
  }
}

// ---------------------------------------------------------------------------
// Engine: beat detection, effect rotation, fade transitions.
// ---------------------------------------------------------------------------

static constexpr int kEffectCount = 6;
static constexpr uint32_t kMinDwellMs = 25000;  // then switch on the next beat
static constexpr uint32_t kMaxDwellMs = 40000;  // switch even without beats
static constexpr uint32_t kFadeMs = 400;
static constexpr float kBeatLoudness = 0.72f;
static constexpr uint32_t kBeatRefractoryMs = 220;

static int effectIndex = 0;
static int pendingEffect = -1;
static uint32_t effectStartMs = 0;
static uint32_t fadeOutUntilMs = 0;  // nonzero while transitioning
static uint32_t lastFrameMs = 0;
static uint32_t lastBeatMs = 0;
static float hueBase = 0.0f;
static bool lutsReady = false;

static void startEffect(int index, uint32_t now) {
  switch (index) {
    case 0: swirlInit(now); break;
    case 2: ripplesInit(); break;
    case 3: starfieldInit(); break;
    default: break;  // radial/plasma/kaleido carry their phase over
  }
}

static int pickNextEffect(int current) {
  int next;
  do {
    next = (int)random(0, kEffectCount);
  } while (next == current);
  return next;
}

void Visualizer::render(uint16_t* canvas, const AudioLevels& levels) {
  cv = canvas;
  if (!lutsReady) {
    buildLuts();
    lutsReady = true;
  }

  uint32_t now = millis();
  bool restart = now - lastFrameMs > 500;  // mode just (re)entered
  lastFrameMs = now;
  if (restart) {
    memset(canvas, 0, CANVAS_PX * sizeof(uint16_t));
    fadeOutUntilMs = 0;
    pendingEffect = -1;
    effectStartMs = now;
    startEffect(effectIndex, now);
  }

  // Shared palette drift persists across effect switches.
  hueBase += 0.25f + levels.loudness * 1.5f;
  if (hueBase >= 256.0f) hueBase -= 256.0f;

  bool beat = levels.loudness > kBeatLoudness && now - lastBeatMs > kBeatRefractoryMs;
  if (beat) lastBeatMs = now;

  if (fadeOutUntilMs == 0) {
    uint32_t elapsed = now - effectStartMs;
    if ((elapsed >= kMinDwellMs && beat) || elapsed >= kMaxDwellMs) {
      fadeOutUntilMs = now + kFadeMs;
      pendingEffect = pickNextEffect(effectIndex);
    }
  }

  if (fadeOutUntilMs != 0) {
    if (now < fadeOutUntilMs) {  // fade the old effect toward black
      for (int i = 0; i < CANVAS_PX; i++) {
        if (canvas[i]) canvas[i] = fade565(fade565(canvas[i]));
      }
      return;
    }
    effectIndex = pendingEffect;
    pendingEffect = -1;
    fadeOutUntilMs = 0;
    effectStartMs = now;
    memset(canvas, 0, CANVAS_PX * sizeof(uint16_t));
    startEffect(effectIndex, now);
  }

  FrameCtx ctx{levels, now, beat, (uint8_t)hueBase};
  switch (effectIndex) {
    case 0: fxSwirl(ctx); break;
    case 1: fxRadial(ctx); break;
    case 2: fxRipples(ctx); break;
    case 3: fxStarfield(ctx); break;
    case 4: fxPlasma(ctx); break;
    case 5: fxKaleido(ctx); break;
  }
}

#endif  // AUDIO_REACTIVE
