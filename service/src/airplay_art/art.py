"""Artwork pipeline: raw bytes -> 64x64 RGB565 big-endian + separator flag.

See design.md "Artwork pipeline" for the spec.
"""

from __future__ import annotations

import hashlib
import io
from dataclasses import dataclass

from PIL import Image, ImageDraw

ART_SIZE = 64
ART_BYTES = ART_SIZE * ART_SIZE * 2  # RGB565
BLACK_ART_ID = "0" * 16

# Bottom rows the white progress bar overlays; if the art is bright there,
# the firmware draws a black separator row above the bar.
_SEPARATOR_ROWS = 4
_SEPARATOR_LUMA_THRESHOLD = 160


@dataclass(frozen=True)
class ProcessedArt:
    art_id: str
    rgb565: bytes
    separator: bool


BLACK_ART = ProcessedArt(art_id=BLACK_ART_ID, rgb565=bytes(ART_BYTES), separator=False)


def _convert(img: Image.Image, art_id: str | None = None) -> ProcessedArt:
    """64x64 RGB image -> ProcessedArt (RGB565 BE + separator flag)."""
    pixels = img.tobytes()  # RGB888, row-major
    out = bytearray(ART_BYTES)
    for i in range(ART_SIZE * ART_SIZE):
        r, g, b = pixels[3 * i], pixels[3 * i + 1], pixels[3 * i + 2]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[2 * i] = v >> 8
        out[2 * i + 1] = v & 0xFF
    if art_id is None:
        art_id = hashlib.sha256(bytes(out)).hexdigest()[:16]
    return ProcessedArt(art_id=art_id, rgb565=bytes(out), separator=_needs_separator(pixels))


def process_artwork(raw: bytes) -> ProcessedArt:
    img = Image.open(io.BytesIO(raw)).convert("RGB")
    w, h = img.size
    side = min(w, h)
    left = (w - side) // 2
    top = (h - side) // 2
    img = img.crop((left, top, left + side, top + side))
    img = img.resize((ART_SIZE, ART_SIZE), Image.LANCZOS)
    return _convert(img, art_id=hashlib.sha256(raw).hexdigest()[:16])


def _make_note_art() -> ProcessedArt:
    """Placeholder for unresolvable artwork: gray beamed eighth notes on black."""
    img = Image.new("RGB", (ART_SIZE, ART_SIZE), (0, 0, 0))
    d = ImageDraw.Draw(img)
    gray = (190, 190, 190)
    d.ellipse((15, 41, 27, 50), fill=gray)                     # left note head
    d.ellipse((35, 38, 47, 47), fill=gray)                     # right note head
    d.rectangle((25, 17, 27, 45), fill=gray)                   # left stem
    d.rectangle((45, 14, 47, 42), fill=gray)                   # right stem
    d.polygon([(25, 13), (47, 10), (47, 18), (25, 21)], fill=gray)  # beam
    return _convert(img)


def _needs_separator(pixels: bytes) -> bool:
    start = 3 * ART_SIZE * (ART_SIZE - _SEPARATOR_ROWS)
    total = 0.0
    count = ART_SIZE * _SEPARATOR_ROWS
    for i in range(count):
        r = pixels[start + 3 * i]
        g = pixels[start + 3 * i + 1]
        b = pixels[start + 3 * i + 2]
        total += 0.299 * r + 0.587 * g + 0.114 * b
    return (total / count) >= _SEPARATOR_LUMA_THRESHOLD


NOTE_ART = _make_note_art()
