# Experiments, benchmarks, and diagnostic tools

This document preserves the engineering history behind the release
configuration. The main [README](../README.md) describes the final architecture;
this file records alternatives, measurements, and reproducible test commands.

## Measurement method

Build and upload the quiet screenshot environment:

```powershell
pio run -e waveshare_amoled_143_screenshot -t upload --upload-port COM21
```

Then query cumulative on-device counters:

```powershell
python tools/profile_performance.py --port COM21
python tools/profile_performance.py --port COM21 --pitch 60 --phase anchor
python tools/profile_performance.py --port COM21 --pitch 60 --phase preview
python tools/profile_performance.py --port COM21 --pitch 60 --phase exact
```

The profiler reports:

- complete frame-render time;
- globe-only render time;
- display-core overlay time;
- complete display-task time;
- QSPI-only transfer time;
- effective FPS;
- total, free, and largest PSRAM block;
- selected tilted texture layout.

Rendering and display transfer overlap on separate cores. FPS is therefore
limited by the slower stage, not by `render + transfer`.

## Optimization history

Measurements were taken on the connected original Waveshare
ESP32-S3-Touch-AMOLED-1.43.

| Configuration | Render | Globe | Overlay | Transfer | FPS |
|---|---:|---:|---:|---:|---:|
| 1024×512 front/back baseline | 49.31 ms | 46.86 ms | 2.45 ms | 22.20 ms | 20.27 |
| Direct RGB565 color table | 46.89 ms | 44.44 ms | 2.45 ms | 22.51 ms | 21.32 |
| Pointer loop, unrolled by two | 43.73 ms | 41.28 ms | 2.45 ms | 23.11 ms | 22.86 |
| Dedicated packed 512×256 back | 41.05 ms | 38.60 ms | 2.45 ms | 21.71 ms | 24.35 |
| Targeted `-O3` | 37.99 ms | 35.59 ms | 2.41 ms | 22.13 ms | 26.31 |
| Hot loop in IRAM | 36.91 ms | 34.50 ms | 2.41 ms | 22.14 ms | 27.08 |
| Packed front, phase reuse, display-core overlay, 8× unroll | 32.71 ms | 32.71 ms | 2.32 ms | 23.17 ms | 30.56 |
| Current rim/back visual pass | 32.77 ms | 32.77 ms | 2.32 ms | 23.16 ms | 30.50 |
| Touch polling | 32.75 ms | 32.75 ms | 2.29 ms | 23.18 ms | 30.28 |
| RTC/NTP and dynamic clock | 32.74 ms | 32.74 ms | 2.18 ms | 23.12 ms | 30.23 |
| Digital clock | 32.87 ms | 32.87 ms | 2.67 ms | 23.61 ms | 30.13 |
| Full analog clock | 32.64 ms | 32.64 ms | 2.37 ms | 23.13 ms | 29.76 |
| Hybrid clock | 32.82 ms | 32.82 ms | 2.89 ms | 23.67 ms | 29.64 |
| Globe only | **32.12 ms** | **32.12 ms** | **0.01 ms** | **20.84 ms** | **30.82** |

After the source split, the diagnostic build measured:

| Path | Render | Transfer | FPS |
|---|---:|---:|---:|
| Zero-pitch fast path | 33.45 ms | 23.85 ms | 29.53 |
| Exact 60°, tiled full-resolution textures | 76.84 ms | 23.46 ms | 12.92 |

The refactor retained the same RAM and flash sizes:

- release RAM: 143,164 bytes;
- release flash: 1,939,563 bytes;
- screenshot RAM: 143,396 bytes;
- screenshot flash: 1,948,143 bytes.

## Pitch-renderer evolution

| Pitch renderer | 30° | 60° | 80° |
|---|---:|---:|---:|
| Legacy interpolated dense map | — | 176.82 ms / 5.64 FPS | — |
| Legacy exact dense map | — | — | 141.50 ms / 7.04 FPS |
| Dynamic span exact map | — | 75.27 ms / 13.22 FPS | 87.93 ms / 11.32 FPS |
| Hybrid preview before texture tiling | 58.79 / 16.89 | 95.75 / 10.39 | 110.27 / 9.02 |
| Hybrid exact before texture tiling | 73.25 / 13.56 | 113.26 / 8.79 | 122.62 / 8.09 |

The span representation was compact but introduced construction complexity and
approximation limits. It remains as a disabled reference under
`GLOBE_ENABLE_SPAN_REFERENCE=1`.

## Tilted texture locality

The pitch LUT removes trigonometry from the frame loop, but nonzero pitch makes
neighboring pixels traverse the equirectangular map diagonally. Row-major PSRAM
textures then miss cache frequently, especially near a visible pole.

The benchmark matrix compares 16×4 packed tiles against row-major storage.
Each tile is exactly 32 bytes.

### Preview renderer

Values are render milliseconds / FPS.

| Layout | 30° | 60° | 80° | Texture PSRAM | Free PSRAM |
|---|---:|---:|---:|---:|---:|
| Row front + row full back | 61.84 / 16.04 | 98.45 / 10.09 | 111.38 / 8.93 | 768 KB | 0.86 MB |
| Tiled front + row full back | 58.41 / 16.97 | 80.43 / 12.36 | 94.69 / 10.50 | 768 KB | 0.86 MB |
| Tiled front + tiled full back | **55.35 / 17.90** | 67.18 / 14.78 | 73.40 / 13.53 | 512 KB | 1.12 MB |
| Tiled front + tiled 512×256 back | 57.28 / 17.31 | **63.90 / 15.52** | **69.54 / 14.25** | 320 KB | 1.31 MB |
| Tiled front only | 40.76 / 24.25 | 46.55 / 21.26 | 51.69 / 19.18 | 256 KB | 1.37 MB |

