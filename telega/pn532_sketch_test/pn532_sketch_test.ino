#include <Wire.h>
#include <Adafruit_PN532.h>
#include <ArduinoJson.h>

#define IRQ_PIN   (2)
#define RESET_PIN (3)

Adafruit_PN532 nfc(IRQ_PIN, RESET_PIN);

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  if (!nfc.begin()) {
    Serial.println("{\"type\":\"error\",\"message\":\"PN532 not found\"}");
    while (1);
  }

  nfc.SAMConfig();
}

void loop() {
  uint8_t success;
  uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };
  uint8_t uidLength;

  success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 1000);

  if (success) {
    // Собираем UID в строку вида "04A1B2C3D4E5F6"
    String uidStr = "";
    for (uint8_t i = 0; i < uidLength; i++) {
      if (uid[i] < 0x10) uidStr += "0";
      uidStr += String(uid[i], HEX);
    }
    uidStr.toUpperCase();

    // Формируем JSON
    JsonDocument doc;
    doc["type"] = "rfid";
    doc["uid"] = uidStr;

    // Отправляем в Serial одной строкой
    serializeJson(doc, Serial);
    Serial.println();

    delay(500); // Антидребезг
  }
}