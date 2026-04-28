#include <Arduino.h>
#include <M5StickCPlus2.h>
#include <NimBLEDevice.h>
#include <mbedtls/base64.h>
#include <math.h>

// ----------------------------------------------------------------------------
// CONSTANTS
// ----------------------------------------------------------------------------

static constexpr const char* DEVICE_NAME = "M5VoiceStick";
static constexpr const char* FIRMWARE_VERSION = "v0.3";

static const BLEUUID SERVICE_UUID("3e7a0001-e33b-4e2f-9a85-f03e1d33c001");
static const BLEUUID AUDIO_UUID("3e7a0002-e33b-4e2f-9a85-f03e1d33c001");
static const BLEUUID CONTROL_UUID("3e7a0003-e33b-4e2f-9a85-f03e1d33c001");

static constexpr uint32_t SAMPLE_RATE = 16000;
static constexpr size_t SAMPLES_PER_CHUNK = 240;
static constexpr size_t BLE_PAYLOAD_BYTES = 180;

static constexpr size_t HISTORY_SIZE = 4;
static constexpr uint32_t OVERLAY_AUTO_DISMISS_MS = 6500;
static constexpr uint32_t REDRAW_INTERVAL_MS = 70;

static constexpr uint8_t BRIGHTNESS_LEVELS[] = {40, 90, 140, 200};
static constexpr size_t BRIGHTNESS_COUNT = sizeof(BRIGHTNESS_LEVELS) / sizeof(BRIGHTNESS_LEVELS[0]);

static constexpr uint16_t COLOR_BG = 0x0000;
static constexpr uint16_t COLOR_FG = 0xEF3B;
static constexpr uint16_t COLOR_FG_DIM = 0xBDB6;
static constexpr uint16_t COLOR_MUTED = 0x7C0F;
static constexpr uint16_t COLOR_HAIRLINE = 0x39E7;
static constexpr uint16_t COLOR_DANGER = 0xFAA7;
static constexpr uint16_t COLOR_OK = 0x37D5;
static constexpr uint16_t COLOR_WARN = 0xFD20;

// ----------------------------------------------------------------------------
// MOOD CATALOG
// ----------------------------------------------------------------------------

enum class Mood : uint8_t {
  Pulse,
  Bloom,
  Drift,
  Static_,
  Storm,
  Orbit,
  Grid,
  Prism,
  Count,
};

struct MoodSpec {
  const char* name;
  uint16_t primary;
  uint16_t secondary;
};

// One palette per mood. Names are short and recognizable from across a room.
static const MoodSpec MOODS[static_cast<size_t>(Mood::Count)] = {
  { "PULSE",  0xFC06, 0x6981 },  // warm coral, expanding rings
  { "BLOOM",  0xFA94, 0x802A },  // hot pink, beating petals
  { "DRIFT",  0x551F, 0x1ACB },  // soft cyan, calm waves
  { "STATIC", 0xFFFF, 0x4208 },  // white noise, anxious
  { "STORM",  0xF986, 0x6800 },  // red, jagged lightning
  { "ORBIT",  0xA21F, 0x310E },  // violet, dots circling
  { "GRID",   0xB7E8, 0x4321 },  // lime, neutral focus
  { "PRISM",  0xFE88, 0xC325 },  // gold, rotating shapes
};

static Mood currentMood = Mood::Grid;

// ----------------------------------------------------------------------------
// STATE
// ----------------------------------------------------------------------------

enum class AppState : uint8_t {
  Standby,
  Ready,
  Recording,
  Uploading,
  Transcript,
  Fault,
};

enum class Overlay : uint8_t {
  None,
  Info,
  Transcript,
  Fault,
};

struct TranscriptEntry {
  String text;
  uint32_t arrivedAtMs = 0;
  bool used = false;
};

static AppState appState = AppState::Standby;
static Overlay overlay = Overlay::None;
static uint32_t overlayUntilMs = 0;
static String currentTranscript;
static String currentFault;
static String phoneStateLabel = "ready";

static TranscriptEntry history[HISTORY_SIZE];
static size_t historyHead = 0;
static size_t historyCount = 0;

static bool phoneConnected = false;
static bool streaming = false;
static bool recordingLock = false;

static uint32_t recordingStartMs = 0;
static uint32_t lastRecordingDurationMs = 0;
static uint32_t bootMs = 0;
static uint32_t lastDrawMs = 0;
static uint32_t frameCounter = 0;
static uint8_t brightnessIndex = 2;

static volatile int32_t audioPeakRaw = 0;   // updated by record loop
static uint16_t audioLevel = 0;             // smoothed 0..255 for draw

static int16_t audioBuffer[SAMPLES_PER_CHUNK];
static NimBLECharacteristic* audioCharacteristic = nullptr;
static NimBLECharacteristic* controlCharacteristic = nullptr;

static String controlRxBuffer;
static portMUX_TYPE controlMux = portMUX_INITIALIZER_UNLOCKED;
static String pendingControlLine;
static volatile bool hasPendingControlLine = false;

// Off-screen sprite for the art region (smooth animation, no flicker).
static M5Canvas artCanvas(&StickCP2.Display);
static constexpr int16_t ART_SIZE = 135;
static constexpr int16_t ART_X = 0;
static constexpr int16_t ART_Y = 50;

// ----------------------------------------------------------------------------
// HELPERS
// ----------------------------------------------------------------------------

static int16_t screenW() { return StickCP2.Display.width(); }
static int16_t screenH() { return StickCP2.Display.height(); }

