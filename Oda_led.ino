#include <WiFi.h>
#include <WebServer.h>
#include <FastLED.h>
#include <EEPROM.h>
#include <math.h>
#include <WiFiClient.h>
#include <Update.h>

extern uint32_t absenceOffMs;
extern uint32_t restoreWindowMs;

#define EEPROM_SIZE_BYTES 64
#define EE_ADDR_BASE 0
#define EE_ADDR_RTC_EPOCH (EE_ADDR_BASE + 32)
#define EE_ADDR_RTC_TZMIN (EE_ADDR_BASE + 36)

struct Persist {
  uint32_t magic;
  uint32_t version;
  uint32_t absenceMs;
  uint32_t restoreMs;
  uint32_t checksum;
  uint32_t gIgnorePirUntilMs;
};

static const uint32_t PERSIST_MAGIC = 0x4D43544C;
static const uint32_t PERSIST_VERSION = 1;

static inline uint32_t persistChecksum(const Persist& p) {
  return (p.magic ^ p.version ^ p.absenceMs ^ p.restoreMs ^ 0xA5A5A5A5UL);
}

uint32_t absenceOffMs = 60000;
uint32_t restoreWindowMs = 600000;

static inline void loadPersist() {
  Persist p{};
  EEPROM.get(EE_ADDR_BASE, p);
  if (p.magic == PERSIST_MAGIC && p.version == PERSIST_VERSION && p.checksum == persistChecksum(p)) {
    if (p.absenceMs >= 60000UL && p.absenceMs <= 600000UL) absenceOffMs = p.absenceMs;
    Serial.println("[EEPROM] Ayarlar yüklendi.");
  } else {
    Serial.println("[EEPROM] Ilk kurulu/geçersiz veri. Varsayılanlar yazılıyor...");
    Persist np{PERSIST_MAGIC, PERSIST_VERSION, absenceOffMs, restoreWindowMs, 0, 0};
    np.checksum = persistChecksum(np);
    EEPROM.put(EE_ADDR_BASE, np);
    EEPROM.commit();
  }
}

static inline void savePersist() {
  Persist p{PERSIST_MAGIC, PERSIST_VERSION, absenceOffMs, restoreWindowMs, 0, 0};
  p.checksum = persistChecksum(p);
  EEPROM.put(EE_ADDR_BASE, p);
  EEPROM.commit();
  Serial.println("[EEPROM] Ayarlar kaydedildi.");
}

#define LED_PIN 4
#define PIR_PIN 13
#define LED_COLS 39
#define LED_ROWS 16
#define LED_TYPE WS2812B
#define COLOR_ORDER BGR

static const int INVALID = -1;

const uint32_t PIR_DELAY_MS = 60000;
const uint8_t PIR_FADE_TARGET_BRIGHTNESS = 200;

uint32_t gLastPirSampleMs = 0;
uint32_t gLastMotionMs = 0;
static uint32_t pirHighStarted = 0;
const uint16_t PIR_DEBOUNCE_MS = 80;
const uint16_t PIR_HOLD_MS = 300;

bool gPirEnabled = true;
uint32_t gIgnorePirUntilMs = 0;
uint32_t gAutoOffRearmUntilMs = 0;
bool gLastPirLevel = false;

int gRowOffsets[LED_ROWS] = {
  0, 0, 0, 2, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1
};

const uint8_t perRowCounts[LED_ROWS] = {
  39, 37, 39, 37, 39, 39, 39, 39, 39, 38, 39, 38, 32, 39, 39, 39
};

struct XY { uint8_t x, y; };

XY DEAD_PIXELS[] = {
};

const uint16_t DEAD_COUNT = sizeof(DEAD_PIXELS) / sizeof(DEAD_PIXELS[0]);

inline bool isDead(uint8_t x, uint8_t y) {
  for (uint16_t i = 0; i < DEAD_COUNT; i++)
    if (DEAD_PIXELS[i].x == x && DEAD_PIXELS[i].y == y) return true;
  return false;
}

inline uint16_t XYi(uint8_t x, uint8_t y) {
  return (uint16_t)y * LED_COLS + x;
}

inline uint8_t clampu8(int v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return (uint8_t)v;
}

CRGB* leds = nullptr;
int physCount = 0;
int* mapping = nullptr;

void buildMapping() {
  if (mapping) free(mapping);
  mapping = (int*)malloc(sizeof(int) * LED_ROWS * LED_COLS);
  if (!mapping) {
    Serial.println("ERR: mapping alloc");
    while (1) delay(1000);
  }

  for (int i = 0; i < LED_ROWS * LED_COLS; i++)
    mapping[i] = INVALID;

  physCount = 0;

  for (uint8_t y = 0; y < LED_ROWS; y++) {
    uint8_t count = perRowCounts[y];
    if (count == 0) continue;
    int physicalIndex = physCount;

    if ((y % 2) == 0) {
      uint8_t placed = 0;
      for (uint8_t x = 0; x < LED_COLS; x++) {
        if (placed < count) {
          mapping[y * LED_COLS + x] = physicalIndex++;
          placed++;
        } else {
          mapping[y * LED_COLS + x] = INVALID;
        }
      }
    } else {
      uint8_t placed = 0;
      for (int x = LED_COLS - 1; x >= 0; x--) {
        if (placed < count) {
          mapping[y * LED_COLS + x] = physicalIndex++;
          placed++;
        } else {
          mapping[y * LED_COLS + x] = INVALID;
        }
      }
    }

    physCount = physicalIndex;
  }

  if (leds) free(leds);
  leds = (CRGB*)malloc(sizeof(CRGB) * physCount);
  if (!leds) {
    Serial.println("ERR: leds alloc");
    while (1) delay(1000);
  }
}

struct ColSpan { uint8_t x, y0, y1; };
struct RowSpan { uint8_t y, x0, x1; };
struct Rect { uint8_t x0, y0, x1, y1; };

inline bool inRange(uint8_t v, uint8_t a, uint8_t b) { return v >= a && v <= b; }

XY RG_SWAP_POINTS[] = {
};

const uint16_t RG_SWAP_POINTS_N = sizeof(RG_SWAP_POINTS) / sizeof(RG_SWAP_POINTS[0]);

ColSpan RG_SWAP_COLS[] = {
};

const uint16_t RG_SWAP_COLS_N = sizeof(RG_SWAP_COLS) / sizeof(RG_SWAP_COLS[0]);

RowSpan RG_SWAP_ROWS[] = {
  { 6, 12, 38 },
  { 5, 0, 38 },
  { 8, 0, 38 },
  { 13, 0, 38 },
  { 14, 0, 38 },
  { 12, 24, 31 }
};

const uint16_t RG_SWAP_ROWS_N = sizeof(RG_SWAP_ROWS) / sizeof(RG_SWAP_ROWS[0]);

Rect RG_SWAP_RECTS[] = {
};

const uint16_t RG_SWAP_RECTS_N = sizeof(RG_SWAP_RECTS) / sizeof(RG_SWAP_RECTS[0]);

inline bool needsSwapRG(uint8_t x, uint8_t y) {
  for (uint16_t i = 0; i < RG_SWAP_POINTS_N; i++)
    if (RG_SWAP_POINTS[i].x == x && RG_SWAP_POINTS[i].y == y) return true;

  for (uint16_t i = 0; i < RG_SWAP_COLS_N; i++)
    if (RG_SWAP_COLS[i].x == x && inRange(y, RG_SWAP_COLS[i].y0, RG_SWAP_COLS[i].y1)) return true;

  for (uint16_t i = 0; i < RG_SWAP_ROWS_N; i++)
    if (RG_SWAP_ROWS[i].y == y && inRange(x, RG_SWAP_ROWS[i].x0, RG_SWAP_ROWS[i].x1)) return true;

  for (uint16_t i = 0; i < RG_SWAP_RECTS_N; i++)
    if (inRange(x, RG_SWAP_RECTS[i].x0, RG_SWAP_RECTS[i].x1) && inRange(y, RG_SWAP_RECTS[i].y0, RG_SWAP_RECTS[i].y1)) return true;

  return false;
}

