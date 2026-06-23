#!/usr/bin/env python3
"""Capture a deterministic ESP32 globe rotation and encode it as an MP4."""

from __future__ import annotations

import argparse
import shutil
import struct
import subprocess
import tempfile
import time
import zlib
from pathlib import Path

import serial

from capture_screenshot import (
    MAGIC,
    find_magic,
    read_exact,
    rgb565_to_image,
)

PITCH_MAGIC = b"GLBT"
PITCH_RECORD = struct.Struct("<hhBBBBII")
PITCH_MODE_NAMES = {0: "zero", 1: "preview", 2: "exact", 3: "anchor"}


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


def request_pitch_status(
    port: serial.Serial, timeout: float
) -> tuple[int, int, int, int, int, int, int, int]:
    port.reset_input_buffer()
    port.write(b"T")
    port.flush()
    deadline = time.monotonic() + timeout
    find_marker(port, PITCH_MAGIC, deadline)
    return PITCH_RECORD.unpack(
        read_exact(port, PITCH_RECORD.size, deadline)
    )


def request_frame(
    port: serial.Serial, timeout: float
) -> tuple[int, int, bytes, int]:
    port.reset_input_buffer()
    port.write(b"S")
    port.flush()
    deadline = time.monotonic() + timeout
    find_magic(port, deadline)
    width, height, byte_count, expected_crc = struct.unpack(
        "<HHII", read_exact(port, 12, deadline)
    )
    if byte_count != width * height * 2:
        raise ValueError(
            f"invalid payload size {byte_count} for "
            f"{width}x{height} RGB565"
        )
    payload = read_exact(port, byte_count, deadline)
    actual_crc = zlib.crc32(payload)
    if actual_crc != expected_crc:
        raise ValueError(
            f"CRC mismatch: device={expected_crc:08x}, "
            f"received={actual_crc:08x}"
        )
    return width, height, payload, actual_crc


def encode_mp4(
    frame_pattern: Path, output: Path, fps: float, width: int, height: int
) -> None:
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        raise RuntimeError(
            "ffmpeg was not found. Install it or use --keep-frames "
            "and encode the PNG sequence separately."
        )
    output.parent.mkdir(parents=True, exist_ok=True)
    # RGB565 screenshots are 423x423 in tight-transfer builds. H.264 yuv420p
    # requires even dimensions, so pad one black row and column when needed.
    pad_width = width + (width & 1)
    pad_height = height + (height & 1)
    command = [
        ffmpeg,
        "-y",
        "-loglevel",
        "error",
        "-framerate",
        str(fps),
        "-i",
        str(frame_pattern),
        "-vf",
        f"pad={pad_width}:{pad_height}:0:0:black",
        "-c:v",
        "libx264",
        "-preset",
        "slow",
        "-crf",
        "16",
        "-pix_fmt",
        "yuv420p",
        "-movflags",
        "+faststart",
        str(output),
    ]
    subprocess.run(command, check=True)


def configure_capture(
    port: serial.Serial,
    pitch: int,
    globe_only: bool,
    phase: str,
    no_back: bool,
    settle: float,
    timeout: float,
) -> None:
    if pitch < -80 or pitch > 80 or pitch % 10 != 0:
        raise ValueError("pitch must be a multiple of 10 from -80 to 80")
    # '0' resets longitude/pitch and freezes auto-rotation. The clock mode is
    # independently selected so movies can isolate geography if desired.
    commands = bytearray(
        b"A" if phase == "anchor"
        else (b"V" if phase == "preview" else b"X")
    )
    commands.extend(b"0")
    if no_back:
        commands.extend(b"B")
    if globe_only:
        commands.extend(b"4")
    if pitch > 0:
        commands.extend(b"]" * (pitch // 10))
    elif pitch < 0:
        commands.extend(b"[" * (-pitch // 10))
    port.write(commands)
    port.flush()
    deadline = time.monotonic() + timeout
    target_q8 = pitch * 256
    required_mode = 0 if pitch == 0 else {
        "anchor": 3,
        "preview": 1,
        "exact": 2,
    }[phase]
    while True:
        time.sleep(settle)
        (
            target,
            rendered,
            mode,
            exact_ready,
            preview_index,
            rendered_mode,
            preview_build_us,
            exact_build_us,
        ) = request_pitch_status(
            port, min(5.0, max(0.5, deadline - time.monotonic()))
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
                f"Pitch target/prepared ready: "
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


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM21")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--pitch", type=int, default=80)
    parser.add_argument(
        "--phase",
        choices=("anchor", "preview", "exact"),
        default="exact",
        help="capture direct anchor blend, synthesized preview, or exact LUT",
    )
    parser.add_argument(
        "--frames",
        type=int,
        default=64,
        help="64 frames with the firmware's 16-texel step is one rotation",
    )
    parser.add_argument(
        "--step-texels",
        type=int,
        default=16,
        help="longitude advance per frame; must be a positive multiple of 16",
    )
    parser.add_argument("--fps", type=float, default=12.0)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--settle", type=float, default=0.08)
    parser.add_argument("--output", type=Path, default=Path("globe.mp4"))
    parser.add_argument(
        "--with-overlay",
        action="store_true",
        help="keep the current clock overlay instead of selecting globe-only",
    )
    parser.add_argument(
        "--no-back",
        action="store_true",
        help="toggle the diagnostic back hemisphere off after reset",
    )
    parser.add_argument(
        "--keep-frames",
        type=Path,
        help="copy the captured PNG sequence to this directory",
    )
    args = parser.parse_args()

    if args.frames <= 0:
        parser.error("--frames must be positive")
    if args.step_texels <= 0 or args.step_texels % 16 != 0:
        parser.error("--step-texels must be a positive multiple of 16")
    if args.fps <= 0:
        parser.error("--fps must be positive")

    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="globe-movie-") as temporary:
        frame_directory = Path(temporary)
        width = height = 0
        with serial.Serial(
            args.port, args.baud, timeout=0.25
        ) as port:
            port.dtr = False
            port.rts = False
            port.reset_input_buffer()
            time.sleep(0.5)
            # Opening USB CDC can reset the board. Complete and discard one
            # request first so setup/LUT generation cannot consume the capture
            # configuration and first 'S' command in the same service pass.
            print("Waiting for firmware and pitch LUT initialization...")
            request_frame(port, args.timeout)
            configure_capture(
                port,
                args.pitch,
                not args.with_overlay,
                args.phase,
                args.no_back,
                args.settle,
                args.timeout,
            )

            for index in range(args.frames):
                width, height, payload, crc = request_frame(
                    port, args.timeout
                )
                image = rgb565_to_image(payload, width, height)
                image.save(frame_directory / f"frame-{index:04d}.png")
                print(
                    f"\rCaptured {index + 1}/{args.frames} "
                    f"(CRC32 {crc:08x})",
                    end="",
                    flush=True,
                )
                if index + 1 < args.frames:
                    port.write(b"." * (args.step_texels // 16))
                    port.flush()
                    time.sleep(args.settle)
            print()

        encode_mp4(
            frame_directory / "frame-%04d.png",
            args.output,
            args.fps,
            width,
            height,
        )
        if args.keep_frames is not None:
            args.keep_frames.mkdir(parents=True, exist_ok=True)
            for frame in frame_directory.glob("frame-*.png"):
                shutil.copy2(frame, args.keep_frames / frame.name)

    elapsed = time.monotonic() - started
    print(
        f"Saved {args.output} ({args.frames} frames, {args.fps:g} FPS, "
        f"{elapsed:.1f} s capture/encode time)"
    )


if __name__ == "__main__":
    main()
