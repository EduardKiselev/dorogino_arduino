/* esp32-bme-universal.ino */
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include "esp_system.h"

#define HAS_DISPLAY

const char* ssid = "ELTEX-8478";
const char* password = "eSm-kp7-VdF-PtA";

const int SENSOR_ID = 12;
#define I2C_SDA 32
#define I2C_SCL 33

const unsigned long HEARTBEAT_INTERVAL = 60000;
const char* servers[] = {"192.168.1.100", "192.168.1.101"};
const int SERVER_COUNT = 2;
const int serverPort = 5000;
const char* endpoint = "/data";

TwoWire I2C_BME = TwoWire(0);
Adafruit_BME280 bme;
WiFiClient wifiClient;
bool bmeReady = false;
String sessionPUID;

unsigned long lastHeartbeatMillis = 0;
unsigned long lastStatusCheckMillis = 0;
unsigned long lastDisplayRefreshMillis = 0;

struct SensorData {
  float h = 0, t = 0, p = 0;
  bool valid = false;
} lastData;

#ifdef HAS_DISPLAY
  #include <Adafruit_GFX.h>
  #include <Adafruit_SSD1306.h>
  #define SCREEN_WIDTH 128
  #define SCREEN_HEIGHT 64
  Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &I2C_BME, -1);

  void drawStatus(const String& msg, bool isError) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(isError ? 2 : 1);
    display.setCursor(0, 0);
    display.println(msg);
    I2C_BME.begin(I2C_SDA, I2C_SCL, 400000);  // Восстанавливаем пины перед отправкой
    display.display();
  }

  void drawData(float h, float t, float p) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.setTextSize(4);
    display.print(h, 1);
    display.println("%");
    display.setTextSize(1);
    display.setCursor(0, 48);
    display.print("T:"); display.print(t, 1); display.print("C  P:");
    display.print(p, 0); display.println(" hPa");
    I2C_BME.begin(I2C_SDA, I2C_SCL, 400000);  // Восстанавливаем пины перед отправкой
    display.display();
  }
#else
  void drawStatus(const String&, bool) {}
  void drawData(float, float, float) {}
#endif

String generateSessionPUID() {
  char buf[16];
  snprintf(buf, sizeof(buf), "%08X", esp_random());
  return String(buf);
}

bool initBME() {
  if (bme.begin(0x76, &I2C_BME) || bme.begin(0x77, &I2C_BME)) {
    I2C_BME.begin(I2C_SDA, I2C_SCL, 400000);  // Восстанавливаем пины после bme.begin()
    Serial.println("BME280 OK");
    return true;
  }
  Serial.println("BME280 FAIL");
  return false;
}

bool sendToServer(const char* host, const String& json) {
  HTTPClient http;
  String url = "http://" + String(host) + ":" + String(serverPort) + endpoint;
  Serial.print("-> "); Serial.print(host);
  if (http.begin(wifiClient, url)) {
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(json);
    Serial.print(" ["); Serial.print(code); Serial.println("]");
    http.end();
    return (code == 200 || code == 201);
  }
  Serial.println(" [HTTP ERR]");
  return false;
}

void ProcessHeartbeat() {
  if (!bmeReady) {
    if (initBME()) {
      bmeReady = true;
      #ifdef HAS_DISPLAY
        drawStatus("Sensor Ready", false);
      #endif
    } else {
      #ifdef HAS_DISPLAY
        drawStatus("Sensor ERR", true);
      #endif
      return;
    }
  }

  float t = bme.readTemperature();
  float h = bme.readHumidity();
  float p = bme.readPressure() / 100.0F;

  if (isnan(t) || isnan(h) || isnan(p)) {
    Serial.println("Read ERR");
    bmeReady = false;
    lastData.valid = false;
    #ifdef HAS_DISPLAY
      drawStatus("Read ERR", true);
    #endif
    return;
  }

  lastData.t = t; lastData.h = h; lastData.p = p; lastData.valid = true;

  #ifdef HAS_DISPLAY
    drawData(h, t, p);
  #endif
  Serial.printf("Data: T=%.1f H=%.1f P=%.1f\n", t, h, p);

  String json = "{\"puid\":\"" + sessionPUID + "|" + String(millis()) + "\","
                "\"sensor_id\":" + String(SENSOR_ID) + ","
                "\"temperature\":" + String(t, 1) + ","
                "\"humidity\":" + String(h, 1) + ","
                "\"pressure\":" + String(p, 1) + "}";

  for (int i = 0; i < SERVER_COUNT; i++) {
    sendToServer(servers[i], json);
    delay(100);
  }
}

void setup() {
  Serial.begin(9600);
  delay(1000);
  Serial.println("ESP32 BME Logger");

  I2C_BME.begin(I2C_SDA, I2C_SCL, 400000);

  #ifdef HAS_DISPLAY
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
      Serial.println("Display FAIL");
    } else {
      display.setTextSize(1);
      drawStatus("Boot...", false);
    }
  #endif

  sessionPUID = generateSessionPUID();
  bmeReady = initBME();

  Serial.print("Wi-Fi: ");
  WiFi.begin(ssid, password);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " OK" : " FAIL");
  
  lastHeartbeatMillis = millis();
}

void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastStatusCheckMillis > 5000) {
      lastStatusCheckMillis = now;
      Serial.println("WiFi Lost. Reconnecting...");
      WiFi.reconnect();
      #ifdef HAS_DISPLAY
        drawStatus("No WiFi", true);
      #endif
    }
  } else if (now - lastHeartbeatMillis >= HEARTBEAT_INTERVAL) {
    lastHeartbeatMillis = now;
    ProcessHeartbeat();
  }

  #ifdef HAS_DISPLAY
    if (now - lastDisplayRefreshMillis > 2000 && lastData.valid) {
      lastDisplayRefreshMillis = now;
      Serial.println(" DISPLAY REFRESH");
      drawData(lastData.h, lastData.t, lastData.p);
    }
  #endif
}