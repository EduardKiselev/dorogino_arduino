// fs3000_sparkfun.ino

#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SparkFun_FS3000_Arduino_Library.h>

// ============ КОНФИГУРАЦИЯ ============

const char* DEVICE_ID = "wind-sensor-01";
const char* DEVICE_TYPE = "wind_sensor";

// WiFi
const char* ssid = "ELTEX-8478";
const char* pass = "eSm-kp7-VdF-PtA";



// Сервер
const char* serverHost = "192.168.1.100";
const int serverPort = 5000;

// Endpoints
const char* HEARTBEAT_ENDPOINT = "/api/heartbeat";
const char* DATA_ENDPOINT = "/api/data";

// Пины I2C
const int SDA_PIN = 21;
const int SCL_PIN = 22;

// Буфер статистики
const int STATS_BUFFER_SIZE = 20;

// Интервалы
const unsigned long MEASUREMENT_INTERVAL_MS = 1000;
const unsigned long HEARTBEAT_INTERVAL_MS = 60000;
const unsigned long DATA_SEND_INTERVAL_MS = 60000;
const unsigned long RECONNECT_TIMEOUT_MS = 10000;

// ============ ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ============

FS3000 sensor;

bool isReconnecting = false;
unsigned long lastReconnectAttempt = 0;
int failedHeartbeats = 0;

unsigned long lastMeasurement = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastDataSend = 0;

// Текущие показания
float currentVelocityMps = 0;
float currentVelocityMph = 0;
uint16_t currentRaw = 0;

// Буфер для статистики
float mpsBuffer[STATS_BUFFER_SIZE];
int bufferIndex = 0;
bool bufferFull = false;

// PUID сессии
String sessionPUID = "";

// ============ ФУНКЦИИ ============

void checkWiFiConnection() {
  if (WiFi.status() == WL_CONNECTED) {
    isReconnecting = false;
    return;
  }

  if (!isReconnecting) {
    Serial.println("[WIFI] Connection lost! Forcing full reset...");
    WiFi.disconnect(true);
    delay(200);
    WiFi.mode(WIFI_STA);
    
    char hostname[32];
    snprintf(hostname, sizeof(hostname), "esp32-%s", DEVICE_ID);
    WiFi.setHostname(hostname);
    
    WiFi.begin(ssid, pass);
    isReconnecting = true;
    lastReconnectAttempt = millis();
    Serial.println("[WIFI] Reconnect initiated...");
  }

  if (isReconnecting && millis() - lastReconnectAttempt > RECONNECT_TIMEOUT_MS) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] Reconnect timeout. Will retry...");
      isReconnecting = false;
    } else {
      Serial.print("[WIFI] Reconnected! IP: ");
      Serial.println(WiFi.localIP());
      isReconnecting = false;
      failedHeartbeats = 0;
    }
  }
}

// Добавление значения в буфер
void addToBuffer(float value) {
  mpsBuffer[bufferIndex] = value;
  bufferIndex++;
  if (bufferIndex >= STATS_BUFFER_SIZE) {
    bufferIndex = 0;
    bufferFull = true;
  }
}

// Вычисление максимума
float calculateMax() {
  int count = bufferFull ? STATS_BUFFER_SIZE : bufferIndex;
  if (count == 0) return 0;
  
  float maxVal = mpsBuffer[0];
  for (int i = 1; i < count; i++) {
    if (mpsBuffer[i] > maxVal) maxVal = mpsBuffer[i];
  }
  return maxVal;
}

// Вычисление среднего
float calculateAvg() {
  int count = bufferFull ? STATS_BUFFER_SIZE : bufferIndex;
  if (count == 0) return 0;
  
  float sum = 0;
  for (int i = 0; i < count; i++) {
    sum += mpsBuffer[i];
  }
  return sum / count;
}

// Вычисление медианы (с сортировкой)
float calculateMedian() {
  int count = bufferFull ? STATS_BUFFER_SIZE : bufferIndex;
  if (count == 0) return 0;
  
  // Копируем в временный массив для сортировки
  float temp[STATS_BUFFER_SIZE];
  for (int i = 0; i < count; i++) {
    temp[i] = mpsBuffer[i];
  }
  
  // Сортировка пузырьком (для 20 элементов быстро)
  for (int i = 0; i < count - 1; i++) {
    for (int j = 0; j < count - i - 1; j++) {
      if (temp[j] > temp[j + 1]) {
        float t = temp[j];
        temp[j] = temp[j + 1];
        temp[j + 1] = t;
      }
    }
  }
  
  // Медиана
  if (count % 2 == 0) {
    return (temp[count / 2 - 1] + temp[count / 2]) / 2.0;
  } else {
    return temp[count / 2];
  }
}

