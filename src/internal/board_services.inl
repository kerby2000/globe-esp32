// Internal implementation fragment: Shared I2C, RTC/NTP, touch input, and display-controller detection.
// Included by main.cpp inside its anonymous namespace.

bool initializeSharedI2c() {
  static bool initialized = false;
  if (initialized) {
    return true;
  }
  if (sharedI2cMutex == nullptr) {
    sharedI2cMutex = xSemaphoreCreateMutex();
    if (sharedI2cMutex == nullptr) {
      return false;
    }
  }
  initialized = Wire.begin(kSharedI2cSda, kSharedI2cScl, kSharedI2cHz);
  if (initialized) {
    Wire.setTimeOut(20);
  }
  return initialized;
}

bool readI2cRegisters(uint8_t address, uint8_t reg, uint8_t *data,
                      uint8_t length) {
  if (sharedI2cMutex == nullptr) {
    return false;
  }
  xSemaphoreTake(sharedI2cMutex, portMAX_DELAY);
  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    xSemaphoreGive(sharedI2cMutex);
    return false;
  }

  if (Wire.requestFrom(address, length) != length) {
    while (Wire.available()) {
      Wire.read();
    }
    xSemaphoreGive(sharedI2cMutex);
    return false;
  }
  for (uint8_t index = 0; index < length; ++index) {
    data[index] = static_cast<uint8_t>(Wire.read());
  }
  xSemaphoreGive(sharedI2cMutex);
  return true;
}

bool writeI2cRegisters(uint8_t address, uint8_t reg, const uint8_t *data,
                       uint8_t length) {
  if (sharedI2cMutex == nullptr) {
    return false;
  }
  xSemaphoreTake(sharedI2cMutex, portMAX_DELAY);
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(data, length);
  const bool success = Wire.endTransmission() == 0;
  xSemaphoreGive(sharedI2cMutex);
  return success;
}

uint8_t bcdToDecimal(uint8_t value) {
  return static_cast<uint8_t>((value >> 4) * 10 + (value & 0x0F));
}

uint8_t decimalToBcd(uint8_t value) {
  return static_cast<uint8_t>((value / 10) << 4 | (value % 10));
}

int64_t daysFromCivil(int year, unsigned month, unsigned day) {
  // Gregorian calendar to Unix-day conversion. This keeps RTC decoding in UTC
  // without temporarily changing the process-wide Brussels timezone.
  year -= month <= 2;
  const int era = year / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned adjustedMonth = month > 2 ? month - 3 : month + 9;
  const unsigned dayOfYear =
      (153 * adjustedMonth + 2) / 5 + day - 1;
  const unsigned dayOfEra =
      yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return static_cast<int64_t>(era) * 146097 + dayOfEra - 719468;
}

bool readRtcUtc(time_t &epoch) {
  uint8_t registers[7];
  if (!readI2cRegisters(kRtcAddress, kRtcTimeRegister, registers,
                        sizeof(registers))) {
    return false;
  }

  // Bit 7 of the seconds register is VL. When set, oscillator history or
  // backup voltage was unreliable and the calendar must not be trusted.
  if ((registers[0] & 0x80U) != 0) {
    return false;
  }

  const int second = bcdToDecimal(registers[0] & 0x7F);
  const int minute = bcdToDecimal(registers[1] & 0x7F);
  const int hour = bcdToDecimal(registers[2] & 0x3F);
  const int day = bcdToDecimal(registers[3] & 0x3F);
  const int month = bcdToDecimal(registers[5] & 0x1F);
  const int year = 2000 + bcdToDecimal(registers[6]);
  if (second > 59 || minute > 59 || hour > 23 || day < 1 || day > 31 ||
      month < 1 || month > 12 || year < 2024 || year > 2099) {
    return false;
  }

  epoch = static_cast<time_t>(
      daysFromCivil(year, static_cast<unsigned>(month),
                    static_cast<unsigned>(day)) *
          86400 +
      hour * 3600 + minute * 60 + second);
  return epoch >= kMinimumValidEpoch;
}

bool writeRtcUtc(time_t epoch) {
  struct tm utc;
  if (gmtime_r(&epoch, &utc) == nullptr) {
    return false;
  }
  const uint8_t registers[7] = {
      decimalToBcd(utc.tm_sec),
      decimalToBcd(utc.tm_min),
      decimalToBcd(utc.tm_hour),
      decimalToBcd(utc.tm_mday),
      decimalToBcd(utc.tm_wday),
      decimalToBcd(utc.tm_mon + 1),
      decimalToBcd((utc.tm_year + 1900) % 100),
  };
  return writeI2cRegisters(kRtcAddress, kRtcTimeRegister, registers,
                           sizeof(registers));
}

