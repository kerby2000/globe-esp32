// Internal implementation fragment: Color tables, sphere projection, pitch anchors, and progressive exact-LUT building.
// Included by main.cpp inside its anonymous namespace.

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
#if GLOBE_USE_DIRECT_COLOR_TABLE
  for (int coverage = 0; coverage < 4; ++coverage) {
    for (int shade = 0; shade < 16; ++shade) {
      const int shade63 = (shade * 63 + 7) / 15;
      const uint16_t surfaceIndex =
          static_cast<uint16_t>((coverage << 4) | shade);
      for (int frontIntensity = 0; frontIntensity < 16; ++frontIntensity) {
        for (int backIntensity = 0; backIntensity < 16; ++backIntensity) {
          // Keep the detailed front geography neutral-white while adding the
          // far hemisphere as a softer cyan emission. Treating both channels
          // as one intensity made back fog look like dark solid terrain.
          const int highlight = max(0, frontIntensity - 10);
          const int red = frontIntensity * 6 + highlight * 11 +
                          backIntensity * 4 + shade63 / 7;
          const int green =
              3 + frontIntensity * 13 + backIntensity * 10 + shade63 / 2;
          const int blue =
              7 + frontIntensity * 16 + backIntensity * 9 + shade63;
          const uint16_t textureIndex =
              static_cast<uint16_t>((frontIntensity << 4) | backIntensity);
          globeColorTable[(surfaceIndex << 8) | textureIndex] =
              frame565(clampByte(red * coverage / 3),
                       clampByte(green * coverage / 3),
                       clampByte(blue * coverage / 3));
        }
      }
    }
  }
#else
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
#endif

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
    overlayColors[kAnalogCyanStyleBase + alpha] = frame565(65, 225, 245);
    overlayAlphas[kAnalogCyanStyleBase + alpha] = alpha * 17;
    overlayColors[kAnalogWhiteStyleBase + alpha] = frame565(220, 255, 252);
    overlayAlphas[kAnalogWhiteStyleBase + alpha] = alpha * 17;
  }

#if GLOBE_OPTIMIZE_OVERLAY
  // The overlay is fixed for many frames, so move its RGB565 alpha
  // multiplications to setup. Runtime blending becomes three internal-RAM
  // byte lookups plus bit packing, with exactly the same 5-bit alpha math.
  for (uint16_t style = 0; style < kOverlayStyleCount; ++style) {
    const uint16_t source = __builtin_bswap16(overlayColors[style]);
    const uint8_t alpha32 = (overlayAlphas[style] + 4U) >> 3;
    const uint8_t inverseAlpha32 = 32U - alpha32;
    const uint8_t sourceRed = source >> 11;
    const uint8_t sourceGreen = (source >> 5) & 0x3FU;
    const uint8_t sourceBlue = source & 0x1FU;
    for (uint8_t background = 0; background < 32; ++background) {
      overlayRedBlend[(style << 5) | background] =
          (sourceRed * alpha32 + background * inverseAlpha32) >> 5;
      overlayBlueBlend[(style << 5) | background] =
          (sourceBlue * alpha32 + background * inverseAlpha32) >> 5;
    }
    for (uint8_t background = 0; background < 64; ++background) {
      overlayGreenBlend[(style << 6) | background] =
          (sourceGreen * alpha32 + background * inverseAlpha32) >> 5;
    }
  }
#endif
}

