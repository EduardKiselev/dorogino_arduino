#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <esp_wifi.h>

// === НАСТРОЙКИ ===
#define CONTROLLER_ID 10  // ← Уникальный ID устройства (1-255)
const char* ssid = "ELTEX-8478";
const char* pass = "eSm-kp7-VdF-PtA";



WebServer server(80);
bool valveOpen = false;
uint32_t openedAt = 0;
const int VALVE_PIN = 25;
const uint32_t CLOSE_DELAY = 300000; // 300 сек

void handleOpen() {
  Serial.println("[HTTP] GET /valve/open");
  digitalWrite(VALVE_PIN, HIGH);
  valveOpen = true;
  openedAt = millis();
  Serial.println("[DEBUG] Valve OPENED");
  server.send(200, "text/plain", "OK: opened");
}

void handleClose() {
  Serial.println("[HTTP] GET /valve/close");
  digitalWrite(VALVE_PIN, LOW);
  valveOpen = false;
  Serial.println("[DEBUG] Valve CLOSED (manual)");
  server.send(200, "text/plain", "OK: closed");
}

// === Генерация и установка кастомного MAC-адреса ===
// Локально-администрируемый адрес: 02:XX:XX:XX:XX:XX
void setCustomMAC(uint32_t id) {
  uint8_t mac[6];
  
  mac[0] = 0x02;                    // Locally administered, unicast
  mac[1] = 0xCA;                    // Фиксированный байт для ваших устройств
  mac[2] = 0xFE;                    // (можно менять для разных серий)
  mac[3] = 0x00;                    // Фиксированный байт
  mac[4] = 0x00;                    // Фиксированный байт
  mac[5] = id & 0xFF;               // Уникальная часть: номер контроллера
  
  esp_wifi_set_mac(WIFI_IF_STA, mac);
  
  Serial.printf("[MAC] Set: %02X:%02X:%02X:%02X:%02X:%02X\n", 
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n[INIT] ESP32 booting...");

  pinMode(VALVE_PIN, OUTPUT);
  digitalWrite(VALVE_PIN, LOW);

  // === 1. Устанавливаем MAC ДО подключения ===
  WiFi.mode(WIFI_STA);              // Обязательно перед esp_wifi_set_mac
  setCustomMAC(CONTROLLER_ID);

  // === 2. Задаём понятное имя хоста ===
  char hostname[32];
  snprintf(hostname, sizeof(hostname), "esp32-controller-%d", CONTROLLER_ID);
  WiFi.setHostname(hostname);
  Serial.printf("[HOST] Set: %s\n", hostname);

  // === 3. Подключаемся к Wi-Fi ===
  Serial.printf("[WIFI] Connecting to '%s'...", ssid);
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\n[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[INFO] Hostname: %s, MAC: %s\n", 
                WiFi.getHostname(), WiFi.macAddress().c_str());

  // === HTTP сервер ===
  server.on("/valve/open", HTTP_GET, handleOpen);
  server.on("/valve/close", HTTP_GET, handleClose);
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/plain", "ESP32 Valve Controller\nEndpoints: /valve/open, /valve/close");
  });

  server.begin();
  Serial.println("[HTTP] Server listening on port 80");
}


void loop() {
  server.handleClient();
  
  if (valveOpen && millis() - openedAt >= CLOSE_DELAY) {
    digitalWrite(VALVE_PIN, LOW);
    valveOpen = false;
    Serial.println("[TIMER] Valve CLOSED (auto)");
  }
}