inline void setXY(uint8_t x, uint8_t y, CRGB c) {
  if (x >= LED_COLS || y >= LED_ROWS || !leds) return;
  if (isDead(x, y)) return;

  if (needsSwapRG(x, y)) {
    uint8_t t = c.r; c.r = c.g; c.g = t;
  }

  uint8_t xo = (x + gRowOffsets[y]) % LED_COLS;
  int m = mapping[y * LED_COLS + xo];
  if (m != INVALID && m < physCount)
    leds[m] = c;
}

inline void addXY(uint8_t x, uint8_t y, CRGB c) {
  if (x >= LED_COLS || y >= LED_ROWS || !leds) return;
  if (isDead(x, y)) return;

  if (needsSwapRG(x, y)) {
    uint8_t t = c.r; c.r = c.g; c.g = t;
  }

  uint8_t xo = (x + gRowOffsets[y]) % LED_COLS;
  int m = mapping[y * LED_COLS + xo];
  if (m != INVALID && m < physCount)
    leds[m] += c;
}

uint8_t gHue = 0;
bool gPower = true;
uint8_t gBrightness = 140;
uint8_t gSpeed = 100;
uint8_t gEffect = 0;
bool gSolidMode = false;
CRGB gSolidColor = CRGB::White;
bool gPaused = false;
uint8_t gLastUserBrightness = 140;

String gText = "I LOVE YOU NAZAN";
int gTextOffset = LED_COLS;
uint8_t gTextHue = 0;
uint8_t gMarqueeSpeed = 100;

uint32_t gPIRDelayStart = 0;
bool gFadeActive = false;
uint8_t gFadeTarget = 200;
uint32_t gLastFadeStepMs = 0;

inline void startFadeTo(uint8_t target, uint8_t start = 0) {
  gFadeTarget = target;
  gFadeActive = true;
  gLastFadeStepMs = 0;
  gBrightness = start;
  FastLED.setBrightness(gBrightness);
}

uint32_t gAutoOffAtMs = 0;

struct SavedState {
  bool power;
  uint8_t brightness;
  uint8_t speed;
  uint8_t effect;
  bool solidMode;
  CRGB solidColor;
  bool paused;
  uint8_t hue;
  int textOffset;
  uint8_t textHue;
  uint8_t marqueeSpeed;
};

SavedState gSaved{};
bool gHasSavedState = false;

inline void saveCurrentState() {
  gSaved = {true, gBrightness, gSpeed, gEffect, gSolidMode, gSolidColor, gPaused, gHue, gTextOffset, gTextHue, gMarqueeSpeed};
  gHasSavedState = true;
}

inline void restoreSavedStateWithFade() {
  if (!gHasSavedState) return;
  gPower = true;
  gSpeed = gSaved.speed;
  gEffect = gSaved.effect;
  gSolidMode = gSaved.solidMode;
  gSolidColor = gSaved.solidColor;
  gPaused = gSaved.paused;
  gHue = gSaved.hue;
  gTextOffset = gSaved.textOffset;
  gTextHue = gSaved.textHue;
  gMarqueeSpeed = gSaved.marqueeSpeed;
  startFadeTo(gSaved.brightness, 0);
}

enum OffReason { OFF_NONE, OFF_MANUAL, OFF_AUTO };
OffReason gPendingOff = OFF_NONE;

uint32_t gTouchBoostUntilMs = 0;

uint32_t deviceEpoch = 0;
uint32_t deviceEpochSetAtMs = 0;
int32_t tzOffsetMin = 180;

int16_t pirOnMin = -1;
int16_t pirOffMin = -1;
int32_t schedDayIndex = -1;
bool appliedOnToday = false;
bool appliedOffToday = false;

void updateDeviceTime() {
  static uint32_t lastUpdateMs = 0;
  uint32_t nowMs = millis();
  if (lastUpdateMs == 0) {
    lastUpdateMs = nowMs;
    return;
  }
  uint32_t elapsed = nowMs - lastUpdateMs;
  if (elapsed >= 1000) {
    deviceEpoch += elapsed / 1000;
    lastUpdateMs = nowMs - (elapsed % 1000);
  }
}

inline uint32_t nowUtcSec() {
  return deviceEpoch;
}

inline bool localDayAndMinute(int32_t& dayIndex, uint16_t& minuteOfDay) {
  uint32_t n = nowUtcSec();
  if (n == 0) return false;
  int64_t local = (int64_t)n + (int64_t)tzOffsetMin * 60;
  if (local < 0) local = 0;
  dayIndex = (int32_t)(local / 86400);
  uint32_t secOfDay = (uint32_t)(local % 86400);
  minuteOfDay = (uint16_t)(secOfDay / 60);
  return true;
}

inline int16_t parseHHMM(const String& s) {
  if (s.length() < 4) return -1;
  int c = s.indexOf(':');
  if (c <= 0) return -1;
  int hh = s.substring(0, c).toInt();
  int mm = s.substring(c + 1).toInt();
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return -1;
  return (int16_t)(hh * 60 + mm);
}

inline bool pirIsHighStable(uint32_t nowMs) {
  if (!gPirEnabled) return false;
  if (nowMs < gIgnorePirUntilMs) return false;
  bool level = (digitalRead(PIR_PIN) == HIGH);
  if (level) {
    if (pirHighStarted == 0) pirHighStarted = nowMs;
    if (nowMs - pirHighStarted >= PIR_HOLD_MS) return true;
  } else {
    pirHighStarted = 0;
  }
  return false;
}

struct Dot { float x, y, vx, vy, life; };
const uint8_t MAX_DOTS = 32;
Dot dots[MAX_DOTS];

inline void spawnDot(float x, float y) {
  for (uint8_t i = 0; i < MAX_DOTS; i++) {
    if (dots[i].life <= 0) {
      dots[i] = {x, y, 0, 0, 1.0f};
      return;
    }
  }
}

inline uint16_t frameDelayMs() {
  return map(gSpeed, 1, 400, 80, 2);
}

struct Glyph { uint8_t ch; uint8_t col[5]; };

#define G(c,a,b,d,e,f) {c,{a,b,d,e,f}}

