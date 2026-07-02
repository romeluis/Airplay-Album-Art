"""AirPlay Album Art service entry point. Full design: ../../../design.md."""

from __future__ import annotations

import argparse
import asyncio
import logging

import pyatv
from websockets.asyncio.server import serve

from .config import Config, load_config
from .monitor import Coordinator
from .server import Broadcaster

log = logging.getLogger(__name__)

_DISCOVERY_INTERVAL = 60
_DISCOVERY_SCAN_TIMEOUT = 5


def _is_homepod(conf: pyatv.interface.BaseConfig) -> bool:
    info = conf.device_info
    text = " ".join(
        str(part)
        for part in (getattr(info, "model_str", ""), getattr(info, "raw_model", ""), info.model)
        if part
    )
    return "homepod" in text.lower()


async def _discovery_loop(coordinator: Coordinator) -> None:
    loop = asyncio.get_running_loop()
    while True:
        try:
            confs = await pyatv.scan(loop, timeout=_DISCOVERY_SCAN_TIMEOUT)
            for conf in confs:
                if _is_homepod(conf):
                    coordinator.add_homepod(conf.identifier, conf.name)
        except Exception as exc:
            log.warning("discovery scan failed: %s", exc)
        await asyncio.sleep(_DISCOVERY_INTERVAL)


async def _run(config: Config) -> None:
    broadcaster = Broadcaster()
    coordinator = Coordinator(broadcaster)

    for hp in config.homepods:
        coordinator.add_homepod(hp.identifier, hp.name, hp.credentials)

    async with serve(broadcaster.handler, config.host, config.port):
        log.info("WebSocket server listening on ws://%s:%d/", config.host, config.port)
        tasks = [asyncio.create_task(broadcaster.keepalive_loop(), name="keepalive")]
        if config.auto:
            tasks.append(asyncio.create_task(_discovery_loop(coordinator), name="discovery"))
        await asyncio.gather(*tasks)


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="airplay-art",
        description="Push HomePod now-playing album art to an ESP32 display.",
    )
    parser.add_argument(
        "--config",
        default="config.yaml",
        help="path to config file (default: %(default)s)",
    )
    parser.add_argument("--verbose", action="store_true", help="debug logging")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
    )
    # pyatv is chatty at INFO for scan/connect internals
    logging.getLogger("pyatv").setLevel(logging.WARNING)

    try:
        asyncio.run(_run(load_config(args.config)))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
