# airplay-esp32-mqtt

**A ~$10 ESP32-S3 board that joins your HomePods as a real AirPlay 2
multi-room speaker, and shows up in Home Assistant by itself.**

A fork of [rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32)
(the ESP32 AirPlay 2 receiver) with MQTT now-playing, Home Assistant
discovery, cover art and remote control back to the sender.

## Why this firmware

- **Real AirPlay 2 multi-room on a microcontroller.** Group it with HomePods
  in the Home app or Control Center and it plays in sync. Upstream does the
  AirPlay 2 work; this fork keeps all of it.
- **A room speaker in Apple Home, so Siri includes it.** Add it in the Home
  app and assign it to a room, like a HomePod. Siri on a HomePod or home hub
  then includes it in "play music everywhere" and "play music in the
  <room>". This is upstream's HomeKit pairing
  ([airplay-esp32#228](https://github.com/rbouteiller/airplay-esp32/pull/228)
  by [@emlynmac](https://github.com/emlynmac)), ported onto this fork. See
  [Apple Home](#apple-home) for the setup and its limits.
- **Appears in Home Assistant automatically.** With MQTT discovery you get one
  device, with title, artist, album, play state, position, duration and cover
  art, with no YAML to write. This also works when the receiver is grouped and
  the stream comes from an Apple TV or home hub, not from the phone.
- **Control from HA:** Play/Pause, Next and Previous buttons, plus a volume
  slider.
- **Controls the sender, not just itself.** Pause or Next in HA pauses or skips
  the actual iPhone or Apple TV session. It uses the AirPlay 2 event channel, so
  it needs no DACP-ID and no MRP pairing.
- **Recovers on its own.** A crash log survives a reset, a boot guard rolls back
  to the previous OTA slot after a crash loop, and `/api/system/info` reports
  the last crash.
- **Costs nothing when off.** MQTT is disabled by default, and no broker,
  credentials or hostnames are compiled in.

> TODO: screenshot of the Home Assistant device card (to add with a neutral
> device name).

### How it compares

Checked against each project's own docs and code on 2026-11-27. "Not built in"
means the feature is not in the project itself; it may be possible with add-ons,
scripts or another integration.

| | **airplay-esp32-mqtt** | [airplay-esp32](https://github.com/rbouteiller/airplay-esp32) v0.3.0 | [Shairport Sync](https://github.com/mikebrady/shairport-sync) 5.6 on a Raspberry Pi | [squeezelite-esp32](https://github.com/sle118/squeezelite-esp32) | Typical commercial AirPlay boards |
|---|---|---|---|---|---|
| Hardware | ESP32-S3 + I2S DAC | ESP32 / S2 / S3 / C5 | Linux computer (Raspberry Pi B or better) | ESP32 / ESP32-S3 | Vendor module |
| AirPlay 2 multi-room with HomePods | Yes (from upstream) | Yes | Yes (AirPlay 2 build, with documented limitations) | No, AirPlay 1 only | Usually yes |
| MQTT now-playing | Yes | Not built in | Yes (`--with-mqtt-client`) | Not built in | Varies by vendor; usually an app or HTTP API |
| Home Assistant auto-discovery | Yes: sensors, image, buttons, volume number | Not built in | Yes: sensors and binary sensors | Not built in | Varies; some have HA integrations |
| Cover art in HA | Yes, MQTT `image` entity (served from PSRAM) | Not built in (used for displays) | Published as binary on MQTT; not part of discovery | Not built in | Varies |
| Remote control of the AirPlay 2 sender | Yes, event channel | In `staging` (PR #226), not yet released | Yes (MQTT `remote` topic) | AirPlay 1 only | Varies |
| Add to Apple Home as a room speaker | Yes (upstream #228, ported) | In `staging` (PR #228), not yet released | Yes (see its `ADDINGTOHOME.md`) | No | Usually yes |
| Crash log / auto-rollback | Yes | Not built in | n/a (Linux service) | Not built in | Closed firmware |
| Open source | GPL-3.0-or-later | GPL-3.0-or-later | Yes (see its LICENSES file) | Yes (see its repo for licenses) | Usually closed |

Where this fork fits: Shairport Sync already does MQTT, HA discovery and
remote control very well, but needs a Linux computer. Upstream airplay-esp32
gets AirPlay 2 onto a $10 microcontroller, but has no Home Assistant side. This
fork does both on the microcontroller.

## Project facts

- Based on upstream tag **v0.3.0** (ESP-IDF **6.1**). The upstream history is
  kept, so `git log v0.3.0..` shows exactly what this fork adds.
- Firmware version strings look like `0.3.0-mqtt.N`.
- License: **GPL-3.0-or-later** with the upstream `LICENSE-EXCEPTION`, the same
  as upstream (see [License](#license)).
- Everything upstream does still works the same. The upstream README is kept as
  [README.upstream.md](README.upstream.md), and the upstream documentation
  site applies: <https://rbouteiller.github.io/airplay-esp32/>.

> **Tested hardware:** one generic ESP32-S3-WROOM-1 board (16 MB flash, 8 MB
> octal PSRAM, I2S DAC). Senders tested: an iPhone playing directly, an iPhone
> group with a HomePod, and an Apple home-hub group with two HomePods. Other
> boards should work wherever upstream works, but have not been tested.
> Release builds are produced by CI; see each release's notes for whether that
> exact build has run on hardware.

## What it adds

| Feature | Summary | Code |
|---|---|---|
| MQTT now-playing | Publishes play state, title, artist, album, genre, position, duration, volume as retained JSON. Home Assistant MQTT discovery creates the device and entities. | `main/mqtt_nowplaying.c` |
| Command topics | Play/pause/next/previous and volume from MQTT (and HA buttons/slider). | `main/mqtt_nowplaying.c` |
| Cover art | Artwork received over AirPlay is kept in **PSRAM** (up to 256 KB) and served over HTTP; HA shows it through an MQTT `image` entity. Also works while the receiver is in a multi-room group. | `main/mqtt_nowplaying.c`, `main/rtsp/rtsp_handlers.c`, `main/network/mdns_airplay.c` |
| Remote control over the AirPlay 2 event channel | Sends MediaRemote commands to the sender, so play/pause/next work even when the sender gives no DACP-ID (normal with AirPlay 2). | `main/event_ctrl.c`, `main/hap/*` |
| Apple Home pairing | HomeKit pair-setup/verify, a pairing store in NVS, `/pair-add`, `/pair-remove`, `/pair-list` and `/configure`, so the Home app can add the speaker to a room. Ported from upstream [#228](https://github.com/rbouteiller/airplay-esp32/pull/228) (by @emlynmac). | `main/hap/*`, `main/network/mdns_airplay.c`, `main/rtsp/rtsp_handlers.c` |
| Crash log | The last ~3 KB of log text and a panic record (reason, PC, task names, backtrace of both cores) survive a reset in RTC RAM. | `main/crash_log.c` |
| Boot guard | After 3 consecutive crash boots, switches back to the other OTA slot (the firmware it was updated from). | `main/boot_guard.c` |
| Generic board config | `config/sdkconfig.defaults.esp32s3-generic-mqtt`: DIO flash mode and SNTP via DHCP. | `config/` |

### About the event-channel remote control

Upstream has since merged an implementation of the same feature:
[rbouteiller/airplay-esp32#226](https://github.com/rbouteiller/airplay-esp32/pull/226)
(merged into `staging`, not yet in a release when this fork was made). It uses
the same key derivation and the same `POST /command` message. **Once a release
contains #226, this fork will drop `event_ctrl.c` and its `hap/*` changes and
call the upstream implementation from the MQTT command handler instead.**

## Apple Home

Ported from upstream
[rbouteiller/airplay-esp32#228](https://github.com/rbouteiller/airplay-esp32/pull/228)
by [@emlynmac](https://github.com/emlynmac) (merged into upstream `staging`,
not yet in an upstream release). Upstream's full guide:
[docs/features/homekit.md](docs/features/homekit.md).

**Setup** (AirPlay 2 mode, the default):

1. Home app → **Home Settings → Speakers & TV**: set **Allow Speaker & TV
   Access** to **Anyone on the Same Network** (or **Everyone**), and turn
   **Require Password** off. The speaker can't be added otherwise. This is a
   home-wide setting: anyone on your network can then stream to your speakers.
2. **+ → Add Accessory → More options…**, pick the speaker, choose a room.
   No setup code is asked for.

**Tested on hardware** (one ESP32-S3 board, 2026-11-27): added to a room that
also holds two HomePods. Siri on an Apple home hub played to it with "play
music in the <room>" and with "play music everywhere", in sync with the
HomePods. MQTT now-playing kept working.

**Known limits:**

- **"Controls not available" on the Home tile.** The Home app shows no track,
  artwork, play/skip or volume for the speaker. That needs Apple's MediaRemote
  protocol, which no open AirPlay receiver implements (Shairport Sync has the
  same limit). Siri, the AirPlay picker, Control Center and Home Assistant
  still control playback.
- **Group membership seen from Home Assistant.** When Siri starts playback,
  the home hub's AirPlay output list (as read by pyatv-based integrations,
  e.g. a `group_members` attribute) did not include this speaker, even though
  it was playing. Automations that track "is the speaker in the hub's group"
  from that list will miss Siri-started sessions. The speaker's own MQTT play
  state is reliable.
- The model advertised over mDNS changes from `AudioAccessory5,1` (HomePod
  mini) to `airplay-esp32`, as upstream does; the Home app doesn't list the
  speaker otherwise. The AirPlay picker then shows a generic speaker icon.
- No speaker password; access control is advertised, not enforced; pairing
  uses the shared AirPlay code. Same as upstream and Shairport Sync.
- `GET /api/homekit` shows the stored pairings; `POST /api/homekit/reset`
  forgets them (remove the speaker in the Home app too).

## Home Assistant

### Topics

`<base>` defaults to `airplay/<last 3 bytes of the Wi-Fi MAC, hex>`, for
example `airplay/a1b2c3`. It can be changed in the settings.

| Topic | Direction | Payload |
|---|---|---|
| `<base>/state` | published, retained | JSON, see below. Published on every change, and every 5 s while playing. |
| `<base>/status` | published, retained (LWT) | `online` / `offline` |
| `<base>/artwork_url` | published, retained | `http://<receiver-ip>/api/nowplaying/artwork?v=<n>`. With no artwork, the URL serves a 1×1 transparent PNG, so the HA image clears. |
| `<base>/cmd` | subscribed | `play_pause`, `play`, `pause`, `next`, `prev`, `stop`, `volume_up`, `volume_down` |
| `<base>/volume/set` | subscribed | `0`–`100` |
| `<base>/cmd/exp_event` | subscribed (testing) | `<n>` (0 play, 1 pause, 2 toggle, 3 stop, 4 next, 5 previous) tries each event-channel message form until one is accepted; `<n>/<form>` sends only form 0, 1 or 2. |

How commands reach the sender:
- If the sender gave a DACP-ID and its DACP service is found, the command goes
  through upstream's DACP client.
- Otherwise, if an AirPlay 2 event-channel client is attached, `play`, `pause`,
  `play_pause`, `next`, `prev` and `stop` go over the event channel.
- `volume/set` goes to the sender over DACP when available. Otherwise it is
  applied locally (as the hardware buttons do), and saved at pause or session
  end, not on every step.

### State JSON

```json
{
  "state": "playing",
  "source": "airplay",
  "connected": true,
  "title": "…", "artist": "…", "album": "…", "genre": "…",
  "duration": 215, "position": 42,
  "position_ts": 1764200000.123,
  "position_updated_at": "2026-11-27T00:00:00Z",
  "volume": 60, "volume_level": 0.6, "muted": false,
  "dacp": false, "dacp_id_seen": false, "event_ctrl": true,
  "boot_crashes": 0,
  "artwork": "http://<receiver-ip>/api/nowplaying/artwork?v=3"
}
```

- `state`: `playing`, `paused` or `idle`.
- `position_ts` / `position_updated_at`: the wall-clock moment at which
  `position` was true (epoch seconds and ISO-8601 UTC). They pair with
  `position` the way HA's `media_position_updated_at` expects. They are `null`
  until SNTP has synced.
- `dacp`: commands can go over DACP right now. `dacp_id_seen`: the sender sent
  a DACP-ID header at all. `event_ctrl`: no DACP, but the event channel can
  control the sender.
- `boot_crashes`: consecutive crash boots counted before this boot.
- `artwork`: `null` when no image is held.

### Discovered entities

One device, `identifiers: airplay_esp32_<12 hex digits of the Wi-Fi MAC>`,
named after the receiver's AirPlay name. Discovery topics are
`<prefix>/<component>/<device id>/<object>/config`, with `<prefix>` defaulting
to `homeassistant`.

| Entity | Component | Notes |
|---|---|---|
| Playback state | `sensor` | All state JSON fields are also attributes |
| Title, Artist, Album | `sensor` | |
| Position, Duration | `sensor` | seconds, `device_class: duration` |
| Volume | `number` | 0–100 slider, writes `volume/set` |
| Artwork | `image` | `url_topic: <base>/artwork_url` |
| Play/Pause, Next, Previous | `button` | write `cmd` |

All entities use `<base>/status` for availability.

## Setting up MQTT

MQTT settings are stored in NVS (namespace `mqtt_np`, keys `en art host port
user pass base prefix`). Nothing is compiled in: the broker host is empty and
MQTT is disabled until you set it.

**Web page:** open `http://<receiver-ip>/mqtt`. Fill in the broker host,
port, username and password, tick *Enabled*, and save. The client reconnects
with the new settings straight away. The password is write-only: the page shows
only whether one is set, and an empty field keeps the stored one.

**JSON API:**

```bash
curl -s http://<receiver-ip>/api/mqtt
```

```bash
curl -s -X POST http://<receiver-ip>/api/mqtt -H 'Content-Type: application/json' -d '{"enabled":true,"host":"mqtt.example.lan","port":1883,"username":"<user>","password":"<password>","base_topic":"airplay/living_room","discovery_prefix":"homeassistant","artwork":true}'
```

Omitted keys keep their stored value.

**Cover art** (`artwork`, default on) is read at boot, because the mDNS `md`
record and the AirPlay feature bit 15 are only advertised then. After you
change it, restart the receiver; the POST reply says
`"restart_needed_for_artwork": true` when that is needed. Upstream warns that
artwork can cause drop-outs on realtime (AirPlay 1) streams; if you hear them,
turn artwork off. Cover art is also received when upstream's compile-time
`CONFIG_ENABLE_AIRPLAY_ARTWORK` is set (display builds), as before.

> The receiver's web server has no authentication (as upstream). Anyone on the
> same network can change these settings or upload firmware. Keep it on a
> trusted network segment.

### Other HTTP endpoints

| Endpoint | Returns |
|---|---|
| `GET /api/nowplaying` | The state JSON above |
| `GET /api/nowplaying/artwork` | The current cover art (JPEG/PNG), or a 1×1 transparent PNG |
| `GET /api/system/crashlog` | Plain text: the previous boot's log tail and panic record (read-only) |
| `GET /api/system/info` | Upstream's fields plus `crashlog` (bool), `crashlog_crash` (bool), `crashlog_reason` (string) |

The crash log does not survive power loss or a brown-out reset (RTC RAM is
cleared). To decode a backtrace:

```bash
xtensa-esp32s3-elf-addr2line -pfiaC -e build/airplay2-receiver.elf 0x4200…
```

## Building

Needs ESP-IDF **v6.1**, like upstream v0.3.0. The path must not contain spaces
(one of the precompiled components breaks the link step).

```bash
git clone --recurse-submodules https://github.com/FredLiuuuu/airplay-esp32-mqtt.git
```

```bash
cd airplay-esp32-mqtt && idf.py -D "SDKCONFIG_DEFAULTS=config/sdkconfig.defaults;config/sdkconfig.defaults.esp32s3;config/sdkconfig.defaults.esp32s3-generic-mqtt" set-target esp32s3 build
```

The app image is `build/airplay2-receiver.bin`. The MQTT features build on any
upstream board config: add your board's defaults file to `SDKCONFIG_DEFAULTS`
instead of the generic one. Cover art needs PSRAM; without PSRAM it is simply
not stored.

### Flash mode

`sdkconfig.defaults.esp32s3-generic-mqtt` sets **DIO**. Many ready-made
ESP32-S3 receiver boards ship with a bootloader that runs the flash in DIO, and
web OTA only replaces the app, not the bootloader. Keep app and bootloader in
the same mode. If you flash the whole image (bootloader included) over serial,
QIO is fine.

### Updating an existing receiver

Upload `airplay2-receiver.bin` on the web UI's firmware update page (it posts
to `/api/ota/update`). The NVS settings (Wi-Fi, name, volume, MQTT) are kept.
Keep a way back: a serial adapter, or the previous `.bin` to upload again. The
boot guard switches back to the previous slot after three crash boots in a row,
but it cannot help with a hang that does not reset.

CI (`.github/workflows/mqtt-build.yml`) builds the generic ESP32-S3 image on
every push and pull request and uploads the app image, the merged full-flash
image and the ELF as artifacts.

## Relationship to upstream

The plan is to keep this fork thin:
- Apple Home pairing is upstream #228, ported as-is (its `main/` and `tests/`
  changes). It will come in with the next upstream release.
- The event-channel remote control will be replaced by upstream #226 once it is
  released (see above).
- The MQTT/Home Assistant part may be proposed upstream as an opt-in feature.
  Until then it lives here.
- Upstream releases will be merged in, and the version suffix reset
  (`<upstream>-mqtt.N`).

The AirPlay 2 event-channel message shapes and key labels were worked out from
[Shairport Sync](https://github.com/mikebrady/shairport-sync)'s development
branch, which upstream already lists as a protocol reference. File-level notes
are at the top of `main/event_ctrl.c`.

## License

GPL-3.0-or-later, the same as upstream. See [LICENSE](LICENSE),
[LICENSE-EXCEPTION](LICENSE-EXCEPTION) (the upstream exception for Espressif's
precompiled components) and [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
The firmware binaries link `espressif/mqtt` (Apache-2.0) in addition to the
upstream components.

AirPlay, HomePod and Apple TV are trademarks of Apple Inc. This project is not
affiliated with or endorsed by Apple.
