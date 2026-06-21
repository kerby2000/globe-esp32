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
output. Sending `Q` additionally returns cumulative render, transfer-task, and
QSPI-only timings.

## Rendering design

- `src/world_texture.h` is a packed 1024×512 intensity texture generated from
  Natural Earth 1:50m land, coastlines, major lakes, and major rivers. It has no
  political borders.
- Natural Earth vectors are rasterized at 4096×2048 and reduced with LANCZOS,
  preserving fractional coastline coverage before the 4-bit quantization.
- The detailed front surface remains at 1024×512. A separate 512×256 texture
  packs two four-bit blurred back-surface samples per byte, producing the
  transparent-globe underlay with lower PSRAM/cache traffic.
- At startup, `buildSphereLut()` projects globe pixels to longitude and stores
  longitude, four-bit shading, and two-bit subpixel rim coverage in a compact
  16-bit PSRAM LUT. Latitude is shared by each scanline.
- A static exterior cyan halo is baked into both reusable framebuffers. A 4×4
  setup-time coverage estimate smooths the sphere silhouette.
- The expensive `sqrt`, `asin`, and `atan2` calls run only once during setup.
- Each frame uses integer texture samples and directly indexes a 32 KB
  precomputed RGB565 color table. The hot scanline loop is unrolled by two,
  compiled with targeted `-O3`, and placed in internal executable RAM.
- The clock, weekday, and date use offline-generated four-bit alpha glyphs.
  The large time includes a baked soft cyan glow. Transparent glyph pixels are
  rejected at setup and the remaining pixels are stored as a compact command
  list, ready to rebuild when a real clock value changes.
- Frame rendering contains no floating-point math.
- Two PSRAM framebuffers form a producer/consumer pipeline: core 1 renders while
  core 0 transfers the previous frame over QSPI.
- The moving content is rendered directly into packed 423×423 framebuffers and
  transferred at screen position `(22,22)`. A temporary full-screen buffer
  transfers the static exterior halo once during startup.
- RGB565 is stored in panel byte order so QSPI can send framebuffer bytes
  directly without a per-pixel byte-swap pass.

Each packed framebuffer and the sphere LUT use about 358 KB. The dedicated
back-surface texture uses 64 KB of flash, while the direct color table uses
32 KB of internal RAM. The release build uses 8192-pixel QSPI chunks and an
80 MHz requested bus clock. The measured rate on the connected target board is
a stable 27.08 FPS with the antialiased overlay, compared with 5 FPS for the
initial single-buffer renderer.

## Performance profiling

Build and upload `waveshare_amoled_143_screenshot`, then run:

```powershell
python tools/profile_performance.py --port COM21
```

The script samples quiet on-demand counters and reports average FPS, complete
render time, globe and overlay phases, transfer-task time, and QSPI-only time.
Measurements on this board:

| Configuration | Render | Globe | Overlay | Transfer | FPS |
|---|---:|---:|---:|---:|---:|
| 1024×512 front/back baseline | 49.31 ms | 46.86 ms | 2.45 ms | 22.20 ms | 20.27 |
| + direct RGB565 color table | 46.89 ms | 44.44 ms | 2.45 ms | 22.51 ms | 21.32 |
| + pointer loop, unrolled by two | 43.73 ms | 41.28 ms | 2.45 ms | 23.11 ms | 22.86 |
| + dedicated packed 512×256 back layer | 41.05 ms | 38.60 ms | 2.45 ms | 21.71 ms | 24.35 |
| + targeted `-O3` | 37.99 ms | 35.59 ms | 2.41 ms | 22.13 ms | 26.31 |
| + hot loop in internal executable RAM | **36.91 ms** | **34.50 ms** | **2.41 ms** | **22.14 ms** | **27.08** |

The 60 MHz request measured identically to 40 MHz, consistent with the ESP32
SPI divider selecting the same effective clock. A 512×256 front texture remains
available as `GLOBE_USE_HALF_TEXTURE=1`, but it is not the release default
because its coastlines are visibly more pixelated. The optimized release only
reduces the intentionally blurred back layer, leaving the detailed front map
unchanged.

Rendering and QSPI transfer run concurrently on separate cores. Therefore their
times overlap; final FPS is limited by the slower 36.91 ms render stage rather
than the sum of render and transfer times.

## Capturing the framebuffer

This is available in `waveshare_amoled_143_debug` and in the quiet
`waveshare_amoled_143_screenshot` environment. The firmware accepts `S` over USB
serial and returns the current RGB565 framebuffer with dimensions and CRC32.
Capture it as a PNG with:

```powershell
pio run -e waveshare_amoled_143_screenshot -t upload --upload-port COM21
python tools/capture_screenshot.py --port COM21 --output globe-screenshot.png
```

This validates the renderer and framebuffer contents independently of the
physical AMOLED. It cannot detect panel power, wiring, or viewing problems.
With `GLOBE_TRANSFER_TIGHT=1`, the returned framebuffer is the packed 423×423
moving region rather than the static 466×466 panel background.

## Regenerating the map

The generated header is already included. To rebuild it:

```powershell
python tools/generate_world_map.py --supersample 4
python tools/generate_world_map.py --width 512 --height 256 --supersample 4 `
  --output src/world_texture_512.h --back-output src/world_back_512.h `
  --preview world-texture-512-preview.png
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
