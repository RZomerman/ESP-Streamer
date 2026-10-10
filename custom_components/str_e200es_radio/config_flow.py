"""Connect to and identify a real local radio."""

from ipaddress import IPv4Address

import voluptuous as vol
from homeassistant import config_entries
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from .api import RadioApi, RadioError
from .const import DOMAIN


class ConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def async_step_user(self, user_input=None):
        errors = {}
        if user_input is not None:
            try:
                address = IPv4Address(user_input["host"].strip())
                if address.is_unspecified or address.is_multicast or int(address) == 0xFFFFFFFF:
                    raise ValueError("Not a unicast IPv4 address")
                host = str(address)
            except ValueError:
                errors["host"] = "invalid_host"
            else:
                api = RadioApi(async_get_clientsession(self.hass), host, user_input["port"])
                try:
                    await api.status()
                    mac, name = await api.identity()
                except RadioError:
                    errors["base"] = "cannot_connect"
                else:
                    await self.async_set_unique_id(mac)
                    self._abort_if_unique_id_configured(
                        updates={"host": host, "port": user_input["port"]},
                    )
                    return self.async_create_entry(
                        title=name, data={"host": host, "port": user_input["port"], "mac": mac},
                    )
        return self.async_show_form(
            step_id="user",
            data_schema=vol.Schema({
                vol.Required("host", default=(user_input or {}).get("host", "172.16.5.21")): str,
                vol.Required("port", default=(user_input or {}).get("port", 80)): vol.All(
                    vol.Coerce(int), vol.Range(min=1, max=65535),
                ),
            }),
            errors=errors,
        )
