"""Config loading. See ../../config.example.yaml and ../../../design.md."""

from __future__ import annotations

import logging
from dataclasses import dataclass, field
from pathlib import Path

import yaml

log = logging.getLogger(__name__)


@dataclass
class HomePodEntry:
    name: str
    identifier: str
    credentials: str | None = None


_DEFAULT_CACHE_DIR = "~/Library/Caches/airplay-art"


@dataclass
class Config:
    host: str = "0.0.0.0"
    port: int = 8765
    auto: bool = True
    homepods: list[HomePodEntry] = field(default_factory=list)
    cache_enabled: bool = True
    cache_dir: str = _DEFAULT_CACHE_DIR
    cache_max_mb: int = 1024


def load_config(path: str | Path) -> Config:
    path = Path(path)
    if not path.exists():
        log.info("no config file at %s, using defaults (auto discovery, port 8765)", path)
        return Config()

    raw = yaml.safe_load(path.read_text()) or {}
    server = raw.get("server") or {}
    discovery = raw.get("discovery") or {}
    cache = raw.get("cache") or {}
    homepods = [
        HomePodEntry(
            name=entry.get("name", entry["identifier"]),
            identifier=entry["identifier"],
            credentials=entry.get("credentials"),
        )
        for entry in raw.get("homepods") or []
    ]
    return Config(
        host=server.get("host", "0.0.0.0"),
        port=int(server.get("port", 8765)),
        auto=bool(discovery.get("auto", True)),
        homepods=homepods,
        cache_enabled=bool(cache.get("enabled", True)),
        cache_dir=str(cache.get("dir", _DEFAULT_CACHE_DIR)),
        cache_max_mb=int(cache.get("max_mb", 1024)),
    )
