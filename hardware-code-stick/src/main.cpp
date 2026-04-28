#include <Arduino.h>
#include <M5StickCPlus2.h>
#include <NimBLEDevice.h>
#include <mbedtls/base64.h>

static constexpr const char* DEVICE_NAME = "M5VoiceStick";

static const BLEUUID SERVICE_UUID("3e7a0001-e33b-4e2f-9a85-f03e1d33c001");
static const BLEUUID AUDIO_UUID("3e7a0002-e33b-4e2f-9a85-f03e1d33c001");
static const BLEUUID CONTROL_UUID("3e7a0003-e33b-4e2f-9a85-f03e1d33c001");

static constexpr uint32_t SAMPLE_RATE = 16000;
static constexpr size_t SAMPLES_PER_CHUNK = 240;
static constexpr size_t BLE_PAYLOAD_BYTES = 180;

static int16_t audioBuffer[SAMPLES_PER_CHUNK];
static NimBLECharacteristic* audioCharacteristic = nullptr;
static NimBLECharacteristic* controlCharacteristic = nullptr;

static bool phoneConnected = false;
static bool streaming = false;
static String controlRxBuffer;
static portMUX_TYPE controlMux = portMUX_INITIALIZER_UNLOCKED;
static String pendingControlLine;
static volatile bool hasPendingControlLine = false;

static constexpr uint16_t COLOR_BG       = 0x0000;  // near-black
static constexpr uint16_t COLOR_FG       = 0xEF3B;  // warm off-white
static constexpr uint16_t COLOR_FG_DIM   = 0xBDB6;  // softened text
static constexpr uint16_t COLOR_MUTED    = 0x7C0F;  // muted gray
static constexpr uint16_t COLOR_HAIRLINE = 0x39E7;  // dim rule
static constexpr uint16_t COLOR_ACCENT   = 0xD7E8;  // signal lime #D6FF45
static constexpr uint16_t COLOR_DANGER   = 0xFAA7;  // hot orange-red
static constexpr uint16_t COLOR_OK       = 0x37D5;  // mint

static int16_t screenW() { return StickCP2.Display.width(); }
static int16_t screenH() { return StickCP2.Display.height(); }

static void drawCornerCrosshair(int16_t x, int16_t y, int16_t dx, int16_t dy, uint16_t color) {
  StickCP2.Display.drawLine(x, y, x + 6 * dx, y, color);
  StickCP2.Display.drawLine(x, y, x, y + 6 * dy, color);
}

static void drawChrome(uint16_t accentColor) {
  StickCP2.Display.fillScreen(COLOR_BG);

  drawCornerCrosshair(2, 2, 1, 1, accentColor);
  drawCornerCrosshair(screenW() - 3, 2, -1, 1, accentColor);
  drawCornerCrosshair(2, screenH() - 3, 1, -1, accentColor);
  drawCornerCrosshair(screenW() - 3, screenH() - 3, -1, -1, accentColor);

  StickCP2.Display.drawFastHLine(8, 16, screenW() - 16, COLOR_HAIRLINE);
  StickCP2.Display.drawFastHLine(8, screenH() - 17, screenW() - 16, COLOR_HAIRLINE);

  StickCP2.Display.setTextDatum(top_left);
  StickCP2.Display.setFont(&fonts::Font0);
  StickCP2.Display.setTextSize(1);
  StickCP2.Display.setTextColor(accentColor, COLOR_BG);
  StickCP2.Display.drawString("M5VS", 8, 4);

  StickCP2.Display.setTextColor(COLOR_MUTED, COLOR_BG);
  StickCP2.Display.drawString("NODE 01", 36, 4);

  const int16_t barsX = screenW() - 30;
  const int16_t bandY = 11;
  const uint16_t onColor = phoneConnected ? COLOR_ACCENT : streaming ? COLOR_ACCENT : COLOR_HAIRLINE;
  const int activeBars = phoneConnected ? 5 : streaming ? 3 : 1;
  for (int i = 0; i < 5; ++i) {
    const int16_t height = 2 + i * 2;
    const int16_t x = barsX + i * 4;
    const int16_t y = bandY - height;
    StickCP2.Display.fillRect(x, y, 3, height, i < activeBars ? onColor : COLOR_HAIRLINE);
  }
}

