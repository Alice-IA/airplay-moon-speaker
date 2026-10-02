# AirPlay Moon Speaker

ESP32-S3 AirPlay 2 speaker with a WS2812 "moon" LED strip, controllable via MQTT, Matter (HomeKit), a physical button, and a binary WebSocket protocol.

Based on [rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32), extended with:

- **MQTT control** — full LED control over MQTT (HiveMQ or your broker) with a companion webapp.
- **Matter (ESP-Matter)** — the moon appears as a color light in Apple Home / Google Home / Alexa.
- **Physical button** — system control: 1 click toggles MQTT ↔ Matter, 5 s hold factory-resets.
- **Status LED** — the moon itself shows system state (boot, WiFi, OTA) with colors.
- **Dual OTA** — safe over-the-air updates with automatic rollback.
- **Binary WebSocket** `/ws/leds` — low-level real-time LED frame streaming.

## Features

- AirPlay 2 audio receiver (ALAC/AAC, PTP clock sync)
- Bluetooth A2DP sink
- WS2812 LED strip (1–64 LEDs), built-in effects + frame streaming
- MQTT control (webapp included) **or** Matter (HomeKit) — mutually exclusive, toggle with the button
- Single-button media + system control
- Status LED colors for boot / WiFi / OTA
- Dual OTA with SHA-256 validation and rollback
- Web-based WiFi setup (captive portal)

## Hardware

| Component | This build | Notes |
|-----------|-----------|-------|
| ESP32-S3 N16R8 | ✅ | 16 MB flash, 8 MB PSRAM. Recommended (fits Matter + dual OTA). |
| ESP32-S3 N8R2 | ⚠️ | 8 MB flash, 2 MB PSRAM. Works for MQTT-only; Matter is too tight. |
| DAC | MAX98357A | I2S, mono, integrated 3.2 W amplifier. No MCLK needed. |
| LEDs | WS2812 ×10–12 | Up to 64 supported. |
| Button | 1× momentary | Active-low, between GPIO and GND. |

### Wiring (ESP32-S3 + MAX98357A + WS2812)

| MAX98357A | ESP32-S3 |
|-----------|----------|
| VIN | 5 V |
| GND | GND |
| BCLK | GPIO 15 |
| LRC (WS) | GPIO 16 |
| DIN | GPIO 7 |
| SD | GND = left, VCC = right, float = both (mixed) |

| WS2812 | ESP32-S3 |
|--------|----------|
| 5 V | 5 V |
| GND | GND |
| DATA | GPIO 10 |

| Button | ESP32-S3 |
|--------|----------|
| one leg | GPIO 0 (BOOT) or your configured GPIO |
| other leg | GND |

> All GPIOs are configurable in `menuconfig`. The button is active-low with internal pull-up (no external resistor needed on ESP32-S3).

## Requirements

- [ESP-IDF v5.5+](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)
- Python 3.8+ and `websockets` package (only for the example client)

## Installation

```bash
git clone --recursive https://github.com/Alice-IA/airplay-moon-speaker.git
cd airplay-moon-speaker
idf.py set-target esp32s3
idf.py build
idf.py flash monitor

The project ships a **`sdkconfig.defaults`** (base) with everything preconfigured for the N16R8: 16 MB flash, Octal PSRAM, dual OTA, I2S/LED/button pins, WebSocket, 32 sockets, Matter. **Do not hand-edit menuconfig for these** — they're applied automatically.

> **Important:** always do a clean reconfigure when the defaults change:
> ```bash
> idf.py fullclean
> rm sdkconfig            # PowerShell: Remove-Item sdkconfig
> idf.py set-target esp32s3
> idf.py build
> ```
> `set-target` does not overwrite an existing `sdkconfig`, so a stale one silently keeps old values (this caused repeated "flash 2 MB" and "partition table" errors).

### Board variant: N8R2 (8 MB flash)

Edit `sdkconfig.defaults` before building:
- `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` → `CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y`
- `CONFIG_AIRPLAY_AUDIO_BUFFER_FRAMES=1000` → `400`
- Use the 8 MB partition table (dual OTA 3 MB each) in `components/boards/partitions.csv`.

## First boot & WiFi setup

1. Power the board. The moon turns **orange** (booting / no WiFi).
2. On first boot (or after factory reset) it broadcasts `ESP32-AirPlay-Setup`.
3. Connect and open `http://192.168.4.1` (captive portal). Set device name + WiFi credentials.
4. The device joins your network; the moon turns **blue** for 3 s, then **off**.
5. AirPlay: the speaker appears by its device name in the iOS/macOS AirPlay menu.

