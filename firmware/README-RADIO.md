# ESP32-S3 Internet Radio + AirPlay

Hobby/non-commercial adaptation of [rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32).
The upstream license and third-party notices remain applicable.

## Hardware

- ESP32-S3 N16R8: 16 MB flash, 8 MB octal PSRAM.
- Optical S/PDIF data on GPIO14, fixed at 48 kHz, stereo, 16-bit PCM.
- Serial logging/programming at 115200 baud; the original adapter was COM6,
  and native USB Serial/JTAG enumerated as COM7 during recovery. Check the
  current port before flashing.
- No Bluetooth audio: the S3 does not have Bluetooth Classic/A2DP.

GPIO14 is a 3.3 V signal. A 74HC04 powered at 5 V does **not** guarantee
recognition of that HIGH level. Use suitable level conversion or a compatible
HCT input stage; do not connect 5 V to the ESP GPIO.

## Use

Open the device's IP address in a browser:

- `/`: station buttons, custom URL form, playback/error status.
- `/settings`: upstream Wi-Fi and AirPlay receiver settings.
- `/logs`: live wireless diagnostics; look for `Gap` and queue/drop warnings.
- `/api/radio`: radio status (GET), selection (POST).

Every boot selects **NPO Radio 2**. Other presets are **BNR Radio**, **Radio 538**
and **SLAM! FM**. A custom direct HTTP/HTTPS MP3 or AAC/ADTS URL is saved to NVS.
Supported radio rates are 32, 44.1 and 48 kHz, mono or stereo. Radio is resampled
to the fixed optical output rate. Playlists, HLS, web pages, authenticated URLs
are not supported; failures appear on the
page and in serial logs.

The homepage shows **Now playing** and stream details, refreshed every 2.5 s.
Radio requests ICY metadata and removes its length bytes and metadata blocks
before MP3/AAC decoding. Station-reported `StreamTitle`, stream name and bitrate
are shown when available, alongside the decoded sample rate/channels, codec,
configured stream URL and fixed optical output format. An empty title clears
the previous song; a zero-length metadata block means no change.

AirPlay displays sender-provided title, artist and album. Progress/artwork-only
updates do not erase the song. Track data is cleared on source/session changes.
Availability depends on the station/app; the page explicitly reports missing
metadata rather than inventing a title. Radio titles reflect the latest received
live metadata and can lead buffered playback by a few seconds. Artwork and
scraping external station websites are not part of this implementation.

`/api/radio` adds a `now_playing` object with `title`, `artist`, `album`,
`stream_name`, `content_type`, `bitrate_kbps` (station-reported string), `url`,
`codec`, `sample_rate`, `channels`, `output_sample_rate` and
`metadata_supported`. Existing radio/Home Assistant fields remain unchanged.

Host parser regression test (any C compiler, run the resulting executable):

```text
cc -Wall -Wextra -Werror tests/test_icy_metadata.c main/audio/icy_metadata.c -o test_icy_metadata
```

It checks 128 network fragmentation sizes, metadata removal without audio byte
changes, empty blocks/titles, apostrophes, the maximum 4080-byte ICY block,
plain streams and missing-title metadata.

Select the receiver in an Apple device's AirPlay audio menu on the same network.
The default advertised name is **STR-E200ES**; it can be changed in `/settings`.
An AirPlay connection preempts radio. Pausing keeps AirPlay ownership; disconnecting
resumes the selected radio station. A station selected during AirPlay is queued
for that fallback. The upstream reconnect grace period can delay fallback for a
legacy AirPlay sender that still advertises its remote-control service.
Radio volume is independent of the last AirPlay volume.

The onboard addressable RGB LED (GPIO48) is solid green while decoded radio or
AirPlay audio is being output, including quiet passages. It turns off when
idle, buffering or paused (with a 250 ms hold to avoid flicker between frames).
The receiver settings' LED brightness control still applies; zero disables it.
The separate power LED, if fitted, cannot be controlled by firmware.

