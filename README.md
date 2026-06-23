# ESP32-S3 real-time rotating globe

Minimal full-screen globe demo for the original Waveshare
ESP32-S3-Touch-AMOLED-1.43 (466×466 QSPI AMOLED).

The visual follows the supplied reference: black background, transparent dark
sphere, cyan/white geographic outlines, a soft atmospheric rim, real clock/date
overlays, and an optional debug FPS counter.

## Touch interaction

The FT3168 touch controller is enabled in all build profiles:

- Touch and hold pauses automatic rotation.
- Drag horizontally to rotate the globe in the same direction as the finger.
  A full-width drag is one complete revolution.
- Drag vertically to tilt the globe between 80° south and 80° north. Adjacent
  projection maps are interpolated, so finger movement remains continuous.
- Release after a drag to continue with inertia; the speed then settles back
  to automatic rotation. Vertical pitch remains where it was released.
- Double-tap resets both longitude and pitch to the startup view.
- A stationary 700 ms long press cycles through digital, analog, hybrid, and
  globe-only display modes.

Touch uses the board's shared 300 kHz I2C bus on GPIO47/48 and adds no
floating-point work to the frame loop. Startup allows 250 ms for the FT3168 to
finish powering up instead of relying on a single early probe. If detection
still fails, rendering continues with automatic rotation while touch recovery
is retried once per second. Touch and RTC transactions share a mutex because
the NTP task can update the PCF85063 while the main core polls the FT3168.

## Real time and RTC

The clock uses the onboard PCF85063 RTC at I2C address `0x51`:

- At boot, valid RTC time is loaded immediately and used before networking is
  available.
- A low-priority background task connects to Wi-Fi, synchronizes from NTP, and
  writes the corrected UTC time back to the PCF85063.
- The RTC stores UTC. Display formatting uses the Brussels CET/CEST rules, so
  daylight-saving transitions do not require rewriting the RTC.
- Clock state is checked once per second. Digital text is rebuilt only when its
  visible minute, weekday, or date changes; analog hands update once per second.
- Wi-Fi is disabled again after synchronization.

Local credentials are kept in the ignored `src/wifi_secrets.h` file. To
configure another checkout:

```powershell
Copy-Item src/wifi_secrets.example.h src/wifi_secrets.h
```

Then edit only `src/wifi_secrets.h`. Never add that file to Git.

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
QSPI-only timings. For visual testing, `M` cycles the clock mode and `1` through
`4` select digital, analog, hybrid, and globe-only modes directly. `[` and `]`
adjust pitch by 10° for screenshot comparisons. `0` resets and freezes the
view, `.` and `,` step longitude by 16 texture texels, and `F` resumes or
freezes automatic rotation. `A` forces the direct anchor-blend projection, `V`
forces the synthesized preview projection, and `X` allows the exact refinement,
which lets the three render paths be benchmarked independently.

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
- Zero pitch keeps the original row-coherent fast path. Nonzero pitch uses the
  progressive dense-LUT system described below; longitude/yaw remains a cheap
  runtime U offset.
- The map generator applies periodic, latitude-aware horizontal prefiltering
  above approximately 55° north/south. Equirectangular texels cover more
  longitude as they approach a pole. The diffuse back layer additionally
  converges toward each latitude row's periodic mean from 65° to 80°. This
  removes the undefined directional component at the pole—the source of the
  rotating radial pinwheel—without changing the sharp front geography or
  adding runtime texture reads.
- A 4×4 setup-time coverage estimate provides one-pixel silhouette
  antialiasing. The experimental wide soft-rim treatment was removed.
- The original subtle exterior cyan halo is baked into both reusable
  framebuffers, adding no recurring render cost.
- The frame loop contains no floating-point projection math. Background LUT
  generation uses lookup-based `asin`/`atan2` approximations and a cached
  per-pixel sphere-Z table.
- Each frame uses integer texture samples and directly indexes a 32 KB
  precomputed RGB565 color table. The hot scanline loop is unrolled by eight,
  compiled with targeted `-O3`, and placed in internal executable RAM.
