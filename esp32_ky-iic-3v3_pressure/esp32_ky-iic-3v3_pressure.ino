#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include "esp_system.h"

// === НАСТРОЙКИ ===
const char* ssid = "ELTEX-8478";
const char* password = "eSm-kp7-VdF-PtA";
const int timeDelay = 10000;  // Интервал между измерениями (мс)
#define SENSOR_ID 1

// I2C пины
#define SDA_PIN 21
#define SCL_PIN 22

// Датчик
const uint8_t SENSOR_ADDR = 0x78;
const uint8_t CMD_TRIGGER = 0xAC;

// === КАЛИБРОВКА ДАВЛЕНИЯ (ДВУХТОЧЕЧНАЯ) ===
const uint32_t RAW_P_MIN = 1624000;  // Среднее значение raw при 0 бар
const uint32_t RAW_P_MAX = 1812000;  // Значение raw при известном давлении
const float PRESSURE_RANGE = 1.0;    // Какое давление подали (в бар)
const float PRESSURE_OFFSET = 0.0;   // 0 бар при RAW_P_MIN

// === КАЛИБРОВКА ТЕМПЕРАТУРЫ ===
const float TEMP_SLOPE = -0.00707f;
const float TEMP_OFFSET = 289.610f;

// === СЕРВЕРЫ ===
const char* servers[] = {"192.168.1.100", "192.168.1.101"};
const int SERVER_COUNT = 2;
const int serverPort = 5000;
const char* endpoint = "/pressure";

// === HEARTBEAT ===
const unsigned long HEARTBEAT_INTERVAL = 60000;
unsigned long lastHeartbeat = 0;
int failedHeartbeats = 0;

// === Wi-Fi reconnect ===
bool isReconnecting = false;
unsigned long lastReconnectAttempt = 0;
const unsigned long reconnectTimeout = 10000;

WiFiClient wifiClient;
String sessionPUID;

// === Генерация сессионного PUID ===
String generateSessionPUID() {
  char buf[16];
  snprintf(buf, sizeof(buf), "%08X", esp_random());
  return String(buf);
}

// === Функция проверки и восстановления Wi-Fi ===
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
    snprintf(hostname, sizeof(hostname), "esp32-sensor-%d", SENSOR_ID);
    WiFi.setHostname(hostname);

    WiFi.begin(ssid, password);
    isReconnecting = true;
    lastReconnectAttempt = millis();
    Serial.println("[WIFI] Reconnect initiated...");
  }

  if (isReconnecting && millis() - lastReconnectAttempt > reconnectTimeout) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] Reconnect timed out. Will retry...");
      isReconnecting = false;
    } else {
      Serial.print("[WIFI] Reconnected! IP: ");
      Serial.println(WiFi.localIP());
      isReconnecting = false;
      failedHeartbeats = 0;
    }
  }
}

// === Heartbeat с защитой от зависания ===
void sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HB] Skipped: Wi-Fi not connected.");
    return;
  }

  String json = "{"
    "\"device_type\":\"pressure\","
    "\"device_id\":" + String(SENSOR_ID) + ","
    "\"health\":{"
      "\"rssi\":" + String(WiFi.RSSI()) + ","
      "\"heap\":" + String(ESP.getFreeHeap()) + ","
      "\"uptime\":" + String(millis()) +
    "}"
  "}";

  String url = "http://" + String(servers[0]) + ":" + String(serverPort) + "/api/heartbeat";
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(wifiClient, url);
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

// === Отправка данных на сервер ===
bool sendToServer(const char* host, const String& json) {
  HTTPClient http;
  String url = "http://" + String(host) + ":" + String(serverPort) + String(endpoint);

  if (http.begin(wifiClient, url)) {
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(json);
    http.end();
    return (code == 200 || code == 201);
  }
  return false;
}

// === Чтение датчика ===
bool readSensor(float &pressureBar, float &temperatureC) {
  Wire.beginTransmission(SENSOR_ADDR);
  Wire.write(CMD_TRIGGER);
  if (Wire.endTransmission() != 0) {
    return false;
  }

  delay(50);

  uint8_t count = Wire.requestFrom(SENSOR_ADDR, (uint8_t)6);
  if (count != 6) {
    return false;
  }

  uint8_t data[6];
  for (int i = 0; i < 6; i++) {
    data[i] = Wire.read();
  }

  uint32_t rawPressure =
      ((uint32_t)data[1] << 16) |
      ((uint32_t)data[2] << 8)  |
      data[3];

  pressureBar = ((float)(rawPressure - RAW_P_MIN) / (float)(RAW_P_MAX - RAW_P_MIN))
                * PRESSURE_RANGE + PRESSURE_OFFSET;

  uint16_t rawTemperature =
      ((uint16_t)data[4] << 8) | data[5];
  temperatureC = TEMP_SLOPE * rawTemperature + TEMP_OFFSET;

  Serial.print("raw pressure: ");
  Serial.print(rawPressure);
  Serial.print(" | raw temp: ");
  Serial.println(rawTemperature);

  return true;
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n[INIT] ESP32 sensor booting...");

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);

  sessionPUID = generateSessionPUID();
  Serial.print("PUID: ");
  Serial.println(sessionPUID);

  // Полный сброс Wi-Fi
  WiFi.disconnect(true);
  delay(100);

  WiFi.mode(WIFI_STA);

  char hostname[32];
  snprintf(hostname, sizeof(hostname), "esp32-sensor-%d", SENSOR_ID);
  WiFi.setHostname(hostname);
  Serial.printf("[HOST] Set: %s\n", hostname);

  // Подключение к Wi-Fi
  Serial.printf("[WIFI] Connecting to '%s'...", ssid);
  WiFi.begin(ssid, password);

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

void loop() {
  // 1. Проверка и восстановление Wi-Fi
  checkWiFiConnection();

  if (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    return;
  }

  // 2. Чтение данных
  float pressureBar = 0.0;
  float temperatureC = 0.0;

  if (!readSensor(pressureBar, temperatureC)) {
    Serial.println("Sensor error");
    delay(2000);
    return;
  }

  // 3. JSON для данных
  String json = "{"
    "\"puid\":\"" + sessionPUID + "-" + String(millis()) + "\","
    "\"sensor_id\":" + String(SENSOR_ID) + ","
    "\"pressure\":" + String(pressureBar, 2) + ","
    "\"temperature\":" + String(temperatureC, 1) +
  "}";

  Serial.println(json);

  // 4. Отправка на серверы
  for (int i = 0; i < SERVER_COUNT; i++) {
    sendToServer(servers[i], json);
    delay(100);
  }

  // 5. Heartbeat
  if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    sendHeartbeat();
    lastHeartbeat = millis();
  }

  delay(timeDelay);
}