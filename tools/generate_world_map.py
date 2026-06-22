#!/usr/bin/env python3
"""Generate the physical-feature intensity texture used by the globe.

The texture intentionally contains no political borders. It combines Natural
Earth 1:50m land, coastline, major lakes, and major rivers. Two 4-bit channels
are packed into every byte:

  high nibble: crisp front-surface line/fill intensity
  low nibble:  broad blurred intensity used for the transparent back surface

Natural Earth data is public domain:
https://www.naturalearthdata.com/about/terms-of-use/
"""

from __future__ import annotations

import argparse
import io
import json
import urllib.request
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter


NATURAL_EARTH_COMMIT = "ca96624a56bd078437bca8184e78163e5039ad19"
SOURCE_ROOT = (
    "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/"
    f"{NATURAL_EARTH_COMMIT}/geojson"
)
SOURCES = {
    "land": f"{SOURCE_ROOT}/ne_50m_land.geojson",
    "coastline": f"{SOURCE_ROOT}/ne_50m_coastline.geojson",
    "lakes": f"{SOURCE_ROOT}/ne_50m_lakes.geojson",
    "rivers": f"{SOURCE_ROOT}/ne_50m_rivers_lake_centerlines.geojson",
}


def fetch_geojson(url: str) -> dict:
    with urllib.request.urlopen(url, timeout=60) as response:
        return json.load(io.BytesIO(response.read()))


def geometry_parts(geometry: dict):
    kind = geometry["type"]
    coordinates = geometry["coordinates"]
    if kind == "LineString":
        yield "line", coordinates
    elif kind == "MultiLineString":
        for line in coordinates:
            yield "line", line
    elif kind == "Polygon":
        yield "polygon", coordinates
    elif kind == "MultiPolygon":
        for polygon in coordinates:
            yield "polygon", polygon


def project(point: list[float], width: int, height: int) -> tuple[float, float]:
    longitude, latitude = point[:2]
    return (
        (longitude + 180.0) * width / 360.0,
        (90.0 - latitude) * height / 180.0,
    )


def unwrap(points: list[tuple[float, float]], width: int):
    if not points:
        return points
    result = [points[0]]
    previous_x = points[0][0]
    for x, y in points[1:]:
        while x - previous_x > width / 2:
            x -= width
        while previous_x - x > width / 2:
            x += width
        result.append((x, y))
        previous_x = x
    return result


def wrapped_copies(points: list[tuple[float, float]], width: int):
    points = unwrap(points, width)
    center = sum(point[0] for point in points) / max(1, len(points))
    shift = round((width / 2 - center) / width) * width
    centered = [(x + shift, y) for x, y in points]
    for copy_shift in (-width, 0, width):
        yield [(x + copy_shift, y) for x, y in centered]


def draw_lines(
    image: Image.Image,
    geojson: dict,
    width: int,
    height: int,
    value: int,
    line_width: int,
    feature_filter=lambda _: True,
) -> None:
    draw = ImageDraw.Draw(image)
    for feature in geojson["features"]:
        if not feature_filter(feature["properties"]):
            continue
        for kind, coordinates in geometry_parts(feature["geometry"]):
            if kind == "polygon":
                lines = coordinates
            else:
                lines = [coordinates]
            for line in lines:
                points = [project(point, width, height) for point in line]
                for wrapped in wrapped_copies(points, width):
                    draw.line(wrapped, fill=value, width=line_width, joint="curve")


def draw_land(
    image: Image.Image, geojson: dict, width: int, height: int, value: int
) -> None:
    draw = ImageDraw.Draw(image)
    for feature in geojson["features"]:
        for kind, polygon in geometry_parts(feature["geometry"]):
            if kind != "polygon" or not polygon:
                continue
            for ring_index, ring in enumerate(polygon):
                points = [project(point, width, height) for point in ring]
                fill = value if ring_index == 0 else 0
                for wrapped in wrapped_copies(points, width):
                    draw.polygon(wrapped, fill=fill)