void buildClockTrigLut() {
  constexpr float kRadiansPerDegree =
      3.14159265358979323846f / 180.0f;
  for (uint16_t degree = 0; degree < 360; ++degree) {
    const float angle = degree * kRadiansPerDegree;
    clockSinQ14[degree] =
        static_cast<int16_t>(roundf(sinf(angle) * 16384.0f));
    clockCosQ14[degree] =
        static_cast<int16_t>(roundf(cosf(angle) * 16384.0f));
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
                             globe_texture::kHeight),
        0, globe_texture::kHeight - 1));
    sphereLeft[localY] = kLutDiameter;
    sphereRight[localY] = 0;

    for (int16_t localX = 0; localX < kLutDiameter; ++localX) {
      const int16_t dx = localX - kGlobeRadius;
      const uint32_t lutIndex =
          static_cast<uint32_t>(localY) * kLutDiameter + localX;

      // Four-by-four subpixel coverage is calculated once and packed into two
      // LUT bits. This removes circle stair-steps without a broad rim fade.
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
#if GLOBE_ENABLE_XY_ROTATION
        sphereNzQ15[lutIndex] = 0;
#endif
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
#if GLOBE_ENABLE_XY_ROTATION
      sphereNzQ15[lutIndex] = static_cast<uint16_t>(
          lroundf(nz * 32767.0f));
#endif
      const float longitude = atan2f(nx, nz);

      int32_t textureU = static_cast<int32_t>(
          (longitude / kTwoPi + 0.5f) * globe_texture::kWidth);
      textureU &= globe_texture::kWidth - 1;

      // A soft upper-left light and brighter edge give the transparent globe
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

#if GLOBE_ENABLE_XY_ROTATION && GLOBE_ENABLE_SPAN_REFERENCE
struct PitchBuilderCoordinate {
  uint16_t frontUQ6;
  uint16_t frontVQ6;
  uint16_t backUQ6;
  uint16_t backVQ6;
};

__attribute__((always_inline)) inline uint32_t absoluteDifference(
    int32_t first, int32_t second) {
  const int32_t difference = first - second;
  return static_cast<uint32_t>(difference < 0 ? -difference : difference);
}

int16_t roundedSpanStep(int32_t difference, uint16_t denominator) {
  if (denominator == 0) {
    return 0;
  }
  const int32_t rounding = denominator / 2;
  const int32_t step =
      difference >= 0
          ? (difference + rounding) / denominator
          : (difference - rounding) / denominator;
  return static_cast<int16_t>(
      constrain(step, static_cast<int32_t>(INT16_MIN),
                static_cast<int32_t>(INT16_MAX)));
}

bool pitchBuildIsCurrent(uint32_t generation) {
  return pitchRequestGeneration == generation;
}

void calculatePitchCoordinate(int16_t localX, int16_t localY,
                              float pitchSin, float pitchCos,
                              PitchBuilderCoordinate &coordinate) {
  constexpr float kPi = 3.14159265358979323846f;
  constexpr float kTwoPi = kPi * 2.0f;
  const int16_t dx = localX - kGlobeRadius;
  const int16_t dy = localY - kGlobeRadius;
  float nx = static_cast<float>(dx) / kGlobeRadius;
  float projectedNy = constrain(
      static_cast<float>(dy) / kGlobeRadius, -1.0f, 1.0f);
  float radiusSquared = nx * nx + projectedNy * projectedNy;
  if (radiusSquared > 1.0f) {
    const float normalize = 0.9999f / sqrtf(radiusSquared);
    nx *= normalize;
    projectedNy *= normalize;
    radiusSquared = nx * nx + projectedNy * projectedNy;
  }
  const float nz = sqrtf(max(0.0f, 1.0f - radiusSquared));
  const float worldY = -projectedNy;

  const float frontY =
      constrain(worldY * pitchCos + nz * pitchSin, -1.0f, 1.0f);
  const float frontZ = -worldY * pitchSin + nz * pitchCos;
  const float backY =
      constrain(worldY * pitchCos - nz * pitchSin, -1.0f, 1.0f);
  const float backZ = -worldY * pitchSin - nz * pitchCos;
  const float frontLongitude = atan2f(nx, frontZ);
  const float frontLatitude = asinf(frontY);
  const float backLongitude = atan2f(nx, backZ);
  const float backLatitude = asinf(backY);

  const int32_t frontUQ6 = lroundf(
      (frontLongitude / kTwoPi + 0.5f) *
      globe_texture::kWidth * kPitchCoordinateOne);
  const int32_t frontVQ6 = lroundf(
      (0.5f - frontLatitude / kPi) *
      globe_texture::kHeight * kPitchCoordinateOne);
  const int32_t backUQ6 = lroundf(
      (backLongitude / kTwoPi + 0.5f) *
      world_back_texture::kWidth * kPitchCoordinateOne);
  const int32_t backVQ6 = lroundf(
      (0.5f - backLatitude / kPi) *
      world_back_texture::kHeight * kPitchCoordinateOne);

  coordinate.frontUQ6 = static_cast<uint16_t>(
      frontUQ6 & ((globe_texture::kWidth * kPitchCoordinateOne) - 1));
  coordinate.frontVQ6 = static_cast<uint16_t>(constrain(
      frontVQ6, 0,
      globe_texture::kHeight * kPitchCoordinateOne - 1));
  coordinate.backUQ6 = static_cast<uint16_t>(
      backUQ6 &
      ((world_back_texture::kWidth * kPitchCoordinateOne) - 1));
  coordinate.backVQ6 = static_cast<uint16_t>(constrain(
      backVQ6, 0,
      world_back_texture::kHeight * kPitchCoordinateOne - 1));
}

uint32_t pitchCoordinateLineError(
    const PitchBuilderCoordinate &start,
    const PitchBuilderCoordinate &end,
    const PitchBuilderCoordinate &sample, uint16_t numerator,
    uint16_t denominator) {
  uint32_t maximumError = 0;
#define UPDATE_PITCH_ERROR(field)                                           \
  do {                                                                      \
    const int32_t predicted =                                               \
        static_cast<int32_t>(start.field) +                                 \
        ((static_cast<int32_t>(end.field) - start.field) * numerator +      \
         denominator / 2) /                                                 \
            denominator;                                                    \
    maximumError = max<uint32_t>(                                           \
        maximumError, absoluteDifference(predicted, sample.field));         \
  } while (false)
  UPDATE_PITCH_ERROR(frontUQ6);
  UPDATE_PITCH_ERROR(frontVQ6);
  UPDATE_PITCH_ERROR(backUQ6);
  UPDATE_PITCH_ERROR(backVQ6);
#undef UPDATE_PITCH_ERROR
  return maximumError;
}

bool appendPitchSpan(PitchSpanLut &lut,
                     const PitchBuilderCoordinate &start,
                     const PitchBuilderCoordinate &end,
                     uint16_t interpolationDistance,
                     uint16_t pixelCount) {
  if (lut.spanCount >= kPitchSpanCapacity || pixelCount == 0) {
    return false;
  }
  PitchSpan &span = lut.spans[lut.spanCount++];
  span.frontUQ6 = start.frontUQ6;
  span.frontVQ6 = start.frontVQ6;
  span.backUQ6 = start.backUQ6;
  span.backVQ6 = start.backVQ6;
  span.frontUStepQ6 = roundedSpanStep(
      static_cast<int32_t>(end.frontUQ6) - start.frontUQ6,
      interpolationDistance);
  span.frontVStepQ6 = roundedSpanStep(
      static_cast<int32_t>(end.frontVQ6) - start.frontVQ6,
      interpolationDistance);
  span.backUStepQ6 = roundedSpanStep(
      static_cast<int32_t>(end.backUQ6) - start.backUQ6,
      interpolationDistance);
  span.backVStepQ6 = roundedSpanStep(
      static_cast<int32_t>(end.backVQ6) - start.backVQ6,
      interpolationDistance);
  span.pixelCount = pixelCount;
  return true;
}

bool pitchSpanWithinError(PitchBuilderCoordinate *coordinates,
                          uint16_t start, uint16_t end) {
  if (end <= start + 1) {
    return true;
  }
  const uint16_t denominator = end - start;
  for (uint16_t index = start + 1; index < end; ++index) {
    if (pitchCoordinateLineError(
            coordinates[start], coordinates[end], coordinates[index],
            index - start, denominator) > kPitchSpanErrorQ6) {
      return false;
    }
  }
  return true;
}

bool appendAdaptivePitchRun(PitchSpanLut &lut,
                            PitchBuilderCoordinate *coordinates,
                            uint16_t runStart, uint16_t runEnd) {
  if (runStart == runEnd) {
    return appendPitchSpan(lut, coordinates[runStart],
                           coordinates[runStart], 0, 1);
  }

  uint16_t segmentStart = runStart;
  while (segmentStart < runEnd) {
    uint16_t segmentEnd = min<uint16_t>(
        runEnd, segmentStart + kMaximumPitchSpanPixels);
    while (segmentEnd > segmentStart + 1 &&
           !pitchSpanWithinError(coordinates, segmentStart, segmentEnd)) {
      segmentEnd =
          segmentStart + max<uint16_t>(1, (segmentEnd - segmentStart) / 2);
    }
    const bool finalSegment = segmentEnd == runEnd;
    const uint16_t distance = segmentEnd - segmentStart;
    const uint16_t pixelCount =
        distance + static_cast<uint16_t>(finalSegment);
    if (!appendPitchSpan(lut, coordinates[segmentStart],
                         coordinates[segmentEnd], distance, pixelCount)) {
      return false;
    }
    segmentStart = segmentEnd;
  }
  return true;
}

bool buildPitchSpanLut(PitchSpanLut &lut, int16_t targetPitchQ8,
                       uint32_t generation) {
  constexpr float kPi = 3.14159265358979323846f;
  const uint32_t buildStartUs = micros();
  const float pitchRadians =
      (static_cast<float>(targetPitchQ8) / 256.0f) * kPi / 180.0f;
  const float pitchSin = sinf(pitchRadians);
  const float pitchCos = cosf(pitchRadians);
  PitchBuilderCoordinate coordinates[kLutDiameter];
  lut.spanCount = 0;
  lut.pitchQ8 = targetPitchQ8;

  constexpr uint32_t kFrontHalfPeriodQ6 =
      globe_texture::kWidth * kPitchCoordinateOne / 2;
  constexpr uint32_t kBackHalfPeriodQ6 =
      world_back_texture::kWidth * kPitchCoordinateOne / 2;

  for (uint16_t localY = 0; localY < kLutDiameter; ++localY) {
    if (!pitchBuildIsCurrent(generation)) {
      return false;
    }
    PitchSpanRow &row = lut.rows[localY];
    row.firstSpan = lut.spanCount;
    row.spanCount = 0;
    row.reserved = 0;
    const uint16_t left = sphereLeft[localY];
    const uint16_t right = sphereRight[localY];
    if (left > right) {
      continue;
    }
    const uint16_t pixelCount = right - left + 1;
    for (uint16_t pixel = 0; pixel < pixelCount; ++pixel) {
      calculatePitchCoordinate(left + pixel, localY, pitchSin,
                               pitchCos, coordinates[pixel]);
    }

    uint16_t runStart = 0;
    for (uint16_t pixel = 1; pixel <= pixelCount; ++pixel) {
      bool endRun = pixel == pixelCount;
      if (!endRun) {
        endRun =
            absoluteDifference(coordinates[pixel].frontUQ6,
                               coordinates[pixel - 1].frontUQ6) >
                kFrontHalfPeriodQ6 ||
            absoluteDifference(coordinates[pixel].backUQ6,
                               coordinates[pixel - 1].backUQ6) >
                kBackHalfPeriodQ6;
      }
      if (!endRun) {
        continue;
      }
      if (!appendAdaptivePitchRun(lut, coordinates, runStart,
                                  pixel - 1)) {
        return false;
      }
      runStart = pixel;
    }
    const uint16_t rowSpanCount = lut.spanCount - row.firstSpan;
    if (rowSpanCount > kMaximumPitchSpansPerRow) {
      return false;
    }
    row.spanCount = static_cast<uint8_t>(rowSpanCount);
    if ((localY & 3U) == 3U) {
      // Block briefly instead of only yielding to equal-priority tasks. This
      // lets IDLE0 run and service the core watchdog while display transfer,
      // which has higher priority, remains responsive.
      vTaskDelay(1);
    }
  }
  lut.buildMicros = micros() - buildStartUs;
  return pitchBuildIsCurrent(generation);
}

void spanReferencePitchBuilderTask(void *parameter) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (true) {
      int16_t targetPitchQ8;
      uint32_t generation;
      int8_t activeIndex;
      int8_t readyIndex;
      portENTER_CRITICAL(&pitchLutMux);
      targetPitchQ8 = requestedPitchQ8;
      generation = pitchRequestGeneration;
      activeIndex = activePitchLutIndex;
      readyIndex = readyPitchLutIndex;
      portEXIT_CRITICAL(&pitchLutMux);

      if (targetPitchQ8 == 0) {
        break;
      }
      if (readyIndex >= 0) {
        vTaskDelay(1);
        continue;
      }
      const int8_t buildIndex = activeIndex == 0 ? 1 : 0;
      PitchSpanLut &buildLut = pitchSpanLuts[buildIndex];
      if (!buildPitchSpanLut(buildLut, targetPitchQ8, generation)) {
        if (!pitchBuildIsCurrent(generation)) {
          continue;
        }
#if GLOBE_ENABLE_SERIAL_STATS
        Serial.printf("Pitch LUT build failed at %u spans\n",
                      buildLut.spanCount);
#endif
        break;
      }

      portENTER_CRITICAL(&pitchLutMux);
      if (pitchRequestGeneration == generation &&
          requestedPitchQ8 == targetPitchQ8 &&
          readyPitchLutIndex < 0) {
        readyPitchLutIndex = buildIndex;
      }
      portEXIT_CRITICAL(&pitchLutMux);
#if GLOBE_ENABLE_SERIAL_STATS
      Serial.printf("Pitch LUT: %.2f deg, %u spans, %lu.%03lu ms\n",
                    targetPitchQ8 / 256.0f, buildLut.spanCount,
                    buildLut.buildMicros / 1000,
                    buildLut.buildMicros % 1000);
#endif
      break;
    }
  }
}

