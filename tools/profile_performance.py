#!/usr/bin/env python3
"""Measure quiet on-device globe render and display-transfer timings."""

from __future__ import annotations

import argparse
import struct
import time

import serial


MAGIC = b"GLBQ"
PITCH_MAGIC = b"GLBT"
RECORD = struct.Struct("<8I")
PITCH_RECORD = struct.Struct("<hhBBBBII")
PITCH_MODE_NAMES = {0: "zero", 1: "preview", 2: "exact", 3: "anchor"}


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
    find_marker(port, MAGIC, deadline)


def find_marker(
    port: serial.Serial, marker: bytes, deadline: float
) -> None:
    window = bytearray()
    while time.monotonic() <= deadline:
        byte = port.read(1)
        if not byte:
            continue
        window.extend(byte)
        if len(window) > len(marker):
            del window[0]
        if bytes(window) == marker:
            return
    raise TimeoutError(f"{marker!r} response marker not received")


def snapshot(port: serial.Serial) -> tuple[int, ...]:
    port.reset_input_buffer()
    port.write(b"Q")
    port.flush()
    deadline = time.monotonic() + 5.0
    find_magic(port, deadline)
    return RECORD.unpack(read_exact(port, RECORD.size, deadline))


def delta32(current: int, previous: int) -> int:
    return (current - previous) & 0xFFFFFFFF


def wait_for_pitch(
    port: serial.Serial,
    pitch: int,
    required_mode: int,
    timeout: float = 30.0,
) -> None:
    deadline = time.monotonic() + timeout
    target_q8 = pitch * 256
    while True:
        port.reset_input_buffer()
        port.write(b"T")
        port.flush()
        response_deadline = min(deadline, time.monotonic() + 5.0)
        find_marker(port, PITCH_MAGIC, response_deadline)
        (
            target,
            rendered,
            mode,
            exact_ready,
            preview_index,
            rendered_mode,
            preview_build_us,
            exact_build_us,
        ) = PITCH_RECORD.unpack(
            read_exact(port, PITCH_RECORD.size, response_deadline)
        )
        if required_mode == 3:
            ready = target == target_q8 and rendered_mode == required_mode
        else:
            ready = (
                target == target_q8
                and rendered == target_q8
                and mode == required_mode
                and rendered_mode == required_mode
            )
        if ready:
            print(
                f"pitch target/prepared: "
                f"{target / 256:.2f}/{rendered / 256:.2f} deg, "
                f"{PITCH_MODE_NAMES.get(mode, mode)}, "
                f"rendered={PITCH_MODE_NAMES.get(rendered_mode, rendered_mode)}, "
                f"preview {preview_build_us / 1000:.1f} ms, "
                f"exact {exact_build_us / 1000:.1f} ms, "
                f"exact-ready={bool(exact_ready)}, "
                f"preview-buffer={preview_index}"
            )
            return
        if time.monotonic() >= deadline:
            raise TimeoutError(
                f"pitch LUT not ready: target={target / 256:.2f}, "
                f"rendered={rendered / 256:.2f}, "
                f"mode={PITCH_MODE_NAMES.get(mode, mode)}, "
                f"effective={PITCH_MODE_NAMES.get(rendered_mode, rendered_mode)}"
            )
        time.sleep(0.1)


def wait_for_firmware(port: serial.Serial, timeout: float = 30.0) -> None:
    deadline = time.monotonic() + timeout
    while True:
        try:
            snapshot(port)
            return
        except TimeoutError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.25)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM21")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--warmup", type=float, default=5.0)
    parser.add_argument("--interval", type=float, default=10.0)
    parser.add_argument("--samples", type=int, default=4)
    parser.add_argument(
        "--pitch",
        type=int,
        default=0,
        help="freeze at a multiple of 10 degrees from -80 to 80",
    )
    parser.add_argument(
        "--phase",
        choices=("anchor", "preview", "exact"),
        default="exact",
        help="benchmark direct anchor blend, synthesized preview, or exact LUT",
    )
    args = parser.parse_args()
    if args.pitch < -80 or args.pitch > 80 or args.pitch % 10 != 0:
        parser.error("--pitch must be a multiple of 10 from -80 to 80")

    with serial.Serial(args.port, args.baud, timeout=0.25) as port:
        port.dtr = False
        port.rts = False
        time.sleep(args.warmup)
        wait_for_firmware(port)
        commands = bytearray(
            b"A" if args.phase == "anchor"
            else (b"V" if args.phase == "preview" else b"X")
        )
        commands.extend(b"0")
        if args.pitch > 0:
            commands.extend(b"]" * (args.pitch // 10))
        elif args.pitch < 0:
            commands.extend(b"[" * (-args.pitch // 10))
        port.write(commands)
        port.flush()
        required_mode = 0 if args.pitch == 0 else {
            "anchor": 3,
            "preview": 1,
            "exact": 2,
        }[args.phase]
        wait_for_pitch(port, args.pitch, required_mode)
        previous = snapshot(port)

        print(
            "sample  fps    render_ms  globe_ms  overlay_ms"
            "  transfer_ms  qspi_ms"
        )
        totals = [0.0] * 6
        for sample_index in range(1, args.samples + 1):
            time.sleep(args.interval)
            current = snapshot(port)
            elapsed_ms = delta32(current[0], previous[0])
            rendered = delta32(current[1], previous[1])
            transferred = delta32(current[2], previous[2])
            render_us = delta32(current[3], previous[3])
            globe_us = delta32(current[4], previous[4])
            overlay_us = delta32(current[5], previous[5])
            transfer_us = delta32(current[6], previous[6])
            qspi_us = delta32(current[7], previous[7])

            fps = rendered * 1000.0 / elapsed_ms
            render_ms = render_us / max(1, rendered) / 1000.0
            globe_ms = globe_us / max(1, rendered) / 1000.0
            overlay_ms = overlay_us / max(1, rendered) / 1000.0
            transfer_ms = transfer_us / max(1, transferred) / 1000.0
            qspi_ms = qspi_us / max(1, transferred) / 1000.0
            print(
                f"{sample_index:>6}  {fps:5.2f}  {render_ms:9.3f}"
                f"  {globe_ms:8.3f}  {overlay_ms:10.3f}"
                f"  {transfer_ms:11.3f}  {qspi_ms:7.3f}"
            )
            totals[0] += fps
            totals[1] += render_ms
            totals[2] += globe_ms
            totals[3] += overlay_ms
            totals[4] += transfer_ms
            totals[5] += qspi_ms
            previous = current

    count = max(1, args.samples)
    print(
        "average "
        f"{totals[0] / count:5.2f}  {totals[1] / count:9.3f}"
        f"  {totals[2] / count:8.3f}  {totals[3] / count:10.3f}"
        f"  {totals[4] / count:11.3f}  {totals[5] / count:7.3f}"
    )


if __name__ == "__main__":
    main()