static void drawFooter(const char* leftLabel, uint16_t leftColor, const char* rightLabel) {
  StickCP2.Display.setTextDatum(top_left);
  StickCP2.Display.setFont(&fonts::Font0);
  StickCP2.Display.setTextSize(1);
  StickCP2.Display.setTextColor(leftColor, COLOR_BG);
  StickCP2.Display.drawString(leftLabel, 8, screenH() - 12);

  if (rightLabel) {
    StickCP2.Display.setTextColor(COLOR_MUTED, COLOR_BG);
    StickCP2.Display.setTextDatum(top_right);
    StickCP2.Display.drawString(rightLabel, screenW() - 8, screenH() - 12);
  }
}

static void drawCentered(const String& title, const String& body, uint16_t accent) {
  drawChrome(accent);

  StickCP2.Display.setTextDatum(middle_center);
  StickCP2.Display.setFont(&fonts::FreeMonoBold12pt7b);
  StickCP2.Display.setTextSize(1);
  String upper = title;
  upper.toUpperCase();
  StickCP2.Display.setTextColor(accent, COLOR_BG);
  StickCP2.Display.drawString(upper, screenW() / 2, screenH() / 2 - 14);

  const int16_t markerY = screenH() / 2 + 4;
  StickCP2.Display.drawFastHLine(screenW() / 2 - 14, markerY, 28, accent);

  StickCP2.Display.setFont(&fonts::Font2);
  StickCP2.Display.setTextColor(COLOR_FG_DIM, COLOR_BG);
  StickCP2.Display.drawString(body, screenW() / 2, screenH() / 2 + 22);

  drawFooter(streaming ? "REC" : phoneConnected ? "STBY" : "OFFLINE",
             streaming ? COLOR_DANGER : phoneConnected ? COLOR_ACCENT : COLOR_MUTED,
             "16K MONO");
}

