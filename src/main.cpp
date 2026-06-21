#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <freertos/queue.h>
#include <math.h>

#include "font5x7.h"
#include "ui_fonts.h"

#ifndef GLOBE_USE_HALF_TEXTURE
#define GLOBE_USE_HALF_TEXTURE 0
#endif

#if GLOBE_USE_HALF_TEXTURE
#include "world_texture_512.h"
#else
#include "world_texture.h"
#endif

#ifndef GLOBE_ENABLE_SERIAL_STATS
#define GLOBE_ENABLE_SERIAL_STATS 0
#endif

#ifndef GLOBE_ENABLE_SCREENSHOT
#define GLOBE_ENABLE_SCREENSHOT 0
#endif

#ifndef GLOBE_DISPLAY_BUS_HZ
#define GLOBE_DISPLAY_BUS_HZ 40000000
#endif

#ifndef GLOBE_TRANSFER_TIGHT
#define GLOBE_TRANSFER_TIGHT 0
#endif

namespace {

constexpr int16_t kDisplayWidth = 466;
constexpr int16_t kDisplayHeight = 466;

// Original (non-C) Waveshare ESP32-S3-Touch-AMOLED-1.43 pinout.
constexpr int8_t kLcdCs = 9;
constexpr int8_t kLcdClock = 10;
constexpr int8_t kLcdData0 = 11;
constexpr int8_t kLcdData1 = 12;
constexpr int8_t kLcdData2 = 13;
constexpr int8_t kLcdData3 = 14;
constexpr int8_t kLcdReset = 21;
constexpr int8_t kAmoledEnable = 42;
constexpr uint8_t kSh8601Id = 0x86;

constexpr int16_t kGlobeCenterX = kDisplayWidth / 2;
constexpr int16_t kGlobeCenterY = kDisplayHeight / 2;
constexpr int16_t kGlobeRadius = 211;
constexpr int16_t kLutDiameter = kGlobeRadius * 2 + 1;
constexpr int16_t kLutOriginX = kGlobeCenterX - kGlobeRadius;
constexpr int16_t kLutOriginY = kGlobeCenterY - kGlobeRadius;
constexpr int16_t kTransferWidth = kLutDiameter;
constexpr int16_t kTransferHeight = kLutDiameter;
constexpr int16_t kTransferX = kLutOriginX;
constexpr int16_t kTransferY = kLutOriginY;
#if GLOBE_TRANSFER_TIGHT
constexpr int16_t kFrameWidth = kTransferWidth;
constexpr int16_t kFrameHeight = kTransferHeight;
constexpr int16_t kFrameScreenX = kTransferX;
constexpr int16_t kFrameScreenY = kTransferY;
#else
constexpr int16_t kFrameWidth = kDisplayWidth;
constexpr int16_t kFrameHeight = kDisplayHeight;
constexpr int16_t kFrameScreenX = 0;
constexpr int16_t kFrameScreenY = 0;
#endif

constexpr uint32_t kUMask = 0x3FFUL;
constexpr uint32_t kShadeMask = 0x0FUL;
constexpr uint32_t kCoverageMask = 0x03UL;
constexpr uint32_t kFrameOffsetMask = 0x3FFFFUL;
constexpr uint16_t kWeekdayStyleBase = 256;
constexpr uint16_t kDateStyleBase = 272;
constexpr uint16_t kOverlayStyleCount = 288;
constexpr uint16_t kOverlayPixelCapacity = 16000;

// One full rotation per 18 seconds, represented as Q16.16 texels/ms.
constexpr uint32_t kRotationStepQ16PerMs =
    (static_cast<uint32_t>(world_texture::kWidth) << 16) / 18000U;

Arduino_DataBus *displayBus = new Arduino_ESP32QSPI(
    kLcdCs, kLcdClock, kLcdData0, kLcdData1, kLcdData2, kLcdData3);
Arduino_SH8601 sh8601Display(
    displayBus, kLcdReset, 0, kDisplayWidth, kDisplayHeight, 0, 0, 0, 0);
// CO5300 versions of this board expose the 466 active columns at RAM columns
// 6..471. Waveshare's official flush callback applies the same +6 offset.
Arduino_CO5300 co5300Display(
    displayBus, kLcdReset, 0, kDisplayWidth, kDisplayHeight, 6, 0, 6, 0);
Arduino_OLED *display = nullptr;

uint16_t *frameBuffer = nullptr;
uint16_t *frameBuffers[2] = {nullptr, nullptr};
uint16_t *sphereLut = nullptr;
const uint8_t *worldTexture = world_texture::kIntensity;
uint16_t sphereLeft[kLutDiameter];
uint16_t sphereRight[kLutDiameter];
uint16_t sphereV[kLutDiameter];

uint16_t globePalette[4 * 16 * 16];
uint16_t overlayColors[kOverlayStyleCount];
uint8_t overlayAlphas[kOverlayStyleCount];
uint32_t *overlayPixels = nullptr;
uint16_t overlayPixelCount = 0;
bool overlayOverflow = false;

uint32_t rotationQ16 = 0;
uint32_t previousFrameMs = 0;

#if GLOBE_ENABLE_SERIAL_STATS
uint32_t fpsWindowStartMs = 0;
uint16_t fpsWindowFrames = 0;
uint16_t displayedFps = 0;
uint32_t renderMicrosAccumulated = 0;
volatile uint32_t lastTransferMicros = 0;
#endif

#if GLOBE_ENABLE_SCREENSHOT
uint32_t renderedFrameCount = 0;
volatile uint32_t transferredFrameCount = 0;
uint32_t profileRenderMicros = 0;
volatile uint32_t profileTransferMicros = 0;
volatile uint32_t profileQspiMicros = 0;
#endif

QueueHandle_t freeFrameQueue = nullptr;
QueueHandle_t readyFrameQueue = nullptr;

void bitBangWriteByte(uint8_t value) {
  for (uint8_t bit = 0; bit < 8; ++bit) {
    digitalWrite(kLcdData0, (value & 0x80U) ? HIGH : LOW);
    value <<= 1;
    digitalWrite(kLcdClock, LOW);
    digitalWrite(kLcdClock, HIGH);
  }
}

uint8_t detectPanelController() {
  // The original board was manufactured with two pin-compatible controllers.
  // This is Waveshare's pre-SPI-bus ID read, expressed in Arduino GPIO calls.
  const int8_t outputPins[] = {kLcdCs,   kLcdClock, kLcdData0, kLcdData1,
                               kLcdData2, kLcdData3, kLcdReset};
  for (const int8_t pin : outputPins) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
  }
  digitalWrite(kLcdCs, HIGH);
  digitalWrite(kLcdClock, HIGH);

