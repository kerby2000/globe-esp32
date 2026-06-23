#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_sntp.h>
#include <freertos/queue.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>

#include "font5x7.h"
#include "ui_fonts.h"

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#define GLOBE_WIFI_SSID ""
#define GLOBE_WIFI_PASSWORD ""
#endif

#ifndef GLOBE_USE_HALF_TEXTURE
#define GLOBE_USE_HALF_TEXTURE 0
#endif

#ifndef GLOBE_USE_HALF_BACK_TEXTURE
#define GLOBE_USE_HALF_BACK_TEXTURE 0
#endif

#ifndef GLOBE_USE_PACKED_FRONT_TEXTURE
#define GLOBE_USE_PACKED_FRONT_TEXTURE 0
#endif

#if GLOBE_USE_PACKED_FRONT_TEXTURE
#include "world_front_1024.h"
namespace globe_texture = world_front_texture;
#elif GLOBE_USE_HALF_TEXTURE
#include "world_texture_512.h"
namespace globe_texture = world_texture;
#else
#include "world_texture.h"
namespace globe_texture = world_texture;
#endif

#if GLOBE_USE_HALF_BACK_TEXTURE
#include "world_back_512.h"
#endif

#ifndef GLOBE_ENABLE_SERIAL_STATS
#define GLOBE_ENABLE_SERIAL_STATS 0
#endif

#ifndef GLOBE_ENABLE_SCREENSHOT
#define GLOBE_ENABLE_SCREENSHOT 0
#endif

#ifndef GLOBE_ENABLE_TOUCH
#define GLOBE_ENABLE_TOUCH 1
#endif

#ifndef GLOBE_DISPLAY_BUS_HZ
#define GLOBE_DISPLAY_BUS_HZ 40000000
#endif

#ifndef GLOBE_TRANSFER_TIGHT
#define GLOBE_TRANSFER_TIGHT 0
#endif

#ifndef GLOBE_ENABLE_OVERLAY
#define GLOBE_ENABLE_OVERLAY 1
#endif

#ifndef GLOBE_ENABLE_BACK_HEMISPHERE
#define GLOBE_ENABLE_BACK_HEMISPHERE 1
#endif

#ifndef GLOBE_ENABLE_DISPLAY_TRANSFER
#define GLOBE_ENABLE_DISPLAY_TRANSFER 1
#endif

#ifndef GLOBE_USE_DIRECT_COLOR_TABLE
#define GLOBE_USE_DIRECT_COLOR_TABLE 0
#endif

#ifndef GLOBE_RENDER_UNROLL
#define GLOBE_RENDER_UNROLL 0
#endif

#ifndef GLOBE_RENDER_O3
#define GLOBE_RENDER_O3 0
#endif

#ifndef GLOBE_RENDER_IRAM
#define GLOBE_RENDER_IRAM 0
#endif

#ifndef GLOBE_OPTIMIZE_OVERLAY
#define GLOBE_OPTIMIZE_OVERLAY 0
#endif

#if GLOBE_RENDER_O3
#define GLOBE_RENDER_OPT __attribute__((optimize("O3")))
#else
#define GLOBE_RENDER_OPT
#endif

#if GLOBE_RENDER_IRAM
#define GLOBE_RENDER_MEM IRAM_ATTR
#else
#define GLOBE_RENDER_MEM
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
constexpr int8_t kSharedI2cSda = 47;
constexpr int8_t kSharedI2cScl = 48;
constexpr uint32_t kSharedI2cHz = 300000;

