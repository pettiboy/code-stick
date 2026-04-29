#include <Arduino.h>
#include <M5StickCPlus2.h>
#include <NimBLEDevice.h>
#include <mbedtls/base64.h>
#include <math.h>

// ----------------------------------------------------------------------------
// CONSTANTS
// ----------------------------------------------------------------------------

static constexpr const char* DEVICE_NAME = "M5VoiceStick";
static constexpr const char* FIRMWARE_VERSION = "v0.4";

static const BLEUUID SERVICE_UUID("3e7a0001-e33b-4e2f-9a85-f03e1d33c001");
static const BLEUUID AUDIO_UUID("3e7a0002-e33b-4e2f-9a85-f03e1d33c001");
static const BLEUUID CONTROL_UUID("3e7a0003-e33b-4e2f-9a85-f03e1d33c001");

static constexpr uint32_t SAMPLE_RATE = 16000;
static constexpr size_t SAMPLES_PER_CHUNK = 240;
static constexpr size_t BLE_PAYLOAD_BYTES = 180;

static constexpr size_t HISTORY_SIZE = 4;
static constexpr uint32_t OVERLAY_AUTO_DISMISS_MS = 6500;
static constexpr uint32_t REDRAW_INTERVAL_MS = 120;

static constexpr uint8_t BRIGHTNESS_LEVELS[] = {40, 90, 140, 200};
static constexpr size_t BRIGHTNESS_COUNT = sizeof(BRIGHTNESS_LEVELS) / sizeof(BRIGHTNESS_LEVELS[0]);

static constexpr int16_t SCREEN_W = 135;
static constexpr int16_t SCREEN_H = 240;

static constexpr uint16_t COLOR_BG = 0x0000;
static constexpr uint16_t COLOR_FG = 0xEF3B;
static constexpr uint16_t COLOR_FG_DIM = 0xBDB6;
static constexpr uint16_t COLOR_MUTED = 0x7C0F;
static constexpr uint16_t COLOR_HAIRLINE = 0x39E7;
static constexpr uint16_t COLOR_DANGER = 0xF986;
static constexpr uint16_t COLOR_OK = 0x37D5;

// ----------------------------------------------------------------------------
// MOOD CATALOG — face expressions, must match mobile-app catalog
// ----------------------------------------------------------------------------

enum class Mood : uint8_t {
  Happy,
  Love,
  Sad,
  Wink,
  Surprise,
  Sleepy,
  Angry,
  Chill,
  Count,
};

struct MoodSpec {
  const char* name;
  uint16_t primary;   // eyes / mouth
  uint16_t accent;    // brows / hearts / extras
};

// Order matches the mobile app and the Mood enum.
static const MoodSpec MOODS[static_cast<size_t>(Mood::Count)] = {
  { "HAPPY",    0xFE88, 0xFE88 },  // gold
  { "LOVE",     0xFA94, 0xF986 },  // pink + red hearts
  { "SAD",      0x551F, 0x551F },  // blue
  { "WINK",     0xFE88, 0xFE88 },  // gold
  { "SURPRISE", 0xEF3B, 0xEF3B },  // cream/white
  { "SLEEPY",   0xB4D0, 0xB4D0 },  // muted lavender
  { "ANGRY",    0xF986, 0xFB80 },  // red + dark accent for brows
  { "CHILL",    0x9FE3, 0x9FE3 },  // lime
};

static Mood currentMood = Mood::Happy;

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

static int16_t audioBuffer[SAMPLES_PER_CHUNK];
static NimBLECharacteristic* audioCharacteristic = nullptr;
static NimBLECharacteristic* controlCharacteristic = nullptr;

static String controlRxBuffer;
static portMUX_TYPE controlMux = portMUX_INITIALIZER_UNLOCKED;
static String pendingControlLine;
static volatile bool hasPendingControlLine = false;

// Full-screen back buffer. Drawn to once per render, pushed once. No flicker.
static M5Canvas screenCanvas(&StickCP2.Display);

// ----------------------------------------------------------------------------
// HELPERS
// ----------------------------------------------------------------------------

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
// FACE PRIMITIVES — all drawing goes into screenCanvas
// ----------------------------------------------------------------------------

static constexpr int16_t EYE_LX = 42;
static constexpr int16_t EYE_RX = 92;
static constexpr int16_t EYE_Y  = 100;
static constexpr int16_t MOUTH_X = 67;
static constexpr int16_t MOUTH_Y = 168;