Windows does not natively transmit ordinary system audio over AirPlay.
Actual AirPlay discovery, audio and disconnect fallback require a sender test;
a successful firmware build or browser test alone does not verify them.

### API

```json
{"station": 0}
```

Station IDs: 0 = NPO Radio 2, 1 = BNR Radio, 2 = Radio 538, 3 = SLAM! FM,
4 = custom. For a custom stream:

```json
{"station": 4, "url": "https://example.com/live.mp3"}
```

POST JSON to `/api/radio`. HTTP 400 means invalid input, HTTP 500 means a
server/persistence failure. A successful response accepts the selection;
connection/decode failures subsequently appear in the status `error` field.
The interface is intended for a trusted local network, not public Internet access.

## Build and USB migration

The `radio-s3` environment is checked in as `radio_platformio.ini` and is the
default PlatformIO environment. From the repository root:

```powershell
git submodule update --init --recursive
Set-Location firmware
pio run -e radio-s3
```

`user_platformio.ini` remains an optional, ignored file for local overrides.
Select the actual USB port explicitly when uploading or monitoring, for example
`pio device monitor --port COM7 --baud 115200`. Do not use a generic upload
command for an existing device until its partition layout and active OTA slot
have been checked. See the migration warning below.

Build the filesystem using `pio run -e radio-s3 -t buildfs`.
On the Windows ARM64 development host, run the x64 Python/PlatformIO installation
with `PLATFORMIO_SYSTEM_TYPE=windows_amd64`.

The installed ESP-IDF `components/spiffs/spiffsgen.py` can also generate the
filesystem directly from `data`, with image size `0x1e0000`, page size 256,
object name length 32, metadata length 4, magic and magic length enabled.
This is the generator used for the initial local migration after the firmware
build passed; the PlatformIO filesystem target stalled in reconfiguration.

If an old generated `sdkconfig.radio-s3` exists, regenerate it when changing
board defaults. Verify the generated configuration selects S/PDIF, GPIO14,
48 kHz, octal PSRAM and DIO flash before uploading.

**Do not install this application through the old standalone radio's OTA.**
The radio partition table retains its NVS at `0x9000` (size `0x5000`),
OTA data at `0xe000`, and first application at `0x10000`, but enlarges the
application slots and relocates SPIFFS. Initial migration must install the
matching bootloader, partition table, application and filesystem over USB.
Do not erase the whole flash. Existing `npo-streamer/wifi` credentials are
imported locally into the AirPlay settings on first boot.

Radio layout:

| Region | Offset | Size |
|---|---|---|
| NVS | `0x9000` | `0x5000` |
| OTA data | `0xe000` | `0x2000` |
| Application 0 | `0x10000` | `0x300000` |
| Application 1 | `0x310000` | `0x300000` |
| SPIFFS | `0x610000` | `0x1e0000` |

Keep the known-good standalone firmware and its matching partition/bootloader
images for recovery. Future OTA updates must use this new partition layout.

## Implementation and validation

One playback task owns I2S DMA for both sources. Radio has separate network and
decoder tasks, a 192 KiB compressed PSRAM buffer and a 128 KiB PCM queue.
Generation tags discard stale audio on station changes and AirPlay transitions.
HTTPS uses the ESP-IDF certificate bundle.

Realtime AirPlay UDP reception runs separately from ALAC decoding: a short,
network-core task copies packets into a bounded 512 KiB PSRAM queue and releases
the network buffers immediately. A separate network-core decoder alone owns
decryption, sequence recovery and the codec, leaving the playback core free
for resampling/output and timing-control packets.
The packet queue is drained between sessions; overflow is counted and logged.
The S/PDIF read buffer includes the extra stereo frame required for the AirPlay
clock servo to both stretch and shorten audio.
Seek detection compares anchor mappings on their sender clock timeline, so
delayed or repeated historical timing packets do not falsely flush playback
and arm a stale timestamp gate. Compile-time checks cover historical updates,
fractional rates, RTP wraparound and real forward/backward seeks.

