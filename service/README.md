# airplay-art service

Runs on the Mac mini. Monitors the HomePods via pyatv and pushes 64x64 album art
+ playback state to the ESP32 display over WebSocket. Full design: [../design.md](../design.md).

```
uv sync
cp config.example.yaml config.yaml   # then edit
uv run airplay-art
```

Run permanently under launchd: see `launchd/com.romeluis.airplay-art.plist`.