// Parabolic curve. smile=true → corners up, middle down (a U shape).
// smile=false → corners down, middle up (an n / frown shape).
static void drawCurve(int16_t cx, int16_t cy, int16_t width, int16_t depth,
                      uint16_t color, int16_t thickness, bool smile) {
  int16_t half = width / 2;
  int16_t prev_x = cx - half;
  int16_t prev_y = cy;
  for (int16_t x = -half + 1; x <= half; ++x) {
    float nx = static_cast<float>(2 * x) / static_cast<float>(width);
    int16_t dy = static_cast<int16_t>(depth * (1.0f - nx * nx));
    int16_t y = smile ? cy + dy : cy - dy;
    int16_t X = cx + x;
    for (int16_t t = 0; t < thickness; ++t) {
      screenCanvas.drawLine(prev_x, prev_y + t, X, y + t, color);
    }
    prev_x = X;
    prev_y = y;
  }
}

static void drawHeart(int16_t cx, int16_t cy, int16_t r, uint16_t color) {
  // Two overlapping circles + a downward triangle.
  int16_t lobeY = cy - r / 3;
  screenCanvas.fillCircle(cx - r, lobeY, r, color);
  screenCanvas.fillCircle(cx + r, lobeY, r, color);
  screenCanvas.fillTriangle(
    cx - 2 * r, lobeY,
    cx + 2 * r, lobeY,
    cx, cy + r + r / 2,
    color);
}

static void drawEye(int16_t x, int16_t y, int16_t r, uint16_t color) {
  screenCanvas.fillCircle(x, y, r, color);
}

static void drawClosedEye(int16_t x, int16_t y, int16_t w, uint16_t color) {
  drawCurve(x, y - 1, w, 4, color, 3, true);
}

// ----------------------------------------------------------------------------
// FACES
// ----------------------------------------------------------------------------

static void drawFaceHappy() {
  uint16_t c = mood().primary;
  drawEye(EYE_LX, EYE_Y, 11, c);
  drawEye(EYE_RX, EYE_Y, 11, c);
  drawCurve(MOUTH_X, MOUTH_Y - 8, 60, 20, c, 4, true);
}

static void drawFaceLove() {
  uint16_t pink = mood().primary;
  uint16_t red = mood().accent;
  drawHeart(EYE_LX, EYE_Y - 2, 9, red);
  drawHeart(EYE_RX, EYE_Y - 2, 9, red);
  drawCurve(MOUTH_X, MOUTH_Y - 6, 56, 18, pink, 4, true);
}

static void drawFaceSad() {
  uint16_t c = mood().primary;
  drawEye(EYE_LX, EYE_Y, 9, c);
  drawEye(EYE_RX, EYE_Y, 9, c);
  // small tear under each eye
  screenCanvas.fillCircle(EYE_LX - 6, EYE_Y + 14, 3, c);
  screenCanvas.fillCircle(EYE_RX + 6, EYE_Y + 14, 3, c);
  drawCurve(MOUTH_X, MOUTH_Y + 10, 56, 16, c, 4, false);
}

static void drawFaceWink() {
  uint16_t c = mood().primary;
  drawEye(EYE_LX, EYE_Y, 11, c);
  drawClosedEye(EYE_RX, EYE_Y, 26, c);
  drawCurve(MOUTH_X, MOUTH_Y - 6, 56, 16, c, 4, true);
}

static void drawFaceSurprise() {
  uint16_t c = mood().primary;
  // hollow ring eyes
  screenCanvas.drawCircle(EYE_LX, EYE_Y, 14, c);
  screenCanvas.drawCircle(EYE_LX, EYE_Y, 13, c);
  screenCanvas.drawCircle(EYE_LX, EYE_Y, 12, c);
  screenCanvas.fillCircle(EYE_LX, EYE_Y, 5, c);

  screenCanvas.drawCircle(EYE_RX, EYE_Y, 14, c);
  screenCanvas.drawCircle(EYE_RX, EYE_Y, 13, c);
  screenCanvas.drawCircle(EYE_RX, EYE_Y, 12, c);
  screenCanvas.fillCircle(EYE_RX, EYE_Y, 5, c);

  // O mouth
  screenCanvas.drawCircle(MOUTH_X, MOUTH_Y, 14, c);
  screenCanvas.drawCircle(MOUTH_X, MOUTH_Y, 13, c);
  screenCanvas.drawCircle(MOUTH_X, MOUTH_Y, 12, c);
}

