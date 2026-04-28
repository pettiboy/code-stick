#include <Arduino.h>
#include <M5StickCPlus2.h>
#include <NimBLEDevice.h>
#include <mbedtls/base64.h>

// ----------------------------------------------------------------------------
// CONSTANTS
// ----------------------------------------------------------------------------

static constexpr const char* DEVICE_NAME = "M5VoiceStick";
static constexpr const char* FIRMWARE_VERSION = "v0.2";

static const BLEUUID SERVICE_UUID("3e7a0001-e33b-4e2f-9a85-f03e1d33c001");
static const BLEUUID AUDIO_UUID("3e7a0002-e33b-4e2f-9a85-f03e1d33c001");
static const BLEUUID CONTROL_UUID("3e7a0003-e33b-4e2f-9a85-f03e1d33c001");

static constexpr uint32_t SAMPLE_RATE = 16000;
static constexpr size_t SAMPLES_PER_CHUNK = 240;
static constexpr size_t BLE_PAYLOAD_BYTES = 180;

static constexpr size_t HISTORY_SIZE = 4;
static constexpr uint32_t VIEW_AUTO_RETURN_MS = 12000;
static constexpr uint32_t REDRAW_INTERVAL_MS = 120;

static constexpr uint8_t BRIGHTNESS_LEVELS[] = {40, 90, 140, 200};
static constexpr size_t BRIGHTNESS_COUNT = sizeof(BRIGHTNESS_LEVELS) / sizeof(BRIGHTNESS_LEVELS[0]);

static constexpr uint16_t COLOR_BG = 0x0000;
static constexpr uint16_t COLOR_FG = 0xEF3B;
static constexpr uint16_t COLOR_FG_DIM = 0xBDB6;
static constexpr uint16_t COLOR_MUTED = 0x7C0F;
static constexpr uint16_t COLOR_HAIRLINE = 0x39E7;
static constexpr uint16_t COLOR_ACCENT = 0xD7E8;
static constexpr uint16_t COLOR_DANGER = 0xFAA7;
static constexpr uint16_t COLOR_OK = 0x37D5;
static constexpr uint16_t COLOR_WARN = 0xFD20;

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

enum class View : uint8_t {
  Live,
  History,
  Info,
};

struct TranscriptEntry {
  String text;
  uint32_t arrivedAtMs = 0;
  bool used = false;
};

static AppState appState = AppState::Standby;
static View currentView = View::Live;
static String currentTranscript;
static String currentFault;
static String phoneStateLabel = "ready";

static TranscriptEntry history[HISTORY_SIZE];
static size_t historyHead = 0;
static size_t historyCount = 0;
static size_t historyCursor = 0;

static bool phoneConnected = false;
static bool streaming = false;
static bool recordingLock = false;

static uint32_t recordingStartMs = 0;
static uint32_t lastRecordingDurationMs = 0;
static uint32_t lastViewChangeMs = 0;
static uint32_t bootMs = 0;
static uint32_t lastDrawMs = 0;
static uint8_t brightnessIndex = 2;

static bool needsRedraw = true;

static int16_t audioBuffer[SAMPLES_PER_CHUNK];
static NimBLECharacteristic* audioCharacteristic = nullptr;
static NimBLECharacteristic* controlCharacteristic = nullptr;

static String controlRxBuffer;
static portMUX_TYPE controlMux = portMUX_INITIALIZER_UNLOCKED;
static String pendingControlLine;
static volatile bool hasPendingControlLine = false;

// ----------------------------------------------------------------------------
// HELPERS
// ----------------------------------------------------------------------------

static int16_t screenW() { return StickCP2.Display.width(); }
static int16_t screenH() { return StickCP2.Display.height(); }

static const char* stateLabel(AppState s) {
  switch (s) {
    case AppState::Standby:    return "STANDBY";
    case AppState::Ready:      return "READY";
    case AppState::Recording:  return "CAPTURE";
    case AppState::Uploading:  return "UPLINK";
    case AppState::Transcript: return "TRANSCRIPT";
    case AppState::Fault:      return "FAULT";
  }
  return "?";
}

static uint16_t stateAccent(AppState s) {
  switch (s) {
    case AppState::Standby:    return COLOR_MUTED;
    case AppState::Ready:      return COLOR_ACCENT;
    case AppState::Recording:  return COLOR_DANGER;
    case AppState::Uploading:  return COLOR_ACCENT;
    case AppState::Transcript: return COLOR_ACCENT;
    case AppState::Fault:      return COLOR_DANGER;
  }
  return COLOR_ACCENT;
}