const Glyph FONT[] PROGMEM = {
  G(' ',0x00,0x00,0x00,0x00,0x00),
  G('0',0x0E,0x11,0x11,0x11,0x0E),
  G('1',0x04,0x06,0x04,0x04,0x1F),
  G('2',0x0E,0x11,0x02,0x04,0x1F),
  G('3',0x1F,0x02,0x0E,0x02,0x1F),
  G('4',0x02,0x06,0x0A,0x1F,0x02),
  G('5',0x1F,0x10,0x1E,0x01,0x1E),
  G('6',0x0E,0x10,0x1E,0x11,0x0E),
  G('7',0x1F,0x01,0x02,0x04,0x08),
  G('8',0x0E,0x11,0x0E,0x11,0x0E),
  G('9',0x0E,0x11,0x0F,0x01,0x0E),
  G('A',0x0E,0x11,0x1F,0x11,0x11),
  G('B',0x1E,0x11,0x1E,0x11,0x1E),
  G('C',0x0E,0x11,0x10,0x11,0x0E),
  G('D',0x1C,0x12,0x11,0x12,0x1C),
  G('E',0x1F,0x10,0x1E,0x10,0x1F),
  G('F',0x1F,0x10,0x1E,0x10,0x10),
  G('G',0x0F,0x10,0x13,0x11,0x0F),
  G('H',0x11,0x11,0x1F,0x11,0x11),
  G('I',0x1F,0x04,0x04,0x04,0x1F),
  G('J',0x01,0x01,0x01,0x11,0x0E),
  G('K',0x11,0x12,0x1C,0x12,0x11),
  G('L',0x10,0x10,0x10,0x10,0x1F),
  G('M',0x11,0x1B,0x15,0x11,0x11),
  G('N',0x11,0x19,0x15,0x13,0x11),
  G('O',0x0E,0x11,0x11,0x11,0x0E),
  G('P',0x1E,0x11,0x1E,0x10,0x10),
  G('Q',0x0E,0x11,0x11,0x15,0x0E),
  G('R',0x1E,0x11,0x1E,0x12,0x11),
  G('S',0x0F,0x10,0x0E,0x01,0x1E),
  G('T',0x1F,0x04,0x04,0x04,0x04),
  G('U',0x11,0x11,0x11,0x11,0x0E),
  G('V',0x11,0x11,0x11,0x0A,0x04),
  G('W',0x11,0x11,0x15,0x1B,0x11),
  G('X',0x11,0x0A,0x04,0x0A,0x11),
  G('Y',0x11,0x0A,0x04,0x04,0x04),
  G('Z',0x1F,0x02,0x04,0x08,0x1F),
  G('-',0x00,0x00,0x1F,0x00,0x00),
  G('!',0x04,0x04,0x04,0x00,0x04),
  G('?',0x0E,0x11,0x02,0x00,0x04),
  G(':',0x00,0x04,0x00,0x04,0x00),
  G('.',0x00,0x00,0x00,0x00,0x04),
  G('@',0x0A,0x1F,0x1F,0x0E,0x04),
  G(200,0x0E,0x11,0x10,0x11,0x0E),
  G(201,0x0F,0x10,0x13,0x11,0x0F),
  G(202,0x1F,0x04,0x04,0x04,0x1F),
  G(203,0x0E,0x11,0x11,0x11,0x0E),
  G(204,0x0F,0x10,0x0E,0x01,0x1E),
  G(205,0x11,0x11,0x11,0x11,0x0E)
};

#undef G

uint8_t getGlyph(const char c, uint8_t out[5]) {
  char up = c;
  if (c == 'Ç' || c == 'ç') up = 200;
  else if (c == 'Ğ' || c == 'ğ') up = 201;
  else if (c == 'İ' || c == 'i') up = 202;
  else if (c == 'Ö' || c == 'ö') up = 203;
  else if (c == 'Ş' || c == 'ş') up = 204;
  else if (c == 'Ü' || c == 'ü') up = 205;
  else if (c == 'ı') up = 'I';
  else if (c >= 'a' && c <= 'z') up = c - 32;

  for (uint16_t i = 0; i < sizeof(FONT) / sizeof(FONT[0]); i++) {
    if ((uint8_t)pgm_read_byte(&FONT[i].ch) == (uint8_t)up) {
      for (uint8_t k = 0; k < 5; k++)
        out[k] = pgm_read_byte(&FONT[i].col[k]);
      return 1;
    }
  }
  for (uint8_t k = 0; k < 5; k++) out[k] = 0;
  return 0;
}

void drawChar(int x0, int y0, char c, CRGB color, bool mirrorY = false, bool mirrorX = true) {
  uint8_t g[5];
  getGlyph(c, g);
  for (uint8_t cx = 0; cx < 5; cx++) {
    for (uint8_t cy = 0; cy < 7; cy++) {
      if (g[cx] & (1 << cy)) {
        int x_draw = x0 + (mirrorX ? (6 - cy) : cy);
        int y_draw = y0 + (mirrorY ? (4 - cx) : cx);
        setXY(x_draw, y_draw, color);
      }
    }
  }
}

void drawString(int x, int y, const String& s, CRGB base) {
  const int CHAR_SPACING = 8;
  for (int16_t i = 0; i < s.length(); i++) {
    char ch = s[i];
    int x_pos = x + i * CHAR_SPACING;
    drawChar(x_pos, y, ch, base, true, true);
  }
}

uint32_t gLastSpawn = 0;
const uint16_t RIPPLE_PERIOD_MS = 1200;
const float RIPPLE_WIDTH = 0.9f;
const float RIPPLE_FADE = 0.035f;
const float RIPPLE_SPEED = 0.065f;

uint32_t gLightningNext = 0;
uint8_t gLightningPhase = 0;

struct LSeg { int x, y; };
LSeg lpath[64];
uint8_t lcount = 0;

void lightningSpawn() {
  lcount = 0;
  int x = random(LED_COLS);
  int y = 0;
  lpath[lcount++] = {x, y};
  while (y < (int)LED_ROWS - 1 && lcount < 63) {
    int dx = random(-1, 2);
    x = constrain(x + dx, 0, LED_COLS - 1);
    y++;
    lpath[lcount++] = {x, y};
    if (random(100) < 15 && lcount < 62 && y < LED_ROWS - 1) {
      int bx = x, by = y;
      int steps = min(2, (int)LED_ROWS - 1 - y);
      for (int i = 0; i < steps; i++) {
        int ddx = random(-1, 2);
        bx = constrain(bx + ddx, 0, LED_COLS - 1);
        by++;
        lpath[lcount++] = {bx, by};
      }
    }
  }
  gLightningPhase = 1;
  uint16_t delay_ms = map(gSpeed, 10, 200, 2200, 550);
  gLightningNext = millis() + random(delay_ms / 4, delay_ms);
}

struct Star { float x, y, vx, vy; uint8_t life; };
Star shoot = {-100, -100, 0, 0, 0};
uint32_t nextShoot = 0;

void spawnShoot() {
  shoot.x = random(-10, LED_COLS + 10);
  shoot.y = random(0, LED_ROWS - 1);

  if (random(2) == 0) {
    shoot.vx = -0.35f - (gSpeed - 10) / 700.0f;
    if (shoot.x < LED_COLS / 2) shoot.x = LED_COLS + random(5, 10);
  } else {
    shoot.vx = 0.35f + (gSpeed - 10) / 700.0f;
    if (shoot.x > LED_COLS / 2) shoot.x = -random(5, 10);
  }

  if (random(2) == 0)
    shoot.vy = 0.18f + (gSpeed - 10) / 1000.0f;
  else
    shoot.vy = -0.18f - (gSpeed - 10) / 1000.0f;

  shoot.life = 220;
}

uint8_t starPhase[LED_ROWS * LED_COLS];
uint32_t gFlashNext = 0;
uint8_t gFlashPhase = 0;
uint8_t flashX = 0, flashY = 0, flashW = 0, flashH = 0;

void fxColorBars(uint32_t) {
  for (uint8_t y = 0; y < LED_ROWS; y++)
    for (uint8_t x = 0; x < LED_COLS; x++)
      setXY(x, y, CHSV(gHue + x * 3 + y * 6, 200, 255));
}

void fxWaves(uint32_t t) {
  float cx = (LED_COLS - 1) / 2.0f, cy = (LED_ROWS - 1) / 2.0f;
  uint8_t speedShift;
  uint16_t sp = gSpeed;
  if (sp <= 10) speedShift = 3;
  else if (sp >= 200) speedShift = 0;
  else speedShift = map(sp, 10, 200, 3, 0);

  for (uint8_t y = 0; y < LED_ROWS; y++)
    for (uint8_t x = 0; x < LED_COLS; x++) {
      float dx = x - cx, dy = y - cy;
      float d = sqrtf(dx * dx + dy * dy);
      uint8_t val = sin8((uint16_t)(d * 28) - (t >> speedShift));
      setXY(x, y, CHSV(gHue + (uint8_t)(d * 6), 200, val));
    }
}