static const MoodSpec& mood() { return MOODS[static_cast<size_t>(currentMood)]; }

static const char* stateLabel(AppState s) {
  switch (s) {
    case AppState::Standby:    return "OFFLINE";
    case AppState::Ready:      return "READY";
    case AppState::Recording:  return recordingLock ? "REC LOCK" : "REC";
    case AppState::Uploading:  return "UPLINK";
    case AppState::Transcript: return "OK";
    case AppState::Fault:      return "FAULT";
  }
  return "?";
}

static uint16_t stateColor(AppState s) {
  switch (s) {
    case AppState::Recording:  return COLOR_DANGER;
    case AppState::Fault:      return COLOR_DANGER;
    case AppState::Uploading:  return COLOR_WARN;
    case AppState::Transcript: return COLOR_OK;
    case AppState::Ready:      return COLOR_FG_DIM;
    default:                   return COLOR_MUTED;
  }
}

// RGB565 lerp toward black, used for fade trails.
static uint16_t fadeColor(uint16_t c, uint8_t scale) {
  uint16_t r = (c >> 11) & 0x1F;
  uint16_t g = (c >> 5) & 0x3F;
  uint16_t b = c & 0x1F;
  r = (r * scale) >> 8;
  g = (g * scale) >> 8;
  b = (b * scale) >> 8;
  return (r << 11) | (g << 5) | b;
}

static void requestRedraw() { /* no-op; redraw is throttled by frame timer */ }

static void setBrightnessIndex(uint8_t i) {
  brightnessIndex = i % BRIGHTNESS_COUNT;
  StickCP2.Display.setBrightness(BRIGHTNESS_LEVELS[brightnessIndex]);
}

static void cycleBrightness() {
  setBrightnessIndex((brightnessIndex + 1) % BRIGHTNESS_COUNT);
}

static void transition(AppState next) {
  if (appState == next) return;
  Serial.printf("[state] %s -> %s\n", stateLabel(appState), stateLabel(next));
  appState = next;
}

static void showOverlay(Overlay o, uint32_t durationMs) {
  overlay = o;
  overlayUntilMs = millis() + durationMs;
}

static void clearOverlay() {
  overlay = Overlay::None;
  overlayUntilMs = 0;
}

static void notifyControl(const char* message);

static void setMood(Mood m, bool broadcast) {
  if (m == currentMood) return;
  currentMood = m;
  Serial.printf("[mood] -> %s\n", mood().name);
  if (broadcast) {
    char buf[32];
    snprintf(buf, sizeof(buf), "MOOD:%s\n", mood().name);
    notifyControl(buf);
  }
}

static void cycleMood() {
  uint8_t next = (static_cast<uint8_t>(currentMood) + 1) % static_cast<uint8_t>(Mood::Count);
  setMood(static_cast<Mood>(next), true);
}

static bool moodFromName(const String& name, Mood& out) {
  for (size_t i = 0; i < static_cast<size_t>(Mood::Count); ++i) {
    if (name.equalsIgnoreCase(MOODS[i].name)) {
      out = static_cast<Mood>(i);
      return true;
    }
  }
  return false;
}

static void pushHistory(const String& text) {
  history[historyHead].text = text;
  history[historyHead].arrivedAtMs = millis();
  history[historyHead].used = true;
  historyHead = (historyHead + 1) % HISTORY_SIZE;
  if (historyCount < HISTORY_SIZE) historyCount++;
}

static String formatDuration(uint32_t ms) {
  uint32_t totalTenths = ms / 100;
  uint32_t seconds = totalTenths / 10;
  uint32_t tenths = totalTenths % 10;
  uint32_t mm = seconds / 60;
  uint32_t ss = seconds % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02lu:%02lu.%lu",
           static_cast<unsigned long>(mm),
           static_cast<unsigned long>(ss),
           static_cast<unsigned long>(tenths));
  return String(buf);
}

static String formatUptime(uint32_t ms) {
  uint32_t s = ms / 1000;
  uint32_t hh = s / 3600;
  uint32_t mm = (s % 3600) / 60;
  uint32_t ss = s % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
           static_cast<unsigned long>(hh),
           static_cast<unsigned long>(mm),
           static_cast<unsigned long>(ss));
  return String(buf);
}

// ----------------------------------------------------------------------------
// AUDIO LEVEL — peak amplitude smoothed for visual reactivity
// ----------------------------------------------------------------------------

static void updateAudioLevel() {
  uint16_t target = 0;

  if (streaming) {
    int32_t peak = audioPeakRaw;
    if (peak < 0) peak = 0;
    int32_t scaled = peak / 96;             // ~32767/96 ≈ 340
    if (scaled > 255) scaled = 255;
    target = static_cast<uint16_t>(scaled);
  } else {
    // Idle breathing — soft sine so the pendant looks alive.
    float t = frameCounter * 0.045f;
    float breath = (sinf(t) + 1.0f) * 0.5f;
    target = static_cast<uint16_t>(40.0f + breath * 80.0f);
  }

  // Asymmetric smoothing: rise fast, fall slow.
  if (target > audioLevel) {
    audioLevel = (audioLevel * 1 + target * 3) / 4;
  } else {
    audioLevel = (audioLevel * 6 + target * 1) / 7;
  }
}

// ----------------------------------------------------------------------------
// MOOD ART — each renders into the 135x135 art sprite
// ----------------------------------------------------------------------------

