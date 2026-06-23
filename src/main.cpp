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
#include "world_texture.h"
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

#ifndef GLOBE_ENABLE_XY_ROTATION
#define GLOBE_ENABLE_XY_ROTATION 1
#endif

#ifndef GLOBE_ENABLE_SPAN_REFERENCE
#define GLOBE_ENABLE_SPAN_REFERENCE 0
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

#if GLOBE_ENABLE_XY_ROTATION
constexpr int16_t kMinimumPitchDegrees = -80;
constexpr int16_t kMaximumPitchDegrees = 80;
constexpr int16_t kMaximumPitchQ8 = kMaximumPitchDegrees * 256;
constexpr int16_t kMinimumPitchQ8 = kMinimumPitchDegrees * 256;
constexpr int16_t kPitchSensitivityQ8PerPixel =
    ((kMaximumPitchDegrees - kMinimumPitchDegrees) << 8) / kDisplayHeight;
constexpr uint16_t kPitchFrontUMask = 0x03FFU;
constexpr uint16_t kPitchFrontVMask = 0x01FFU;
constexpr uint16_t kPitchBackUMask = 0x03FFU;
constexpr uint16_t kPitchBackVMask = 0x01FFU;
constexpr int16_t kPitchAnchorDegrees[] = {
    -80, -55, -25, 0, 25, 55, 80,
};
constexpr uint8_t kPitchAnchorCount =
    sizeof(kPitchAnchorDegrees) / sizeof(kPitchAnchorDegrees[0]);
constexpr uint8_t kZeroPitchAnchorIndex = 3;
constexpr uint8_t kPitchPreviewBytesPerPixel = 3;
constexpr uint8_t kPitchExactBytesPerPixel = 5;
constexpr uint16_t kPitchExactStabilityMs = 150;
constexpr uint16_t kPitchBackFadeMs = 300;
constexpr uint16_t kProjectionTrigLutSize = 4097;

enum class PitchDenseMode : uint8_t {
  Zero = 0,
  Preview,
  Exact,
  AnchorBlend,
};

#if GLOBE_ENABLE_SPAN_REFERENCE
constexpr uint8_t kPitchCoordinateFractionBits = 6;
constexpr uint16_t kPitchCoordinateOne =
    1U << kPitchCoordinateFractionBits;
constexpr uint16_t kPitchSpanErrorQ6 = kPitchCoordinateOne;
constexpr uint16_t kPitchSpanCapacity = 16000;
constexpr uint8_t kMaximumPitchSpansPerRow = 64;
constexpr uint8_t kMaximumPitchSpanPixels = 64;

struct PitchSpan {
  uint16_t frontUQ6;
  uint16_t frontVQ6;
  uint16_t backUQ6;
  uint16_t backVQ6;
  int16_t frontUStepQ6;
  int16_t frontVStepQ6;
  int16_t backUStepQ6;
  int16_t backVStepQ6;
  uint16_t pixelCount;
};

struct PitchSpanRow {
  uint16_t firstSpan;
  uint8_t spanCount;
  uint8_t reserved;
};

struct PitchSpanLut {
  PitchSpan *spans;
  PitchSpanRow rows[kLutDiameter];
  uint16_t spanCount;
  int16_t pitchQ8;
  uint32_t buildMicros;
};

static_assert(sizeof(PitchSpan) == 18);
static_assert(sizeof(PitchSpanRow) == 4);
#endif
static_assert(kPitchAnchorCount == 7);
static_assert(kPitchAnchorDegrees[kZeroPitchAnchorIndex] == 0);
#endif

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
#if GLOBE_ENABLE_XY_ROTATION
uint16_t *sphereNzQ15 = nullptr;
#if GLOBE_ENABLE_SPAN_REFERENCE
PitchSpanLut pitchSpanLuts[2] = {};
PitchSpan pitchRowScratch[kMaximumPitchSpansPerRow];
volatile int8_t activePitchLutIndex = -1;
volatile int8_t readyPitchLutIndex = -1;
#endif
uint8_t *pitchAnchorMaps[kPitchAnchorCount] = {};
uint8_t *pitchPreviewMaps[2] = {};
uint8_t *pitchExactMap = nullptr;
uint8_t *pitchFrontTexture = nullptr;
uint8_t *pitchBackTextureHigh = nullptr;
uint16_t projectionAsinVLut[kProjectionTrigLutSize];
uint16_t projectionAtanULut[kProjectionTrigLutSize];
portMUX_TYPE pitchLutMux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t pitchBuilderTaskHandle = nullptr;
volatile PitchDenseMode activePitchMode = PitchDenseMode::Zero;
volatile PitchDenseMode readyPitchMode = PitchDenseMode::Zero;
volatile PitchDenseMode lastRenderedPitchMode = PitchDenseMode::Zero;
volatile bool pitchSwapPending = false;
volatile int8_t activePreviewMapIndex = -1;
volatile int8_t readyPreviewMapIndex = -1;
volatile int16_t readyPitchQ8 = 0;
volatile uint32_t readyPitchGeneration = 0;
volatile int16_t requestedPitchQ8 = 0;
volatile uint32_t pitchRequestGeneration = 0;
volatile uint32_t pitchLastChangeMs = 0;
volatile bool pitchTouchActive = false;
volatile bool pitchExactReady = false;
volatile uint32_t pitchExactActivatedMs = 0;
uint32_t pitchPreviewBuildMicros = 0;
uint32_t pitchExactBuildMicros = 0;
#if GLOBE_ENABLE_SCREENSHOT
volatile bool screenshotForcePitchPreview = false;
volatile bool screenshotForcePitchAnchorBlend = false;
#endif
#endif
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
#if GLOBE_ENABLE_XY_ROTATION
int16_t pitchQ8 = 0;
int16_t renderedPitchQ8 = 0;
#endif
uint32_t previousFrameMs = 0;

