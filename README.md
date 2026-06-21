# ESP32-S3 real-time rotating globe

Minimal full-screen globe demo for the original Waveshare
ESP32-S3-Touch-AMOLED-1.43 (466×466 QSPI AMOLED).

The visual follows the supplied reference: black background, transparent dark
sphere, cyan/white geographic outlines, a soft atmospheric rim, fixed clock/date
placeholder text, and an optional debug FPS counter.

## Build and upload

Install PlatformIO, connect the board, then run:

```powershell
$env:PYTHONUTF8 = "1"  # avoids an esptool progress-bar code-page error on Windows
pio run
pio run -t upload
pio device monitor
```

The default `waveshare_amoled_143` environment is the release build. USB serial
logging, screenshot handling, and the on-screen FPS counter are compiled out:

```ini
GLOBE_ENABLE_SERIAL_STATS=0
GLOBE_ENABLE_SCREENSHOT=0
```

Use the separate debug environment when profiling or capturing a framebuffer:

```powershell
pio run -e waveshare_amoled_143_debug -t upload --upload-port COM21
pio device monitor --port COM21 --baud 115200
```

The debug build defines both flags as `1`. Keeping serial output out of the
release frame loop prevents an unconsumed USB CDC transmit buffer from blocking
rendering after several seconds.

`waveshare_amoled_143_screenshot` is a quiet diagnostic environment with
screenshots enabled but periodic serial stats disabled. Sending `P` returns a
compact uptime/frame-count record for stability testing without autonomous USB
output.

## Rendering design

- `src/world_texture.h` is a packed 1024×512 intensity texture generated from
  Natural Earth 1:50m land, coastlines, major lakes, and major rivers. It has no
  political borders.
- Natural Earth vectors are rasterized at 4096×2048 and reduced with LANCZOS,
  preserving fractional coastline coverage before the 4-bit quantization.
- Each texture byte contains a detailed front-surface channel and a broad
  blurred back-surface channel, producing the transparent-globe underlay.
- At startup, `buildSphereLut()` projects globe pixels to longitude and stores
  longitude, four-bit shading, and two-bit subpixel rim coverage in a compact
  16-bit PSRAM LUT. Latitude is shared by each scanline.
- A static exterior cyan halo is baked into both reusable framebuffers. A 4×4
  setup-time coverage estimate smooths the sphere silhouette.
- The expensive `sqrt`, `asin`, and `atan2` calls run only once during setup.
- Each frame uses two integer texture samples, selects a precomputed RGB565
  palette entry, and composites the fixed overlay.
- The clock, weekday, and date use offline-generated four-bit alpha glyphs.
  The large time includes a baked soft cyan glow. Transparent glyph pixels are
  rejected at setup and the remaining pixels are stored as a compact command
  list, ready to rebuild when a real clock value changes.
- Frame rendering contains no floating-point math.
- Two PSRAM framebuffers form a producer/consumer pipeline: core 1 renders while
  core 0 transfers the previous frame over QSPI.
- RGB565 is stored in panel byte order so QSPI can send framebuffer bytes
  directly without a per-pixel byte-swap pass.

Each framebuffer uses about 434 KB and the sphere LUT about 358 KB. The measured
rate on the target board is a stable 19.25 FPS with the antialiased overlay,
compared with 5 FPS for the initial single-buffer renderer.

## Capturing the framebuffer

This is available in `waveshare_amoled_143_debug` and in the quiet
`waveshare_amoled_143_screenshot` environment. The firmware accepts `S` over USB
serial and returns the current 466×466 RGB565 frame with dimensions and CRC32.
Capture it as a PNG with:

```powershell
pio run -e waveshare_amoled_143_screenshot -t upload --upload-port COM21
python tools/capture_screenshot.py --port COM21 --output globe-screenshot.png
```

This validates the renderer and framebuffer contents independently of the
physical AMOLED. It cannot detect panel power, wiring, or viewing problems.

## Regenerating the map

The generated header is already included. To rebuild it:

```powershell
python tools/generate_world_map.py --supersample 4
```

The script downloads pinned Natural Earth physical vector layers, filters lakes
and rivers to major features, and bakes detailed/blurred intensity channels at
4× resolution before LANCZOS downsampling.
Natural Earth data is public domain:
<https://www.naturalearthdata.com/about/terms-of-use/>.

## Regenerating the overlay fonts

The generated `src/ui_fonts.h` is committed, so firmware builds do not need
desktop fonts or Pillow. To regenerate it from the Windows Segoe UI faces:

```powershell
python tools/generate_ui_fonts.py --supersample 4
```

Glyphs and the clock glow are rendered at 4× resolution, downsampled with
LANCZOS, cropped, quantized to four-bit alpha, and packed two pixels per byte.

## Board variants

This pinout targets the original `ESP32-S3-Touch-AMOLED-1.43`:

| Signal | GPIO |
|---|---:|
| QSPI CS | 9 |
| QSPI CLK | 10 |
| QSPI D0..D3 | 11, 12, 13, 14 |
| AMOLED reset | 21 |
| AMOLED enable | 42 |

The newer `ESP32-S3-Touch-AMOLED-1.43C` uses a different display pinout and is
not source-compatible with this configuration.

The original board itself shipped with either SH8601 or CO5300-compatible panel
controllers. Firmware reads the display ID at startup and selects the matching
initialization sequence and column offset.
