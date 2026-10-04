#include "HX711.h"

// Пины подключения к ESP32
#define LOADCELL_DOUT_PIN  4
#define LOADCELL_SCK_PIN   5

HX711 scale;

// ================== КАЛИБРОВКА ==================
// Эти два значения нужно заменить после калибровки.

// Сырое значение пустой платформы.
const long ZERO_RAW = -313000L;

// Сколько сырых единиц приходится на 1 грамм.
// Если при нагрузке сырое значение уменьшается,
// коэффициент может быть отрицательным — это нормально.
const float COUNTS_PER_GRAM = 400.0f;

// Количество усредняемых выборок
const int SAMPLES = 10;
// ================================================

void setup() {
  Serial.begin(115200);
  Serial.println("Тест тензодатчика HX711");

  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);

 while (!scale.is_ready()) {
    Serial.println("HX711 не отвечает! Проверь провода.");
    delay(1000);
  }

  Serial.println("Датчик найден. Читаю сырые значения и вес...");
}

void loop() {
  if (scale.is_ready()) {
    long raw = scale.read_average(SAMPLES);

    float grams = 0.0f;

    if (COUNTS_PER_GRAM != 0.0f) {
      grams = (float)(raw - ZERO_RAW) / COUNTS_PER_GRAM;
    }

    // Небольшая зона нуля, чтобы около нуля не прыгали значения
    if (grams > -3.0f && grams < 3.0f) {
      grams = 0.0f;
    }

    float kg = grams / 1000.0f;

    Serial.print("Raw: ");
    Serial.print(raw);
    Serial.print("  Weight: ");
    Serial.print(grams, 1);
    Serial.print(" g = ");
    Serial.print(kg, 3);
    Serial.println(" kg");
  }

  delay(100);
}