### Exact renderer

| Layout | 30° | 60° | 80° | Texture PSRAM | Free PSRAM |
|---|---:|---:|---:|---:|---:|
| Row front + row full back | 74.51 / 13.33 | 114.33 / 8.70 | 123.50 / 8.06 | 768 KB | 0.86 MB |
| Tiled front + row full back | 68.75 / 14.40 | 92.30 / 10.77 | 105.99 / 9.39 | 768 KB | 0.86 MB |
| Tiled front + tiled full back | **66.19 / 14.99** | 76.92 / 12.90 | 92.68 / 10.73 | 512 KB | 1.12 MB |
| Tiled front + tiled 512×256 back | 66.11 / 15.02 | **73.20 / 13.56** | **80.01 / 12.41** | 320 KB | 1.31 MB |
| Tiled front only | 45.00 / 22.00 | 50.78 / 19.52 | 55.95 / 17.73 | 256 KB | 1.37 MB |

The release uses tiled front plus tiled full-resolution back:

- exact 60° improved by 32.7%;
- texture PSRAM fell from 768 KB to 512 KB;
- free PSRAM rose from 0.86 MB to 1.12 MB;
- settled row-major and tiled full-resolution captures were pixel-identical,
  both CRC32 `41ae23d8`.

The 512×256 rear texture was faster but changed 22.24% of framebuffer pixels
at 60° with a mean absolute channel difference of 1.95/255. It remains available
as `GLOBE_TILTED_BACK_MODE=2`.

Benchmark environments:

```text
bench_tilt_row_full
bench_tilt_front_fullrow
bench_tilt_full
bench_tilt_half
bench_tilt_front_only
```

## Projection build latency

| Pitch | Preview LUT | Exact LUT |
|---:|---:|---:|
| 30° | 476 ms | 1,015 ms |
| 60° | 488–535 ms | 805–875 ms |
| 80° anchor | 92 ms | 780–922 ms |

Rapid pitch requests are coalesced. Exact publication waits while touch is
active, so build time does not block interaction.

## Display-transfer experiments

- Tight 423×423 transfers save bandwidth compared with 466×466 full-screen
  transfers.
- `ESP32QSPI_MAX_PIXELS_AT_ONCE=8192` was the best stable tested chunk size.
- A requested 60 MHz display clock measured the same as 40 MHz, consistent
  with the available SPI divider selecting the same effective clock.
- The release requests 80 MHz and transfers the static exterior halo only once.

## Visual experiments

### Retained

- 4× supersampled Natural Earth rasterization with LANCZOS reduction.
- Packed four-bit front geography.
- Full-resolution tiled rear geography for tilted exact rendering.
- One-pixel sphere coverage antialiasing.
- Four-bit alpha clock glyphs with a soft cyan glow.
- Blurred physical coastlines/lakes/rivers and localized fog on the rear layer.

### Rejected or disabled

- Wide mathematical soft-rim fade: visually too artificial.
- Procedural aurora before true XY rotation: geographic placement was wrong.
- Half-resolution front texture: coastlines became visibly pixelated.
- Dark filled-land rear shadow: did not match the translucent reference.
- Always-on serial statistics: an unconsumed USB CDC buffer eventually stalled
  rendering.

## Screenshot protocol

The release build contains no screenshot or periodic serial code. The
`waveshare_amoled_143_screenshot` environment accepts these commands:

| Command | Action |
|---|---|
| `S` | Return RGB565 framebuffer, dimensions, and CRC32 |
| `Q` | Return cumulative render/overlay/transfer/QSPI counters |
| `T` | Return pitch target, prepared pitch, mode, and build times |
| `H` | Return PSRAM and tilted texture-layout status |
| `0` | Reset and freeze the globe |
| `.` / `,` | Step yaw ±16 texture texels |
| `[` / `]` | Step pitch ±10° |
| `A` | Force direct anchor-blend preview |
| `V` | Force synthesized preview map |
| `X` | Allow exact refinement |
| `1`…`4` | Digital, analog, hybrid, globe-only modes |

Capture one framebuffer:

```powershell
python tools/capture_screenshot.py --port COM21 --output globe.png
```

Capture one exact rotation:

```powershell
python tools/capture_movie.py --port COM21 --pitch 60 --phase exact `
  --frames 64 --step-texels 16 --fps 12 `
  --output globe-60.mp4 --keep-frames globe-60-frames
```

Capture the preview-to-exact transition used in the README:

```powershell
python tools/capture_movie.py --port COM21 --pitch 60 `
  --phase progressive --refine-at-frame 16 `
  --frames 64 --step-texels 16 --fps 12 --with-overlay `
  --output globe-pitch-60-progressive.mp4
```

`--post-ready-settle 0.6` waits until the 300 ms rear fade is complete before
the first exact comparison frame.

## Device presentation media

The screen footage in `docs/media` is actual ESP32 output. The surrounding
round development-board body is an AI-generated presentation mockup, prepared
as a reusable transparent overlay:

```powershell
python tools/prepare_device_mockup.py `
  --input device-mockup-outer-alpha.png `
  --output docs/media/device-mockup-overlay.png

python tools/create_device_video.py `
  --frames globe-frames `
  --device docs/media/device-mockup-overlay.png `
  --fps 12 `
  --output docs/media/demo.mp4 `
  --preview-gif docs/media/demo-preview.gif
```