// Heartbeat - только health информация
void sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HB] Skipped: Wi-Fi not connected.");
    return;
  }

  String json = "{"
                "\"device_type\":\"" + String(DEVICE_TYPE) + "\","
                "\"device_id\":\"" + String(DEVICE_ID) + "\","
                "\"health\":{"
                  "\"rssi\":" + String(WiFi.RSSI()) + ","
                  "\"heap\":" + String(ESP.getFreeHeap()) + ","
                  "\"uptime\":" + String(millis()) +
                "}"
                "}";

  String url = "http://" + String(serverHost) + ":" + String(serverPort) + HEARTBEAT_ENDPOINT;
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  
  int httpCode = http.POST(json);
  
  if (httpCode > 0) {
    Serial.printf("[HB] Sent | Code: %d\n", httpCode);
    failedHeartbeats = 0;
  } else {
    Serial.printf("[HB] Failed | Error: %s\n", http.errorToString(httpCode).c_str());
    failedHeartbeats++;
    
    if (failedHeartbeats >= 3) {
      Serial.println("[WIFI] 3 consecutive HB failures. Forcing reconnect...");
      WiFi.disconnect(true);
      isReconnecting = false;
      failedHeartbeats = 0;
    }
  }
  http.end();
}

// Отправка данных датчиков со статистикой
void sendData() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[DATA] Skipped: Wi-Fi not connected.");
    return;
  }

  // Вычисление статистики
  float maxMps = calculateMax();
  float avgMps = calculateAvg();
  float medianMps = calculateMedian();
  int count = bufferFull ? STATS_BUFFER_SIZE : bufferIndex;

  String json = "{"
                "\"puid\":\"" + sessionPUID + "|" + String(millis()) + "\","
                "\"device_type\":\"" + String(DEVICE_TYPE) + "\","
                "\"device_id\":\"" + String(DEVICE_ID) + "\","
                "\"interval\":\"" + String(DATA_SEND_INTERVAL_MS) + "\","
                "\"info\":{"
                  "\"data_type\":\"speed\","
                  "\"data\":" + String(currentVelocityMps, 2) + ","
                  "\"max_mps\":" + String(maxMps, 2) + ","
                  "\"avg_mps\":" + String(avgMps, 2) + ","
                  "\"median_mps\":" + String(medianMps, 2) +
                "}"
                "}";

  String url = "http://" + String(serverHost) + ":" + String(serverPort) + DATA_ENDPOINT;
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  
  int httpCode = http.POST(json);
  
  if (httpCode > 0) {
    Serial.printf("[DATA] Sent | Code: %d | Wind: %.2f m/s | Max: %.2f | Avg: %.2f | Median: %.2f\n", 
                  httpCode, currentVelocityMps, maxMps, avgMps, medianMps);
  } else {
    Serial.printf("[DATA] Failed | Error: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
}

String generateSessionPUID() {
  char buf[16];
  snprintf(buf, sizeof(buf), "%08X", esp_random());
  return String(buf);
}

// ============ SETUP ============

void setup() {
  Serial.begin(9600);
  Serial.println("\n[INIT] ESP32 Wind Sensor booting...");

  // Инициализация буфера
  for (int i = 0; i < STATS_BUFFER_SIZE; i++) {
    mpsBuffer[i] = 0;
  }

  // Инициализация I2C и датчика
  Wire.begin(SDA_PIN, SCL_PIN);
  
  if (sensor.begin() == false) {
    Serial.println("[ERROR] Sensor not found! Check wiring.");
    while(1);
  }
  
  sensor.setRange(AIRFLOW_RANGE_15_MPS);
  Serial.println("[SENSOR] FS3000-1015 initialized (0-15 m/s)");

  // WiFi подключение
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);

  // Генерация PUID для текущей сессии
  sessionPUID = generateSessionPUID();
  Serial.print("Session PUID: ");
  Serial.println(sessionPUID);

  Serial.printf("[WIFI] Connecting to '%s'...", ssid);
  WiFi.begin(ssid, pass);
  
  unsigned long connectStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - connectStart < 15000) {
    delay(500);
    Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WIFI] Initial connection failed. Will retry in loop.");
  }
}

// ============ LOOP ============

void loop() {
  checkWiFiConnection();
  
  // Чтение датчика каждые 1000мс
  if (millis() - lastMeasurement >= MEASUREMENT_INTERVAL_MS) {
    currentVelocityMps = sensor.readMetersPerSecond();
    currentVelocityMph = sensor.readMilesPerHour();
    currentRaw = sensor.readRaw();
    
    // Добавление в буфер статистики
    addToBuffer(currentVelocityMps);
    
    Serial.printf("[READ] Raw: %d | Velocity: %.2f m/s (%.2f mph)\n", 
                  currentRaw, currentVelocityMps, currentVelocityMph);
    
    lastMeasurement = millis();
  }
  
  // Heartbeat раз в минуту
  if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    sendHeartbeat();
    lastHeartbeat = millis();
  }
  
  // Отправка данных каждые 60 секунд
  if (millis() - lastDataSend >= DATA_SEND_INTERVAL_MS) {
    sendData();
    lastDataSend = millis();
  }
}