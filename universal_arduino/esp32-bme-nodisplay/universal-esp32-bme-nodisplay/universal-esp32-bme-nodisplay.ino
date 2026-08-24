#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include "esp_system.h"
#include <esp_wifi.h> // Добавлено для управления MAC

// === НАСТРОЙКИ ===
const char* ssid = "TP-LINK_LCV";
const char* password = "";
const int timeDelay = 3000;

#define SENSOR_ID 1
#define OPOKA_ID 42
#define I2C_SDA 32
#define I2C_SCL 33

const char* servers[] = {"192.168.4.144",};
const int SERVER_COUNT = 1;
const int serverPort = 5000;
const char* endpoint = "/data";

// Глобальные переменные
TwoWire I2C_BME = TwoWire(0);
Adafruit_BME280 bme;
WiFiClient wifiClient;
bool bmeReady = false;
String sessionPUID;  // Случайный ID сессии

// === ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ДЛЯ WI-FI И HEARTBEAT ===
bool wifiReconnecting = false;
unsigned long lastReconnectTime = 0;
const unsigned long WIFI_RECONNECT_DELAY = 60000; // 60 секунд между попытками

const unsigned long HEARTBEAT_INTERVAL = 60000;   // Интервал heartbeat (60 сек)
unsigned long lastHeartbeat = 0;

// === Генерация случайного PUID для сессии ===
String generateSessionPUID() {
  char buf[16];
  snprintf(buf, sizeof(buf), "%08X", esp_random());
  return String(buf);
}

// === НОВАЯ ФУНКЦИЯ: Генерация кастомного MAC-адреса ===
// Формат: 02:CA:FE:00:00:XX (где XX - ID сенсора)
void setCustomMAC(uint32_t id) {
  uint8_t mac[6];
  
  mac[0] = 0x02;                    // Locally administered, unicast
  mac[1] = 0xCA;                    // Фиксированный байт
  mac[2] = 0xFE;                    // Фиксированный байт
  mac[3] = 0x00;                    
  mac[4] = 0x00;                    
  mac[5] = id & 0xFF;               // Уникальная часть: номер сенсора
  
  esp_wifi_set_mac(WIFI_IF_STA, mac);
  
  Serial.printf("[MAC] Set: %02X:%02X:%02X:%02X:%02X:%02X\n", 
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
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

// === ФУНКЦИЯ: Переподключение Wi-Fi ===
void checkAndReconnectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiReconnecting = false;
    return;
  }

  // Если связь потеряна и мы не в режиме ожидания
  if (!wifiReconnecting) {
    Serial.println("[WIFI] Connection lost. Waiting 60s before retry...");
    wifiReconnecting = true;
    lastReconnectTime = millis();
    return;
  }

  // Если прошло 60 секунд ожидания
  if (millis() - lastReconnectTime >= WIFI_RECONNECT_DELAY) {
    Serial.println("[WIFI] Attempting to reconnect...");
    WiFi.begin(ssid, password);
    // Сбрасываем флаг: это запустит новый цикл ожидания 60 сек на следующем витке loop
    wifiReconnecting = false; 
  }
}

// === ФУНКЦИЯ: Heartbeat ===
void sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) return; // Не отправляем, если нет связи

  String json = "{"
                "\"device_type\":\"sensor\","
                "\"device_id\":" + String(SENSOR_ID) + ","
                "\"health\":{"
                  "\"rssi\":" + String(WiFi.RSSI()) + ","
                  "\"heap\":" + String(ESP.getFreeHeap()) + ","
                  "\"uptime\":" + String(millis()) +
                "}"
                "}";

  String url = "http://" + String(servers[0]) + ":" + String(serverPort) + "/api/heartbeat";
  HTTPClient http;
  // Используем wifiClient для согласованности с остальным кодом
  if (http.begin(wifiClient, url)) {
    http.addHeader("Content-Type", "application/json");
    int httpCode = http.POST(json);
    Serial.printf("[HB] Sent | Code: %d\n", httpCode);
    http.end();
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

  // Wi-Fi настройки
  String hostname = "esp-sensor-" + String(SENSOR_ID);
  WiFi.setHostname(hostname.c_str());
  
  // === Генерация и установка кастомного MAC-адреса ===
  WiFi.mode(WIFI_STA); // Обязательно перед установкой MAC
  setCustomMAC(SENSOR_ID); 
  
  // Вывод информации о Wi-Fi
  Serial.printf("[INFO] Hostname: %s, MAC: %s\n", 
                WiFi.getHostname(), WiFi.macAddress().c_str());

  // Подключение
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
  // 1. Проверка и восстановление Wi-Fi (неблокирующее)
  checkAndReconnectWiFi();

  // Если связи нет — пропускаем шаг, ждем переподключения
  if (WiFi.status() != WL_CONNECTED) {
    delay(1000); 
    return;
  }

  // 2. Отправка Heartbeat (раз в 60 сек)
  if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    sendHeartbeat();
    lastHeartbeat = millis();
  }

  // 3. Восстановление BME при необходимости
  if (!bmeReady) {
    if (initBME()) bmeReady = true;
    else { delay(2000); return; }
  }

  // 4. Чтение данных
  float t = bme.readTemperature();
  float h = bme.readHumidity();
  float p = bme.readPressure() / 100.0F;

  if (isnan(t) || isnan(h) || isnan(p)) {
    Serial.println("✗ BME read error");
    bmeReady = false;
    delay(2000);
    return;
  }

  // 5. Формирование JSON
  String json = "{"
                "\"puid\":\"" + sessionPUID + "|" + String(millis()) + "\","
                "\"sensor_id\":" + String(SENSOR_ID) + ","
                "\"opoka_id\":" + String(OPOKA_ID) + ","
                "\"temperature\":" + String(t, 1) + ","
                "\"humidity\":" + String(h, 1) + ","
                "\"pressure\":" + String(p, 1) +
                "}";

  Serial.println("\n→ Payload: " + json);

  // 6. Отправка на все сервера
  for (int i = 0; i < SERVER_COUNT; i++) {
    sendToServer(servers[i], json);
    delay(100);
  }

  delay(timeDelay);
}