static void drawFaceSleepy() {
  uint16_t c = mood().primary;
  drawClosedEye(EYE_LX, EYE_Y, 26, c);
  drawClosedEye(EYE_RX, EYE_Y, 26, c);
  // tiny soft mouth
  drawCurve(MOUTH_X, MOUTH_Y, 26, 6, c, 3, true);
  // little Z above the right eye for "asleep" hint
  screenCanvas.setTextDatum(top_left);
  screenCanvas.setFont(&fonts::FreeMonoBold9pt7b);
  screenCanvas.setTextColor(c, COLOR_BG);
  screenCanvas.drawString("z", EYE_RX + 14, EYE_Y - 28);
  screenCanvas.drawString("z", EYE_RX + 22, EYE_Y - 38);
}

static void drawFaceAngry() {
  uint16_t c = mood().primary;
  uint16_t accent = mood().accent;
  // eyes pushed slightly down to leave room for brows
  drawEye(EYE_LX, EYE_Y + 4, 9, c);
  drawEye(EYE_RX, EYE_Y + 4, 9, c);
  // angled brows
  for (int t = 0; t < 4; ++t) {
    screenCanvas.drawLine(EYE_LX - 14, EYE_Y - 16 + t,
                          EYE_LX + 12, EYE_Y - 4 + t, accent);
    screenCanvas.drawLine(EYE_RX + 14, EYE_Y - 16 + t,
                          EYE_RX - 12, EYE_Y - 4 + t, accent);
  }
  drawCurve(MOUTH_X, MOUTH_Y + 10, 54, 14, c, 4, false);
}

static void drawFaceChill() {
  uint16_t c = mood().primary;
  // half-closed easygoing eyes
  drawClosedEye(EYE_LX, EYE_Y, 22, c);
  drawClosedEye(EYE_RX, EYE_Y, 22, c);
  // straight mouth, slight upward tilt at the right (smirk)
  for (int t = 0; t < 4; ++t) {
    screenCanvas.drawLine(MOUTH_X - 24, MOUTH_Y + t,
                          MOUTH_X + 24, MOUTH_Y + t - 4, c);
  }
}

using FaceDrawFn = void (*)();
static const FaceDrawFn FACE_DRAW[static_cast<size_t>(Mood::Count)] = {
  drawFaceHappy,
  drawFaceLove,
  drawFaceSad,
  drawFaceWink,
  drawFaceSurprise,
  drawFaceSleepy,
  drawFaceAngry,
  drawFaceChill,
};

// ----------------------------------------------------------------------------
// OVERLAYS — drawn into the same screenCanvas, full-screen
// ----------------------------------------------------------------------------

static void drawWrappedToCanvas(const String& text,
                                int16_t startY,
                                int16_t maxY,
                                int16_t marginX,
                                int16_t lineHeight,
                                uint16_t color) {
  screenCanvas.setTextDatum(top_left);
  screenCanvas.setFont(&fonts::FreeMonoBold9pt7b);
  screenCanvas.setTextSize(1);
  screenCanvas.setTextColor(color, COLOR_BG);

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
      int16_t width = screenCanvas.textWidth(word + " ");
      if (x + width > SCREEN_W - marginX) {
        x = marginX;
        y += lineHeight;
      }
      if (y + lineHeight > maxY) {
        screenCanvas.setTextColor(COLOR_MUTED, COLOR_BG);
        screenCanvas.setFont(&fonts::Font0);
        screenCanvas.drawString("...", x, y + 2);
        return;
      }
      screenCanvas.drawString(word + " ", x, y);
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
  screenCanvas.setTextDatum(top_left);
  screenCanvas.setFont(&fonts::Font0);
  screenCanvas.setTextColor(COLOR_OK, COLOR_BG);
  screenCanvas.drawString("HEARD YOU", 8, 12);

  screenCanvas.setTextDatum(top_right);
  screenCanvas.setTextColor(COLOR_MUTED, COLOR_BG);
  String meta = String(currentTranscript.length()) + "C";
  if (lastRecordingDurationMs > 0) {
    meta += " " + formatDuration(lastRecordingDurationMs);
  }
  screenCanvas.drawString(meta.c_str(), SCREEN_W - 8, 12);

  screenCanvas.drawFastHLine(8, 26, SCREEN_W - 16, COLOR_HAIRLINE);
  drawWrappedToCanvas(currentTranscript, 34, SCREEN_H - 12, 8, 14, COLOR_FG);
}

static void drawFaultOverlay() {
  screenCanvas.setTextDatum(top_left);
  screenCanvas.setFont(&fonts::Font0);
  screenCanvas.setTextColor(COLOR_DANGER, COLOR_BG);
  screenCanvas.drawString("FAULT", 8, 12);
  screenCanvas.drawFastHLine(8, 26, SCREEN_W - 16, COLOR_HAIRLINE);
  drawWrappedToCanvas(currentFault.length() ? currentFault : "see phone",
                      34, SCREEN_H - 12, 8, 14, COLOR_DANGER);
}

