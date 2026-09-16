#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "esp_system.h"

// === НАСТРОЙКИ ===
const char* ssid = "ELTEX-8478";
const char* password = "eSm-kp7-VdF-PtA";
const int timeDelay = 60000;
#define SENSOR_ID 12

// ✅ НОВЫЕ ПИНЫ: дисплей на 32/33, BME на 21/22
#define DISPLAY_SDA 32
#define DISPLAY_SCL 33
#define BME_SDA 21
#define BME_SCL 22

const char* servers[] = {"192.168.1.100", "192.168.1.101"};
const int SERVER_COUNT = 2;
const int serverPort = 5000;
const char* endpoint = "/data";

// OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C

// ✅ Две отдельные шины I2C
TwoWire I2C_DISPLAY = TwoWire(0);  // Шина 0 — дисплей (пины 32/33)
TwoWire I2C_BME = TwoWire(1);      // Шина 1 — BME280 (пины 21/22)

Adafruit_BME280 bme;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &I2C_DISPLAY, OLED_RESET);
WiFiClient wifiClient;
bool bmeReady = false;
String sessionPUID;

// === Генерация сессионного PUID ===
String generateSessionPUID() {
  char buf[16];
  snprintf(buf, sizeof(buf), "%08X", esp_random());
  return String(buf);
}

// === Инициализация BME280 ===
bool initBME() {
  if (bme.begin(0x76, &I2C_BME) || bme.begin(0x77, &I2C_BME)) {
    Serial.println("BME280 OK");
    return true;
  }
  Serial.println("BME280 FAIL");
  return false;
}

// === Отправка на сервер ===
bool sendToServer(const char* host, const String& json) {
  HTTPClient http;
  String url = "http://" + String(host) + ":" + String(serverPort) + String(endpoint);
  Serial.print("  -> ");
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

// === Обновление OLED ===
void updateDisplay(float h, float t, float p) {
  display.clearDisplay();
  display.setTextSize(4);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(h, 1);
  display.println("%");
  display.setTextSize(1);
  display.setCursor(0, 48);
  display.print("T:");
  display.print(t, 1);
  display.print("C  P:");
  display.print(p, 0);
  display.println(" hPa");
  display.display();
}

// === Показ статуса на OLED ===
void showStatus(const String& msg) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(msg);
  display.display();
}

void setup() {
  Serial.begin(9600);
  delay(1000);
  Serial.println("\n=== ESP32 BME280 + OLED ===");

  // ✅ Инициализация шины дисплея на пинах 32/33
  I2C_DISPLAY.begin(DISPLAY_SDA, DISPLAY_SCL, 400000);

  // OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("OLED FAIL");
    for (;;);
  }
  showStatus("Zagruzka...");

  // ✅ Инициализация шины BME на пинах 21/22
  I2C_BME.begin(BME_SDA, BME_SCL, 400000);

  // PUID
  sessionPUID = generateSessionPUID();
  Serial.print("Session PUID: ");
  Serial.println(sessionPUID);

  // BME
  int attempts = 0;
  while (!bmeReady && attempts < 5) {
    if (initBME()) bmeReady = true;
    else { attempts++; delay(1000); }
  }

  // Wi-Fi
  Serial.print("Wi-Fi: ");
  WiFi.begin(ssid, password);
  unsigned long timeout = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - timeout < 30000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? "OK" : "ERROR");

  showStatus(WiFi.status() == WL_CONNECTED ? "Wi-Fi OK" : "Wi-Fi Error");
  delay(1000);
}

void loop() {
  // Переподключение Wi-Fi
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWi-Fi reconnect...");
    showStatus("No WiFi");
    WiFi.reconnect();
    delay(5000);
    return;
  }

  // Восстановление BME
  if (!bmeReady) {
    if (initBME()) bmeReady = true;
    else { delay(2000); return; }
  }

  // Чтение данных
  float t = bme.readTemperature();
  float h = bme.readHumidity();
  float p = bme.readPressure() / 100.0F;

  if (isnan(t) || isnan(h) || isnan(p)) {
    Serial.println("BME read error");
    bmeReady = false;
    showStatus("Read ERR");
    delay(2000);
    return;
  }

  // OLED
  Serial.println("DISPLAY REFRESH");
  updateDisplay(h, t, p);

  // JSON
  String json = "{"
    "\"puid\":\"" + sessionPUID + "-" + String(millis()) + "\","
    "\"sensor_id\":" + String(SENSOR_ID) + ","
    "\"temperature\":" + String(t, 1) + ","
    "\"humidity\":" + String(h, 1) + ","
    "\"pressure\":" + String(p, 1) +
  "}";

  Serial.println("Payload: " + json);

  // Отправка
  for (int i = 0; i < SERVER_COUNT; i++) {
    sendToServer(servers[i], json);
    delay(100);
  }

  delay(timeDelay);
}