void activateReadyPitchSpanLutReference() {
  int8_t readyIndex = -1;
  portENTER_CRITICAL(&pitchLutMux);
  if (readyPitchLutIndex >= 0) {
    readyIndex = readyPitchLutIndex;
    readyPitchLutIndex = -1;
    if (requestedPitchQ8 != 0 &&
        pitchSpanLuts[readyIndex].pitchQ8 == requestedPitchQ8) {
      activePitchLutIndex = readyIndex;
    } else {
      readyIndex = -1;
    }
  }
  portEXIT_CRITICAL(&pitchLutMux);
  if (readyIndex >= 0) {
    renderedPitchQ8 = pitchSpanLuts[readyIndex].pitchQ8;
  }
}
#endif

#if GLOBE_ENABLE_XY_ROTATION
struct DensePitchCoordinates {
  uint16_t frontU;
  uint16_t frontV;
  uint16_t backU;
  uint16_t backV;
};

void writePreviewPitchCoordinate(uint8_t *destination, uint16_t frontU,
                                 uint16_t frontV) {
  destination[0] = static_cast<uint8_t>(frontU);
  destination[1] = static_cast<uint8_t>(
      ((frontU >> 8) & 0x03U) | ((frontV & 0x3FU) << 2));
  destination[2] = static_cast<uint8_t>((frontV >> 6) & 0x07U);
}

