#!/usr/bin/env python3
"""Cut the circular display opening from a chroma-keyed device mockup."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--screen-box",
        default="265,250,979,971",
        help="left,top,right,bottom display opening in source pixels",
    )
    parser.add_argument("--feather", type=float, default=1.5)
    args = parser.parse_args()

    box = tuple(int(value) for value in args.screen_box.split(","))
    if len(box) != 4:
        parser.error("--screen-box requires four comma-separated integers")

    image = Image.open(args.input).convert("RGBA")
    hole = Image.new("L", image.size, 0)
    draw = ImageDraw.Draw(hole)
    inset = max(1, round(args.feather * 2))
    draw.ellipse(
        (
            box[0] + inset,
            box[1] + inset,
            box[2] - inset,
            box[3] - inset,
        ),
        fill=255,
    )
    if args.feather > 0:
        hole = hole.filter(ImageFilter.GaussianBlur(args.feather))

    red, green, blue, _alpha = image.split()
    # Pillow's ImageMath is unnecessary here: subtract the screen mask from
    # the existing outer alpha with a compact point-wise byte operation.
    alpha_bytes = bytes(
        max(0, outer - opening)
        for outer, opening in zip(image.getchannel("A").tobytes(), hole.tobytes())
    )
    alpha = Image.frombytes("L", image.size, alpha_bytes)
    output = Image.merge("RGBA", (red, green, blue, alpha))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    output.save(args.output)
    print(f"Saved {args.output} ({image.width}x{image.height})")


if __name__ == "__main__":
    main()
