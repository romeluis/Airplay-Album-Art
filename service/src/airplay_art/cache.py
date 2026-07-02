"""On-disk artwork cache.

Stores *processed* art (the 64x64 RGB565 payload + separator flag, ~8 KB per
album) keyed by artist+album, so replayed tracks resolve instantly without
re-hitting the network. Size-capped with LRU eviction (mtime is bumped on
read). Methods do blocking file I/O — call them via asyncio.to_thread.

Entry format: 16 bytes art_id (ascii hex) + 1 byte separator + 8,192 bytes
RGB565 big-endian.
"""

from __future__ import annotations

import hashlib
import logging
import os
from pathlib import Path

from pyatv import interface

from .art import ART_BYTES, ProcessedArt

log = logging.getLogger(__name__)

_ART_ID_LEN = 16
_ENTRY_LEN = _ART_ID_LEN + 1 + ART_BYTES


class ArtCache:
    def __init__(self, directory: Path, max_bytes: int) -> None:
        self.directory = directory
        self.max_bytes = max_bytes
        self.directory.mkdir(parents=True, exist_ok=True)

    @staticmethod
    def key_for(ps: interface.Playing) -> str | None:
        # Art belongs to the album, so tracks from one album share an entry;
        # fall back to the title for singles/streams without album metadata.
        artist = (ps.artist or "").strip().lower()
        album = (ps.album or "").strip().lower()
        title = (ps.title or "").strip().lower()
        if not artist or not (album or title):
            return None
        ident = f"{artist}|{album or title}"
        return hashlib.sha256(ident.encode()).hexdigest()[:24]

    def get(self, key: str) -> ProcessedArt | None:
        path = self._path(key)
        try:
            data = path.read_bytes()
        except FileNotFoundError:
            return None
        except OSError as exc:
            log.warning("art cache read failed: %s", exc)
            return None
        if len(data) != _ENTRY_LEN:
            log.warning("art cache: dropping malformed entry %s", path.name)
            path.unlink(missing_ok=True)
            return None
        try:
            os.utime(path)  # LRU bump
        except OSError:
            pass
        return ProcessedArt(
            art_id=data[:_ART_ID_LEN].decode("ascii", errors="replace"),
            separator=data[_ART_ID_LEN] != 0,
            rgb565=data[_ART_ID_LEN + 1:],
        )

    def put(self, key: str, art: ProcessedArt) -> None:
        entry = art.art_id.encode("ascii") + bytes([1 if art.separator else 0]) + art.rgb565
        try:
            self._path(key).write_bytes(entry)
            self._evict()
        except OSError as exc:
            log.warning("art cache write failed: %s", exc)

    def _path(self, key: str) -> Path:
        return self.directory / f"{key}.art"

    def _evict(self) -> None:
        files = []
        total = 0
        for p in self.directory.glob("*.art"):
            try:
                st = p.stat()
            except OSError:
                continue
            files.append((st.st_mtime, st.st_size, p))
            total += st.st_size
        files.sort()  # oldest first
        for _, size, victim in files:
            if total <= self.max_bytes:
                break
            victim.unlink(missing_ok=True)
            total -= size
            log.info("art cache: evicted %s", victim.name)
