#!/usr/bin/env python3
"""
Example client for the /ws/leds binary WebSocket protocol.

Protocol version 1

Client -> ESP32:
  STREAM_START  0x01  payload: led_count (u16 be), fps (u16 be)
  FRAME         0x02  payload: frame_id (u32 be), timestamp_ms (u32 be),
                              pixels[led_count * 4] (RGBA)
  STREAM_STOP   0x03  no payload

ESP32 -> Client:
  STREAM_READY  0x81  payload: led_count (u16 be), max_fps (u16 be)
  BUFFER_STATUS 0x82  payload: last_frame_id (u32 be), buffer_time_ms (u16 be)

Usage:
  # Color picker mode: single static frame, persists on the device
  python3 led_stream_client.py --ip 192.168.1.100 --mode static --color ff3366 --brightness 128

  # Animation mode: stream rainbow frames at 30 fps
  python3 led_stream_client.py --ip 192.168.1.100 --mode animate
"""

import argparse
import asyncio
import struct
import sys
import time
import websockets

LED_COUNT = 12
FPS = 30
FRAME_PERIOD = 1.0 / FPS


def pack_stream_start(led_count, fps):
    return struct.pack(">BB", 1, 0x01) + struct.pack(">HH", led_count, fps)


def pack_stream_stop():
    return struct.pack(">BB", 1, 0x03)


def pack_frame(frame_id, rgba_pixels):
    header = struct.pack(">BB", 1, 0x02)
    body = struct.pack(">II", frame_id, int(time.time() * 1000)) + bytes(rgba_pixels)
    return header + body


def hex_to_rgb(hex_color):
    hex_color = hex_color.lstrip("#")
    if len(hex_color) != 6:
        raise ValueError("Color must be 6 hex digits, e.g. ff3366")
    return (
        int(hex_color[0:2], 16),
        int(hex_color[2:4], 16),
        int(hex_color[4:6], 16),
    )


def solid_frame(frame_id, r, g, b, a, led_count):
    pixels = [r, g, b, a] * led_count
    return pack_frame(frame_id, pixels)


def hsv_to_rgb(h, s, v):
    if s == 0.0:
        return (v, v, v)
    i = int(h * 6.0)
    f = (h * 6.0) - i
    p = v * (1.0 - s)
    q = v * (1.0 - s * f)
    t = v * (1.0 - s * (1.0 - f))
    i = i % 6
    if i == 0:
        return (v, t, p)
    if i == 1:
        return (q, v, p)
    if i == 2:
        return (p, v, t)
    if i == 3:
        return (p, q, v)
    if i == 4:
        return (t, p, v)
    return (v, p, q)


async def receive_loop(ws):
    while True:
        try:
            msg = await ws.recv()
        except websockets.exceptions.ConnectionClosed:
            break
        if isinstance(msg, bytes) and len(msg) >= 2:
            version, msg_type = msg[0], msg[1]
            payload = msg[2:]
            if msg_type == 0x81 and len(payload) == 4:
                led_count, max_fps = struct.unpack(">HH", payload)
                print(f"STREAM_READY: led_count={led_count}, max_fps={max_fps}")
            elif msg_type == 0x82 and len(payload) == 6:
                last_id, buf_time = struct.unpack(">IH", payload)
                print(f"BUFFER_STATUS: last_frame_id={last_id}, buffer_time_ms={buf_time}")


async def run_static(ws, color, brightness):
    r, g, b = hex_to_rgb(color)
    a = max(0, min(255, brightness))
    print(f"Setting static color: #{color} alpha={a}")
    await ws.send(solid_frame(0, r, g, b, a, LED_COUNT))
    # Keep connection alive so user can send more frames later.
    await asyncio.sleep(3600)


async def run_animate(ws):
    asyncio.create_task(receive_loop(ws))
    await ws.send(pack_stream_start(LED_COUNT, FPS))
    await asyncio.sleep(0.2)

    frame_id = 0
    start = time.time()
    try:
        while True:
            pixels = []
            phase = (frame_id % 60) / 60.0
            for i in range(LED_COUNT):
                hue = (phase + i / LED_COUNT) % 1.0
                r, g, b = hsv_to_rgb(hue, 1.0, 0.5)
                pixels.extend([int(r * 255), int(g * 255), int(b * 255), 255])

            await ws.send(pack_frame(frame_id, pixels))
            frame_id += 1

            next_time = start + frame_id * FRAME_PERIOD
            sleep_for = next_time - time.time()
            if sleep_for > 0:
                await asyncio.sleep(sleep_for)
    except KeyboardInterrupt:
        await ws.send(pack_stream_stop())
        await asyncio.sleep(0.1)


async def main():
    parser = argparse.ArgumentParser(description="LED WebSocket test client")
    parser.add_argument("--ip", required=True, help="ESP32 IP address")
    parser.add_argument("--mode", choices=["static", "animate"], default="static")
    parser.add_argument("--color", default="ff3366", help="Static color hex")
    parser.add_argument("--brightness", type=int, default=255,
                        help="Alpha/brightness 0-255")
    args = parser.parse_args()

    uri = f"ws://{args.ip}/ws/leds"
    async with websockets.connect(uri) as ws:
        if args.mode == "static":
            await run_static(ws, args.color, args.brightness)
        else:
            await run_animate(ws)


if __name__ == "__main__":
    asyncio.run(main())
