"""Validated, bounded HTTP communication with the radio."""

from __future__ import annotations

from dataclasses import dataclass
from urllib.parse import urlsplit

import aiohttp


class RadioError(Exception):
    """A radio transport, protocol or command error."""


def validate_url(value: str) -> str:
    """Accept only the direct HTTP(S) URLs supported by the firmware."""
    value = value.strip()
    if not value or len(value) > 511:
        raise ValueError("The stream URL must contain 1–511 characters")
    if any(ord(char) <= 32 or ord(char) >= 127 or char in "\\#@" for char in value):
        raise ValueError("The stream URL cannot contain spaces, credentials or fragments")
    parts = urlsplit(value)
    if parts.scheme not in ("http", "https") or not parts.hostname:
        raise ValueError("Enter a direct HTTP or HTTPS MP3/AAC stream URL")
    if parts.port is not None and not 1 <= parts.port <= 65535:
        raise ValueError("Invalid stream port")
    return value


@dataclass(frozen=True)
class RadioStatus:
    """The subset of status needed by Home Assistant entities."""

    selected: int
    station: str
    custom_url: str
    source: str
    radio_playing: bool
    error: str

    @classmethod
    def from_json(cls, data: object) -> RadioStatus:
        if not isinstance(data, dict):
            raise RadioError("Radio returned a non-object status")
        for key in ("station", "custom_url", "source", "error"):
            if not isinstance(data.get(key), str):
                raise RadioError(f"Radio status is missing valid {key}")
        selected = data.get("selected")
        if type(selected) is not int or not 0 <= selected <= 4:
            raise RadioError("Radio returned an invalid station selection")
        if type(data.get("radio_playing")) is not bool:
            raise RadioError("Radio returned an invalid playback state")
        if data["source"] not in ("Radio", "AirPlay"):
            raise RadioError("Radio returned an unknown audio source")
        return cls(
            selected, data["station"], data["custom_url"], data["source"],
            data["radio_playing"], data["error"],
        )


class RadioApi:
    """Use Home Assistant's shared HTTP session without owning its lifetime."""

    def __init__(self, session: aiohttp.ClientSession, host: str, port: int) -> None:
        self.session = session
        self.base_url = f"http://{host}:{port}"

    async def _request(self, path: str, payload: dict | None = None) -> object:
        method = "GET" if payload is None else "POST"
        try:
            async with self.session.request(
                method, self.base_url + path, json=payload,
                timeout=aiohttp.ClientTimeout(total=8),
                allow_redirects=False,
            ) as response:
                if response.status != 200:
                    raise RadioError(f"Radio returned HTTP {response.status}")
                if response.content_type != "application/json":
                    raise RadioError("The host did not return the radio JSON API")
                try:
                    return await response.json()
                except ValueError as err:
                    raise RadioError("Radio returned malformed JSON") from err
        except (aiohttp.ClientError, TimeoutError) as err:
            raise RadioError(f"Cannot communicate with the radio: {err}") from err

    async def status(self) -> RadioStatus:
        return RadioStatus.from_json(await self._request("/api/radio"))

    async def identity(self) -> tuple[str, str]:
        data = await self._request("/api/system/info")
        info = data.get("info") if isinstance(data, dict) else None
        if not isinstance(info, dict):
            raise RadioError("Radio returned invalid device information")
        mac, name = info.get("mac"), info.get("device_name")
        if not isinstance(mac, str) or not isinstance(name, str) or not name:
            raise RadioError("Radio returned invalid device identity")
        parts = mac.split(":")
        if len(parts) != 6 or any(
            len(part) != 2 or any(c not in "0123456789abcdefABCDEF" for c in part)
            for part in parts
        ):
            raise RadioError("Radio returned an invalid MAC address")
        return mac.lower(), name

    async def select(self, station: int, url: str | None = None) -> RadioStatus:
        if type(station) is not int or not 0 <= station <= 4:
            raise ValueError("Invalid radio station")
        payload: dict[str, int | str] = {"station": station}
        if station == 4:
            if url is None:
                raise ValueError("Set the custom stream URL first")
            payload["url"] = validate_url(url)
        status = RadioStatus.from_json(await self._request("/api/radio", payload))
        if status.selected != station or (station == 4 and status.custom_url != payload["url"]):
            raise RadioError("Radio did not confirm the requested selection")
        return status
