"""Read-only radio selection, ownership and error status."""

from homeassistant.components.sensor import SensorEntity

from .const import DOMAIN
from .entity import RadioEntity


async def async_setup_entry(hass, entry, async_add_entities):
    coordinator = hass.data[DOMAIN][entry.entry_id]
    async_add_entities([
        RadioSensor(coordinator, entry, key, name, icon)
        for key, name, icon in (
            ("station", "Selected station", "mdi:radio"),
            ("source", "Audio source", "mdi:cast-audio"),
            ("status", "Playback status", "mdi:play-circle"),
        )
    ])


class RadioSensor(RadioEntity, SensorEntity):
    def __init__(self, coordinator, entry, key: str, name: str, icon: str) -> None:
        super().__init__(coordinator, entry, key)
        self.key = key
        self._attr_name = name
        self._attr_icon = icon

    @property
    def native_value(self) -> str:
        data = self.coordinator.data
        if self.key == "station":
            return data.station
        if self.key == "source":
            return data.source
        if data.source == "AirPlay":
            return "AirPlay connected"
        if data.error:
            return "Stream error"
        return "Playing" if data.radio_playing else "Buffering"

    @property
    def extra_state_attributes(self):
        if self.key == "status":
            return {"stream_error": self.coordinator.data.error}
        return None