static void drawMoodPulse(uint32_t frame, uint16_t level) {
  artCanvas.fillSprite(COLOR_BG);
  const int16_t cx = ART_SIZE / 2;
  const int16_t cy = ART_SIZE / 2;
  uint16_t primary = mood().primary;
  uint16_t secondary = mood().secondary;

  // Expanding rings drift outward forever.
  for (int i = 0; i < 4; ++i) {
    int16_t r = ((frame / 2) + i * 16) % 64;
    uint8_t scale = 240 - (r * 3);
    uint16_t color = fadeColor(primary, scale);
    artCanvas.drawCircle(cx, cy, r, color);
    if (r > 2) artCanvas.drawCircle(cx, cy, r - 1, fadeColor(secondary, scale));
  }

  int16_t innerR = 7 + (level * 22) / 255;
  artCanvas.fillCircle(cx, cy, innerR, primary);
  artCanvas.fillCircle(cx, cy, innerR / 2, COLOR_FG);
}

static void drawMoodBloom(uint32_t frame, uint16_t level) {
  artCanvas.fillSprite(COLOR_BG);
  const int16_t cx = ART_SIZE / 2;
  const int16_t cy = ART_SIZE / 2;
  uint16_t primary = mood().primary;
  uint16_t secondary = mood().secondary;

  float beat = sinf(frame * 0.12f) * 0.5f + 0.5f;
  float reach = 22.0f + beat * 18.0f + level * 0.08f;
  float angleOffset = frame * 0.018f;

  for (int i = 0; i < 6; ++i) {
    float a = angleOffset + i * (PI / 3.0f);
    int16_t px = cx + cosf(a) * reach;
    int16_t py = cy + sinf(a) * reach;
    artCanvas.fillCircle(px, py, 12, secondary);
    artCanvas.fillCircle(px, py, 7, primary);
  }

  int16_t coreR = 6 + (level * 8) / 255;
  artCanvas.fillCircle(cx, cy, coreR + 4, secondary);
  artCanvas.fillCircle(cx, cy, coreR, primary);
  artCanvas.fillCircle(cx, cy, coreR / 2, COLOR_FG);
}

static void drawMoodDrift(uint32_t frame, uint16_t level) {
  artCanvas.fillSprite(COLOR_BG);
  uint16_t primary = mood().primary;
  uint16_t secondary = mood().secondary;
  float amp = 6.0f + level * 0.05f;
  float phase = frame * 0.08f;

  for (int wave = 0; wave < 3; ++wave) {
    int baseY = 30 + wave * 38;
    uint16_t color = wave == 1 ? primary : secondary;
    int16_t prevY = baseY;
    for (int x = 0; x < ART_SIZE; ++x) {
      float t = (x * 0.09f) + phase + wave * 1.7f;
      int16_t y = baseY + sinf(t) * amp + cosf(t * 0.5f) * (amp * 0.4f);
      artCanvas.drawLine(x - 1, prevY, x, y, color);
      if (wave == 1) artCanvas.drawPixel(x, y + 1, fadeColor(color, 90));
      prevY = y;
    }
  }
}

static void drawMoodStatic(uint32_t frame, uint16_t level) {
  artCanvas.fillSprite(COLOR_BG);
  uint16_t primary = mood().primary;
  uint16_t secondary = mood().secondary;
  uint32_t seed = frame * 2654435761u;

  // Pseudo-random noise — densest near top.
  int density = 80 + (level / 2);
  for (int i = 0; i < density; ++i) {
    seed = seed * 1103515245u + 12345u;
    int16_t x = seed % ART_SIZE;
    seed = seed * 1103515245u + 12345u;
    int16_t y = seed % ART_SIZE;
    seed = seed * 1103515245u + 12345u;
    uint16_t color = (seed & 7) == 0 ? primary : secondary;
    artCanvas.drawPixel(x, y, color);
  }

  // Scanlines that drift downward.
  for (int row = 0; row < 4; ++row) {
    int16_t y = ((frame * 2) + row * 36) % ART_SIZE;
    artCanvas.drawFastHLine(0, y, ART_SIZE, fadeColor(primary, 110));
  }

  // Center hairline cross to anchor the chaos.
  artCanvas.drawFastHLine(ART_SIZE / 2 - 12, ART_SIZE / 2, 24, fadeColor(primary, 200));
  artCanvas.drawFastVLine(ART_SIZE / 2, ART_SIZE / 2 - 12, 24, fadeColor(primary, 200));
}

static void drawMoodStorm(uint32_t frame, uint16_t level) {
  artCanvas.fillSprite(COLOR_BG);
  uint16_t primary = mood().primary;
  uint16_t secondary = mood().secondary;

  // Lightning bolts — three jagged paths refreshed in sequence.
  uint32_t seed = (frame / 3) * 2654435761u;
  int boltCount = 2 + (level / 80);
  for (int b = 0; b < boltCount; ++b) {
    seed = seed * 1664525u + 1013904223u;
    int16_t x = (seed % (ART_SIZE - 30)) + 15;
    int16_t y = 4;
    uint16_t color = (b == 0) ? primary : fadeColor(primary, 160);
    while (y < ART_SIZE - 4) {
      seed = seed * 1664525u + 1013904223u;
      int16_t dx = ((seed >> 4) & 0xF) - 7;
      int16_t dy = 6 + ((seed >> 8) & 0x7);
      int16_t nx = x + dx;
      int16_t ny = y + dy;
      artCanvas.drawLine(x, y, nx, ny, color);
      if (b == 0) artCanvas.drawLine(x + 1, y, nx + 1, ny, fadeColor(secondary, 180));
      x = nx;
      y = ny;
    }
  }

  // Top "cloud" sweeping flicker.
  if ((frame / 4) & 1) {
    artCanvas.fillRect(0, 0, ART_SIZE, 6, fadeColor(secondary, 80));
  }
}

