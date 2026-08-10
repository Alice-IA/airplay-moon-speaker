# Moon Speaker — API Reference

Protocolo WebSocket binario y endpoints HTTP para controlar los LEDs WS2812.

---

## Tabla de contenidos

1. [Visión general](#visión-general)
2. [WebSocket binario — `/ws/leds`](#websocket-binario--wsleds)
   - [STREAM_START (0x01)](#stream_start-0x01)
   - [FRAME (0x02)](#frame-0x02)
   - [STREAM_STOP (0x03)](#stream_stop-0x03)
   - [STREAM_READY (0x81)](#stream_ready-0x81)
   - [BUFFER_STATUS (0x82)](#buffer_status-0x82)
   - [Modos de operación](#modos-de-operación)
3. [Endpoints HTTP](#endpoints-http)
   - [GET /api/device/info](#get-apideviceinfo)
   - [GET /api/system/info](#get-apisysteminfo)
   - [GET /api/led/brightness](#get-apiledbrightness)
   - [POST /api/led/brightness](#post-apiledbrightness)
   - [POST /api/led/effect](#post-apiledeffect)
   - [POST /api/device/name](#post-apidevicename)
   - [POST /api/system/restart](#post-apisystemrestart)
   - [GET /api/wifi/scan](#get-apiwifiscan)
   - [POST /api/wifi/config](#post-apiwificonfig)
   - [POST /api/ota/update](#post-apiotaupdate)
   - [GET /api/fs/list](#get-apifslist)
   - [POST /api/fs/upload](#post-apifsupload)
   - [POST /api/fs/delete](#post-apifsdelete)
4. [Efectos integrados](#efectos-integrados)
5. [Ejemplos completos](#ejemplos-completos)
   - [Python](#python)
   - [JavaScript / Navegador](#javascript--navegador)
   - [curl](#curl)
   - [HomeAssistant](#homeassistant)

---

## Visión general

El Moon Speaker expone dos interfaces de control para los LEDs:

| Interfaz | URL | Uso |
|---|---|---|
| WebSocket binario | `ws://<ip>/ws/leds` | Streaming de frames en tiempo real, loop de animaciones |
| HTTP REST | `http://<ip>/api/...` | Efectos preconfigurados, brillo, info del dispositivo |

**Puerto por defecto:** 80

**Codificación de colores:** RGBA, 4 bytes por LED (Red, Green, Blue, Alpha/brillo).

**Orden de LEDs:** GRB (típico de WS2812).

---

## WebSocket binario — `/ws/leds`

Conexión WebSocket estándar (RFC 6455). Todos los mensajes son **binarios** (no texto).

### Estructura de los mensajes

Cada mensaje tiene un header de 2 bytes seguido del payload:

```
Byte 0: versión de protocolo (actualmente 0x01)
Byte 1: tipo de mensaje
Byte 2..: payload (varía según el tipo)
```

Los enteros multi-byte son **big-endian** (network byte order).

---

### Client → ESP32

#### STREAM_START (0x01)

Inicia el modo de streaming. Le dice al ESP32 cuántos LEDs tiene la tira y a qué FPS debe renderizar.

```
Byte 0: versión  = 0x01
Byte 1: tipo     = 0x01
Bytes 2-3: led_count  (uint16 big-endian)
Bytes 4-5: fps        (uint16 big-endian)
Byte 6:   flags      (bit 0: loop)
```

**Flags:**

| Bit | Nombre | Descripción |
|-----|--------|-------------|
| 0   | loop   | 1 = almacenar frames y reproducirlos en bucle. 0 = reproducir una sola vez. |

**Ejemplo en bytes** (12 LEDs, 30 fps, loop activado):

```
01 01 00 0C 00 1E 01
```

Desglose:
- `01` — versión 1
- `01` — STREAM_START
- `00 0C` — 12 LEDs
- `00 1E` — 30 fps
- `01` — flags: loop = 1

**Restricciones:**
- `led_count` debe ser > 0 y ≤ `CONFIG_LED_ANIM_COUNT` (configurado en menuconfig).
- `fps` debe estar entre 1 y 120. Si es 0 o > 120, se ajusta a 30.
- Si `led_count` excede el máximo, el mensaje se ignora.

---

#### FRAME (0x02)

Envía un frame de animación. El comportamiento depende del modo activo:

- **Sin stream activo:** el frame se renderiza inmediatamente y se mantiene (modo color picker).
- **Modo streaming (loop=0):** el frame se encola en el buffer para reproducción a FPS constante.
- **Modo loop (loop=1):** el frame se almacena en PSRAM para reproducirlo en bucle.

```
Byte 0: versión     = 0x01
Byte 1: tipo        = 0x02
Bytes 2-5: frame_id      (uint32 big-endian)
Bytes 6-9: timestamp_ms  (uint32 big-endian)
Bytes 10..: pixels        (4 bytes por LED = RGBA)
```

**Formato de pixels:**

Para cada LED `i` (de 0 a led_count-1):

```
pixels[i*4 + 0] = Red   (0-255)
pixels[i*4 + 1] = Green (0-255)
pixels[i*4 + 2] = Blue  (0-255)
pixels[i*4 + 3] = Alpha/brillo (0-255, típicamente 255)
```

**Tamaño esperado del payload:** `8 + led_count * 4` bytes.

**Ejemplo en bytes** (2 LEDs: rojo puro y azul puro):

```
01 02  00 00 00 01  00 00 00 00  FF 00 00 FF  00 00 FF FF
```

Desglose:
- `01 02` — versión 1, FRAME
- `00 00 00 01` — frame_id = 1
- `00 00 00 00` — timestamp_ms = 0
- `FF 00 00 FF` — LED 0: rojo, brillo 255
- `00 00 FF FF` — LED 1: azul, brillo 255

**Nota:** `frame_id` y `timestamp_ms` son informativos. El ESP32 los usa para reportar el último frame renderizado en `BUFFER_STATUS`, pero no los usa para sincronización de tiempo.

---

#### STREAM_STOP (0x03)

Detiene cualquier modo activo (streaming, loop, o efecto), limpia la tira y libera la memoria del loop.

```
Byte 0: versión = 0x01
Byte 1: tipo    = 0x03
```

Sin payload.

---

### ESP32 → Client

#### STREAM_READY (0x81)

Respuesta a `STREAM_START`. Confirma que el modo fue configurado.

```
Byte 0: versión     = 0x01
Byte 1: tipo        = 0x81
Bytes 2-3: led_count  (uint16 big-endian)
Bytes 4-5: max_fps    (uint16 big-endian, actualmente 60)
```

---

#### BUFFER_STATUS (0x82)

Enviado cuando el buffer de frames está por agotarse (menos de 2 frames en cola). El cliente debe enviar más frames.

```
Byte 0: versión     = 0x01
Byte 1: tipo        = 0x82
Bytes 2-5: last_frame_id  (uint32 big-endian)
Bytes 6-7: buffer_time_ms (uint16 big-endian, tiempo restante aprox.)
```

**Nota:** Solo se envía en modo streaming (no loop). En modo loop, los frames se almacenan directamente y no se usa el buffer de cola.

---

### Modos de operación

| Modo | Cómo se activa | Qué hace |
|------|---------------|----------|
| **OFF** | `STREAM_STOP` o al iniciar | LEDs apagados |
| **STATIC** | Enviar `FRAME` sin stream activo | Renderiza el frame y lo mantiene fijo |
| **STREAMING** | `STREAM_START` con loop=0 | Encola frames y los reproduce a FPS constante |
| **STREAMING_LOOP** | `STREAM_START` con loop=1 | Almacena frames en PSRAM y los reproduce en bucle |
| **EFFECT** | `POST /api/led/effect` | Ejecuta un efecto preconfigurado en el firmware |

**Transiciones:**
- Cualquier `STREAM_START` reemplaza el modo actual.
- `STREAM_STOP` siempre vuelve a OFF.
- `POST /api/led/effect` reemplaza cualquier modo activo.
- Un `FRAME` suelto (sin `STREAM_START` previo) activa modo STATIC.

---

## Endpoints HTTP

Todos los endpoints devuelven JSON con un campo `"success": true/false`.

---

### GET /api/device/info

Información básica del dispositivo para que las apps se adapten automáticamente.

**Respuesta:**

```json
{
  "led_count": 12,
  "color_order": "GRB",
  "rgb_type": "RGB8",
  "firmware_version": "v0.1.29",
  "success": true
}
```

---

### GET /api/system/info

Estado detallado del sistema: red, memoria, firmware.

**Respuesta:**

```json
{
  "info": {
    "ip": "192.168.68.64",
    "mac": "aa:bb:cc:dd:ee:ff",
    "device_name": "MoonSpeaker",
    "wifi_connected": true,
    "eth_connected": false,
    "free_heap": 234567,
    "wifi_ssid": "MiRed",
    "wifi_bssid": "aa:bb:cc:dd:ee:ff",
    "wifi_rssi": -52,
    "wifi_channel": 6,
    "wifi_phy": "11n",
    "firmware_version": "v0.1.29",
    "eq_supported": false,
    "sub_supported": false
  },
  "success": true
}
```

Los campos `wifi_ssid`, `wifi_bssid`, `wifi_rssi`, `wifi_channel`, `wifi_phy` solo aparecen si WiFi está conectado.

---

### GET /api/led/brightness

Devuelve el brillo global actual.

**Respuesta:**

```json
{
  "brightness": 128,
  "success": true
}
```

---

### POST /api/led/brightness

Establece el brillo global (0-255). Se aplica a todos los modos.

**Request:**

```json
{ "brightness": 128 }
```

**Respuesta:**

```json
{
  "brightness": 128,
  "success": true
}
```

---

### POST /api/led/effect

Activa un efecto preconfigurado en el firmware. No requiere mantener una conexión WebSocket.

**Request:**

```json
{
  "effect": "rainbow",
  "speed": 128,
  "intensity": 128,
  "brightness": 200,
  "color1": "ff0000",
  "color2": "0000ff"
}
```

**Parámetros:**

| Campo | Tipo | Default | Descripción |
|-------|------|---------|-------------|
| `effect` | string | `"off"` | Nombre del efecto (ver tabla abajo) |
| `speed` | int 0-255 | 128 | Velocidad de animación (mayor = más rápido) |
| `intensity` | int 0-255 | 128 | Intensidad del efecto (varía según efecto) |
| `brightness` | int 0-255 | 255 | Brillo global |
| `color1` | hex string | `"ff0000"` | Color primario (formato RRGGBB, con o sin #) |
| `color2` | hex string | `"0000ff"` | Color secundario |

**Respuesta exitosa:**

```json
{
  "success": true
}
```

**Respuesta de error (efecto no reconocido):**

```json
{
  "success": false,
  "error": "ESP_ERR_NOT_FOUND"
}
```

**Ejemplo curl:**

```bash
curl -X POST http://192.168.68.64/api/led/effect \
  -H "Content-Type: application/json" \
  -d '{"effect":"breathe","speed":80,"color1":"ff3366","brightness":150}'
```

---

### POST /api/device/name

Cambia el nombre del dispositivo (hostname en la red).

**Request:**

```json
{ "name": "MiBocina" }
```

**Respuesta:**

```json
{
  "success": true
}
```

---

### POST /api/system/restart

Reinicia el ESP32. Responde antes de reiniciar.

**Respuesta:**

```json
{
  "success": true
}
```

---

### GET /api/wifi/scan

Escanea redes WiFi disponibles.

**Respuesta:**

```json
{
  "networks": [
    { "ssid": "Red1", "rssi": -45, "auth": true },
    { "ssid": "Red2", "rssi": -67, "auth": false }
  ],
  "success": true
}
```

---

### POST /api/wifi/config

Configura credenciales WiFi y reconecta.

**Request:**

```json
{
  "ssid": "MiRed",
  "password": "mipassword"
}
```

---

### POST /api/ota/update

Actualiza el firmware por OTA (Over-The-Air).

**Request:** multipart/form-data con el archivo binario.

---

### GET /api/fs/list

Lista archivos almacenados en SPIFFS.

**Respuesta:**

```json
{
  "files": [
    { "name": "anim1.bin", "size": 1024 }
  ],
  "success": true
}
```

---

### POST /api/fs/upload

Sube un archivo a SPIFFS.

**Request:** multipart/form-data con `path` y `file`.

---

### POST /api/fs/delete

Elimina un archivo de SPIFFS.

**Request:**

```json
{ "path": "/spiffs/anim1.bin" }
```

---

## Efectos integrados

| Nombre | Descripción | Usa color1 | Usa color2 | Usa intensity |
|--------|-------------|:---:|:---:|:---:|
| `off` | Apaga todos los LEDs | — | — | — |
| `static` | Color fijo en toda la tira | Si | — | — |
| `rainbow` | Arcoíris animado que se desplaza | — | — | — |
| `breathe` | Pulso de respiración en color1 | Si | — | — |
| `chase` | Cometa que recorre la tira, mezclando color1 y color2 | Si | Si | Si (largo de la cola) |
| `sparkle` | Destellos aleatorios en color1 sobre fondo color2 | Si | Si | Si (densidad de destellos) |
| `fire` | Simulación de fuego (rojo/naranja) | — | — | Si (intensidad de llamas) |

**Notas:**
- `speed` controla la velocidad de animación. 0 = muy lento, 255 = muy rápido.
- `brightness` se aplica como multiplicador global sobre todos los canales.
- Los efectos corren a 30 FPS internamente.

---

## Ejemplos completos

### Python

#### Color estático (color picker)

```python
import asyncio
import struct
import websockets

async def main():
    IP = "192.168.68.64"
    LED_COUNT = 12

    async with websockets.connect(f"ws://{IP}/ws/leds") as ws:
        # No need for STREAM_START — a bare FRAME renders immediately
        frame_id = 1
        timestamp = 0
        pixels = b""
        for i in range(LED_COUNT):
            r, g, b, a = 0xFF, 0x33, 0x66, 0xFF  # Rosa
            pixels += struct.pack("BBBB", r, g, b, a)

        msg = struct.pack(">BB", 1, 0x02)  # version=1, type=FRAME
        msg += struct.pack(">II", frame_id, timestamp)
        msg += pixels
        await ws.send(msg)
        print("Color enviado")

asyncio.run(main())
```

#### Animación en streaming (loop)

```python
import asyncio
import struct
import math
import websockets

async def main():
    IP = "192.168.68.64"
    LED_COUNT = 12
    FPS = 30
    DURATION_SEC = 3  # 3 seconds of animation, then loops forever

    async with websockets.connect(f"ws://{IP}/ws/leds") as ws:
        # STREAM_START with loop flag
        flags = 0x01  # loop = 1
        stream_start = struct.pack(">BB", 1, 0x01)
        stream_start += struct.pack(">HHB", LED_COUNT, FPS, flags)
        await ws.send(stream_start)

        await asyncio.sleep(0.2)

        # Send all frames
        total_frames = FPS * DURATION_SEC
        for frame_id in range(1, total_frames + 1):
            t = frame_id / FPS
            pixels = b""
            for i in range(LED_COUNT):
                hue = (t * 0.5 + i / LED_COUNT) % 1.0
                # Simple HSV to RGB
                import colorsys
                r, g, b = colorsys.hsv_to_rgb(hue, 1.0, 1.0)
                pixels += struct.pack("BBBB",
                    int(r * 255), int(g * 255), int(b * 255), 255)

            msg = struct.pack(">BB", 1, 0x02)
            msg += struct.pack(">II", frame_id, int(t * 1000))
            msg += pixels
            await ws.send(msg)

            # Pace the sending
            await asyncio.sleep(1 / FPS)

        print(f"Sent {total_frames} frames. Loop is now playing.")

asyncio.run(main())
```

#### Activar efecto vía HTTP

```python
import requests

response = requests.post("http://192.168.68.64/api/led/effect", json={
    "effect": "rainbow",
    "speed": 150,
    "brightness": 180
})
print(response.json())
```

---

### JavaScript / Navegador

#### Color estático

```javascript
const ws = new WebSocket("ws://192.168.68.64/ws/leds");
ws.binaryType = "arraybuffer";

ws.onopen = () => {
  const LED_COUNT = 12;
  const header = new Uint8Array([0x01, 0x02]); // version=1, FRAME
  const meta = new Uint8Array(8);
  const view = new DataView(meta.buffer);
  view.setUint32(0, 1, false);  // frame_id = 1 (big-endian)
  view.setUint32(4, 0, false);  // timestamp = 0

  const pixels = new Uint8Array(LED_COUNT * 4);
  for (let i = 0; i < LED_COUNT; i++) {
    pixels[i * 4] = 0xFF;     // R
    pixels[i * 4 + 1] = 0x33; // G
    pixels[i * 4 + 2] = 0x66; // B
    pixels[i * 4 + 3] = 0xFF; // A
  }

  const msg = new Uint8Array(header.length + meta.length + pixels.length);
  msg.set(header, 0);
  msg.set(meta, header.length);
  msg.set(pixels, header.length + meta.length);
  ws.send(msg.buffer);
  console.log("Color enviado");
};

ws.onmessage = (event) => {
  const data = new Uint8Array(event.data);
  const type = data[1];
  if (type === 0x81) {
    const ledCount = (data[2] << 8) | data[3];
    const maxFps = (data[4] << 8) | data[5];
    console.log(`Stream ready: ${ledCount} LEDs, max ${maxFps} fps`);
  }
};
```

#### Activar efecto vía fetch

```javascript
fetch("http://192.168.68.64/api/led/effect", {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({
    effect: "breathe",
    speed: 80,
    color1: "ff3366",
    brightness: 150
  })
})
.then(r => r.json())
.then(console.log);
```

---

### curl

#### Efecto rainbow

```bash
curl -X POST http://192.168.68.64/api/led/effect \
  -H "Content-Type: application/json" \
  -d '{"effect":"rainbow","speed":128,"brightness":200}'
```

#### Efecto chase con dos colores

```bash
curl -X POST http://192.168.68.64/api/led/effect \
  -H "Content-Type: application/json" \
  -d '{"effect":"chase","speed":100,"intensity":180,"color1":"00ff00","color2":"ff0000","brightness":200}'
```

#### Apagar LEDs

```bash
curl -X POST http://192.168.68.64/api/led/effect \
  -H "Content-Type: application/json" \
  -d '{"effect":"off"}'
```

#### Cambiar brillo

```bash
curl -X POST http://192.168.68.64/api/led/brightness \
  -H "Content-Type: application/json" \
  -d '{"brightness":128}'
```

#### Info del dispositivo

```bash
curl http://192.168.68.64/api/device/info
```

---

### HomeAssistant

#### Sensor de info del dispositivo

```yaml
rest:
  - resource: http://192.168.68.64/api/device/info
    scan_interval: 60
    sensor:
      - name: "Moon Speaker LED Count"
        value_template: "{{ value_json.led_count }}"
      - name: "Moon Speaker Firmware"
        value_template: "{{ value_json.firmware_version }}"
```

#### Switch de efecto rainbow

```yaml
rest_command:
  moon_speaker_rainbow:
    url: http://192.168.68.64/api/led/effect
    method: POST
    content_type: application/json
    payload: '{"effect":"rainbow","speed":128,"brightness":200}'
```

#### Switch de efecto breathe con color personalizable

```yaml
rest_command:
  moon_speaker_breathe:
    url: http://192.168.68.64/api/led/effect
    method: POST
    content_type: application/json
    payload: >
      {"effect":"breathe","speed":{{ speed | default(80) }},
       "color1":"{{ color1 | default('ff3366') }}",
       "brightness":{{ brightness | default(150) }}}
```

Uso desde una automatización:

```yaml
action:
  - service: rest_command.moon_speaker_breathe
    data:
      speed: 60
      color1: "3366ff"
      brightness: 120
```

#### Control de brillo

```yaml
rest_command:
  moon_speaker_brightness:
    url: http://192.168.68.64/api/led/brightness
    method: POST
    content_type: application/json
    payload: '{"brightness":{{ brightness }}}'
```

---

## Notas de implementación

### Memoria

- **Modo streaming:** buffer circular de 8 frames en RAM interna (~12 KB para 12 LEDs).
- **Modo loop:** hasta 2000 frames en PSRAM (~288 KB para 12 LEDs, ~6 MB para 256 LEDs).
- Si la PSRAM no tiene espacio, el loop cae a RAM interna (puede fallar con muchos LEDs).

### Concurrencia

- Solo un cliente WebSocket a la vez. Una nueva conexión reemplaza la anterior.
- `POST /api/led/effect` puede usarse sin WebSocket activo.
- Si se envía un efecto HTTP mientras hay un stream WebSocket activo, el efecto reemplaza al stream.

### Limitaciones actuales

- El tamaño máximo de mensaje WebSocket es 512 bytes (definido por `WS_RX_BUF_SIZE`). Para 12 LEDs, un frame ocupa `2 + 8 + 48 = 58` bytes. Para más LEDs, puede ser necesario aumentar este buffer en el firmware.
- Los efectos corren a 30 FPS fijo. El streaming respeta el FPS solicitado en `STREAM_START`.
- No hay autenticación. El dispositivo está abierto en la red local.