void fxRipple(uint32_t t) {
  static float baseR = -1000.0f;
  if (t - gLastSpawn > RIPPLE_PERIOD_MS || baseR < -100.0f) {
    gLastSpawn = t;
    baseR = 0.0f;
  }

  float R = baseR + (t - gLastSpawn) * RIPPLE_SPEED;
  float cx = (LED_COLS - 1) / 2.0f, cy = (LED_ROWS - 1) / 2.0f;

  for (uint8_t y = 0; y < LED_ROWS; y++)
    for (uint8_t x = 0; x < LED_COLS; x++) {
      float dx = x - cx, dy = y - cy;
      float d = sqrtf(dx * dx + dy * dy);
      float band = expf(-((d - R) * (d - R)) / (2.0f * RIPPLE_WIDTH * RIPPLE_WIDTH));
      float fade = expf(-R * RIPPLE_FADE);
      float vfloat = band * fade * 255.0f;
      uint8_t v = (uint8_t)constrain(vfloat, 0.0f, 255.0f);
      setXY(x, y, CHSV((uint8_t)(gHue + d * 6), 200, v));
    }
}

void fxLightning(uint32_t t) {
  if (t > gLightningNext && gLightningPhase == 0) {
    lightningSpawn();
  }

  for (uint8_t y = 0; y < LED_ROWS; y++)
    for (uint8_t x = 0; x < LED_COLS; x++)
      addXY(x, y, CRGB(1, 1, 2));

  if (gLightningPhase > 0 && gLightningPhase < 4 && lcount > 0) {
    CRGB c = (gLightningPhase == 1) ? CRGB(255, 255, 255) :
             (gLightningPhase == 2) ? CRGB(150, 180, 255) :
             CRGB(60, 90, 180);

    for (uint8_t i = 0; i < lcount; i++) {
      setXY(lpath[i].x, lpath[i].y, c);
      if (lpath[i].x + 1 < LED_COLS) addXY(lpath[i].x + 1, lpath[i].y, c / 4);
      if (lpath[i].x > 0) addXY(lpath[i].x - 1, lpath[i].y, c / 4);
    }
    gLightningPhase++;
  }

  if (gLightningPhase >= 4) gLightningPhase = 0;
}

void fxShootingStar(uint32_t t) {
  if (t > nextShoot && shoot.life == 0) {
    spawnShoot();
    uint16_t delay_ms = map(gSpeed, 10, 200, 3500, 1000);
    nextShoot = t + random(delay_ms / 3, delay_ms);
  }

  for (uint8_t y = 0; y < LED_ROWS; y++)
    for (uint8_t x = 0; x < LED_COLS; x++)
      addXY(x, y, CHSV(160, 200, 6));

  if (shoot.life) {
    for (int i = 0; i < 15; i++) {
      int px = (int)(shoot.x - i * shoot.vx * 1.5f);
      int py = (int)(shoot.y - i * shoot.vy * 1.5f);
      if (px >= 0 && py >= 0 && px < LED_COLS && py < LED_ROWS) {
        uint8_t v = clampu8(255 - i * 16);
        addXY(px, py, CRGB(v, v, v));
      }
    }
    setXY((int)shoot.x, (int)shoot.y, CRGB::White);
    shoot.x += shoot.vx;
    shoot.y += shoot.vy;
    if (shoot.x < -20 || shoot.x >= LED_COLS + 20 || shoot.y < -20 || shoot.y >= LED_ROWS + 20)
      shoot.life = 0;
    else
      shoot.life--;
  }
}

void fxGalaxy(uint32_t) {
  for (uint8_t y = 0; y < LED_ROWS; y++)
    for (uint8_t x = 0; x < LED_COLS; x++)
      addXY(x, y, CHSV(180, 200, 4));

  for (uint8_t y = 0; y < LED_ROWS; y++) {
    for (uint8_t x = 0; x < LED_COLS; x++) {
      uint16_t i = XYi(x, y);
      if (starPhase[i] == 0 && random(1000) < 3) starPhase[i] = random(1, 255);
      if (starPhase[i]) {
        uint8_t v = sin8(starPhase[i]);
        setXY(x, y, CHSV(0, 0, v));
        if (starPhase[i] > 2) starPhase[i] -= 2;
        else starPhase[i] = 0;
      }
    }
  }
}

void fxRainbow(uint32_t t) {
  uint8_t speedShift;
  uint16_t sp = gSpeed;
  if (sp <= 10) speedShift = 3;
  else if (sp >= 200) speedShift = 0;
  else speedShift = map(sp, 10, 200, 3, 0);

  for (uint8_t y = 0; y < LED_ROWS; y++)
    for (uint8_t x = 0; x < LED_COLS; x++) {
      uint8_t h = (x * 6 + (t >> speedShift)) & 0xFF;
      setXY(x, y, CHSV(h, 240, 255));
    }
}

void fxRegionalFlash(uint32_t t) {
  if (t > gFlashNext && gFlashPhase == 0) {
    flashW = random(5, 12);
    flashH = random(3, 7);
    flashX = random(0, LED_COLS - flashW);
    flashY = random(0, LED_ROWS - flashH);
    gFlashPhase = 1;
    uint16_t delay_ms = map(gSpeed, 10, 200, 1800, 450);
    gFlashNext = millis() + random(delay_ms / 4, delay_ms);
  }

  for (uint8_t y = 0; y < LED_ROWS; y++)
    for (uint8_t x = 0; x < LED_COLS; x++)
      addXY(x, y, CRGB(1, 1, 2));

  if (gFlashPhase > 0 && gFlashPhase < 5) {
    CRGB c = (gFlashPhase == 1) ? CRGB(255, 255, 255) :
             (gFlashPhase == 2) ? CRGB(150, 150, 150) :
             (gFlashPhase == 3) ? CRGB(80, 80, 80) :
             CRGB(30, 30, 30);

    for (uint8_t y = flashY; y < flashY + flashH && y < LED_ROWS; y++)
      for (uint8_t x = flashX; x < flashX + flashW && x < LED_COLS; x++)
        setXY(x, y, c);

    gFlashPhase++;
  }

  if (gFlashPhase >= 5) gFlashPhase = 0;
}

void fxMarquee(uint32_t t) {
  for (uint8_t y = 0; y < LED_ROWS; y++)
    for (uint8_t x = 0; x < LED_COLS; x++)
      addXY(x, y, CRGB(0, 0, 0));

  gTextHue += 1;
  CRGB c;
  hsv2rgb_rainbow(CHSV(gTextHue, 200, 255), c);

  int startY = (LED_ROWS - 5) / 2;
  drawString(gTextOffset, startY, gText, c);

  uint8_t speedMod;
  uint16_t ms = gMarqueeSpeed;

  if (ms <= 10) speedMod = 12;
  else if (ms >= 200) speedMod = 1;

  speedMod = map(gMarqueeSpeed, 1, 400, 20, 1);
  if (speedMod == 0) speedMod = 1;

  int w = gText.length() * 8 - 3;
  if ((t % speedMod) == 0) gTextOffset -= 1;
  if (gTextOffset < -w) gTextOffset = LED_COLS;
}

typedef void(*FxFn)(uint32_t);

FxFn effects[] = {
  fxColorBars,
  fxWaves,
  fxRipple,
  fxLightning,
  fxShootingStar,
  fxGalaxy,
  fxRainbow,
  fxRegionalFlash,
  fxMarquee
};

const uint8_t FX_COUNT = sizeof(effects) / sizeof(effects[0]);

WebServer server(80);

const char* AP_SSID = "MCT LED";
const char* AP_PASS = "192513";

String escapeJson(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    char c = in[i];
    if (c == '"' || c == '\\') {
      out += '\\';
    }
    out += c;
  }
  return out;
}

