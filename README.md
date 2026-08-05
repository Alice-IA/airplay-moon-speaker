# AirPlay Moon Speaker

ESP32-based AirPlay 2 speaker with WS2812 LED strip control over a binary WebSocket protocol.

This project is based on [rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32) and adds:

- Binary WebSocket endpoint `/ws/leds` for real-time LED control and animation streaming.
- Support for 1 to 64 WS2812 addressable LEDs.
- Two operating modes:
  - **Single frame mode** (color picker / static color): a single `FRAME` is rendered immediately and persists until the next frame.
  - **Animation stream mode** (`STREAM_START` + frames): frames are buffered and rendered at the configured FPS.
- HTTP endpoint `/api/device/info` to query LED and firmware configuration.

## Features

- AirPlay 2 audio receiver
- Bluetooth A2DP sink (on classic ESP32 boards)
- Web-based setup and OTA updates
- WS2812 LED strip control via WebSocket
- HomeAssistant-friendly HTTP and WebSocket interfaces

## Hardware

| Component | Notes |
|-----------|-------|
| ESP32 or ESP32-S3 | With at least 4 MB flash. PSRAM recommended for AirPlay stability. |
| PCM5102A I2S DAC | Or any I2S DAC supported by the base project. |
| WS2812 LED strip | 10–12 LEDs recommended, up to 64 supported. |
| 5 V power supply | Enough for the ESP32, DAC, amplifier, and LEDs. |

### Wiring example (ESP32-S3 + PCM5102A + WS2812)

| PCM5102A | ESP32-S3 |
|----------|----------|
| VIN      | 5 V      |
| GND      | GND      |
| BCK      | GPIO 11  |
| DIN      | GPIO 12  |
| LCK      | GPIO 13  |

| WS2812 | ESP32-S3 |
|--------|----------|
| 5 V    | 5 V      |
| GND    | GND      |
| DATA   | GPIO 21  |

> The LED data GPIO is configurable in `menuconfig`. Choose a free GPIO for your board.

## Requirements

- [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)
- Python 3.8+
- `websockets` Python package (for the example client)

## Installation

1. Clone this repository with submodules:

```bash
git clone --recursive https://github.com/Alice-IA/airplay-moon-speaker.git
cd airplay-moon-speaker
```

2. Set the target (ESP32 or ESP32-S3):

```bash
idf.py set-target esp32s3
# or
idf.py set-target esp32
```

3. Open menuconfig and configure your board:

```bash
idf.py menuconfig
```

Required configuration sections:

- **Board Selection**: choose your board (e.g. ESP32-S3 generic).
- **Pin Configuration → LED GPIOs**:
  - `LED animation strip GPIO (WS2812)` — set to your LED data pin.
  - `LED animation strip LED count` — set to the number of LEDs (e.g. 12).
- **Pin Configuration → I2S and S/PDIF Pin Configuration**: set BCK, WS, and DO pins for your DAC.
- **Audio Output**: choose I2S, SPDIF, or USB depending on your hardware.

4. Build and flash:

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash
idf.py -p /dev/ttyUSB0 monitor
```

> On first boot the device creates an access point named `ESP32-AirPlay-Setup`. Connect to it and configure your home WiFi.

## First boot and WiFi setup

1. Power the board.
2. On your phone or computer, connect to `ESP32-AirPlay-Setup`.
3. A captive portal opens at `http://192.168.4.1`.
4. Set a device name and your home WiFi credentials.
5. The device restarts and joins your network.
6. Find the device IP in your router or serial monitor.

## LED WebSocket protocol

Endpoint: `ws://<device-ip>/ws/leds`

All messages are binary. First byte is protocol version (`0x01`), second byte is message type.

### Client → Device

#### `STREAM_START` (0x01)

Starts animation streaming mode. Frames will be buffered and rendered at the requested FPS.

```
Byte 0: version  = 0x01
Byte 1: type     = 0x01
Bytes 2-3: led_count (uint16 big-endian)
Bytes 4-5: fps (uint16 big-endian)
```

#### `FRAME` (0x02)

Single frame. Behavior depends on current mode:

- If no stream is active: rendered immediately and persisted.
- If a stream is active: queued in the frame buffer.

