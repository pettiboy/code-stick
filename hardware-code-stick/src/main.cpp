#include <Arduino.h>
#include <M5StickCPlus2.h>
#include <NimBLEDevice.h>

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

static void drawCentered(const String& title, const String& body, uint16_t accent) {
  StickCP2.Display.clear(TFT_BLACK);
  StickCP2.Display.setTextDatum(middle_center);
  StickCP2.Display.setTextColor(accent, TFT_BLACK);
  StickCP2.Display.setFont(&fonts::Font4);
  StickCP2.Display.drawString(title, StickCP2.Display.width() / 2, 28);

  StickCP2.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  StickCP2.Display.setFont(&fonts::Font2);
  StickCP2.Display.drawString(body, StickCP2.Display.width() / 2, 66);
}

static void drawTranscript(const String& text) {
  StickCP2.Display.clear(TFT_BLACK);
  StickCP2.Display.setTextDatum(top_left);
  StickCP2.Display.setTextColor(TFT_GREEN, TFT_BLACK);
  StickCP2.Display.setFont(&fonts::Font2);
  StickCP2.Display.drawString("Transcript", 6, 4);

  StickCP2.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  const int16_t lineHeight = 18;
  int16_t x = 6;
  int16_t y = 26;
  String word;

  for (size_t i = 0; i <= text.length(); ++i) {
    const char ch = i < text.length() ? text[i] : ' ';
    if (ch != ' ' && ch != '\n') {
      word += ch;
      continue;
    }

    if (word.length()) {
      int16_t width = StickCP2.Display.textWidth(word + " ");
      if (x + width > StickCP2.Display.width() - 6) {
        x = 6;
        y += lineHeight;
      }
      if (y + lineHeight > StickCP2.Display.height()) {
        break;
      }
      StickCP2.Display.drawString(word + " ", x, y);
      x += width;
      word = "";
    }

    if (ch == '\n') {
      x = 6;
      y += lineHeight;
    }
  }
}

static void notifyControl(const char* message) {
  if (!phoneConnected || controlCharacteristic == nullptr) {
    return;
  }
  controlCharacteristic->setValue(reinterpret_cast<const uint8_t*>(message), strlen(message));
  controlCharacteristic->notify();
}

static void handleControlLine(const String& line) {
  if (line.startsWith("TEXT:")) {
    drawTranscript(line.substring(5));
  } else if (line.startsWith("STATE:")) {
    drawCentered("Phone", line.substring(6), TFT_CYAN);
  } else if (line.startsWith("ERR:")) {
    drawCentered("Error", line.substring(4), TFT_RED);
  }
}

class ControlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic) override {
    std::string value = characteristic->getValue();
    for (char ch : value) {
      if (ch == '\n') {
        handleControlLine(controlRxBuffer);
        controlRxBuffer = "";
      } else if (controlRxBuffer.length() < 480) {
        controlRxBuffer += ch;
      }
    }
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*) override {
    phoneConnected = true;
    drawCentered("Connected", "Hold Btn A + speak", TFT_GREEN);
  }

  void onDisconnect(NimBLEServer*) override {
    phoneConnected = false;
    streaming = false;
    drawCentered("Waiting", "Open phone app", TFT_ORANGE);
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
    drawCentered("No Phone", "Connect first", TFT_RED);
    return;
  }

  streaming = true;
  notifyControl("START\n");
  drawCentered("Listening", "Keep holding Btn A", TFT_RED);
  delay(180);
}

static void stopStreaming() {
  streaming = false;
  while (StickCP2.Mic.isRecording()) {
    delay(1);
  }
  notifyControl("STOP\n");
  drawCentered("Sent", "Waiting for text", TFT_CYAN);
}

void setup() {
  auto cfg = M5.config();
  StickCP2.begin(cfg);
  StickCP2.Display.setRotation(1);
  StickCP2.Display.setTextDatum(middle_center);
  StickCP2.Display.setBrightness(80);

  StickCP2.Speaker.end();
  StickCP2.Mic.begin();

  drawCentered("Waiting", "Open phone app", TFT_ORANGE);
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

  delay(1);
}