- The clock, weekday, and date use offline-generated four-bit alpha glyphs.
  The large time includes a baked soft cyan glow. Transparent glyph pixels are
  rejected at setup and the remaining pixels are stored as a compact command
  list, ready to rebuild when a real clock value changes.
- Four clock presentations are available: the original digital overlay, a
  full-size analog face, a compact analog/digital hybrid, and globe-only mode.
  The analog face uses translucent pale-cyan ticks and softly glowing hands.
  Its trigonometric table is generated during setup; hand rasterization is
  integer-only and runs once per second rather than once per globe frame.
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
build uses 8192-pixel QSPI chunks and an 80 MHz requested bus clock.

## Hybrid progressive pitch architecture

Pitch cannot be produced correctly by linearly interpolating latitude/longitude
coordinates from two precomputed views. Longitude wraps at the antimeridian and
is undefined at a visible pole. Interpolating UV values across those
singularities produced the vertical stitching line seen at intermediate pitches
such as 60°, even though directly generated maps such as 80° were clean.

The renderer now separates immediate interaction from final geometric
correctness:

1. Six front-only dense anchors are generated at startup for -80°, -55°, -25°,
   +25°, +55°, and +80°. The existing zero-pitch sphere LUT is the seventh
   implicit anchor and remains the default fast path.
2. A pitch request coalesces any obsolete build and synthesizes an inactive
   three-byte-per-pixel preview map from the two nearest anchors.
3. Longitude uses shortest-path circular interpolation rather than raw U
   interpolation. Around the visible pole, where longitude is undefined, a
   small feathered region uses the accelerated exact projection to prevent the
   former full-height center hairline from dominating the preview.
4. If the requested pitch is not yet represented by the active exact map,
   the frame renderer does not wait for the synthesized map. It directly blends
   the two nearest anchor maps for the current `pitchQ8`, using circular U
   interpolation, so vertical drag gives immediate pitch feedback and does not
   snap back after release. The rear hemisphere is suppressed during this
   transient path to avoid flicker from changing back-layer mappings.
5. The synthesized preview map is still built in the background and selected at
   a frame boundary only when explicitly requested by the screenshot/profiling
   tools. Normal touch interaction skips this intermediate projection, so the
   visible sequence is direct anchor-blend first, then exact correction.
6. After 150 ms of pitch stability, a low-priority task on core 0 builds a
   five-byte-per-pixel exact map with independent front and rear U/V. The
   display transfer task has higher priority and can preempt this work. When
   exact becomes active, the rear hemisphere fades in over 300 ms.
7. Exact generation uses 4097-entry `asin` and first-quadrant `atan` tables plus
   a cached Q15 sphere-Z map. No `asin`, `atan2`, or normal-case square root is
   evaluated per generated pixel.
8. While touch is active, exact publication waits. On release, a completed exact
   map is atomically swapped at a frame boundary without changing yaw.
9. Rapid pitch updates invalidate partial work by generation number, so only
   the newest stable target can be published.

The preview format stores front U10/V9 in three bytes. The exact format stores
front and rear U10/V9 coordinates in five bytes. The hybrid PSRAM allocation is
approximately 3.22 MB for six anchors, 1.07 MB for two preview maps, 0.89 MB for
the exact map, 0.36 MB for cached sphere Z, and 0.79 MB for the packed front and
full-resolution back texture copies.

Anchor interpolation is still an approximation near a visible pole. The
feathered exact patch confines the temporary error to that polar area; the
progressive exact swap removes it completely. The former span builder remains
in `src/main.cpp` as a compile-time reference under
`GLOBE_ENABLE_SPAN_REFERENCE=1`, but is disabled in normal builds.

## Performance profiling

Build and upload `waveshare_amoled_143_screenshot`, then run:

```powershell
python tools/profile_performance.py --port COM21
python tools/profile_performance.py --port COM21 --pitch 60 --phase anchor
python tools/profile_performance.py --port COM21 --pitch 60 --phase preview
python tools/profile_performance.py --port COM21 --pitch 60 --phase exact
```

