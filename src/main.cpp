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

// Tilted renderers can keep their private PSRAM texture copies in cache-line
// sized tiles. The zero-pitch renderer always continues to read the original
// row-major flash textures selected above.
#ifndef GLOBE_TILTED_FRONT_TILED
#define GLOBE_TILTED_FRONT_TILED 0
#endif

// 0: row-major full-resolution back (legacy 8-bit source bytes)
// 1: tiled full-resolution back, packed 4-bit
// 2: tiled 512x256 blurred back, packed 4-bit
// 3: no back hemisphere in tilted renderers
#ifndef GLOBE_TILTED_BACK_MODE
#define GLOBE_TILTED_BACK_MODE 0
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

static_assert(GLOBE_TILTED_BACK_MODE >= 0 &&
              GLOBE_TILTED_BACK_MODE <= 3);
static_assert(!GLOBE_TILTED_FRONT_TILED ||
              GLOBE_USE_PACKED_FRONT_TEXTURE);
static_assert(GLOBE_TILTED_BACK_MODE != 2 ||
              GLOBE_USE_HALF_BACK_TEXTURE);

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
constexpr uint8_t kPitchTextureTileWidth = 16;
constexpr uint8_t kPitchTextureTileHeight = 4;
constexpr uint8_t kPitchTextureTileBytes =
    kPitchTextureTileWidth * kPitchTextureTileHeight / 2;
constexpr size_t kPitchFrontTextureByteCount =
    static_cast<size_t>(globe_texture::kWidth) *
    globe_texture::kHeight / 2;
#if GLOBE_TILTED_BACK_MODE == 0
constexpr size_t kPitchBackTextureByteCount =
    world_texture::kByteCount;
#elif GLOBE_TILTED_BACK_MODE == 1
constexpr size_t kPitchBackTextureByteCount =
    static_cast<size_t>(world_texture::kWidth) *
    world_texture::kHeight / 2;
#elif GLOBE_TILTED_BACK_MODE == 2
constexpr size_t kPitchBackTextureByteCount =
    world_back_texture::kByteCount;
#else
constexpr size_t kPitchBackTextureByteCount = 0;
#endif
constexpr size_t kPitchTextureByteCount =
    kPitchFrontTextureByteCount + kPitchBackTextureByteCount;

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
static_assert(globe_texture::kWidth == 1024);
static_assert(globe_texture::kHeight == 512);
static_assert(kPitchTextureTileBytes == 32);
static_assert(globe_texture::kWidth % kPitchTextureTileWidth == 0);
static_assert(globe_texture::kHeight % kPitchTextureTileHeight == 0);
#if GLOBE_TILTED_BACK_MODE == 2
static_assert(world_back_texture::kWidth % kPitchTextureTileWidth == 0);
static_assert(world_back_texture::kHeight % kPitchTextureTileHeight == 0);
#endif
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

#include "internal/board_services.inl"
#include "internal/projection_pipeline.inl"
#include "internal/overlay.inl"
#include "internal/diagnostics.inl"
#include "internal/render_pipeline.inl"

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
      kPitchFrontTextureByteCount,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#if GLOBE_TILTED_BACK_MODE != 3
  pitchBackTextureHigh = static_cast<uint8_t *>(heap_caps_malloc(
      kPitchBackTextureByteCount,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#endif
  pitchAllocationFailed |=
      sphereNzQ15 == nullptr || pitchExactMap == nullptr ||
      pitchFrontTexture == nullptr
#if GLOBE_TILTED_BACK_MODE != 3
      || pitchBackTextureHigh == nullptr
#endif
      ;
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
  preparePitchTextures();
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
  Serial.printf(
      "Pitch texture:%u bytes (front tiled=%u, back mode=%u)\n",
      static_cast<unsigned>(kPitchTextureByteCount),
      static_cast<unsigned>(GLOBE_TILTED_FRONT_TILED),
      static_cast<unsigned>(GLOBE_TILTED_BACK_MODE));
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
