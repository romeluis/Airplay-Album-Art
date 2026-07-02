"""WebSocket push server. Protocol spec: design.md "WebSocket protocol".

Server -> client only. On connect a client gets a full snapshot (art frame,
then state frame); afterwards frames are pushed on change plus a periodic
state keepalive.
"""

from __future__ import annotations

import asyncio
import json
import logging
import time

from websockets.asyncio.server import ServerConnection

from .art import BLACK_ART, ProcessedArt

log = logging.getLogger(__name__)

KEEPALIVE_SECONDS = 5


def _art_frame(art: ProcessedArt) -> bytes:
    return art.art_id.encode("ascii") + art.rgb565


class Broadcaster:
    """Holds the latest state/art and fans them out to connected clients."""

    def __init__(self) -> None:
        self._clients: set[ServerConnection] = set()
        self._art = BLACK_ART
        self._state_msg = json.dumps({"type": "state", "playing": False})

    @property
    def art(self) -> ProcessedArt:
        return self._art

    async def handler(self, ws: ServerConnection) -> None:
        peer = ws.remote_address
        log.info("client connected: %s (%d total)", peer, len(self._clients) + 1)
        self._clients.add(ws)
        try:
            # Full snapshot on connect: art first so the state's art_id resolves.
            await ws.send(_art_frame(self._art))
            await ws.send(self._state_msg)
            async for _ in ws:  # clients never send application messages
                pass
        except Exception as exc:
            log.debug("client %s errored: %s", peer, exc)
        finally:
            self._clients.discard(ws)
            log.info("client disconnected: %s (%d total)", peer, len(self._clients))

    def set_state(
        self,
        *,
        playing: bool,
        position: float = 0.0,
        duration: float = 0.0,
        art_pending: bool = False,
        art_id: str | None = None,
        separator: bool | None = None,
    ) -> None:
        if not playing:
            self._state_msg = json.dumps({"type": "state", "playing": False})
        else:
            self._state_msg = json.dumps(
                {
                    "type": "state",
                    "playing": True,
                    "position": position,
                    "duration": duration,
                    "ts": time.time(),
                    "art_pending": art_pending,
                    "art_id": art_id if art_id is not None else self._art.art_id,
                    "separator": separator if separator is not None else self._art.separator,
                }
            )
        self._broadcast(self._state_msg)

    def set_art(self, art: ProcessedArt) -> None:
        if art.art_id == self._art.art_id:
            return
        self._art = art
        self._broadcast(_art_frame(art))

    def rebroadcast_state(self) -> None:
        self._broadcast(self._state_msg)

    def _broadcast(self, message: str | bytes) -> None:
        for ws in set(self._clients):
            asyncio.create_task(self._send(ws, message))

    async def _send(self, ws: ServerConnection, message: str | bytes) -> None:
        try:
            await ws.send(message)
        except Exception:
            self._clients.discard(ws)

    async def keepalive_loop(self) -> None:
        while True:
            await asyncio.sleep(KEEPALIVE_SECONDS)
            self.rebroadcast_state()