static const char* viewLabel(View v) {
  switch (v) {
    case View::Live:    return "LIVE";
    case View::History: return "HIST";
    case View::Info:    return "INFO";
  }
  return "?";
}

static void requestRedraw() { needsRedraw = true; }

static void setView(View v) {
  if (currentView == v) return;
  currentView = v;
  lastViewChangeMs = millis();
  requestRedraw();
}

static void cycleView() {
  switch (currentView) {
    case View::Live:    setView(View::History); break;
    case View::History: setView(View::Info);    break;
    case View::Info:    setView(View::Live);    break;
  }
}

static void setBrightnessIndex(uint8_t i) {
  brightnessIndex = i % BRIGHTNESS_COUNT;
  StickCP2.Display.setBrightness(BRIGHTNESS_LEVELS[brightnessIndex]);
  requestRedraw();
}

static void cycleBrightness() {
  setBrightnessIndex((brightnessIndex + 1) % BRIGHTNESS_COUNT);
}

static void transition(AppState next) {
  if (appState == next) return;
  Serial.printf("[state] %s -> %s\n", stateLabel(appState), stateLabel(next));
  appState = next;
  setView(View::Live);
  requestRedraw();
}

static void pushHistory(const String& text) {
  history[historyHead].text = text;
  history[historyHead].arrivedAtMs = millis();
  history[historyHead].used = true;
  historyHead = (historyHead + 1) % HISTORY_SIZE;
  if (historyCount < HISTORY_SIZE) historyCount++;
  historyCursor = 0;
}