__attribute__((always_inline)) inline void readPreviewPitchCoordinate(
    const uint8_t *__restrict__ source, uint16_t &frontU,
    uint16_t &frontV) {
  frontU = static_cast<uint16_t>(
      source[0] | ((source[1] & 0x03U) << 8));
  frontV = static_cast<uint16_t>(
      (source[1] >> 2) | ((source[2] & 0x07U) << 6));
}

void writeExactPitchCoordinate(uint8_t *destination,
                               const DensePitchCoordinates &coordinate) {
  destination[0] = static_cast<uint8_t>(coordinate.frontU);
  destination[1] = static_cast<uint8_t>(
      ((coordinate.frontU >> 8) & 0x03U) |
      ((coordinate.frontV & 0x3FU) << 2));
  destination[2] = static_cast<uint8_t>(
      ((coordinate.frontV >> 6) & 0x07U) |
      ((coordinate.backU & 0x1FU) << 3));
  destination[3] = static_cast<uint8_t>(
      ((coordinate.backU >> 5) & 0x1FU) |
      ((coordinate.backV & 0x07U) << 5));
  destination[4] =
      static_cast<uint8_t>((coordinate.backV >> 3) & 0x3FU);
}

__attribute__((always_inline)) inline DensePitchCoordinates
readExactPitchCoordinate(const uint8_t *__restrict__ source) {
  DensePitchCoordinates coordinate;
  coordinate.frontU = static_cast<uint16_t>(
      source[0] | ((source[1] & 0x03U) << 8));
  coordinate.frontV = static_cast<uint16_t>(
      (source[1] >> 2) | ((source[2] & 0x07U) << 6));
  coordinate.backU = static_cast<uint16_t>(
      (source[2] >> 3) | ((source[3] & 0x1FU) << 5));
  coordinate.backV = static_cast<uint16_t>(
      (source[3] >> 5) | ((source[4] & 0x3FU) << 3));
  return coordinate;
}