  digitalWrite(kLcdReset, HIGH);
  delay(120);
  digitalWrite(kLcdReset, LOW);
  delay(120);
  digitalWrite(kLcdReset, HIGH);
  delay(120);

  digitalWrite(kLcdCs, LOW);
  bitBangWriteByte(0x03);  // QSPI read opcode
  bitBangWriteByte(0x00);
  bitBangWriteByte(0xDA);  // Read display ID 1
  bitBangWriteByte(0x00);

  pinMode(kLcdData0, INPUT_PULLUP);
  uint8_t panelId = 0;
  for (uint8_t bit = 0; bit < 8; ++bit) {
    digitalWrite(kLcdClock, LOW);
    delayMicroseconds(1);
    panelId = static_cast<uint8_t>((panelId << 1) | digitalRead(kLcdData0));
    digitalWrite(kLcdClock, HIGH);
    delayMicroseconds(1);
  }
  digitalWrite(kLcdCs, HIGH);
  pinMode(kLcdData0, OUTPUT);
  return panelId;
}

uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue) {
  return static_cast<uint16_t>(((red & 0xF8U) << 8) |
                               ((green & 0xFCU) << 3) | (blue >> 3));
}

uint16_t frame565(uint8_t red, uint8_t green, uint8_t blue) {
  // Store high byte first in memory so the QSPI driver can transmit the
  // framebuffer directly without swapping every pixel into a staging buffer.
  return __builtin_bswap16(rgb565(red, green, blue));
}

uint8_t clampByte(int value) {
  if (value < 0) {
    return 0;
  }
  if (value > 255) {
    return 255;
  }
  return static_cast<uint8_t>(value);
}

void buildColorPalettes() {
  for (int coverage = 0; coverage < 4; ++coverage) {
    for (int shade = 0; shade < 16; ++shade) {
      for (int intensity = 0; intensity < 16; ++intensity) {
        // Expand the compact four-bit shade back to the old six-bit range.
        // Coverage is a two-bit area estimate for antialiasing the silhouette.
        const int shade63 = (shade * 63 + 7) / 15;
        const int highlight = max(0, intensity - 10);
        const int red = intensity * 6 + highlight * 11 + shade63 / 7;
        const int green = 3 + intensity * 13 + shade63 / 2;
        const int blue = 7 + intensity * 16 + shade63;
        globePalette[(coverage << 8) | (shade << 4) | intensity] =
            frame565(clampByte(red * coverage / 3),
                     clampByte(green * coverage / 3),
                     clampByte(blue * coverage / 3));
      }
    }
  }

  // Collapse foreground alpha and glow alpha into one source color/alpha pair.
  // This preserves the soft clock halo while requiring only one RGB565 blend
  // per framebuffer pixel.
  constexpr uint8_t kTextRed = 238;
  constexpr uint8_t kTextGreen = 255;
  constexpr uint8_t kTextBlue = 255;
  constexpr uint8_t kGlowRed = 0;
  constexpr uint8_t kGlowGreen = 150;
  constexpr uint8_t kGlowBlue = 178;
  for (uint8_t alpha = 0; alpha < 16; ++alpha) {
    for (uint8_t glow = 0; glow < 16; ++glow) {
      const uint16_t glowWeight = glow * (15 - alpha);
      const uint16_t textWeight = alpha * 15;
      const uint16_t totalWeight = textWeight + glowWeight;
      const uint8_t index = static_cast<uint8_t>((alpha << 4) | glow);
      if (totalWeight == 0) {
        overlayColors[index] = 0;
        overlayAlphas[index] = 0;
        continue;
      }
      const uint8_t red = static_cast<uint8_t>(
          (kTextRed * textWeight + kGlowRed * glowWeight +
           totalWeight / 2) /
          totalWeight);
      const uint8_t green = static_cast<uint8_t>(
          (kTextGreen * textWeight + kGlowGreen * glowWeight +
           totalWeight / 2) /
          totalWeight);
      const uint8_t blue = static_cast<uint8_t>(
          (kTextBlue * textWeight + kGlowBlue * glowWeight +
           totalWeight / 2) /
          totalWeight);
      overlayColors[index] = frame565(red, green, blue);
      overlayAlphas[index] =
          static_cast<uint8_t>((totalWeight * 255 + 112) / 225);
    }
  }
  for (uint8_t alpha = 0; alpha < 16; ++alpha) {
    overlayColors[kWeekdayStyleBase + alpha] = frame565(215, 255, 250);
    overlayAlphas[kWeekdayStyleBase + alpha] = alpha * 17;
    overlayColors[kDateStyleBase + alpha] = frame565(150, 215, 220);
    overlayAlphas[kDateStyleBase + alpha] = alpha * 17;
  }
}

