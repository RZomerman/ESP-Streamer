# Firmware provenance and improvements

This directory is a source snapshot of the STR-E200ES adaptation of
[rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32),
based on upstream commit
[`811d5f8af750096683dcec7c95d1cbc08c2f7601`](https://github.com/rbouteiller/airplay-esp32/commit/811d5f8af750096683dcec7c95d1cbc08c2f7601).
Upstream source, documentation, license and third-party notices are retained.
Upstream repository automation and agent instructions are omitted.
The u8g2 dependency is a submodule pinned to
`764ee93150d23779ea9d8a7e8cae73efb60749d4`.

The upstream [non-commercial license](LICENSE) applies to this firmware.
The Home Assistant integration's root MIT license does not apply to this
directory or override dependency licenses. This firmware is intended for
hobby/non-commercial use.

## Included local improvements

- ESP32-S3 N16R8 radio configuration with GPIO14 optical S/PDIF at 48 kHz,
  stereo, 16-bit PCM, plus encoder compile-time regression checks.
- NPO Radio 2 on boot; BNR Radio, Radio 538 and SLAM! FM presets; persistent
  custom MP3/AAC URL and a local HTTP control API.
- AirPlay receiver name STR-E200ES, radio preemption/fallback and GPIO48
  playback LED. ESP32-S3 does not support Bluetooth Classic/A2DP.
- Separate UDP capture/ALAC decode tasks on the network core, bounded PSRAM
  packet queue, sender-timeline seek detection and corrected logging varargs.
- A 48 ms S/PDIF DMA queue, measured output latency, separate write-error and
  DMA-starvation diagnostics, and clock-servo sample-stretch capacity.
- ICY metadata removal before radio decoding; AirPlay title/artist/album
  handling; now-playing API fields and a mobile-friendly song/stream webpage.
- Corrected ICY parser lifetime after the initial metadata deployment exposed
  a double-free reboot loop. The repaired image was recovered over USB.

The checked-in radio build environment replaces the development machine's
ignored, COM-port-specific configuration. No generated build output, device
flash dump, Wi-Fi credentials or local tool environments are included.

## Validation and limitations

See [README-RADIO.md](README-RADIO.md) for hardware, partition migration,
build instructions and detailed on-device verification.

Before publication, both `pio run -e radio-s3` and
`pio run -e radio-s3 -t buildfs` passed from this repository's firmware
directory using the checked-in configuration. The firmware build includes
the S/PDIF encoder and AirPlay anchor compile-time regression checks.
The ICY host tests (128 fragmentation sizes and boundary cases) and all
14 Home Assistant API tests also passed.

All four presets and a custom AAC stream decoded after USB recovery. Real
Spotify AirPlay title/artist/album updates were observed in the API/browser.
A 61-second AirPlay window had no additional continuity gaps, decode/decrypt
errors, queue overflows, buffer underflows, late frames, failed S/PDIF writes
or DMA underruns. This is not a long-duration or zero-packet-loss guarantee.

A separate steady radio window still showed DMA underrun counter increases.
Do not treat the metadata changes as proof of glitch-free radio playback.
The user reported clean skip/pause/resume/fallback behavior, but the final
API observation remained on AirPlay, so that latest disconnect fallback was
not independently observed.

No binary release is included. HACS updates only the Home Assistant
integration; it does not deploy this firmware or its SPIFFS webpage.