String jsonState() {
  String s = "{";

  s += "\"power\":";
  s += (gPower ? "true" : "false");
  s += ",";

  s += "\"brightness\":";
  s += String(gBrightness);
  s += ",";

  s += "\"speed\":";
  s += String(gSpeed);
  s += ",";

  s += "\"effect\":";
  s += String(gEffect);
  s += ",";

  s += "\"solid\":";
  s += (gSolidMode ? "true" : "false");
  s += ",";

  s += "\"paused\":";
  s += (gPaused ? "true" : "false");
  s += ",";

  s += "\"text\":\"";
  s += escapeJson(gText);
  s += "\",";

  s += "\"marqueespeed\":";
  s += String(gMarqueeSpeed);
  s += ",";

  s += "\"absence_ms\":";
  s += String(absenceOffMs);
  s += ",";

  s += "\"restore_ms\":";
  s += String(restoreWindowMs);
  s += ",";

  s += "\"pir\":";
  s += (digitalRead(PIR_PIN) == HIGH ? "true" : "false");
  s += ",";

  s += "\"pir_enabled\":";
  s += (gPirEnabled ? "true" : "false");
  s += ",";

  s += "\"tz_min\":";
  s += String(tzOffsetMin);
  s += ",";

  s += "\"pir_on_min\":";
  s += String(pirOnMin);
  s += ",";

  s += "\"pir_off_min\":";
  s += String(pirOffMin);
  s += ",";

  s += "\"epoch\":";
  s += String(nowUtcSec());

  s += "}";
  return s;
}

const char* OTA_PAGE = R"OTA(
<html>
  <head><meta charset="utf-8"><title>OTA Update</title></head>
  <body>
    <h2>OTA Firmware Update</h2>
    <form method="POST" action="/update" enctype="multipart/form-data">
      <input type="file" name="firmware">
      <input type="submit" value="Upload">
    </form>
  </body>
</html>
)OTA";