bool setSystemTimeFromRtc() {
  time_t rtcEpoch = 0;
  if (!readRtcUtc(rtcEpoch)) {
    return false;
  }
  const struct timeval value = {rtcEpoch, 0};
  if (settimeofday(&value, nullptr) != 0) {
    return false;
  }
  systemTimeValid = true;
  return true;
}

void networkTimeTask(void *parameter) {
  if (GLOBE_WIFI_SSID[0] == '\0') {
#if GLOBE_ENABLE_SERIAL_STATS
    Serial.println("NTP: Wi-Fi credentials not configured");
#endif
    vTaskDelete(nullptr);
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(GLOBE_WIFI_SSID, GLOBE_WIFI_PASSWORD);
#if GLOBE_ENABLE_SERIAL_STATS
  Serial.println("NTP: connecting Wi-Fi");
#endif

  const uint32_t connectStartMs = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - connectStartMs < kWifiConnectTimeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  if (WiFi.status() == WL_CONNECTED) {
    configTzTime(kBrusselsTimezone, kNtpServer1, kNtpServer2, kNtpServer3);
    const uint32_t syncStartMs = millis();
    time_t now = 0;
    while (millis() - syncStartMs < kNtpSyncTimeoutMs) {
      if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        time(&now);
      }
      if (now >= kMinimumValidEpoch) {
        systemTimeValid = true;
        const bool rtcWritten = writeRtcUtc(now);
#if GLOBE_ENABLE_SERIAL_STATS
        Serial.printf("NTP: synchronized, RTC %s\n",
                      rtcWritten ? "updated" : "write failed");
#endif
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(100));
    }
#if GLOBE_ENABLE_SERIAL_STATS
    if (now < kMinimumValidEpoch) {
      Serial.println("NTP: synchronization timed out");
    }
#endif
  } else {
#if GLOBE_ENABLE_SERIAL_STATS
    Serial.println("NTP: Wi-Fi connection timed out");
#endif
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  vTaskDelete(nullptr);
}

#if GLOBE_ENABLE_TOUCH
bool readTouchRegisters(uint8_t reg, uint8_t *data, uint8_t length) {
  return readI2cRegisters(kTouchAddress, reg, data, length);
}

bool configureTouchController() {
  // Match Waveshare's driver: register 0 selects normal operating mode.
  const uint8_t normalMode = 0x00;
  if (!writeI2cRegisters(kTouchAddress, kTouchModeRegister, &normalMode, 1)) {
    return false;
  }

  uint8_t pointCount = 0;
  return readTouchRegisters(kTouchPointCountRegister, &pointCount, 1);
}

bool initializeTouch() {
  if (!initializeSharedI2c()) {
    return false;
  }

  // FT3168 needs time after power-up before it starts acknowledging I2C.
  // The old one-shot probe happened to work in debug builds because their
  // serial startup delay was 100 ms, but failed in the faster release build.
  const uint32_t startMs = millis();
  do {
    if (configureTouchController()) {
      return true;
    }
    delay(kTouchStartupRetryMs);
  } while (millis() - startMs < kTouchStartupTimeoutMs);
  return false;
}

bool readTouchPoint(int16_t &x, int16_t &y) {
  uint8_t pointCount = 0;
  if (!readTouchRegisters(kTouchPointCountRegister, &pointCount, 1) ||
      (pointCount & 0x0F) == 0) {
    return false;
  }

  uint8_t point[4];
  if (!readTouchRegisters(kTouchFirstPointRegister, point, sizeof(point))) {
    return false;
  }

  x = static_cast<int16_t>(((point[0] & 0x0F) << 8) | point[1]);
  y = static_cast<int16_t>(((point[2] & 0x0F) << 8) | point[3]);
  x = constrain(x, 0, kDisplayWidth - 1);
  y = constrain(y, 0, kDisplayHeight - 1);
  return true;
}

void handleLongPress() {
  clockMode = static_cast<ClockMode>(
      (static_cast<uint8_t>(clockMode) + 1U) %
      static_cast<uint8_t>(ClockMode::Count));
  clockOverlayDirty = true;
#if GLOBE_ENABLE_SERIAL_STATS
  static const char *const names[] = {
      "digital", "analog", "hybrid", "globe only",
  };
  Serial.printf("Clock mode: %s\n", names[static_cast<uint8_t>(clockMode)]);
#endif
}

#if GLOBE_ENABLE_XY_ROTATION
void setPitchQ8(int32_t requestedValueQ8) {
  const int16_t constrainedPitchQ8 = static_cast<int16_t>(
      constrain(requestedValueQ8,
                static_cast<int32_t>(kMinimumPitchQ8),
                static_cast<int32_t>(kMaximumPitchQ8)));
  if (constrainedPitchQ8 == pitchQ8) {
    return;
  }

  pitchQ8 = constrainedPitchQ8;
  portENTER_CRITICAL(&pitchLutMux);
  requestedPitchQ8 = constrainedPitchQ8;
  pitchRequestGeneration = pitchRequestGeneration + 1;
  pitchLastChangeMs = millis();
  pitchExactReady = false;
  // Any unpublished map belongs to the previous target. Discard it before
  // allowing the builder to reuse that preview buffer.
  pitchSwapPending = false;
  if (constrainedPitchQ8 == 0) {
    readyPitchMode = PitchDenseMode::Zero;
    readyPitchQ8 = 0;
    readyPitchGeneration = pitchRequestGeneration;
    pitchSwapPending = true;
  }
  portEXIT_CRITICAL(&pitchLutMux);
  if (pitchBuilderTaskHandle != nullptr) {
    xTaskNotifyGive(pitchBuilderTaskHandle);
  }
}
#endif

void resetGlobeView() {
  rotationQ16 = 0;
#if GLOBE_ENABLE_XY_ROTATION
  setPitchQ8(0);
#endif
  touchState.freeVelocityQ16PerMs = kAutoRotationVelocityQ16PerMs;
  touchState.dragVelocityQ16PerMs = 0;
#if GLOBE_ENABLE_SERIAL_STATS
  Serial.println("Touch: default view");
#endif
}

void updateTouchInteraction(uint32_t now, uint32_t elapsedMs) {
  if (!touchState.available &&
      now - touchState.lastRecoveryAttemptMs >= kTouchRecoveryIntervalMs) {
    touchState.lastRecoveryAttemptMs = now;
    touchState.available = configureTouchController();
#if GLOBE_ENABLE_SERIAL_STATS
    if (touchState.available) {
      Serial.println("FT3168 touch: recovered");
    }
#endif
  }

  int16_t x = 0;
  int16_t y = 0;
  const bool touched = touchState.available && readTouchPoint(x, y);

  if (touched) {
    if (!touchState.down) {
      touchState.down = true;
#if GLOBE_ENABLE_XY_ROTATION
      pitchTouchActive = true;
#endif
      touchState.moved = false;
      touchState.dragging = false;
      touchState.horizontalDragged = false;
      touchState.longPressHandled = false;
      touchState.startX = x;
      touchState.startY = y;
      touchState.lastX = x;
      touchState.lastY = y;
      touchState.downMs = now;
      touchState.lastSampleMs = now;
      touchState.dragVelocityQ16PerMs = 0;
#if GLOBE_ENABLE_SERIAL_STATS
      Serial.printf("Touch: down %d,%d\n", x, y);
#endif
      return;
    }

    const int16_t totalX = x - touchState.startX;
    const int16_t totalY = y - touchState.startY;
    if (abs(totalX) > kGestureMoveThreshold ||
        abs(totalY) > kGestureMoveThreshold) {
      touchState.moved = true;
    }
    if (touchState.moved) {
      touchState.dragging = true;
    }
    if (abs(totalX) > kGestureMoveThreshold) {
      touchState.horizontalDragged = true;
    }

    const uint32_t sampleElapsedMs = max<uint32_t>(1, now - touchState.lastSampleMs);
    const int16_t deltaX = x - touchState.lastX;
    const int16_t deltaY = y - touchState.lastY;
    if (touchState.horizontalDragged && deltaX != 0) {
      const int32_t rotationDeltaQ16 =
          -static_cast<int32_t>(deltaX) * kDragSensitivityQ16PerPixel;
      rotationQ16 += static_cast<uint32_t>(rotationDeltaQ16);

      int32_t sampleVelocity = rotationDeltaQ16 /
                               static_cast<int32_t>(sampleElapsedMs);
      sampleVelocity =
          constrain(sampleVelocity, -kMaximumInertiaVelocityQ16PerMs,
                    kMaximumInertiaVelocityQ16PerMs);
      touchState.dragVelocityQ16PerMs =
          (touchState.dragVelocityQ16PerMs * 2 + sampleVelocity) / 3;
    } else {
      // A stationary hold should not preserve an old fast flick sample.
      touchState.dragVelocityQ16PerMs =
          (touchState.dragVelocityQ16PerMs * 3) / 4;
    }
#if GLOBE_ENABLE_XY_ROTATION
    if (touchState.dragging && deltaY != 0) {
      // The globe follows the finger: dragging the northern cap downward
      // increases pitch and brings the north pole toward the viewer.
      setPitchQ8(static_cast<int32_t>(pitchQ8) +
                 static_cast<int32_t>(deltaY) *
                     kPitchSensitivityQ8PerPixel);
    }
#endif

    if (!touchState.moved && !touchState.longPressHandled &&
        now - touchState.downMs >= kLongPressMs) {
      touchState.longPressHandled = true;
      touchState.previousTapValid = false;
      handleLongPress();
    }

    touchState.lastX = x;
    touchState.lastY = y;
    touchState.lastSampleMs = now;
    return;
  }

  if (touchState.down) {
    const uint32_t pressDurationMs = now - touchState.downMs;
    if (touchState.dragging) {
      touchState.freeVelocityQ16PerMs =
          touchState.horizontalDragged
              ? constrain(touchState.dragVelocityQ16PerMs,
                          -kMaximumInertiaVelocityQ16PerMs,
                          kMaximumInertiaVelocityQ16PerMs)
              : kAutoRotationVelocityQ16PerMs;
      touchState.previousTapValid = false;
    } else if (!touchState.moved && !touchState.longPressHandled &&
               pressDurationMs <= kTapMaximumMs) {
      const bool nearby =
          abs(touchState.startX - touchState.previousTapX) <=
              kDoubleTapPositionThreshold &&
          abs(touchState.startY - touchState.previousTapY) <=
              kDoubleTapPositionThreshold;
      if (touchState.previousTapValid && nearby &&
          now - touchState.previousTapMs <= kDoubleTapMaximumGapMs) {
        resetGlobeView();
        touchState.previousTapValid = false;
      } else {
        touchState.previousTapValid = true;
        touchState.previousTapMs = now;
        touchState.previousTapX = touchState.startX;
        touchState.previousTapY = touchState.startY;
        touchState.freeVelocityQ16PerMs = kAutoRotationVelocityQ16PerMs;
      }
    } else {
      touchState.previousTapValid = false;
      touchState.freeVelocityQ16PerMs = kAutoRotationVelocityQ16PerMs;
    }
#if GLOBE_ENABLE_SERIAL_STATS
    Serial.printf("Touch: up %d,%d%s, pitch %d deg\n", touchState.lastX,
                  touchState.lastY, touchState.dragging ? " drag" : "",
#if GLOBE_ENABLE_XY_ROTATION
                  pitchQ8 / 256
#else
                  0
#endif
    );
#endif
    touchState.down = false;
#if GLOBE_ENABLE_XY_ROTATION
    pitchTouchActive = false;
    if (pitchBuilderTaskHandle != nullptr) {
      xTaskNotifyGive(pitchBuilderTaskHandle);
    }
#endif
  }

  if (touchState.previousTapValid &&
      now - touchState.previousTapMs > kDoubleTapMaximumGapMs) {
    touchState.previousTapValid = false;
  }

  // Preserve flick direction and speed after release, then smoothly converge
  // back to the normal automatic rotation instead of stopping abruptly.
#if GLOBE_ENABLE_SCREENSHOT
  if (screenshotFreezeRotation) {
    return;
  }
#endif
  rotationQ16 += static_cast<uint32_t>(
      touchState.freeVelocityQ16PerMs * static_cast<int32_t>(elapsedMs));
  const int32_t velocityDifference =
      kAutoRotationVelocityQ16PerMs - touchState.freeVelocityQ16PerMs;
  touchState.freeVelocityQ16PerMs +=
      velocityDifference * static_cast<int32_t>(elapsedMs) /
      kInertiaReturnToAutoMs;
}
#endif

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

