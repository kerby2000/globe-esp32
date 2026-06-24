// Internal implementation fragment: Optional framebuffer capture, profiling protocol, and serial debug commands.
// Included by main.cpp inside its anonymous namespace.

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
  // microseconds, cumulative globe and overlay microseconds, cumulative
  // transfer-task microseconds, and cumulative QSPI-only microseconds. The
  // quiet screenshot build emits this only on a 'Q' request.
  Serial.write(reinterpret_cast<const uint8_t *>("GLBQ"), 4);
  writeLittleEndian32(millis());
  writeLittleEndian32(renderedFrameCount);
  writeLittleEndian32(transferredFrameCount);
  writeLittleEndian32(profileRenderMicros);
  writeLittleEndian32(profileGlobeMicros);
  writeLittleEndian32(profileOverlayMicros);
  writeLittleEndian32(profileTransferMicros);
  writeLittleEndian32(profileQspiMicros);
  Serial.flush();
}

void sendPitchStatus() {
  // "GLBT", target/rendered pitch Q8, active mode, exact-ready flag,
  // active preview index (255 means none), last rendered mode, and the most
  // recent preview/exact build times. Capture tools use this only on demand.
  Serial.write(reinterpret_cast<const uint8_t *>("GLBT"), 4);
#if GLOBE_ENABLE_XY_ROTATION
  writeLittleEndian16(static_cast<uint16_t>(pitchQ8));
  writeLittleEndian16(static_cast<uint16_t>(renderedPitchQ8));
  Serial.write(static_cast<uint8_t>(activePitchMode));
  Serial.write(static_cast<uint8_t>(pitchExactReady));
  Serial.write(activePreviewMapIndex >= 0
                   ? static_cast<uint8_t>(activePreviewMapIndex)
                   : 0xFFU);
  Serial.write(static_cast<uint8_t>(lastRenderedPitchMode));
  writeLittleEndian32(pitchPreviewBuildMicros);
  writeLittleEndian32(pitchExactBuildMicros);
#else
  writeLittleEndian16(0);
  writeLittleEndian16(0);
  Serial.write(static_cast<uint8_t>(0));
  Serial.write(static_cast<uint8_t>(0));
  Serial.write(static_cast<uint8_t>(0xFF));
  Serial.write(static_cast<uint8_t>(0));
  writeLittleEndian32(0);
  writeLittleEndian32(0);
#endif
  Serial.flush();
}

void sendMemoryStatus() {
  // "GLBM", total/free/largest PSRAM, tilted texture bytes, and layout flags.
  Serial.write(reinterpret_cast<const uint8_t *>("GLBM"), 4);
  writeLittleEndian32(ESP.getPsramSize());
  writeLittleEndian32(ESP.getFreePsram());
  writeLittleEndian32(
      heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
#if GLOBE_ENABLE_XY_ROTATION
  writeLittleEndian32(kPitchTextureByteCount);
  Serial.write(static_cast<uint8_t>(GLOBE_TILTED_FRONT_TILED));
  Serial.write(static_cast<uint8_t>(GLOBE_TILTED_BACK_MODE));
#else
  writeLittleEndian32(0);
  Serial.write(static_cast<uint8_t>(0));
  Serial.write(static_cast<uint8_t>(0));
#endif
  Serial.write(static_cast<uint8_t>(GLOBE_USE_HALF_BACK_TEXTURE));
  Serial.write(static_cast<uint8_t>(0));
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
    } else if (command == 'T' || command == 't') {
      sendPitchStatus();
    } else if (command == 'H' || command == 'h') {
      sendMemoryStatus();
#if GLOBE_ENABLE_SERIAL_STATS
    } else if (command == 'I' || command == 'i') {
      Serial.printf(
          "Pitch target/rendered: %.2f/%.2f deg, mode: %u, "
          "preview/exact build: %.1f/%.1f ms, "
          "free PSRAM: %u bytes\n",
#if GLOBE_ENABLE_XY_ROTATION
          pitchQ8 / 256.0f, renderedPitchQ8 / 256.0f,
          static_cast<uint8_t>(activePitchMode),
          pitchPreviewBuildMicros / 1000.0f,
          pitchExactBuildMicros / 1000.0f,
#else
          0.0f, 0.0f, 0, 0.0f, 0.0f,
#endif
          ESP.getFreePsram());
#endif
    } else if (command == 'M' || command == 'm') {
      handleLongPress();
    } else if (command >= '1' && command <= '4') {
      clockMode = static_cast<ClockMode>(command - '1');
      clockOverlayDirty = true;
    } else if (command == '0') {
      resetGlobeView();
      screenshotFreezeRotation = true;
    } else if (command == 'F' || command == 'f') {
      screenshotFreezeRotation = !screenshotFreezeRotation;
    } else if (command == 'B' || command == 'b') {
      screenshotBackHemisphereEnabled =
          !screenshotBackHemisphereEnabled;
#if GLOBE_ENABLE_XY_ROTATION
    } else if (command == 'A' || command == 'a') {
      screenshotForcePitchAnchorBlend = true;
      screenshotForcePitchPreview = false;
      if (pitchQ8 != 0) {
        portENTER_CRITICAL(&pitchLutMux);
        pitchRequestGeneration = pitchRequestGeneration + 1;
        pitchLastChangeMs = millis();
        pitchExactReady = false;
        pitchSwapPending = false;
        portEXIT_CRITICAL(&pitchLutMux);
        if (pitchBuilderTaskHandle != nullptr) {
          xTaskNotifyGive(pitchBuilderTaskHandle);
        }
      }
    } else if (command == 'V' || command == 'v') {
      screenshotForcePitchPreview = true;
      screenshotForcePitchAnchorBlend = false;
      if (pitchQ8 != 0) {
        portENTER_CRITICAL(&pitchLutMux);
        pitchRequestGeneration = pitchRequestGeneration + 1;
        pitchLastChangeMs = millis();
        pitchExactReady = false;
        pitchSwapPending = false;
        portEXIT_CRITICAL(&pitchLutMux);
        if (pitchBuilderTaskHandle != nullptr) {
          xTaskNotifyGive(pitchBuilderTaskHandle);
        }
      }
    } else if (command == 'X' || command == 'x') {
      screenshotForcePitchPreview = false;
      screenshotForcePitchAnchorBlend = false;
      if (pitchBuilderTaskHandle != nullptr) {
        xTaskNotifyGive(pitchBuilderTaskHandle);
      }
#endif
    } else if (command == '.') {
      screenshotFreezeRotation = true;
      rotationQ16 +=
          static_cast<uint32_t>(kScreenshotLongitudeStepTexels) << 16;
    } else if (command == ',') {
      screenshotFreezeRotation = true;
      rotationQ16 -=
          static_cast<uint32_t>(kScreenshotLongitudeStepTexels) << 16;
#if GLOBE_ENABLE_XY_ROTATION
    } else if (command == '[') {
      setPitchQ8(static_cast<int32_t>(pitchQ8) -
                 10 * 256);
    } else if (command == ']') {
      setPitchQ8(static_cast<int32_t>(pitchQ8) +
                 10 * 256);
#endif
    }
  }
}
#endif