void tilePacked4Texture(uint8_t *__restrict__ destination,
                        const uint8_t *__restrict__ source,
                        uint16_t width, uint16_t height) {
  const uint16_t sourceRowBytes = width / 2;
  uint8_t *output = destination;
  for (uint16_t tileY = 0;
       tileY < height / kPitchTextureTileHeight; ++tileY) {
    for (uint16_t tileX = 0;
         tileX < width / kPitchTextureTileWidth; ++tileX) {
      for (uint8_t localY = 0; localY < kPitchTextureTileHeight;
           ++localY) {
        const uint8_t *sourceBytes =
            source +
            static_cast<uint32_t>(
                tileY * kPitchTextureTileHeight + localY) *
                sourceRowBytes +
            tileX * (kPitchTextureTileWidth / 2);
        memcpy(output, sourceBytes, kPitchTextureTileWidth / 2);
        output += kPitchTextureTileWidth / 2;
      }
    }
  }
}

void tileFullBackTexture(uint8_t *__restrict__ destination) {
  uint8_t *output = destination;
  for (uint16_t tileY = 0;
       tileY < world_texture::kHeight / kPitchTextureTileHeight;
       ++tileY) {
    for (uint16_t tileX = 0;
         tileX < world_texture::kWidth / kPitchTextureTileWidth;
         ++tileX) {
      for (uint8_t localY = 0; localY < kPitchTextureTileHeight;
           ++localY) {
        const uint8_t *source =
            world_texture::kIntensity +
            static_cast<uint32_t>(
                tileY * kPitchTextureTileHeight + localY) *
                world_texture::kWidth +
            tileX * kPitchTextureTileWidth;
        for (uint8_t localX = 0; localX < kPitchTextureTileWidth;
             localX += 2) {
          *output++ = static_cast<uint8_t>(
              ((source[localX] & 0x0FU) << 4) |
              (source[localX + 1] & 0x0FU));
        }
      }
    }
  }
}

void preparePitchTextures() {
#if GLOBE_TILTED_FRONT_TILED
  tilePacked4Texture(pitchFrontTexture, globe_texture::kIntensity,
                     globe_texture::kWidth, globe_texture::kHeight);
#else
  memcpy(pitchFrontTexture, globe_texture::kIntensity,
         kPitchFrontTextureByteCount);
#endif

#if GLOBE_TILTED_BACK_MODE == 0
  memcpy(pitchBackTextureHigh, world_texture::kIntensity,
         kPitchBackTextureByteCount);
#elif GLOBE_TILTED_BACK_MODE == 1
  tileFullBackTexture(pitchBackTextureHigh);
#elif GLOBE_TILTED_BACK_MODE == 2
  tilePacked4Texture(pitchBackTextureHigh,
                     world_back_texture::kIntensity,
                     world_back_texture::kWidth,
                     world_back_texture::kHeight);
#endif
}

void buildProjectionTrigLuts() {
  constexpr float kPi = 3.14159265358979323846f;
  for (uint16_t index = 0; index < kProjectionTrigLutSize; ++index) {
    const float unit =
        static_cast<float>(index) / (kProjectionTrigLutSize - 1);
    const float y = unit * 2.0f - 1.0f;
    projectionAsinVLut[index] = static_cast<uint16_t>(constrain(
        static_cast<int32_t>(
            lroundf((0.5f - asinf(y) / kPi) *
                    globe_texture::kHeight)),
        0, globe_texture::kHeight - 1));
    projectionAtanULut[index] = static_cast<uint16_t>(lroundf(
        atanf(unit) * globe_texture::kWidth / (2.0f * kPi)));
  }
}

__attribute__((always_inline)) inline uint16_t fastLatitudeV(float y) {
  const int32_t index = constrain(
      static_cast<int32_t>(lroundf((y + 1.0f) *
                                   (kProjectionTrigLutSize - 1) * 0.5f)),
      0, kProjectionTrigLutSize - 1);
  return projectionAsinVLut[index];
}

__attribute__((always_inline)) inline uint16_t fastLongitudeU(float x,
                                                              float z) {
  const float absoluteX = fabsf(x);
  const float absoluteZ = fabsf(z);
  uint16_t firstQuadrantU;
  if (absoluteX == 0.0f && absoluteZ == 0.0f) {
    firstQuadrantU = 0;
  } else if (absoluteZ >= absoluteX) {
    const uint16_t ratioIndex = static_cast<uint16_t>(constrain(
        static_cast<int32_t>(
            lroundf(absoluteX / max(absoluteZ, 1.0e-9f) *
                    (kProjectionTrigLutSize - 1))),
        0, kProjectionTrigLutSize - 1));
    firstQuadrantU = projectionAtanULut[ratioIndex];
  } else {
    const uint16_t ratioIndex = static_cast<uint16_t>(constrain(
        static_cast<int32_t>(
            lroundf(absoluteZ / max(absoluteX, 1.0e-9f) *
                    (kProjectionTrigLutSize - 1))),
        0, kProjectionTrigLutSize - 1));
    firstQuadrantU =
        globe_texture::kWidth / 4U - projectionAtanULut[ratioIndex];
  }

  int32_t signedU;
  if (z >= 0.0f) {
    signedU = x >= 0.0f ? firstQuadrantU : -firstQuadrantU;
  } else {
    signedU =
        x >= 0.0f
            ? static_cast<int32_t>(globe_texture::kWidth / 2U) -
                  firstQuadrantU
            : -static_cast<int32_t>(globe_texture::kWidth / 2U) +
                  firstQuadrantU;
  }
  return static_cast<uint16_t>(
      signedU + globe_texture::kWidth / 2U) &
         kPitchFrontUMask;
}

