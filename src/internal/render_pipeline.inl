// Internal implementation fragment: Display transfer task and zero-pitch/preview/exact frame renderers.
// Included by main.cpp inside its anonymous namespace.

void displayTask(void *parameter) {
  uint16_t *buffer = nullptr;
  while (true) {
    if (xQueueReceive(readyFrameQueue, &buffer, portMAX_DELAY) == pdTRUE) {
#if GLOBE_ENABLE_SERIAL_STATS || GLOBE_ENABLE_SCREENSHOT
      const uint32_t transferStartUs = micros();
#endif
#if GLOBE_ENABLE_OVERLAY
#if GLOBE_ENABLE_SCREENSHOT
      const uint32_t overlayStartUs = micros();
#endif
      drawOverlay(buffer);
#if GLOBE_ENABLE_SCREENSHOT
      profileOverlayMicros += micros() - overlayStartUs;
#endif
#endif
      lastComposedFrame = buffer;
#if GLOBE_ENABLE_DISPLAY_TRANSFER
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

__attribute__((always_inline)) inline uint16_t sampleGlobeColor(
    uint16_t sample, const uint8_t *__restrict__ textureRow,
    const uint8_t *__restrict__ backTextureRow,
    uint16_t rotationTexels, uint16_t backPhase) {
  const uint16_t baseU = sample & kUMask;
  const uint16_t frontU =
      (baseU + rotationTexels) & (globe_texture::kWidth - 1);
#if GLOBE_USE_PACKED_FRONT_TEXTURE
  const uint8_t frontPair = textureRow[frontU >> 1];
  const uint8_t frontIntensity = static_cast<uint8_t>(
      (frontPair >> (((frontU ^ 1U) & 1U) << 2)) & 0x0FU);
#else
  const uint8_t frontPacked = textureRow[frontU];
  const uint8_t frontIntensity = frontPacked >> 4;
#endif
#if GLOBE_ENABLE_BACK_HEMISPHERE
  const uint16_t backUFull =
      (backPhase - frontU) & (globe_texture::kWidth - 1);
#if GLOBE_USE_HALF_BACK_TEXTURE
  const uint16_t backU = backUFull >> 1;
  const uint8_t backPair = backTextureRow[backU >> 1];
  uint8_t backIntensity =
      (backU & 1U) ? (backPair & 0x0FU) : (backPair >> 4);
#else
  uint8_t backIntensity = backTextureRow[backUFull] & 0x0FU;
#endif
#if GLOBE_ENABLE_SCREENSHOT
  if (!screenshotBackHemisphereEnabled) {
    backIntensity = 0;
  }
#endif
#if GLOBE_USE_DIRECT_COLOR_TABLE
  const uint16_t textureIndex =
      static_cast<uint16_t>(
          (frontIntensity << 4) | backIntensity);
#else
  const uint8_t backContribution =
      static_cast<uint8_t>((backIntensity * 3 + 2) >> 2);
  const uint8_t intensity =
      min<uint8_t>(15, frontIntensity + backContribution);
#endif
#else
#if GLOBE_USE_DIRECT_COLOR_TABLE
  const uint16_t textureIndex = frontIntensity << 4;
#else
  const uint8_t intensity = frontIntensity;
#endif
#endif

#if GLOBE_USE_DIRECT_COLOR_TABLE
  return globeColorTable[((sample >> 10) << 8) | textureIndex];
#else
  const uint8_t shade = (sample >> 10) & kShadeMask;
  const uint8_t coverage = sample >> 14;
  return globePalette[(coverage << 8) | (shade << 4) | intensity];
#endif
}

#if GLOBE_ENABLE_XY_ROTATION
__attribute__((always_inline)) inline uint8_t samplePitchFrontTiled(
    uint16_t frontU, uint16_t frontV) {
  // 16x4 packed tiles are exactly 32 bytes. All bits in this offset occupy
  // disjoint ranges, so OR avoids a multiply in the tilted hot loop.
  const uint32_t byteOffset =
      (static_cast<uint32_t>(frontV >> 2) << 11) |
      (static_cast<uint32_t>(frontU >> 4) << 5) |
      (static_cast<uint32_t>(frontV & 3U) << 3) |
      ((frontU & 15U) >> 1);
  const uint8_t pair = pitchFrontTexture[byteOffset];
  return static_cast<uint8_t>(
      (frontU & 1U) ? (pair & 0x0FU) : (pair >> 4));
}

__attribute__((always_inline)) inline uint8_t samplePitchFront(
    uint16_t frontU, uint16_t frontV) {
#if GLOBE_TILTED_FRONT_TILED
  return samplePitchFrontTiled(frontU, frontV);
#else
  const uint8_t *__restrict__ textureRow =
      pitchFrontTexture + static_cast<uint32_t>(frontV) *
#if GLOBE_USE_PACKED_FRONT_TEXTURE
                         globe_texture::kRowByteCount;
  const uint8_t frontPair = textureRow[frontU >> 1];
  return static_cast<uint8_t>(
      (frontPair >> (((frontU ^ 1U) & 1U) << 2)) & 0x0FU);
#else
                         globe_texture::kWidth;
  return textureRow[frontU] >> 4;
#endif
#endif
}

__attribute__((always_inline)) inline uint8_t samplePitchBackTiled(
    uint16_t backU, uint16_t backV) {
#if GLOBE_TILTED_BACK_MODE == 1
  const uint32_t byteOffset =
      (static_cast<uint32_t>(backV >> 2) << 11) |
      (static_cast<uint32_t>(backU >> 4) << 5) |
      (static_cast<uint32_t>(backV & 3U) << 3) |
      ((backU & 15U) >> 1);
  const uint8_t pair = pitchBackTextureHigh[byteOffset];
  return static_cast<uint8_t>(
      (backU & 1U) ? (pair & 0x0FU) : (pair >> 4));
#elif GLOBE_TILTED_BACK_MODE == 2
  const uint16_t halfU = backU >> 1;
  const uint16_t halfV = backV >> 1;
  const uint32_t byteOffset =
      (static_cast<uint32_t>(halfV >> 2) << 10) |
      (static_cast<uint32_t>(halfU >> 4) << 5) |
      (static_cast<uint32_t>(halfV & 3U) << 3) |
      ((halfU & 15U) >> 1);
  const uint8_t pair = pitchBackTextureHigh[byteOffset];
  return static_cast<uint8_t>(
      (halfU & 1U) ? (pair & 0x0FU) : (pair >> 4));
#else
  return 0;
#endif
}

__attribute__((always_inline)) inline uint8_t samplePitchBack(
    uint16_t backU, uint16_t backV, uint8_t backAlpha = 255) {
#if GLOBE_ENABLE_BACK_HEMISPHERE && GLOBE_TILTED_BACK_MODE != 3
#if GLOBE_TILTED_BACK_MODE == 0
  const uint8_t *__restrict__ backTextureRow =
      pitchBackTextureHigh + static_cast<uint32_t>(backV) *
                                 world_texture::kWidth;
  uint8_t backIntensity = backTextureRow[backU] & 0x0FU;
#else
  uint8_t backIntensity = samplePitchBackTiled(backU, backV);
#endif
#if GLOBE_ENABLE_SCREENSHOT
  if (!screenshotBackHemisphereEnabled) {
    backIntensity = 0;
  }
#endif
  if (backAlpha < 255U) {
    backIntensity = static_cast<uint8_t>(
        (static_cast<uint16_t>(backIntensity) * backAlpha + 127U) >>
        8);
  }
  return backIntensity;
#else
  return 0;
#endif
}

__attribute__((always_inline)) inline uint16_t composeTiltedGlobeColor(
    uint16_t surfaceSample, uint8_t frontIntensity,
    uint8_t backIntensity) {
#if GLOBE_USE_DIRECT_COLOR_TABLE
  const uint16_t textureIndex =
      static_cast<uint16_t>((frontIntensity << 4) | backIntensity);
  return globeColorTable[((surfaceSample >> 10) << 8) | textureIndex];
#else
  const uint8_t shade = (surfaceSample >> 10) & kShadeMask;
  const uint8_t coverage = surfaceSample >> 14;
  const uint8_t backContribution =
      static_cast<uint8_t>((backIntensity * 3 + 2) >> 2);
  const uint8_t intensity =
      min<uint8_t>(15, frontIntensity + backContribution);
  return globePalette[(coverage << 8) | (shade << 4) | intensity];
#endif
}

#if GLOBE_ENABLE_SPAN_REFERENCE
GLOBE_RENDER_MEM GLOBE_RENDER_OPT void renderPitchSpanLut(
    uint16_t rotationTexels, const PitchSpanLut &lut) {
  for (int16_t localY = 0; localY < kLutDiameter; ++localY) {
    uint16_t *__restrict__ destination =
        frameBuffer +
        static_cast<uint32_t>(kLutOriginY + localY - kFrameScreenY) *
            kFrameWidth +
        (kLutOriginX - kFrameScreenX);
    const uint32_t rowOffset =
        static_cast<uint32_t>(localY) * kLutDiameter;
    const uint16_t *__restrict__ surface = sphereLut + rowOffset;
    const uint16_t left = sphereLeft[localY];
    const uint16_t right = sphereRight[localY];
    if (left > right) {
      continue;
    }

    uint16_t *__restrict__ dst = destination + left;
    surface += left;
    const PitchSpanRow &row = lut.rows[localY];
    memcpy(pitchRowScratch, lut.spans + row.firstSpan,
           static_cast<size_t>(row.spanCount) * sizeof(PitchSpan));
    for (uint8_t spanIndex = 0; spanIndex < row.spanCount;
         ++spanIndex) {
      const PitchSpan &span = pitchRowScratch[spanIndex];
      int32_t frontUQ6 = span.frontUQ6;
      int32_t frontVQ6 = span.frontVQ6;
      int32_t backUQ6 = span.backUQ6;
      int32_t backVQ6 = span.backVQ6;
      uint16_t count = span.pixelCount;
      while (count-- != 0) {
        const uint16_t frontU =
            static_cast<uint16_t>(frontUQ6 >>
                                  kPitchCoordinateFractionBits) &
            kPitchFrontUMask;
        const uint16_t frontV = min<uint16_t>(
            kPitchFrontVMask,
            static_cast<uint16_t>(
                frontVQ6 >> kPitchCoordinateFractionBits));
        const uint16_t backU =
            static_cast<uint16_t>(backUQ6 >>
                                  kPitchCoordinateFractionBits) &
            kPitchBackUMask;
        const uint16_t backV = static_cast<uint16_t>(
            min<int32_t>(world_texture::kHeight - 1,
                         backVQ6 >> kPitchCoordinateFractionBits));
        const uint8_t frontIntensity = samplePitchFront(
            (frontU + rotationTexels) & kPitchFrontUMask, frontV);
        const uint8_t backIntensity = samplePitchBack(
            (backU + rotationTexels) & kPitchBackUMask, backV);
        *dst++ = composeTiltedGlobeColor(
            *surface++, frontIntensity, backIntensity);
        frontUQ6 += span.frontUStepQ6;
        frontVQ6 += span.frontVStepQ6;
        backUQ6 += span.backUStepQ6;
        backVQ6 += span.backVStepQ6;
      }
    }
  }
}
#endif

void renderGlobe(uint16_t rotationTexels);

GLOBE_RENDER_MEM GLOBE_RENDER_OPT void renderPitchPreviewLut(
    uint16_t rotationTexels, const uint8_t *__restrict__ map,
    uint8_t backAlpha = 255) {
  for (uint16_t localY = 0; localY < kLutDiameter; ++localY) {
    const uint16_t left = sphereLeft[localY];
    const uint16_t right = sphereRight[localY];
    if (left > right) {
      continue;
    }
    const uint32_t rowOffset =
        static_cast<uint32_t>(localY) * kLutDiameter;
    uint16_t *__restrict__ dst =
        frameBuffer +
        static_cast<uint32_t>(kLutOriginY + localY - kFrameScreenY) *
            kFrameWidth +
        (kLutOriginX - kFrameScreenX) + left;
    const uint16_t *__restrict__ surface =
        sphereLut + rowOffset + left;
    const uint8_t *__restrict__ coordinate =
        map + (rowOffset + left) * kPitchPreviewBytesPerPixel;
    uint16_t count = right - left + 1;
    while (count-- != 0) {
      uint16_t baseFrontU;
      uint16_t frontV;
      readPreviewPitchCoordinate(coordinate, baseFrontU, frontV);
      coordinate += kPitchPreviewBytesPerPixel;
      const uint16_t frontU =
          (baseFrontU + rotationTexels) & kPitchFrontUMask;
      // Preview stores only the blended visible surface. Mirror its final
      // longitude for the translucent rear layer, matching the zero-pitch
      // renderer while retaining the full 1024x512 diffuse texture.
      const uint16_t backU =
          (globe_texture::kWidth / 2U - baseFrontU + rotationTexels) &
          kPitchBackUMask;
      const uint8_t frontIntensity =
          samplePitchFront(frontU, frontV);
      const uint8_t backIntensity =
          samplePitchBack(backU, frontV, backAlpha);
      *dst++ = composeTiltedGlobeColor(
          *surface++, frontIntensity, backIntensity);
    }
  }
}

GLOBE_RENDER_MEM GLOBE_RENDER_OPT void renderPitchAnchorBlendPreview(
    uint16_t rotationTexels, int16_t targetPitchQ8,
    uint8_t backAlpha = 0) {
  uint8_t lowerIndex;
  uint8_t upperIndex;
  uint8_t blendQ8;
  selectPitchAnchors(targetPitchQ8, lowerIndex, upperIndex, blendQ8);

  if (lowerIndex == upperIndex || blendQ8 == 0) {
    if (lowerIndex == kZeroPitchAnchorIndex) {
      renderGlobe(rotationTexels);
      return;
    }
    renderPitchPreviewLut(rotationTexels, pitchAnchorMaps[lowerIndex],
                          backAlpha);
    return;
  }

  for (uint16_t localY = 0; localY < kLutDiameter; ++localY) {
    const uint16_t left = sphereLeft[localY];
    const uint16_t right = sphereRight[localY];
    if (left > right) {
      continue;
    }
    const uint32_t rowOffset =
        static_cast<uint32_t>(localY) * kLutDiameter;
    uint16_t *__restrict__ dst =
        frameBuffer +
        static_cast<uint32_t>(kLutOriginY + localY - kFrameScreenY) *
            kFrameWidth +
        (kLutOriginX - kFrameScreenX) + left;
    const uint16_t *__restrict__ surface =
        sphereLut + rowOffset + left;
    for (uint16_t localX = left; localX <= right; ++localX) {
      const uint32_t lutIndex = rowOffset + localX;
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
      const uint16_t baseFrontU = static_cast<uint16_t>(
          lowerU + ((wrappedDeltaU * blendQ8 + 128) >> 8)) &
          kPitchFrontUMask;
      const uint16_t frontV = static_cast<uint16_t>(
          lowerV +
          (((static_cast<int32_t>(upperV) - lowerV) * blendQ8 + 128) >>
           8));
      const uint16_t frontU =
          (baseFrontU + rotationTexels) & kPitchFrontUMask;
      const uint16_t backU =
          (globe_texture::kWidth / 2U - baseFrontU + rotationTexels) &
          kPitchBackUMask;
      const uint8_t frontIntensity =
          samplePitchFront(frontU, frontV);
      const uint8_t backIntensity =
          samplePitchBack(backU, frontV, backAlpha);
      *dst++ = composeTiltedGlobeColor(
          *surface++, frontIntensity, backIntensity);
    }
  }
}

GLOBE_RENDER_MEM GLOBE_RENDER_OPT void renderPitchExactLut(
    uint16_t rotationTexels, const uint8_t *__restrict__ map,
    uint8_t backAlpha = 255) {
  for (uint16_t localY = 0; localY < kLutDiameter; ++localY) {
    const uint16_t left = sphereLeft[localY];
    const uint16_t right = sphereRight[localY];
    if (left > right) {
      continue;
    }
    const uint32_t rowOffset =
        static_cast<uint32_t>(localY) * kLutDiameter;
    uint16_t *__restrict__ dst =
        frameBuffer +
        static_cast<uint32_t>(kLutOriginY + localY - kFrameScreenY) *
            kFrameWidth +
        (kLutOriginX - kFrameScreenX) + left;
    const uint16_t *__restrict__ surface =
        sphereLut + rowOffset + left;
    const uint8_t *__restrict__ coordinate =
        map + (rowOffset + left) * kPitchExactBytesPerPixel;
    uint16_t count = right - left + 1;
    while (count-- != 0) {
      const DensePitchCoordinates decoded =
          readExactPitchCoordinate(coordinate);
      coordinate += kPitchExactBytesPerPixel;
      const uint8_t frontIntensity = samplePitchFront(
          (decoded.frontU + rotationTexels) & kPitchFrontUMask,
          decoded.frontV);
      const uint8_t backIntensity = samplePitchBack(
          (decoded.backU + rotationTexels) & kPitchBackUMask,
          decoded.backV, backAlpha);
      *dst++ = composeTiltedGlobeColor(
          *surface++, frontIntensity, backIntensity);
    }
  }
}
#endif

GLOBE_RENDER_MEM GLOBE_RENDER_OPT void renderGlobe(uint16_t rotationTexels) {
  // backU = 1.5 turns - baseU + rotation. Reusing frontU avoids rebuilding
  // the same baseU/rotation expression for every pixel.
  const uint16_t backPhase =
      (globe_texture::kWidth / 2U + (rotationTexels << 1)) &
      (globe_texture::kWidth - 1);
  for (int16_t localY = 0; localY < kLutDiameter; ++localY) {
    uint16_t *__restrict__ destination =
        frameBuffer +
        static_cast<uint32_t>(kLutOriginY + localY - kFrameScreenY) *
            kFrameWidth +
        (kLutOriginX - kFrameScreenX);
    const uint16_t *__restrict__ lutRow =
        sphereLut + static_cast<uint32_t>(localY) * kLutDiameter;
    const uint16_t v = sphereV[localY];
    const uint8_t *__restrict__ textureRow =
        worldTexture + static_cast<uint32_t>(v) *
#if GLOBE_USE_PACKED_FRONT_TEXTURE
                           globe_texture::kRowByteCount;
#else
                           globe_texture::kWidth;
#endif
#if GLOBE_USE_HALF_BACK_TEXTURE
    const uint8_t *__restrict__ backTextureRow =
        world_back_texture::kIntensity +
        static_cast<uint32_t>(v >> 1) * world_back_texture::kRowByteCount;
#else
    const uint8_t *__restrict__ backTextureRow = textureRow;
#endif
    const uint16_t left = sphereLeft[localY];
    const uint16_t right = sphereRight[localY];
    if (left > right) {
      continue;
    }

#if GLOBE_RENDER_UNROLL == 0
    for (uint16_t localX = left; localX <= right; ++localX) {
      destination[localX] =
          sampleGlobeColor(lutRow[localX], textureRow, backTextureRow,
                           rotationTexels, backPhase);
    }
#else
    uint16_t *__restrict__ dst = destination + left;
    const uint16_t *__restrict__ lut = lutRow + left;
    uint16_t count = right - left + 1;
#if GLOBE_RENDER_UNROLL == 8
    while (count >= 8) {
      const uint16_t sample0 = *lut++;
      const uint16_t sample1 = *lut++;
      const uint16_t sample2 = *lut++;
      const uint16_t sample3 = *lut++;
      *dst++ =
          sampleGlobeColor(sample0, textureRow, backTextureRow, rotationTexels,
                           backPhase);
      *dst++ =
          sampleGlobeColor(sample1, textureRow, backTextureRow, rotationTexels,
                           backPhase);
      *dst++ =
          sampleGlobeColor(sample2, textureRow, backTextureRow, rotationTexels,
                           backPhase);
      *dst++ =
          sampleGlobeColor(sample3, textureRow, backTextureRow, rotationTexels,
                           backPhase);
      const uint16_t sample4 = *lut++;
      const uint16_t sample5 = *lut++;
      const uint16_t sample6 = *lut++;
      const uint16_t sample7 = *lut++;
      *dst++ =
          sampleGlobeColor(sample4, textureRow, backTextureRow, rotationTexels,
                           backPhase);
      *dst++ =
          sampleGlobeColor(sample5, textureRow, backTextureRow, rotationTexels,
                           backPhase);
      *dst++ =
          sampleGlobeColor(sample6, textureRow, backTextureRow, rotationTexels,
                           backPhase);
      *dst++ =
          sampleGlobeColor(sample7, textureRow, backTextureRow, rotationTexels,
                           backPhase);
      count -= 8;
    }
#elif GLOBE_RENDER_UNROLL == 4
    while (count >= 4) {
      const uint16_t sample0 = *lut++;
      const uint16_t sample1 = *lut++;
      const uint16_t sample2 = *lut++;
      const uint16_t sample3 = *lut++;
      *dst++ =
          sampleGlobeColor(sample0, textureRow, backTextureRow, rotationTexels,
                           backPhase);
      *dst++ =
          sampleGlobeColor(sample1, textureRow, backTextureRow, rotationTexels,
                           backPhase);
      *dst++ =
          sampleGlobeColor(sample2, textureRow, backTextureRow, rotationTexels,
                           backPhase);
      *dst++ =
          sampleGlobeColor(sample3, textureRow, backTextureRow, rotationTexels,
                           backPhase);
      count -= 4;
    }
#elif GLOBE_RENDER_UNROLL == 2
    while (count >= 2) {
      const uint16_t sample0 = *lut++;
      const uint16_t sample1 = *lut++;
      *dst++ =
          sampleGlobeColor(sample0, textureRow, backTextureRow, rotationTexels,
                           backPhase);
      *dst++ =
          sampleGlobeColor(sample1, textureRow, backTextureRow, rotationTexels,
                           backPhase);
      count -= 2;
    }
#endif
    while (count != 0) {
      *dst++ =
          sampleGlobeColor(*lut++, textureRow, backTextureRow, rotationTexels,
                           backPhase);
      --count;
    }
#endif
  }
}

void renderFrame(uint16_t rotationTexels) {
#if GLOBE_ENABLE_SCREENSHOT
  const uint32_t globeStartUs = micros();
#endif
#if GLOBE_ENABLE_XY_ROTATION
  const int16_t targetPitchQ8 = pitchQ8;
  if (targetPitchQ8 == 0) {
    renderGlobe(rotationTexels);
    lastRenderedPitchMode = PitchDenseMode::Zero;
  } else {
    const PitchDenseMode mode = activePitchMode;
    const int8_t previewIndex = activePreviewMapIndex;
    const int16_t preparedPitchQ8 = renderedPitchQ8;
    const bool preparedPreviewCurrent =
        mode == PitchDenseMode::Preview && previewIndex >= 0 &&
        preparedPitchQ8 == targetPitchQ8;
    const bool preparedExactCurrent =
        mode == PitchDenseMode::Exact && preparedPitchQ8 == targetPitchQ8;
#if GLOBE_ENABLE_SCREENSHOT
    const bool forceAnchorBlend = screenshotForcePitchAnchorBlend;
    const bool forceSynthesizedPreview =
        screenshotForcePitchPreview && !screenshotForcePitchAnchorBlend;
#else
    constexpr bool forceAnchorBlend = false;
    constexpr bool forceSynthesizedPreview = false;
#endif

    if (forceAnchorBlend ||
        (!preparedExactCurrent && !forceSynthesizedPreview)) {
      // Transient pitch uses the direct anchor blend. Hide the rear layer here
      // so release does not show several different back-hemisphere mappings.
      renderPitchAnchorBlendPreview(rotationTexels, targetPitchQ8, 0);
      lastRenderedPitchMode = PitchDenseMode::AnchorBlend;
    } else if (forceSynthesizedPreview && preparedPreviewCurrent) {
      renderPitchPreviewLut(rotationTexels,
                            pitchPreviewMaps[previewIndex], 255);
      lastRenderedPitchMode = PitchDenseMode::Preview;
    } else if (preparedExactCurrent) {
      uint8_t backAlpha = 255;
      const uint32_t exactActivatedMs = pitchExactActivatedMs;
      if (exactActivatedMs != 0) {
        const uint32_t elapsedMs = millis() - exactActivatedMs;
        if (elapsedMs < kPitchBackFadeMs) {
          backAlpha = static_cast<uint8_t>(
              (elapsedMs * 255U) / kPitchBackFadeMs);
        }
      }
      renderPitchExactLut(rotationTexels, pitchExactMap, backAlpha);
      lastRenderedPitchMode = PitchDenseMode::Exact;
    } else {
      renderPitchAnchorBlendPreview(rotationTexels, targetPitchQ8, 0);
      lastRenderedPitchMode = PitchDenseMode::AnchorBlend;
    }
  }
#else
  renderGlobe(rotationTexels);
#endif
#if GLOBE_ENABLE_SCREENSHOT
  profileGlobeMicros += micros() - globeStartUs;
#endif
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

