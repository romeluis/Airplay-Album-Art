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
from pyatv.const import DeviceState, Protocol

from .art import BLACK_ART, process_artwork
from .server import Broadcaster

log = logging.getLogger(__name__)

_SCAN_TIMEOUT = 5
_MAX_BACKOFF = 30
_PUSH_RESTART_DELAY = 5


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

    def __init__(self, broadcaster: Broadcaster) -> None:
        self._broadcaster = broadcaster
        self._devices: dict[str, _DeviceEntry] = {}
        self._active_id: str | None = None
        self._art_key: str | None = None  # (device, track-hash) of last art fetch
        self._art_task: asyncio.Task | None = None

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
            "%s: %s — %s / %s / %s (%s/%ss)",
            monitor.name,
            playstatus.device_state.name,
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

        art_key = f"{new_active}:{ps.hash}"
        if art_key != self._art_key:
            self._art_key = art_key
            if self._art_task is not None:
                self._art_task.cancel()
            self._art_task = asyncio.create_task(self._fetch_art(entry.monitor, ps))

        self._emit_state(ps)

    def _emit_state(self, ps: interface.Playing) -> None:
        self._broadcaster.set_state(
            playing=True,
            position=float(ps.position or 0),
            duration=float(ps.total_time or 0),
        )

    async def _fetch_art(self, monitor: DeviceMonitor, ps: interface.Playing) -> None:
        art = BLACK_ART
        atv = monitor.atv
        try:
            if atv is not None:
                info = await atv.metadata.artwork(width=256, height=256)
                if info is not None and info.bytes:
                    art = await asyncio.to_thread(process_artwork, info.bytes)
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            log.warning("%s: artwork fetch failed (%s), using black", monitor.name, exc)
        log.info("%s: artwork %s (separator=%s)", monitor.name, art.art_id, art.separator)
        self._broadcaster.set_art(art)
        # Re-emit state so clients get the new art_id/separator with fresh position.
        entry = self._devices[monitor.identifier]
        if self._active_id == monitor.identifier and _is_playing(entry.playstatus):
            assert entry.playstatus is not None
            self._emit_state(entry.playstatus)


def _is_playing(ps: interface.Playing | None) -> bool:
    return ps is not None and ps.device_state == DeviceState.Playing