const char HTML_INDEX[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="tr"><head><meta charset="UTF-8" />
<title>LED Kontrol Paneli 💡</title>
<meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no" />
<style>
.effect-row {
  display: flex;
  align-items: center;
  gap: 12px;
}
.effect-label {
  font-size: 0.9rem;
  opacity: 0.85;
}
.effect-select {
  flex: 1;
  background: #0f172a;
  color: #e2e8f0;
  padding: 8px 12px;
  border-radius: 10px;
  border: 1px solid #1e293b;
  appearance: none;
  outline: none;
  font-size: 0.9rem;
}
.effect-select:focus {
  border-color: #3b82f6;
  box-shadow: 0 0 6px #3b82f6;
}
.effect-select option {
  background: #0f172a;
  color: #e2e8f0;
}
.text-row {
  display: flex;
  align-items: center;
  gap: 10px;
}
.text-row input {
  flex: 1;
}
.text-buttons {
  display: flex;
  flex-direction: row;
  gap: 8px;
}
.clr-white { background: #ffffff; color: #000; }
.clr-red   { background: #dc2626; color: #fff; }
.clr-green { background: #16a34a; color: #fff; }
.clr-blue  { background: #2563eb; color: #fff; }
body {
  background: #05060a;
  color: #f5f5f5;
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  margin: 0;
  padding: 0;
}
.container {
  max-width: 420px;
  margin: 0 auto;
  padding: 16px;
}
h1 {
  font-size: 1.3rem;
  margin-bottom: 12px;
  text-align: center;
}
.card {
  background: #101320;
  border-radius: 12px;
  padding: 12px 14px;
  margin-bottom: 12px;
  box-shadow: 0 8px 16px rgba(0,0,0,0.4);
}
.card h2 {
  font-size: 1rem;
  margin: 0 0 8px 0;
}
.row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 8px;
  flex-wrap: wrap;
  gap: 6px;
}
label {
  font-size: 0.85rem;
}
button, select, input[type="range"], input[type="time"],
input[type="number"], input[type="text"] {
  font-family: inherit;
  font-size: 0.86rem;
}
button {
  background: #2563eb;
  border: none;
  color: white;
  padding: 6px 10px;
  border-radius: 999px;
  cursor: pointer;
}
button.secondary { background: #1e293b; }
button.danger { background: #dc2626; }
button:active { transform: scale(0.97); }
.slider-row {
  display: flex;
  align-items: center;
  gap: 8px;
}
.slider-row span {
  font-size: 0.8rem;
  opacity: 0.8;
}
input[type="range"] { width: 100%; }
.badge {
  display: inline-block;
  padding: 2px 8px;
  border-radius: 999px;
  background: #1e293b;
  font-size: 0.7rem;
  margin-left: 4px;
}
.toggle-row {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}
.small {
  font-size: 0.75rem;
  opacity: 0.8;
}
input[type="time"],
input[type="number"],
input[type="text"] {
  background: #020617;
  border: 1px solid #1f2937;
  border-radius: 999px;
  padding: 4px 8px;
  color: #e5e7eb;
}
input[type="time"]:focus,
input[type="number"]:focus,
input[type="text"]:focus {
  outline: none;
  border-color: #3b82f6;
}
.grid-buttons {
  display: grid;
  grid-template-columns: repeat(2, minmax(0,1fr));
  gap: 6px;
  margin-top: 6px;
}
.grid-buttons button { width: 100%; }
.touchpad {
  margin-top: 8px;
  width: 100%;
  aspect-ratio: 16 / 9;
  background:
    radial-gradient(circle at 20% 20%, #38bdf8 0, transparent 60%),
    radial-gradient(circle at 80% 80%, #a855f7 0, transparent 60%),
    #020617;
  border-radius: 12px;
  position: relative;
  overflow: hidden;
  touch-action: none;
}
.touchpad::after {
  content: "Dokun & Sürükle";
  position: absolute;
  left: 50%;
  top: 50%;
  transform: translate(-50%,-50%);
  font-size: 0.75rem;
  opacity: 0.7;
}
.footer {
  margin-top: 16px;
  text-align: center;
  font-size: 0.7rem;
  opacity: 0.7;
}
.footer-buttons {
  margin-top: 8px;
  display: flex;
  justify-content: center;
  gap: 8px;
  flex-wrap: wrap;
}
@media (orientation: landscape) {
  .container {
    max-width: 900px;
    padding: 12px 32px;
  }
}
</style></head>
<body>
<div class="container">
  <div class="card">
    <h2>Genel<span id="statusBadge" class="badge">Bağlanıyor...</span></h2>
    <div class="row">
      <div class="toggle-row">
        <button id="btnPower">Güç</button>
        <button id="btnPir" class="secondary">Sensör:</button>
      </div>
      <div class="toggle-row" style="flex:1; justify-content:flex-end; gap:6px;">
        <span class="small">Saat:</span>
        <span id="devTime" class="badge">--:--</span>
        <span class="small">Hareket:</span>
        <span id="pirState" class="badge">-</span>
      </div>
    </div>
  </div>

  <div class="card">
    <h2>Efektler 🌈</h2>
    <div class="row effect-row">
      <label for="effect" class="effect-label">Efekt Seç:</label>
      <select id="effect" class="effect-select">
        <option value="0">Rengarenk</option>
        <option value="1">İlüzyon</option>
        <option value="2">Dalga</option>
        <option value="3">Yıldırım</option>
        <option value="4">Yıldız Kayması</option>
        <option value="5">Parıltı</option>
        <option value="6">Gökkuşağı</option>
        <option value="7">Flaş</option>
        <option value="8">I LOVE YOU NAZAN</option>
      </select>
    </div>
    <div class="grid-buttons">
      <button id="btnSolidWhite" class="clr-white">Beyaz</button>
      <button id="btnSolidRed"   class="clr-red">Kırmızı</button>
      <button id="btnSolidGreen" class="clr-green">Yeşil</button>
      <button id="btnSolidBlue"  class="clr-blue">Mavi</button>
      <button id="btnPlay" class="secondary">Durdur</button>
      <button id="btnNextFx" class="secondary">Efekte Geç</button>
    </div>
  </div>

  <div class="card">
    <h2>Parlaklık & Hız</h2>
    <div class="slider-row">
      <span>Parlaklık</span>
      <input id="brightness" type="range" min="0" max="200" value="140" />
      <span id="brightnessVal">70%</span>
    </div>
    <div class="slider-row">
      <span>Efekt Hızı</span>
      <input id="speed" type="range" min="1" max="10" value="5" />
      <span id="speedVal">Seviye 5/10</span>
    </div>
    <div class="slider-row">
      <span>Yazı Hızı</span>
      <input id="marquee" type="range" min="1" max="400" value="100" />
      <span id="marqueeVal">25%</span>
    </div>
  </div>

  <div class="card">
    <h2>Yazı Efekti ✍️</h2>
    <div class="row text-row">
      <input id="textMsg" type="text" placeholder="Yazıyı Giriniz..." />
      <div class="text-buttons">
        <button id="btnSendText">Gönder</button>
        <button id="btnResetOffset" class="secondary">Başa Sar</button>
      </div>
    </div>
  </div>

  <div class="card">
    <h2>Touchpad Efekti 🪄</h2>
    <div id="touchpad" class="touchpad"></div>
  </div>

  <div class="card">
    <h2>Sensör Zamanlama & Auto-Off</h2>
    <div class="row">
      <label for="autoMinutes">Kapanma Süresi (dk)</label>
      <input id="autoMinutes" type="number" min="1" max="10" value="1" />
    </div>
    <div class="row">
      <label for="onTime">Açılış Saati</label>
      <input id="onTime" type="time" />
    </div>
    <div class="row">
      <label for="offTime">Kapanış Saati</label>
      <input id="offTime" type="time" />
    </div>
    <div class="grid-buttons">
      <button id="btnSetSchedule">Kaydet</button>
      <button id="btnSyncTime" class="secondary">Saati Eşitle</button>
    </div>
  </div>

  <div class="footer">
    <div>• Designed and Programmed by Murat Can TUTAR •</div>
    <div class="footer-buttons">
      <button id="btnOta" class="secondary">Sistem Güncelleme</button>
    </div>
  </div>
</div>

<script>
const $ = (id) => document.getElementById(id);

function apiGet(path) {
  return fetch(path, { cache: "no-store" })
    .then(r => r.json())
    .catch(() => null);
}

function formatDeviceTime(epoch, tzMin) {
  if (!epoch) return "--:--";
  const localSec = epoch + (tzMin * 60);
  const d = new Date(localSec * 1000);
  const hh = d.getUTCHours().toString().padStart(2, "0");
  const mm = d.getUTCMinutes().toString().padStart(2, "0");
  return hh + ":" + mm;
}

function updateFromState(st) {
  if (!st) return;

  $("statusBadge").textContent = st.power ? "Açık" : "Kapalı";
  $("statusBadge").style.background = st.power ? "#16a34a" : "#1e293b";

  $("brightness").value = st.brightness;

  let speedLevel   = Math.round(st.speed / 40);
  let marqueeLevel = Math.round(st.marqueespeed / 40);

  if (speedLevel   < 1)  speedLevel   = 1;
  if (speedLevel   > 10) speedLevel   = 10;
  if (marqueeLevel < 1)  marqueeLevel = 1;
  if (marqueeLevel > 10) marqueeLevel = 10;

  $("speed").value   = speedLevel;
  $("marquee").value = marqueeLevel;

  const bPct = Math.round((st.brightness / 200) * 100);
  $("brightnessVal").textContent = bPct + "%";
  $("speedVal").textContent      = "Seviye " + speedLevel   + "/10";
  $("marqueeVal").textContent    = "Seviye " + marqueeLevel + "/10";

  $("effect").value = st.effect;

  $("pirState").textContent = st.pir ? "Hareket Var" : "Yok";
  $("pirState").style.background = st.pir ? "#f97316" : "#1e293b";

  $("btnPir").textContent = st.pir_enabled ? "Sensör: Açık" : "Sensör: Kapalı";
  $("btnPir").className = st.pir_enabled ? "secondary" : "danger";

  $("btnPlay").textContent = st.paused ? "Devam Et" : "Durdur";

  $("textMsg").placeholder = "Yazıyı Giriniz...";

  $("autoMinutes").value = Math.round(st.absence_ms / 60000);

  const devStr = formatDeviceTime(st.epoch, st.tz_min || 0);
  if ($("devTime")) $("devTime").textContent = devStr;
}

function refreshState() {
  apiGet("/api/state").then(updateFromState);
}

$("brightness").addEventListener("input", (e) => {
  const v = Number(e.target.value);
  $("brightnessVal").textContent = Math.round((v / 200) * 100) + "%";
});

$("brightness").addEventListener("change", (e) => {
  const v = e.target.value;
  fetch("/api/brightness?value=" + v);
});

$("speed").addEventListener("input", (e) => {
  const lvl = Number(e.target.value);
  $("speedVal").textContent = "Seviye " + lvl + "/10";
});

$("speed").addEventListener("change", (e) => {
  const lvl = Number(e.target.value);
  const mapped = Math.min(400, Math.max(1, Math.round(lvl * 40)));
  fetch("/api/speed?value=" + mapped);
});

$("marquee").addEventListener("input", (e) => {
  const lvl = Number(e.target.value);
  $("marqueeVal").textContent = "Seviye " + lvl + "/10";
});

$("marquee").addEventListener("change", (e) => {
  const lvl = Number(e.target.value);
  const mapped = Math.min(400, Math.max(1, Math.round(lvl * 40)));
  fetch("/api/marqueespeed?value=" + mapped);
});

$("btnPower").addEventListener("click", () => {
  apiGet("/api/state").then(st => {
    if (!st) return;
    const newVal = st.power ? 0 : 1;
    fetch("/api/power?value=" + newVal).then(() => setTimeout(refreshState, 200));
  });
});

$("btnPir").addEventListener("click", () => {
  apiGet("/api/state").then(st => {
    if (!st) return;
    const en = st.pir_enabled ? 0 : 1;
    fetch("/api/pir?enable=" + en).then(() => setTimeout(refreshState, 200));
  });
});

$("btnSolidWhite").addEventListener("click", () => fetch("/api/solid?color=white").then(refreshState));
$("btnSolidRed"  ).addEventListener("click", () => fetch("/api/solid?color=red"  ).then(refreshState));
$("btnSolidGreen").addEventListener("click", () => fetch("/api/solid?color=green").then(refreshState));
$("btnSolidBlue" ).addEventListener("click", () => fetch("/api/solid?color=blue" ).then(refreshState));

$("btnPlay").addEventListener("click", () => {
  apiGet("/api/state").then(st => {
    if (!st) return;
    const nv = st.paused ? 0 : 1;
    fetch("/api/play?value=" + nv).then(() => setTimeout(refreshState, 200));
  });
});

$("btnNextFx").addEventListener("click", () => {
  fetch("/api/solid?color=none").then(() => setTimeout(refreshState, 200));
});

$("btnSendText").addEventListener("click", () => {
  const msg = $("textMsg").value || "";
  const enc = encodeURIComponent(msg);
  fetch("/api/text?msg=" + enc).then(() => setTimeout(refreshState, 300));
});

$("btnResetOffset").addEventListener("click", () => {
  $("textMsg").value = "I LOVE YOU NAZAN";
  fetch("/api/resetoffset").then(() => setTimeout(refreshState, 200));
});

$("btnSetSchedule").addEventListener("click", () => {
  const onT = $("onTime").value;
  const offT = $("offTime").value;
  const absM = $("autoMinutes").value || "1";

  let qs = "/api/auto?abs=" + encodeURIComponent(absM);
  fetch(qs).then(() => {
    let q2 = "/api/pirschedule?";
    const parts = [];
    if (onT)  parts.push("on="  + encodeURIComponent(onT));
    if (offT) parts.push("off=" + encodeURIComponent(offT));
    q2 += parts.join("&");
    if (parts.length > 0) {
      fetch(q2).then(() => setTimeout(refreshState, 400));
    } else {
      setTimeout(refreshState, 200);
    }
  });
});

$("btnSyncTime").addEventListener("click", () => {
  const epoch = Math.floor(Date.now() / 1000);
  const tzmin = -new Date().getTimezoneOffset();
  const url = "/api/settime?epoch=" + epoch + "&tzmin=" + tzmin;
  fetch(url).then(() => setTimeout(refreshState, 300));
});

$("btnOta").addEventListener("click", () => {
  window.location.href = "/update";
});

(function(){
  const pad = $("touchpad");
  let active = false;

  function sendTouch(evt){
    const rect = pad.getBoundingClientRect();
    const x = (evt.touches ? evt.touches[0].clientX : evt.clientX) - rect.left;
    const y = (evt.touches ? evt.touches[0].clientY : evt.clientY) - rect.top;
    const nx = Math.min(Math.max(x / rect.width, 0), 1);
    const ny = Math.min(Math.max(y / rect.height, 0), 1);
    fetch("/api/touch?x=" + nx + "&y=" + ny);
  }

  pad.addEventListener("mousedown", (e) => {
    active = true;
    sendTouch(e);
  });

  pad.addEventListener("mousemove", (e) => {
    if (!active) return;
    sendTouch(e);
  });

  window.addEventListener("mouseup", () => active = false);

  pad.addEventListener("touchstart", (e) => {
    active = true;
    sendTouch(e);
    e.preventDefault();
  }, {passive:false});

  pad.addEventListener("touchmove", (e) => {
    if (!active) return;
    sendTouch(e);
    e.preventDefault();
  }, {passive:false});

  pad.addEventListener("touchend", () => {
    active = false;
  });
})();

refreshState();
setInterval(refreshState, 3000);
</script>
</body></html>
)HTML";

inline void sendJson200(const String& body) {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", body);
}

void handleIndex() {
  server.send_P(200, "text/html; charset=utf-8", HTML_INDEX);
}

void handleState() {
  sendJson200(jsonState());
}

void handleSet() {
  if (server.hasArg("effect")) {
    int v = server.arg("effect").toInt();
    if (v >= 0 && v < FX_COUNT) {
      gEffect = v;
      FastLED.clear(true);
    }
  }
  sendJson200(jsonState());
}

void handleBrightness() {
  if (server.hasArg("value")) {
    gFadeActive = false;
    gBrightness = constrain(server.arg("value").toInt(), 0, 200);
    FastLED.setBrightness(gBrightness);
    if (gBrightness > 0) gLastUserBrightness = gBrightness;
  }
  sendJson200(jsonState());
}

void handleSpeed() {
  if (server.hasArg("value")) {
    gSpeed = constrain(server.arg("value").toInt(), 1, 400);
  }
  sendJson200(jsonState());
}

void handleMarqueeSpeed() {
  if (server.hasArg("value")) {
    gMarqueeSpeed = constrain(server.arg("value").toInt(), 1, 400);
  }
  sendJson200(jsonState());
}

void handlePower() {
  if (server.hasArg("value")) {
    bool newPower = (server.arg("value").toInt() != 0);
    if (!newPower && gPower) {
      gPIRDelayStart = millis();
      gFadeActive = false;
      gAutoOffAtMs = 0;
      gPendingOff = OFF_MANUAL;
      gIgnorePirUntilMs = millis() + 2000UL;
      if (gBrightness > 0) gLastUserBrightness = gBrightness;
      startFadeTo(0, gBrightness);
    } else if (newPower && !gPower) {
      gPIRDelayStart = 0;
      gFadeActive = false;
      gLastMotionMs = millis();
      gAutoOffAtMs = 0;
      gPower = true;
      uint8_t target = (gLastUserBrightness > 0) ? gLastUserBrightness : 140;
      startFadeTo(target, 0);
    }
    gPower = newPower || (gPendingOff != OFF_NONE) || gPower;
  }
  sendJson200(jsonState());
}

void handleSolid() {
  if (server.hasArg("color")) {
    String c = server.arg("color");
    c.toLowerCase();
    if (c == "white" || c == "red" || c == "green" || c == "blue") {
      gSolidMode = true;
      if      (c == "white") gSolidColor = CRGB::White;
      else if (c == "red")   gSolidColor = CRGB::Red;
      else if (c == "green") gSolidColor = CRGB::Green;
      else if (c == "blue")  gSolidColor = CRGB::Blue;
      gFadeActive = false;
    } else if (c == "none") {
      gSolidMode = false;
      gFadeActive = false;
    }
  }
  sendJson200(jsonState());
}

void handlePlay() {
  if (server.hasArg("value")) {
    gPaused = (server.arg("value").toInt() != 0);
  }
  sendJson200(jsonState());
}

void handleText() {
  if (server.hasArg("msg")) {
    gText = server.arg("msg");
    gText.trim();
    if (gText.length() > 64) gText.remove(64);
    if (gText.length() == 0) gText = " ";
    gText.replace("❤", "LOVE");
    gText.replace("♥", "LOVE");
    gTextOffset = LED_COLS;
  }
  sendJson200(jsonState());
}

void handleTouch() {
  if (server.hasArg("x") && server.hasArg("y")) {
    float x = server.arg("x").toFloat();
    float y = server.arg("y").toFloat();
    if (x <= 1.0f && y <= 1.0f) {
      x *= (LED_COLS - 1);
      y *= (LED_ROWS - 1);
    }
    x = constrain(x, 0, LED_COLS - 1);
    y = constrain(y, 0, LED_ROWS - 1);
    spawnDot(x, y);
    gTouchBoostUntilMs = millis() + 400;
  }
  sendJson200(jsonState());
}

void handleResetOffset() {
  gText = "I LOVE YOU NAZAN";
  gTextOffset = LED_COLS;
  sendJson200(jsonState());
}

void handleAuto() {
  uint32_t newAbsMs = absenceOffMs;
  if (server.hasArg("abs_ms")) {
    newAbsMs = (uint32_t)server.arg("abs_ms").toInt();
  } else if (server.hasArg("abs")) {
    newAbsMs = (uint32_t)server.arg("abs").toInt() * 60000UL;
  }
  if (newAbsMs < 60000UL) newAbsMs = 60000UL;
  if (newAbsMs > 600000UL) newAbsMs = 600000UL;
  absenceOffMs = newAbsMs;
  savePersist();
  sendJson200(jsonState());
}

void handlePirEnable() {
  if (server.hasArg("enable")) {
    int v = server.arg("enable").toInt();
    gPirEnabled = (v != 0);
    if (!gPirEnabled) {
      gPIRDelayStart = 0;
      gAutoOffAtMs = 0;
      gLastMotionMs = millis();
    } else {
      gLastMotionMs = millis();
    }
  }
  sendJson200(jsonState());
}

void handleSetTime() {
  if (server.hasArg("epoch")) {
    deviceEpoch = (uint32_t)server.arg("epoch").toInt();
    deviceEpochSetAtMs = millis();
    EEPROM.put(EE_ADDR_RTC_EPOCH, deviceEpoch);
  }
  if (server.hasArg("tzmin")) {
    tzOffsetMin = server.arg("tzmin").toInt();
    EEPROM.put(EE_ADDR_RTC_TZMIN, tzOffsetMin);
  }
  EEPROM.commit();
  sendJson200(jsonState());
}

void handlePirSchedule() {
  if (server.hasArg("on")) {
    int16_t m = parseHHMM(server.arg("on"));
    pirOnMin = m;
  }
  if (server.hasArg("off")) {
    int16_t m = parseHHMM(server.arg("off"));
    pirOffMin = m;
  }
  sendJson200(jsonState());
}
void setup() {
  Serial.begin(115200);
  delay(50);

  EEPROM.begin(EEPROM_SIZE_BYTES);

  buildMapping();
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, physCount);
  FastLED.setBrightness(gBrightness);
  FastLED.clear(true);

  pinMode(PIR_PIN, INPUT_PULLDOWN);

  loadPersist();

  uint32_t eEpoch = 0;
  int32_t eTz = 180;
  EEPROM.get(EE_ADDR_RTC_EPOCH, eEpoch);
  EEPROM.get(EE_ADDR_RTC_TZMIN, eTz);

  if (eEpoch > 1700000000UL && eEpoch < 4102444800UL) {
    deviceEpoch = eEpoch;
  } else {
    deviceEpoch = 0;
  }

  tzOffsetMin = eTz;
  deviceEpochSetAtMs = millis();

  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("AP start: ");
  Serial.println(ok ? "Bağlantı Hazır... Keyifli Vakitler Murat Can Bey :)" : "Hatalı Durum");
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, handleIndex);
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/set", HTTP_GET, handleSet);
  server.on("/api/brightness", HTTP_GET, handleBrightness);
  server.on("/api/speed", HTTP_GET, handleSpeed);
  server.on("/api/marqueespeed", HTTP_GET, handleMarqueeSpeed);
  server.on("/api/power", HTTP_GET, handlePower);
  server.on("/api/solid", HTTP_GET, handleSolid);
  server.on("/api/play", HTTP_GET, handlePlay);
  server.on("/api/text", HTTP_GET, handleText);
  server.on("/api/touch", HTTP_GET, handleTouch);
  server.on("/api/resetoffset", HTTP_GET, handleResetOffset);
  server.on("/api/auto", HTTP_GET, handleAuto);
  server.on("/api/pir", HTTP_GET, handlePirEnable);
  server.on("/api/settime", HTTP_GET, handleSetTime);
  server.on("/api/pirschedule", HTTP_GET, handlePirSchedule);

  server.on("/update", HTTP_GET, []() {
    server.send(200, "text/html", OTA_PAGE);
  });

  server.on("/update", HTTP_POST,
    []() {
      server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
      delay(500);
      ESP.restart();
    },
    []() {
      HTTPUpload& upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) {
        Update.begin();
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        Update.write(upload.buf, upload.currentSize);
      } else if (upload.status == UPLOAD_FILE_END) {
        Update.end(true);
      }
    }
  );

  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });

  server.begin();

  randomSeed(esp_random());
  gLastMotionMs = millis();
}

void loop() {
  server.handleClient();
  updateDeviceTime();

  static uint32_t lastSchedCheckMs = 0;
  uint32_t msNow = millis();

  if (msNow - lastSchedCheckMs >= 1000) {
    lastSchedCheckMs = msNow;
    int32_t dayIdx;
    uint16_t minOfDay;
    if (localDayAndMinute(dayIdx, minOfDay)) {
      if (dayIdx != schedDayIndex) {
        schedDayIndex = dayIdx;
        appliedOnToday = false;
        appliedOffToday = false;
      }

      if (pirOffMin >= 0 && minOfDay == (uint16_t)pirOffMin && !appliedOffToday) {
        gPirEnabled = false;
        appliedOffToday = true;
        Serial.println("[SCHEDULE] PIR OFF tek-atım uygulandı");
      }

      if (pirOnMin >= 0 && minOfDay == (uint16_t)pirOnMin && !appliedOnToday) {
        gPirEnabled = true;
        appliedOnToday = true;
        gLastMotionMs = millis();
        Serial.println("[SCHEDULE] PIR ON tek-atım uygulandı");
      }
    }
  }

  if (!gPower) {
    uint32_t currentTime = millis();

    if (gAutoOffAtMs != 0) {
      if (pirIsHighStable(currentTime)) {
        uint32_t sinceOff = currentTime - gAutoOffAtMs;
        if (sinceOff <= restoreWindowMs && gHasSavedState) {
          restoreSavedStateWithFade();
          gAutoOffAtMs = 0;
          gPIRDelayStart = 0;
          gLastMotionMs = currentTime;
        } else {
          gPower = true;
          gSolidMode = true;
          gSolidColor = CRGB::White;
          startFadeTo(PIR_FADE_TARGET_BRIGHTNESS, 0);
          gAutoOffAtMs = 0;
          gPIRDelayStart = 0;
          gLastMotionMs = currentTime;
        }
      }
      delay(10);
      return;
    }

    if (gPIRDelayStart != 0 && (currentTime - gPIRDelayStart) >= PIR_DELAY_MS) {
      if (pirIsHighStable(currentTime)) {
        gPower = true;
        gSolidMode = true;
        gSolidColor = CRGB::White;
        startFadeTo(PIR_FADE_TARGET_BRIGHTNESS, 0);
        gPIRDelayStart = 0;
        gLastMotionMs = currentTime;
      }
    }

    delay(10);
    return;
  }

  uint32_t nowMs = millis();

  if (nowMs - gLastPirSampleMs >= PIR_DEBOUNCE_MS) {
    gLastPirSampleMs = nowMs;
    if (pirIsHighStable(nowMs)) {
      gLastMotionMs = nowMs;
    }
  }

  if (gPirEnabled && absenceOffMs > 0 && (nowMs - gLastMotionMs) > absenceOffMs && gPendingOff == OFF_NONE) {
    saveCurrentState();
    gAutoOffAtMs = nowMs;
    gPendingOff = OFF_AUTO;
    gIgnorePirUntilMs = nowMs + 2000UL;
    startFadeTo(0, gBrightness);
    return;
  }

  if (gFadeActive) {
    uint32_t tnow = millis();
    if (tnow - gLastFadeStepMs >= 5) {
      gLastFadeStepMs = tnow;
      if (gBrightness < gFadeTarget) {
        gBrightness++;
        FastLED.setBrightness(gBrightness);
      } else if (gBrightness > gFadeTarget) {
        gBrightness--;
        FastLED.setBrightness(gBrightness);
      } else {
        gFadeActive = false;
        FastLED.setBrightness(gFadeTarget);
        if (gFadeTarget > 0) gLastUserBrightness = gFadeTarget;
        if (gFadeTarget == 0 && gPendingOff != OFF_NONE) {
          FastLED.clear(true);
          FastLED.show();
          gPower = false;
          gPendingOff = OFF_NONE;
          return;
        }
      }
    }
  }

  FastLED.setBrightness(gBrightness);
  uint32_t t = millis();
  bool touchBoost = (millis() < gTouchBoostUntilMs);

  if (!gPaused) {
    FastLED.clear();
    if (gSolidMode) {
      for (uint8_t y = 0; y < LED_ROWS; y++)
        for (uint8_t x = 0; x < LED_COLS; x++)
          setXY(x, y, gSolidColor);
    } else {
      if (touchBoost) {
        for (uint8_t y = 0; y < LED_ROWS; y++)
          for (uint8_t x = 0; x < LED_COLS; x++)
            addXY(x, y, CHSV(gHue, 40, 3));
      } else {
        effects[gEffect % FX_COUNT](t);
        gHue++;
      }
    }
  }

  for (uint8_t i = 0; i < MAX_DOTS; i++) {
    if (dots[i].life > 0) {
      setXY((uint8_t)dots[i].x, (uint8_t)dots[i].y, CHSV(gHue + i * 3, 180, (uint8_t)(dots[i].life * 255)));
      dots[i].life -= 0.03f;
      if (dots[i].life < 0.0f) dots[i].life = 0.0f;
    }
  }

  FastLED.show();

  static uint32_t last = 0;
  uint16_t d = frameDelayMs();
  if (touchBoost) d = max<uint16_t>(2, d / 3);
  uint32_t e = millis() - last;
  if (e < d) delay(d - e);
  last = millis();
}
