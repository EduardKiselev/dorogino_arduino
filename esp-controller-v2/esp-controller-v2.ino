#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h> // Оставлен из оригинала
#include <esp_wifi.h>
#include <HTTPClient.h> 

// === НАСТРОЙКИ ===
#define CONTROLLER_ID 5  // ← Уникальный ID устройства (1-255)
const char* ssid = "ELTEX-8478";
const char* pass = "eSm-kp7-VdF-PtA";

const char* servers[] = {"192.168.1.100", "192.168.1.101"};
const int serverCount = 2;
const int serverPort = 5000;

// === Глобальные переменные ===
bool isReconnecting = false;
unsigned long lastReconnectAttempt = 0;
const unsigned long reconnectTimeout = 10000; // 10 секунд таймаут
int failedHeartbeats = 0; // Счетчик неудачных heartbeat для выявления "фантомного" подключения

WebServer server(80);
bool valveOpen = false;
uint32_t openedAt = 0;
const int VALVE_PIN = 25;
const uint32_t CLOSE_DELAY = 300000; // 300 сек

const unsigned long HEARTBEAT_INTERVAL = 60000;
unsigned long lastHeartbeat = 0;

// === Обработчики HTTP ===
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
void setCustomMAC(uint32_t id) {
  uint8_t mac[6];
  mac[0] = 0x02;                    // Locally administered, unicast
  mac[1] = 0xCA;                    // Фиксированный байт
  mac[2] = 0xFE;                    // Фиксированный байт
  mac[3] = 0x00;                    // Фиксированный байт
  mac[4] = 0x00;                    // Фиксированный байт
  mac[5] = id & 0xFF;               // Уникальная часть: номер контроллера
  
  esp_wifi_set_mac(WIFI_IF_STA, mac);
  Serial.printf("[MAC] Set: %02X:%02X:%02X:%02X:%02X:%02X\n", 
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// === Функция проверки и восстановления Wi-Fi (УСИЛЕННАЯ) ===
void checkWiFiConnection() {
  if (WiFi.status() == WL_CONNECTED) {
    isReconnecting = false;
    return;
  }

  if (!isReconnecting) {
    Serial.println("[WIFI] Connection lost! Forcing full reset of Wi-Fi stack...");
    WiFi.disconnect(true); // ВАЖНО: Полный разрыв и очистка состояния стека
    delay(200);            // Даем стеку время на сброс
    WiFi.mode(WIFI_STA);
    setCustomMAC(CONTROLLER_ID + 200); // Переназначаем MAC, так как disconnect мог его сбросить
    
    char hostname[32];
    snprintf(hostname, sizeof(hostname), "esp32-controller-%d", CONTROLLER_ID);
    WiFi.setHostname(hostname);
    
    WiFi.begin(ssid, pass);
    isReconnecting = true;
    lastReconnectAttempt = millis();
    Serial.println("[WIFI] Reconnect initiated...");
  }

  if (isReconnecting && millis() - lastReconnectAttempt > reconnectTimeout) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] Reconnect attempt timed out. Will retry...");
      isReconnecting = false; // Сброс флага для новой попытки на следующем цикле
    } else {
      Serial.print("[WIFI] Reconnected successfully! IP: ");
      Serial.println(WiFi.localIP());
      isReconnecting = false;
      failedHeartbeats = 0; // Сброс счетчика ошибок при успешном переподключении
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
                "\"device_type\":\"controller\","
                "\"device_id\":" + String(CONTROLLER_ID) + ","
                "\"health\":{"
                  "\"rssi\":" + String(WiFi.RSSI()) + ","
                  "\"heap\":" + String(ESP.getFreeHeap()) + ","
                  "\"uptime\":" + String(millis()) +
                "}"
                "}";

  String url = "http://" + String(servers[0]) + ":" + String(serverPort) + "/api/heartbeat";
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  
  int httpCode = http.POST(json);
  
  if (httpCode > 0) {
    Serial.printf("[HB] Sent | Code: %d\n", httpCode);
    failedHeartbeats = 0; // Сброс счетчика ошибок при успехе
  } else {
    Serial.printf("[HB] Failed | Error: %s\n", http.errorToString(httpCode).c_str());
    failedHeartbeats++;
    
    // ВАЖНО: Если 3 подряд неудачных запроса, принудительно переподключаем Wi-Fi, 
    // даже если WiFi.status() == WL_CONNECTED (лечение "фантомного" подключения)
    if (failedHeartbeats >= 3) {
      Serial.println("[WIFI] 3 consecutive HB failures. Forcing Wi-Fi reconnect...");
      WiFi.disconnect(true);
      isReconnecting = false; // Сбросим флаг, чтобы checkWiFiConnection сразу начал переподключение
      failedHeartbeats = 0;
    }
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n[INIT] ESP32 booting...");

  pinMode(VALVE_PIN, OUTPUT);
  digitalWrite(VALVE_PIN, LOW);

  // === 1. Полный сброс перед настройкой ===
  WiFi.disconnect(true);
  delay(100);
  
  WiFi.mode(WIFI_STA);
  setCustomMAC(CONTROLLER_ID + 200);

  // === 2. Задаём имя хоста ===
  char hostname[32];
  snprintf(hostname, sizeof(hostname), "esp32-controller-%d", CONTROLLER_ID);
  WiFi.setHostname(hostname);
  Serial.printf("[HOST] Set: %s\n", hostname);

  // === 3. Подключаемся к Wi-Fi с таймаутом ===
  Serial.printf("[WIFI] Connecting to '%s'...", ssid);
  WiFi.begin(ssid, pass);
  
  unsigned long connectStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - connectStart < 15000) {
    delay(500);
    Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[INFO] Hostname: %s, MAC: %s\n", 
                  WiFi.getHostname(), WiFi.macAddress().c_str());
  } else {
    Serial.println("\n[WIFI] Initial connection failed. Will retry in loop.");
  }

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
  // 1. Проверка и восстановление Wi-Fi
  checkWiFiConnection();

  // 2. Обработка HTTP запросов
  server.handleClient();
  
  // 3. Логика клапана
  if (valveOpen && millis() - openedAt >= CLOSE_DELAY) {
    digitalWrite(VALVE_PIN, LOW);
    valveOpen = false;
    Serial.println("[TIMER] Valve CLOSED (auto)");
  }

  // 4. Heartbeat (отправляем только если есть связь)  
  if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    sendHeartbeat();
    lastHeartbeat = millis();
  }
}