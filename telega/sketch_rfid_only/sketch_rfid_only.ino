#include <Wire.h>
#include <Adafruit_PN532.h>
#include <ArduinoJson.h>

// ========================= PIN MAP =========================
// I2C для PN532
#define I2C_SDA_PIN          21
#define I2C_SCL_PIN          22

// PN532 IRQ/RESET
#define PN532_IRQ_PIN        25
#define PN532_RESET_PIN      26
// ===========================================================

// Тайминги
#define RFID_POLL_TIMEOUT_MS 50
#define RFID_RESEND_MS       700

Adafruit_PN532 nfc(PN532_IRQ_PIN, PN532_RESET_PIN);

bool pn532Ready = false;

unsigned long lastRfidSendMs = 0;
String lastUid = "";

void sendBootStatus() {
  JsonDocument doc;
  doc["type"] = "boot";
  doc["pn532"] = pn532Ready;

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

void pollPN532() {
  static unsigned long lastRetryMs = 0;

  if (!pn532Ready) {
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

void setup() {
  Serial.begin(115200);

  delay(1000);

  while (Serial.available()) {
    Serial.read();
  }

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  initPN532();

  sendBootStatus();
}

void loop() {
  pollPN532();
}
