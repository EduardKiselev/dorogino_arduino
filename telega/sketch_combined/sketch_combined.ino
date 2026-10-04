#include <Wire.h>
#include <Adafruit_PN532.h>
#include <ArduinoJson.h>
#include <HX711.h>

// ========================= PIN MAP =========================
// I2C для PN532
#define I2C_SDA_PIN          21
#define I2C_SCL_PIN          22

// PN532 IRQ/RESET.
// Если у тебя уже собрано на GPIO2/GPIO3 и стабильно работает,
// можно оставить:
// #define PN532_IRQ_PIN        2
// #define PN532_RESET_PIN      3
//
// Но для новой сборки лучше использовать более безопасные GPIO.
#define PN532_IRQ_PIN        25
#define PN532_RESET_PIN      26

// HX711
#define LOADCELL_DOUT_PIN    4
#define LOADCELL_SCK_PIN     5
// ===========================================================

// Тайминги и период отправки
#define RFID_POLL_TIMEOUT_MS 50
#define RFID_RESEND_MS       700
#define LOADCELL_SEND_MS     100

Adafruit_PN532 nfc(PN532_IRQ_PIN, PN532_RESET_PIN);
HX711 scale;

bool pn532Ready = false;
bool hx711Ready = false;

unsigned long lastLoadcellSendMs = 0;
unsigned long lastRfidSendMs = 0;
String lastUid = "";

void sendBootStatus() {
  JsonDocument doc;
  doc["type"] = "boot";
  doc["pn532"] = pn532Ready;
  doc["hx711"] = hx711Ready;

  serializeJson(doc, Serial);
  Serial.println();
}

void sendModuleStatus(const char* module, bool ok) {
  JsonDocument doc;
  doc["type"] = "status";
  doc[module] = ok;

  serializeJson(doc, Serial);
  Serial.println();
}

void sendError(const char* module, const char* message) {
  JsonDocument doc;
  doc["type"] = "error";
  doc["module"] = module;
  doc["message"] = message;

  serializeJson(doc, Serial);
  Serial.println();
}

void initPN532() {
  pn532Ready = nfc.begin();

  if (pn532Ready) {
    nfc.SAMConfig();
  } else {
    sendError("pn532", "PN532 not found");
  }
}

void initHX711() {
  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);

  unsigned long start = millis();

  // Ждем готовности тензодатчика максимум 2 секунды.
  while (!scale.is_ready() && millis() - start < 2000) {
    delay(10);
  }

  hx711Ready = scale.is_ready();

  if (!hx711Ready) {
    sendError("hx711", "HX711 not ready");
  }
}

void pollPN532() {
  static unsigned long lastRetryMs = 0;

  if (!pn532Ready) {
    // Редкие повторные попытки оживить PN532.
    if (millis() - lastRetryMs >= 5000) {
      lastRetryMs = millis();

      pn532Ready = nfc.begin();

      if (pn532Ready) {
        nfc.SAMConfig();
        sendModuleStatus("pn532", true);
      }
    }

    return;
  }

  uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };
  uint8_t uidLength = 0;

  bool success = nfc.readPassiveTargetID(
    PN532_MIFARE_ISO14443A,
    uid,
    &uidLength,
    RFID_POLL_TIMEOUT_MS
  );

  if (!success) {
    return;
  }

  String uidStr = "";

  for (uint8_t i = 0; i < uidLength; i++) {
    if (uid[i] < 0x10) {
      uidStr += "0";
    }

    uidStr += String(uid[i], HEX);
  }

  uidStr.toUpperCase();

  bool changed = uidStr != lastUid;
  bool expired = millis() - lastRfidSendMs >= RFID_RESEND_MS;

  if (changed || expired) {
    JsonDocument doc;
    doc["type"] = "rfid";
    doc["uid"] = uidStr;

    serializeJson(doc, Serial);
    Serial.println();

    lastUid = uidStr;
    lastRfidSendMs = millis();
  }
}

void pollHX711() {
  static unsigned long lastRetryMs = 0;

  if (!hx711Ready) {
    if (millis() - lastRetryMs >= 2000) {
      lastRetryMs = millis();

      if (scale.is_ready()) {
        hx711Ready = true;
        sendModuleStatus("hx711", true);
      }
    }

    return;
  }

  if (millis() - lastLoadcellSendMs < LOADCELL_SEND_MS) {
    return;
  }

  if (!scale.is_ready()) {
    return;
  }

  long raw = scale.read();

  JsonDocument doc;
  doc["type"] = "loadcell";
  doc["raw"] = raw;

  serializeJson(doc, Serial);
  Serial.println();

  lastLoadcellSendMs = millis();
}

void setup() {
  Serial.begin(115200);

  // Ждём, пока USB-serial поднимется
  delay(1000);
  
  // Очистим буфер от мусора boot-лога
  while (Serial.available()) {
    Serial.read();
  }

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  initPN532();
  initHX711();

  sendBootStatus();
}

void loop() {
  pollPN532();
  pollHX711();
}