void buildSphereLut() {
  // This runs once during setup. It is the only place where sphere projection
  // uses floating point, sqrt(), asin(), and atan2(). Every frame later only
  // unpacks integer texture coordinates, lighting, and rim values.
  constexpr float kPi = 3.14159265358979323846f;
  constexpr float kTwoPi = kPi * 2.0f;

  for (int16_t localY = 0; localY < kLutDiameter; ++localY) {
    const int16_t dy = localY - kGlobeRadius;
    const float rawNy = static_cast<float>(dy) / kGlobeRadius;
    const float ny = constrain(rawNy, -1.0f, 1.0f);
    const float latitude = asinf(-ny);
    sphereV[localY] = static_cast<uint16_t>(constrain(
        static_cast<int32_t>((0.5f - latitude / kPi) *
                             world_texture::kHeight),
        0, world_texture::kHeight - 1));
    sphereLeft[localY] = kLutDiameter;
    sphereRight[localY] = 0;

    for (int16_t localX = 0; localX < kLutDiameter; ++localX) {
      const int16_t dx = localX - kGlobeRadius;
      const uint32_t lutIndex =
          static_cast<uint32_t>(localY) * kLutDiameter + localX;

      // Four-by-four subpixel coverage is calculated once and packed into two
      // LUT bits. This removes the stair-step circle edge without any runtime
      // distance math or a separate mask fetch.
      uint8_t coveredSamples = 0;
      for (uint8_t sampleY = 0; sampleY < 4; ++sampleY) {
        const float subY = dy + (sampleY + 0.5f) * 0.25f - 0.5f;
        for (uint8_t sampleX = 0; sampleX < 4; ++sampleX) {
          const float subX = dx + (sampleX + 0.5f) * 0.25f - 0.5f;
          if (subX * subX + subY * subY <=
              static_cast<float>(kGlobeRadius * kGlobeRadius)) {
            ++coveredSamples;
          }
        }
      }
      if (coveredSamples == 0) {
        sphereLut[lutIndex] = 0;
        continue;
      }
      sphereLeft[localY] = min<uint16_t>(sphereLeft[localY], localX);
      sphereRight[localY] = max<uint16_t>(sphereRight[localY], localX);
      const uint32_t coverage =
          min<uint32_t>(3, (coveredSamples * 3U + 15U) / 16U);

      float nx = static_cast<float>(dx) / kGlobeRadius;
      float projectedNy = ny;
      float radiusSquared = nx * nx + projectedNy * projectedNy;
      if (radiusSquared > 1.0f) {
        const float normalize = 0.9999f / sqrtf(radiusSquared);
        nx *= normalize;
        projectedNy *= normalize;
        radiusSquared = nx * nx + projectedNy * projectedNy;
      }
      const float nz = sqrtf(max(0.0f, 1.0f - radiusSquared));
      const float longitude = atan2f(nx, nz);

      int32_t textureU = static_cast<int32_t>(
          (longitude / kTwoPi + 0.5f) * world_texture::kWidth);
      textureU &= world_texture::kWidth - 1;

      // A soft upper-left light and a brighter edge give the transparent globe
      // volume. They are collapsed into one six-bit palette coordinate.
      float lightDot =
          nx * -0.32f + projectedNy * -0.48f + nz * 0.82f;
      lightDot = constrain(lightDot, 0.0f, 1.0f);
      const float rim = (1.0f - nz) * (1.0f - nz);
      const float shade63 =
          constrain(3.0f + lightDot * 22.0f + rim * 38.0f, 0.0f, 63.0f);
      const uint32_t shade =
          static_cast<uint32_t>((shade63 * 15.0f + 31.5f) / 63.0f);

      sphereLut[lutIndex] = static_cast<uint16_t>(
          (static_cast<uint32_t>(textureU) & kUMask) |
          ((shade & kShadeMask) << 10) |
          ((coverage & kCoverageMask) << 14));
    }
  }
}

