// temperature_sensor.ino

#include <WiFi.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ============ КОНФИГУРАЦИЯ ============

const char* DEVICE_ID = "temp-sensor-01";
const char* DEVICE_TYPE = "temperature_sensor";

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
const int ONE_WIRE_BUS = 4;

// Интервалы
const unsigned long MEASUREMENT_INTERVAL_MS = 1000;
const unsigned long HEARTBEAT_INTERVAL_MS = 60000;
const unsigned long DATA_SEND_INTERVAL_MS = 60000;  // Отправка данных каждые 60 секунд
const unsigned long RECONNECT_TIMEOUT_MS = 10000;

const int DS18B20_RESOLUTION = 12;

// ============ ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ============

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

bool isReconnecting = false;
unsigned long lastReconnectAttempt = 0;
int failedHeartbeats = 0;

unsigned long lastMeasurement = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastDataSend = 0;

// Текущие показания
float currentTemperature = 0;

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
                  "\"data_type\":\"temperature\","
                  "\"data\":" + String(currentTemperature, 1) +
                "}"
                "}";

  String url = "http://" + String(serverHost) + ":" + String(serverPort) + DATA_ENDPOINT;
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  
  int httpCode = http.POST(json);
  
  if (httpCode > 0) {
    Serial.printf("[DATA] Sent | Code: %d | Temp: %.2f°C\n", httpCode, currentTemperature);
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
  Serial.println("\n[INIT] ESP32 Temperature Sensor booting...");

  pinMode(ONE_WIRE_BUS, INPUT_PULLUP);
  
  Serial.println("=== DS18B20 Parasitic Power ===");
  
  sensors.begin();
  sensors.setResolution(12);

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
  
  // Чтение датчика каждые 500мс
  if (millis() - lastMeasurement >= MEASUREMENT_INTERVAL_MS) {
    sensors.requestTemperatures();
    currentTemperature = sensors.getTempCByIndex(0);
    
   // Serial.printf("[READ] Temperature: %.1f°C\n", currentTemperature);
    
    lastMeasurement = millis();
  }
  
  // Heartbeat раз в минуту
  if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    sendHeartbeat();
    lastHeartbeat = millis();
  }
  
  if (millis() - lastDataSend >= DATA_SEND_INTERVAL_MS) {
    sendData();
    lastDataSend = millis();
  }
}