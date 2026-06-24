// Internal implementation fragment: Static background, alpha-font text, analog hands, and overlay composition.
// Included by main.cpp inside its anonymous namespace.

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

void fillRectClipped(uint16_t *target, int16_t x, int16_t y, int16_t width,
                     int16_t height, uint16_t color) {
  const int16_t x0 = max<int16_t>(kFrameScreenX, x);
  const int16_t y0 = max<int16_t>(kFrameScreenY, y);
  const int16_t x1 = min<int16_t>(kFrameScreenX + kFrameWidth, x + width);
  const int16_t y1 = min<int16_t>(kFrameScreenY + kFrameHeight, y + height);
  for (int16_t py = y0; py < y1; ++py) {
    uint16_t *row =
        target +
        static_cast<uint32_t>(py - kFrameScreenY) * kFrameWidth;
    for (int16_t px = x0; px < x1; ++px) {
      row[px - kFrameScreenX] = color;
    }
  }
}

void drawText(uint16_t *target, const char *text, int16_t x, int16_t y,
              uint8_t scale, uint16_t color) {
  for (const char *cursor = text; *cursor != '\0'; ++cursor) {
    const uint8_t *rows = font5x7::rowsFor(*cursor);
    for (uint8_t row = 0; row < 7; ++row) {
      for (uint8_t column = 0; column < 5; ++column) {
        if ((rows[row] & (0x10U >> column)) != 0) {
          fillRectClipped(target, x + column * scale, y + row * scale, scale,
                          scale, color);
        }
      }
    }
    x += 6 * scale;
  }
}