void initializeBackground(uint16_t *buffer, int16_t width, int16_t height,
                          int16_t screenOriginX, int16_t screenOriginY) {
  // The exterior halo is static, so bake it into both reusable framebuffers.
  // The rotating sphere overwrites its own pixels each frame.
  for (int16_t y = 0; y < height; ++y) {
    for (int16_t x = 0; x < width; ++x) {
      const float dx = x + screenOriginX - kGlobeCenterX;
      const float dy = y + screenOriginY - kGlobeCenterY;
      const float exteriorDistance =
          sqrtf(dx * dx + dy * dy) - kGlobeRadius;
      uint16_t color = 0;
      if (exteriorDistance >= 0.0f && exteriorDistance < 10.0f) {
        const float strength =
            expf(-(exteriorDistance * exteriorDistance) / 14.0f);
        color = frame565(0, static_cast<uint8_t>(18.0f * strength),
                         static_cast<uint8_t>(34.0f * strength));
      }
      buffer[static_cast<uint32_t>(y) * width + x] = color;
    }
  }
}

void fillRectClipped(int16_t x, int16_t y, int16_t width, int16_t height,
                     uint16_t color) {
  const int16_t x0 = max<int16_t>(kFrameScreenX, x);
  const int16_t y0 = max<int16_t>(kFrameScreenY, y);
  const int16_t x1 = min<int16_t>(kFrameScreenX + kFrameWidth, x + width);
  const int16_t y1 = min<int16_t>(kFrameScreenY + kFrameHeight, y + height);
  for (int16_t py = y0; py < y1; ++py) {
    uint16_t *row =
        frameBuffer +
        static_cast<uint32_t>(py - kFrameScreenY) * kFrameWidth;
    for (int16_t px = x0; px < x1; ++px) {
      row[px - kFrameScreenX] = color;
    }
  }
}

void drawText(const char *text, int16_t x, int16_t y, uint8_t scale,
              uint16_t color) {
  for (const char *cursor = text; *cursor != '\0'; ++cursor) {
    const uint8_t *rows = font5x7::rowsFor(*cursor);
    for (uint8_t row = 0; row < 7; ++row) {
      for (uint8_t column = 0; column < 5; ++column) {
        if ((rows[row] & (0x10U >> column)) != 0) {
          fillRectClipped(x + column * scale, y + row * scale, scale, scale,
                          color);
        }
      }
    }
    x += 6 * scale;
  }
}

void drawCenteredText(const char *text, int16_t y, uint8_t scale,
                      uint16_t color) {
  const int16_t width = static_cast<int16_t>((strlen(text) * 6 - 1) * scale);
  const int16_t x = (kDisplayWidth - width) / 2;
  drawText(text, x + 1, y + 1, scale, frame565(0, 18, 24));
  drawText(text, x, y, scale, color);
}

uint8_t packedAlpha(const uint8_t *data, uint32_t pixelIndex) {
  const uint8_t packed = pgm_read_byte(data + (pixelIndex >> 1));
  return (pixelIndex & 1U) ? (packed & 0x0FU) : (packed >> 4);
}

bool findGlyph(const ui_fonts::Font &font, char character,
               ui_fonts::Glyph &glyph) {
  for (uint8_t index = 0; index < font.glyphCount; ++index) {
    memcpy_P(&glyph, font.glyphs + index, sizeof(glyph));
    if (glyph.character == character) {
      return true;
    }
  }
  return false;
}

int16_t alphaTextWidth(const ui_fonts::Font &font, const char *text) {
  int16_t width = 0;
  ui_fonts::Glyph glyph;
  for (const char *cursor = text; *cursor != '\0'; ++cursor) {
    if (findGlyph(font, *cursor, glyph)) {
      width += glyph.advance;
    }
  }
  return width;
}

void blendFramePixel(uint16_t *destination, uint16_t sourceFrame,
                     uint8_t alpha) {
  if (alpha == 0) {
    return;
  }
  if (alpha == 255) {
    *destination = sourceFrame;
    return;
  }

  const uint16_t source = __builtin_bswap16(sourceFrame);
  const uint16_t background = __builtin_bswap16(*destination);
  const uint32_t alpha32 = (alpha + 4U) >> 3;
  const uint32_t inverseAlpha32 = 32U - alpha32;
  const uint16_t redBlue = static_cast<uint16_t>(
      (((source & 0xF81FU) * alpha32 +
        (background & 0xF81FU) * inverseAlpha32) >>
       5) &
      0xF81FU);
  const uint16_t green = static_cast<uint16_t>(
      (((source & 0x07E0U) * alpha32 +
        (background & 0x07E0U) * inverseAlpha32) >>
       5) &
      0x07E0U);
  *destination = __builtin_bswap16(redBlue | green);
}

