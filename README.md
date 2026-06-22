# ESP32-S3 real-time rotating globe

Minimal full-screen globe demo for the original Waveshare
ESP32-S3-Touch-AMOLED-1.43 (466×466 QSPI AMOLED).

The visual follows the supplied reference: black background, transparent dark
sphere, cyan/white geographic outlines, a soft atmospheric rim, fixed clock/date
placeholder text, and an optional debug FPS counter.

## Touch interaction

The FT3168 touch controller is enabled in all build profiles:

- Touch and hold pauses automatic rotation.
- Drag horizontally to rotate the globe manually. A full-width drag is one
  complete revolution.
- Release after a drag to continue with inertia; the speed then settles back
  to automatic rotation.
- Double-tap resets the globe to its startup longitude.
- A stationary 700 ms long press is detected by `handleLongPress()` as the hook
  for a future settings/speed-control screen.

This first interaction pass intentionally leaves the sphere latitude fixed.
Touch uses the board's shared 300 kHz I2C bus on GPIO47/48 and adds no
floating-point work to the frame loop. Startup allows 250 ms for the FT3168 to
finish powering up instead of relying on a single early probe. If detection
still fails, rendering continues with automatic rotation while touch recovery
is retried once per second.

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

- `src/world_front_1024.h` stores the Natural Earth 1:50m land, coastlines,
  major lakes, and major rivers as two four-bit front-surface texels per byte.
  It has no political borders and is pixel-equivalent to the former high
  nibbles in `src/world_texture.h`.
- Natural Earth vectors are rasterized at 4096×2048 and reduced with LANCZOS,
  preserving fractional coastline coverage before the 4-bit quantization.
- The detailed front surface remains at 1024×512 but now occupies 256 KB
  instead of 512 KB. A separate 512×256 texture packs two four-bit blurred
  back-surface samples per byte. That layer is generated from blurred
  coastlines, major lakes/rivers, and deterministic geographically localized
  fog; it contains no filled land shadow.
- At startup, `buildSphereLut()` projects globe pixels to longitude and stores
  longitude, four-bit shading, and two-bit subpixel rim coverage in a compact
  16-bit PSRAM LUT. Latitude is shared by each scanline.
- A 4×4 setup-time coverage estimate provides one-pixel silhouette
  antialiasing. The experimental wide soft-rim treatment was removed.
- The original subtle exterior cyan halo is baked into both reusable
  framebuffers, adding no recurring render cost.
- The expensive `sqrt`, `asin`, and `atan2` calls run only once during setup.
- Each frame uses integer texture samples and directly indexes a 32 KB
  precomputed RGB565 color table. The hot scanline loop is unrolled by eight,
  compiled with targeted `-O3`, and placed in internal executable RAM.
- The clock, weekday, and date use offline-generated four-bit alpha glyphs.
  The large time includes a baked soft cyan glow. Transparent glyph pixels are
  rejected at setup and the remaining pixels are stored as a compact command
  list, ready to rebuild when a real clock value changes.
- Exact RGB565 overlay blend tables are generated during setup. Fixed-overlay
  composition runs on the display core immediately before QSPI transfer,
  overlapping it with the next globe render instead of extending the critical
  render stage.
- Frame rendering contains no floating-point math.
- Two PSRAM framebuffers form a producer/consumer pipeline: core 1 renders while
  core 0 transfers the previous frame over QSPI.
- The moving content is rendered directly into packed 423×423 framebuffers and
  transferred at screen position `(22,22)`. A temporary full-screen buffer
  transfers the static exterior halo once during startup.
- RGB565 is stored in panel byte order so QSPI can send framebuffer bytes
  directly without a per-pixel byte-swap pass.

Each packed framebuffer and the sphere LUT use about 358 KB. The front and back
textures use 256 KB and 64 KB of flash. The direct globe color table uses 32 KB
of internal RAM and the exact overlay component tables use 36 KB. The release
build uses 8192-pixel QSPI chunks and an 80 MHz requested bus clock. The
measured rate on the connected target board is a stable 30.28 FPS with touch
polling and the antialiased overlay, compared with 5 FPS for the initial
single-buffer renderer.

## Performance profiling

Build and upload `waveshare_amoled_143_screenshot`, then run:

```powershell
python tools/profile_performance.py --port COM21
```

The script samples quiet on-demand counters and reports average FPS, globe
render time, display-core overlay time, complete display-task time, and
QSPI-only time. Measurements on this board:

| Configuration | Render | Globe | Overlay | Transfer | FPS |
|---|---:|---:|---:|---:|---:|
| 1024×512 front/back baseline | 49.31 ms | 46.86 ms | 2.45 ms | 22.20 ms | 20.27 |
| + direct RGB565 color table | 46.89 ms | 44.44 ms | 2.45 ms | 22.51 ms | 21.32 |
| + pointer loop, unrolled by two | 43.73 ms | 41.28 ms | 2.45 ms | 23.11 ms | 22.86 |
| + dedicated packed 512×256 back layer | 41.05 ms | 38.60 ms | 2.45 ms | 21.71 ms | 24.35 |
| + targeted `-O3` | 37.99 ms | 35.59 ms | 2.41 ms | 22.13 ms | 26.31 |
| + hot loop in internal executable RAM (v0.4) | 36.91 ms | 34.50 ms | 2.41 ms | 22.14 ms | 27.08 |
| Packed front, back-phase reuse, overlay on display core, 8× unroll | 32.71 ms | 32.71 ms | 2.32 ms | 23.17 ms | 30.56 |
| Restored v0.5 rim + tighter physical-line/fog back layer | 32.77 ms | 32.77 ms | 2.32 ms | 23.16 ms | 30.50 |
| + FT3168 polling and horizontal gestures | **32.75 ms** | **32.75 ms** | **2.29 ms** | **23.18 ms** | **30.28** |

The current visual pass keeps the v0.5 rim and sharp 1024×512 front geography.
Only the back texture and its pale green-cyan color treatment differ.
Idle FT3168 polling adds about 0.24 ms to the complete frame period but does
not change the measured globe render stage.

The 60 MHz request measured identically to 40 MHz, consistent with the ESP32
SPI divider selecting the same effective clock. A 512×256 front texture remains
available as `GLOBE_USE_HALF_TEXTURE=1`, but it is not the release default
because its coastlines are visibly more pixelated. The optimized release only
reduces the intentionally blurred back layer, leaving the detailed front map
unchanged.

Rendering and display work run concurrently on separate cores. The reported
23.18 ms display-task time includes the 2.29 ms overlay phase and the 20.89 ms
QSPI phase. Final FPS remains limited by the 32.75 ms globe render stage plus
the small touch-polling cost rather than by display transfer.

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
python tools/generate_world_map.py --width 1024 --height 512 --supersample 4 `
  --output src/world_texture.h --front-output src/world_front_1024.h `
  --preview world-texture-preview.png
python tools/generate_world_map.py --width 512 --height 256 --supersample 4 `
  --back-style fog --output src/world_texture_512.h `
  --back-output src/world_back_512.h `
  --preview world-texture-512-preview.png
```

The script downloads pinned Natural Earth physical vector layers, filters lakes
and rivers to major features, and bakes detailed/blurred intensity channels at
4× resolution before LANCZOS downsampling. `--back-style legacy` remains
available for direct visual and performance comparisons.
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
