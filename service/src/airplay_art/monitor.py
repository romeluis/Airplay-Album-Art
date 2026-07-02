"""HomePod monitoring and arbitration. See design.md "Discovery & monitoring".

DeviceMonitor: one supervised pyatv connection per HomePod (reconnects with
backoff forever). Coordinator: collects push updates from all monitors,
decides the active source, drives the Broadcaster.
"""

from __future__ import annotations

import asyncio
import logging
import time
from dataclasses import dataclass, field

import pyatv
from pyatv import interface
from pyatv.const import DeviceState, MediaType, Protocol

from .art import BLACK_ART, process_artwork
from .cache import ArtCache
from .lookup import fetch_supplementary_artwork
from .server import Broadcaster

log = logging.getLogger(__name__)

_SCAN_TIMEOUT = 5
_MAX_BACKOFF = 30
_PUSH_RESTART_DELAY = 5

# Native artwork often lags the track-change push by a moment; retry before
# falling back to the iTunes lookup.
_ART_ATTEMPTS = 4
_ART_RETRY_DELAY = 1.5


class DeviceMonitor(interface.PushListener, interface.DeviceListener):
    """Keeps one HomePod connected and forwards its push updates."""

    def __init__(
        self,
        identifier: str,
        name: str,
        coordinator: "Coordinator",
        credentials: str | None = None,
    ) -> None:
        self.identifier = identifier
        self.name = name
        self.atv: interface.AppleTV | None = None
        self._coordinator = coordinator
        self._credentials = credentials
        self._disconnected = asyncio.Event()
        self._task: asyncio.Task | None = None

    def start(self) -> None:
        self._task = asyncio.create_task(self._run(), name=f"monitor-{self.name}")

    async def _run(self) -> None:
        loop = asyncio.get_running_loop()
        backoff = 1
        while True:
            try:
                confs = await pyatv.scan(loop, identifier=self.identifier, timeout=_SCAN_TIMEOUT)
                if not confs:
                    raise RuntimeError("not found on network")
                conf = confs[0]
                if self._credentials:
                    service = conf.get_service(Protocol.AirPlay)
                    if service:
                        service.credentials = self._credentials
                atv = await pyatv.connect(conf, loop)
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                log.warning("%s: connect failed (%s), retrying in %ds", self.name, exc, backoff)
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2, _MAX_BACKOFF)
                continue

            backoff = 1
            self.atv = atv
            self._disconnected.clear()
            # pyatv holds listeners by weakref; `self` outlives the connection.
            atv.listener = self
            atv.push_updater.listener = self
            atv.push_updater.start()
            log.info("%s: connected", self.name)

            await self._disconnected.wait()
            self.atv = None
            try:
                atv.close()
            except Exception:
                pass
            self._coordinator.on_disconnect(self)
            log.info("%s: connection lost, reconnecting", self.name)

    # -- interface.PushListener ------------------------------------------------

    def playstatus_update(self, updater, playstatus: interface.Playing) -> None:
        self._coordinator.on_playstatus(self, playstatus)

    def playstatus_error(self, updater, exception: Exception) -> None:
        log.warning("%s: push update error (%s), restarting push updates", self.name, exception)
        loop = asyncio.get_running_loop()
        loop.call_later(_PUSH_RESTART_DELAY, self._restart_push, updater)

    def _restart_push(self, updater) -> None:
        if self.atv is None:  # reconnect already in progress
            return
        try:
            updater.start()
        except Exception as exc:
            log.warning("%s: could not restart push updates: %s", self.name, exc)
            self._disconnected.set()

    # -- interface.DeviceListener ----------------------------------------------

    def connection_lost(self, exception: Exception) -> None:
        self._disconnected.set()

    def connection_closed(self) -> None:
        self._disconnected.set()


@dataclass
class _DeviceEntry:
    monitor: DeviceMonitor
    playstatus: interface.Playing | None = None
    last_play_transition: float = field(default=0.0)


