#!/usr/bin/env python3
"""Request an RGB565 framebuffer from the globe firmware and save a PNG."""

from __future__ import annotations

import argparse
import struct
import time
import zlib
from pathlib import Path

import serial
from PIL import Image


MAGIC = b"GLB2"


def read_exact(port: serial.Serial, length: int, deadline: float) -> bytes:
    result = bytearray()
    while len(result) < length:
        if time.monotonic() > deadline:
            raise TimeoutError(f"received {len(result)} of {length} bytes")
        chunk = port.read(length - len(result))
        if chunk:
            result.extend(chunk)
    return bytes(result)


def find_magic(port: serial.Serial, deadline: float) -> None:
    window = bytearray()
    while time.monotonic() <= deadline:
        byte = port.read(1)
        if not byte:
            continue
        window.extend(byte)
        if len(window) > len(MAGIC):
            del window[0]
        if bytes(window) == MAGIC:
            return
    raise TimeoutError("screenshot response marker not received")


def rgb565_to_image(payload: bytes, width: int, height: int) -> Image.Image:
    rgb = bytearray(width * height * 3)
    destination = memoryview(rgb)
    for index in range(width * height):
        pixel = (payload[index * 2] << 8) | payload[index * 2 + 1]
        destination[index * 3] = ((pixel >> 11) & 0x1F) * 255 // 31
        destination[index * 3 + 1] = ((pixel >> 5) & 0x3F) * 255 // 63
        destination[index * 3 + 2] = (pixel & 0x1F) * 255 // 31
    return Image.frombytes("RGB", (width, height), rgb)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM21")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--output", type=Path, default=Path("globe-screenshot.png"))
    args = parser.parse_args()

    with serial.Serial(args.port, args.baud, timeout=0.25) as port:
        # Avoid an RTS/DTR application reset when opening the USB CDC port.
        port.dtr = False
        port.rts = False
        port.reset_input_buffer()
        # Some Windows USB CDC drivers pulse control lines while opening the
        # port. The firmware also defers commands until a complete frame exists.
        time.sleep(0.5)
        port.write(b"S")
        port.flush()

        deadline = time.monotonic() + args.timeout
        find_magic(port, deadline)
        width, height, byte_count, expected_crc = struct.unpack(
            "<HHII", read_exact(port, 12, deadline)
        )
        if byte_count != width * height * 2:
            raise ValueError(
                f"invalid payload size {byte_count} for {width}x{height} RGB565"
            )
        payload = read_exact(port, byte_count, deadline)

    actual_crc = zlib.crc32(payload)
    if actual_crc != expected_crc:
        raise ValueError(
            f"CRC mismatch: device={expected_crc:08x}, received={actual_crc:08x}"
        )

    image = rgb565_to_image(payload, width, height)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    image.save(args.output)
    print(
        f"Saved {args.output} ({width}x{height}, CRC32 {actual_crc:08x})"
    )


if __name__ == "__main__":
    main()