static void drawMoodOrbit(uint32_t frame, uint16_t level) {
  artCanvas.fillSprite(COLOR_BG);
  const int16_t cx = ART_SIZE / 2;
  const int16_t cy = ART_SIZE / 2;
  uint16_t primary = mood().primary;
  uint16_t secondary = mood().secondary;

  // Three concentric orbital paths with different speeds.
  for (int ring = 0; ring < 3; ++ring) {
    int16_t radius = 18 + ring * 18;
    artCanvas.drawCircle(cx, cy, radius, fadeColor(secondary, 110));
    int dotCount = 1 + ring;
    float speed = 0.025f + ring * 0.012f;
    if (ring & 1) speed = -speed;
    for (int i = 0; i < dotCount; ++i) {
      float a = frame * speed + i * (2.0f * PI / dotCount);
      int16_t px = cx + cosf(a) * radius;
      int16_t py = cy + sinf(a) * radius;
      uint16_t color = (ring == 1) ? primary : secondary;
      artCanvas.fillCircle(px, py, 4 + ring, fadeColor(color, 200));
      artCanvas.fillCircle(px, py, 2 + ring, primary);
    }
  }

  int16_t coreR = 4 + (level * 5) / 255;
  artCanvas.fillCircle(cx, cy, coreR, primary);
}

static void drawMoodGrid(uint32_t frame, uint16_t level) {
  artCanvas.fillSprite(COLOR_BG);
  uint16_t primary = mood().primary;
  uint16_t secondary = mood().secondary;
  const int16_t cell = 15;
  const int16_t cols = ART_SIZE / cell;
  const int16_t rows = ART_SIZE / cell;
  const int16_t offsetX = (ART_SIZE - cols * cell) / 2;
  const int16_t offsetY = (ART_SIZE - rows * cell) / 2;

  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      float dx = c - cols / 2.0f + 0.5f;
      float dy = r - rows / 2.0f + 0.5f;
      float dist = sqrtf(dx * dx + dy * dy);
      float wave = sinf(dist * 0.9f - frame * 0.13f);
      uint8_t intensity = static_cast<uint8_t>(constrain(80 + (wave + 1.0f) * 80, 0, 255));
      intensity = (intensity * (180 + level / 4)) / 255;
      uint16_t color = (((r + c) & 1) == 0) ? primary : secondary;
      int16_t x = offsetX + c * cell;
      int16_t y = offsetY + r * cell;
      int16_t s = 3 + (intensity / 32);
      artCanvas.fillRect(x + (cell - s) / 2, y + (cell - s) / 2, s, s, fadeColor(color, intensity));
    }
  }
}

static void drawMoodPrism(uint32_t frame, uint16_t level) {
  artCanvas.fillSprite(COLOR_BG);
  const int16_t cx = ART_SIZE / 2;
  const int16_t cy = ART_SIZE / 2;
  uint16_t primary = mood().primary;
  uint16_t secondary = mood().secondary;

  float spin = frame * 0.04f;
  int16_t reach = 38 + (level * 12) / 255;

  // Three triangles, staggered rotation, alternating colors.
  for (int t = 0; t < 3; ++t) {
    float a = spin + t * (2.0f * PI / 3.0f);
    int16_t x1 = cx + cosf(a) * reach;
    int16_t y1 = cy + sinf(a) * reach;
    int16_t x2 = cx + cosf(a + 2.094f) * reach;
    int16_t y2 = cy + sinf(a + 2.094f) * reach;
    int16_t x3 = cx + cosf(a + 4.188f) * reach;
    int16_t y3 = cy + sinf(a + 4.188f) * reach;
    uint16_t color = (t == 0) ? primary : (t == 1) ? secondary : 0xC744;
    artCanvas.drawTriangle(x1, y1, x2, y2, x3, y3, color);
  }

  // Inner counter-rotating polygon.
  float spin2 = -spin * 1.3f;
  int16_t reach2 = 16 + (level * 6) / 255;
  for (int i = 0; i < 6; ++i) {
    float a1 = spin2 + i * (PI / 3.0f);
    float a2 = spin2 + (i + 1) * (PI / 3.0f);
    artCanvas.drawLine(
      cx + cosf(a1) * reach2, cy + sinf(a1) * reach2,
      cx + cosf(a2) * reach2, cy + sinf(a2) * reach2,
      primary);
  }

  artCanvas.fillCircle(cx, cy, 3, COLOR_FG);
}

using MoodDrawFn = void (*)(uint32_t, uint16_t);
static const MoodDrawFn MOOD_DRAW[static_cast<size_t>(Mood::Count)] = {
  drawMoodPulse,
  drawMoodBloom,
  drawMoodDrift,
  drawMoodStatic,
  drawMoodStorm,
  drawMoodOrbit,
  drawMoodGrid,
  drawMoodPrism,
};

// ----------------------------------------------------------------------------
// CHROME — top status row, mood label, audio meter, bottom state line
// ----------------------------------------------------------------------------

