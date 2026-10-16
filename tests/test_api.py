"""Exercise the real HTTP client against a local aiohttp server."""

import importlib.util
from pathlib import Path
import sys
import unittest

import aiohttp
from aiohttp import web

SPEC = importlib.util.spec_from_file_location(
    "radio_api", Path(__file__).parents[1] / "custom_components" / "str_e200es_radio" / "api.py",
)
api = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = api
SPEC.loader.exec_module(api)


def status(**changes):
    return {
        "selected": 0, "station": "NPO Radio 2", "source": "Radio",
        "custom_url": "", "radio_playing": True, "error": "", **changes,
    }


class ValidationTests(unittest.TestCase):
    def test_valid_urls(self):
        for url in ("http://example.com/live.mp3", "https://example.com:443/live?a=1"):
            self.assertEqual(api.validate_url(" " + url + " "), url)

    def test_invalid_urls(self):
        for url in (
            "", "ftp://example.com", "https:///live", "http://",
            "https://user:password@example.com/live", "https://example.com/a#fragment",
            "https://example.com/a b", "https://example.com/é",
            "https://example.com\\live", "https://example.com:99999/live",
            "https://example.com/" + "x" * 512,
        ):
            with self.subTest(url=url), self.assertRaises(ValueError):
                api.validate_url(url)

    def test_valid_status(self):
        self.assertEqual(api.RadioStatus.from_json(status()).selected, 0)

    def test_invalid_status(self):
        for data in ([], {}, status(selected=True), status(selected=5),
                     status(radio_playing="yes"), status(source="Unknown"),
                     status(error=None), status(station=None)):
            with self.subTest(data=data), self.assertRaises(api.RadioError):
                api.RadioStatus.from_json(data)

    def test_airplay_preserves_selection(self):
        parsed = api.RadioStatus.from_json(status(source="AirPlay", radio_playing=False, selected=2))
        self.assertEqual(parsed.selected, 2)
        self.assertEqual(parsed.source, "AirPlay")


class HttpTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.state = status()
        self.requests = []
        self.http_status = 200
        self.bad_json = False
        self.confirm = True
        app = web.Application()
        app.router.add_route("*", "/api/radio", self.handle)
        app.router.add_get("/api/system/info", self.identity)
        self.runner = web.AppRunner(app)
        await self.runner.setup()
        server = web.TCPSite(self.runner, "127.0.0.1", 0)
        await server.start()
        port = self.runner.addresses[0][1]
        self.session = aiohttp.ClientSession()
        self.client = api.RadioApi(self.session, "127.0.0.1", port)

    async def asyncTearDown(self):
        await self.session.close()
        await self.runner.cleanup()

    async def handle(self, request):
        if self.http_status != 200:
            return web.Response(status=self.http_status)
        if self.bad_json:
            return web.Response(text="{", content_type="application/json")
        if request.method == "POST":
            body = await request.json()
            self.requests.append(body)
            if self.confirm:
                self.state["selected"] = body["station"]
                if "url" in body:
                    self.state["custom_url"] = body["url"]
        return web.json_response(self.state)

    async def identity(self, request):
        return web.json_response({"info": {"mac": "28:84:85:49:EC:C8", "device_name": "STR-E200ES"}})

    async def test_get_and_identity(self):
        self.assertEqual((await self.client.status()).station, "NPO Radio 2")
        self.assertEqual(await self.client.identity(), ("28:84:85:49:ec:c8", "STR-E200ES"))
        self.assertEqual(self.requests, [])

    async def test_four_presets(self):
        for station in range(4):
            self.assertEqual((await self.client.select(station)).selected, station)
        self.assertEqual(self.requests, [{"station": i} for i in range(4)])

    async def test_custom_stream(self):
        result = await self.client.select(4, "https://example.com/live.mp3")
        self.assertEqual(result.custom_url, "https://example.com/live.mp3")

    async def test_airplay_queued_command(self):
        self.state.update(source="AirPlay", radio_playing=False)
        result = await self.client.select(3)
        self.assertEqual(result.source, "AirPlay")
        self.assertEqual(result.selected, 3)

    async def test_v2_airplay_conflict(self):
        self.state.update(source="AirPlay", radio_playing=False)
        before = self.state.copy()
        self.http_status = 409
        with self.assertRaisesRegex(api.RadioError, "HTTP 409"):
            await self.client.select(4, "https://example.com/new.mp3")
        self.assertEqual(self.state, before)

    async def test_v2_status_extra_fields(self):
        self.state.update(
            protocol_version=2, radio_changes_blocked=True,
            source="AirPlay", radio_playing=False,
            now_playing={"title": "Example track", "codec": "ALAC"},
            sony_pcm_enabled=False, dit_pcm_enabled=True,
            dit={"running": True, "sample_rate": 44100, "mclk_hz": 11289600,
                 "bclk_hz": 2822400, "pcm_bits": 16, "container_bits": 24,
                 "slot_bits": 32, "write_errors": 0, "queue_overflows": 0,
                 "starvation_resets": 0, "frames_submitted": 44100},
        )
        result = await self.client.status()
        self.assertEqual(result.source, "AirPlay")
        self.assertFalse(result.radio_playing)

    async def test_http_failure(self):
        self.http_status = 500
        with self.assertRaises(api.RadioError):
            await self.client.status()

    async def test_malformed_json(self):
        self.bad_json = True
        with self.assertRaises(api.RadioError):
            await self.client.status()

    async def test_unconfirmed_selection(self):
        self.confirm = False
        with self.assertRaises(api.RadioError):
            await self.client.select(2)

    async def test_invalid_commands_never_sent(self):
        for station, url in ((5, None), (True, None), (4, None), (4, "")):
            with self.assertRaises(ValueError):
                await self.client.select(station, url)
        self.assertEqual(self.requests, [])

    async def test_connection_failure(self):
        await self.runner.cleanup()
        with self.assertRaises(api.RadioError):
            await self.client.status()