static void drawTranscript(const String& text) {
  drawChrome(COLOR_ACCENT);

  StickCP2.Display.setTextDatum(top_left);
  StickCP2.Display.setFont(&fonts::Font0);
  StickCP2.Display.setTextSize(1);
  StickCP2.Display.setTextColor(COLOR_ACCENT, COLOR_BG);
  StickCP2.Display.drawString("TRANSCRIPT", 8, 22);

  StickCP2.Display.setTextDatum(top_right);
  StickCP2.Display.setTextColor(COLOR_MUTED, COLOR_BG);
  String wordCount = String(text.length()) + "C";
  StickCP2.Display.drawString(wordCount.c_str(), screenW() - 8, 22);

  StickCP2.Display.drawFastHLine(8, 34, screenW() - 16, COLOR_HAIRLINE);

  StickCP2.Display.setTextDatum(top_left);
  StickCP2.Display.setFont(&fonts::FreeMonoBold9pt7b);
  StickCP2.Display.setTextSize(1);
  StickCP2.Display.setTextColor(COLOR_FG, COLOR_BG);

  const int16_t lineHeight = 16;
  const int16_t marginX = 10;
  const int16_t startY = 40;
  const int16_t maxY = screenH() - 22;
  int16_t x = marginX;
  int16_t y = startY;
  String word;

  for (size_t i = 0; i <= text.length(); ++i) {
    const char ch = i < text.length() ? text[i] : ' ';
    if (ch != ' ' && ch != '\n') {
      word += ch;
      continue;
    }

    if (word.length()) {
      const int16_t width = StickCP2.Display.textWidth(word + " ");
      if (x + width > screenW() - marginX) {
        x = marginX;
        y += lineHeight;
      }
      if (y + lineHeight > maxY) {
        StickCP2.Display.setTextColor(COLOR_MUTED, COLOR_BG);
        StickCP2.Display.setFont(&fonts::Font0);
        StickCP2.Display.drawString("...", x, y + 2);
        break;
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

  drawFooter("OK", COLOR_ACCENT, "GPT-4O");
}

static void notifyControl(const char* message) {
  if (!phoneConnected || controlCharacteristic == nullptr) {
    return;
  }
  controlCharacteristic->setValue(reinterpret_cast<const uint8_t*>(message), strlen(message));
  controlCharacteristic->notify();
}

static void renderControlLine(const String& line) {
  Serial.printf("[ctrl<-] %s\n", line.c_str());
  if (line.startsWith("TEXT:")) {
    drawTranscript(line.substring(5));
  } else if (line.startsWith("STATE:")) {
    String label = line.substring(6);
    String upper = label;
    upper.toUpperCase();
    drawCentered(upper, "phone telemetry", COLOR_ACCENT);
  } else if (line.startsWith("ERR:")) {
    drawCentered("FAULT", line.substring(4), COLOR_DANGER);
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
    const char ch = static_cast<char>(bytes[i]);
    if (ch == '\n') {
      enqueueControlLine(controlRxBuffer);
      controlRxBuffer = "";
    } else if (ch != '\r' && controlRxBuffer.length() < 480) {
      controlRxBuffer += ch;
    }
  }

  if (controlRxBuffer.startsWith("STATE:") || controlRxBuffer.startsWith("TEXT:") || controlRxBuffer.startsWith("ERR:")) {
    enqueueControlLine(controlRxBuffer);
    controlRxBuffer = "";
  }
}

static bool tryParseBase64Control(const std::string& value) {
  uint8_t decoded[512];
  size_t decodedLength = 0;
  const int result = mbedtls_base64_decode(
      decoded,
      sizeof(decoded),
      &decodedLength,
      reinterpret_cast<const uint8_t*>(value.data()),
      value.size());

  if (result != 0 || decodedLength == 0) {
    return false;
  }

  if ((decodedLength >= 5 && memcmp(decoded, "TEXT:", 5) == 0) ||
      (decodedLength >= 6 && memcmp(decoded, "STATE:", 6) == 0) ||
      (decodedLength >= 4 && memcmp(decoded, "ERR:", 4) == 0)) {
    Serial.printf("[ctrl decoded=%u]\n", static_cast<unsigned>(decodedLength));
    parseControlBytes(decoded, decodedLength);
    return true;
  }

  return false;
}

class ControlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic) override {
    std::string value = characteristic->getValue();
    Serial.printf("[ctrl bytes=%u] ", static_cast<unsigned>(value.size()));
    for (uint8_t byte : value) {
      Serial.printf("%02X ", byte);
    }
    Serial.print("| ");
    for (char ch : value) {
      Serial.print(isPrintable(ch) ? ch : '.');
    }
    Serial.println();
    if (!tryParseBase64Control(value)) {
      parseControlBytes(reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*) override {
    Serial.println("[ble] phone connected");
    phoneConnected = true;
    drawCentered("LINKED", "hold btn a", COLOR_ACCENT);
  }

  void onDisconnect(NimBLEServer*) override {
    Serial.println("[ble] phone disconnected");
    phoneConnected = false;
    streaming = false;
    drawCentered("STANDBY", "awaiting host", COLOR_MUTED);
    NimBLEDevice::startAdvertising();
  }
};

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

static void notifyAudio(const uint8_t* bytes, size_t byteCount) {
  if (!phoneConnected || audioCharacteristic == nullptr) {
    return;
  }

  for (size_t offset = 0; offset < byteCount; offset += BLE_PAYLOAD_BYTES) {
    const size_t len = min(BLE_PAYLOAD_BYTES, byteCount - offset);
    audioCharacteristic->setValue(bytes + offset, len);
    audioCharacteristic->notify();
    delay(2);
  }
}

static void startStreaming() {
  if (!phoneConnected) {
    drawCentered("OFFLINE", "host required", COLOR_DANGER);
    return;
  }

  streaming = true;
  notifyControl("START\n");
  drawCentered("CAPTURE", "keep holding", COLOR_DANGER);
  delay(180);
}

static void stopStreaming() {
  streaming = false;
  while (StickCP2.Mic.isRecording()) {
    delay(1);
  }
  notifyControl("STOP\n");
  drawCentered("UPLINK", "awaiting text", COLOR_ACCENT);
}

void setup() {
  Serial.begin(115200);
  Serial.println("[boot] M5 Voice Stick");

  auto cfg = M5.config();
  StickCP2.begin(cfg);
  StickCP2.Display.setRotation(1);
  StickCP2.Display.setTextDatum(middle_center);
  StickCP2.Display.setBrightness(110);

  StickCP2.Speaker.end();
  StickCP2.Mic.begin();

  drawCentered("STANDBY", "awaiting host", COLOR_MUTED);
  startBle();
}

void loop() {
  StickCP2.update();

  const bool pressed = StickCP2.BtnA.isPressed();
  if (pressed && !streaming) {
    startStreaming();
  } else if (!pressed && streaming) {
    stopStreaming();
  }

  if (streaming && phoneConnected && StickCP2.Mic.isEnabled()) {
    if (StickCP2.Mic.record(audioBuffer, SAMPLES_PER_CHUNK, SAMPLE_RATE)) {
      notifyAudio(reinterpret_cast<const uint8_t*>(audioBuffer), sizeof(audioBuffer));
    }
  }

  String line;
  if (popPendingControlLine(line)) {
    renderControlLine(line);
  }

  delay(1);
}