#if GLOBE_ENABLE_TOUCH
// FT3168 shares this I2C bus with the optional IMU and RTC.
constexpr uint8_t kTouchAddress = 0x38;
constexpr uint8_t kTouchModeRegister = 0x00;
constexpr uint8_t kTouchPointCountRegister = 0x02;
constexpr uint8_t kTouchFirstPointRegister = 0x03;
constexpr uint16_t kTouchStartupTimeoutMs = 250;
constexpr uint16_t kTouchStartupRetryMs = 10;
constexpr uint16_t kTouchRecoveryIntervalMs = 1000;
constexpr uint16_t kTapMaximumMs = 250;
constexpr uint16_t kDoubleTapMaximumGapMs = 350;
constexpr uint16_t kLongPressMs = 700;
constexpr int16_t kGestureMoveThreshold = 10;
constexpr int16_t kDoubleTapPositionThreshold = 48;
#endif

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
#if GLOBE_USE_HALF_BACK_TEXTURE
static_assert(world_back_texture::kWidth * 2 == globe_texture::kWidth);
static_assert(world_back_texture::kHeight * 2 == globe_texture::kHeight);
#endif
static_assert(!GLOBE_USE_PACKED_FRONT_TEXTURE || !GLOBE_USE_HALF_TEXTURE);
static_assert(!GLOBE_USE_PACKED_FRONT_TEXTURE ||
              GLOBE_USE_HALF_BACK_TEXTURE ||
              !GLOBE_ENABLE_BACK_HEMISPHERE);
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
constexpr uint16_t kAnalogCyanStyleBase = 288;
constexpr uint16_t kAnalogWhiteStyleBase = 304;
constexpr uint16_t kOverlayStyleCount = 320;
constexpr uint16_t kOverlayPixelCapacity = 16000;
constexpr uint8_t kRtcAddress = 0x51;
constexpr uint8_t kRtcTimeRegister = 0x04;
constexpr time_t kMinimumValidEpoch = 1704067200;  // 2024-01-01 UTC.
constexpr char kBrusselsTimezone[] =
    "CET-1CEST,M3.5.0/2,M10.5.0/3";
constexpr char kNtpServer1[] = "pool.ntp.org";
constexpr char kNtpServer2[] = "time.cloudflare.com";
constexpr char kNtpServer3[] = "time.google.com";
constexpr uint32_t kWifiConnectTimeoutMs = 20000;
constexpr uint32_t kNtpSyncTimeoutMs = 20000;

// One full rotation per 18 seconds, represented as Q16.16 texels/ms.
constexpr int32_t kAutoRotationVelocityQ16PerMs =
    (static_cast<uint32_t>(globe_texture::kWidth) << 16) / 18000U;
#if GLOBE_ENABLE_TOUCH
// Dragging across the display rotates the texture by one complete revolution.
constexpr int32_t kDragSensitivityQ16PerPixel =
    (static_cast<uint32_t>(globe_texture::kWidth) << 16) / kDisplayWidth;
// Bound pathological touch samples to a roughly 1.2-second minimum revolution.
constexpr int32_t kMaximumInertiaVelocityQ16PerMs =
    (static_cast<uint32_t>(globe_texture::kWidth) << 16) / 1200U;
constexpr uint16_t kInertiaReturnToAutoMs = 900;
#endif

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
volatile uint16_t *lastComposedFrame = nullptr;
uint16_t *sphereLut = nullptr;
const uint8_t *worldTexture = globe_texture::kIntensity;
uint16_t sphereLeft[kLutDiameter];
uint16_t sphereRight[kLutDiameter];
uint16_t sphereV[kLutDiameter];
int16_t clockSinQ14[360];
int16_t clockCosQ14[360];

#if GLOBE_USE_DIRECT_COLOR_TABLE
uint16_t globeColorTable[64 * 256];
#else
uint16_t globePalette[4 * 16 * 16];
#endif
uint16_t overlayColors[kOverlayStyleCount];
uint8_t overlayAlphas[kOverlayStyleCount];
#if GLOBE_OPTIMIZE_OVERLAY
uint8_t overlayRedBlend[kOverlayStyleCount * 32];
uint8_t overlayGreenBlend[kOverlayStyleCount * 64];
uint8_t overlayBlueBlend[kOverlayStyleCount * 32];
#endif
uint32_t *overlayPixels = nullptr;
uint16_t overlayPixelCount = 0;
bool overlayOverflow = false;
SemaphoreHandle_t overlayMutex = nullptr;
volatile bool systemTimeValid = false;
uint32_t lastClockUpdateMs = 0;
char displayedWeekday[10] = "SATURDAY";
char displayedTime[6] = "10:43";
char displayedDate[12] = "20 JUN 2026";
int8_t displayedSecond = -1;

enum class ClockMode : uint8_t {
  Digital = 0,
  Analog,
  Hybrid,
  GlobeOnly,
  Count,
};

