"""Polling and serialized commands for one radio."""

from datetime import timedelta
import asyncio
import logging

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .api import RadioApi, RadioError, RadioStatus

_LOGGER = logging.getLogger(__name__)


class RadioCoordinator(DataUpdateCoordinator[RadioStatus]):
    def __init__(self, hass: HomeAssistant, entry: ConfigEntry, api: RadioApi) -> None:
        super().__init__(
            hass, _LOGGER, config_entry=entry, name=entry.title,
            update_interval=timedelta(seconds=5),
        )
        self.api = api
        self.entry = entry
        self.command_lock = asyncio.Lock()

    async def _async_update_data(self) -> RadioStatus:
        try:
            async with self.command_lock:
                return await self.api.status()
        except RadioError as err:
            raise UpdateFailed(str(err)) from err

    async def async_select(self, station: int, url: str | None = None) -> None:
        async with self.command_lock:
            try:
                status = await self.api.select(station, url)
            except (RadioError, ValueError) as err:
                raise HomeAssistantError(str(err)) from err
            self.async_set_updated_data(status)