void calculateDensePitchCoordinate(int16_t localX, int16_t localY,
                                   uint32_t lutIndex,
                                   float pitchSin, float pitchCos,
                                   DensePitchCoordinates &coordinate) {
  const int16_t dx = localX - kGlobeRadius;
  const int16_t dy = localY - kGlobeRadius;
  constexpr float kInverseGlobeRadius = 1.0f / kGlobeRadius;
  float nx = static_cast<float>(dx) * kInverseGlobeRadius;
  float projectedNy = constrain(
      static_cast<float>(dy) * kInverseGlobeRadius, -1.0f, 1.0f);
  float radiusSquared = nx * nx + projectedNy * projectedNy;
  float nz;
  if (radiusSquared > 1.0f) {
    const float normalize = 0.9999f / sqrtf(radiusSquared);
    nx *= normalize;
    projectedNy *= normalize;
    radiusSquared = nx * nx + projectedNy * projectedNy;
    nz = sqrtf(max(0.0f, 1.0f - radiusSquared));
  } else {
    nz = sphereNzQ15[lutIndex] * (1.0f / 32767.0f);
  }
  const float worldY = -projectedNy;
  const float frontY =
      constrain(worldY * pitchCos + nz * pitchSin, -1.0f, 1.0f);
  const float frontZ = -worldY * pitchSin + nz * pitchCos;
  const float backY =
      constrain(worldY * pitchCos - nz * pitchSin, -1.0f, 1.0f);
  const float backZ = -worldY * pitchSin - nz * pitchCos;
  coordinate.frontU = fastLongitudeU(nx, frontZ);
  coordinate.frontV = fastLatitudeV(frontY);
  coordinate.backU = fastLongitudeU(nx, backZ);
  coordinate.backV = fastLatitudeV(backY);
}

void readPitchAnchorCoordinate(uint8_t anchorIndex, uint32_t lutIndex,
                               int16_t localY, uint16_t &frontU,
                               uint16_t &frontV) {
  if (anchorIndex == kZeroPitchAnchorIndex) {
    frontU = sphereLut[lutIndex] & kUMask;
    frontV = sphereV[localY];
    return;
  }
  readPreviewPitchCoordinate(
      pitchAnchorMaps[anchorIndex] +
          lutIndex * kPitchPreviewBytesPerPixel,
      frontU, frontV);
}

void buildPitchAnchorBank() {
  constexpr float kPi = 3.14159265358979323846f;
  for (uint8_t anchorIndex = 0; anchorIndex < kPitchAnchorCount;
       ++anchorIndex) {
    if (anchorIndex == kZeroPitchAnchorIndex) {
      continue;
    }
    const float radians =
        kPitchAnchorDegrees[anchorIndex] * kPi / 180.0f;
    const float pitchSin = sinf(radians);
    const float pitchCos = cosf(radians);
    for (uint16_t localY = 0; localY < kLutDiameter; ++localY) {
      const uint16_t left = sphereLeft[localY];
      const uint16_t right = sphereRight[localY];
      if (left > right) {
        continue;
      }
      for (uint16_t localX = left; localX <= right; ++localX) {
        const uint32_t lutIndex =
            static_cast<uint32_t>(localY) * kLutDiameter + localX;
        DensePitchCoordinates coordinate;
        calculateDensePitchCoordinate(localX, localY, lutIndex, pitchSin,
                                      pitchCos, coordinate);
        writePreviewPitchCoordinate(
            pitchAnchorMaps[anchorIndex] +
                lutIndex * kPitchPreviewBytesPerPixel,
            coordinate.frontU, coordinate.frontV);
      }
    }
  }
}

void selectPitchAnchors(int16_t targetPitchQ8, uint8_t &lowerIndex,
                        uint8_t &upperIndex, uint8_t &blendQ8) {
  lowerIndex = kPitchAnchorCount - 1;
  for (uint8_t index = 0; index + 1 < kPitchAnchorCount; ++index) {
    if (targetPitchQ8 < kPitchAnchorDegrees[index + 1] * 256) {
      lowerIndex = index;
      break;
    }
  }
  if (lowerIndex + 1 >= kPitchAnchorCount) {
    upperIndex = lowerIndex;
    blendQ8 = 0;
    return;
  }
  upperIndex = lowerIndex + 1;
  const int32_t lowerQ8 = kPitchAnchorDegrees[lowerIndex] * 256;
  const int32_t upperQ8 = kPitchAnchorDegrees[upperIndex] * 256;
  blendQ8 = static_cast<uint8_t>(
      (static_cast<int32_t>(targetPitchQ8) - lowerQ8) * 256 /
      (upperQ8 - lowerQ8));
}

bool densePitchRequestIsCurrent(uint32_t generation) {
  return pitchRequestGeneration == generation;
}

