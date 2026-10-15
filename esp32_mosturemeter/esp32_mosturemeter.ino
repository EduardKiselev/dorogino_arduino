// soil_moisture_sensor.ino

#include <WiFi.h>
#include <HTTPClient.h>

// ============ КОНФИГУРАЦИЯ ============

const char* DEVICE_ID = "soil-sensor-01";
const char* DEVICE_TYPE = "soil_moisture_sensor";

// WiFi
const char* ssid = "ELTEX-8478";
const char* pass = "eSm-kp7-VdF-PtA";

//const char* ssid = "ElSenorNegro";
//const char* pass = "pomidorka38";

// Сервер
const char* serverHost = "192.168.1.100";
const int serverPort = 5000;

// Endpoints
const char* HEARTBEAT_ENDPOINT = "/api/heartbeat";
const char* DATA_ENDPOINT = "/api/data";

// Пин датчика
const int SOIL_SENSOR_PIN = 34;

// Калибровка датчика влажности почвы
const int DRY_VALUE = 3000;
const int WET_VALUE = 950;

// Усреднение
const int BUFFER_SIZE = 20; 

// Интервалы
const unsigned long MEASUREMENT_INTERVAL_MS = 500;
const unsigned long HEARTBEAT_INTERVAL_MS = 60000;
const unsigned long DATA_SEND_INTERVAL_MS = 60000;  // Отправка данных каждые 60 секунд
const unsigned long RECONNECT_TIMEOUT_MS = 10000;

// ============ ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ============

bool isReconnecting = false;
unsigned long lastReconnectAttempt = 0;
int failedHeartbeats = 0;

unsigned long lastMeasurement = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastDataSend = 0;

// Буфер усреднения  // ← ДОБАВИТЬ ЭТОТ БЛОК
int soilReadings[BUFFER_SIZE];
int readIndex = 0;
long soilTotal = 0;
bool bufferFull = false;

// Текущие показания
int currentMoisture = 0;
int currentSoilRaw = 0;

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

// Отправка данных датчиков
void sendData() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[DATA] Skipped: Wi-Fi not connected.");
    return;
  }

  String json = "{"
                "\"puid\":\"" + sessionPUID + "|" + String(millis()) + "\","
                "\"device_type\":\"" + String(DEVICE_TYPE) + "\","
                "\"device_id\":\"" + String(DEVICE_ID) + "\","
                "\"interval\":\"" + String(DATA_SEND_INTERVAL_MS) + "\","
                "\"info\":{"
                  "\"data_type\":\"moisture\","
                  "\"moisture\":" + String(currentMoisture) + ","
                  "\"soil_raw\":" + String(currentSoilRaw) +
                "}"
                "}";

  String url = "http://" + String(serverHost) + ":" + String(serverPort) + DATA_ENDPOINT;
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  
  int httpCode = http.POST(json);
  
  if (httpCode > 0) {
    Serial.printf("[DATA] Sent | Code: %d | Moisture: %d%%\n", httpCode, currentMoisture);
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
  Serial.println("\n[INIT] ESP32 Soil Moisture Sensor booting...");

  // Инициализация буфера нулями  // ← ДОБАВИТЬ
  for (int i = 0; i < BUFFER_SIZE; i++) {
    soilReadings[i] = 0;
  }

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
  
  // Чтение датчика каждые 500мс с усреднением
  if (millis() - lastMeasurement >= MEASUREMENT_INTERVAL_MS) {
    int newReading = analogRead(SOIL_SENSOR_PIN);
    
    // Обновление скользящего среднего
    soilTotal = soilTotal - soilReadings[readIndex];
    soilReadings[readIndex] = newReading;
    soilTotal = soilTotal + soilReadings[readIndex];
    
    readIndex++;
    if (readIndex >= BUFFER_SIZE) {
      readIndex = 0;
      bufferFull = true;
    }
    
    // Вычисление среднего
    int count = bufferFull ? BUFFER_SIZE : readIndex;
    currentSoilRaw = soilTotal / count;
    currentMoisture = map(currentSoilRaw, DRY_VALUE, WET_VALUE, 0, 100);
    currentMoisture = constrain(currentMoisture, 0, 100);
    
    // Вывод в консоль
    Serial.printf("[READ] Raw: %d | Avg: %d | Moisture: %d%% | Count: %d/%d\n", 
                  newReading, currentSoilRaw, currentMoisture, count, BUFFER_SIZE);
    
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