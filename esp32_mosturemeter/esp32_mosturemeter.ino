cd const int SENSOR_PIN = 34;

// Калибровочные значения (ЗАМЕНИТЕ НА СВОИ после калибровки!)
const int DRY_VALUE = 3000;
const int WET_VALUE = 1000;

// Буфер для скользящего среднего
const int BUFFER_SIZE = 100;
int readings[BUFFER_SIZE];
int readIndex = 0;
long total = 0;
bool bufferFull = false;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Датчик влажности почвы ===");
  Serial.println("Усреднение по 100 замерам (каждые 500мс)");
  Serial.println("----------------------------------------");
  
  // Инициализация буфера нулями
  for (int i = 0; i < BUFFER_SIZE; i++) {
    readings[i] = 0;
  }
}

void loop() {
  // Читаем новое значение
  int newReading = analogRead(SENSOR_PIN);
  
  // Вычитаем старое значение из суммы
  total = total - readings[readIndex];
  
  // Сохраняем новое значение в буфер
  readings[readIndex] = newReading;
  
  // Добавляем новое значение в сумму
  total = total + readings[readIndex];
  
  // Переходим к следующей позиции в буфере
  readIndex++;
  if (readIndex >= BUFFER_SIZE) {
    readIndex = 0;
    bufferFull = true;
  }
  
  // Вычисляем среднее значение
  int count = bufferFull ? BUFFER_SIZE : readIndex;
  int average = total / count;
  
  // Преобразуем в проценты влажности
  int moisture = map(average, DRY_VALUE, WET_VALUE, 0, 100);
  moisture = constrain(moisture, 0, 100);
  
  // Вывод результата  
  Serial.print("Последнее: ");
  Serial.print(newReading);
  Serial.print(" | Среднее: ");
  Serial.print(average);
  Serial.print(" | Влажность: ");
  Serial.print(moisture);
  Serial.print("% | Замеров: ");
  Serial.println(count);
  
  delay(500); // Опрос каждые 500мс
}