static const TranscriptEntry* historyAt(size_t cursor) {
  if (cursor >= historyCount) return nullptr;
  size_t idx = (historyHead + HISTORY_SIZE - 1 - cursor) % HISTORY_SIZE;
  return &history[idx];
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
// DRAW: shared chrome
// ----------------------------------------------------------------------------

static void drawCornerCrosshair(int16_t x, int16_t y, int16_t dx, int16_t dy, uint16_t color) {
  StickCP2.Display.drawLine(x, y, x + 6 * dx, y, color);
  StickCP2.Display.drawLine(x, y, x, y + 6 * dy, color);
}

static void drawTopBar(uint16_t accentColor) {
  drawCornerCrosshair(2, 2, 1, 1, accentColor);
  drawCornerCrosshair(screenW() - 3, 2, -1, 1, accentColor);
  StickCP2.Display.drawFastHLine(8, 16, screenW() - 16, COLOR_HAIRLINE);

  StickCP2.Display.setTextDatum(top_left);
  StickCP2.Display.setFont(&fonts::Font0);
  StickCP2.Display.setTextSize(1);

  StickCP2.Display.setTextColor(accentColor, COLOR_BG);
  StickCP2.Display.drawString("M5VS", 8, 4);

  StickCP2.Display.setTextColor(COLOR_MUTED, COLOR_BG);
  StickCP2.Display.drawString("NODE 01", 36, 4);

  StickCP2.Display.setTextDatum(top_right);
  StickCP2.Display.setTextColor(accentColor, COLOR_BG);
  StickCP2.Display.drawString(viewLabel(currentView), screenW() - 38, 4);

  const int16_t barsX = screenW() - 30;
  const int16_t bandY = 11;
  const uint16_t onColor = phoneConnected ? COLOR_ACCENT : COLOR_HAIRLINE;
  int activeBars = phoneConnected ? 5 : streaming ? 2 : 1;
  for (int i = 0; i < 5; ++i) {
    int16_t height = 2 + i * 2;
    int16_t x = barsX + i * 4;
    int16_t y = bandY - height;
    StickCP2.Display.fillRect(x, y, 3, height, i < activeBars ? onColor : COLOR_HAIRLINE);
  }
}

static void drawBottomBar(const char* leftLabel, uint16_t leftColor, const char* rightLabel) {
  drawCornerCrosshair(2, screenH() - 3, 1, -1, COLOR_HAIRLINE);
  drawCornerCrosshair(screenW() - 3, screenH() - 3, -1, -1, COLOR_HAIRLINE);
  StickCP2.Display.drawFastHLine(8, screenH() - 17, screenW() - 16, COLOR_HAIRLINE);

  StickCP2.Display.setTextDatum(top_left);
  StickCP2.Display.setFont(&fonts::Font0);
  StickCP2.Display.setTextSize(1);
  StickCP2.Display.setTextColor(leftColor, COLOR_BG);
  StickCP2.Display.drawString(leftLabel, 8, screenH() - 12);

  if (rightLabel) {
    StickCP2.Display.setTextDatum(top_right);
    StickCP2.Display.setTextColor(COLOR_MUTED, COLOR_BG);
    StickCP2.Display.drawString(rightLabel, screenW() - 8, screenH() - 12);
  }
}

static void drawStateLine(uint16_t color) {
  const char* leftLabel = nullptr;
  switch (appState) {
    case AppState::Standby:    leftLabel = "OFFLINE"; break;
    case AppState::Ready:      leftLabel = "STBY";    break;
    case AppState::Recording:  leftLabel = recordingLock ? "REC LOCK" : "REC"; break;
    case AppState::Uploading:  leftLabel = "UPLINK";  break;
    case AppState::Transcript: leftLabel = "OK";      break;
    case AppState::Fault:      leftLabel = "ERR";     break;
  }
  char right[32];
  snprintf(right, sizeof(right), "BRT %u/%u",
           static_cast<unsigned>(brightnessIndex + 1),
           static_cast<unsigned>(BRIGHTNESS_COUNT));
  drawBottomBar(leftLabel, color, right);
}

// ----------------------------------------------------------------------------
// DRAW: per-view
// ----------------------------------------------------------------------------

static void drawCenteredHeadline(const String& title, const String& subtitle, uint16_t accent) {
  StickCP2.Display.setTextDatum(middle_center);
  StickCP2.Display.setFont(&fonts::FreeMonoBold12pt7b);
  StickCP2.Display.setTextSize(1);
  StickCP2.Display.setTextColor(accent, COLOR_BG);

  String upper = title;
  upper.toUpperCase();
  StickCP2.Display.drawString(upper, screenW() / 2, screenH() / 2 - 14);

  StickCP2.Display.drawFastHLine(screenW() / 2 - 14, screenH() / 2 + 4, 28, accent);

  StickCP2.Display.setFont(&fonts::Font2);
  StickCP2.Display.setTextColor(COLOR_FG_DIM, COLOR_BG);
  StickCP2.Display.drawString(subtitle, screenW() / 2, screenH() / 2 + 22);
}

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

static void drawRecordingScreen() {
  StickCP2.Display.fillScreen(COLOR_BG);
  uint16_t accent = stateAccent(appState);
  drawTopBar(accent);

  StickCP2.Display.setTextDatum(top_left);
  StickCP2.Display.setFont(&fonts::FreeMonoBold12pt7b);
  StickCP2.Display.setTextColor(COLOR_DANGER, COLOR_BG);
  StickCP2.Display.drawString("CAPTURE", 10, 26);

  StickCP2.Display.setFont(&fonts::Font0);
  StickCP2.Display.setTextColor(recordingLock ? COLOR_ACCENT : COLOR_MUTED, COLOR_BG);
  StickCP2.Display.drawString(recordingLock ? "LOCK · TAP A TO STOP" : "HOLD A TO TALK", 10, 50);

  uint32_t elapsed = millis() - recordingStartMs;
  String dur = formatDuration(elapsed);
  StickCP2.Display.setTextDatum(top_right);
  StickCP2.Display.setFont(&fonts::FreeMonoBold9pt7b);
  StickCP2.Display.setTextColor(COLOR_FG, COLOR_BG);
  StickCP2.Display.drawString(dur, screenW() - 10, 50);

  // Level meter (pseudo from tick to feel alive)
  const int16_t meterX = 10;
  const int16_t meterY = screenH() - 38;
  const int16_t meterW = screenW() - 20;
  const int meterCells = 14;
  const int16_t cellW = meterW / meterCells - 1;
  uint32_t phase = (elapsed / 60) % meterCells;
  for (int i = 0; i < meterCells; ++i) {
    int distance = abs(static_cast<int>(phase) - i);
    bool active = distance <= 2;
    uint16_t color = active ? (i > meterCells - 4 ? COLOR_DANGER : COLOR_ACCENT) : COLOR_HAIRLINE;
    StickCP2.Display.fillRect(meterX + i * (cellW + 1), meterY, cellW, 8, color);
  }

  drawStateLine(accent);
}

static void drawTranscriptScreen() {
  StickCP2.Display.fillScreen(COLOR_BG);
  drawTopBar(COLOR_ACCENT);

  StickCP2.Display.setTextDatum(top_left);
  StickCP2.Display.setFont(&fonts::Font0);
  StickCP2.Display.setTextColor(COLOR_ACCENT, COLOR_BG);
  StickCP2.Display.drawString("TRANSCRIPT", 8, 22);

  StickCP2.Display.setTextDatum(top_right);
  StickCP2.Display.setTextColor(COLOR_MUTED, COLOR_BG);
  String meta = String(currentTranscript.length()) + "C";
  if (lastRecordingDurationMs > 0) {
    meta += " / " + formatDuration(lastRecordingDurationMs);
  }
  StickCP2.Display.drawString(meta.c_str(), screenW() - 8, 22);

  StickCP2.Display.drawFastHLine(8, 34, screenW() - 16, COLOR_HAIRLINE);

  drawWrappedBody(currentTranscript, 40, screenH() - 22, 10, 16, COLOR_FG);
  drawStateLine(COLOR_ACCENT);
}

static void drawHistoryScreen() {
  StickCP2.Display.fillScreen(COLOR_BG);
  drawTopBar(COLOR_ACCENT);

  StickCP2.Display.setTextDatum(top_left);
  StickCP2.Display.setFont(&fonts::Font0);
  StickCP2.Display.setTextColor(COLOR_ACCENT, COLOR_BG);
  StickCP2.Display.drawString("HISTORY", 8, 22);

  StickCP2.Display.setTextDatum(top_right);
  StickCP2.Display.setTextColor(COLOR_MUTED, COLOR_BG);
  if (historyCount == 0) {
    StickCP2.Display.drawString("0 / 0", screenW() - 8, 22);
  } else {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u / %u",
             static_cast<unsigned>(historyCursor + 1),
             static_cast<unsigned>(historyCount));
    StickCP2.Display.drawString(buf, screenW() - 8, 22);
  }

  StickCP2.Display.drawFastHLine(8, 34, screenW() - 16, COLOR_HAIRLINE);

  if (historyCount == 0) {
    StickCP2.Display.setTextDatum(middle_center);
    StickCP2.Display.setFont(&fonts::Font2);
    StickCP2.Display.setTextColor(COLOR_MUTED, COLOR_BG);
    StickCP2.Display.drawString("no transmissions yet", screenW() / 2, screenH() / 2);
  } else {
    const TranscriptEntry* entry = historyAt(historyCursor);
    if (entry) {
      uint32_t age = (millis() - entry->arrivedAtMs) / 1000;
      char ageBuf[24];
      snprintf(ageBuf, sizeof(ageBuf), "%lus ago", static_cast<unsigned long>(age));

      StickCP2.Display.setTextDatum(top_left);
      StickCP2.Display.setFont(&fonts::Font0);
      StickCP2.Display.setTextColor(COLOR_MUTED, COLOR_BG);
      StickCP2.Display.drawString(ageBuf, 10, 38);

      drawWrappedBody(entry->text, 50, screenH() - 22, 10, 16, COLOR_FG);
    }
  }

  drawBottomBar("BTN B", COLOR_ACCENT, "TAP=NEXT");
}

