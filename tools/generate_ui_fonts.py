#!/usr/bin/env python3
"""Generate compact 4-bit-alpha UI fonts for the globe watch overlay.

The generated header is committed, so firmware builds do not depend on fonts
being installed. Regeneration currently uses Windows Segoe UI faces.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont


@dataclass(frozen=True)
class FontSpec:
    name: str
    path: Path
    size: int
    characters: str
    padding: int
    glow_radius: float = 0.0
    glow_strength: float = 0.0


WEEKDAYS = "MONDAYTUESDAYWEDNESDAYTHURSDAYFRIDAYSATURDAYSUNDAY"
MONTHS = "JANFEBMARAPRMAYJUNJULAUGSEPOCTNOVDEC"


def unique_characters(text: str) -> str:
    return "".join(sorted(set(text)))


def quantize_4bit(image: Image.Image) -> bytes:
    values = list(image.getdata())
    packed = bytearray((len(values) + 1) // 2)
    for index, value in enumerate(values):
        nibble = min(15, (value + 8) // 17)
        if index & 1:
            packed[index >> 1] |= nibble
        else:
            packed[index >> 1] = nibble << 4
    return bytes(packed)


def render_glyph(
    font_path: Path,
    size: int,
    character: str,
    padding: int,
    supersample: int,
) -> tuple[Image.Image, int]:
    high_font = ImageFont.truetype(str(font_path), size * supersample)
    ascent, descent = high_font.getmetrics()
    advance = max(1, round(high_font.getlength(character) / supersample))
    width = (advance + padding * 2) * supersample
    height = (size + padding * 2) * supersample
    image = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(image)

    # Baseline positioning keeps every glyph in a font on one common line box.
    baseline = padding * supersample + min(ascent, size * supersample)
    draw.text(
        (padding * supersample, baseline),
        character,
        font=high_font,
        fill=255,
        anchor="ls",
    )
    image = image.resize(
        (advance + padding * 2, size + padding * 2), Image.Resampling.LANCZOS
    )
    return image, advance


def format_bytes(data: bytes) -> str:
    lines = []
    for offset in range(0, len(data), 16):
        chunk = data[offset : offset + 16]
        lines.append("    " + ", ".join(f"0x{value:02X}" for value in chunk) + ",")
    return "\n".join(lines)


def generate_font(spec: FontSpec, supersample: int) -> tuple[str, str, str]:
    alpha_data = bytearray()
    glow_data = bytearray()
    glyph_rows = []

    for character in spec.characters:
        alpha, advance = render_glyph(
            spec.path, spec.size, character, spec.padding, supersample
        )
        glow = Image.new("L", alpha.size, 0)
        if spec.glow_radius > 0:
            glow = alpha.filter(ImageFilter.GaussianBlur(spec.glow_radius))
            glow = glow.point(lambda value: min(255, round(value * spec.glow_strength)))

        combined = ImageChops.lighter(alpha, glow)
        bounds = combined.getbbox()
        if bounds is None:
            bounds = (0, 0, 1, 1)
        alpha = alpha.crop(bounds)
        glow = glow.crop(bounds)

        alpha_offset = len(alpha_data)
        glow_offset = len(glow_data)
        alpha_data.extend(quantize_4bit(alpha))
        glow_data.extend(quantize_4bit(glow))
        glyph_rows.append(
            "    {"
            f"'{character}', {alpha_offset}, {glow_offset}, "
            f"{alpha.width}, {alpha.height}, "
            f"{-spec.padding + bounds[0]}, {-spec.padding + bounds[1]}, "
            f"{advance}"
            "},"
        )

    prefix = f"k{spec.name}"
    declarations = f"""\
static const Glyph {prefix}Glyphs[] PROGMEM = {{
{chr(10).join(glyph_rows)}
}};

static const uint8_t {prefix}Alpha[] PROGMEM = {{
{format_bytes(bytes(alpha_data))}
}};

static const uint8_t {prefix}Glow[] PROGMEM = {{
{format_bytes(bytes(glow_data))}
}};

static constexpr Font {prefix} = {{
    {prefix}Glyphs,
    static_cast<uint8_t>(sizeof({prefix}Glyphs) / sizeof({prefix}Glyphs[0])),
    {prefix}Alpha,
    {prefix}Glow,
    {spec.size},
}};
"""
    stats = (
        f"{spec.name}: {len(spec.characters)} glyphs, "
        f"{len(alpha_data) + len(glow_data)} bytes"
    )
    return declarations, stats, prefix


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=Path("src/ui_fonts.h"))
    parser.add_argument("--supersample", type=int, default=4)
    parser.add_argument(
        "--font-dir", type=Path, default=Path(r"C:\Windows\Fonts")
    )
    args = parser.parse_args()

    specs = (
        FontSpec(
            "Time",
            args.font_dir / "segoeuil.ttf",
            104,
            "0123456789:",
            8,
            glow_radius=4.0,
            glow_strength=0.72,
        ),
        FontSpec(
            "Weekday",
            args.font_dir / "segoeuib.ttf",
            24,
            unique_characters(WEEKDAYS),
            2,
        ),
        FontSpec(
            "Date",
            args.font_dir / "segoeui.ttf",
            18,
            unique_characters("0123456789 " + MONTHS),
            2,
        ),
    )

    for spec in specs:
        if not spec.path.exists():
            raise FileNotFoundError(spec.path)

    blocks = []
    stats = []
    for spec in specs:
        block, stat, _ = generate_font(spec, args.supersample)
        blocks.append(block)
        stats.append(stat)

    content = f"""\
#pragma once

#include <Arduino.h>

// Generated by tools/generate_ui_fonts.py. Alpha and glow pixels are packed
// two per byte, high nibble first.
namespace ui_fonts {{

struct Glyph {{
  char character;
  uint32_t alphaOffset;
  uint32_t glowOffset;
  uint8_t width;
  uint8_t height;
  int8_t xOffset;
  int8_t yOffset;
  uint8_t advance;
}};

struct Font {{
  const Glyph *glyphs;
  uint8_t glyphCount;
  const uint8_t *alpha;
  const uint8_t *glow;
  uint8_t lineHeight;
}};

{chr(10).join(blocks)}

}}  // namespace ui_fonts
"""
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(content, encoding="utf-8", newline="\n")
    print(f"Wrote {args.output}")
    for stat in stats:
        print(stat)


if __name__ == "__main__":
    main()