## Status LED colors

The moon strip doubles as a system status light:

| Color | Meaning |
|-------|---------|
| 🟠 Orange | Booting, no WiFi / waiting for setup, or OTA in progress |
| 🔵 Blue | WiFi connected (3 s, then off) |
| 🟢 Green | OTA update succeeded (brief, before reboot) |
| 🔴 Red | OTA update failed |
| ⚫ Off | Normal operation |

## Physical button

One button drives system control (default GPIO 0 / BOOT). **System only** — no media control. AirPlay 2 uses MRP (Media Remote Protocol) for remote control, which is not implemented, so media buttons can't drive the source over AirPlay 2. (Bluetooth AVRCP media control works fully if you use the A2DP source instead.)

| Gesture | Action |
|---------|--------|
| 1 click | Toggle control mode MQTT ↔ Matter (reboots) |
| Hold 5 s | Factory reset — erases WiFi, HomeKit pairing, all settings (reboots to setup) |

## Control modes: MQTT vs Matter

**MQTT and Matter are mutually exclusive** (Matter's RAM footprint is too large to coexist). The active mode is stored in NVS and toggled with the button (1 click) or compiled default.

### MQTT mode (default)

Full-featured control via MQTT. Designed for the included webapp but works with any MQTT client.

- **Broker:** default HiveMQ public (`broker.hivemq.com:1883`, WSS `broker.hivemq.com:8884/mqtt`). Configurable.
- **Device ID:** the ESP32 MAC address without colons (e.g. `AABBCCDDEEFF`).
- **Client ID:** `moon_{device_id}`.

Topics (`moon/{device_id}/...`):

| Topic | Direction | Payload |
|-------|-----------|---------|
| `cmd/frame` | → device | Binary: frame_id(u32be) + timestamp(u32be) + RGBA pixels |
| `cmd/stream` | → device | Binary: led_count(u16be) + fps(u16be) + flags(u8) |
| `cmd/stop` | → device | empty or `stop` |
| `cmd/effect` | → device | JSON `{effect,speed,intensity,brightness,color1,color2}` |
| `cmd/brightness` | → device | JSON `{"brightness":180}` |
| `status` | device → | JSON every 30 s: online, ip, mac, led_count, brightness, free_heap, rssi… |
| (Last Will) | device → | retained `{"online":false}` on disconnect |

The webapp lives in `webapp/` (uses MQTT.js over WSS, QoS 1, retained).

### Matter mode

The moon appears as a standard **Extended Color Light**:

- **OnOff** → LEDs on/off (restores previous effect)
- **Level Control** → brightness (0–254)
- **Color Control** → hue + saturation
- **Custom cluster `0x131BFC01`** → animation: `EffectEnum` (off/static/rainbow/breathe/chase/sparkle/fire), `Speed`, `Intensity`

**Commissioning (Apple Home):** Casa → Agregar accesorio → scan the QR shown in the boot log, or enter manual code `34970112332`. Requires the N16R8 (8 MB PSRAM) for stable operation.

Matter-specific config (already in `sdkconfig.defaults`):
- `CONFIG_USE_MINIMAL_MDNS=n` — shares the ESP-IDF mDNS socket with AirPlay (both use UDP 5353). **Critical** to avoid the port conflict.
- `CONFIG_ESP_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` — moves Matter BSS to PSRAM.

## OTA updates

Dual OTA slots (6 MB each on N16R8) with SHA-256 validation and automatic rollback.

```bash
curl -X POST --data-binary "@build/airplay2-receiver.bin" \
  http://<device-ip>/api/ota/update
```

The moon turns **orange** during upload, **green** on success, then the device reboots into the new firmware. If the new image fails to boot, the bootloader rolls back to the previous slot automatically.

## WebSocket LED protocol (low-level)

Endpoint: `ws://<device-ip>/ws/leds`. All messages binary: byte 0 = version (`0x01`), byte 1 = type.

### Client → Device

**`STREAM_START` (0x01)** — start animation streaming.
```
Byte 0: version=0x01 | Byte 1: type=0x01
Bytes 2-3: led_count (u16be) | Bytes 4-5: fps (u16be) | Byte 6: flags (bit0: loop)
```
With **loop** set, frames are stored in PSRAM and replayed after the stream ends.

**`FRAME` (0x02)** — one frame. If no stream active: rendered immediately + persisted. If streaming: queued.
```
Byte 0: version=0x01 | Byte 1: type=0x02
Bytes 2-5: frame_id (u32be) | Bytes 6-9: timestamp_ms (u32be) | Bytes 10..: RGBA pixels
```

**`STREAM_STOP` (0x03)** — stop animation, clear strip.

### Device → Client

**`STREAM_READY` (0x81)** — reply to STREAM_START: `led_count(u16be) + max_fps(u16be)`.
**`BUFFER_STATUS` (0x82)** — low buffer: `last_frame_id(u32be) + buffer_time_ms(u16be)`.

## HTTP API

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/device/info` | GET | LED count, color order, firmware version |
| `/api/system/info` | GET | IP, MAC, free heap, network status |
| `/api/led/brightness` | GET/POST | Global brightness `{"brightness":128}` |
| `/api/led/effect` | POST | Run built-in effect (JSON) |
| `/api/ota/update` | POST | OTA firmware upload (.bin body) |

Built-in effects for `/api/led/effect` and MQTT `cmd/effect`: `off`, `static`, `rainbow`, `breathe`, `chase`, `sparkle`, `fire`.

```bash
curl -X POST http://192.168.1.100/api/led/effect \
  -H "Content-Type: application/json" \
  -d '{"effect":"breathe","speed":80,"color1":"ff3366","brightness":150}'
```

## Testing with the Python client

```bash
cd scripts
pip install websockets

# Static color
python3 led_stream_client.py --ip 192.168.1.100 --mode static --color ff3366 --brightness 200

# Rainbow animation at 30 FPS
python3 led_stream_client.py --ip 192.168.1.100 --mode animate

# Loop mode (device keeps replaying after disconnect)
python3 led_stream_client.py --ip 192.168.1.100 --mode animate --loop
```

## HomeAssistant integration

Two paths:

- **Matter (recommended):** in Matter mode the device is a native Matter light — add it to Home Assistant via the Matter integration. Color, brightness, on/off work out of the box.
- **MQTT:** point the firmware and HA at the same broker and drive `cmd/effect` / `cmd/brightness`, or use the REST endpoints:

```yaml
rest:
  - resource: http://192.168.1.100/api/device/info
    scan_interval: 60
    sensor:
      - name: "Moon Speaker Firmware"
        value_template: "{{ value_json.firmware_version }}"
```

## Project structure

```
main/
├── main.c                 # App init, control-mode selection, WiFi status LED
├── led_strip_ctrl.c/h     # WS2812 strip driver
├── led_anim_stream.c/h    # Frame protocol, effects engine, /ws/leds
├── moon_mqtt.c/h          # MQTT client (cmd topics + status + LWT)
├── moon_matter.cpp/h      # Matter Extended Color Light + custom animation cluster
├── status_led.c/h         # System status colors on the moon strip
├── buttons.c/h            # Button: 1 click = mode toggle, 5s hold = factory reset
├── settings.c/h           # NVS: WiFi, mode, factory reset, etc.
├── playback_control.c/h   # Source-agnostic media control (DACP/AVRCP)
├── network/
│   ├── web_server.c       # HTTP API + OTA handler
│   └── ota.c/h            # Dual-slot OTA with SHA-256 + rollback
└── CMakeLists.txt

components/boards/         # Board Kconfig + partitions.csv (dual OTA)
webapp/                    # MQTT.js webapp
scripts/led_stream_client.py
sdkconfig.defaults         # Base config (N16R8) — applied by set-target
```

## Notes & limitations

- **AirPlay 2 + MRP:** button media control of the source is limited (see "Physical button"). Bluetooth AVRCP works fully.
- **RAM:** Matter + MQTT don't coexist; toggle mode with the button. N16R8 required for Matter.
- **mDNS:** Matter and AirPlay share UDP 5353 via `CONFIG_USE_MINIMAL_MDNS=n`. Do not enable CHIP's minimal mDNS.
- **WS2812 color order** is GRB internally; clients send RGBA and the firmware converts.
- The animation WebSocket and AirPlay share WiFi; at 10–12 LEDs / 30 FPS traffic is ~1.5 KB/s (negligible).

## License

The original airplay-esp32 project has a non-commercial license. This fork inherits that license. See the original project for details.