static void drawInfoOverlay() {
  screenCanvas.setTextDatum(top_left);
  screenCanvas.setFont(&fonts::Font0);
  screenCanvas.setTextColor(mood().primary, COLOR_BG);
  screenCanvas.drawString("DEVICE", 8, 12);
  screenCanvas.setTextDatum(top_right);
  screenCanvas.setTextColor(COLOR_MUTED, COLOR_BG);
  screenCanvas.drawString(FIRMWARE_VERSION, SCREEN_W - 8, 12);
  screenCanvas.drawFastHLine(8, 26, SCREEN_W - 16, COLOR_HAIRLINE);

  int16_t y = 34;
  auto row = [&](const char* label, const String& value, uint16_t valueColor) {
    screenCanvas.setTextDatum(top_left);
    screenCanvas.setFont(&fonts::Font0);
    screenCanvas.setTextColor(COLOR_MUTED, COLOR_BG);
    screenCanvas.drawString(label, 8, y);
    screenCanvas.setTextColor(valueColor, COLOR_BG);
    screenCanvas.drawString(value, 60, y);
    y += 14;
  };

  int batteryLevel = StickCP2.Power.getBatteryLevel();
  if (batteryLevel < 0) batteryLevel = 0;
  if (batteryLevel > 100) batteryLevel = 100;
  uint16_t batteryColor = batteryLevel > 30 ? COLOR_OK :
                          batteryLevel > 15 ? mood().accent : COLOR_DANGER;
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
// FRAME RENDER — single sprite fill, single sprite push. No flicker.
// ----------------------------------------------------------------------------

static void render() {
  screenCanvas.fillSprite(COLOR_BG);

  switch (overlay) {
    case Overlay::Transcript: drawTranscriptOverlay(); break;
    case Overlay::Fault:      drawFaultOverlay();      break;
    case Overlay::Info:       drawInfoOverlay();       break;
    default:
      FACE_DRAW[static_cast<size_t>(currentMood)]();
      break;
  }

  // Recording dot — small, top-right corner, blinks while capturing.
  if (streaming && overlay == Overlay::None) {
    bool blink = (frameCounter / 4) & 1;
    int16_t cx = SCREEN_W - 12;
    int16_t cy = 14;
    if (blink) screenCanvas.fillCircle(cx, cy, 5, COLOR_DANGER);
    screenCanvas.drawCircle(cx, cy, 6, COLOR_DANGER);
  }

  screenCanvas.pushSprite(0, 0);
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
// CAPTURE — hold-to-talk path. Always available when phone is connected.
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
static ButtonTracker btnExt;

static constexpr uint8_t EXT_BTN_PIN = 26;

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
  ButtonEvent ext = updateButton(btnExt, digitalRead(EXT_BTN_PIN) == LOW);

  // ---- Button A: hold to talk + double-click to lock. Always available. ----
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
        if (overlay != Overlay::None) clearOverlay();
        recordingLock = true;
        startCapture();
      }
      break;
    default:
      break;
  }

  // ---- Ext button (G26 + GND): mirrors Button A ----
  switch (ext) {
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
        if (overlay != Overlay::None) clearOverlay();
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
  StickCP2.Display.setRotation(0);  // portrait — lanyard at top
  StickCP2.Display.fillScreen(COLOR_BG);
  setBrightnessIndex(brightnessIndex);

  screenCanvas.setColorDepth(16);
  screenCanvas.createSprite(SCREEN_W, SCREEN_H);
  screenCanvas.fillSprite(COLOR_BG);

  pinMode(EXT_BTN_PIN, INPUT_PULLUP);

  StickCP2.Speaker.end();
  StickCP2.Mic.begin();

  bootMs = millis();
  transition(AppState::Standby);
  startBle();
}

void loop() {
  StickCP2.update();

  handleButtons();

  // Capture audio while in Recording state. Hold-to-talk always works
  // regardless of mood, overlay, or display state.
  if (streaming && phoneConnected && StickCP2.Mic.isEnabled()) {
    if (StickCP2.Mic.record(audioBuffer, SAMPLES_PER_CHUNK, SAMPLE_RATE)) {
      notifyAudio(reinterpret_cast<const uint8_t*>(audioBuffer), sizeof(audioBuffer));
    }
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

  // One sprite-fill + one sprite-push per frame. Tearing-free.
  uint32_t now = millis();
  if (now - lastDrawMs >= REDRAW_INTERVAL_MS) {
    frameCounter++;
    render();
  }

  delay(2);
}
