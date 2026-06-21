#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <freertos/queue.h>
#include <math.h>

#include "font5x7.h"
#include "world_texture.h"

#ifndef GLOBE_ENABLE_SERIAL_STATS
#define GLOBE_ENABLE_SERIAL_STATS 0
#endif

#ifndef GLOBE_ENABLE_SCREENSHOT
#define GLOBE_ENABLE_SCREENSHOT 0
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

constexpr uint32_t kUMask = 0x3FFUL;
constexpr uint32_t kSixBitMask = 0x3FUL;

// 1024 texture texels per 18 seconds, represented as Q16.16 texels/ms.
constexpr uint32_t kRotationStepQ16PerMs = 3728;

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

uint16_t globePalette[64 * 16];

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
  for (int shade = 0; shade < 64; ++shade) {
    for (int intensity = 0; intensity < 16; ++intensity) {
      // Low texture intensities create the transparent land haze. High values
      // become cyan-white physical outlines. Shade already contains the sphere
      // lighting and atmospheric rim, so the frame loop is only a table lookup.
      const int highlight = max(0, intensity - 10);
      const uint8_t red =
          clampByte(intensity * 6 + highlight * 11 + shade / 7);
      const uint8_t green =
          clampByte(3 + intensity * 13 + shade / 2);
      const uint8_t blue =
          clampByte(7 + intensity * 16 + shade);
      globePalette[(shade << 4) | intensity] = frame565(red, green, blue);
    }
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
    const float ny = static_cast<float>(dy) / kGlobeRadius;
    const float latitude = asinf(-ny);
    sphereV[localY] = static_cast<uint16_t>(constrain(
        static_cast<int32_t>((0.5f - latitude / kPi) *
                             world_texture::kHeight),
        0, world_texture::kHeight - 1));
    const float rowHalfWidth =
        sqrtf(static_cast<float>(kGlobeRadius * kGlobeRadius - dy * dy));
    sphereLeft[localY] =
        static_cast<uint16_t>(ceilf(kGlobeRadius - rowHalfWidth));
    sphereRight[localY] =
        static_cast<uint16_t>(floorf(kGlobeRadius + rowHalfWidth));

    for (int16_t localX = 0; localX < kLutDiameter; ++localX) {
      const int16_t dx = localX - kGlobeRadius;
      const uint32_t lutIndex =
          static_cast<uint32_t>(localY) * kLutDiameter + localX;
      const float nx = static_cast<float>(dx) / kGlobeRadius;
      const float radiusSquared = nx * nx + ny * ny;

      if (radiusSquared > 1.0f) {
        sphereLut[lutIndex] = 0;
        continue;
      }

      const float nz = sqrtf(1.0f - radiusSquared);
      const float longitude = atan2f(nx, nz);

      int32_t textureU = static_cast<int32_t>(
          (longitude / kTwoPi + 0.5f) * world_texture::kWidth);
      textureU &= world_texture::kWidth - 1;

      // A soft upper-left light and a brighter edge give the transparent globe
      // volume. They are collapsed into one six-bit palette coordinate.
      float lightDot = nx * -0.32f + ny * -0.48f + nz * 0.82f;
      lightDot = constrain(lightDot, 0.0f, 1.0f);
      const float rim = (1.0f - nz) * (1.0f - nz);
      const uint32_t shade = static_cast<uint32_t>(
          constrain(3.0f + lightDot * 22.0f + rim * 38.0f, 0.0f, 63.0f));

      sphereLut[lutIndex] = static_cast<uint16_t>(
          (static_cast<uint32_t>(textureU) & kUMask) |
          ((shade & kSixBitMask) << 10));
    }
  }
}