bool buildPitchPreviewMap(uint8_t *destination, int16_t targetPitchQ8,
                          uint32_t generation) {
  const uint32_t buildStartUs = micros();
  uint8_t lowerIndex;
  uint8_t upperIndex;
  uint8_t blendQ8;
  selectPitchAnchors(targetPitchQ8, lowerIndex, upperIndex, blendQ8);
  if (lowerIndex == upperIndex || blendQ8 == 0) {
    if (lowerIndex == kZeroPitchAnchorIndex) {
      // Zero pitch is routed to the original fast renderer and never needs a
      // synthesized dense map.
      return false;
    }
    memcpy(destination, pitchAnchorMaps[lowerIndex],
           static_cast<size_t>(kLutDiameter) * kLutDiameter *
               kPitchPreviewBytesPerPixel);
    pitchPreviewBuildMicros = micros() - buildStartUs;
    return densePitchRequestIsCurrent(generation);
  }
  constexpr float kPi = 3.14159265358979323846f;
  const float pitchRadians =
      (static_cast<float>(targetPitchQ8) / 256.0f) * kPi / 180.0f;
  const float pitchSin = sinf(pitchRadians);
  const float pitchCos = cosf(pitchRadians);
  const int16_t visiblePoleLocalY = static_cast<int16_t>(lroundf(
      kGlobeRadius -
      copysignf(pitchCos * kGlobeRadius,
                static_cast<float>(targetPitchQ8))));
  constexpr int16_t kPreviewPoleRepairRadius = 64;
  constexpr int32_t kPreviewPoleRepairRadiusSquared =
      kPreviewPoleRepairRadius * kPreviewPoleRepairRadius;
  for (uint16_t localY = 0; localY < kLutDiameter; ++localY) {
    if (!densePitchRequestIsCurrent(generation)) {
      return false;
    }
    const uint16_t left = sphereLeft[localY];
    const uint16_t right = sphereRight[localY];
    if (left > right) {
      continue;
    }
    for (uint16_t localX = left; localX <= right; ++localX) {
      const uint32_t lutIndex =
          static_cast<uint32_t>(localY) * kLutDiameter + localX;
      uint16_t lowerU;
      uint16_t lowerV;
      uint16_t upperU;
      uint16_t upperV;
      readPitchAnchorCoordinate(lowerIndex, lutIndex, localY, lowerU,
                                lowerV);
      readPitchAnchorCoordinate(upperIndex, lutIndex, localY, upperU,
                                upperV);
      const int32_t wrappedDeltaU =
          ((upperU - lowerU + globe_texture::kWidth / 2) &
           (globe_texture::kWidth - 1)) -
          globe_texture::kWidth / 2;
      uint16_t frontU = static_cast<uint16_t>(
          lowerU + ((wrappedDeltaU * blendQ8 + 128) >> 8)) &
          kPitchFrontUMask;
      uint16_t frontV = static_cast<uint16_t>(
          lowerV +
          (((static_cast<int32_t>(upperV) - lowerV) * blendQ8 + 128) >>
           8));
      const int16_t poleDx = localX - kGlobeRadius;
      const int16_t poleDy = localY - visiblePoleLocalY;
      const int32_t poleDistanceSquared =
          static_cast<int32_t>(poleDx) * poleDx +
          static_cast<int32_t>(poleDy) * poleDy;
      if (poleDistanceSquared <= kPreviewPoleRepairRadiusSquared) {
        // Longitude is singular at a visible pole. UV interpolation cannot
        // represent that topology even when wrapping is circular. Blend an
        // accelerated exact projection into the pole core and taper it to the
        // ordinary preview at the patch boundary.
        DensePitchCoordinates exact;
        calculateDensePitchCoordinate(localX, localY, lutIndex, pitchSin,
                                      pitchCos, exact);
        constexpr int32_t kFullRepairRadiusSquared = 28 * 28;
        const uint16_t repairQ8 = static_cast<uint16_t>(constrain(
            (kPreviewPoleRepairRadiusSquared - poleDistanceSquared) *
                256 /
                (kPreviewPoleRepairRadiusSquared -
                 kFullRepairRadiusSquared),
            0, 256));
        const int32_t exactDeltaU =
            ((exact.frontU - frontU + globe_texture::kWidth / 2) &
             (globe_texture::kWidth - 1)) -
            globe_texture::kWidth / 2;
        frontU = static_cast<uint16_t>(
            frontU + ((exactDeltaU * repairQ8 + 128) >> 8)) &
            kPitchFrontUMask;
        frontV = static_cast<uint16_t>(
            frontV +
            (((static_cast<int32_t>(exact.frontV) - frontV) *
                  repairQ8 +
              128) >>
             8));
      }
      writePreviewPitchCoordinate(
          destination + lutIndex * kPitchPreviewBytesPerPixel, frontU,
          frontV);
    }
    if ((localY & 15U) == 15U) {
      vTaskDelay(1);
    }
  }
  pitchPreviewBuildMicros = micros() - buildStartUs;
  return densePitchRequestIsCurrent(generation);
}

