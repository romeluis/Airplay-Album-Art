"""Supplementary album-art lookup for streams without embedded artwork.

Common when casting from phone apps. Resolution order (all keyless APIs):

1. iTunes Lookup by the track's iTunes Store identifier — exact.
2. Deezer search — field-scoped (artist:"..." track:"..."), precise ranking.
3. iTunes search — fuzzy ranking, last resort.

Every non-exact result is validated against the track's artist AND title/album
before use — a confidently wrong cover is worse than the black fallback.
"""

from __future__ import annotations

import logging
import re

import aiohttp
from pyatv import interface

log = logging.getLogger(__name__)

_TIMEOUT = aiohttp.ClientTimeout(total=10)
_ITUNES_LOOKUP_URL = "https://itunes.apple.com/lookup"
_ITUNES_SEARCH_URL = "https://itunes.apple.com/search"
_DEEZER_TRACK_URL = "https://api.deezer.com/search"
_DEEZER_ALBUM_URL = "https://api.deezer.com/search/album"
_SEARCH_LIMIT = 25


async def fetch_supplementary_artwork(ps: interface.Playing) -> bytes | None:
    """Returns raw image bytes for the playing track, or None."""
    try:
        async with aiohttp.ClientSession(timeout=_TIMEOUT) as session:
            url = await _resolve_artwork_url(session, ps)
            if url is None:
                return None
            async with session.get(url) as resp:
                resp.raise_for_status()
                return await resp.read()
    except aiohttp.ClientError as exc:
        log.warning("supplementary artwork lookup failed: %s", exc)
        return None


async def _resolve_artwork_url(session: aiohttp.ClientSession, ps: interface.Playing) -> str | None:
    store_id = getattr(ps, "itunes_store_identifier", None)
    if store_id:
        results = await _itunes_query(session, _ITUNES_LOOKUP_URL, {"id": str(store_id)})
        if results:
            url = _itunes_artwork_url(results[0])
            if url:
                return url

    artist = ps.artist or ""
    if not artist:
        return None

    url = await _deezer_url(session, ps, artist)
    if url:
        return url
    return await _itunes_search_url(session, ps, artist)


# -- Deezer ------------------------------------------------------------------


async def _deezer_url(session: aiohttp.ClientSession, ps: interface.Playing, artist: str) -> str | None:
    # Deezer credits only the primary artist; strip collaborators for the query
    # ("Kali Uchis & SZA" -> "Kali Uchis"). Validation still uses the full name.
    q_artist = _primary_artist(artist)

    if ps.title:
        data = await _get_json(session, _DEEZER_TRACK_URL, {"q": f'artist:"{q_artist}" track:"{ps.title}"'})
        for r in data.get("data") or []:
            if _loose_match(artist, r.get("artist", {}).get("name", "")) and _loose_match(ps.title, r.get("title", "")):
                album = r.get("album") or {}
                url = album.get("cover_xl") or album.get("cover_big")
                if url:
                    return url

    if ps.album:
        data = await _get_json(session, _DEEZER_ALBUM_URL, {"q": f'artist:"{q_artist}" album:"{ps.album}"'})
        for r in data.get("data") or []:
            if _loose_match(artist, r.get("artist", {}).get("name", "")) and _loose_match(ps.album, r.get("title", "")):
                url = r.get("cover_xl") or r.get("cover_big")
                if url:
                    return url
    return None


def _primary_artist(artist: str) -> str:
    head = re.split(r"\s*(?:&|,|;|feat\.?|featuring|with)\s+", artist, flags=re.IGNORECASE)[0].strip()
    return head or artist


# -- iTunes ------------------------------------------------------------------


async def _itunes_search_url(session: aiohttp.ClientSession, ps: interface.Playing, artist: str) -> str | None:
    # The combined-term search is fuzzy (may bury the exact track), so also
    # try attribute-scoped searches on the bare title/album name.
    attempts: list[tuple[dict, str, str]] = []
    if ps.title:
        attempts.append(({"term": f"{artist} {ps.title}", "entity": "song"}, ps.title, "trackName"))
        attempts.append(({"term": ps.title, "attribute": "songTerm", "entity": "song"}, ps.title, "trackName"))
    if ps.album:
        attempts.append(({"term": f"{artist} {ps.album}", "entity": "album"}, ps.album, "collectionName"))
        attempts.append(({"term": ps.album, "attribute": "albumTerm", "entity": "album"}, ps.album, "collectionName"))

    for params, name, name_field in attempts:
        results = await _itunes_query(
            session, _ITUNES_SEARCH_URL,
            {**params, "media": "music", "limit": str(_SEARCH_LIMIT)},
        )
        for r in results:
            if _loose_match(artist, r.get("artistName", "")) and _loose_match(name, r.get(name_field, "")):
                url = _itunes_artwork_url(r)
                if url:
                    return url
    return None


async def _itunes_query(session: aiohttp.ClientSession, endpoint: str, params: dict) -> list[dict]:
    data = await _get_json(session, endpoint, params)
    return data.get("results") or []


def _itunes_artwork_url(result: dict) -> str | None:
    url = result.get("artworkUrl100")
    # The store serves arbitrary sizes; swap the size token for a sharper source.
    return url.replace("100x100bb", "600x600bb") if url else None


# -- shared ------------------------------------------------------------------


async def _get_json(session: aiohttp.ClientSession, endpoint: str, params: dict) -> dict:
    async with session.get(endpoint, params=params) as resp:
        resp.raise_for_status()
        data = await resp.json(content_type=None)
    return data if isinstance(data, dict) else {}


def _loose_match(a: str, b: str) -> bool:
    na, nb = _norm(a), _norm(b)
    return bool(na) and bool(nb) and (na in nb or nb in na)


def _norm(s: str) -> str:
    return "".join(c for c in s.lower() if c.isalnum() or c.isspace()).strip()
