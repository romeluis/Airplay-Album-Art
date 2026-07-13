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


def _peer_str(ws: ServerConnection) -> str:
    peer = ws.remote_address
    if isinstance(peer, tuple) and len(peer) >= 2:
        return f"{peer[0]}:{peer[1]}"
    return str(peer)


def _fmt_duration(seconds: float) -> str:
    s = int(seconds)
    if s < 60:
        return f"{s}s"
    if s < 3600:
        return f"{s // 60}m{s % 60:02d}s"
    return f"{s // 3600}h{(s % 3600) // 60:02d}m"


class Broadcaster:
    """Holds the latest state/art and fans them out to connected clients."""

    def __init__(self) -> None:
        self._clients: set[ServerConnection] = set()
        self._art = BLACK_ART
        self._playing_fields: dict | None = None  # last playing-state sample
        self._state_msg = json.dumps({"type": "state", "playing": False})

    @property
    def art(self) -> ProcessedArt:
        return self._art

    async def handler(self, ws: ServerConnection) -> None:
        peer = _peer_str(ws)
        connected_at = time.monotonic()
        log.info("client connected: %s (%d total)", peer, len(self._clients) + 1)
        self._clients.add(ws)
        try:
            # Full snapshot on connect: art first so the state's art_id resolves.
            await ws.send(_art_frame(self._art))
            await ws.send(self._snapshot_state_msg())
            async for _ in ws:  # clients never send application messages
                pass
        except Exception as exc:
            log.debug("client %s errored: %s", peer, exc)
        finally:
            self._clients.discard(ws)
            log.info(
                "client disconnected: %s after %s (%d total)",
                peer,
                _fmt_duration(time.monotonic() - connected_at),
                len(self._clients),
            )

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
            self._playing_fields = None
            self._state_msg = json.dumps({"type": "state", "playing": False})
        else:
            self._playing_fields = {
                "type": "state",
                "playing": True,
                "position": position,
                "duration": duration,
                "ts": time.time(),
                "art_pending": art_pending,
                "art_id": art_id if art_id is not None else self._art.art_id,
                "separator": separator if separator is not None else self._art.separator,
            }
            self._state_msg = json.dumps(self._playing_fields)
        self._broadcast(self._state_msg)

    def _snapshot_state_msg(self) -> str:
        """State frame for a newly connected client.

        State is only sampled on AirPlay push events (track change,
        play/pause), so the stored sample can be minutes old. A client joining
        mid-song must not take it as current — extrapolate the position to now.
        Broadcasts and keepalives still send the frozen sample; clients that
        received it fresh interpolate locally.
        """
        if self._playing_fields is None:
            return self._state_msg
        fields = dict(self._playing_fields)
        now = time.time()
        position = fields["position"] + (now - fields["ts"])
        if fields["duration"] > 0:
            position = min(position, fields["duration"])
        fields["position"] = position
        fields["ts"] = now
        return json.dumps(fields)

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