void appendOverlayPixel(int16_t x, int16_t y, uint16_t styleIndex) {
  if (x < kFrameScreenX || x >= kFrameScreenX + kFrameWidth ||
      y < kFrameScreenY || y >= kFrameScreenY + kFrameHeight ||
      overlayAlphas[styleIndex] == 0) {
    return;
  }
  // The glow-only 1/15 fringe is nearly invisible on black but accounts for
  // thousands of blends. Keep glow levels 2..15 and every glyph alpha level.
  if (styleIndex < kWeekdayStyleBase && (styleIndex >> 4) == 0 &&
      (styleIndex & 0x0F) == 1) {
    return;
  }
  if (overlayPixelCount >= kOverlayPixelCapacity) {
    overlayOverflow = true;
    return;
  }
  const uint32_t frameOffset =
      static_cast<uint32_t>(y - kFrameScreenY) * kFrameWidth +
      (x - kFrameScreenX);
  overlayPixels[overlayPixelCount++] =
      frameOffset | (static_cast<uint32_t>(styleIndex) << 18);
}

void appendAlphaText(const ui_fonts::Font &font, const char *text, int16_t x,
                     int16_t lineY, uint16_t styleBase) {
  ui_fonts::Glyph glyph;
  for (const char *cursor = text; *cursor != '\0'; ++cursor) {
    if (!findGlyph(font, *cursor, glyph)) {
      continue;
    }
    const int16_t glyphX = x + glyph.xOffset;
    const int16_t glyphY = lineY + glyph.yOffset;
    for (uint8_t py = 0; py < glyph.height; ++py) {
      const int16_t screenY = glyphY + py;
      if (screenY < 0 || screenY >= kDisplayHeight) {
        continue;
      }
      for (uint8_t px = 0; px < glyph.width; ++px) {
        const int16_t screenX = glyphX + px;
        if (screenX < 0 || screenX >= kDisplayWidth) {
          continue;
        }
        const uint32_t pixelIndex =
            static_cast<uint32_t>(py) * glyph.width + px;
        const uint8_t alpha =
            packedAlpha(font.alpha + glyph.alphaOffset, pixelIndex);
        appendOverlayPixel(screenX, screenY, styleBase + alpha);
      }
    }
    x += glyph.advance;
  }
}

void appendGlowingTime(const char *text, int16_t x, int16_t lineY) {
  const ui_fonts::Font &font = ui_fonts::kTime;
  ui_fonts::Glyph glyph;
  for (const char *cursor = text; *cursor != '\0'; ++cursor) {
    if (!findGlyph(font, *cursor, glyph)) {
      continue;
    }
    const int16_t glyphX = x + glyph.xOffset;
    const int16_t glyphY = lineY + glyph.yOffset;
    for (uint8_t py = 0; py < glyph.height; ++py) {
      const int16_t screenY = glyphY + py;
      if (screenY < 0 || screenY >= kDisplayHeight) {
        continue;
      }
      for (uint8_t px = 0; px < glyph.width; ++px) {
        const int16_t screenX = glyphX + px;
        if (screenX < 0 || screenX >= kDisplayWidth) {
          continue;
        }
        const uint32_t pixelIndex =
            static_cast<uint32_t>(py) * glyph.width + px;
        const uint8_t alpha =
            packedAlpha(font.alpha + glyph.alphaOffset, pixelIndex);
        const uint8_t glow =
            packedAlpha(font.glow + glyph.glowOffset, pixelIndex);
        const uint8_t compositeIndex =
            static_cast<uint8_t>((alpha << 4) | glow);
        appendOverlayPixel(screenX, screenY, compositeIndex);
      }
    }
    x += glyph.advance;
  }
}

void appendCenteredAlphaText(const ui_fonts::Font &font, const char *text,
                             int16_t lineY, uint16_t styleBase) {
  appendAlphaText(font, text,
                  (kDisplayWidth - alphaTextWidth(font, text)) / 2, lineY,
                  styleBase);
}

void buildOverlayPixels() {
  // Font decoding and transparent-pixel rejection happen only when overlay
  // text changes. A real clock can call this once per minute instead of
  // walking packed glyph bitmaps during every globe frame.
  overlayPixelCount = 0;
  overlayOverflow = false;
  appendCenteredAlphaText(ui_fonts::kWeekday, "SATURDAY", 159,
                          kWeekdayStyleBase);
  const char *time = "10:43";
  appendGlowingTime(
      time, (kDisplayWidth - alphaTextWidth(ui_fonts::kTime, time)) / 2, 177);
  appendCenteredAlphaText(ui_fonts::kDate, "20 JUN 2026", 296,
                          kDateStyleBase);
}

