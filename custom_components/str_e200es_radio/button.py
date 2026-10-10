"""Four preset buttons and a custom stream playback button."""

from homeassistant.components.button import ButtonEntity

from .const import DOMAIN, PRESETS
from .entity import RadioEntity


async def async_setup_entry(hass, entry, async_add_entities):
    coordinator = hass.data[DOMAIN][entry.entry_id]
    async_add_entities([
        RadioButton(coordinator, entry, station, name)
        for station, name in enumerate((*PRESETS, "Play custom stream"))
    ])


class RadioButton(RadioEntity, ButtonEntity):
    _attr_icon = "mdi:radio"

    def __init__(self, coordinator, entry, station: int, name: str) -> None:
        super().__init__(coordinator, entry, f"station_{station}")
        self.station = station
        self._attr_name = name

    async def async_press(self) -> None:
        url = None
        if self.station == 4:
            url = self.coordinator.entry.options.get(
                "custom_url", self.coordinator.data.custom_url,
            )
        await self.coordinator.async_select(self.station, url)
