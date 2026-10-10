"""Common coordinator entity and device association."""

from homeassistant.helpers.entity import DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN
from .coordinator import RadioCoordinator


class RadioEntity(CoordinatorEntity[RadioCoordinator]):
    _attr_has_entity_name = True

    def __init__(self, coordinator: RadioCoordinator, entry, key: str) -> None:
        super().__init__(coordinator)
        self._attr_unique_id = f"{entry.data['mac']}_{key}"
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, entry.data["mac"])},
            name=entry.title,
            manufacturer="DIY ESP32",
            model="ESP32-S3 Internet Radio / AirPlay",
            configuration_url=coordinator.api.base_url,
        )