void drawOverlay() {
  for (uint16_t index = 0; index < overlayPixelCount; ++index) {
    const uint32_t packed = overlayPixels[index];
    const uint16_t styleIndex = packed >> 18;
    blendFramePixel(frameBuffer + (packed & kFrameOffsetMask),
                    overlayColors[styleIndex], overlayAlphas[styleIndex]);
  }

#if GLOBE_ENABLE_SERIAL_STATS
  // Keep FPS visible only in the profiling build.
  fillRectClipped(10, 12, 95, 22, 0);
  char fpsText[12];
  snprintf(fpsText, sizeof(fpsText), "FPS %u", displayedFps);
  drawText(fpsText, 15, 18, 2, frame565(55, 205, 220));
#endif
}

#if GLOBE_ENABLE_SCREENSHOT
uint32_t crc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFFUL;
  while (length-- > 0) {
    crc ^= *data++;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

void writeLittleEndian16(uint16_t value) {
  Serial.write(static_cast<uint8_t>(value));
  Serial.write(static_cast<uint8_t>(value >> 8));
}

void writeLittleEndian32(uint32_t value) {
  Serial.write(static_cast<uint8_t>(value));
  Serial.write(static_cast<uint8_t>(value >> 8));
  Serial.write(static_cast<uint8_t>(value >> 16));
  Serial.write(static_cast<uint8_t>(value >> 24));
}

void sendFramebufferScreenshot() {
  // Binary protocol:
  // "GLB2", uint16 width, uint16 height, uint32 byte_count, uint32 CRC32,
  // followed by RGB565 bytes in panel order (big endian).
  const uint32_t byteCount =
      static_cast<uint32_t>(kFrameWidth) * kFrameHeight * sizeof(uint16_t);
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(frameBuffer);
  const uint32_t checksum = crc32(bytes, byteCount);

  Serial.write(reinterpret_cast<const uint8_t *>("GLB2"), 4);
  writeLittleEndian16(kFrameWidth);
  writeLittleEndian16(kFrameHeight);
  writeLittleEndian32(byteCount);
  writeLittleEndian32(checksum);
  Serial.write(bytes, byteCount);
  Serial.flush();
}

void sendPerformanceSnapshot() {
  // Compact, on-demand telemetry for stability tests. Unlike serial stats,
  // this emits nothing unless the host sends 'P'.
  Serial.write(reinterpret_cast<const uint8_t *>("GLBP"), 4);
  writeLittleEndian32(millis());
  writeLittleEndian32(renderedFrameCount);
  Serial.flush();
}

void sendDetailedPerformanceSnapshot() {
  // "GLBQ", uptime, rendered frames, transferred frames, cumulative render
  // microseconds, cumulative transfer-task microseconds, cumulative QSPI-only
  // microseconds. The quiet screenshot build emits this only on a 'Q' request.
  Serial.write(reinterpret_cast<const uint8_t *>("GLBQ"), 4);
  writeLittleEndian32(millis());
  writeLittleEndian32(renderedFrameCount);
  writeLittleEndian32(transferredFrameCount);
  writeLittleEndian32(profileRenderMicros);
  writeLittleEndian32(profileTransferMicros);
  writeLittleEndian32(profileQspiMicros);
  Serial.flush();
}

void serviceSerialCommands() {
  while (Serial.available() > 0) {
    const int command = Serial.read();
    if (command == 'S' || command == 's') {
      sendFramebufferScreenshot();
    } else if (command == 'P' || command == 'p') {
      sendPerformanceSnapshot();
    } else if (command == 'Q' || command == 'q') {
      sendDetailedPerformanceSnapshot();
    }
  }
}
#endif

void displayTask(void *parameter) {
  uint16_t *buffer = nullptr;
  while (true) {
    if (xQueueReceive(readyFrameQueue, &buffer, portMAX_DELAY) == pdTRUE) {
#if GLOBE_ENABLE_SERIAL_STATS || GLOBE_ENABLE_SCREENSHOT
      const uint32_t transferStartUs = micros();
#endif
#if GLOBE_TRANSFER_TIGHT
#if GLOBE_ENABLE_SCREENSHOT
      const uint32_t qspiStartUs = micros();
#endif
      display->draw16bitBeRGBBitmap(kTransferX, kTransferY, buffer,
                                    kTransferWidth, kTransferHeight);
#if GLOBE_ENABLE_SCREENSHOT
      profileQspiMicros += micros() - qspiStartUs;
#endif
#else
#if GLOBE_ENABLE_SCREENSHOT
      const uint32_t qspiStartUs = micros();
#endif
      display->draw16bitBeRGBBitmap(0, 0, buffer, kDisplayWidth,
                                    kDisplayHeight);
#if GLOBE_ENABLE_SCREENSHOT
      profileQspiMicros += micros() - qspiStartUs;
#endif
#endif
#if GLOBE_ENABLE_SERIAL_STATS
      lastTransferMicros = micros() - transferStartUs;
#endif
#if GLOBE_ENABLE_SCREENSHOT
      profileTransferMicros += micros() - transferStartUs;
      transferredFrameCount = transferredFrameCount + 1;
#endif
      xQueueSend(freeFrameQueue, &buffer, portMAX_DELAY);
    }
  }
}

void renderFrame(uint16_t rotationTexels) {
  for (int16_t localY = 0; localY < kLutDiameter; ++localY) {
    uint16_t *destination = frameBuffer +
                            static_cast<uint32_t>(
                                kLutOriginY + localY - kFrameScreenY) *
                                kFrameWidth +
                            (kLutOriginX - kFrameScreenX);
    const uint16_t *lutRow =
        sphereLut + static_cast<uint32_t>(localY) * kLutDiameter;
    const uint16_t v = sphereV[localY];
    const uint8_t *textureRow =
        worldTexture + static_cast<uint32_t>(v) * world_texture::kWidth;

    for (uint16_t localX = sphereLeft[localY];
         localX <= sphereRight[localY]; ++localX) {
      const uint16_t sample = lutRow[localX];
      const uint16_t baseU = sample & kUMask;
      const uint16_t frontU =
          (baseU + rotationTexels) & (world_texture::kWidth - 1);
      // The far intersection of the same viewing ray is the opposite
      // longitude. Sampling its blurred channel makes the back of the
      // transparent globe visible beneath the front surface.
      const uint16_t backU =
          (world_texture::kWidth + world_texture::kWidth / 2U - baseU +
           rotationTexels) &
          (world_texture::kWidth - 1);
      const uint8_t frontPacked = textureRow[frontU];
      const uint8_t backPacked = textureRow[backU];
      const uint8_t frontIntensity = frontPacked >> 4;
      const uint8_t backIntensity = backPacked & 0x0F;
      const uint8_t backContribution =
          static_cast<uint8_t>((backIntensity * 3 + 2) >> 2);
      const uint8_t intensity =
          min<uint8_t>(15, frontIntensity + backContribution);
      const uint8_t shade = (sample >> 10) & kShadeMask;
      const uint8_t coverage = sample >> 14;

      destination[localX] =
          globePalette[(coverage << 8) | (shade << 4) | intensity];
    }
  }

  drawOverlay();
}

[[noreturn]] void haltWithMessage(const char *message) {
#if GLOBE_ENABLE_SERIAL_STATS
  Serial.println(message);
#endif
  display->fillScreen(rgb565(45, 0, 0));
  display->setTextColor(rgb565(255, 210, 210));
  display->setTextSize(2);
  display->setCursor(16, kDisplayHeight / 2 - 10);
  display->print(message);
  while (true) {
    delay(1000);
  }
}

}  // namespace