```
Byte 0: version  = 0x01
Byte 1: type     = 0x02
Bytes 2-5: frame_id (uint32 big-endian)
Bytes 6-9: timestamp_ms (uint32 big-endian)
Bytes 10..: pixels (RGBA, 4 bytes per LED)
```

#### `STREAM_STOP` (0x03)

Stops animation mode and clears the LED strip.

```
Byte 0: version  = 0x01
Byte 1: type     = 0x03
```

### Device → Client

#### `STREAM_READY` (0x81)

Sent in response to `STREAM_START`.

```
Byte 0: version  = 0x01
Byte 1: type     = 0x81
Bytes 2-3: led_count (uint16 big-endian)
Bytes 4-5: max_fps (uint16 big-endian)
```

#### `BUFFER_STATUS` (0x82)

Sent when the frame buffer is running low.

```
Byte 0: version  = 0x01
Byte 1: type     = 0x82
Bytes 2-5: last_frame_id (uint32 big-endian)
Bytes 6-7: buffer_time_ms (uint16 big-endian)
```

## Testing with the example Python client

Install the dependency:

```bash
cd scripts
pip install websockets
```

### Static color / color picker mode

```bash
python3 led_stream_client.py \
  --ip 192.168.1.100 \
  --mode static \
  --color ff3366 \
  --brightness 200
```

The strip turns the requested color and stays that way until another frame arrives.

### Animation stream mode

```bash
python3 led_stream_client.py \
  --ip 192.168.1.100 \
  --mode animate
```

This sends `STREAM_START` and a rainbow animation at 30 FPS.

## HTTP API

### `GET /api/device/info`

Returns LED and firmware configuration.

Example response:

```json
{
  "led_count": 12,
  "color_order": "GRB",
  "rgb_type": "RGB8",
  "firmware_version": "v0.1.29",
  "success": true
}
```

### `GET /api/system/info`

Returns network and system status (IP, MAC, free heap, etc.).

### `GET /api/led/brightness`

Returns the current global LED brightness.

### `POST /api/led/brightness`

Sets global LED brightness.

```json
{ "brightness": 128 }
```

## HomeAssistant integration

### Query device info

```yaml
rest:
  - resource: http://192.168.1.100/api/device/info
    scan_interval: 60
    sensor:
      - name: "Moon Speaker LED Count"
        value_template: "{{ value_json.led_count }}"
      - name: "Moon Speaker Firmware"
        value_template: "{{ value_json.firmware_version }}"
```

### Send a static color

You can use the `websocket_client` integration or a Python script triggered by an automation to send a `FRAME` message. For simple scenes, a shell command calling the example client also works:

```yaml
shell_command:
  moon_speaker_red: |
    python3 /config/scripts/led_stream_client.py --ip 192.168.1.100 --mode static --color ff0000 --brightness 200
```

For smoother integrations, wrap the binary protocol in a small Python service or HomeAssistant custom component.

## Project structure

```
main/
├── led_strip_ctrl.c/h      # WS2812 strip driver
├── led_anim_stream.c/h     # WebSocket server and frame protocol
├── network/web_server.c    # HTTP server, extended with /api/device/info and /ws/leds
├── led.c                   # Status and single RGB LED (untouched for animation strip)
├── main.c                  # App init, now calls led_anim_stream_init()
└── CMakeLists.txt          # Includes new source files

components/boards/Kconfig.projbuild  # LED_ANIM_GPIO and LED_ANIM_COUNT options

scripts/
└── led_stream_client.py    # Example Python client
```

## Notes and limitations

- The animation WebSocket and AirPlay share the WiFi interface. With only 10–12 LEDs at 30 FPS, traffic is negligible (~1.5 KB/s).
- The frame buffer holds 8 frames. If the buffer runs low, the device sends `BUFFER_STATUS` to request more frames.
- `STREAM_STOP` clears the strip. To keep the last frame after an animation, send a single `FRAME` before stopping.
- WS2812 color order is GRB internally; the client still sends RGBA and the firmware handles the conversion.

## License

The original airplay-esp32 project has a non-commercial license. This fork inherits that license. See the original project for details.