void fillRectClipped(int16_t x, int16_t y, int16_t width, int16_t height,
                     uint16_t color) {
  const int16_t x0 = max<int16_t>(0, x);
  const int16_t y0 = max<int16_t>(0, y);
  const int16_t x1 = min<int16_t>(kDisplayWidth, x + width);
  const int16_t y1 = min<int16_t>(kDisplayHeight, y + height);
  for (int16_t py = y0; py < y1; ++py) {
    uint16_t *row = frameBuffer + static_cast<uint32_t>(py) * kDisplayWidth;
    for (int16_t px = x0; px < x1; ++px) {
      row[px] = color;
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

void drawOverlay() {
  drawCenteredText("SATURDAY", 164, 3, frame565(205, 255, 250));
  drawCenteredText("10:43", 203, 8, frame565(230, 255, 255));
  drawCenteredText("20 JUN 2026", 278, 2, frame565(145, 215, 220));

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
      static_cast<uint32_t>(kDisplayWidth) * kDisplayHeight * sizeof(uint16_t);
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(frameBuffer);
  const uint32_t checksum = crc32(bytes, byteCount);

  Serial.write(reinterpret_cast<const uint8_t *>("GLB2"), 4);
  writeLittleEndian16(kDisplayWidth);
  writeLittleEndian16(kDisplayHeight);
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

void serviceSerialCommands() {
  while (Serial.available() > 0) {
    const int command = Serial.read();
    if (command == 'S' || command == 's') {
      sendFramebufferScreenshot();
    } else if (command == 'P' || command == 'p') {
      sendPerformanceSnapshot();
    }
  }
}
#endif

void displayTask(void *parameter) {
  uint16_t *buffer = nullptr;
  while (true) {
    if (xQueueReceive(readyFrameQueue, &buffer, portMAX_DELAY) == pdTRUE) {
#if GLOBE_ENABLE_SERIAL_STATS
      const uint32_t transferStartUs = micros();
#endif
      display->draw16bitBeRGBBitmap(0, 0, buffer, kDisplayWidth,
                                    kDisplayHeight);
#if GLOBE_ENABLE_SERIAL_STATS
      lastTransferMicros = micros() - transferStartUs;
#endif
      xQueueSend(freeFrameQueue, &buffer, portMAX_DELAY);
    }
  }
}

void renderFrame(uint16_t rotationTexels) {
  for (int16_t localY = 0; localY < kLutDiameter; ++localY) {
    uint16_t *destination =
        frameBuffer + static_cast<uint32_t>(kLutOriginY + localY) *
                          kDisplayWidth +
        kLutOriginX;
    const uint16_t *lutRow =
        sphereLut + static_cast<uint32_t>(localY) * kLutDiameter;
    const uint16_t v = sphereV[localY];
    const uint8_t *textureRow =
        worldTexture + (static_cast<uint32_t>(v) << 10);

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
          (1536U - baseU + rotationTexels) & (world_texture::kWidth - 1);
      const uint8_t frontPacked = textureRow[frontU];
      const uint8_t backPacked = textureRow[backU];
      const uint8_t frontIntensity = frontPacked >> 4;
      const uint8_t backIntensity = backPacked & 0x0F;
      const uint8_t backContribution =
          static_cast<uint8_t>((backIntensity * 3 + 2) >> 2);
      const uint8_t intensity =
          min<uint8_t>(15, frontIntensity + backContribution);
      const uint8_t shade = sample >> 10;

      destination[localX] = globePalette[(shade << 4) | intensity];
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

  if (!display->begin(40000000)) {
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

  const size_t frameByteCount =
      static_cast<size_t>(kDisplayWidth) * kDisplayHeight * sizeof(uint16_t);
  for (uint8_t index = 0; index < 2; ++index) {
    frameBuffers[index] = static_cast<uint16_t *>(heap_caps_malloc(
        frameByteCount, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  frameBuffer = frameBuffers[0];
  sphereLut = static_cast<uint16_t *>(heap_caps_malloc(
      static_cast<size_t>(kLutDiameter) * kLutDiameter * sizeof(uint16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

  if (frameBuffers[0] == nullptr || frameBuffers[1] == nullptr ||
      sphereLut == nullptr) {
    haltWithMessage("PSRAM ALLOCATION FAILED");
  }
  memset(frameBuffers[0], 0, frameByteCount);
  memset(frameBuffers[1], 0, frameByteCount);

  buildColorPalettes();
  buildSphereLut();

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
                kDisplayWidth * kDisplayHeight * sizeof(uint16_t));
  Serial.printf("Sphere LUT:   %u bytes\n",
                kLutDiameter * kLutDiameter * sizeof(uint16_t));
  Serial.printf("World texture:%u bytes\n", world_texture::kByteCount);
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

#if GLOBE_ENABLE_SERIAL_STATS
  const uint32_t renderStartUs = micros();
#endif
  renderFrame((rotationQ16 >> 16) & (world_texture::kWidth - 1));
#if GLOBE_ENABLE_SERIAL_STATS
  renderMicrosAccumulated += micros() - renderStartUs;
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
