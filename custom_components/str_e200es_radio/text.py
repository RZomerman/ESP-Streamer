"""A persisted URL draft; editing never interrupts playback."""

from homeassistant.components.text import TextEntity, TextMode
from homeassistant.exceptions import HomeAssistantError

from .api import validate_url
from .const import DOMAIN
from .entity import RadioEntity


async def async_setup_entry(hass, entry, async_add_entities):
    async_add_entities([StreamUrl(hass.data[DOMAIN][entry.entry_id], entry)])


class StreamUrl(RadioEntity, TextEntity):
    _attr_name = "Custom stream URL"
    _attr_icon = "mdi:link"
    _attr_mode = TextMode.TEXT
    _attr_native_min = 0
    _attr_native_max = 511

    def __init__(self, coordinator, entry) -> None:
        super().__init__(coordinator, entry, "custom_url")
        self.entry = entry

    @property
    def native_value(self) -> str:
        return self.entry.options.get("custom_url", self.coordinator.data.custom_url)

    async def async_set_value(self, value: str) -> None:
        try:
            value = validate_url(value) if value.strip() else ""
        except ValueError as err:
            raise HomeAssistantError(str(err)) from err
        self.hass.config_entries.async_update_entry(
            self.entry, options={**self.entry.options, "custom_url": value},
        )
        self.async_write_ha_state()