static void drawTopChrome() {
  // Tiny mark + link strength bars, fits a 135-wide screen.
  StickCP2.Display.setTextDatum(top_left);
  StickCP2.Display.setFont(&fonts::Font0);
  StickCP2.Display.setTextSize(1);
  StickCP2.Display.setTextColor(mood().primary, COLOR_BG);
  StickCP2.Display.drawString("M5VS", 6, 4);

  // Connection bars on the right.
  const int16_t barsX = screenW() - 26;
  const int16_t bandY = 12;
  const uint16_t onColor = phoneConnected ? mood().primary : COLOR_HAIRLINE;
  int activeBars = phoneConnected ? 4 : streaming ? 2 : 1;
  for (int i = 0; i < 4; ++i) {
    int16_t height = 2 + i * 2;
    int16_t x = barsX + i * 5;
    int16_t y = bandY - height;
    StickCP2.Display.fillRect(x, y, 3, height, i < activeBars ? onColor : COLOR_HAIRLINE);
  }

  StickCP2.Display.drawFastHLine(6, 18, screenW() - 12, COLOR_HAIRLINE);

  // Mood name in display font.
  StickCP2.Display.setFont(&fonts::FreeMonoBold12pt7b);
  StickCP2.Display.setTextColor(mood().primary, COLOR_BG);
  StickCP2.Display.drawString(mood().name, 6, 26);
}

static void drawBottomChrome() {
  // Audio level bar — only when no overlay is taking the lower region.
  if (overlay == Overlay::None) {
    const int16_t meterY = ART_Y + ART_SIZE + 4;
    const int16_t meterW = screenW() - 12;
    const int16_t meterH = 4;
    StickCP2.Display.fillRect(6, meterY, meterW, meterH, COLOR_HAIRLINE);
    int16_t fill = (meterW * audioLevel) / 255;
    uint16_t meterColor = streaming ? COLOR_DANGER : mood().primary;
    if (fill > 0) StickCP2.Display.fillRect(6, meterY, fill, meterH, meterColor);
  }

  // Bottom state line.
  StickCP2.Display.setFont(&fonts::Font0);
  StickCP2.Display.setTextSize(1);

  StickCP2.Display.setTextDatum(top_left);
  StickCP2.Display.setTextColor(stateColor(appState), COLOR_BG);
  StickCP2.Display.drawString(stateLabel(appState), 6, screenH() - 12);

  StickCP2.Display.setTextDatum(top_right);
  StickCP2.Display.setTextColor(COLOR_MUTED, COLOR_BG);
  char idx[16];
  snprintf(idx, sizeof(idx), "%u/%u",
           static_cast<unsigned>(static_cast<uint8_t>(currentMood) + 1),
           static_cast<unsigned>(Mood::Count));
  StickCP2.Display.drawString(idx, screenW() - 6, screenH() - 12);
}

static void drawArtRegion() {
  MoodDrawFn fn = MOOD_DRAW[static_cast<size_t>(currentMood)];
  fn(frameCounter, audioLevel);

  // Recording indicator overlaid on the art top-right.
  if (streaming) {
    bool blink = (frameCounter / 4) & 1;
    if (blink) artCanvas.fillCircle(ART_SIZE - 10, 10, 4, COLOR_DANGER);
    artCanvas.drawCircle(ART_SIZE - 10, 10, 4, COLOR_DANGER);

    // Recording timer, top-left of art.
    artCanvas.setTextDatum(top_left);
    artCanvas.setFont(&fonts::Font0);
    artCanvas.setTextColor(COLOR_FG, COLOR_BG);
    String dur = formatDuration(millis() - recordingStartMs);
    artCanvas.drawString(dur, 6, 6);
  }

  artCanvas.pushSprite(ART_X, ART_Y);
}

// ----------------------------------------------------------------------------
// OVERLAYS — transcript, fault, info take over the art area
// ----------------------------------------------------------------------------

static void drawWrappedBody(const String& text,
                            int16_t startY,
                            int16_t maxY,
                            int16_t marginX,
                            int16_t lineHeight,
                            uint16_t color) {
  StickCP2.Display.setTextDatum(top_left);
  StickCP2.Display.setFont(&fonts::FreeMonoBold9pt7b);
  StickCP2.Display.setTextSize(1);
  StickCP2.Display.setTextColor(color, COLOR_BG);

  int16_t x = marginX;
  int16_t y = startY;
  String word;

  for (size_t i = 0; i <= text.length(); ++i) {
    char ch = i < text.length() ? text[i] : ' ';
    if (ch != ' ' && ch != '\n') {
      word += ch;
      continue;
    }

    if (word.length()) {
      int16_t width = StickCP2.Display.textWidth(word + " ");
      if (x + width > screenW() - marginX) {
        x = marginX;
        y += lineHeight;
      }
      if (y + lineHeight > maxY) {
        StickCP2.Display.setTextColor(COLOR_MUTED, COLOR_BG);
        StickCP2.Display.setFont(&fonts::Font0);
        StickCP2.Display.drawString("...", x, y + 2);
        return;
      }
      StickCP2.Display.drawString(word + " ", x, y);
      x += width;
      word = "";
    }

    if (ch == '\n') {
      x = marginX;
      y += lineHeight;
    }
  }
}

static void drawTranscriptOverlay() {
  StickCP2.Display.fillRect(0, ART_Y - 4, screenW(), screenH() - ART_Y - 18, COLOR_BG);

  StickCP2.Display.setTextDatum(top_left);
  StickCP2.Display.setFont(&fonts::Font0);
  StickCP2.Display.setTextColor(COLOR_OK, COLOR_BG);
  StickCP2.Display.drawString("HEARD YOU", 6, ART_Y);

  StickCP2.Display.setTextDatum(top_right);
  StickCP2.Display.setTextColor(COLOR_MUTED, COLOR_BG);
  String meta = String(currentTranscript.length()) + "C";
  if (lastRecordingDurationMs > 0) {
    meta += " " + formatDuration(lastRecordingDurationMs);
  }
  StickCP2.Display.drawString(meta.c_str(), screenW() - 6, ART_Y);

  StickCP2.Display.drawFastHLine(6, ART_Y + 12, screenW() - 12, COLOR_HAIRLINE);

  drawWrappedBody(currentTranscript, ART_Y + 18, screenH() - 18, 6, 14, COLOR_FG);
}