class Coordinator:
    """Arbitrates the active source across all HomePods and emits updates."""

    def __init__(self, broadcaster: Broadcaster, cache: ArtCache | None = None) -> None:
        self._broadcaster = broadcaster
        self._cache = cache
        self._devices: dict[str, _DeviceEntry] = {}
        self._active_id: str | None = None
        self._art_key: str | None = None  # (device, track-hash) of last art fetch
        self._art_task: asyncio.Task | None = None
        self._art_pending = False  # a fetch is in flight; clients show a loading animation

    def add_homepod(self, identifier: str, name: str, credentials: str | None = None) -> None:
        if identifier in self._devices:
            return
        log.info("monitoring HomePod: %s (%s)", name, identifier)
        monitor = DeviceMonitor(identifier, name, self, credentials)
        self._devices[identifier] = _DeviceEntry(monitor=monitor)
        monitor.start()

    def on_playstatus(self, monitor: DeviceMonitor, playstatus: interface.Playing) -> None:
        entry = self._devices[monitor.identifier]
        was_playing = _is_playing(entry.playstatus)
        entry.playstatus = playstatus
        if _is_playing(playstatus) and not was_playing:
            entry.last_play_transition = time.monotonic()
        log.info(
            "%s: %s [%s] — %s / %s / %s (%s/%ss)",
            monitor.name,
            playstatus.device_state.name,
            playstatus.media_type.name,
            playstatus.title,
            playstatus.artist,
            playstatus.album,
            playstatus.position,
            playstatus.total_time,
        )
        self._recompute()

    def on_disconnect(self, monitor: DeviceMonitor) -> None:
        self._devices[monitor.identifier].playstatus = None
        self._recompute()

    def _recompute(self) -> None:
        playing = [
            (entry.last_play_transition, identifier)
            for identifier, entry in self._devices.items()
            if _is_playing(entry.playstatus)
        ]
        new_active = max(playing)[1] if playing else None

        if new_active is None:
            if self._active_id is not None:
                log.info("idle: nothing playing")
            self._active_id = None
            self._broadcaster.set_state(playing=False)
            return

        if new_active != self._active_id:
            self._active_id = new_active
            log.info("active source: %s", self._devices[new_active].monitor.name)

        entry = self._devices[new_active]
        ps = entry.playstatus
        assert ps is not None

        # Right after an AirPlay stream starts, the first push updates can have
        # empty metadata; wait for the follow-up that fills it in before
        # resolving artwork (avoids flashing black art for a real track).
        has_metadata = ps.title is not None or ps.artist is not None
        art_key = f"{new_active}:{ps.hash}"
        if has_metadata and art_key != self._art_key:
            self._art_key = art_key
            if self._art_task is not None:
                self._art_task.cancel()
            self._art_pending = True
            self._art_task = asyncio.create_task(self._fetch_art(entry.monitor, ps))

        self._emit_state(ps)

    def _emit_state(self, ps: interface.Playing) -> None:
        self._broadcaster.set_state(
            playing=True,
            position=float(ps.position or 0),
            duration=float(ps.total_time or 0),
            art_pending=self._art_pending,
        )

    async def _fetch_art(self, monitor: DeviceMonitor, ps: interface.Playing) -> None:
        # Resolution chain: cache -> native artwork (with retries) -> lookup
        # APIs -> black. Successful resolutions are written back to the cache.
        art = None
        source = "cache"
        cache_key = ArtCache.key_for(ps) if self._cache else None
        if cache_key:
            art = await asyncio.to_thread(self._cache.get, cache_key)

        if art is None:
            source = "native"
            for attempt in range(_ART_ATTEMPTS):
                if attempt:
                    await asyncio.sleep(_ART_RETRY_DELAY)
                atv = monitor.atv
                if atv is None:
                    break
                try:
                    info = await atv.metadata.artwork(width=256, height=256)
                except asyncio.CancelledError:
                    raise
                except Exception as exc:
                    log.debug("%s: artwork attempt %d failed: %s", monitor.name, attempt + 1, exc)
                    info = None
                if info is not None and info.bytes:
                    art = await asyncio.to_thread(process_artwork, info.bytes)
                    break

            if art is None:
                raw = await fetch_supplementary_artwork(ps)
                if raw:
                    art = await asyncio.to_thread(process_artwork, raw)
                    source = "lookup"

            if art is not None and cache_key:
                await asyncio.to_thread(self._cache.put, cache_key, art)

        if art is None:
            art = BLACK_ART
            source = "none"
        self._art_pending = False
        log.info(
            "%s: artwork %s (source=%s, separator=%s)",
            monitor.name, art.art_id, source, art.separator,
        )
        self._broadcaster.set_art(art)
        # Re-emit state so clients get the new art_id/separator with fresh position.
        entry = self._devices[monitor.identifier]
        if self._active_id == monitor.identifier and _is_playing(entry.playstatus):
            assert entry.playstatus is not None
            self._emit_state(entry.playstatus)


def _is_playing(ps: interface.Playing | None) -> bool:
    # Music only: HomePods also act as TV speakers (Apple TV audio routing);
    # shows/movies must not take over the display.
    return (
        ps is not None
        and ps.device_state == DeviceState.Playing
        and ps.media_type == MediaType.Music
    )