bool buildPitchExactMap(uint8_t *destination, int16_t targetPitchQ8,
                        uint32_t generation) {
  constexpr float kPi = 3.14159265358979323846f;
  const uint32_t buildStartUs = micros();
  const float radians =
      (static_cast<float>(targetPitchQ8) / 256.0f) * kPi / 180.0f;
  const float pitchSin = sinf(radians);
  const float pitchCos = cosf(radians);
  for (uint16_t localY = 0; localY < kLutDiameter; ++localY) {
    if (!densePitchRequestIsCurrent(generation)) {
      return false;
    }
    const uint16_t left = sphereLeft[localY];
    const uint16_t right = sphereRight[localY];
    if (left > right) {
      continue;
    }
    for (uint16_t localX = left; localX <= right; ++localX) {
      const uint32_t lutIndex =
          static_cast<uint32_t>(localY) * kLutDiameter + localX;
      DensePitchCoordinates coordinate;
      calculateDensePitchCoordinate(localX, localY, lutIndex, pitchSin,
                                    pitchCos, coordinate);
      writeExactPitchCoordinate(
          destination + lutIndex * kPitchExactBytesPerPixel, coordinate);
    }
    if ((localY & 7U) == 7U) {
      vTaskDelay(1);
    }
  }
  pitchExactBuildMicros = micros() - buildStartUs;
  return densePitchRequestIsCurrent(generation);
}

void publishPitchSwap(PitchDenseMode mode, int8_t previewIndex,
                      int16_t pitchValueQ8, uint32_t generation) {
  portENTER_CRITICAL(&pitchLutMux);
  if (generation == pitchRequestGeneration &&
      pitchValueQ8 == requestedPitchQ8) {
    readyPitchMode = mode;
    readyPreviewMapIndex = previewIndex;
    readyPitchQ8 = pitchValueQ8;
    readyPitchGeneration = generation;
    pitchSwapPending = true;
  }
  portEXIT_CRITICAL(&pitchLutMux);
}

void pitchBuilderTask(void *parameter) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (true) {
      int16_t targetPitchQ8;
      uint32_t generation;
      int8_t activePreviewIndex;
      portENTER_CRITICAL(&pitchLutMux);
      targetPitchQ8 = requestedPitchQ8;
      generation = pitchRequestGeneration;
      activePreviewIndex = activePreviewMapIndex;
      portEXIT_CRITICAL(&pitchLutMux);
      if (targetPitchQ8 == 0) {
        break;
      }

#if GLOBE_ENABLE_SCREENSHOT
      const bool forceSynthesizedPreview =
          screenshotForcePitchPreview &&
          !screenshotForcePitchAnchorBlend;
#else
      constexpr bool forceSynthesizedPreview = false;
#endif

      // A buffer published for the current target must be consumed at a frame
      // boundary before it can be selected as the next inactive build buffer.
      while (densePitchRequestIsCurrent(generation) && pitchSwapPending) {
        vTaskDelay(1);
      }
      if (!densePitchRequestIsCurrent(generation)) {
        continue;
      }
      if (forceSynthesizedPreview) {
        const int8_t buildPreviewIndex =
            activePreviewIndex == 0 ? 1 : 0;
        if (!buildPitchPreviewMap(pitchPreviewMaps[buildPreviewIndex],
                                  targetPitchQ8, generation)) {
          continue;
        }
        publishPitchSwap(PitchDenseMode::Preview, buildPreviewIndex,
                         targetPitchQ8, generation);

        while (densePitchRequestIsCurrent(generation) &&
               !(activePitchMode == PitchDenseMode::Preview &&
                 renderedPitchQ8 == targetPitchQ8)) {
          vTaskDelay(1);
        }
        if (!densePitchRequestIsCurrent(generation)) {
          continue;
        }
      }

      while (densePitchRequestIsCurrent(generation) &&
             pitchTouchActive &&
             millis() - pitchLastChangeMs < kPitchExactStabilityMs) {
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      if (!densePitchRequestIsCurrent(generation)) {
        continue;
      }
      if (!buildPitchExactMap(pitchExactMap, targetPitchQ8,
                              generation)) {
        continue;
      }
      pitchExactReady = true;

      while (densePitchRequestIsCurrent(generation) &&
             (pitchTouchActive
#if GLOBE_ENABLE_SCREENSHOT
              || screenshotForcePitchPreview ||
              screenshotForcePitchAnchorBlend
#endif
              )) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
      }
      if (!densePitchRequestIsCurrent(generation)) {
        continue;
      }
      publishPitchSwap(PitchDenseMode::Exact, -1, targetPitchQ8,
                       generation);
      break;
    }
  }
}

void activateReadyPitchLut() {
  if (!pitchSwapPending) {
    return;
  }
  portENTER_CRITICAL(&pitchLutMux);
  if (pitchSwapPending) {
    if (readyPitchGeneration == pitchRequestGeneration &&
        readyPitchQ8 == requestedPitchQ8) {
      if (readyPitchMode == PitchDenseMode::Preview) {
        activePreviewMapIndex = readyPreviewMapIndex;
      }
      activePitchMode = readyPitchMode;
      renderedPitchQ8 = readyPitchQ8;
      if (readyPitchMode == PitchDenseMode::Exact) {
        pitchExactActivatedMs = millis();
      } else if (readyPitchMode == PitchDenseMode::Zero) {
        pitchExactActivatedMs = 0;
      }
    }
    pitchSwapPending = false;
  }
  portEXIT_CRITICAL(&pitchLutMux);
}
#endif