static void drawFaultOverlay() {
  StickCP2.Display.fillRect(0, ART_Y - 4, screenW(), screenH() - ART_Y - 18, COLOR_BG);

  StickCP2.Display.setTextDatum(top_left);
  StickCP2.Display.setFont(&fonts::Font0);
  StickCP2.Display.setTextColor(COLOR_DANGER, COLOR_BG);
  StickCP2.Display.drawString("FAULT", 6, ART_Y);

  StickCP2.Display.drawFastHLine(6, ART_Y + 12, screenW() - 12, COLOR_HAIRLINE);

  drawWrappedBody(currentFault.length() ? currentFault : "see phone",
                  ART_Y + 18, screenH() - 18, 6, 14, COLOR_DANGER);
}

static void drawInfoOverlay() {
  StickCP2.Display.fillRect(0, ART_Y - 4, screenW(), screenH() - ART_Y - 18, COLOR_BG);

  StickCP2.Display.setTextDatum(top_left);
  StickCP2.Display.setFont(&fonts::Font0);
  StickCP2.Display.setTextColor(mood().primary, COLOR_BG);
  StickCP2.Display.drawString("DEVICE", 6, ART_Y);

  StickCP2.Display.setTextDatum(top_right);
  StickCP2.Display.setTextColor(COLOR_MUTED, COLOR_BG);
  StickCP2.Display.drawString(FIRMWARE_VERSION, screenW() - 6, ART_Y);

  StickCP2.Display.drawFastHLine(6, ART_Y + 12, screenW() - 12, COLOR_HAIRLINE);

  int16_t y = ART_Y + 18;
  auto row = [&](const char* label, const String& value, uint16_t valueColor) {
    StickCP2.Display.setTextDatum(top_left);
    StickCP2.Display.setFont(&fonts::Font0);
    StickCP2.Display.setTextColor(COLOR_MUTED, COLOR_BG);
    StickCP2.Display.drawString(label, 6, y);
    StickCP2.Display.setTextColor(valueColor, COLOR_BG);
    StickCP2.Display.drawString(value, 56, y);
    y += 12;
  };

  int batteryLevel = StickCP2.Power.getBatteryLevel();
  if (batteryLevel < 0) batteryLevel = 0;
  if (batteryLevel > 100) batteryLevel = 100;
  uint16_t batteryColor = batteryLevel > 30 ? COLOR_OK : batteryLevel > 15 ? COLOR_WARN : COLOR_DANGER;
  bool charging = StickCP2.Power.isCharging();

  row("BATT", String(batteryLevel) + "%" + (charging ? " CHG" : ""), batteryColor);
  row("LINK", phoneConnected ? "ONLINE" : "ADV",
      phoneConnected ? COLOR_OK : COLOR_MUTED);
  row("UP",   formatUptime(millis() - bootMs), COLOR_FG);
  row("BRT",  String(brightnessIndex + 1) + "/" + String(BRIGHTNESS_COUNT), COLOR_FG);
  row("HIST", String(historyCount) + "/" + String(HISTORY_SIZE), COLOR_FG);
  row("MOOD", String(mood().name), mood().primary);
}

// ----------------------------------------------------------------------------
// FRAME RENDER
// ----------------------------------------------------------------------------

static void render() {
  StickCP2.Display.fillRect(0, 0, screenW(), ART_Y, COLOR_BG);
  StickCP2.Display.fillRect(0, ART_Y + ART_SIZE, screenW(), screenH() - (ART_Y + ART_SIZE), COLOR_BG);

  drawTopChrome();

  switch (overlay) {
    case Overlay::Transcript: drawTranscriptOverlay(); break;
    case Overlay::Fault:      drawFaultOverlay();      break;
    case Overlay::Info:       drawInfoOverlay();       break;
    default:                  drawArtRegion();         break;
  }

  drawBottomChrome();

  lastDrawMs = millis();
}

// ----------------------------------------------------------------------------
// BLE
// ----------------------------------------------------------------------------

static void notifyControl(const char* message) {
  if (!phoneConnected || controlCharacteristic == nullptr) return;
  controlCharacteristic->setValue(reinterpret_cast<const uint8_t*>(message), strlen(message));
  controlCharacteristic->notify();
}

static void notifyAudio(const uint8_t* bytes, size_t byteCount) {
  if (!phoneConnected || audioCharacteristic == nullptr) return;
  for (size_t offset = 0; offset < byteCount; offset += BLE_PAYLOAD_BYTES) {
    size_t len = min(BLE_PAYLOAD_BYTES, byteCount - offset);
    audioCharacteristic->setValue(bytes + offset, len);
    audioCharacteristic->notify();
    delay(2);
  }
}

static void enqueueControlLine(const String& line) {
  portENTER_CRITICAL(&controlMux);
  pendingControlLine = line;
  hasPendingControlLine = true;
  portEXIT_CRITICAL(&controlMux);
}