void setup() {
#if GLOBE_ENABLE_SERIAL_STATS || GLOBE_ENABLE_SCREENSHOT
  Serial.begin(115200);
  delay(100);
#endif

#if GLOBE_ENABLE_SERIAL_STATS
  Serial.println("\nESP32-S3 AMOLED rotating globe");
#endif

  pinMode(kAmoledEnable, OUTPUT);
  digitalWrite(kAmoledEnable, HIGH);
  delay(20);

  const uint8_t panelId = detectPanelController();
  if (panelId == kSh8601Id) {
    display = &sh8601Display;
#if GLOBE_ENABLE_SERIAL_STATS
    Serial.printf("AMOLED controller: SH8601 (ID 0x%02X)\n", panelId);
#endif
  } else {
    display = &co5300Display;
#if GLOBE_ENABLE_SERIAL_STATS
    Serial.printf("AMOLED controller: CO5300-compatible (ID 0x%02X)\n",
                  panelId);
#endif
  }

  if (!display->begin(GLOBE_DISPLAY_BUS_HZ)) {
#if GLOBE_ENABLE_SERIAL_STATS
    Serial.println("Display initialization failed");
#endif
    while (true) {
      delay(1000);
    }
  }
  display->setBrightness(210);
  display->fillScreen(0);

  if (!psramFound()) {
    haltWithMessage("PSRAM NOT FOUND");
  }

#if GLOBE_TRANSFER_TIGHT
  // The halo extends beyond the moving 423x423 region. Transfer it once as a
  // full-screen static background, then release this temporary buffer.
  const size_t staticBackgroundByteCount =
      static_cast<size_t>(kDisplayWidth) * kDisplayHeight * sizeof(uint16_t);
  uint16_t *staticBackground = static_cast<uint16_t *>(heap_caps_malloc(
      staticBackgroundByteCount, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (staticBackground == nullptr) {
    haltWithMessage("STATIC BACKGROUND FAILED");
  }
  initializeBackground(staticBackground, kDisplayWidth, kDisplayHeight, 0, 0);
  display->draw16bitBeRGBBitmap(0, 0, staticBackground, kDisplayWidth,
                                kDisplayHeight);
  heap_caps_free(staticBackground);
#endif

  const size_t frameByteCount =
      static_cast<size_t>(kFrameWidth) * kFrameHeight * sizeof(uint16_t);
  for (uint8_t index = 0; index < 2; ++index) {
    frameBuffers[index] = static_cast<uint16_t *>(heap_caps_malloc(
        frameByteCount, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  frameBuffer = frameBuffers[0];
  sphereLut = static_cast<uint16_t *>(heap_caps_malloc(
      static_cast<size_t>(kLutDiameter) * kLutDiameter * sizeof(uint16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  overlayPixels = static_cast<uint32_t *>(heap_caps_malloc(
      static_cast<size_t>(kOverlayPixelCapacity) * sizeof(uint32_t),
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

  if (frameBuffers[0] == nullptr || frameBuffers[1] == nullptr ||
      sphereLut == nullptr || overlayPixels == nullptr) {
    haltWithMessage("PSRAM ALLOCATION FAILED");
  }
  initializeBackground(frameBuffers[0], kFrameWidth, kFrameHeight,
                       kFrameScreenX, kFrameScreenY);
  initializeBackground(frameBuffers[1], kFrameWidth, kFrameHeight,
                       kFrameScreenX, kFrameScreenY);

  buildColorPalettes();
  buildSphereLut();
  buildOverlayPixels();
  if (overlayOverflow) {
    haltWithMessage("OVERLAY PIXEL CAPACITY");
  }

  freeFrameQueue = xQueueCreate(2, sizeof(uint16_t *));
  readyFrameQueue = xQueueCreate(2, sizeof(uint16_t *));
  if (freeFrameQueue == nullptr || readyFrameQueue == nullptr) {
    haltWithMessage("FRAME QUEUE FAILED");
  }
  xQueueSend(freeFrameQueue, &frameBuffers[0], portMAX_DELAY);
  xQueueSend(freeFrameQueue, &frameBuffers[1], portMAX_DELAY);
  if (xTaskCreatePinnedToCore(displayTask, "globe-display", 4096, nullptr, 2,
                              nullptr, 0) != pdPASS) {
    haltWithMessage("DISPLAY TASK FAILED");
  }

  previousFrameMs = millis();

#if GLOBE_ENABLE_SERIAL_STATS
  Serial.printf("Frame buffer: %u bytes\n",
                kFrameWidth * kFrameHeight * sizeof(uint16_t));
  Serial.printf("Sphere LUT:   %u bytes\n",
                kLutDiameter * kLutDiameter * sizeof(uint16_t));
  Serial.printf("World texture:%u bytes\n", world_texture::kByteCount);
  Serial.printf("Overlay pixels:%u (%u bytes)\n", overlayPixelCount,
                overlayPixelCount * sizeof(uint32_t));
  Serial.printf("Free PSRAM:   %u bytes\n", ESP.getFreePsram());
  fpsWindowStartMs = previousFrameMs;
#endif
}

void loop() {
  uint16_t *renderBuffer = nullptr;
  xQueueReceive(freeFrameQueue, &renderBuffer, portMAX_DELAY);
  frameBuffer = renderBuffer;

  const uint32_t now = millis();
  uint32_t elapsedMs = now - previousFrameMs;
  previousFrameMs = now;
  if (elapsedMs > 100) {
    elapsedMs = 100;
  }
  rotationQ16 += elapsedMs * kRotationStepQ16PerMs;

#if GLOBE_ENABLE_SERIAL_STATS || GLOBE_ENABLE_SCREENSHOT
  const uint32_t renderStartUs = micros();
#endif
  renderFrame((rotationQ16 >> 16) & (world_texture::kWidth - 1));
#if GLOBE_ENABLE_SERIAL_STATS
  renderMicrosAccumulated += micros() - renderStartUs;
#endif
#if GLOBE_ENABLE_SCREENSHOT
  profileRenderMicros += micros() - renderStartUs;
#endif
  xQueueSend(readyFrameQueue, &renderBuffer, portMAX_DELAY);

#if GLOBE_ENABLE_SCREENSHOT
  ++renderedFrameCount;
  // Handle screenshot requests only after a complete frame exists. Opening a
  // USB serial port can reset the ESP32 and queue 'S' before the first loop.
  serviceSerialCommands();
#endif

#if GLOBE_ENABLE_SERIAL_STATS
  ++fpsWindowFrames;
  const uint32_t fpsElapsed = now - fpsWindowStartMs;
  if (fpsElapsed >= 1000) {
    displayedFps =
        static_cast<uint16_t>((fpsWindowFrames * 1000UL) / fpsElapsed);
    const uint32_t averageRenderUs =
        renderMicrosAccumulated / max<uint16_t>(1, fpsWindowFrames);
    fpsWindowFrames = 0;
    fpsWindowStartMs = now;
    renderMicrosAccumulated = 0;
    Serial.printf("FPS: %u, render: %lu.%03lu ms, transfer: %lu.%03lu ms\n",
                  displayedFps, averageRenderUs / 1000,
                  averageRenderUs % 1000, lastTransferMicros / 1000,
                  lastTransferMicros % 1000);
  }
#endif
}