static void drawInfoScreen() {
  StickCP2.Display.fillScreen(COLOR_BG);
  drawTopBar(COLOR_ACCENT);

  StickCP2.Display.setTextDatum(top_left);
  StickCP2.Display.setFont(&fonts::Font0);
  StickCP2.Display.setTextColor(COLOR_ACCENT, COLOR_BG);
  StickCP2.Display.drawString("DEVICE INFO", 8, 22);

  StickCP2.Display.drawFastHLine(8, 34, screenW() - 16, COLOR_HAIRLINE);

  int16_t y = 40;
  const int16_t labelX = 10;
  const int16_t valueX = 90;

  auto drawRow = [&](const char* label, const String& value, uint16_t valueColor = COLOR_FG) {
    StickCP2.Display.setTextDatum(top_left);
    StickCP2.Display.setFont(&fonts::Font0);
    StickCP2.Display.setTextColor(COLOR_MUTED, COLOR_BG);
    StickCP2.Display.drawString(label, labelX, y);
    StickCP2.Display.setTextColor(valueColor, COLOR_BG);
    StickCP2.Display.drawString(value, valueX, y);
    y += 12;
  };

  int batteryLevel = StickCP2.Power.getBatteryLevel();
  if (batteryLevel < 0) batteryLevel = 0;
  if (batteryLevel > 100) batteryLevel = 100;
  uint16_t batteryColor = batteryLevel > 30 ? COLOR_OK : batteryLevel > 15 ? COLOR_WARN : COLOR_DANGER;
  bool charging = StickCP2.Power.isCharging();

  drawRow("BATT", String(batteryLevel) + "%" + (charging ? " CHG" : ""), batteryColor);
  drawRow("LINK", phoneConnected ? "CONNECTED" : "ADVERTISING",
          phoneConnected ? COLOR_OK : COLOR_MUTED);
  drawRow("UP",   formatUptime(millis() - bootMs));
  drawRow("BRT",  String(BRIGHTNESS_LEVELS[brightnessIndex]) + " / 255");
  drawRow("HIST", String(historyCount) + " / " + String(HISTORY_SIZE));

  drawBottomBar("B·BRT", COLOR_ACCENT, FIRMWARE_VERSION);
}