`/api/radio` includes an `airplay` diagnostic object. `stream_type` is the
configured transport (96 = realtime UDP, 103 = buffered TCP); `playing` also
requires AirPlay ownership. Counters reset on stream start:

- `packets_received`: datagrams/packets received, including retransmissions.
- `chunks_decoded`: PCM chunks queued, not necessarily one per packet.
- `packets_dropped`: detected sequence holes and rejected packets; recovered
  sequence holes remain counted, so this is not an unrecovered-loss total.
- `decrypt_errors`, `decoder_errors`, `packet_queue_overflows`,
  `buffer_overflows`, `buffer_underflows`, and `late_frames`: distinct failure
  counters. Buffer underflows can occur while waiting for startup audio.
- `gaps`: playback continuity holes, reset with the timing state (including
  flush/seek). Compare changes during uninterrupted playback, not track seeks.
- `buffered_frames`: currently queued PCM chunks.

The existing top-level decoder/underflow counters describe **radio**, not
AirPlay. `spdif_errors` counts failed/short DMA writes. The independent
`spdif_dma_underruns` counts observations where hardware completion has overtaken
submitted audio. It includes startup/source-transition empty descriptors;
compare increments during steady playback rather than treating a nonzero
boot total as proof of an audible interruption.

The S/PDIF encoder uses MSB-first DMA words, B/M/W preambles, even parity and
48 kHz channel status. Compile-time checks decode two complete 192-frame blocks
and test extreme sample values, preamble polarity, PCM, parity and channel status.
They are compiled with the S/PDIF backend.

The S/PDIF DMA queue holds 48 ms (12 descriptors, each containing 384 I2S
frames = 192 stereo audio frames). TX completion callbacks measure the live
output queue for AirPlay timing instead of assuming it is always full.
This covers brief scheduling/logging stalls that exhausted the former 16 ms
queue despite a full AirPlay PCM buffer. The ISR uses a short ESP-IDF critical
section for its 32-bit cursor; modulo differences remain bounded by the queue.

Before calling a deployment verified, test optical audio on all four presets,
custom MP3/AAC, invalid streams, rapid switching, AirPlay connect/pause/disconnect,
NPO boot default and custom URL persistence. Monitor `/api/radio` for uptime,
heap, sessions, decoder errors and buffer underflows during extended playback.

### Local hardware verification

The firmware and SPIFFS page were installed over USB, followed by the final
application update through the new layout's OTA endpoint. Verified:

- NPO boot default, Wi-Fi reuse and custom URL persistence after reboot.
- AirPlay mDNS advertisement contains `STR-E200ES`.
- All four presets and a custom Radio Paradise AAC stream decode at their
  expected input rates (approximately 44.1 or 48 kHz), resampled to 48 kHz output.
- No buffer underflows or S/PDIF write errors during the final station tests.
- Rapid station changes and HTTP 400 responses for invalid selection/URLs.
- The device serves both `/` and `/settings`; browser tests cover selection,
  custom URL submission, queued AirPlay selection, errors and mobile themes.
- User-confirmed clean AirPlay optical audio, green LED during playback,
  LED off on AirPlay pause, and automatic NPO/green-LED return on disconnect.

These checks do not constitute an unattended long-duration stability guarantee.

### AirPlay stutter fix verification (2026-10-10)

Installed the receiver/core-assignment and sender-timeline fixes through the
compatible OTA endpoint, preserving Wi-Fi settings, presets and custom URL.
The sender was Spotify on an iPhone/iPad using ALAC/UDP.

- User confirmed clean optical audio, LED off on pause, clean resume with
  green LED, and automatic NPO fallback on disconnect.
- During a five-minute capture, the steady AirPlay sample window was 245 s:
  three additional continuity gaps, versus dozens per second before the fix.
  Occasional continuity gaps remain; this is not a zero-packet-loss claim.
