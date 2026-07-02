// Shared canvas dimensions. The 64x64 RGB565 canvas is the single source of
// truth for rendering — identical on the TFT preview and the future HUB75
// matrix. See ../../design.md "Rendering".
#pragma once

#include <stdint.h>

constexpr int CANVAS_W = 64;
constexpr int CANVAS_H = 64;
constexpr int CANVAS_PX = CANVAS_W * CANVAS_H;
