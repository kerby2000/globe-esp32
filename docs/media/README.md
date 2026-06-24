# Demo media provenance

The globe content in every video is actual RGB565 framebuffer output captured
from the connected ESP32-S3 with CRC validation. The surrounding development
board is a virtual presentation mockup and is not a product photograph.

## Files

| File | Content |
|---|---|
| `globe-zero-pitch.mp4` | Raw 423×423 ESP32 framebuffer rotation |
| `globe-pitch-60-progressive.mp4` | Raw 60° preview-to-exact rotation |
| `demo-zero-pitch-device.mp4` | Zero-pitch footage composited into the mockup |
| `demo-pitch-refinement-device.mp4` | Progressive pitch footage composited into the mockup |
| `*-preview.gif` | GitHub README previews |
| `device-mockup-overlay.png` | Transparent reusable board/bezel overlay |

## Virtual-device prompt

The overlay was generated with the built-in image-generation tool using this
prompt, then chroma-keyed and given a transparent circular screen opening:

```text
Use case: product-mockup
Asset type: reusable video frame for a GitHub README demo
Primary request: a clean photorealistic product render of a compact round
black smartwatch-like ESP32 AMOLED development module inspired by the
Waveshare ESP32-S3-Touch-AMOLED-1.43 board, viewed perfectly straight-on,
centered, with a circular empty screen opening intended for later video
compositing
Scene/backdrop: perfectly flat solid #00ff00 chroma-key background
Subject: matte-black circular development board with a dark graphite bezel;
the screen is a flat pure black circle with no graphics, reflections, text,
logo, clock, icons, or glow
Composition: square, centered, front-facing, no perspective tilt
Constraints: no hands, cable, stand, wrist strap, branding, watermark, cast
shadow, floor plane, or background gradient
```

The final screen composite is produced by
[`tools/create_device_video.py`](../../tools/create_device_video.py).