def scaled(image: Image.Image, numerator: int, denominator: int) -> Image.Image:
    return image.point(lambda value: value * numerator // denominator)


def build_texture(
    land_data: dict,
    coastline_data: dict,
    lakes_data: dict,
    rivers_data: dict,
    width: int,
    height: int,
    supersample: int,
) -> tuple[bytes, Image.Image, Image.Image]:
    work_width = width * supersample
    work_height = height * supersample
    line_width = max(1, supersample)
    land = Image.new("L", (work_width, work_height), 0)
    coastline = Image.new("L", (work_width, work_height), 0)
    lakes = Image.new("L", (work_width, work_height), 0)
    rivers = Image.new("L", (work_width, work_height), 0)

    draw_land(land, land_data, work_width, work_height, 255)
    draw_lines(
        coastline,
        coastline_data,
        work_width,
        work_height,
        255,
        line_width,
    )
    draw_lines(
        lakes,
        lakes_data,
        work_width,
        work_height,
        230,
        line_width,
        lambda properties: float(properties.get("min_zoom", 99)) <= 2.0,
    )
    draw_lines(
        rivers,
        rivers_data,
        work_width,
        work_height,
        175,
        line_width,
        lambda properties: float(properties.get("min_zoom", 99)) <= 2.0,
    )

    # Crisp physical lines plus a baked halo. Runtime sampling is therefore one
    # byte per surface instead of checking four neighboring map pixels.
    front = scaled(land, 14, 255)
    front = ImageChops.lighter(
        front,
        scaled(
            coastline.filter(ImageFilter.GaussianBlur(2.0 * supersample)),
            150,
            255,
        ),
    )
    front = ImageChops.lighter(
        front,
        scaled(
            lakes.filter(ImageFilter.GaussianBlur(1.6 * supersample)), 115, 255
        ),
    )
    front = ImageChops.lighter(
        front,
        scaled(
            rivers.filter(ImageFilter.GaussianBlur(1.3 * supersample)), 80, 255
        ),
    )
    front = ImageChops.lighter(front, coastline)
    front = ImageChops.lighter(front, lakes)
    front = ImageChops.lighter(front, rivers)

    # A broad, low-contrast version is sampled using the back-surface longitude.
    # This creates the translucent blurred continents visible through the globe.
    physical = ImageChops.lighter(coastline, lakes)
    physical = ImageChops.lighter(physical, scaled(rivers, 3, 5))
    back = scaled(
        land.filter(ImageFilter.GaussianBlur(2.5 * supersample)), 35, 255
    )
    back = ImageChops.lighter(
        back,
        scaled(
            physical.filter(ImageFilter.GaussianBlur(5.0 * supersample)),
            155,
            255,
        ),
    )

    # LANCZOS converts the high-resolution vector rasterization into fractional
    # edge coverage at the final texture size. The 4-bit quantizer preserves
    # those intermediate levels, so coastlines no longer staircase on-screen.
    target_size = (width, height)
    front = front.resize(target_size, Image.Resampling.LANCZOS)
    back = back.resize(target_size, Image.Resampling.LANCZOS)

    front_values = front.load()
    back_values = back.load()
    packed = bytearray(width * height)
    for y in range(height):
        row_offset = y * width
        for x in range(width):
            front_nibble = min(15, (front_values[x, y] + 8) // 17)
            back_nibble = min(15, (back_values[x, y] + 8) // 17)
            packed[row_offset + x] = (front_nibble << 4) | back_nibble
    return bytes(packed), front, back


def write_header(output: Path, packed: bytes, width: int, height: int) -> None:
    byte_lines = []
    for offset in range(0, len(packed), 16):
        chunk = packed[offset : offset + 16]
        byte_lines.append("    " + ", ".join(f"0x{value:02X}" for value in chunk) + ",")

    content = f"""\
#pragma once

#include <Arduino.h>

// Generated by tools/generate_world_map.py from Natural Earth 1:50m physical
// vectors. High nibble = detailed front intensity; low nibble = blurred back.
namespace world_texture {{

static constexpr uint16_t kWidth = {width};
static constexpr uint16_t kHeight = {height};
static constexpr uint32_t kByteCount = {len(packed)};

static const uint8_t kIntensity[kByteCount] PROGMEM = {{
{chr(10).join(byte_lines)}
}};

inline uint8_t sample(uint16_t x, uint16_t y) {{
  return pgm_read_byte(kIntensity + static_cast<uint32_t>(y) * kWidth + x);
}}

}}  // namespace world_texture
"""
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(content, encoding="utf-8", newline="\n")


def write_back_header(
    output: Path, back: Image.Image, width: int, height: int
) -> None:
    values = back.load()
    packed = bytearray((width * height + 1) // 2)
    for y in range(height):
        for x in range(width):
            pixel_index = y * width + x
            nibble = min(15, (values[x, y] + 8) // 17)
            if pixel_index & 1:
                packed[pixel_index >> 1] |= nibble
            else:
                packed[pixel_index >> 1] = nibble << 4

    byte_lines = []
    for offset in range(0, len(packed), 16):
        chunk = packed[offset : offset + 16]
        byte_lines.append(
            "    " + ", ".join(f"0x{value:02X}" for value in chunk) + ","
        )

    content = f"""\
#pragma once

#include <Arduino.h>

// Generated by tools/generate_world_map.py. Two blurred back-surface
// intensity texels are packed per byte, high nibble first.
namespace world_back_texture {{

static constexpr uint16_t kWidth = {width};
static constexpr uint16_t kHeight = {height};
static constexpr uint32_t kByteCount = {len(packed)};
static constexpr uint16_t kRowByteCount = kWidth / 2;

static const uint8_t kIntensity[kByteCount] PROGMEM = {{
{chr(10).join(byte_lines)}
}};

}}  // namespace world_back_texture
"""
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(content, encoding="utf-8", newline="\n")


def write_front_header(
    output: Path, front: Image.Image, width: int, height: int
) -> None:
    values = front.load()
    packed = bytearray((width * height + 1) // 2)
    for y in range(height):
        for x in range(width):
            pixel_index = y * width + x
            nibble = min(15, (values[x, y] + 8) // 17)
            if pixel_index & 1:
                packed[pixel_index >> 1] |= nibble
            else:
                packed[pixel_index >> 1] = nibble << 4

    byte_lines = []
    for offset in range(0, len(packed), 16):
        chunk = packed[offset : offset + 16]
        byte_lines.append(
            "    " + ", ".join(f"0x{value:02X}" for value in chunk) + ","
        )

    content = f"""\
#pragma once

#include <Arduino.h>

// Generated by tools/generate_world_map.py. Two detailed front-surface
// intensity texels are packed per byte, high nibble first.
namespace world_front_texture {{

static constexpr uint16_t kWidth = {width};
static constexpr uint16_t kHeight = {height};
static constexpr uint32_t kByteCount = {len(packed)};
static constexpr uint16_t kRowByteCount = kWidth / 2;

static const uint8_t kIntensity[kByteCount] PROGMEM = {{
{chr(10).join(byte_lines)}
}};

}}  // namespace world_front_texture
"""
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(content, encoding="utf-8", newline="\n")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--width", type=int, default=1024)
    parser.add_argument("--height", type=int, default=512)
    parser.add_argument("--supersample", type=int, default=4)
    parser.add_argument("--output", type=Path, default=Path("src/world_texture.h"))
    parser.add_argument("--preview", type=Path, default=Path("world-texture-preview.png"))
    parser.add_argument("--front-output", type=Path)
    parser.add_argument("--back-output", type=Path)
    args = parser.parse_args()
    if args.supersample < 1:
        parser.error("--supersample must be at least 1")

    data = {name: fetch_geojson(url) for name, url in SOURCES.items()}
    packed, front, back = build_texture(
        data["land"],
        data["coastline"],
        data["lakes"],
        data["rivers"],
        args.width,
        args.height,
        args.supersample,
    )
    write_header(args.output, packed, args.width, args.height)
    if args.front_output is not None:
        write_front_header(args.front_output, front, args.width, args.height)
    if args.back_output is not None:
        write_back_header(args.back_output, back, args.width, args.height)

    preview = Image.new("RGB", (args.width, args.height))
    preview_pixels = preview.load()
    front_pixels = front.load()
    back_pixels = back.load()
    for y in range(args.height):
        for x in range(args.width):
            preview_pixels[x, y] = (
                back_pixels[x, y] // 3,
                min(255, front_pixels[x, y] + back_pixels[x, y] // 2),
                min(255, front_pixels[x, y] + back_pixels[x, y]),
            )
    preview.save(args.preview)
    print(f"Wrote {args.output} ({len(packed)} packed texture bytes)")
    if args.front_output is not None:
        print(
            f"Wrote {args.front_output} "
            f"({args.width * args.height // 2} packed front-texture bytes)"
        )
    if args.back_output is not None:
        print(
            f"Wrote {args.back_output} "
            f"({args.width * args.height // 2} packed back-texture bytes)"
        )
    print(f"Wrote {args.preview}")
    print(
        f"Rasterized at {args.width * args.supersample}x"
        f"{args.height * args.supersample}, downsampled with LANCZOS"
    )


if __name__ == "__main__":
    main()