The script samples quiet on-demand counters and reports average FPS, globe
render time, display-core overlay time, complete display-task time, and
QSPI-only time. `--pitch` freezes the view after the serial connection has
reset the board. `--phase anchor|preview|exact` forces the requested
progressive stage. Measurements on the connected board:

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
| + FT3168 polling and horizontal gestures | 32.75 ms | 32.75 ms | 2.29 ms | 23.18 ms | 30.28 |
| + RTC/NTP and dynamic real-time overlay | 32.74 ms | 32.74 ms | 2.18 ms | 23.12 ms | 30.23 |
| Digital clock mode | 32.87 ms | 32.87 ms | 2.67 ms | 23.61 ms | 30.13 |
| Full analog clock mode | 32.64 ms | 32.64 ms | 2.37 ms | 23.13 ms | 29.76 |
| Hybrid clock mode | 32.82 ms | 32.82 ms | 2.89 ms | 23.67 ms | 29.64 |
| Globe-only mode | **32.12 ms** | **32.12 ms** | **0.01 ms** | **20.84 ms** | **30.82** |
| XY rotation, zero-pitch fast path | **32.91 ms** | **32.91 ms** | **2.18 ms** | **23.12 ms** | **30.02** |
| Legacy interpolated +60° dense maps | 176.82 ms | 176.82 ms | 1.89 ms | 25.14 ms | 5.64 |
| Legacy exact +80° dense map | 141.50 ms | 141.50 ms | 2.03 ms | 27.90 ms | 7.04 |
| Dynamic exact +60° span LUT, 12,241 spans | 75.27 ms | 75.27 ms | 1.79 ms | 22.97 ms | **13.22** |
| Dynamic exact +80° span LUT, 12,886 spans | 87.93 ms | 87.92 ms | 1.92 ms | 22.89 ms | **11.32** |
| Hybrid preview +30° | 58.79 ms | 58.78 ms | 1.93 ms | 23.55 ms | **16.89** |
| Hybrid exact +30° | 73.25 ms | 73.24 ms | 2.05 ms | 23.44 ms | **13.56** |
| Hybrid preview +60° | 95.75 ms | 95.75 ms | 2.00 ms | 23.76 ms | **10.39** |
| Hybrid exact +60° | 113.26 ms | 113.26 ms | 1.82 ms | 23.38 ms | **8.79** |
| Hybrid preview +80° | 110.27 ms | 110.27 ms | 2.10 ms | 24.07 ms | **9.02** |
| Hybrid exact +80° | 122.62 ms | 122.61 ms | 2.03 ms | 23.43 ms | **8.09** |

Measured projection build latency:

| Pitch | Preview LUT | Exact LUT |
|---:|---:|---:|
| 30° | 476 ms | 1,015 ms |
| 60° | 488–535 ms | 805–829 ms |
| 80° anchor | 92 ms | 780 ms |

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
display-task time includes overlay composition and QSPI transfer. Zero pitch
remains render-bound at roughly 33 ms. Tilted dense rendering is limited by
non-row-coherent PSRAM texture access; the progressive design prioritizes
sub-second interaction and exact seam-free correction over final tilted FPS.

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

## Capturing a rotation movie

The screenshot build also supports deterministic movie capture. The tool
freezes automatic rotation, selects either the preview or exact projection,
steps longitude between frames, validates every framebuffer CRC, and encodes
the PNG sequence with FFmpeg:

```powershell
python tools/capture_movie.py --port COM21 --pitch 80 --phase exact `
  --frames 16 --step-texels 64 --fps 8 `
  --output polar-80.mp4 --keep-frames polar-80-frames
```

Sixteen frames stepped by 64 texels, or 64 frames stepped by 16 texels, cover
one full 1024-texel revolution. Capture is slower than real time because every
423×423 RGB565 framebuffer is transferred over USB, but playback timing is
independent of capture speed. This records the actual ESP32 renderer and is
therefore more useful for LUT/texture bugs than a separate web approximation.

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