static void drawLiveScreen() {
  switch (appState) {
    case AppState::Recording:
      drawRecordingScreen();
      return;
    case AppState::Transcript:
      drawTranscriptScreen();
      return;
    default:
      break;
  }

  StickCP2.Display.fillScreen(COLOR_BG);
  uint16_t accent = stateAccent(appState);
  drawTopBar(accent);

  String subtitle;
  switch (appState) {
    case AppState::Standby:   subtitle = "awaiting host"; break;
    case AppState::Ready:     subtitle = "hold btn a";    break;
    case AppState::Uploading: subtitle = "awaiting text"; break;
    case AppState::Fault:     subtitle = currentFault.length() ? currentFault : "see phone"; break;
    default:                  subtitle = phoneStateLabel;  break;
  }

  drawCenteredHeadline(stateLabel(appState), subtitle, accent);
  drawStateLine(accent);
}

static void render() {
  switch (currentView) {
    case View::Live:    drawLiveScreen();    break;
    case View::History: drawHistoryScreen(); break;
    case View::Info:    drawInfoScreen();    break;
  }
  needsRedraw = false;
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
    requestRedraw();
    return;
  }

  if (line.startsWith("ERR:")) {
    currentFault = line.substring(4);
    transition(AppState::Fault);
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
    return;
  }

  streaming = true;
  recordingStartMs = millis();
  notifyControl("START\n");
  transition(AppState::Recording);
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

// We only use isPressed() because higher-level helpers (wasPressed/wasClicked
// etc.) crash with this lib version of M5Unified. We track edges and gestures
// ourselves from the raw boolean state.

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
  // NOTE: StickCP2.BtnA reference is broken in M5StickCPlus2 1.0.2 (zero-init
  // in BSS, default ctor never runs at startup). Access M5.BtnA directly.
  ButtonEvent a = updateButton(btnA, M5.BtnA.isPressed());
  ButtonEvent b = updateButton(btnB, M5.BtnB.isPressed());

  // ---- BtnA: hold-to-talk + double-click lock ----
  switch (a) {
    case ButtonEvent::Press:
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

  // ---- BtnB: cycle view / dismiss / brightness ----
  switch (b) {
    case ButtonEvent::Click:
      if (currentView == View::History && historyCount > 1) {
        historyCursor = (historyCursor + 1) % historyCount;
        requestRedraw();
      } else {
        cycleView();
      }
      break;
    case ButtonEvent::DoubleClick:
      cycleBrightness();
      break;
    case ButtonEvent::LongPress:
      currentTranscript = "";
      currentFault = "";
      if (phoneConnected && !streaming) {
        transition(AppState::Ready);
      } else if (!phoneConnected) {
        transition(AppState::Standby);
      }
      setView(View::Live);
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
  StickCP2.Display.setRotation(1);
  StickCP2.Display.setTextDatum(middle_center);
  setBrightnessIndex(brightnessIndex);

  StickCP2.Speaker.end();
  StickCP2.Mic.begin();

  bootMs = millis();
  transition(AppState::Standby);
  startBle();
}

void loop() {
  StickCP2.update();

  handleButtons();

  // Capture audio in Recording state
  if (streaming && phoneConnected && StickCP2.Mic.isEnabled()) {
    if (StickCP2.Mic.record(audioBuffer, SAMPLES_PER_CHUNK, SAMPLE_RATE)) {
      notifyAudio(reinterpret_cast<const uint8_t*>(audioBuffer), sizeof(audioBuffer));
    }
  }

  // Process incoming control lines from phone
  String line;
  if (popPendingControlLine(line)) {
    handleIncomingLine(line);
  }

  // Auto-return to Live view after inactivity in History/Info
  if (currentView != View::Live && millis() - lastViewChangeMs > VIEW_AUTO_RETURN_MS) {
    setView(View::Live);
  }

  // Throttled redraw for animated views
  uint32_t now = millis();
  bool animating = (currentView == View::Live && appState == AppState::Recording) ||
                   (currentView == View::Info);
  if (needsRedraw || (animating && now - lastDrawMs > REDRAW_INTERVAL_MS)) {
    render();
  }

  delay(2);
}