#if GLOBE_ENABLE_TOUCH
struct TouchInteractionState {
  bool available = false;
  bool down = false;
  bool moved = false;
  bool dragging = false;
  bool horizontalDragged = false;
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
bool screenshotFreezeRotation = false;
bool screenshotBackHemisphereEnabled = true;
constexpr uint16_t kScreenshotLongitudeStepTexels = 16;
#endif

QueueHandle_t freeFrameQueue = nullptr;
QueueHandle_t readyFrameQueue = nullptr;
SemaphoreHandle_t sharedI2cMutex = nullptr;

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
__attribute__((always_inline)) inline uint8_t samplePitchFront(
    uint16_t frontU, uint16_t frontV) {
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
}

__attribute__((always_inline)) inline uint8_t samplePitchBack(
    uint16_t backU, uint16_t backV, uint8_t backAlpha = 255) {
#if GLOBE_ENABLE_BACK_HEMISPHERE
  const uint8_t *__restrict__ backTextureRow =
      pitchBackTextureHigh + static_cast<uint32_t>(backV) *
                                 world_texture::kWidth;
  uint8_t backIntensity = backTextureRow[backU] & 0x0FU;
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
#if GLOBE_ENABLE_XY_ROTATION
  sphereNzQ15 = static_cast<uint16_t *>(heap_caps_malloc(
      static_cast<size_t>(kLutDiameter) * kLutDiameter *
          sizeof(uint16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  const size_t pitchPixelCount =
      static_cast<size_t>(kLutDiameter) * kLutDiameter;
  const size_t pitchPreviewMapByteCount =
      pitchPixelCount * kPitchPreviewBytesPerPixel;
  const size_t pitchExactMapByteCount =
      pitchPixelCount * kPitchExactBytesPerPixel;
  bool pitchAllocationFailed = false;
  for (uint8_t index = 0; index < kPitchAnchorCount; ++index) {
    if (index == kZeroPitchAnchorIndex) {
      continue;
    }
    pitchAnchorMaps[index] = static_cast<uint8_t *>(heap_caps_malloc(
        pitchPreviewMapByteCount, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    pitchAllocationFailed |= pitchAnchorMaps[index] == nullptr;
  }
  for (uint8_t index = 0; index < 2; ++index) {
    pitchPreviewMaps[index] = static_cast<uint8_t *>(heap_caps_malloc(
        pitchPreviewMapByteCount, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    pitchAllocationFailed |= pitchPreviewMaps[index] == nullptr;
  }
  pitchExactMap = static_cast<uint8_t *>(heap_caps_malloc(
      pitchExactMapByteCount, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  pitchFrontTexture = static_cast<uint8_t *>(heap_caps_malloc(
      globe_texture::kByteCount, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  pitchBackTextureHigh = static_cast<uint8_t *>(heap_caps_malloc(
      world_texture::kByteCount,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  pitchAllocationFailed |=
      sphereNzQ15 == nullptr || pitchExactMap == nullptr ||
      pitchFrontTexture == nullptr || pitchBackTextureHigh == nullptr;
#endif
  overlayPixels = static_cast<uint32_t *>(heap_caps_malloc(
      static_cast<size_t>(kOverlayPixelCapacity) * sizeof(uint32_t),
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  overlayMutex = xSemaphoreCreateMutex();

  if (frameBuffers[0] == nullptr || frameBuffers[1] == nullptr ||
      sphereLut == nullptr || overlayPixels == nullptr ||
      overlayMutex == nullptr
#if GLOBE_ENABLE_XY_ROTATION
      || pitchAllocationFailed
#endif
  ) {
    haltWithMessage("PSRAM ALLOCATION FAILED");
  }
  initializeBackground(frameBuffers[0], kFrameWidth, kFrameHeight,
                       kFrameScreenX, kFrameScreenY);
  initializeBackground(frameBuffers[1], kFrameWidth, kFrameHeight,
                       kFrameScreenX, kFrameScreenY);
#if GLOBE_ENABLE_XY_ROTATION
  memcpy(pitchFrontTexture, globe_texture::kIntensity,
         globe_texture::kByteCount);
  memcpy(pitchBackTextureHigh, world_texture::kIntensity,
         world_texture::kByteCount);
#endif

  buildColorPalettes();
  buildClockTrigLut();
  buildSphereLut();
#if GLOBE_ENABLE_XY_ROTATION
  buildProjectionTrigLuts();
  buildPitchAnchorBank();
#endif
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
#if GLOBE_ENABLE_XY_ROTATION
  if (xTaskCreatePinnedToCore(pitchBuilderTask, "globe-pitch", 16384, nullptr,
                              1, &pitchBuilderTaskHandle, 0) != pdPASS) {
    haltWithMessage("PITCH TASK FAILED");
  }
#endif
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
#if GLOBE_ENABLE_XY_ROTATION
  Serial.printf("Sphere Z LUT: %u bytes\n",
                kLutDiameter * kLutDiameter * sizeof(uint16_t));
  Serial.printf("Pitch anchors:%u bytes (6 front-only maps)\n",
                pitchPreviewMapByteCount * (kPitchAnchorCount - 1));
  Serial.printf("Pitch preview:%u bytes (2 dense maps)\n",
                pitchPreviewMapByteCount * 2);
  Serial.printf("Pitch exact:  %u bytes (front + back dense map)\n",
                pitchExactMapByteCount);
  Serial.printf("Pitch texture:%u bytes (PSRAM front + high-res back)\n",
                globe_texture::kByteCount +
                    world_texture::kByteCount);
#endif
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
#if GLOBE_ENABLE_XY_ROTATION
  activateReadyPitchLut();
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
