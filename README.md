# ESP-Streamer

An ESP32-S3 internet radio and AirPlay receiver, with an optional Home Assistant
integration. This repository contains **two separate parts**:

| | ESP-Streamer Core | ESP-Streamer integration for Home Assistant |
|---|---|---|
| Runs on | ESP32-S3 | Home Assistant |
| Purpose | Plays internet radio and receives AirPlay audio | Controls an existing Core device over its local HTTP API |
| Features | Radio presets, custom MP3/AAC streams, GPIO14 optical S/PDIF, playback LED, webpage with song/stream information | Four preset buttons, custom URL input/play button, station/source/playback sensors |
| Needs Home Assistant? | **No** | **Yes**, plus a compatible Core device |
| Installation | Build and flash firmware; install its SPIFFS webpage | Install through HACS, restart Home Assistant, add the device |
| Updates | Firmware and webpage deployment to the ESP | HACS update and Home Assistant restart |
| Source | [`firmware/`](firmware/) | [`custom_components/str_e200es_radio/`](custom_components/str_e200es_radio/) |
| Guide | [Core setup, hardware and updates](firmware/README-RADIO.md) | [Home Assistant setup, controls and updates](docs/HOME-ASSISTANT.md) |
| License | [Non-commercial upstream license](firmware/LICENSE) | [MIT](LICENSE) |

## 1. ESP-Streamer Core

The standalone radio: it starts **NPO Radio 2** on boot, offers **BNR Radio**,
**Radio 538** and **SLAM! FM**, and accepts a custom direct MP3/AAC stream URL.
AirPlay takes over the output while connected; radio resumes after disconnect.

Control Core directly through its own webpage, including live song metadata
when supplied by the station or AirPlay app. Optical output is fixed at
48 kHz stereo, 16-bit PCM on GPIO14.

- [Hardware, build, installation and update guide](firmware/README-RADIO.md)
- [Upstream attribution, improvements and validation limitations](firmware/PROVENANCE.md)
- [V2 development: UART controllers, AirPlay override and revised wiring](firmware/V2.md)

**Home Assistant and HACS are not required.** The device's default AirPlay name
remains **STR-E200ES**. ESP32-S3 does not support Bluetooth Classic/A2DP audio.

## 2. ESP-Streamer integration for Home Assistant

An optional local control layer for Core. It adds station buttons, custom
stream controls and status sensors to Home Assistant, without replacing Core's
webpage or audio functions.

- [HACS installation and configuration](docs/HOME-ASSISTANT.md#install-via-hacs)
- [Controls and example dashboard](docs/HOME-ASSISTANT.md#controls)
- [Integration updates and removal](docs/HOME-ASSISTANT.md#update-or-remove-the-integration)

For compatibility, the current HACS/Home Assistant listing is still named
**STR-E200ES Radio**. The integration domain and existing entity IDs are unchanged.

## Which part should I install or update?

- **Building a new radio:** start with the [Core guide](firmware/README-RADIO.md).
- **Adding a working radio to Home Assistant:** use the
  [integration guide](docs/HOME-ASSISTANT.md).
- **Updating radio, AirPlay or the device webpage:** update
  [Core](firmware/README-RADIO.md#updating-core).
- **Updating Home Assistant controls:** update the
  [integration through HACS](docs/HOME-ASSISTANT.md#update-or-remove-the-integration).

**HACS never flashes the ESP or updates its webpage.** Updating Core does not
install or update Home Assistant entities. These are separate deployments,
even though their source is maintained in one repository.

Use both parts on a trusted LAN; the Core HTTP API is unauthenticated and
must not be exposed directly to the Internet.
