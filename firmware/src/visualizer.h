// Multi-effect audio visualizer for the 64x64 canvas. A rotating set of
// beat-synced effects (particle swirl, radial spectrum, beat ripples,
// starfield warp, plasma waves, kaleidoscope); each runs ~25s, then hands
// off on the next detected beat with a quick fade to black.
#pragma once

#include <Arduino.h>

#include "audio_analyzer.h"

class Visualizer {
 public:
  // Composes one frame into canvas (CANVAS_PX RGB565). Detects mode re-entry
  // (>500ms since the last call) and restarts itself.
  void render(uint16_t* canvas, const AudioLevels& levels);
};