void drawCenteredText(uint16_t *target, const char *text, int16_t y,
                      uint8_t scale, uint16_t color) {
  const int16_t width = static_cast<int16_t>((strlen(text) * 6 - 1) * scale);
  const int16_t x = (kDisplayWidth - width) / 2;
  drawText(target, text, x + 1, y + 1, scale, frame565(0, 18, 24));
  drawText(target, text, x, y, scale, color);
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

#if GLOBE_OPTIMIZE_OVERLAY
__attribute__((always_inline)) inline void blendOverlayPixel(
    uint16_t *destination, uint16_t styleIndex) {
  if (overlayAlphas[styleIndex] == 255) {
    *destination = overlayColors[styleIndex];
    return;
  }

  const uint16_t background = __builtin_bswap16(*destination);
  const uint16_t red =
      overlayRedBlend[(styleIndex << 5) | (background >> 11)];
  const uint16_t green = overlayGreenBlend[
      (styleIndex << 6) | ((background >> 5) & 0x3FU)];
  const uint16_t blue =
      overlayBlueBlend[(styleIndex << 5) | (background & 0x1FU)];
  *destination =
      __builtin_bswap16(static_cast<uint16_t>((red << 11) | (green << 5) |
                                              blue));
}
#endif

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

uint32_t distanceSquaredToSegment(int16_t px, int16_t py, int16_t x0,
                                  int16_t y0, int16_t x1, int16_t y1) {
  const int32_t vx = x1 - x0;
  const int32_t vy = y1 - y0;
  const int32_t wx = px - x0;
  const int32_t wy = py - y0;
  const int32_t lengthSquared = vx * vx + vy * vy;
  if (lengthSquared == 0) {
    return static_cast<uint32_t>(wx * wx + wy * wy);
  }

  const int32_t projection = wx * vx + wy * vy;
  if (projection <= 0) {
    return static_cast<uint32_t>(wx * wx + wy * wy);
  }
  if (projection >= lengthSquared) {
    const int32_t dx = px - x1;
    const int32_t dy = py - y1;
    return static_cast<uint32_t>(dx * dx + dy * dy);
  }

  const int64_t cross =
      static_cast<int64_t>(wx) * vy - static_cast<int64_t>(wy) * vx;
  return static_cast<uint32_t>((cross * cross) / lengthSquared);
}

void appendSoftSegment(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                       uint8_t coreRadius, uint8_t glowRadius,
                       uint16_t coreStyleBase, uint8_t coreAlpha,
                       uint16_t glowStyleBase, uint8_t glowAlpha) {
  const int16_t minX = min(x0, x1) - glowRadius;
  const int16_t maxX = max(x0, x1) + glowRadius;
  const int16_t minY = min(y0, y1) - glowRadius;
  const int16_t maxY = max(y0, y1) + glowRadius;
  const uint32_t coreSquared =
      static_cast<uint32_t>(coreRadius) * coreRadius;
  const uint32_t glowSquared =
      static_cast<uint32_t>(glowRadius) * glowRadius;
  const uint32_t fadeSpan = max<uint32_t>(1, glowSquared - coreSquared);

  for (int16_t y = minY; y <= maxY; ++y) {
    for (int16_t x = minX; x <= maxX; ++x) {
      const uint32_t distanceSquared =
          distanceSquaredToSegment(x, y, x0, y0, x1, y1);
      if (distanceSquared > glowSquared) {
        continue;
      }
      if (distanceSquared <= coreSquared) {
        appendOverlayPixel(x, y, coreStyleBase + coreAlpha);
        continue;
      }
      const uint8_t glowRange = glowAlpha > 1 ? glowAlpha - 1 : 1;
      const uint8_t alpha = static_cast<uint8_t>(
          1U + ((glowSquared - distanceSquared) *
                glowRange) /
                   fadeSpan);
      appendOverlayPixel(x, y, glowStyleBase + alpha);
    }
  }
}

int16_t clockPolarX(int16_t centerX, uint16_t degree, int16_t radius) {
  return centerX +
         static_cast<int32_t>(clockSinQ14[degree % 360]) * radius / 16384;
}

int16_t clockPolarY(int16_t centerY, uint16_t degree, int16_t radius) {
  return centerY -
         static_cast<int32_t>(clockCosQ14[degree % 360]) * radius / 16384;
}

void appendAnalogFace(int16_t centerX, int16_t centerY, int16_t radius,
                      const struct tm &local, bool compact) {
  for (uint8_t minute = 0; minute < 60; ++minute) {
    const bool major = (minute % 5) == 0;
    const uint16_t degree = minute * 6U;
    const int16_t innerRadius =
        radius - (major ? (compact ? 7 : 13) : (compact ? 3 : 6));
    const int16_t outerRadius = radius - 1;
    appendSoftSegment(
        clockPolarX(centerX, degree, innerRadius),
        clockPolarY(centerY, degree, innerRadius),
        clockPolarX(centerX, degree, outerRadius),
        clockPolarY(centerY, degree, outerRadius), major ? 1 : 0,
        major ? (compact ? 2 : 3) : (compact ? 1 : 2),
        kAnalogCyanStyleBase, major ? 8 : 4, kAnalogCyanStyleBase,
        major ? 4 : 2);
  }

  const uint16_t secondDegree = local.tm_sec * 6U;
  const uint16_t minuteDegree =
      (local.tm_min * 6U + local.tm_sec / 10U) % 360U;
  const uint16_t hourDegree =
      ((local.tm_hour % 12) * 30U + local.tm_min / 2U) % 360U;
  const int16_t hourLength = compact ? radius * 48 / 100 : radius * 53 / 100;
  const int16_t minuteLength =
      compact ? radius * 70 / 100 : radius * 75 / 100;
  const int16_t secondLength =
      compact ? radius * 84 / 100 : radius * 86 / 100;
  const int16_t tailLength = compact ? 4 : 13;

  appendSoftSegment(
      clockPolarX(centerX, hourDegree, -tailLength),
      clockPolarY(centerY, hourDegree, -tailLength),
      clockPolarX(centerX, hourDegree, hourLength),
      clockPolarY(centerY, hourDegree, hourLength), compact ? 1 : 3,
      compact ? 3 : 7, kAnalogWhiteStyleBase, 10, kAnalogCyanStyleBase, 4);
  appendSoftSegment(
      clockPolarX(centerX, minuteDegree, -tailLength),
      clockPolarY(centerY, minuteDegree, -tailLength),
      clockPolarX(centerX, minuteDegree, minuteLength),
      clockPolarY(centerY, minuteDegree, minuteLength), compact ? 1 : 2,
      compact ? 3 : 6, kAnalogWhiteStyleBase, 11, kAnalogCyanStyleBase, 4);
  appendSoftSegment(
      clockPolarX(centerX, secondDegree, -(compact ? 5 : 20)),
      clockPolarY(centerY, secondDegree, -(compact ? 5 : 20)),
      clockPolarX(centerX, secondDegree, secondLength),
      clockPolarY(centerY, secondDegree, secondLength), 0,
      compact ? 2 : 3, kAnalogCyanStyleBase, 12, kAnalogCyanStyleBase, 5);
  appendSoftSegment(centerX, centerY, centerX, centerY, compact ? 2 : 4,
                    compact ? 4 : 7, kAnalogWhiteStyleBase, 11,
                    kAnalogCyanStyleBase, 4);
}

void buildOverlayPixels(const char *weekday, const char *timeText,
                        const char *date, const struct tm &local) {
  // Font decoding and transparent-pixel rejection happen only when overlay
  // text changes instead of walking packed glyph bitmaps every globe frame.
  overlayPixelCount = 0;
  overlayOverflow = false;
  switch (clockMode) {
    case ClockMode::Digital:
      appendCenteredAlphaText(ui_fonts::kWeekday, weekday, 159,
                              kWeekdayStyleBase);
      appendGlowingTime(
          timeText,
          (kDisplayWidth - alphaTextWidth(ui_fonts::kTime, timeText)) / 2,
          177);
      appendCenteredAlphaText(ui_fonts::kDate, date, 296, kDateStyleBase);
      break;
    case ClockMode::Analog:
      appendAnalogFace(kGlobeCenterX, kGlobeCenterY, 164, local, false);
      break;
    case ClockMode::Hybrid:
      appendAnalogFace(kGlobeCenterX, 105, 43, local, true);
      appendGlowingTime(
          timeText,
          (kDisplayWidth - alphaTextWidth(ui_fonts::kTime, timeText)) / 2,
          177);
      appendCenteredAlphaText(ui_fonts::kDate, date, 296, kDateStyleBase);
      break;
    case ClockMode::GlobeOnly:
    case ClockMode::Count:
      break;
  }
}

bool refreshClockOverlay(bool force) {
  if (overlayPixels == nullptr || overlayMutex == nullptr) {
    return false;
  }

  struct tm local = {};
  bool validTime = systemTimeValid;
  if (validTime) {
    time_t now;
    time(&now);
    validTime =
        now >= kMinimumValidEpoch && localtime_r(&now, &local) != nullptr;
  }

  static const char *const weekdays[] = {
      "SUNDAY",   "MONDAY", "TUESDAY", "WEDNESDAY",
      "THURSDAY", "FRIDAY", "SATURDAY",
  };
  static const char *const months[] = {
      "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
      "JUL", "AUG", "SEP", "OCT", "NOV", "DEC",
  };
  char weekday[10];
  char timeText[6];
  char date[12];
  if (validTime) {
    snprintf(weekday, sizeof(weekday), "%s", weekdays[local.tm_wday]);
    snprintf(timeText, sizeof(timeText), "%02d:%02d", local.tm_hour,
             local.tm_min);
    snprintf(date, sizeof(date), "%02d %s %04d", local.tm_mday,
             months[local.tm_mon], local.tm_year + 1900);
  } else {
    snprintf(weekday, sizeof(weekday), "%s", displayedWeekday);
    snprintf(timeText, sizeof(timeText), "%s", displayedTime);
    snprintf(date, sizeof(date), "%s", displayedDate);
    local.tm_hour = 10;
    local.tm_min = 43;
    local.tm_sec = 0;
  }

  const bool secondSensitive =
      clockMode == ClockMode::Analog || clockMode == ClockMode::Hybrid;
  if (!force && !clockOverlayDirty &&
      clockMode == displayedClockMode &&
      (!secondSensitive || local.tm_sec == displayedSecond) &&
      strcmp(weekday, displayedWeekday) == 0 &&
      strcmp(timeText, displayedTime) == 0 &&
      strcmp(date, displayedDate) == 0) {
    return true;
  }

  xSemaphoreTake(overlayMutex, portMAX_DELAY);
  buildOverlayPixels(weekday, timeText, date, local);
  snprintf(displayedWeekday, sizeof(displayedWeekday), "%s", weekday);
  snprintf(displayedTime, sizeof(displayedTime), "%s", timeText);
  snprintf(displayedDate, sizeof(displayedDate), "%s", date);
  displayedSecond = local.tm_sec;
  displayedClockMode = clockMode;
  clockOverlayDirty = false;
  xSemaphoreGive(overlayMutex);
  return !overlayOverflow;
}

GLOBE_RENDER_OPT void drawOverlay(uint16_t *target) {
  xSemaphoreTake(overlayMutex, portMAX_DELAY);
  for (uint16_t index = 0; index < overlayPixelCount; ++index) {
    const uint32_t packed = overlayPixels[index];
    const uint16_t styleIndex = packed >> 18;
#if GLOBE_OPTIMIZE_OVERLAY
    blendOverlayPixel(target + (packed & kFrameOffsetMask), styleIndex);
#else
    blendFramePixel(target + (packed & kFrameOffsetMask),
                    overlayColors[styleIndex], overlayAlphas[styleIndex]);
#endif
  }
  xSemaphoreGive(overlayMutex);

#if GLOBE_ENABLE_SERIAL_STATS
  // Keep FPS visible only in the profiling build.
  fillRectClipped(target, 10, 12, 95, 22, 0);
  char fpsText[12];
  snprintf(fpsText, sizeof(fpsText), "FPS %u", displayedFps);
  drawText(target, fpsText, 15, 18, 2, frame565(55, 205, 220));
#endif
}