ClockMode clockMode = ClockMode::Digital;
ClockMode displayedClockMode = ClockMode::Count;
bool clockOverlayDirty = true;

uint32_t rotationQ16 = 0;
uint32_t previousFrameMs = 0;

#if GLOBE_ENABLE_TOUCH
struct TouchInteractionState {
  bool available = false;
  bool down = false;
  bool moved = false;
  bool dragging = false;
  bool longPressHandled = false;
  bool previousTapValid = false;
  int16_t startX = 0;
  int16_t startY = 0;
  int16_t lastX = 0;
  int16_t lastY = 0;
  int16_t previousTapX = 0;
  int16_t previousTapY = 0;
  uint32_t downMs = 0;
  uint32_t lastSampleMs = 0;
  uint32_t previousTapMs = 0;
  uint32_t lastRecoveryAttemptMs = 0;
  int32_t dragVelocityQ16PerMs = 0;
  int32_t freeVelocityQ16PerMs = kAutoRotationVelocityQ16PerMs;
};

TouchInteractionState touchState;
#endif

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
uint32_t profileGlobeMicros = 0;
uint32_t profileOverlayMicros = 0;
volatile uint32_t profileTransferMicros = 0;
volatile uint32_t profileQspiMicros = 0;
#endif

QueueHandle_t freeFrameQueue = nullptr;
QueueHandle_t readyFrameQueue = nullptr;

bool initializeSharedI2c() {
  static bool initialized = false;
  if (initialized) {
    return true;
  }
  initialized = Wire.begin(kSharedI2cSda, kSharedI2cScl, kSharedI2cHz);
  if (initialized) {
    Wire.setTimeOut(20);
  }
  return initialized;
}

bool readI2cRegisters(uint8_t address, uint8_t reg, uint8_t *data,
                      uint8_t length) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom(address, length) != length) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }
  for (uint8_t index = 0; index < length; ++index) {
    data[index] = static_cast<uint8_t>(Wire.read());
  }
  return true;
}

