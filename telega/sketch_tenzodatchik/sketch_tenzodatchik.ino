#include "HX711.h"

// Пины подключения к ESP32
#define LOADCELL_DOUT_PIN  4
#define LOADCELL_SCK_PIN   5

HX711 scale;

void setup() {
  Serial.begin(115200);
  Serial.println("Тест тензодатчика HX711");

  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);

  if (!scale.is_ready()) {
    Serial.println("HX711 не отвечает! Проверь провода.");
    while (1);
  }

  Serial.println("Датчик найден. Читаю сырые значения...");
  Serial.println("Нажмите на датчик — числа должны меняться.\n");
}

void loop() {
  if (scale.is_ready()) {
    long reading = scale.read();
    Serial.print("Raw: ");
    Serial.println(reading);
  }
  delay(100);
}