- No AirPlay decoding/decryption errors, packet/PCM queue overflows, buffer
  underflows, late-frame drops or failed/short S/PDIF writes in that window.
  All HTTP status polls succeeded. PCM depth remained around 250 chunks
  (roughly two seconds), rather than collapsing to empty.
- Resume continued decoding without a false seek-gate lockout.
- All four radio presets and the saved custom AAC URL decoded and progressed;
  invalid station/URL requests returned HTTP 400. Left NPO Radio 2 playing.
- Firmware build, anchor/S/PDIF compile-time checks and all 14 Home Assistant
  API regression tests passed.

### Residual Spotify stutter verification (2026-10-10)

Added measured S/PDIF completion tracking to distinguish packet/PCM problems
from an empty hardware queue. With the former 16 ms DMA queue, steady Spotify
playback had 12 additional DMA starvation observations over 180.6 s while the
PCM queue remained populated, with no decoder errors or PCM underflows.

After increasing the hardware queue to 48 ms and using its measured depth for
AirPlay timing, the steady test window was 321.1 s with **zero additional DMA
underruns**, decoding/decryption errors, packet/PCM overflows, PCM underflows,
late-frame drops or HTTP polling errors. Two continuity-gap events remained,
so this is not a zero-network-loss or long-duration guarantee. The user
confirmed clean playback across several songs. Home Assistant remained
connected; Spotify was left playing.

Build and S/PDIF/anchor compile-time checks passed, as did all 14 Home Assistant
client tests. Additional arithmetic checks covered the 48 ms capacity, the
3072-byte descriptor size (below the 4092-byte hardware limit), 32-bit cursor
rollover and starvation rebasing.

### Metadata deployment and USB recovery

The first metadata application update caused a reboot loop: the ICY parser
was freed inside the HTTP read loop, then reused and freed again. Its lifetime
now spans the entire stream read loop, with cleanup after that loop exits.
The corrected application was installed over USB into the active application
slot, with flash hash verification, preserving NVS, partitions and SPIFFS.
The updated homepage was then uploaded separately.

Post-recovery checks:

- All four presets and the saved Radio Paradise AAC stream decoded with
  advancing playback frames and zero reported decoder errors.
- NPO supplied its programme title, station name and 192 kbps MP3 format;
  Radio Paradise supplied an actual song title and 128 kbps AAC format.
- Station changes cleared stale titles. Other presets advertised ICY support
  but did not supply a title during the short observation windows.
- Uptime increased without a reset through the station tests and a further
  45 seconds of NPO playback. This is not a long-duration stability guarantee.
- The installed homepage matched the local HTML byte-for-byte. Browser checks
  confirmed live NPO information, no horizontal overflow at 390 px, dark theme,
  missing-metadata messaging and safe literal rendering of HTML-like titles.
- The ICY host parser regression tests passed again after recovery.

Actual Spotify AirPlay metadata was verified in the API and browser:
`Bossa Antigua` / Paul Desmond / `Bossa Antigua`, followed by
`Say It (Over And Over Again)` / John Coltrane Quartet /
`Ballads (Deluxe Edition - Rudy Van Gelder Remaster)`. During a 61-second
AirPlay window, decoded chunks increased by 7660 with no additional gaps,
decoder/decrypt errors, queue overflows, buffer underflows, late frames,
S/PDIF write errors or DMA underruns. The user reported clean audio during
the skip/pause/resume/fallback test; the final API observation still showed
active AirPlay, so disconnect fallback was not independently observed.
The top-level `frames_played` counter counts radio frames only; use AirPlay
`chunks_decoded` and output diagnostics when checking AirPlay progress.

A separate 61-second steady NPO window showed 26 additional DMA underrun
observations despite advancing radio frames and no decoder/write errors.
This remains a radio scheduling/output diagnostic to investigate, not evidence
of long-duration glitch-free playback. The metadata update must not be treated
as a blanket audio stability guarantee.
