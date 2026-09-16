#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include "esp_system.h"

// === НАСТРОЙКИ ===
const char* ssid = "ELTEX-8478";
const char* password = "eSm-kp7-VdF-PtA";
const int timeDelay = 30000;

#define SENSOR_ID 2
#define I2C_SDA 32
#define I2C_SCL 33

const char* servers[] = {"192.168.1.100",};
const int SERVER_COUNT = 1;


const int serverPort = 5000;
const char* endpoint = "/data";

// Глобальные переменные
TwoWire I2C_BME = TwoWire(0);
Adafruit_BME280 bme;
WiFiClient wifiClient;
bool bmeReady = false;
String sessionPUID;  // Случайный ID сессии

// === Генерация случайного PUID для сессии ===
String generateSessionPUID() {
  char buf[16];
  snprintf(buf, sizeof(buf), "%08X", esp_random());
  return String(buf);
}

// === Инициализация сенсора ===
bool initBME() {
  I2C_BME.begin(I2C_SDA, I2C_SCL, 400000);
  if (bme.begin(0x76, &I2C_BME) || bme.begin(0x77, &I2C_BME)) {
    Serial.println("✓ BME280 найден");
    return true;
  }
  Serial.println("✗ BME280 не найден");
  return false;
}

// === Отправка на один сервер ===
bool sendToServer(const char* host, const String& json) {
  HTTPClient http;
  String url = "http://" + String(host) + ":" + String(serverPort) + String(endpoint);
  
  Serial.print("  → ");
  Serial.print(host);
  
  if (http.begin(wifiClient, url)) {
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(json);
    Serial.print(" [");
    Serial.print(code);
    Serial.println("]");
    http.end();
    return (code == 200 || code == 201);
  } else {
    Serial.println(" [HTTP ERROR]");
    return false;
  }
}

void setup() {
  Serial.begin(9600);
  delay(1000);
  Serial.println("\n=== ESP32 BME280 Logger ===");

  // Генерация PUID для текущей сессии
  sessionPUID = generateSessionPUID();
  Serial.print("Session PUID: ");
  Serial.println(sessionPUID);

  // Инициализация BME
  int attempts = 0;
  while (!bmeReady && attempts < 5) {
    if (initBME()) bmeReady = true;
    else { attempts++; delay(1000); }
  }

  // Wi-Fi
  String hostname = "esp-sensor-" + String(SENSOR_ID);
  WiFi.setHostname(hostname.c_str()); 
  Serial.print("Wi-Fi: ");
  WiFi.begin(ssid, password);
  unsigned long timeout = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - timeout < 30000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? "OK" : "ERROR");
}

void loop() {
  // Переподключение Wi-Fi
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n⚠ Wi-Fi reconnect...");
    WiFi.reconnect();
    delay(5000);
    return;
  }

  // Восстановление BME при необходимости
  if (!bmeReady) {
    if (initBME()) bmeReady = true;
    else { delay(2000); return; }
  }

  // Чтение данных
  float t = bme.readTemperature();
  float h = bme.readHumidity();
  float p = bme.readPressure() / 100.0F;

  if (isnan(t) || isnan(h) || isnan(p)) {
    Serial.println("✗ BME read error");
    bmeReady = false;
    delay(2000);
    return;
  }

// Формирование JSON с session PUID и timestamp
  String json = "{"
                "\"puid\":\"" + sessionPUID + "|" + String(millis()) + "\","
                "\"sensor_id\":" + String(SENSOR_ID) + ","
                "\"temperature\":" + String(t, 1) + ","
                "\"humidity\":" + String(h, 1) + ","
                "\"pressure\":" + String(p, 1) +
                "}";

  Serial.println("\n→ Payload: " + json);

  // Отправка на все сервера в цикле
  for (int i = 0; i < SERVER_COUNT; i++) {
    sendToServer(servers[i], json);
    delay(100);
  }

  delay(timeDelay);
}