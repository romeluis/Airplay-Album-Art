# AirPlay Album Art

Shows the album art of whatever is AirPlaying to the HomePods on a 64x64 RGB LED
matrix, with a white progress bar along the bottom; black when idle. A Python
service on the Mac mini ([service/](service/)) watches the HomePods and pushes
frames to an ESP32-S3 ([firmware/](firmware/)) — currently previewing letterboxed
on a TFT Feather until the matrix arrives.

**Complete system design: [design.md](design.md)**
