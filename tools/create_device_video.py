#!/usr/bin/env python3
"""Composite ESP32 framebuffer frames into a virtual round device body."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont


def make_background(size: tuple[int, int]) -> Image.Image:
    width, height = size
    background = Image.new("RGB", size)
    pixels = background.load()
    center_x = width * 0.48
    center_y = height * 0.40
    maximum = (width * width + height * height) ** 0.5
    for y in range(height):
        for x in range(width):
            distance = (
                ((x - center_x) ** 2 + (y - center_y) ** 2) ** 0.5
                / maximum
            )
            blue = max(8, round(24 - 18 * distance))
            pixels[x, y] = (3, 7 + blue // 4, blue)
    return background


def encode_mp4(frame_pattern: Path, output: Path, fps: float) -> None:
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        raise RuntimeError("ffmpeg was not found")
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            ffmpeg,
            "-y",
            "-loglevel",
            "error",
            "-framerate",
            str(fps),
            "-i",
            str(frame_pattern),
            "-c:v",
            "libx264",
            "-preset",
            "slow",
            "-crf",
            "18",
            "-pix_fmt",
            "yuv420p",
            "-movflags",
            "+faststart",
            str(output),
        ],
        check=True,
    )


def encode_gif(frame_pattern: Path, output: Path, fps: float) -> None:
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        raise RuntimeError("ffmpeg was not found")
    output.parent.mkdir(parents=True, exist_ok=True)
    filter_graph = (
        f"fps={fps},scale=720:-1:flags=lanczos,split[s0][s1];"
        "[s0]palettegen=max_colors=128:stats_mode=diff[p];"
        "[s1][p]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle"
    )
    subprocess.run(
        [
            ffmpeg,
            "-y",
            "-loglevel",
            "error",
            "-framerate",
            str(fps),
            "-i",
            str(frame_pattern),
            "-filter_complex",
            filter_graph,
            "-loop",
            "0",
            str(output),
        ],
        check=True,
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--frames", type=Path, required=True)
    parser.add_argument("--device", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--preview-gif", type=Path)
    parser.add_argument("--poster", type=Path)
    parser.add_argument("--fps", type=float, default=12.0)
    parser.add_argument("--label")
    parser.add_argument("--transition-label")
    parser.add_argument("--transition-frame", type=int)
    parser.add_argument(
        "--screen-box",
        default="265,250,979,971",
        help="left,top,right,bottom display opening in device pixels",
    )
    args = parser.parse_args()

    frame_paths = sorted(args.frames.glob("frame-*.png"))
    if not frame_paths:
        raise FileNotFoundError(f"No frame-*.png files in {args.frames}")
    screen_box = tuple(int(value) for value in args.screen_box.split(","))
    if len(screen_box) != 4:
        parser.error("--screen-box requires four comma-separated integers")

    device = Image.open(args.device).convert("RGBA")
    background = make_background(device.size)
    try:
        label_font = ImageFont.truetype("arial.ttf", 25)
    except OSError:
        label_font = ImageFont.load_default()
    shadow_alpha = device.getchannel("A").filter(ImageFilter.GaussianBlur(18))
    shadow = Image.new("RGBA", device.size, (0, 0, 0, 0))
    shadow.putalpha(shadow_alpha.point(lambda value: value * 110 // 255))

    screen_width = screen_box[2] - screen_box[0]
    screen_height = screen_box[3] - screen_box[1]
    screen_mask = Image.new("L", (screen_width, screen_height), 0)
    ImageDraw.Draw(screen_mask).ellipse(
        (2, 2, screen_width - 3, screen_height - 3), fill=255
    )
    screen_mask = screen_mask.filter(ImageFilter.GaussianBlur(0.8))

    with tempfile.TemporaryDirectory(prefix="globe-device-video-") as temp:
        rendered = Path(temp)
        first_composite: Image.Image | None = None
        for index, frame_path in enumerate(frame_paths):
            frame = Image.open(frame_path).convert("RGB")
            frame = frame.resize(
                (screen_width, screen_height), Image.Resampling.LANCZOS
            )
            canvas = background.copy().convert("RGBA")
            canvas.alpha_composite(shadow, (12, 18))
            canvas.paste(frame, (screen_box[0], screen_box[1]), screen_mask)
            canvas.alpha_composite(device)
            label = args.label
            if (
                args.transition_label is not None
                and args.transition_frame is not None
                and index >= args.transition_frame
            ):
                label = args.transition_label
            if label:
                draw = ImageDraw.Draw(canvas)
                text_box = draw.textbbox((0, 0), label, font=label_font)
                label_width = text_box[2] - text_box[0] + 44
                label_box = (42, 42, 42 + label_width, 92)
                draw.rounded_rectangle(
                    label_box,
                    radius=20,
                    fill=(2, 15, 25, 220),
                    outline=(40, 190, 220, 220),
                    width=2,
                )
                draw.text(
                    (64, 53),
                    label,
                    font=label_font,
                    fill=(190, 245, 255, 255),
                )
            if first_composite is None:
                first_composite = canvas.copy()
            canvas.convert("RGB").save(
                rendered / f"frame-{index:04d}.png",
                optimize=True,
            )

        pattern = rendered / "frame-%04d.png"
        encode_mp4(pattern, args.output, args.fps)
        if args.preview_gif is not None:
            encode_gif(pattern, args.preview_gif, args.fps)
        if args.poster is not None and first_composite is not None:
            args.poster.parent.mkdir(parents=True, exist_ok=True)
            first_composite.convert("RGB").save(
                args.poster, quality=92, optimize=True
            )

    print(f"Saved {args.output}")
    if args.preview_gif is not None:
        print(f"Saved {args.preview_gif}")
    if args.poster is not None:
        print(f"Saved {args.poster}")


if __name__ == "__main__":
    main()
