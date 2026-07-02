# AirPlay Album Art Display — System Design

Show the album art of whatever is currently AirPlaying to the HomePods on a 64x64
RGB LED matrix, with a thin white progress bar along the bottom. When nothing is
playing (or the server is unreachable), the display is solid black.

## Hardware

| Stage | Display | Controller |
|---|---|---|
| Now (interim) | Adafruit ESP32-S3 Reverse TFT Feather — 240x135 ST7789 TFT | same board |
| Final | 64x64 P3 HUB75 RGB LED matrix (192x192mm, 1/32 scan, 5V, SMD2121) | displayless ESP32-S3 |

The interim TFT shows a **letterboxed preview** of the exact same 64x64 frame the
matrix would receive (see [Rendering](#rendering)). Swapping to the real matrix
changes only the output driver, never the rendering.

## Why a Mac mini service?

Music is AirPlayed to the HomePods from *any* device (iPhone, iPad, Mac). The Mac
mini's own now-playing APIs only see local playback, so they are useless here.
Instead, a Python service on the always-on Mac mini uses
[pyatv](https://pyatv.dev) to connect to each HomePod directly over the LAN and
receive **push updates** for playback state and artwork — source-agnostic: it sees
whatever the HomePod itself is playing.

## Architecture

```
iPhone / iPad / Mac ──AirPlay──▶  HomePods (multiple rooms)
                                      │
                                      │  pyatv push listeners
                                      │  (playback state + artwork)
                                      ▼
                        Mac mini — Python service (launchd)
                          • monitors all HomePods
                          • arbitrates the "active" source
                          • art → 64x64 RGB565
                          • WebSocket server (push)
                                      │
                                      │  WebSocket over LAN
                                      ▼
                        ESP32-S3 firmware
                          • 64x64 RGB565 canvas (single source of truth)
                          • interpolates progress locally between updates
                          • output driver: TFT preview now, HUB75 later
```

---

## Python service (`service/`)

Python 3.11+, managed with **uv**. Dependencies: `pyatv`, `Pillow`, `websockets`,
`PyYAML`. Fully asyncio (pyatv is asyncio-native).

### HomePod discovery & monitoring

- HomePods are discovered with pyatv's zeroconf scan.
- `config.yaml` either lists device identifiers explicitly or sets
  `discovery.auto: true` to monitor every HomePod found.
- One pyatv connection + `PushListener` per HomePod. The listener fires on every
  playback change (play/pause/track change/seek); artwork is fetched via
  `atv.metadata.artwork()` when the track changes.
- Connections to HomePods are supervised: on drop, reconnect with exponential
  backoff (max ~30s). A HomePod being offline is normal, not an error.

### Pairing

HomePods may require one-time AirPlay pairing depending on the Home's access
settings. Setup flow (documented in `service/README` section of this file):

```
uv run atvremote scan                          # find identifiers
uv run atvremote --id <ID> --protocol airplay pair   # if prompted for a PIN
```

Resulting credentials go into `config.yaml` under the device entry.

### Arbitration (multiple HomePods, separate rooms)

- **Active source** = the HomePod that most recently transitioned to *playing*.
- If the active source pauses/stops, fall back to any other HomePod still playing
  (most recent first). If none, the system is **idle**.
- Idle → clients are told "nothing playing" → black screen.
- Stereo pairs appear as a single AirPlay target; no special handling.

### Artwork pipeline

1. Raw artwork bytes from pyatv (usually JPEG/PNG, any size).
2. Pillow: center-crop to square → **Lanczos** downscale to **64x64** → RGB.
3. Convert to **RGB565 big-endian**, row-major, top-left origin: 64·64·2 =
   **8,192 bytes**.
4. `art_id` = first 8 bytes of SHA-256 of the raw artwork bytes, hex-encoded
   (16 chars). Used for dedup/reference.
5. `separator` flag = `true` when the mean luminance (ITU-R BT.601) of the bottom
   4 rows of the 64x64 image is ≥ 160/255 — i.e. the art is light where the white
   progress bar sits, so the firmware should draw a black divider row.
6. No artwork available (some sources) → art payload of all-black pixels with
   `art_id = "0000000000000000"`.

### WebSocket protocol

Server listens on `ws://<mac-mini>:8765/` (port configurable). Server → client
only; clients never send application messages (pings are protocol-level).

**Text frame — `state` (JSON):**

```json
{
  "type": "state",
  "playing": true,
  "position": 83.4,          // seconds, at time `ts`
  "duration": 214.0,         // seconds; 0 or null → hide progress bar
  "ts": 1730000000.123,      // server epoch seconds when position was sampled
  "art_id": "a1b2c3d4e5f60718",
  "separator": false
}
```

Sent: on every playback change, and every **5 s** as resync/keepalive while
playing. When idle: `{"type":"state","playing":false}` once, then keepalives.

**Binary frame — `art`:**

```
bytes 0..15   art_id (16 ASCII hex chars, matches state.art_id)
bytes 16..8207  8,192 bytes RGB565 big-endian, row-major, 64x64
```

Sent when the artwork changes, and **immediately on client connect** together
with a `state` frame (full snapshot — this is what makes ESP32 reconnection
seamless).

Client rule: if a `state` arrives whose `art_id` doesn't match the art it holds,
keep showing the old art until the binary frame arrives (it is always in flight).

### Service management (launchd)

The service must survive Mac mini reboots and crashes. It runs as a **LaunchAgent**
with `RunAtLoad` + `KeepAlive`; launchd restarts it on crash and starts it at
login/boot. Template: `service/launchd/com.romeluis.airplay-art.plist` (invokes
`uv run airplay-art` with the project directory as working dir).

Install:

```
cp service/launchd/com.romeluis.airplay-art.plist ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/com.romeluis.airplay-art.plist
```

Logs go to `~/Library/Logs/airplay-art.log` (path set in the plist).

---

## ESP32 firmware (`firmware/`)

PlatformIO, Arduino framework, board `adafruit_feather_esp32s3_reversetft`.
Libraries: Adafruit GFX + ST7789 (interim display), `links2004/WebSockets`
(event-driven client with auto-reconnect). Later: `ESP32-HUB75-MatrixPanel-I2S-DMA`.

### Rendering

All drawing happens into an in-RAM **64x64 RGB565 canvas** — the single source of
truth. Every frame:

1. Blit current album art into the canvas (or all black if idle/disconnected).
2. Overlay the progress bar (below).
3. Hand the canvas to the active **output driver**.

Output drivers implement one interface (`begin()`, `show(const uint16_t* canvas)`):

- **`TftPreviewOutput` (now):** nearest-neighbor **2x** scale → 128x128, centered
  on the 240x135 ST7789. 135−128 = 7 → 3px top / 4px bottom black bars; 240−128 =
  112 → 56px black bars left/right. Integer scale + nearest neighbor = pixel-exact
  preview of what the matrix will show.
- **`Hub75Output` (later):** blit the canvas 1:1 to the matrix via the HUB75 DMA
  library. Selected by a PlatformIO build flag; nothing else changes.

### Progress bar

- Bottom **2 rows** (y = 62, 63), **white** (0xFFFF), filled left→right:
  `fill_px = round(position / duration * 64)`, overlaid on the art.
- If the current art's `separator` flag is set (light artwork at the bottom),
  the entire bottom **3 rows** (y = 61–63) are blacked out full-width first, and
  the white fill is drawn on top — both the bar and its empty track stay
  readable on white album art.
- `duration` missing/0 → no bar (art only).
- **Local interpolation:** the server sends `position`, `duration`, `ts`. While
  `playing`, the firmware advances position every frame using `millis()` since the
  last `state` frame, clamped to `duration`. Each incoming `state` re-syncs it —
  smooth motion with zero network chatter.

### Connection state machine

```
BOOT → WIFI_CONNECTING → WS_CONNECTING → CONNECTED
         ▲    │(retry forever)  │(fixed 2s interval, forever)
         └────┴─────────────────┴── any drop → black screen, retry
```

A WebSocket ping/pong heartbeat (15s interval, 2 missed pongs → reconnect)
detects a silently dead server, e.g. the Mac mini being powered off.

- Wi-Fi credentials and server host/port live in `include/secrets.h`
  (git-ignored; `secrets.h.example` is checked in).
- On WS connect the server pushes a full snapshot, so the firmware needs no
  request logic — it is a pure listener.
- **Mac mini off/rebooting** is a non-event: display goes black, the client
  retries forever, and recovers the full state on the next successful connect.

### Idle behavior

Nothing playing, or no server → solid black canvas. TFT backlight stays on for
now; on the matrix, black pixels = LEDs off anyway.

---

## Future: HUB75 migration

1. Add `ESP32-HUB75-MatrixPanel-I2S-DMA` to `lib_deps`.
2. Implement `Hub75Output` (canvas is already the matrix's native 64x64 RGB565;
   the library accepts per-pixel RGB565 draws or 565→888 expansion).
3. Add a `hub75` PlatformIO environment with the displayless board + pin map for
   the HUB75 connector; build flag selects the output driver.
4. Panel is 5V/indoor; power it from a proper 5V supply (a 64x64 P3 panel can
   draw ~3–4A at full white), logic from the ESP32-S3.

No changes to the Python service, the protocol, or the rendering code.

## Repository layout

```
design.md            this file
firmware/            PlatformIO project (ESP32-S3)
  platformio.ini
  src/main.cpp
  include/secrets.h.example
service/             Python service (uv project)
  pyproject.toml / uv.lock
  airplay_art/main.py
  config.example.yaml
  launchd/com.romeluis.airplay-art.plist
```