static bool popPendingControlLine(String& line) {
  bool hasLine = false;
  portENTER_CRITICAL(&controlMux);
  if (hasPendingControlLine) {
    line = pendingControlLine;
    pendingControlLine = "";
    hasPendingControlLine = false;
    hasLine = true;
  }
  portEXIT_CRITICAL(&controlMux);
  return hasLine;
}

static void parseControlBytes(const uint8_t* bytes, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    char ch = static_cast<char>(bytes[i]);
    if (ch == '\n') {
      enqueueControlLine(controlRxBuffer);
      controlRxBuffer = "";
    } else if (ch != '\r' && controlRxBuffer.length() < 480) {
      controlRxBuffer += ch;
    }
  }
  if (controlRxBuffer.startsWith("STATE:") ||
      controlRxBuffer.startsWith("TEXT:") ||
      controlRxBuffer.startsWith("MOOD:") ||
      controlRxBuffer.startsWith("ERR:")) {
    enqueueControlLine(controlRxBuffer);
    controlRxBuffer = "";
  }
}

static bool tryParseBase64Control(const std::string& value) {
  uint8_t decoded[512];
  size_t decodedLength = 0;
  int result = mbedtls_base64_decode(
      decoded, sizeof(decoded), &decodedLength,
      reinterpret_cast<const uint8_t*>(value.data()), value.size());

  if (result != 0 || decodedLength == 0) return false;

  if ((decodedLength >= 5 && memcmp(decoded, "TEXT:", 5) == 0) ||
      (decodedLength >= 6 && memcmp(decoded, "STATE:", 6) == 0) ||
      (decodedLength >= 5 && memcmp(decoded, "MOOD:", 5) == 0) ||
      (decodedLength >= 4 && memcmp(decoded, "ERR:", 4) == 0)) {
    parseControlBytes(decoded, decodedLength);
    return true;
  }
  return false;
}

class ControlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic) override {
    std::string value = characteristic->getValue();
    if (!tryParseBase64Control(value)) {
      parseControlBytes(reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*) override {
    Serial.println("[ble] phone connected");
    phoneConnected = true;
    transition(AppState::Ready);
    // Tell phone what mood the stick is showing.
    char buf[32];
    snprintf(buf, sizeof(buf), "MOOD:%s\n", mood().name);
    notifyControl(buf);
  }
  void onDisconnect(NimBLEServer*) override {
    Serial.println("[ble] phone disconnected");
    phoneConnected = false;
    streaming = false;
    recordingLock = false;
    transition(AppState::Standby);
    NimBLEDevice::startAdvertising();
  }
};

static void handleIncomingLine(const String& line) {
  Serial.printf("[ctrl<-] %s\n", line.c_str());

  if (line.startsWith("TEXT:")) {
    currentTranscript = line.substring(5);
    pushHistory(currentTranscript);
    transition(AppState::Transcript);
    showOverlay(Overlay::Transcript, OVERLAY_AUTO_DISMISS_MS);
    return;
  }

  if (line.startsWith("MOOD:")) {
    String name = line.substring(5);
    name.trim();
    Mood next;
    if (moodFromName(name, next)) {
      // Don't echo back to phone — phone is the source.
      setMood(next, false);
    }
    return;
  }

  if (line.startsWith("STATE:")) {
    String label = line.substring(6);
    phoneStateLabel = label;
    phoneStateLabel.toLowerCase();
    if (label.equalsIgnoreCase("Recording")) {
      // ignore; the stick already knows it's recording
    } else if (label.equalsIgnoreCase("Transcribing")) {
      if (appState != AppState::Transcript) transition(AppState::Uploading);
    } else if (label.equalsIgnoreCase("Ready")) {
      if (appState != AppState::Recording) transition(AppState::Ready);
    }
    return;
  }

  if (line.startsWith("ERR:")) {
    currentFault = line.substring(4);
    transition(AppState::Fault);
    showOverlay(Overlay::Fault, OVERLAY_AUTO_DISMISS_MS);
  }
}

static void startBle() {
  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setMTU(247);

  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService* service = server->createService(SERVICE_UUID);
  audioCharacteristic = service->createCharacteristic(AUDIO_UUID, NIMBLE_PROPERTY::NOTIFY);
  controlCharacteristic = service->createCharacteristic(
      CONTROL_UUID,
      NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  controlCharacteristic->setCallbacks(new ControlCallbacks());

  service->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->start();
}

// ----------------------------------------------------------------------------
// CAPTURE
// ----------------------------------------------------------------------------

static void startCapture() {
  if (!phoneConnected) {
    currentFault = "host required";
    transition(AppState::Fault);
    showOverlay(Overlay::Fault, OVERLAY_AUTO_DISMISS_MS);
    return;
  }

  streaming = true;
  recordingStartMs = millis();
  notifyControl("START\n");
  transition(AppState::Recording);
  clearOverlay();
}

static void stopCapture() {
  if (!streaming) return;
  streaming = false;
  recordingLock = false;
  lastRecordingDurationMs = millis() - recordingStartMs;
  while (StickCP2.Mic.isRecording()) delay(1);
  notifyControl("STOP\n");
  transition(AppState::Uploading);
}

// ----------------------------------------------------------------------------
// BUTTONS
// ----------------------------------------------------------------------------

static constexpr uint32_t DOUBLE_CLICK_WINDOW_MS = 380;
static constexpr uint32_t LONG_PRESS_MS = 700;
static constexpr uint32_t CLICK_DEBOUNCE_MS = 30;

struct ButtonTracker {
  bool wasDown = false;
  uint32_t pressedAtMs = 0;
  uint32_t releasedAtMs = 0;
  uint32_t lastClickMs = 0;
  bool longFired = false;
  bool clickPending = false;
};

static ButtonTracker btnA;
static ButtonTracker btnB;

enum class ButtonEvent : uint8_t {
  None,
  Press,
  Release,
  Click,
  DoubleClick,
  LongPress,
};

static ButtonEvent updateButton(ButtonTracker& t, bool isDown) {
  uint32_t now = millis();
  ButtonEvent ev = ButtonEvent::None;

  if (isDown && !t.wasDown) {
    t.wasDown = true;
    t.pressedAtMs = now;
    t.longFired = false;
    return ButtonEvent::Press;
  }

  if (!isDown && t.wasDown) {
    t.wasDown = false;
    t.releasedAtMs = now;
    if (t.longFired) {
      t.clickPending = false;
      return ButtonEvent::Release;
    }
    if (now - t.pressedAtMs < CLICK_DEBOUNCE_MS) {
      return ButtonEvent::Release;
    }
    if (t.clickPending && now - t.lastClickMs <= DOUBLE_CLICK_WINDOW_MS) {
      t.clickPending = false;
      t.lastClickMs = 0;
      return ButtonEvent::DoubleClick;
    }
    t.clickPending = true;
    t.lastClickMs = now;
    return ButtonEvent::Release;
  }

  if (isDown && !t.longFired && now - t.pressedAtMs >= LONG_PRESS_MS) {
    t.longFired = true;
    t.clickPending = false;
    return ButtonEvent::LongPress;
  }

  if (t.clickPending && now - t.lastClickMs > DOUBLE_CLICK_WINDOW_MS) {
    t.clickPending = false;
    return ButtonEvent::Click;
  }

  return ev;
}

static void handleButtons() {
  // M5StickCPlus2 1.0.2 has a broken StickCP2.BtnA reference at startup,
  // so we read M5.BtnA / M5.BtnB raw and track gestures ourselves.
  ButtonEvent a = updateButton(btnA, M5.BtnA.isPressed());
  ButtonEvent b = updateButton(btnB, M5.BtnB.isPressed());

  // ---- Button A: hold to talk + double-click to lock ----
  switch (a) {
    case ButtonEvent::Press:
      if (overlay != Overlay::None) clearOverlay();
      if (!streaming && !recordingLock) startCapture();
      break;
    case ButtonEvent::Release:
      if (streaming && !recordingLock) stopCapture();
      break;
    case ButtonEvent::DoubleClick:
      if (streaming) {
        stopCapture();
      } else {
        recordingLock = true;
        startCapture();
      }
      break;
    default:
      break;
  }

  // ---- Button B: cycle mood / brightness / info ----
  switch (b) {
    case ButtonEvent::Click:
      if (overlay != Overlay::None) {
        clearOverlay();
      } else {
        cycleMood();
      }
      break;
    case ButtonEvent::DoubleClick:
      cycleBrightness();
      break;
    case ButtonEvent::LongPress:
      if (overlay == Overlay::Info) {
        clearOverlay();
      } else {
        showOverlay(Overlay::Info, 8000);
      }
      break;
    default:
      break;
  }
}

// ----------------------------------------------------------------------------
// SETUP / LOOP
// ----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  Serial.println("[boot] M5 Voice Stick");

  auto cfg = M5.config();
  StickCP2.begin(cfg);
  StickCP2.Display.setRotation(0);  // portrait — lanyard hole at top, USB at bottom
  StickCP2.Display.fillScreen(COLOR_BG);
  setBrightnessIndex(brightnessIndex);

  artCanvas.setColorDepth(16);
  artCanvas.createSprite(ART_SIZE, ART_SIZE);
  artCanvas.fillSprite(COLOR_BG);

  StickCP2.Speaker.end();
  StickCP2.Mic.begin();

  bootMs = millis();
  transition(AppState::Standby);
  startBle();
}

void loop() {
  StickCP2.update();

  handleButtons();

  // Capture audio in Recording state and compute peak amplitude.
  if (streaming && phoneConnected && StickCP2.Mic.isEnabled()) {
    if (StickCP2.Mic.record(audioBuffer, SAMPLES_PER_CHUNK, SAMPLE_RATE)) {
      notifyAudio(reinterpret_cast<const uint8_t*>(audioBuffer), sizeof(audioBuffer));
      int32_t peak = 0;
      for (size_t i = 0; i < SAMPLES_PER_CHUNK; ++i) {
        int16_t s = audioBuffer[i];
        int16_t a = s < 0 ? -s : s;
        if (a > peak) peak = a;
      }
      audioPeakRaw = peak;
    }
  } else {
    audioPeakRaw = 0;
  }

  // Process incoming control lines from phone.
  String line;
  if (popPendingControlLine(line)) {
    handleIncomingLine(line);
  }

  // Auto-dismiss overlays after their timeout.
  if (overlay != Overlay::None && millis() > overlayUntilMs) {
    if (overlay == Overlay::Transcript || overlay == Overlay::Fault) {
      currentTranscript = "";
      currentFault = "";
      if (appState == AppState::Transcript || appState == AppState::Fault) {
        transition(phoneConnected ? AppState::Ready : AppState::Standby);
      }
    }
    clearOverlay();
  }

  // Frame loop — animate at REDRAW_INTERVAL_MS (~14fps).
  uint32_t now = millis();
  if (now - lastDrawMs >= REDRAW_INTERVAL_MS) {
    frameCounter++;
    updateAudioLevel();
    render();
  }

  delay(2);
}