bool writeI2cRegisters(uint8_t address, uint8_t reg, const uint8_t *data,
                       uint8_t length) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(data, length);
  return Wire.endTransmission() == 0;
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
  Wire.beginTransmission(kTouchAddress);
  Wire.write(kTouchModeRegister);
  Wire.write(static_cast<uint8_t>(0x00));
  if (Wire.endTransmission() != 0) {
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

void resetGlobeView() {
  rotationQ16 = 0;
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
      touchState.moved = false;
      touchState.dragging = false;
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
    if (abs(totalX) > kGestureMoveThreshold) {
      touchState.dragging = true;
    }

    const uint32_t sampleElapsedMs = max<uint32_t>(1, now - touchState.lastSampleMs);
    const int16_t deltaX = x - touchState.lastX;
    if (touchState.dragging && deltaX != 0) {
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
      touchState.freeVelocityQ16PerMs = constrain(
          touchState.dragVelocityQ16PerMs,
          -kMaximumInertiaVelocityQ16PerMs,
          kMaximumInertiaVelocityQ16PerMs);
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
    Serial.printf("Touch: up %d,%d%s\n", touchState.lastX, touchState.lastY,
                  touchState.dragging ? " drag" : "");
#endif
    touchState.down = false;
  }

  if (touchState.previousTapValid &&
      now - touchState.previousTapMs > kDoubleTapMaximumGapMs) {
    touchState.previousTapValid = false;
  }

  // Preserve flick direction and speed after release, then smoothly converge
  // back to the normal automatic rotation instead of stopping abruptly.
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

void serviceSerialCommands() {
  while (Serial.available() > 0) {
    const int command = Serial.read();
    if (command == 'S' || command == 's') {
      sendFramebufferScreenshot();
    } else if (command == 'P' || command == 'p') {
      sendPerformanceSnapshot();
    } else if (command == 'Q' || command == 'q') {
      sendDetailedPerformanceSnapshot();
    } else if (command == 'M' || command == 'm') {
      handleLongPress();
    } else if (command >= '1' && command <= '4') {
      clockMode = static_cast<ClockMode>(command - '1');
      clockOverlayDirty = true;
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
  const uint8_t backIntensity =
      (backU & 1U) ? (backPair & 0x0FU) : (backPair >> 4);
#else
  const uint8_t backPacked = backTextureRow[backUFull];
#endif
#if GLOBE_USE_DIRECT_COLOR_TABLE
  const uint16_t textureIndex =
      static_cast<uint16_t>(
          (frontIntensity << 4) |
#if GLOBE_USE_HALF_BACK_TEXTURE
          backIntensity
#else
          (backPacked & 0x0FU)
#endif
      );
#else
#if !GLOBE_USE_HALF_BACK_TEXTURE
  const uint8_t backIntensity = backPacked & 0x0F;
#endif
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
  renderGlobe(rotationTexels);
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

void setup() {
#if GLOBE_ENABLE_SERIAL_STATS || GLOBE_ENABLE_SCREENSHOT
  Serial.begin(115200);
#endif

#if GLOBE_ENABLE_SERIAL_STATS
  Serial.println("\nESP32-S3 AMOLED rotating globe");
#endif

  setenv("TZ", kBrusselsTimezone, 1);
  tzset();

  pinMode(kAmoledEnable, OUTPUT);
  digitalWrite(kAmoledEnable, HIGH);
  delay(20);

  initializeSharedI2c();

#if GLOBE_ENABLE_TOUCH
  // Initialize before the slower panel setup. FT3168 can enter monitor mode,
  // where shared-bus traffic may temporarily make it stop acknowledging I2C.
  touchState.available = initializeTouch();
#if GLOBE_ENABLE_SERIAL_STATS
  Serial.printf("FT3168 touch: %s\n",
                touchState.available ? "ready" : "not detected");
#endif
#endif

  const bool rtcLoaded = setSystemTimeFromRtc();
#if GLOBE_ENABLE_SERIAL_STATS
  Serial.printf("PCF85063 RTC: %s\n",
                rtcLoaded ? "time loaded" : "invalid or unavailable");
#endif

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
  overlayMutex = xSemaphoreCreateMutex();

  if (frameBuffers[0] == nullptr || frameBuffers[1] == nullptr ||
      sphereLut == nullptr || overlayPixels == nullptr ||
      overlayMutex == nullptr) {
    haltWithMessage("PSRAM ALLOCATION FAILED");
  }
  initializeBackground(frameBuffers[0], kFrameWidth, kFrameHeight,
                       kFrameScreenX, kFrameScreenY);
  initializeBackground(frameBuffers[1], kFrameWidth, kFrameHeight,
                       kFrameScreenX, kFrameScreenY);

  buildColorPalettes();
  buildClockTrigLut();
  buildSphereLut();
  refreshClockOverlay(true);
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
  if (xTaskCreatePinnedToCore(networkTimeTask, "globe-ntp", 6144, nullptr, 1,
                              nullptr, 0) != pdPASS) {
#if GLOBE_ENABLE_SERIAL_STATS
    Serial.println("NTP: task creation failed");
#endif
  }

  previousFrameMs = millis();
  lastClockUpdateMs = previousFrameMs;

#if GLOBE_ENABLE_SERIAL_STATS
  Serial.printf("Frame buffer: %u bytes\n",
                kFrameWidth * kFrameHeight * sizeof(uint16_t));
  Serial.printf("Sphere LUT:   %u bytes\n",
                kLutDiameter * kLutDiameter * sizeof(uint16_t));
  Serial.printf("World texture:%u bytes\n", globe_texture::kByteCount);
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
#if GLOBE_ENABLE_TOUCH
  updateTouchInteraction(now, elapsedMs);
#else
  rotationQ16 += elapsedMs * kAutoRotationVelocityQ16PerMs;
#endif
  if (clockOverlayDirty || now - lastClockUpdateMs >= 1000) {
    lastClockUpdateMs = now;
    refreshClockOverlay(false);
  }

#if GLOBE_ENABLE_SERIAL_STATS || GLOBE_ENABLE_SCREENSHOT
  const uint32_t renderStartUs = micros();
#endif
  renderFrame((rotationQ16 >> 16) & (globe_texture::kWidth - 1));
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
  if (Serial.available() > 0) {
    while (lastComposedFrame != renderBuffer) {
      delay(1);
    }
    serviceSerialCommands();
  }
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
